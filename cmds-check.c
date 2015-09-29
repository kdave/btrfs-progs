/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include "ctree.h"
#include "volumes.h"
#include "repair.h"
#include "disk-io.h"
#include "print-tree.h"
#include "task-utils.h"
#include "transaction.h"
#include "utils.h"
#include "commands.h"
#include "free-space-cache.h"
#include "btrfsck.h"
#include "qgroup-verify.h"
#include "rbtree-utils.h"
#include "backref.h"
#include "ulist.h"

enum task_position {
	TASK_EXTENTS,
	TASK_FREE_SPACE,
	TASK_FS_ROOTS,
	TASK_NOTHING, /* have to be the last element */
};

struct task_ctx {
	int progress_enabled;
	enum task_position tp;

	struct task_info *info;
};

static u64 bytes_used = 0;
static u64 total_csum_bytes = 0;
static u64 total_btree_bytes = 0;
static u64 total_fs_tree_bytes = 0;
static u64 total_extent_tree_bytes = 0;
static u64 btree_space_waste = 0;
static u64 data_bytes_allocated = 0;
static u64 data_bytes_referenced = 0;
static int found_old_backref = 0;
static LIST_HEAD(duplicate_extents);
static LIST_HEAD(delete_items);
static int repair = 0;
static int no_holes = 0;
static int init_extent_tree = 0;
static int check_data_csum = 0;
static struct btrfs_fs_info *global_info;
static struct task_ctx ctx = { 0 };

static void *print_status_check(void *p)
{
	struct task_ctx *priv = p;
	const char work_indicator[] = { '.', 'o', 'O', 'o' };
	uint32_t count = 0;
	static char *task_position_string[] = {
		"checking extents",
		"checking free space cache",
		"checking fs roots",
	};

	task_period_start(priv->info, 1000 /* 1s */);

	if (priv->tp == TASK_NOTHING)
		return NULL;

	while (1) {
		printf("%s [%c]\r", task_position_string[priv->tp],
				work_indicator[count % 4]);
		count++;
		fflush(stdout);
		task_period_wait(priv->info);
	}
	return NULL;
}

static int print_status_return(void *p)
{
	printf("\n");
	fflush(stdout);

	return 0;
}

struct extent_backref {
	struct list_head list;
	unsigned int is_data:1;
	unsigned int found_extent_tree:1;
	unsigned int full_backref:1;
	unsigned int found_ref:1;
	unsigned int broken:1;
};

struct data_backref {
	struct extent_backref node;
	union {
		u64 parent;
		u64 root;
	};
	u64 owner;
	u64 offset;
	u64 disk_bytenr;
	u64 bytes;
	u64 ram_bytes;
	u32 num_refs;
	u32 found_ref;
};

/*
 * Much like data_backref, just removed the undetermined members
 * and change it to use list_head.
 * During extent scan, it is stored in root->orphan_data_extent.
 * During fs tree scan, it is then moved to inode_rec->orphan_data_extents.
 */
struct orphan_data_extent {
	struct list_head list;
	u64 root;
	u64 objectid;
	u64 offset;
	u64 disk_bytenr;
	u64 disk_len;
};

struct tree_backref {
	struct extent_backref node;
	union {
		u64 parent;
		u64 root;
	};
};

struct extent_record {
	struct list_head backrefs;
	struct list_head dups;
	struct list_head list;
	struct cache_extent cache;
	struct btrfs_disk_key parent_key;
	u64 start;
	u64 max_size;
	u64 nr;
	u64 refs;
	u64 extent_item_refs;
	u64 generation;
	u64 parent_generation;
	u64 info_objectid;
	u32 num_duplicates;
	u8 info_level;
	int flag_block_full_backref;
	unsigned int found_rec:1;
	unsigned int content_checked:1;
	unsigned int owner_ref_checked:1;
	unsigned int is_root:1;
	unsigned int metadata:1;
	unsigned int bad_full_backref:1;
	unsigned int crossing_stripes:1;
	unsigned int wrong_chunk_type:1;
};

struct inode_backref {
	struct list_head list;
	unsigned int found_dir_item:1;
	unsigned int found_dir_index:1;
	unsigned int found_inode_ref:1;
	unsigned int filetype:8;
	int errors;
	unsigned int ref_type;
	u64 dir;
	u64 index;
	u16 namelen;
	char name[0];
};

struct root_item_record {
	struct list_head list;
	u64 objectid;
	u64 bytenr;
	u64 last_snapshot;
	u8 level;
	u8 drop_level;
	int level_size;
	struct btrfs_key drop_key;
};

#define REF_ERR_NO_DIR_ITEM		(1 << 0)
#define REF_ERR_NO_DIR_INDEX		(1 << 1)
#define REF_ERR_NO_INODE_REF		(1 << 2)
#define REF_ERR_DUP_DIR_ITEM		(1 << 3)
#define REF_ERR_DUP_DIR_INDEX		(1 << 4)
#define REF_ERR_DUP_INODE_REF		(1 << 5)
#define REF_ERR_INDEX_UNMATCH		(1 << 6)
#define REF_ERR_FILETYPE_UNMATCH	(1 << 7)
#define REF_ERR_NAME_TOO_LONG		(1 << 8) // 100
#define REF_ERR_NO_ROOT_REF		(1 << 9)
#define REF_ERR_NO_ROOT_BACKREF		(1 << 10)
#define REF_ERR_DUP_ROOT_REF		(1 << 11)
#define REF_ERR_DUP_ROOT_BACKREF	(1 << 12)

struct file_extent_hole {
	struct rb_node node;
	u64 start;
	u64 len;
};

/* Compatible function to allow reuse of old codes */
static u64 first_extent_gap(struct rb_root *holes)
{
	struct file_extent_hole *hole;

	if (RB_EMPTY_ROOT(holes))
		return (u64)-1;

	hole = rb_entry(rb_first(holes), struct file_extent_hole, node);
	return hole->start;
}

static int compare_hole(struct rb_node *node1, struct rb_node *node2)
{
	struct file_extent_hole *hole1;
	struct file_extent_hole *hole2;

	hole1 = rb_entry(node1, struct file_extent_hole, node);
	hole2 = rb_entry(node2, struct file_extent_hole, node);

	if (hole1->start > hole2->start)
		return -1;
	if (hole1->start < hole2->start)
		return 1;
	/* Now hole1->start == hole2->start */
	if (hole1->len >= hole2->len)
		/*
		 * Hole 1 will be merge center
		 * Same hole will be merged later
		 */
		return -1;
	/* Hole 2 will be merge center */
	return 1;
}

/*
 * Add a hole to the record
 *
 * This will do hole merge for copy_file_extent_holes(),
 * which will ensure there won't be continuous holes.
 */
static int add_file_extent_hole(struct rb_root *holes,
				u64 start, u64 len)
{
	struct file_extent_hole *hole;
	struct file_extent_hole *prev = NULL;
	struct file_extent_hole *next = NULL;

	hole = malloc(sizeof(*hole));
	if (!hole)
		return -ENOMEM;
	hole->start = start;
	hole->len = len;
	/* Since compare will not return 0, no -EEXIST will happen */
	rb_insert(holes, &hole->node, compare_hole);

	/* simple merge with previous hole */
	if (rb_prev(&hole->node))
		prev = rb_entry(rb_prev(&hole->node), struct file_extent_hole,
				node);
	if (prev && prev->start + prev->len >= hole->start) {
		hole->len = hole->start + hole->len - prev->start;
		hole->start = prev->start;
		rb_erase(&prev->node, holes);
		free(prev);
		prev = NULL;
	}

	/* iterate merge with next holes */
	while (1) {
		if (!rb_next(&hole->node))
			break;
		next = rb_entry(rb_next(&hole->node), struct file_extent_hole,
					node);
		if (hole->start + hole->len >= next->start) {
			if (hole->start + hole->len <= next->start + next->len)
				hole->len = next->start + next->len -
					    hole->start;
			rb_erase(&next->node, holes);
			free(next);
			next = NULL;
		} else
			break;
	}
	return 0;
}

static int compare_hole_range(struct rb_node *node, void *data)
{
	struct file_extent_hole *hole;
	u64 start;

	hole = (struct file_extent_hole *)data;
	start = hole->start;

	hole = rb_entry(node, struct file_extent_hole, node);
	if (start < hole->start)
		return -1;
	if (start >= hole->start && start < hole->start + hole->len)
		return 0;
	return 1;
}

/*
 * Delete a hole in the record
 *
 * This will do the hole split and is much restrict than add.
 */
static int del_file_extent_hole(struct rb_root *holes,
				u64 start, u64 len)
{
	struct file_extent_hole *hole;
	struct file_extent_hole tmp;
	u64 prev_start = 0;
	u64 prev_len = 0;
	u64 next_start = 0;
	u64 next_len = 0;
	struct rb_node *node;
	int have_prev = 0;
	int have_next = 0;
	int ret = 0;

	tmp.start = start;
	tmp.len = len;
	node = rb_search(holes, &tmp, compare_hole_range, NULL);
	if (!node)
		return -EEXIST;
	hole = rb_entry(node, struct file_extent_hole, node);
	if (start + len > hole->start + hole->len)
		return -EEXIST;

	/*
	 * Now there will be no overflap, delete the hole and re-add the
	 * split(s) if they exists.
	 */
	if (start > hole->start) {
		prev_start = hole->start;
		prev_len = start - hole->start;
		have_prev = 1;
	}
	if (hole->start + hole->len > start + len) {
		next_start = start + len;
		next_len = hole->start + hole->len - start - len;
		have_next = 1;
	}
	rb_erase(node, holes);
	free(hole);
	if (have_prev) {
		ret = add_file_extent_hole(holes, prev_start, prev_len);
		if (ret < 0)
			return ret;
	}
	if (have_next) {
		ret = add_file_extent_hole(holes, next_start, next_len);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int copy_file_extent_holes(struct rb_root *dst,
				  struct rb_root *src)
{
	struct file_extent_hole *hole;
	struct rb_node *node;
	int ret = 0;

	node = rb_first(src);
	while (node) {
		hole = rb_entry(node, struct file_extent_hole, node);
		ret = add_file_extent_hole(dst, hole->start, hole->len);
		if (ret)
			break;
		node = rb_next(node);
	}
	return ret;
}

static void free_file_extent_holes(struct rb_root *holes)
{
	struct rb_node *node;
	struct file_extent_hole *hole;

	node = rb_first(holes);
	while (node) {
		hole = rb_entry(node, struct file_extent_hole, node);
		rb_erase(node, holes);
		free(hole);
		node = rb_first(holes);
	}
}

struct inode_record {
	struct list_head backrefs;
	unsigned int checked:1;
	unsigned int merging:1;
	unsigned int found_inode_item:1;
	unsigned int found_dir_item:1;
	unsigned int found_file_extent:1;
	unsigned int found_csum_item:1;
	unsigned int some_csum_missing:1;
	unsigned int nodatasum:1;
	int errors;

	u64 ino;
	u32 nlink;
	u32 imode;
	u64 isize;
	u64 nbytes;

	u32 found_link;
	u64 found_size;
	u64 extent_start;
	u64 extent_end;
	struct rb_root holes;
	struct list_head orphan_extents;

	u32 refs;
};

#define I_ERR_NO_INODE_ITEM		(1 << 0)
#define I_ERR_NO_ORPHAN_ITEM		(1 << 1)
#define I_ERR_DUP_INODE_ITEM		(1 << 2)
#define I_ERR_DUP_DIR_INDEX		(1 << 3)
#define I_ERR_ODD_DIR_ITEM		(1 << 4)
#define I_ERR_ODD_FILE_EXTENT		(1 << 5)
#define I_ERR_BAD_FILE_EXTENT		(1 << 6)
#define I_ERR_FILE_EXTENT_OVERLAP	(1 << 7)
#define I_ERR_FILE_EXTENT_DISCOUNT	(1 << 8) // 100
#define I_ERR_DIR_ISIZE_WRONG		(1 << 9)
#define I_ERR_FILE_NBYTES_WRONG		(1 << 10) // 400
#define I_ERR_ODD_CSUM_ITEM		(1 << 11)
#define I_ERR_SOME_CSUM_MISSING		(1 << 12)
#define I_ERR_LINK_COUNT_WRONG		(1 << 13)
#define I_ERR_FILE_EXTENT_ORPHAN	(1 << 14)

struct root_backref {
	struct list_head list;
	unsigned int found_dir_item:1;
	unsigned int found_dir_index:1;
	unsigned int found_back_ref:1;
	unsigned int found_forward_ref:1;
	unsigned int reachable:1;
	int errors;
	u64 ref_root;
	u64 dir;
	u64 index;
	u16 namelen;
	char name[0];
};

struct root_record {
	struct list_head backrefs;
	struct cache_extent cache;
	unsigned int found_root_item:1;
	u64 objectid;
	u32 found_ref;
};

struct ptr_node {
	struct cache_extent cache;
	void *data;
};

struct shared_node {
	struct cache_extent cache;
	struct cache_tree root_cache;
	struct cache_tree inode_cache;
	struct inode_record *current;
	u32 refs;
};

struct block_info {
	u64 start;
	u32 size;
};

struct walk_control {
	struct cache_tree shared;
	struct shared_node *nodes[BTRFS_MAX_LEVEL];
	int active_node;
	int root_level;
};

struct bad_item {
	struct btrfs_key key;
	u64 root_id;
	struct list_head list;
};

static void reset_cached_block_groups(struct btrfs_fs_info *fs_info);

static void record_root_in_trans(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root)
{
	if (root->last_trans != trans->transid) {
		root->track_dirty = 1;
		root->last_trans = trans->transid;
		root->commit_root = root->node;
		extent_buffer_get(root->node);
	}
}

static u8 imode_to_type(u32 imode)
{
#define S_SHIFT 12
	static unsigned char btrfs_type_by_mode[S_IFMT >> S_SHIFT] = {
		[S_IFREG >> S_SHIFT]	= BTRFS_FT_REG_FILE,
		[S_IFDIR >> S_SHIFT]	= BTRFS_FT_DIR,
		[S_IFCHR >> S_SHIFT]	= BTRFS_FT_CHRDEV,
		[S_IFBLK >> S_SHIFT]	= BTRFS_FT_BLKDEV,
		[S_IFIFO >> S_SHIFT]	= BTRFS_FT_FIFO,
		[S_IFSOCK >> S_SHIFT]	= BTRFS_FT_SOCK,
		[S_IFLNK >> S_SHIFT]	= BTRFS_FT_SYMLINK,
	};

	return btrfs_type_by_mode[(imode & S_IFMT) >> S_SHIFT];
#undef S_SHIFT
}

static int device_record_compare(struct rb_node *node1, struct rb_node *node2)
{
	struct device_record *rec1;
	struct device_record *rec2;

	rec1 = rb_entry(node1, struct device_record, node);
	rec2 = rb_entry(node2, struct device_record, node);
	if (rec1->devid > rec2->devid)
		return -1;
	else if (rec1->devid < rec2->devid)
		return 1;
	else
		return 0;
}

static struct inode_record *clone_inode_rec(struct inode_record *orig_rec)
{
	struct inode_record *rec;
	struct inode_backref *backref;
	struct inode_backref *orig;
	struct orphan_data_extent *src_orphan;
	struct orphan_data_extent *dst_orphan;
	size_t size;
	int ret;

	rec = malloc(sizeof(*rec));
	memcpy(rec, orig_rec, sizeof(*rec));
	rec->refs = 1;
	INIT_LIST_HEAD(&rec->backrefs);
	INIT_LIST_HEAD(&rec->orphan_extents);
	rec->holes = RB_ROOT;

	list_for_each_entry(orig, &orig_rec->backrefs, list) {
		size = sizeof(*orig) + orig->namelen + 1;
		backref = malloc(size);
		memcpy(backref, orig, size);
		list_add_tail(&backref->list, &rec->backrefs);
	}
	list_for_each_entry(src_orphan, &orig_rec->orphan_extents, list) {
		dst_orphan = malloc(sizeof(*dst_orphan));
		/* TODO: Fix all the HELL of un-catched -ENOMEM case */
		BUG_ON(!dst_orphan);
		memcpy(dst_orphan, src_orphan, sizeof(*src_orphan));
		list_add_tail(&dst_orphan->list, &rec->orphan_extents);
	}
	ret = copy_file_extent_holes(&rec->holes, &orig_rec->holes);
	BUG_ON(ret < 0);

	return rec;
}

static void print_orphan_data_extents(struct list_head *orphan_extents,
				      u64 objectid)
{
	struct orphan_data_extent *orphan;

	if (list_empty(orphan_extents))
		return;
	printf("The following data extent is lost in tree %llu:\n",
	       objectid);
	list_for_each_entry(orphan, orphan_extents, list) {
		printf("\tinode: %llu, offset:%llu, disk_bytenr: %llu, disk_len: %llu\n",
		       orphan->objectid, orphan->offset, orphan->disk_bytenr,
		       orphan->disk_len);
	}
}

static void print_inode_error(struct btrfs_root *root, struct inode_record *rec)
{
	u64 root_objectid = root->root_key.objectid;
	int errors = rec->errors;

	if (!errors)
		return;
	/* reloc root errors, we print its corresponding fs root objectid*/
	if (root_objectid == BTRFS_TREE_RELOC_OBJECTID) {
		root_objectid = root->root_key.offset;
		fprintf(stderr, "reloc");
	}
	fprintf(stderr, "root %llu inode %llu errors %x",
		(unsigned long long) root_objectid,
		(unsigned long long) rec->ino, rec->errors);

	if (errors & I_ERR_NO_INODE_ITEM)
		fprintf(stderr, ", no inode item");
	if (errors & I_ERR_NO_ORPHAN_ITEM)
		fprintf(stderr, ", no orphan item");
	if (errors & I_ERR_DUP_INODE_ITEM)
		fprintf(stderr, ", dup inode item");
	if (errors & I_ERR_DUP_DIR_INDEX)
		fprintf(stderr, ", dup dir index");
	if (errors & I_ERR_ODD_DIR_ITEM)
		fprintf(stderr, ", odd dir item");
	if (errors & I_ERR_ODD_FILE_EXTENT)
		fprintf(stderr, ", odd file extent");
	if (errors & I_ERR_BAD_FILE_EXTENT)
		fprintf(stderr, ", bad file extent");
	if (errors & I_ERR_FILE_EXTENT_OVERLAP)
		fprintf(stderr, ", file extent overlap");
	if (errors & I_ERR_FILE_EXTENT_DISCOUNT)
		fprintf(stderr, ", file extent discount");
	if (errors & I_ERR_DIR_ISIZE_WRONG)
		fprintf(stderr, ", dir isize wrong");
	if (errors & I_ERR_FILE_NBYTES_WRONG)
		fprintf(stderr, ", nbytes wrong");
	if (errors & I_ERR_ODD_CSUM_ITEM)
		fprintf(stderr, ", odd csum item");
	if (errors & I_ERR_SOME_CSUM_MISSING)
		fprintf(stderr, ", some csum missing");
	if (errors & I_ERR_LINK_COUNT_WRONG)
		fprintf(stderr, ", link count wrong");
	if (errors & I_ERR_FILE_EXTENT_ORPHAN)
		fprintf(stderr, ", orphan file extent");
	fprintf(stderr, "\n");
	/* Print the orphan extents if needed */
	if (errors & I_ERR_FILE_EXTENT_ORPHAN)
		print_orphan_data_extents(&rec->orphan_extents, root->objectid);

	/* Print the holes if needed */
	if (errors & I_ERR_FILE_EXTENT_DISCOUNT) {
		struct file_extent_hole *hole;
		struct rb_node *node;
		int found = 0;

		node = rb_first(&rec->holes);
		fprintf(stderr, "Found file extent holes:\n");
		while (node) {
			found = 1;
			hole = rb_entry(node, struct file_extent_hole, node);
			fprintf(stderr, "\tstart: %llu, len: %llu\n",
				hole->start, hole->len);
			node = rb_next(node);
		}
		if (!found)
			fprintf(stderr, "\tstart: 0, len: %llu\n",
				round_up(rec->isize, root->sectorsize));
	}
}

static void print_ref_error(int errors)
{
	if (errors & REF_ERR_NO_DIR_ITEM)
		fprintf(stderr, ", no dir item");
	if (errors & REF_ERR_NO_DIR_INDEX)
		fprintf(stderr, ", no dir index");
	if (errors & REF_ERR_NO_INODE_REF)
		fprintf(stderr, ", no inode ref");
	if (errors & REF_ERR_DUP_DIR_ITEM)
		fprintf(stderr, ", dup dir item");
	if (errors & REF_ERR_DUP_DIR_INDEX)
		fprintf(stderr, ", dup dir index");
	if (errors & REF_ERR_DUP_INODE_REF)
		fprintf(stderr, ", dup inode ref");
	if (errors & REF_ERR_INDEX_UNMATCH)
		fprintf(stderr, ", index unmatch");
	if (errors & REF_ERR_FILETYPE_UNMATCH)
		fprintf(stderr, ", filetype unmatch");
	if (errors & REF_ERR_NAME_TOO_LONG)
		fprintf(stderr, ", name too long");
	if (errors & REF_ERR_NO_ROOT_REF)
		fprintf(stderr, ", no root ref");
	if (errors & REF_ERR_NO_ROOT_BACKREF)
		fprintf(stderr, ", no root backref");
	if (errors & REF_ERR_DUP_ROOT_REF)
		fprintf(stderr, ", dup root ref");
	if (errors & REF_ERR_DUP_ROOT_BACKREF)
		fprintf(stderr, ", dup root backref");
	fprintf(stderr, "\n");
}

static struct inode_record *get_inode_rec(struct cache_tree *inode_cache,
					  u64 ino, int mod)
{
	struct ptr_node *node;
	struct cache_extent *cache;
	struct inode_record *rec = NULL;
	int ret;

	cache = lookup_cache_extent(inode_cache, ino, 1);
	if (cache) {
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		if (mod && rec->refs > 1) {
			node->data = clone_inode_rec(rec);
			rec->refs--;
			rec = node->data;
		}
	} else if (mod) {
		rec = calloc(1, sizeof(*rec));
		rec->ino = ino;
		rec->extent_start = (u64)-1;
		rec->refs = 1;
		INIT_LIST_HEAD(&rec->backrefs);
		INIT_LIST_HEAD(&rec->orphan_extents);
		rec->holes = RB_ROOT;

		node = malloc(sizeof(*node));
		node->cache.start = ino;
		node->cache.size = 1;
		node->data = rec;

		if (ino == BTRFS_FREE_INO_OBJECTID)
			rec->found_link = 1;

		ret = insert_cache_extent(inode_cache, &node->cache);
		BUG_ON(ret);
	}
	return rec;
}

static void free_orphan_data_extents(struct list_head *orphan_extents)
{
	struct orphan_data_extent *orphan;

	while (!list_empty(orphan_extents)) {
		orphan = list_entry(orphan_extents->next,
				    struct orphan_data_extent, list);
		list_del(&orphan->list);
		free(orphan);
	}
}

static void free_inode_rec(struct inode_record *rec)
{
	struct inode_backref *backref;

	if (--rec->refs > 0)
		return;

	while (!list_empty(&rec->backrefs)) {
		backref = list_entry(rec->backrefs.next,
				     struct inode_backref, list);
		list_del(&backref->list);
		free(backref);
	}
	free_orphan_data_extents(&rec->orphan_extents);
	free_file_extent_holes(&rec->holes);
	free(rec);
}

static int can_free_inode_rec(struct inode_record *rec)
{
	if (!rec->errors && rec->checked && rec->found_inode_item &&
	    rec->nlink == rec->found_link && list_empty(&rec->backrefs))
		return 1;
	return 0;
}

static void maybe_free_inode_rec(struct cache_tree *inode_cache,
				 struct inode_record *rec)
{
	struct cache_extent *cache;
	struct inode_backref *tmp, *backref;
	struct ptr_node *node;
	unsigned char filetype;

	if (!rec->found_inode_item)
		return;

	filetype = imode_to_type(rec->imode);
	list_for_each_entry_safe(backref, tmp, &rec->backrefs, list) {
		if (backref->found_dir_item && backref->found_dir_index) {
			if (backref->filetype != filetype)
				backref->errors |= REF_ERR_FILETYPE_UNMATCH;
			if (!backref->errors && backref->found_inode_ref) {
				list_del(&backref->list);
				free(backref);
			}
		}
	}

	if (!rec->checked || rec->merging)
		return;

	if (S_ISDIR(rec->imode)) {
		if (rec->found_size != rec->isize)
			rec->errors |= I_ERR_DIR_ISIZE_WRONG;
		if (rec->found_file_extent)
			rec->errors |= I_ERR_ODD_FILE_EXTENT;
	} else if (S_ISREG(rec->imode) || S_ISLNK(rec->imode)) {
		if (rec->found_dir_item)
			rec->errors |= I_ERR_ODD_DIR_ITEM;
		if (rec->found_size != rec->nbytes)
			rec->errors |= I_ERR_FILE_NBYTES_WRONG;
		if (rec->nlink > 0 && !no_holes &&
		    (rec->extent_end < rec->isize ||
		     first_extent_gap(&rec->holes) < rec->isize))
			rec->errors |= I_ERR_FILE_EXTENT_DISCOUNT;
	}

	if (S_ISREG(rec->imode) || S_ISLNK(rec->imode)) {
		if (rec->found_csum_item && rec->nodatasum)
			rec->errors |= I_ERR_ODD_CSUM_ITEM;
		if (rec->some_csum_missing && !rec->nodatasum)
			rec->errors |= I_ERR_SOME_CSUM_MISSING;
	}

	BUG_ON(rec->refs != 1);
	if (can_free_inode_rec(rec)) {
		cache = lookup_cache_extent(inode_cache, rec->ino, 1);
		node = container_of(cache, struct ptr_node, cache);
		BUG_ON(node->data != rec);
		remove_cache_extent(inode_cache, &node->cache);
		free(node);
		free_inode_rec(rec);
	}
}

static int check_orphan_item(struct btrfs_root *root, u64 ino)
{
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;

	key.objectid = BTRFS_ORPHAN_OBJECTID;
	key.type = BTRFS_ORPHAN_ITEM_KEY;
	key.offset = ino;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	btrfs_release_path(&path);
	if (ret > 0)
		ret = -ENOENT;
	return ret;
}

static int process_inode_item(struct extent_buffer *eb,
			      int slot, struct btrfs_key *key,
			      struct shared_node *active_node)
{
	struct inode_record *rec;
	struct btrfs_inode_item *item;

	rec = active_node->current;
	BUG_ON(rec->ino != key->objectid || rec->refs > 1);
	if (rec->found_inode_item) {
		rec->errors |= I_ERR_DUP_INODE_ITEM;
		return 1;
	}
	item = btrfs_item_ptr(eb, slot, struct btrfs_inode_item);
	rec->nlink = btrfs_inode_nlink(eb, item);
	rec->isize = btrfs_inode_size(eb, item);
	rec->nbytes = btrfs_inode_nbytes(eb, item);
	rec->imode = btrfs_inode_mode(eb, item);
	if (btrfs_inode_flags(eb, item) & BTRFS_INODE_NODATASUM)
		rec->nodatasum = 1;
	rec->found_inode_item = 1;
	if (rec->nlink == 0)
		rec->errors |= I_ERR_NO_ORPHAN_ITEM;
	maybe_free_inode_rec(&active_node->inode_cache, rec);
	return 0;
}

static struct inode_backref *get_inode_backref(struct inode_record *rec,
						const char *name,
						int namelen, u64 dir)
{
	struct inode_backref *backref;

	list_for_each_entry(backref, &rec->backrefs, list) {
		if (rec->ino == BTRFS_MULTIPLE_OBJECTIDS)
			break;
		if (backref->dir != dir || backref->namelen != namelen)
			continue;
		if (memcmp(name, backref->name, namelen))
			continue;
		return backref;
	}

	backref = malloc(sizeof(*backref) + namelen + 1);
	memset(backref, 0, sizeof(*backref));
	backref->dir = dir;
	backref->namelen = namelen;
	memcpy(backref->name, name, namelen);
	backref->name[namelen] = '\0';
	list_add_tail(&backref->list, &rec->backrefs);
	return backref;
}

static int add_inode_backref(struct cache_tree *inode_cache,
			     u64 ino, u64 dir, u64 index,
			     const char *name, int namelen,
			     int filetype, int itemtype, int errors)
{
	struct inode_record *rec;
	struct inode_backref *backref;

	rec = get_inode_rec(inode_cache, ino, 1);
	backref = get_inode_backref(rec, name, namelen, dir);
	if (errors)
		backref->errors |= errors;
	if (itemtype == BTRFS_DIR_INDEX_KEY) {
		if (backref->found_dir_index)
			backref->errors |= REF_ERR_DUP_DIR_INDEX;
		if (backref->found_inode_ref && backref->index != index)
			backref->errors |= REF_ERR_INDEX_UNMATCH;
		if (backref->found_dir_item && backref->filetype != filetype)
			backref->errors |= REF_ERR_FILETYPE_UNMATCH;

		backref->index = index;
		backref->filetype = filetype;
		backref->found_dir_index = 1;
	} else if (itemtype == BTRFS_DIR_ITEM_KEY) {
		rec->found_link++;
		if (backref->found_dir_item)
			backref->errors |= REF_ERR_DUP_DIR_ITEM;
		if (backref->found_dir_index && backref->filetype != filetype)
			backref->errors |= REF_ERR_FILETYPE_UNMATCH;

		backref->filetype = filetype;
		backref->found_dir_item = 1;
	} else if ((itemtype == BTRFS_INODE_REF_KEY) ||
		   (itemtype == BTRFS_INODE_EXTREF_KEY)) {
		if (backref->found_inode_ref)
			backref->errors |= REF_ERR_DUP_INODE_REF;
		if (backref->found_dir_index && backref->index != index)
			backref->errors |= REF_ERR_INDEX_UNMATCH;
		else
			backref->index = index;

		backref->ref_type = itemtype;
		backref->found_inode_ref = 1;
	} else {
		BUG_ON(1);
	}

	maybe_free_inode_rec(inode_cache, rec);
	return 0;
}

static int merge_inode_recs(struct inode_record *src, struct inode_record *dst,
			    struct cache_tree *dst_cache)
{
	struct inode_backref *backref;
	u32 dir_count = 0;
	int ret = 0;

	dst->merging = 1;
	list_for_each_entry(backref, &src->backrefs, list) {
		if (backref->found_dir_index) {
			add_inode_backref(dst_cache, dst->ino, backref->dir,
					backref->index, backref->name,
					backref->namelen, backref->filetype,
					BTRFS_DIR_INDEX_KEY, backref->errors);
		}
		if (backref->found_dir_item) {
			dir_count++;
			add_inode_backref(dst_cache, dst->ino,
					backref->dir, 0, backref->name,
					backref->namelen, backref->filetype,
					BTRFS_DIR_ITEM_KEY, backref->errors);
		}
		if (backref->found_inode_ref) {
			add_inode_backref(dst_cache, dst->ino,
					backref->dir, backref->index,
					backref->name, backref->namelen, 0,
					backref->ref_type, backref->errors);
		}
	}

	if (src->found_dir_item)
		dst->found_dir_item = 1;
	if (src->found_file_extent)
		dst->found_file_extent = 1;
	if (src->found_csum_item)
		dst->found_csum_item = 1;
	if (src->some_csum_missing)
		dst->some_csum_missing = 1;
	if (first_extent_gap(&dst->holes) > first_extent_gap(&src->holes)) {
		ret = copy_file_extent_holes(&dst->holes, &src->holes);
		if (ret < 0)
			return ret;
	}

	BUG_ON(src->found_link < dir_count);
	dst->found_link += src->found_link - dir_count;
	dst->found_size += src->found_size;
	if (src->extent_start != (u64)-1) {
		if (dst->extent_start == (u64)-1) {
			dst->extent_start = src->extent_start;
			dst->extent_end = src->extent_end;
		} else {
			if (dst->extent_end > src->extent_start)
				dst->errors |= I_ERR_FILE_EXTENT_OVERLAP;
			else if (dst->extent_end < src->extent_start) {
				ret = add_file_extent_hole(&dst->holes,
					dst->extent_end,
					src->extent_start - dst->extent_end);
			}
			if (dst->extent_end < src->extent_end)
				dst->extent_end = src->extent_end;
		}
	}

	dst->errors |= src->errors;
	if (src->found_inode_item) {
		if (!dst->found_inode_item) {
			dst->nlink = src->nlink;
			dst->isize = src->isize;
			dst->nbytes = src->nbytes;
			dst->imode = src->imode;
			dst->nodatasum = src->nodatasum;
			dst->found_inode_item = 1;
		} else {
			dst->errors |= I_ERR_DUP_INODE_ITEM;
		}
	}
	dst->merging = 0;

	return 0;
}

static int splice_shared_node(struct shared_node *src_node,
			      struct shared_node *dst_node)
{
	struct cache_extent *cache;
	struct ptr_node *node, *ins;
	struct cache_tree *src, *dst;
	struct inode_record *rec, *conflict;
	u64 current_ino = 0;
	int splice = 0;
	int ret;

	if (--src_node->refs == 0)
		splice = 1;
	if (src_node->current)
		current_ino = src_node->current->ino;

	src = &src_node->root_cache;
	dst = &dst_node->root_cache;
again:
	cache = search_cache_extent(src, 0);
	while (cache) {
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		cache = next_cache_extent(cache);

		if (splice) {
			remove_cache_extent(src, &node->cache);
			ins = node;
		} else {
			ins = malloc(sizeof(*ins));
			ins->cache.start = node->cache.start;
			ins->cache.size = node->cache.size;
			ins->data = rec;
			rec->refs++;
		}
		ret = insert_cache_extent(dst, &ins->cache);
		if (ret == -EEXIST) {
			conflict = get_inode_rec(dst, rec->ino, 1);
			merge_inode_recs(rec, conflict, dst);
			if (rec->checked) {
				conflict->checked = 1;
				if (dst_node->current == conflict)
					dst_node->current = NULL;
			}
			maybe_free_inode_rec(dst, conflict);
			free_inode_rec(rec);
			free(ins);
		} else {
			BUG_ON(ret);
		}
	}

	if (src == &src_node->root_cache) {
		src = &src_node->inode_cache;
		dst = &dst_node->inode_cache;
		goto again;
	}

	if (current_ino > 0 && (!dst_node->current ||
	    current_ino > dst_node->current->ino)) {
		if (dst_node->current) {
			dst_node->current->checked = 1;
			maybe_free_inode_rec(dst, dst_node->current);
		}
		dst_node->current = get_inode_rec(dst, current_ino, 1);
	}
	return 0;
}

static void free_inode_ptr(struct cache_extent *cache)
{
	struct ptr_node *node;
	struct inode_record *rec;

	node = container_of(cache, struct ptr_node, cache);
	rec = node->data;
	free_inode_rec(rec);
	free(node);
}

FREE_EXTENT_CACHE_BASED_TREE(inode_recs, free_inode_ptr);

static struct shared_node *find_shared_node(struct cache_tree *shared,
					    u64 bytenr)
{
	struct cache_extent *cache;
	struct shared_node *node;

	cache = lookup_cache_extent(shared, bytenr, 1);
	if (cache) {
		node = container_of(cache, struct shared_node, cache);
		return node;
	}
	return NULL;
}

static int add_shared_node(struct cache_tree *shared, u64 bytenr, u32 refs)
{
	int ret;
	struct shared_node *node;

	node = calloc(1, sizeof(*node));
	node->cache.start = bytenr;
	node->cache.size = 1;
	cache_tree_init(&node->root_cache);
	cache_tree_init(&node->inode_cache);
	node->refs = refs;

	ret = insert_cache_extent(shared, &node->cache);
	BUG_ON(ret);
	return 0;
}

static int enter_shared_node(struct btrfs_root *root, u64 bytenr, u32 refs,
			     struct walk_control *wc, int level)
{
	struct shared_node *node;
	struct shared_node *dest;

	if (level == wc->active_node)
		return 0;

	BUG_ON(wc->active_node <= level);
	node = find_shared_node(&wc->shared, bytenr);
	if (!node) {
		add_shared_node(&wc->shared, bytenr, refs);
		node = find_shared_node(&wc->shared, bytenr);
		wc->nodes[level] = node;
		wc->active_node = level;
		return 0;
	}

	if (wc->root_level == wc->active_node &&
	    btrfs_root_refs(&root->root_item) == 0) {
		if (--node->refs == 0) {
			free_inode_recs_tree(&node->root_cache);
			free_inode_recs_tree(&node->inode_cache);
			remove_cache_extent(&wc->shared, &node->cache);
			free(node);
		}
		return 1;
	}

	dest = wc->nodes[wc->active_node];
	splice_shared_node(node, dest);
	if (node->refs == 0) {
		remove_cache_extent(&wc->shared, &node->cache);
		free(node);
	}
	return 1;
}

static int leave_shared_node(struct btrfs_root *root,
			     struct walk_control *wc, int level)
{
	struct shared_node *node;
	struct shared_node *dest;
	int i;

	if (level == wc->root_level)
		return 0;

	for (i = level + 1; i < BTRFS_MAX_LEVEL; i++) {
		if (wc->nodes[i])
			break;
	}
	BUG_ON(i >= BTRFS_MAX_LEVEL);

	node = wc->nodes[wc->active_node];
	wc->nodes[wc->active_node] = NULL;
	wc->active_node = i;

	dest = wc->nodes[wc->active_node];
	if (wc->active_node < wc->root_level ||
	    btrfs_root_refs(&root->root_item) > 0) {
		BUG_ON(node->refs <= 1);
		splice_shared_node(node, dest);
	} else {
		BUG_ON(node->refs < 2);
		node->refs--;
	}
	return 0;
}

/*
 * Returns:
 * < 0 - on error
 * 1   - if the root with id child_root_id is a child of root parent_root_id
 * 0   - if the root child_root_id isn't a child of the root parent_root_id but
 *       has other root(s) as parent(s)
 * 2   - if the root child_root_id doesn't have any parent roots
 */
static int is_child_root(struct btrfs_root *root, u64 parent_root_id,
			 u64 child_root_id)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	int has_parent = 0;
	int ret;

	btrfs_init_path(&path);

	key.objectid = parent_root_id;
	key.type = BTRFS_ROOT_REF_KEY;
	key.offset = child_root_id;
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root, &key, &path,
				0, 0);
	if (ret < 0)
		return ret;
	btrfs_release_path(&path);
	if (!ret)
		return 1;

	key.objectid = child_root_id;
	key.type = BTRFS_ROOT_BACKREF_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root, &key, &path,
				0, 0);
	if (ret < 0)
		goto out;

	while (1) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root->fs_info->tree_root, &path);
			if (ret)
				break;
			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid != child_root_id ||
		    key.type != BTRFS_ROOT_BACKREF_KEY)
			break;

		has_parent = 1;

		if (key.offset == parent_root_id) {
			btrfs_release_path(&path);
			return 1;
		}

		path.slots[0]++;
	}
out:
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;
	return has_parent ? 0 : 2;
}

static int process_dir_item(struct btrfs_root *root,
			    struct extent_buffer *eb,
			    int slot, struct btrfs_key *key,
			    struct shared_node *active_node)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u32 data_len;
	int error;
	int nritems = 0;
	int filetype;
	struct btrfs_dir_item *di;
	struct inode_record *rec;
	struct cache_tree *root_cache;
	struct cache_tree *inode_cache;
	struct btrfs_key location;
	char namebuf[BTRFS_NAME_LEN];

	root_cache = &active_node->root_cache;
	inode_cache = &active_node->inode_cache;
	rec = active_node->current;
	rec->found_dir_item = 1;

	di = btrfs_item_ptr(eb, slot, struct btrfs_dir_item);
	total = btrfs_item_size_nr(eb, slot);
	while (cur < total) {
		nritems++;
		btrfs_dir_item_key_to_cpu(eb, di, &location);
		name_len = btrfs_dir_name_len(eb, di);
		data_len = btrfs_dir_data_len(eb, di);
		filetype = btrfs_dir_type(eb, di);

		rec->found_size += name_len;
		if (name_len <= BTRFS_NAME_LEN) {
			len = name_len;
			error = 0;
		} else {
			len = BTRFS_NAME_LEN;
			error = REF_ERR_NAME_TOO_LONG;
		}
		read_extent_buffer(eb, namebuf, (unsigned long)(di + 1), len);

		if (location.type == BTRFS_INODE_ITEM_KEY) {
			add_inode_backref(inode_cache, location.objectid,
					  key->objectid, key->offset, namebuf,
					  len, filetype, key->type, error);
		} else if (location.type == BTRFS_ROOT_ITEM_KEY) {
			add_inode_backref(root_cache, location.objectid,
					  key->objectid, key->offset,
					  namebuf, len, filetype,
					  key->type, error);
		} else {
			fprintf(stderr, "invalid location in dir item %u\n",
				location.type);
			add_inode_backref(inode_cache, BTRFS_MULTIPLE_OBJECTIDS,
					  key->objectid, key->offset, namebuf,
					  len, filetype, key->type, error);
		}

		len = sizeof(*di) + name_len + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}
	if (key->type == BTRFS_DIR_INDEX_KEY && nritems > 1)
		rec->errors |= I_ERR_DUP_DIR_INDEX;

	return 0;
}

static int process_inode_ref(struct extent_buffer *eb,
			     int slot, struct btrfs_key *key,
			     struct shared_node *active_node)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	int error;
	struct cache_tree *inode_cache;
	struct btrfs_inode_ref *ref;
	char namebuf[BTRFS_NAME_LEN];

	inode_cache = &active_node->inode_cache;

	ref = btrfs_item_ptr(eb, slot, struct btrfs_inode_ref);
	total = btrfs_item_size_nr(eb, slot);
	while (cur < total) {
		name_len = btrfs_inode_ref_name_len(eb, ref);
		index = btrfs_inode_ref_index(eb, ref);
		if (name_len <= BTRFS_NAME_LEN) {
			len = name_len;
			error = 0;
		} else {
			len = BTRFS_NAME_LEN;
			error = REF_ERR_NAME_TOO_LONG;
		}
		read_extent_buffer(eb, namebuf, (unsigned long)(ref + 1), len);
		add_inode_backref(inode_cache, key->objectid, key->offset,
				  index, namebuf, len, 0, key->type, error);

		len = sizeof(*ref) + name_len;
		ref = (struct btrfs_inode_ref *)((char *)ref + len);
		cur += len;
	}
	return 0;
}

static int process_inode_extref(struct extent_buffer *eb,
				int slot, struct btrfs_key *key,
				struct shared_node *active_node)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	u64 parent;
	int error;
	struct cache_tree *inode_cache;
	struct btrfs_inode_extref *extref;
	char namebuf[BTRFS_NAME_LEN];

	inode_cache = &active_node->inode_cache;

	extref = btrfs_item_ptr(eb, slot, struct btrfs_inode_extref);
	total = btrfs_item_size_nr(eb, slot);
	while (cur < total) {
		name_len = btrfs_inode_extref_name_len(eb, extref);
		index = btrfs_inode_extref_index(eb, extref);
		parent = btrfs_inode_extref_parent(eb, extref);
		if (name_len <= BTRFS_NAME_LEN) {
			len = name_len;
			error = 0;
		} else {
			len = BTRFS_NAME_LEN;
			error = REF_ERR_NAME_TOO_LONG;
		}
		read_extent_buffer(eb, namebuf,
				   (unsigned long)(extref + 1), len);
		add_inode_backref(inode_cache, key->objectid, parent,
				  index, namebuf, len, 0, key->type, error);

		len = sizeof(*extref) + name_len;
		extref = (struct btrfs_inode_extref *)((char *)extref + len);
		cur += len;
	}
	return 0;

}

static int count_csum_range(struct btrfs_root *root, u64 start,
			    u64 len, u64 *found)
{
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	int ret;
	size_t size;
	*found = 0;
	u64 csum_end;
	u16 csum_size = btrfs_super_csum_size(root->fs_info->super_copy);

	btrfs_init_path(&path);

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.offset = start;
	key.type = BTRFS_EXTENT_CSUM_KEY;

	ret = btrfs_search_slot(NULL, root->fs_info->csum_root,
				&key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0 && path.slots[0] > 0) {
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0] - 1);
		if (key.objectid == BTRFS_EXTENT_CSUM_OBJECTID &&
		    key.type == BTRFS_EXTENT_CSUM_KEY)
			path.slots[0]--;
	}

	while (len > 0) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root->fs_info->csum_root, &path);
			if (ret > 0)
				break;
			else if (ret < 0)
				goto out;
			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
		    key.type != BTRFS_EXTENT_CSUM_KEY)
			break;

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.offset >= start + len)
			break;

		if (key.offset > start)
			start = key.offset;

		size = btrfs_item_size_nr(leaf, path.slots[0]);
		csum_end = key.offset + (size / csum_size) * root->sectorsize;
		if (csum_end > start) {
			size = min(csum_end - start, len);
			len -= size;
			start += size;
			*found += size;
		}

		path.slots[0]++;
	}
out:
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;
	return 0;
}

static int process_file_extent(struct btrfs_root *root,
				struct extent_buffer *eb,
				int slot, struct btrfs_key *key,
				struct shared_node *active_node)
{
	struct inode_record *rec;
	struct btrfs_file_extent_item *fi;
	u64 num_bytes = 0;
	u64 disk_bytenr = 0;
	u64 extent_offset = 0;
	u64 mask = root->sectorsize - 1;
	int extent_type;
	int ret;

	rec = active_node->current;
	BUG_ON(rec->ino != key->objectid || rec->refs > 1);
	rec->found_file_extent = 1;

	if (rec->extent_start == (u64)-1) {
		rec->extent_start = key->offset;
		rec->extent_end = key->offset;
	}

	if (rec->extent_end > key->offset)
		rec->errors |= I_ERR_FILE_EXTENT_OVERLAP;
	else if (rec->extent_end < key->offset) {
		ret = add_file_extent_hole(&rec->holes, rec->extent_end,
					   key->offset - rec->extent_end);
		if (ret < 0)
			return ret;
	}

	fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);
	extent_type = btrfs_file_extent_type(eb, fi);

	if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
		num_bytes = btrfs_file_extent_inline_len(eb, slot, fi);
		if (num_bytes == 0)
			rec->errors |= I_ERR_BAD_FILE_EXTENT;
		rec->found_size += num_bytes;
		num_bytes = (num_bytes + mask) & ~mask;
	} else if (extent_type == BTRFS_FILE_EXTENT_REG ||
		   extent_type == BTRFS_FILE_EXTENT_PREALLOC) {
		num_bytes = btrfs_file_extent_num_bytes(eb, fi);
		disk_bytenr = btrfs_file_extent_disk_bytenr(eb, fi);
		extent_offset = btrfs_file_extent_offset(eb, fi);
		if (num_bytes == 0 || (num_bytes & mask))
			rec->errors |= I_ERR_BAD_FILE_EXTENT;
		if (num_bytes + extent_offset >
		    btrfs_file_extent_ram_bytes(eb, fi))
			rec->errors |= I_ERR_BAD_FILE_EXTENT;
		if (extent_type == BTRFS_FILE_EXTENT_PREALLOC &&
		    (btrfs_file_extent_compression(eb, fi) ||
		     btrfs_file_extent_encryption(eb, fi) ||
		     btrfs_file_extent_other_encoding(eb, fi)))
			rec->errors |= I_ERR_BAD_FILE_EXTENT;
		if (disk_bytenr > 0)
			rec->found_size += num_bytes;
	} else {
		rec->errors |= I_ERR_BAD_FILE_EXTENT;
	}
	rec->extent_end = key->offset + num_bytes;

	/*
	 * The data reloc tree will copy full extents into its inode and then
	 * copy the corresponding csums.  Because the extent it copied could be
	 * a preallocated extent that hasn't been written to yet there may be no
	 * csums to copy, ergo we won't have csums for our file extent.  This is
	 * ok so just don't bother checking csums if the inode belongs to the
	 * data reloc tree.
	 */
	if (disk_bytenr > 0 &&
	    btrfs_header_owner(eb) != BTRFS_DATA_RELOC_TREE_OBJECTID) {
		u64 found;
		if (btrfs_file_extent_compression(eb, fi))
			num_bytes = btrfs_file_extent_disk_num_bytes(eb, fi);
		else
			disk_bytenr += extent_offset;

		ret = count_csum_range(root, disk_bytenr, num_bytes, &found);
		if (ret < 0)
			return ret;
		if (extent_type == BTRFS_FILE_EXTENT_REG) {
			if (found > 0)
				rec->found_csum_item = 1;
			if (found < num_bytes)
				rec->some_csum_missing = 1;
		} else if (extent_type == BTRFS_FILE_EXTENT_PREALLOC) {
			if (found > 0)
				rec->errors |= I_ERR_ODD_CSUM_ITEM;
		}
	}
	return 0;
}

static int process_one_leaf(struct btrfs_root *root, struct extent_buffer *eb,
			    struct walk_control *wc)
{
	struct btrfs_key key;
	u32 nritems;
	int i;
	int ret = 0;
	struct cache_tree *inode_cache;
	struct shared_node *active_node;

	if (wc->root_level == wc->active_node &&
	    btrfs_root_refs(&root->root_item) == 0)
		return 0;

	active_node = wc->nodes[wc->active_node];
	inode_cache = &active_node->inode_cache;
	nritems = btrfs_header_nritems(eb);
	for (i = 0; i < nritems; i++) {
		btrfs_item_key_to_cpu(eb, &key, i);

		if (key.objectid == BTRFS_FREE_SPACE_OBJECTID)
			continue;
		if (key.type == BTRFS_ORPHAN_ITEM_KEY)
			continue;

		if (active_node->current == NULL ||
		    active_node->current->ino < key.objectid) {
			if (active_node->current) {
				active_node->current->checked = 1;
				maybe_free_inode_rec(inode_cache,
						     active_node->current);
			}
			active_node->current = get_inode_rec(inode_cache,
							     key.objectid, 1);
		}
		switch (key.type) {
		case BTRFS_DIR_ITEM_KEY:
		case BTRFS_DIR_INDEX_KEY:
			ret = process_dir_item(root, eb, i, &key, active_node);
			break;
		case BTRFS_INODE_REF_KEY:
			ret = process_inode_ref(eb, i, &key, active_node);
			break;
		case BTRFS_INODE_EXTREF_KEY:
			ret = process_inode_extref(eb, i, &key, active_node);
			break;
		case BTRFS_INODE_ITEM_KEY:
			ret = process_inode_item(eb, i, &key, active_node);
			break;
		case BTRFS_EXTENT_DATA_KEY:
			ret = process_file_extent(root, eb, i, &key,
						  active_node);
			break;
		default:
			break;
		};
	}
	return ret;
}

static void reada_walk_down(struct btrfs_root *root,
			    struct extent_buffer *node, int slot)
{
	u64 bytenr;
	u64 ptr_gen;
	u32 nritems;
	u32 blocksize;
	int i;
	int level;

	level = btrfs_header_level(node);
	if (level != 1)
		return;

	nritems = btrfs_header_nritems(node);
	blocksize = btrfs_level_size(root, level - 1);
	for (i = slot; i < nritems; i++) {
		bytenr = btrfs_node_blockptr(node, i);
		ptr_gen = btrfs_node_ptr_generation(node, i);
		readahead_tree_block(root, bytenr, blocksize, ptr_gen);
	}
}

/*
 * Check the child node/leaf by the following condition:
 * 1. the first item key of the node/leaf should be the same with the one
 *    in parent.
 * 2. block in parent node should match the child node/leaf.
 * 3. generation of parent node and child's header should be consistent.
 *
 * Or the child node/leaf pointed by the key in parent is not valid.
 *
 * We hope to check leaf owner too, but since subvol may share leaves,
 * which makes leaf owner check not so strong, key check should be
 * sufficient enough for that case.
 */
static int check_child_node(struct btrfs_root *root,
			    struct extent_buffer *parent, int slot,
			    struct extent_buffer *child)
{
	struct btrfs_key parent_key;
	struct btrfs_key child_key;
	int ret = 0;

	btrfs_node_key_to_cpu(parent, &parent_key, slot);
	if (btrfs_header_level(child) == 0)
		btrfs_item_key_to_cpu(child, &child_key, 0);
	else
		btrfs_node_key_to_cpu(child, &child_key, 0);

	if (memcmp(&parent_key, &child_key, sizeof(parent_key))) {
		ret = -EINVAL;
		fprintf(stderr,
			"Wrong key of child node/leaf, wanted: (%llu, %u, %llu), have: (%llu, %u, %llu)\n",
			parent_key.objectid, parent_key.type, parent_key.offset,
			child_key.objectid, child_key.type, child_key.offset);
	}
	if (btrfs_header_bytenr(child) != btrfs_node_blockptr(parent, slot)) {
		ret = -EINVAL;
		fprintf(stderr, "Wrong block of child node/leaf, wanted: %llu, have: %llu\n",
			btrfs_node_blockptr(parent, slot),
			btrfs_header_bytenr(child));
	}
	if (btrfs_node_ptr_generation(parent, slot) !=
	    btrfs_header_generation(child)) {
		ret = -EINVAL;
		fprintf(stderr, "Wrong generation of child node/leaf, wanted: %llu, have: %llu\n",
			btrfs_header_generation(child),
			btrfs_node_ptr_generation(parent, slot));
	}
	return ret;
}

static int walk_down_tree(struct btrfs_root *root, struct btrfs_path *path,
			  struct walk_control *wc, int *level)
{
	enum btrfs_tree_block_status status;
	u64 bytenr;
	u64 ptr_gen;
	struct extent_buffer *next;
	struct extent_buffer *cur;
	u32 blocksize;
	int ret, err = 0;
	u64 refs;

	WARN_ON(*level < 0);
	WARN_ON(*level >= BTRFS_MAX_LEVEL);
	ret = btrfs_lookup_extent_info(NULL, root,
				       path->nodes[*level]->start,
				       *level, 1, &refs, NULL);
	if (ret < 0) {
		err = ret;
		goto out;
	}

	if (refs > 1) {
		ret = enter_shared_node(root, path->nodes[*level]->start,
					refs, wc, *level);
		if (ret > 0) {
			err = ret;
			goto out;
		}
	}

	while (*level >= 0) {
		WARN_ON(*level < 0);
		WARN_ON(*level >= BTRFS_MAX_LEVEL);
		cur = path->nodes[*level];

		if (btrfs_header_level(cur) != *level)
			WARN_ON(1);

		if (path->slots[*level] >= btrfs_header_nritems(cur))
			break;
		if (*level == 0) {
			ret = process_one_leaf(root, cur, wc);
			if (ret < 0)
				err = ret;
			break;
		}
		bytenr = btrfs_node_blockptr(cur, path->slots[*level]);
		ptr_gen = btrfs_node_ptr_generation(cur, path->slots[*level]);
		blocksize = btrfs_level_size(root, *level - 1);
		ret = btrfs_lookup_extent_info(NULL, root, bytenr, *level - 1,
					       1, &refs, NULL);
		if (ret < 0)
			refs = 0;

		if (refs > 1) {
			ret = enter_shared_node(root, bytenr, refs,
						wc, *level - 1);
			if (ret > 0) {
				path->slots[*level]++;
				continue;
			}
		}

		next = btrfs_find_tree_block(root, bytenr, blocksize);
		if (!next || !btrfs_buffer_uptodate(next, ptr_gen)) {
			free_extent_buffer(next);
			reada_walk_down(root, cur, path->slots[*level]);
			next = read_tree_block(root, bytenr, blocksize,
					       ptr_gen);
			if (!extent_buffer_uptodate(next)) {
				struct btrfs_key node_key;

				btrfs_node_key_to_cpu(path->nodes[*level],
						      &node_key,
						      path->slots[*level]);
				btrfs_add_corrupt_extent_record(root->fs_info,
						&node_key,
						path->nodes[*level]->start,
						root->leafsize, *level);
				err = -EIO;
				goto out;
			}
		}

		ret = check_child_node(root, cur, path->slots[*level], next);
		if (ret) {
			err = ret;
			goto out;
		}

		if (btrfs_is_leaf(next))
			status = btrfs_check_leaf(root, NULL, next);
		else
			status = btrfs_check_node(root, NULL, next);
		if (status != BTRFS_TREE_BLOCK_CLEAN) {
			free_extent_buffer(next);
			err = -EIO;
			goto out;
		}

		*level = *level - 1;
		free_extent_buffer(path->nodes[*level]);
		path->nodes[*level] = next;
		path->slots[*level] = 0;
	}
out:
	path->slots[*level] = btrfs_header_nritems(path->nodes[*level]);
	return err;
}

static int walk_up_tree(struct btrfs_root *root, struct btrfs_path *path,
			struct walk_control *wc, int *level)
{
	int i;
	struct extent_buffer *leaf;

	for (i = *level; i < BTRFS_MAX_LEVEL - 1 && path->nodes[i]; i++) {
		leaf = path->nodes[i];
		if (path->slots[i] + 1 < btrfs_header_nritems(leaf)) {
			path->slots[i]++;
			*level = i;
			return 0;
		} else {
			free_extent_buffer(path->nodes[*level]);
			path->nodes[*level] = NULL;
			BUG_ON(*level > wc->active_node);
			if (*level == wc->active_node)
				leave_shared_node(root, wc, *level);
			*level = i + 1;
		}
	}
	return 1;
}

static int check_root_dir(struct inode_record *rec)
{
	struct inode_backref *backref;
	int ret = -1;

	if (!rec->found_inode_item || rec->errors)
		goto out;
	if (rec->nlink != 1 || rec->found_link != 0)
		goto out;
	if (list_empty(&rec->backrefs))
		goto out;
	backref = list_entry(rec->backrefs.next, struct inode_backref, list);
	if (!backref->found_inode_ref)
		goto out;
	if (backref->index != 0 || backref->namelen != 2 ||
	    memcmp(backref->name, "..", 2))
		goto out;
	if (backref->found_dir_index || backref->found_dir_item)
		goto out;
	ret = 0;
out:
	return ret;
}

static int repair_inode_isize(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, struct btrfs_path *path,
			      struct inode_record *rec)
{
	struct btrfs_inode_item *ei;
	struct btrfs_key key;
	int ret;

	key.objectid = rec->ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret < 0)
		goto out;
	if (ret) {
		if (!path->slots[0]) {
			ret = -ENOENT;
			goto out;
		}
		path->slots[0]--;
		ret = 0;
	}
	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	if (key.objectid != rec->ino) {
		ret = -ENOENT;
		goto out;
	}

	ei = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_size(path->nodes[0], ei, rec->found_size);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	rec->errors &= ~I_ERR_DIR_ISIZE_WRONG;
	printf("reset isize for dir %Lu root %Lu\n", rec->ino,
	       root->root_key.objectid);
out:
	btrfs_release_path(path);
	return ret;
}

static int repair_inode_orphan_item(struct btrfs_trans_handle *trans,
				    struct btrfs_root *root,
				    struct btrfs_path *path,
				    struct inode_record *rec)
{
	int ret;

	ret = btrfs_add_orphan_item(trans, root, path, rec->ino);
	btrfs_release_path(path);
	if (!ret)
		rec->errors &= ~I_ERR_NO_ORPHAN_ITEM;
	return ret;
}

static int repair_inode_nbytes(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root,
			       struct btrfs_path *path,
			       struct inode_record *rec)
{
	struct btrfs_inode_item *ei;
	struct btrfs_key key;
	int ret = 0;

	key.objectid = rec->ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret) {
		if (ret > 0)
			ret = -ENOENT;
		goto out;
	}

	/* Since ret == 0, no need to check anything */
	ei = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_nbytes(path->nodes[0], ei, rec->found_size);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	rec->errors &= ~I_ERR_FILE_NBYTES_WRONG;
	printf("reset nbytes for ino %llu root %llu\n",
	       rec->ino, root->root_key.objectid);
out:
	btrfs_release_path(path);
	return ret;
}

static int add_missing_dir_index(struct btrfs_root *root,
				 struct cache_tree *inode_cache,
				 struct inode_record *rec,
				 struct inode_backref *backref)
{
	struct btrfs_path *path;
	struct btrfs_trans_handle *trans;
	struct btrfs_dir_item *dir_item;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_disk_key disk_key;
	struct inode_record *dir_rec;
	unsigned long name_ptr;
	u32 data_size = sizeof(*dir_item) + backref->namelen;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		return PTR_ERR(trans);
	}

	fprintf(stderr, "repairing missing dir index item for inode %llu\n",
		(unsigned long long)rec->ino);
	key.objectid = backref->dir;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = backref->index;

	ret = btrfs_insert_empty_item(trans, root, path, &key, data_size);
	BUG_ON(ret);

	leaf = path->nodes[0];
	dir_item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_dir_item);

	disk_key.objectid = cpu_to_le64(rec->ino);
	disk_key.type = BTRFS_INODE_ITEM_KEY;
	disk_key.offset = 0;

	btrfs_set_dir_item_key(leaf, dir_item, &disk_key);
	btrfs_set_dir_type(leaf, dir_item, imode_to_type(rec->imode));
	btrfs_set_dir_data_len(leaf, dir_item, 0);
	btrfs_set_dir_name_len(leaf, dir_item, backref->namelen);
	name_ptr = (unsigned long)(dir_item + 1);
	write_extent_buffer(leaf, backref->name, name_ptr, backref->namelen);
	btrfs_mark_buffer_dirty(leaf);
	btrfs_free_path(path);
	btrfs_commit_transaction(trans, root);

	backref->found_dir_index = 1;
	dir_rec = get_inode_rec(inode_cache, backref->dir, 0);
	if (!dir_rec)
		return 0;
	dir_rec->found_size += backref->namelen;
	if (dir_rec->found_size == dir_rec->isize &&
	    (dir_rec->errors & I_ERR_DIR_ISIZE_WRONG))
		dir_rec->errors &= ~I_ERR_DIR_ISIZE_WRONG;
	if (dir_rec->found_size != dir_rec->isize)
		dir_rec->errors |= I_ERR_DIR_ISIZE_WRONG;

	return 0;
}

static int delete_dir_index(struct btrfs_root *root,
			    struct cache_tree *inode_cache,
			    struct inode_record *rec,
			    struct inode_backref *backref)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_dir_item *di;
	struct btrfs_path *path;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		return PTR_ERR(trans);
	}


	fprintf(stderr, "Deleting bad dir index [%llu,%u,%llu] root %llu\n",
		(unsigned long long)backref->dir,
		BTRFS_DIR_INDEX_KEY, (unsigned long long)backref->index,
		(unsigned long long)root->objectid);

	di = btrfs_lookup_dir_index(trans, root, path, backref->dir,
				    backref->name, backref->namelen,
				    backref->index, -1);
	if (IS_ERR(di)) {
		ret = PTR_ERR(di);
		btrfs_free_path(path);
		btrfs_commit_transaction(trans, root);
		if (ret == -ENOENT)
			return 0;
		return ret;
	}

	if (!di)
		ret = btrfs_del_item(trans, root, path);
	else
		ret = btrfs_delete_one_dir_name(trans, root, path, di);
	BUG_ON(ret);
	btrfs_free_path(path);
	btrfs_commit_transaction(trans, root);
	return ret;
}

static int create_inode_item(struct btrfs_root *root,
			     struct inode_record *rec,
			     struct inode_backref *backref, int root_dir)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_inode_item inode_item;
	time_t now = time(NULL);
	int ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		return ret;
	}

	fprintf(stderr, "root %llu inode %llu recreating inode item, this may "
		"be incomplete, please check permissions and content after "
		"the fsck completes.\n", (unsigned long long)root->objectid,
		(unsigned long long)rec->ino);

	memset(&inode_item, 0, sizeof(inode_item));
	btrfs_set_stack_inode_generation(&inode_item, trans->transid);
	if (root_dir)
		btrfs_set_stack_inode_nlink(&inode_item, 1);
	else
		btrfs_set_stack_inode_nlink(&inode_item, rec->found_link);
	btrfs_set_stack_inode_nbytes(&inode_item, rec->found_size);
	if (rec->found_dir_item) {
		if (rec->found_file_extent)
			fprintf(stderr, "root %llu inode %llu has both a dir "
				"item and extents, unsure if it is a dir or a "
				"regular file so setting it as a directory\n",
				(unsigned long long)root->objectid,
				(unsigned long long)rec->ino);
		btrfs_set_stack_inode_mode(&inode_item, S_IFDIR | 0755);
		btrfs_set_stack_inode_size(&inode_item, rec->found_size);
	} else if (!rec->found_dir_item) {
		btrfs_set_stack_inode_size(&inode_item, rec->extent_end);
		btrfs_set_stack_inode_mode(&inode_item, S_IFREG | 0755);
	}
	btrfs_set_stack_timespec_sec(&inode_item.atime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.atime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.ctime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.ctime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.mtime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.mtime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.otime, 0);
	btrfs_set_stack_timespec_nsec(&inode_item.otime, 0);

	ret = btrfs_insert_inode(trans, root, rec->ino, &inode_item);
	BUG_ON(ret);
	btrfs_commit_transaction(trans, root);
	return 0;
}

static int repair_inode_backrefs(struct btrfs_root *root,
				 struct inode_record *rec,
				 struct cache_tree *inode_cache,
				 int delete)
{
	struct inode_backref *tmp, *backref;
	u64 root_dirid = btrfs_root_dirid(&root->root_item);
	int ret = 0;
	int repaired = 0;

	list_for_each_entry_safe(backref, tmp, &rec->backrefs, list) {
		if (!delete && rec->ino == root_dirid) {
			if (!rec->found_inode_item) {
				ret = create_inode_item(root, rec, backref, 1);
				if (ret)
					break;
				repaired++;
			}
		}

		/* Index 0 for root dir's are special, don't mess with it */
		if (rec->ino == root_dirid && backref->index == 0)
			continue;

		if (delete &&
		    ((backref->found_dir_index && !backref->found_inode_ref) ||
		     (backref->found_dir_index && backref->found_inode_ref &&
		      (backref->errors & REF_ERR_INDEX_UNMATCH)))) {
			ret = delete_dir_index(root, inode_cache, rec, backref);
			if (ret)
				break;
			repaired++;
			list_del(&backref->list);
			free(backref);
		}

		if (!delete && !backref->found_dir_index &&
		    backref->found_dir_item && backref->found_inode_ref) {
			ret = add_missing_dir_index(root, inode_cache, rec,
						    backref);
			if (ret)
				break;
			repaired++;
			if (backref->found_dir_item &&
			    backref->found_dir_index &&
			    backref->found_dir_index) {
				if (!backref->errors &&
				    backref->found_inode_ref) {
					list_del(&backref->list);
					free(backref);
				}
			}
		}

		if (!delete && (!backref->found_dir_index &&
				!backref->found_dir_item &&
				backref->found_inode_ref)) {
			struct btrfs_trans_handle *trans;
			struct btrfs_key location;

			ret = check_dir_conflict(root, backref->name,
						 backref->namelen,
						 backref->dir,
						 backref->index);
			if (ret) {
				/*
				 * let nlink fixing routine to handle it,
				 * which can do it better.
				 */
				ret = 0;
				break;
			}
			location.objectid = rec->ino;
			location.type = BTRFS_INODE_ITEM_KEY;
			location.offset = 0;

			trans = btrfs_start_transaction(root, 1);
			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				break;
			}
			fprintf(stderr, "adding missing dir index/item pair "
				"for inode %llu\n",
				(unsigned long long)rec->ino);
			ret = btrfs_insert_dir_item(trans, root, backref->name,
						    backref->namelen,
						    backref->dir, &location,
						    imode_to_type(rec->imode),
						    backref->index);
			BUG_ON(ret);
			btrfs_commit_transaction(trans, root);
			repaired++;
		}

		if (!delete && (backref->found_inode_ref &&
				backref->found_dir_index &&
				backref->found_dir_item &&
				!(backref->errors & REF_ERR_INDEX_UNMATCH) &&
				!rec->found_inode_item)) {
			ret = create_inode_item(root, rec, backref, 0);
			if (ret)
				break;
			repaired++;
		}

	}
	return ret ? ret : repaired;
}

/*
 * To determine the file type for nlink/inode_item repair
 *
 * Return 0 if file type is found and BTRFS_FT_* is stored into type.
 * Return -ENOENT if file type is not found.
 */
static int find_file_type(struct inode_record *rec, u8 *type)
{
	struct inode_backref *backref;

	/* For inode item recovered case */
	if (rec->found_inode_item) {
		*type = imode_to_type(rec->imode);
		return 0;
	}

	list_for_each_entry(backref, &rec->backrefs, list) {
		if (backref->found_dir_index || backref->found_dir_item) {
			*type = backref->filetype;
			return 0;
		}
	}
	return -ENOENT;
}

/*
 * To determine the file name for nlink repair
 *
 * Return 0 if file name is found, set name and namelen.
 * Return -ENOENT if file name is not found.
 */
static int find_file_name(struct inode_record *rec,
			  char *name, int *namelen)
{
	struct inode_backref *backref;

	list_for_each_entry(backref, &rec->backrefs, list) {
		if (backref->found_dir_index || backref->found_dir_item ||
		    backref->found_inode_ref) {
			memcpy(name, backref->name, backref->namelen);
			*namelen = backref->namelen;
			return 0;
		}
	}
	return -ENOENT;
}

/* Reset the nlink of the inode to the correct one */
static int reset_nlink(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root,
		       struct btrfs_path *path,
		       struct inode_record *rec)
{
	struct inode_backref *backref;
	struct inode_backref *tmp;
	struct btrfs_key key;
	struct btrfs_inode_item *inode_item;
	int ret = 0;

	/* We don't believe this either, reset it and iterate backref */
	rec->found_link = 0;

	/* Remove all backref including the valid ones */
	list_for_each_entry_safe(backref, tmp, &rec->backrefs, list) {
		ret = btrfs_unlink(trans, root, rec->ino, backref->dir,
				   backref->index, backref->name,
				   backref->namelen, 0);
		if (ret < 0)
			goto out;

		/* remove invalid backref, so it won't be added back */
		if (!(backref->found_dir_index &&
		      backref->found_dir_item &&
		      backref->found_inode_ref)) {
			list_del(&backref->list);
			free(backref);
		} else {
			rec->found_link++;
		}
	}

	/* Set nlink to 0 */
	key.objectid = rec->ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}
	inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_inode_item);
	btrfs_set_inode_nlink(path->nodes[0], inode_item, 0);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	btrfs_release_path(path);

	/*
	 * Add back valid inode_ref/dir_item/dir_index,
	 * add_link() will handle the nlink inc, so new nlink must be correct
	 */
	list_for_each_entry(backref, &rec->backrefs, list) {
		ret = btrfs_add_link(trans, root, rec->ino, backref->dir,
				     backref->name, backref->namelen,
				     backref->ref_type, &backref->index, 1);
		if (ret < 0)
			goto out;
	}
out:
	btrfs_release_path(path);
	return ret;
}

static int repair_inode_nlinks(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root,
			       struct btrfs_path *path,
			       struct inode_record *rec)
{
	char *dir_name = "lost+found";
	char namebuf[BTRFS_NAME_LEN] = {0};
	u64 lost_found_ino;
	u32 mode = 0700;
	u8 type = 0;
	int namelen = 0;
	int name_recovered = 0;
	int type_recovered = 0;
	int ret = 0;

	/*
	 * Get file name and type first before these invalid inode ref
	 * are deleted by remove_all_invalid_backref()
	 */
	name_recovered = !find_file_name(rec, namebuf, &namelen);
	type_recovered = !find_file_type(rec, &type);

	if (!name_recovered) {
		printf("Can't get file name for inode %llu, using '%llu' as fallback\n",
		       rec->ino, rec->ino);
		namelen = count_digits(rec->ino);
		sprintf(namebuf, "%llu", rec->ino);
		name_recovered = 1;
	}
	if (!type_recovered) {
		printf("Can't get file type for inode %llu, using FILE as fallback\n",
		       rec->ino);
		type = BTRFS_FT_REG_FILE;
		type_recovered = 1;
	}

	ret = reset_nlink(trans, root, path, rec);
	if (ret < 0) {
		fprintf(stderr,
			"Failed to reset nlink for inode %llu: %s\n",
			rec->ino, strerror(-ret));
		goto out;
	}

	if (rec->found_link == 0) {
		lost_found_ino = root->highest_inode;
		if (lost_found_ino >= BTRFS_LAST_FREE_OBJECTID) {
			ret = -EOVERFLOW;
			goto out;
		}
		lost_found_ino++;
		ret = btrfs_mkdir(trans, root, dir_name, strlen(dir_name),
				  BTRFS_FIRST_FREE_OBJECTID, &lost_found_ino,
				  mode);
		if (ret < 0) {
			fprintf(stderr, "Failed to create '%s' dir: %s\n",
				dir_name, strerror(-ret));
			goto out;
		}
		ret = btrfs_add_link(trans, root, rec->ino, lost_found_ino,
				     namebuf, namelen, type, NULL, 1);
		/*
		 * Add ".INO" suffix several times to handle case where
		 * "FILENAME.INO" is already taken by another file.
		 */
		while (ret == -EEXIST) {
			/*
			 * Conflicting file name, add ".INO" as suffix * +1 for '.'
			 */
			if (namelen + count_digits(rec->ino) + 1 >
			    BTRFS_NAME_LEN) {
				ret = -EFBIG;
				goto out;
			}
			snprintf(namebuf + namelen, BTRFS_NAME_LEN - namelen,
				 ".%llu", rec->ino);
			namelen += count_digits(rec->ino) + 1;
			ret = btrfs_add_link(trans, root, rec->ino,
					     lost_found_ino, namebuf,
					     namelen, type, NULL, 1);
		}
		if (ret < 0) {
			fprintf(stderr,
				"Failed to link the inode %llu to %s dir: %s\n",
				rec->ino, dir_name, strerror(-ret));
			goto out;
		}
		/*
		 * Just increase the found_link, don't actually add the
		 * backref. This will make things easier and this inode
		 * record will be freed after the repair is done.
		 * So fsck will not report problem about this inode.
		 */
		rec->found_link++;
		printf("Moving file '%.*s' to '%s' dir since it has no valid backref\n",
		       namelen, namebuf, dir_name);
	}
	printf("Fixed the nlink of inode %llu\n", rec->ino);
out:
	/*
	 * Clear the flag anyway, or we will loop forever for the same inode
	 * as it will not be removed from the bad inode list and the dead loop
	 * happens.
	 */
	rec->errors &= ~I_ERR_LINK_COUNT_WRONG;
	btrfs_release_path(path);
	return ret;
}

/*
 * Check if there is any normal(reg or prealloc) file extent for given
 * ino.
 * This is used to determine the file type when neither its dir_index/item or
 * inode_item exists.
 *
 * This will *NOT* report error, if any error happens, just consider it does
 * not have any normal file extent.
 */
static int find_normal_file_extent(struct btrfs_root *root, u64 ino)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_file_extent_item *fi;
	u8 type;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		goto out;
	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0) {
		ret = 0;
		goto out;
	}
	if (ret && path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
		ret = btrfs_next_leaf(root, path);
		if (ret) {
			ret = 0;
			goto out;
		}
	}
	while (1) {
		btrfs_item_key_to_cpu(path->nodes[0], &found_key,
				      path->slots[0]);
		if (found_key.objectid != ino ||
		    found_key.type != BTRFS_EXTENT_DATA_KEY)
			break;
		fi = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_file_extent_item);
		type = btrfs_file_extent_type(path->nodes[0], fi);
		if (type != BTRFS_FILE_EXTENT_INLINE) {
			ret = 1;
			goto out;
		}
	}
out:
	btrfs_free_path(path);
	return ret;
}

static u32 btrfs_type_to_imode(u8 type)
{
	static u32 imode_by_btrfs_type[] = {
		[BTRFS_FT_REG_FILE]	= S_IFREG,
		[BTRFS_FT_DIR]		= S_IFDIR,
		[BTRFS_FT_CHRDEV]	= S_IFCHR,
		[BTRFS_FT_BLKDEV]	= S_IFBLK,
		[BTRFS_FT_FIFO]		= S_IFIFO,
		[BTRFS_FT_SOCK]		= S_IFSOCK,
		[BTRFS_FT_SYMLINK]	= S_IFLNK,
	};

	return imode_by_btrfs_type[(type)];
}

static int repair_inode_no_item(struct btrfs_trans_handle *trans,
				struct btrfs_root *root,
				struct btrfs_path *path,
				struct inode_record *rec)
{
	u8 filetype;
	u32 mode = 0700;
	int type_recovered = 0;
	int ret = 0;

	printf("Trying to rebuild inode:%llu\n", rec->ino);

	type_recovered = !find_file_type(rec, &filetype);

	/*
	 * Try to determine inode type if type not found.
	 *
	 * For found regular file extent, it must be FILE.
	 * For found dir_item/index, it must be DIR.
	 *
	 * For undetermined one, use FILE as fallback.
	 *
	 * TODO:
	 * 1. If found backref(inode_index/item is already handled) to it,
	 *    it must be DIR.
	 *    Need new inode-inode ref structure to allow search for that.
	 */
	if (!type_recovered) {
		if (rec->found_file_extent &&
		    find_normal_file_extent(root, rec->ino)) {
			type_recovered = 1;
			filetype = BTRFS_FT_REG_FILE;
		} else if (rec->found_dir_item) {
			type_recovered = 1;
			filetype = BTRFS_FT_DIR;
		} else if (!list_empty(&rec->orphan_extents)) {
			type_recovered = 1;
			filetype = BTRFS_FT_REG_FILE;
		} else{
			printf("Can't determint the filetype for inode %llu, assume it is a normal file\n",
			       rec->ino);
			type_recovered = 1;
			filetype = BTRFS_FT_REG_FILE;
		}
	}

	ret = btrfs_new_inode(trans, root, rec->ino,
			      mode | btrfs_type_to_imode(filetype));
	if (ret < 0)
		goto out;

	/*
	 * Here inode rebuild is done, we only rebuild the inode item,
	 * don't repair the nlink(like move to lost+found).
	 * That is the job of nlink repair.
	 *
	 * We just fill the record and return
	 */
	rec->found_dir_item = 1;
	rec->imode = mode | btrfs_type_to_imode(filetype);
	rec->nlink = 0;
	rec->errors &= ~I_ERR_NO_INODE_ITEM;
	/* Ensure the inode_nlinks repair function will be called */
	rec->errors |= I_ERR_LINK_COUNT_WRONG;
out:
	return ret;
}

static int repair_inode_orphan_extent(struct btrfs_trans_handle *trans,
				      struct btrfs_root *root,
				      struct btrfs_path *path,
				      struct inode_record *rec)
{
	struct orphan_data_extent *orphan;
	struct orphan_data_extent *tmp;
	int ret = 0;

	list_for_each_entry_safe(orphan, tmp, &rec->orphan_extents, list) {
		/*
		 * Check for conflicting file extents
		 *
		 * Here we don't know whether the extents is compressed or not,
		 * so we can only assume it not compressed nor data offset,
		 * and use its disk_len as extent length.
		 */
		ret = btrfs_get_extent(NULL, root, path, orphan->objectid,
				       orphan->offset, orphan->disk_len, 0);
		btrfs_release_path(path);
		if (ret < 0)
			goto out;
		if (!ret) {
			fprintf(stderr,
				"orphan extent (%llu, %llu) conflicts, delete the orphan\n",
				orphan->disk_bytenr, orphan->disk_len);
			ret = btrfs_free_extent(trans,
					root->fs_info->extent_root,
					orphan->disk_bytenr, orphan->disk_len,
					0, root->objectid, orphan->objectid,
					orphan->offset);
			if (ret < 0)
				goto out;
		}
		ret = btrfs_insert_file_extent(trans, root, orphan->objectid,
				orphan->offset, orphan->disk_bytenr,
				orphan->disk_len, orphan->disk_len);
		if (ret < 0)
			goto out;

		/* Update file size info */
		rec->found_size += orphan->disk_len;
		if (rec->found_size == rec->nbytes)
			rec->errors &= ~I_ERR_FILE_NBYTES_WRONG;

		/* Update the file extent hole info too */
		ret = del_file_extent_hole(&rec->holes, orphan->offset,
					   orphan->disk_len);
		if (ret < 0)
			goto out;
		if (RB_EMPTY_ROOT(&rec->holes))
			rec->errors &= ~I_ERR_FILE_EXTENT_DISCOUNT;

		list_del(&orphan->list);
		free(orphan);
	}
	rec->errors &= ~I_ERR_FILE_EXTENT_ORPHAN;
out:
	return ret;
}

static int repair_inode_discount_extent(struct btrfs_trans_handle *trans,
					struct btrfs_root *root,
					struct btrfs_path *path,
					struct inode_record *rec)
{
	struct rb_node *node;
	struct file_extent_hole *hole;
	int found = 0;
	int ret = 0;

	node = rb_first(&rec->holes);

	while (node) {
		found = 1;
		hole = rb_entry(node, struct file_extent_hole, node);
		ret = btrfs_punch_hole(trans, root, rec->ino,
				       hole->start, hole->len);
		if (ret < 0)
			goto out;
		ret = del_file_extent_hole(&rec->holes, hole->start,
					   hole->len);
		if (ret < 0)
			goto out;
		if (RB_EMPTY_ROOT(&rec->holes))
			rec->errors &= ~I_ERR_FILE_EXTENT_DISCOUNT;
		node = rb_first(&rec->holes);
	}
	/* special case for a file losing all its file extent */
	if (!found) {
		ret = btrfs_punch_hole(trans, root, rec->ino, 0,
				       round_up(rec->isize, root->sectorsize));
		if (ret < 0)
			goto out;
	}
	printf("Fixed discount file extents for inode: %llu in root: %llu\n",
	       rec->ino, root->objectid);
out:
	return ret;
}

static int try_repair_inode(struct btrfs_root *root, struct inode_record *rec)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_path *path;
	int ret = 0;

	if (!(rec->errors & (I_ERR_DIR_ISIZE_WRONG |
			     I_ERR_NO_ORPHAN_ITEM |
			     I_ERR_LINK_COUNT_WRONG |
			     I_ERR_NO_INODE_ITEM |
			     I_ERR_FILE_EXTENT_ORPHAN |
			     I_ERR_FILE_EXTENT_DISCOUNT|
			     I_ERR_FILE_NBYTES_WRONG)))
		return rec->errors;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * For nlink repair, it may create a dir and add link, so
	 * 2 for parent(256)'s dir_index and dir_item
	 * 2 for lost+found dir's inode_item and inode_ref
	 * 1 for the new inode_ref of the file
	 * 2 for lost+found dir's dir_index and dir_item for the file
	 */
	trans = btrfs_start_transaction(root, 7);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		return PTR_ERR(trans);
	}

	if (rec->errors & I_ERR_NO_INODE_ITEM)
		ret = repair_inode_no_item(trans, root, path, rec);
	if (!ret && rec->errors & I_ERR_FILE_EXTENT_ORPHAN)
		ret = repair_inode_orphan_extent(trans, root, path, rec);
	if (!ret && rec->errors & I_ERR_FILE_EXTENT_DISCOUNT)
		ret = repair_inode_discount_extent(trans, root, path, rec);
	if (!ret && rec->errors & I_ERR_DIR_ISIZE_WRONG)
		ret = repair_inode_isize(trans, root, path, rec);
	if (!ret && rec->errors & I_ERR_NO_ORPHAN_ITEM)
		ret = repair_inode_orphan_item(trans, root, path, rec);
	if (!ret && rec->errors & I_ERR_LINK_COUNT_WRONG)
		ret = repair_inode_nlinks(trans, root, path, rec);
	if (!ret && rec->errors & I_ERR_FILE_NBYTES_WRONG)
		ret = repair_inode_nbytes(trans, root, path, rec);
	btrfs_commit_transaction(trans, root);
	btrfs_free_path(path);
	return ret;
}

static int check_inode_recs(struct btrfs_root *root,
			    struct cache_tree *inode_cache)
{
	struct cache_extent *cache;
	struct ptr_node *node;
	struct inode_record *rec;
	struct inode_backref *backref;
	int stage = 0;
	int ret = 0;
	int err = 0;
	u64 error = 0;
	u64 root_dirid = btrfs_root_dirid(&root->root_item);

	if (btrfs_root_refs(&root->root_item) == 0) {
		if (!cache_tree_empty(inode_cache))
			fprintf(stderr, "warning line %d\n", __LINE__);
		return 0;
	}

	/*
	 * We need to record the highest inode number for later 'lost+found'
	 * dir creation.
	 * We must select a ino not used/refered by any existing inode, or
	 * 'lost+found' ino may be a missing ino in a corrupted leaf,
	 * this may cause 'lost+found' dir has wrong nlinks.
	 */
	cache = last_cache_extent(inode_cache);
	if (cache) {
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		if (rec->ino > root->highest_inode)
			root->highest_inode = rec->ino;
	}

	/*
	 * We need to repair backrefs first because we could change some of the
	 * errors in the inode recs.
	 *
	 * We also need to go through and delete invalid backrefs first and then
	 * add the correct ones second.  We do this because we may get EEXIST
	 * when adding back the correct index because we hadn't yet deleted the
	 * invalid index.
	 *
	 * For example, if we were missing a dir index then the directories
	 * isize would be wrong, so if we fixed the isize to what we thought it
	 * would be and then fixed the backref we'd still have a invalid fs, so
	 * we need to add back the dir index and then check to see if the isize
	 * is still wrong.
	 */
	while (stage < 3) {
		stage++;
		if (stage == 3 && !err)
			break;

		cache = search_cache_extent(inode_cache, 0);
		while (repair && cache) {
			node = container_of(cache, struct ptr_node, cache);
			rec = node->data;
			cache = next_cache_extent(cache);

			/* Need to free everything up and rescan */
			if (stage == 3) {
				remove_cache_extent(inode_cache, &node->cache);
				free(node);
				free_inode_rec(rec);
				continue;
			}

			if (list_empty(&rec->backrefs))
				continue;

			ret = repair_inode_backrefs(root, rec, inode_cache,
						    stage == 1);
			if (ret < 0) {
				err = ret;
				stage = 2;
				break;
			} if (ret > 0) {
				err = -EAGAIN;
			}
		}
	}
	if (err)
		return err;

	rec = get_inode_rec(inode_cache, root_dirid, 0);
	if (rec) {
		ret = check_root_dir(rec);
		if (ret) {
			fprintf(stderr, "root %llu root dir %llu error\n",
				(unsigned long long)root->root_key.objectid,
				(unsigned long long)root_dirid);
			print_inode_error(root, rec);
			error++;
		}
	} else {
		if (repair) {
			struct btrfs_trans_handle *trans;

			trans = btrfs_start_transaction(root, 1);
			if (IS_ERR(trans)) {
				err = PTR_ERR(trans);
				return err;
			}

			fprintf(stderr,
				"root %llu missing its root dir, recreating\n",
				(unsigned long long)root->objectid);

			ret = btrfs_make_root_dir(trans, root, root_dirid);
			BUG_ON(ret);

			btrfs_commit_transaction(trans, root);
			return -EAGAIN;
		}

		fprintf(stderr, "root %llu root dir %llu not found\n",
			(unsigned long long)root->root_key.objectid,
			(unsigned long long)root_dirid);
	}

	while (1) {
		cache = search_cache_extent(inode_cache, 0);
		if (!cache)
			break;
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		remove_cache_extent(inode_cache, &node->cache);
		free(node);
		if (rec->ino == root_dirid ||
		    rec->ino == BTRFS_ORPHAN_OBJECTID) {
			free_inode_rec(rec);
			continue;
		}

		if (rec->errors & I_ERR_NO_ORPHAN_ITEM) {
			ret = check_orphan_item(root, rec->ino);
			if (ret == 0)
				rec->errors &= ~I_ERR_NO_ORPHAN_ITEM;
			if (can_free_inode_rec(rec)) {
				free_inode_rec(rec);
				continue;
			}
		}

		if (!rec->found_inode_item)
			rec->errors |= I_ERR_NO_INODE_ITEM;
		if (rec->found_link != rec->nlink)
			rec->errors |= I_ERR_LINK_COUNT_WRONG;
		if (repair) {
			ret = try_repair_inode(root, rec);
			if (ret == 0 && can_free_inode_rec(rec)) {
				free_inode_rec(rec);
				continue;
			}
			ret = 0;
		}

		if (!(repair && ret == 0))
			error++;
		print_inode_error(root, rec);
		list_for_each_entry(backref, &rec->backrefs, list) {
			if (!backref->found_dir_item)
				backref->errors |= REF_ERR_NO_DIR_ITEM;
			if (!backref->found_dir_index)
				backref->errors |= REF_ERR_NO_DIR_INDEX;
			if (!backref->found_inode_ref)
				backref->errors |= REF_ERR_NO_INODE_REF;
			fprintf(stderr, "\tunresolved ref dir %llu index %llu"
				" namelen %u name %s filetype %d errors %x",
				(unsigned long long)backref->dir,
				(unsigned long long)backref->index,
				backref->namelen, backref->name,
				backref->filetype, backref->errors);
			print_ref_error(backref->errors);
		}
		free_inode_rec(rec);
	}
	return (error > 0) ? -1 : 0;
}

static struct root_record *get_root_rec(struct cache_tree *root_cache,
					u64 objectid)
{
	struct cache_extent *cache;
	struct root_record *rec = NULL;
	int ret;

	cache = lookup_cache_extent(root_cache, objectid, 1);
	if (cache) {
		rec = container_of(cache, struct root_record, cache);
	} else {
		rec = calloc(1, sizeof(*rec));
		rec->objectid = objectid;
		INIT_LIST_HEAD(&rec->backrefs);
		rec->cache.start = objectid;
		rec->cache.size = 1;

		ret = insert_cache_extent(root_cache, &rec->cache);
		BUG_ON(ret);
	}
	return rec;
}

static struct root_backref *get_root_backref(struct root_record *rec,
					     u64 ref_root, u64 dir, u64 index,
					     const char *name, int namelen)
{
	struct root_backref *backref;

	list_for_each_entry(backref, &rec->backrefs, list) {
		if (backref->ref_root != ref_root || backref->dir != dir ||
		    backref->namelen != namelen)
			continue;
		if (memcmp(name, backref->name, namelen))
			continue;
		return backref;
	}

	backref = calloc(1, sizeof(*backref) + namelen + 1);
	backref->ref_root = ref_root;
	backref->dir = dir;
	backref->index = index;
	backref->namelen = namelen;
	memcpy(backref->name, name, namelen);
	backref->name[namelen] = '\0';
	list_add_tail(&backref->list, &rec->backrefs);
	return backref;
}

static void free_root_record(struct cache_extent *cache)
{
	struct root_record *rec;
	struct root_backref *backref;

	rec = container_of(cache, struct root_record, cache);
	while (!list_empty(&rec->backrefs)) {
		backref = list_entry(rec->backrefs.next,
				     struct root_backref, list);
		list_del(&backref->list);
		free(backref);
	}

	kfree(rec);
}

FREE_EXTENT_CACHE_BASED_TREE(root_recs, free_root_record);

static int add_root_backref(struct cache_tree *root_cache,
			    u64 root_id, u64 ref_root, u64 dir, u64 index,
			    const char *name, int namelen,
			    int item_type, int errors)
{
	struct root_record *rec;
	struct root_backref *backref;

	rec = get_root_rec(root_cache, root_id);
	backref = get_root_backref(rec, ref_root, dir, index, name, namelen);

	backref->errors |= errors;

	if (item_type != BTRFS_DIR_ITEM_KEY) {
		if (backref->found_dir_index || backref->found_back_ref ||
		    backref->found_forward_ref) {
			if (backref->index != index)
				backref->errors |= REF_ERR_INDEX_UNMATCH;
		} else {
			backref->index = index;
		}
	}

	if (item_type == BTRFS_DIR_ITEM_KEY) {
		if (backref->found_forward_ref)
			rec->found_ref++;
		backref->found_dir_item = 1;
	} else if (item_type == BTRFS_DIR_INDEX_KEY) {
		backref->found_dir_index = 1;
	} else if (item_type == BTRFS_ROOT_REF_KEY) {
		if (backref->found_forward_ref)
			backref->errors |= REF_ERR_DUP_ROOT_REF;
		else if (backref->found_dir_item)
			rec->found_ref++;
		backref->found_forward_ref = 1;
	} else if (item_type == BTRFS_ROOT_BACKREF_KEY) {
		if (backref->found_back_ref)
			backref->errors |= REF_ERR_DUP_ROOT_BACKREF;
		backref->found_back_ref = 1;
	} else {
		BUG_ON(1);
	}

	if (backref->found_forward_ref && backref->found_dir_item)
		backref->reachable = 1;
	return 0;
}

static int merge_root_recs(struct btrfs_root *root,
			   struct cache_tree *src_cache,
			   struct cache_tree *dst_cache)
{
	struct cache_extent *cache;
	struct ptr_node *node;
	struct inode_record *rec;
	struct inode_backref *backref;
	int ret = 0;

	if (root->root_key.objectid == BTRFS_TREE_RELOC_OBJECTID) {
		free_inode_recs_tree(src_cache);
		return 0;
	}

	while (1) {
		cache = search_cache_extent(src_cache, 0);
		if (!cache)
			break;
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		remove_cache_extent(src_cache, &node->cache);
		free(node);

		ret = is_child_root(root, root->objectid, rec->ino);
		if (ret < 0)
			break;
		else if (ret == 0)
			goto skip;

		list_for_each_entry(backref, &rec->backrefs, list) {
			BUG_ON(backref->found_inode_ref);
			if (backref->found_dir_item)
				add_root_backref(dst_cache, rec->ino,
					root->root_key.objectid, backref->dir,
					backref->index, backref->name,
					backref->namelen, BTRFS_DIR_ITEM_KEY,
					backref->errors);
			if (backref->found_dir_index)
				add_root_backref(dst_cache, rec->ino,
					root->root_key.objectid, backref->dir,
					backref->index, backref->name,
					backref->namelen, BTRFS_DIR_INDEX_KEY,
					backref->errors);
		}
skip:
		free_inode_rec(rec);
	}
	if (ret < 0)
		return ret;
	return 0;
}

static int check_root_refs(struct btrfs_root *root,
			   struct cache_tree *root_cache)
{
	struct root_record *rec;
	struct root_record *ref_root;
	struct root_backref *backref;
	struct cache_extent *cache;
	int loop = 1;
	int ret;
	int error;
	int errors = 0;

	rec = get_root_rec(root_cache, BTRFS_FS_TREE_OBJECTID);
	rec->found_ref = 1;

	/* fixme: this can not detect circular references */
	while (loop) {
		loop = 0;
		cache = search_cache_extent(root_cache, 0);
		while (1) {
			if (!cache)
				break;
			rec = container_of(cache, struct root_record, cache);
			cache = next_cache_extent(cache);

			if (rec->found_ref == 0)
				continue;

			list_for_each_entry(backref, &rec->backrefs, list) {
				if (!backref->reachable)
					continue;

				ref_root = get_root_rec(root_cache,
							backref->ref_root);
				if (ref_root->found_ref > 0)
					continue;

				backref->reachable = 0;
				rec->found_ref--;
				if (rec->found_ref == 0)
					loop = 1;
			}
		}
	}

	cache = search_cache_extent(root_cache, 0);
	while (1) {
		if (!cache)
			break;
		rec = container_of(cache, struct root_record, cache);
		cache = next_cache_extent(cache);

		if (rec->found_ref == 0 &&
		    rec->objectid >= BTRFS_FIRST_FREE_OBJECTID &&
		    rec->objectid <= BTRFS_LAST_FREE_OBJECTID) {
			ret = check_orphan_item(root->fs_info->tree_root,
						rec->objectid);
			if (ret == 0)
				continue;

			/*
			 * If we don't have a root item then we likely just have
			 * a dir item in a snapshot for this root but no actual
			 * ref key or anything so it's meaningless.
			 */
			if (!rec->found_root_item)
				continue;
			errors++;
			fprintf(stderr, "fs tree %llu not referenced\n",
				(unsigned long long)rec->objectid);
		}

		error = 0;
		if (rec->found_ref > 0 && !rec->found_root_item)
			error = 1;
		list_for_each_entry(backref, &rec->backrefs, list) {
			if (!backref->found_dir_item)
				backref->errors |= REF_ERR_NO_DIR_ITEM;
			if (!backref->found_dir_index)
				backref->errors |= REF_ERR_NO_DIR_INDEX;
			if (!backref->found_back_ref)
				backref->errors |= REF_ERR_NO_ROOT_BACKREF;
			if (!backref->found_forward_ref)
				backref->errors |= REF_ERR_NO_ROOT_REF;
			if (backref->reachable && backref->errors)
				error = 1;
		}
		if (!error)
			continue;

		errors++;
		fprintf(stderr, "fs tree %llu refs %u %s\n",
			(unsigned long long)rec->objectid, rec->found_ref,
			 rec->found_root_item ? "" : "not found");

		list_for_each_entry(backref, &rec->backrefs, list) {
			if (!backref->reachable)
				continue;
			if (!backref->errors && rec->found_root_item)
				continue;
			fprintf(stderr, "\tunresolved ref root %llu dir %llu"
				" index %llu namelen %u name %s errors %x\n",
				(unsigned long long)backref->ref_root,
				(unsigned long long)backref->dir,
				(unsigned long long)backref->index,
				backref->namelen, backref->name,
				backref->errors);
			print_ref_error(backref->errors);
		}
	}
	return errors > 0 ? 1 : 0;
}

static int process_root_ref(struct extent_buffer *eb, int slot,
			    struct btrfs_key *key,
			    struct cache_tree *root_cache)
{
	u64 dirid;
	u64 index;
	u32 len;
	u32 name_len;
	struct btrfs_root_ref *ref;
	char namebuf[BTRFS_NAME_LEN];
	int error;

	ref = btrfs_item_ptr(eb, slot, struct btrfs_root_ref);

	dirid = btrfs_root_ref_dirid(eb, ref);
	index = btrfs_root_ref_sequence(eb, ref);
	name_len = btrfs_root_ref_name_len(eb, ref);

	if (name_len <= BTRFS_NAME_LEN) {
		len = name_len;
		error = 0;
	} else {
		len = BTRFS_NAME_LEN;
		error = REF_ERR_NAME_TOO_LONG;
	}
	read_extent_buffer(eb, namebuf, (unsigned long)(ref + 1), len);

	if (key->type == BTRFS_ROOT_REF_KEY) {
		add_root_backref(root_cache, key->offset, key->objectid, dirid,
				 index, namebuf, len, key->type, error);
	} else {
		add_root_backref(root_cache, key->objectid, key->offset, dirid,
				 index, namebuf, len, key->type, error);
	}
	return 0;
}

static void free_corrupt_block(struct cache_extent *cache)
{
	struct btrfs_corrupt_block *corrupt;

	corrupt = container_of(cache, struct btrfs_corrupt_block, cache);
	free(corrupt);
}

FREE_EXTENT_CACHE_BASED_TREE(corrupt_blocks, free_corrupt_block);

/*
 * Repair the btree of the given root.
 *
 * The fix is to remove the node key in corrupt_blocks cache_tree.
 * and rebalance the tree.
 * After the fix, the btree should be writeable.
 */
static int repair_btree(struct btrfs_root *root,
			struct cache_tree *corrupt_blocks)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_path *path;
	struct btrfs_corrupt_block *corrupt;
	struct cache_extent *cache;
	struct btrfs_key key;
	u64 offset;
	int level;
	int ret = 0;

	if (cache_tree_empty(corrupt_blocks))
		return 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		fprintf(stderr, "Error starting transaction: %s\n",
			strerror(-ret));
		goto out_free_path;
	}
	cache = first_cache_extent(corrupt_blocks);
	while (cache) {
		corrupt = container_of(cache, struct btrfs_corrupt_block,
				       cache);
		level = corrupt->level;
		path->lowest_level = level;
		key.objectid = corrupt->key.objectid;
		key.type = corrupt->key.type;
		key.offset = corrupt->key.offset;

		/*
		 * Here we don't want to do any tree balance, since it may
		 * cause a balance with corrupted brother leaf/node,
		 * so ins_len set to 0 here.
		 * Balance will be done after all corrupt node/leaf is deleted.
		 */
		ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
		if (ret < 0)
			goto out;
		offset = btrfs_node_blockptr(path->nodes[level],
					     path->slots[level]);

		/* Remove the ptr */
		ret = btrfs_del_ptr(trans, root, path, level,
				    path->slots[level]);
		if (ret < 0)
			goto out;
		/*
		 * Remove the corresponding extent
		 * return value is not concerned.
		 */
		btrfs_release_path(path);
		ret = btrfs_free_extent(trans, root, offset, root->nodesize,
					0, root->root_key.objectid,
					level - 1, 0);
		cache = next_cache_extent(cache);
	}

	/* Balance the btree using btrfs_search_slot() */
	cache = first_cache_extent(corrupt_blocks);
	while (cache) {
		corrupt = container_of(cache, struct btrfs_corrupt_block,
				       cache);
		memcpy(&key, &corrupt->key, sizeof(key));
		ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret < 0)
			goto out;
		/* return will always >0 since it won't find the item */
		ret = 0;
		btrfs_release_path(path);
		cache = next_cache_extent(cache);
	}
out:
	btrfs_commit_transaction(trans, root);
out_free_path:
	btrfs_free_path(path);
	return ret;
}

static int check_fs_root(struct btrfs_root *root,
			 struct cache_tree *root_cache,
			 struct walk_control *wc)
{
	int ret = 0;
	int err = 0;
	int wret;
	int level;
	struct btrfs_path path;
	struct shared_node root_node;
	struct root_record *rec;
	struct btrfs_root_item *root_item = &root->root_item;
	struct cache_tree corrupt_blocks;
	struct orphan_data_extent *orphan;
	struct orphan_data_extent *tmp;
	enum btrfs_tree_block_status status;

	/*
	 * Reuse the corrupt_block cache tree to record corrupted tree block
	 *
	 * Unlike the usage in extent tree check, here we do it in a per
	 * fs/subvol tree base.
	 */
	cache_tree_init(&corrupt_blocks);
	root->fs_info->corrupt_blocks = &corrupt_blocks;

	if (root->root_key.objectid != BTRFS_TREE_RELOC_OBJECTID) {
		rec = get_root_rec(root_cache, root->root_key.objectid);
		if (btrfs_root_refs(root_item) > 0)
			rec->found_root_item = 1;
	}

	btrfs_init_path(&path);
	memset(&root_node, 0, sizeof(root_node));
	cache_tree_init(&root_node.root_cache);
	cache_tree_init(&root_node.inode_cache);

	/* Move the orphan extent record to corresponding inode_record */
	list_for_each_entry_safe(orphan, tmp,
				 &root->orphan_data_extents, list) {
		struct inode_record *inode;

		inode = get_inode_rec(&root_node.inode_cache, orphan->objectid,
				      1);
		inode->errors |= I_ERR_FILE_EXTENT_ORPHAN;
		list_move(&orphan->list, &inode->orphan_extents);
	}

	level = btrfs_header_level(root->node);
	memset(wc->nodes, 0, sizeof(wc->nodes));
	wc->nodes[level] = &root_node;
	wc->active_node = level;
	wc->root_level = level;

	/* We may not have checked the root block, lets do that now */
	if (btrfs_is_leaf(root->node))
		status = btrfs_check_leaf(root, NULL, root->node);
	else
		status = btrfs_check_node(root, NULL, root->node);
	if (status != BTRFS_TREE_BLOCK_CLEAN)
		return -EIO;

	if (btrfs_root_refs(root_item) > 0 ||
	    btrfs_disk_key_objectid(&root_item->drop_progress) == 0) {
		path.nodes[level] = root->node;
		extent_buffer_get(root->node);
		path.slots[level] = 0;
	} else {
		struct btrfs_key key;
		struct btrfs_disk_key found_key;

		btrfs_disk_key_to_cpu(&key, &root_item->drop_progress);
		level = root_item->drop_level;
		path.lowest_level = level;
		wret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
		if (wret < 0)
			goto skip_walking;
		btrfs_node_key(path.nodes[level], &found_key,
				path.slots[level]);
		WARN_ON(memcmp(&found_key, &root_item->drop_progress,
					sizeof(found_key)));
	}

	while (1) {
		wret = walk_down_tree(root, &path, wc, &level);
		if (wret < 0)
			ret = wret;
		if (wret != 0)
			break;

		wret = walk_up_tree(root, &path, wc, &level);
		if (wret < 0)
			ret = wret;
		if (wret != 0)
			break;
	}
skip_walking:
	btrfs_release_path(&path);

	if (!cache_tree_empty(&corrupt_blocks)) {
		struct cache_extent *cache;
		struct btrfs_corrupt_block *corrupt;

		printf("The following tree block(s) is corrupted in tree %llu:\n",
		       root->root_key.objectid);
		cache = first_cache_extent(&corrupt_blocks);
		while (cache) {
			corrupt = container_of(cache,
					       struct btrfs_corrupt_block,
					       cache);
			printf("\ttree block bytenr: %llu, level: %d, node key: (%llu, %u, %llu)\n",
			       cache->start, corrupt->level,
			       corrupt->key.objectid, corrupt->key.type,
			       corrupt->key.offset);
			cache = next_cache_extent(cache);
		}
		if (repair) {
			printf("Try to repair the btree for root %llu\n",
			       root->root_key.objectid);
			ret = repair_btree(root, &corrupt_blocks);
			if (ret < 0)
				fprintf(stderr, "Failed to repair btree: %s\n",
					strerror(-ret));
			if (!ret)
				printf("Btree for root %llu is fixed\n",
				       root->root_key.objectid);
		}
	}

	err = merge_root_recs(root, &root_node.root_cache, root_cache);
	if (err < 0)
		ret = err;

	if (root_node.current) {
		root_node.current->checked = 1;
		maybe_free_inode_rec(&root_node.inode_cache,
				root_node.current);
	}

	err = check_inode_recs(root, &root_node.inode_cache);
	if (!ret)
		ret = err;

	free_corrupt_blocks_tree(&corrupt_blocks);
	root->fs_info->corrupt_blocks = NULL;
	free_orphan_data_extents(&root->orphan_data_extents);
	return ret;
}

static int fs_root_objectid(u64 objectid)
{
	if (objectid == BTRFS_TREE_RELOC_OBJECTID ||
	    objectid == BTRFS_DATA_RELOC_TREE_OBJECTID)
		return 1;
	return is_fstree(objectid);
}

static int check_fs_roots(struct btrfs_root *root,
			  struct cache_tree *root_cache)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct walk_control wc;
	struct extent_buffer *leaf, *tree_node;
	struct btrfs_root *tmp_root;
	struct btrfs_root *tree_root = root->fs_info->tree_root;
	int ret;
	int err = 0;

	if (ctx.progress_enabled) {
		ctx.tp = TASK_FS_ROOTS;
		task_start(ctx.info);
	}

	/*
	 * Just in case we made any changes to the extent tree that weren't
	 * reflected into the free space cache yet.
	 */
	if (repair)
		reset_cached_block_groups(root->fs_info);
	memset(&wc, 0, sizeof(wc));
	cache_tree_init(&wc.shared);
	btrfs_init_path(&path);

again:
	key.offset = 0;
	key.objectid = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	if (ret < 0) {
		err = 1;
		goto out;
	}
	tree_node = tree_root->node;
	while (1) {
		if (tree_node != tree_root->node) {
			free_root_recs_tree(root_cache);
			btrfs_release_path(&path);
			goto again;
		}
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(tree_root, &path);
			if (ret) {
				if (ret < 0)
					err = 1;
				break;
			}
			leaf = path.nodes[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type == BTRFS_ROOT_ITEM_KEY &&
		    fs_root_objectid(key.objectid)) {
			if (key.objectid == BTRFS_TREE_RELOC_OBJECTID) {
				tmp_root = btrfs_read_fs_root_no_cache(
						root->fs_info, &key);
			} else {
				key.offset = (u64)-1;
				tmp_root = btrfs_read_fs_root(
						root->fs_info, &key);
			}
			if (IS_ERR(tmp_root)) {
				err = 1;
				goto next;
			}
			ret = check_fs_root(tmp_root, root_cache, &wc);
			if (ret == -EAGAIN) {
				free_root_recs_tree(root_cache);
				btrfs_release_path(&path);
				goto again;
			}
			if (ret)
				err = 1;
			if (key.objectid == BTRFS_TREE_RELOC_OBJECTID)
				btrfs_free_fs_root(tmp_root);
		} else if (key.type == BTRFS_ROOT_REF_KEY ||
			   key.type == BTRFS_ROOT_BACKREF_KEY) {
			process_root_ref(leaf, path.slots[0], &key,
					 root_cache);
		}
next:
		path.slots[0]++;
	}
out:
	btrfs_release_path(&path);
	if (err)
		free_extent_cache_tree(&wc.shared);
	if (!cache_tree_empty(&wc.shared))
		fprintf(stderr, "warning line %d\n", __LINE__);

	task_stop(ctx.info);

	return err;
}

static int all_backpointers_checked(struct extent_record *rec, int print_errs)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *back;
	struct tree_backref *tback;
	struct data_backref *dback;
	u64 found = 0;
	int err = 0;

	while(cur != &rec->backrefs) {
		back = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (!back->found_extent_tree) {
			err = 1;
			if (!print_errs)
				goto out;
			if (back->is_data) {
				dback = (struct data_backref *)back;
				fprintf(stderr, "Backref %llu %s %llu"
					" owner %llu offset %llu num_refs %lu"
					" not found in extent tree\n",
					(unsigned long long)rec->start,
					back->full_backref ?
					"parent" : "root",
					back->full_backref ?
					(unsigned long long)dback->parent:
					(unsigned long long)dback->root,
					(unsigned long long)dback->owner,
					(unsigned long long)dback->offset,
					(unsigned long)dback->num_refs);
			} else {
				tback = (struct tree_backref *)back;
				fprintf(stderr, "Backref %llu parent %llu"
					" root %llu not found in extent tree\n",
					(unsigned long long)rec->start,
					(unsigned long long)tback->parent,
					(unsigned long long)tback->root);
			}
		}
		if (!back->is_data && !back->found_ref) {
			err = 1;
			if (!print_errs)
				goto out;
			tback = (struct tree_backref *)back;
			fprintf(stderr, "Backref %llu %s %llu not referenced back %p\n",
				(unsigned long long)rec->start,
				back->full_backref ? "parent" : "root",
				back->full_backref ?
				(unsigned long long)tback->parent :
				(unsigned long long)tback->root, back);
		}
		if (back->is_data) {
			dback = (struct data_backref *)back;
			if (dback->found_ref != dback->num_refs) {
				err = 1;
				if (!print_errs)
					goto out;
				fprintf(stderr, "Incorrect local backref count"
					" on %llu %s %llu owner %llu"
					" offset %llu found %u wanted %u back %p\n",
					(unsigned long long)rec->start,
					back->full_backref ?
					"parent" : "root",
					back->full_backref ?
					(unsigned long long)dback->parent:
					(unsigned long long)dback->root,
					(unsigned long long)dback->owner,
					(unsigned long long)dback->offset,
					dback->found_ref, dback->num_refs, back);
			}
			if (dback->disk_bytenr != rec->start) {
				err = 1;
				if (!print_errs)
					goto out;
				fprintf(stderr, "Backref disk bytenr does not"
					" match extent record, bytenr=%llu, "
					"ref bytenr=%llu\n",
					(unsigned long long)rec->start,
					(unsigned long long)dback->disk_bytenr);
			}

			if (dback->bytes != rec->nr) {
				err = 1;
				if (!print_errs)
					goto out;
				fprintf(stderr, "Backref bytes do not match "
					"extent backref, bytenr=%llu, ref "
					"bytes=%llu, backref bytes=%llu\n",
					(unsigned long long)rec->start,
					(unsigned long long)rec->nr,
					(unsigned long long)dback->bytes);
			}
		}
		if (!back->is_data) {
			found += 1;
		} else {
			dback = (struct data_backref *)back;
			found += dback->found_ref;
		}
	}
	if (found != rec->refs) {
		err = 1;
		if (!print_errs)
			goto out;
		fprintf(stderr, "Incorrect global backref count "
			"on %llu found %llu wanted %llu\n",
			(unsigned long long)rec->start,
			(unsigned long long)found,
			(unsigned long long)rec->refs);
	}
out:
	return err;
}

static int free_all_extent_backrefs(struct extent_record *rec)
{
	struct extent_backref *back;
	struct list_head *cur;
	while (!list_empty(&rec->backrefs)) {
		cur = rec->backrefs.next;
		back = list_entry(cur, struct extent_backref, list);
		list_del(cur);
		free(back);
	}
	return 0;
}

static void free_extent_record_cache(struct btrfs_fs_info *fs_info,
				     struct cache_tree *extent_cache)
{
	struct cache_extent *cache;
	struct extent_record *rec;

	while (1) {
		cache = first_cache_extent(extent_cache);
		if (!cache)
			break;
		rec = container_of(cache, struct extent_record, cache);
		remove_cache_extent(extent_cache, cache);
		free_all_extent_backrefs(rec);
		free(rec);
	}
}

static int maybe_free_extent_rec(struct cache_tree *extent_cache,
				 struct extent_record *rec)
{
	if (rec->content_checked && rec->owner_ref_checked &&
	    rec->extent_item_refs == rec->refs && rec->refs > 0 &&
	    rec->num_duplicates == 0 && !all_backpointers_checked(rec, 0) &&
	    !rec->bad_full_backref && !rec->crossing_stripes &&
	    !rec->wrong_chunk_type) {
		remove_cache_extent(extent_cache, &rec->cache);
		free_all_extent_backrefs(rec);
		list_del_init(&rec->list);
		free(rec);
	}
	return 0;
}

static int check_owner_ref(struct btrfs_root *root,
			    struct extent_record *rec,
			    struct extent_buffer *buf)
{
	struct extent_backref *node;
	struct tree_backref *back;
	struct btrfs_root *ref_root;
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *parent;
	int level;
	int found = 0;
	int ret;

	list_for_each_entry(node, &rec->backrefs, list) {
		if (node->is_data)
			continue;
		if (!node->found_ref)
			continue;
		if (node->full_backref)
			continue;
		back = (struct tree_backref *)node;
		if (btrfs_header_owner(buf) == back->root)
			return 0;
	}
	BUG_ON(rec->is_root);

	/* try to find the block by search corresponding fs tree */
	key.objectid = btrfs_header_owner(buf);
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	ref_root = btrfs_read_fs_root(root->fs_info, &key);
	if (IS_ERR(ref_root))
		return 1;

	level = btrfs_header_level(buf);
	if (level == 0)
		btrfs_item_key_to_cpu(buf, &key, 0);
	else
		btrfs_node_key_to_cpu(buf, &key, 0);

	btrfs_init_path(&path);
	path.lowest_level = level + 1;
	ret = btrfs_search_slot(NULL, ref_root, &key, &path, 0, 0);
	if (ret < 0)
		return 0;

	parent = path.nodes[level + 1];
	if (parent && buf->start == btrfs_node_blockptr(parent,
							path.slots[level + 1]))
		found = 1;

	btrfs_release_path(&path);
	return found ? 0 : 1;
}

static int is_extent_tree_record(struct extent_record *rec)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *node;
	struct tree_backref *back;
	int is_extent = 0;

	while(cur != &rec->backrefs) {
		node = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (node->is_data)
			return 0;
		back = (struct tree_backref *)node;
		if (node->full_backref)
			return 0;
		if (back->root == BTRFS_EXTENT_TREE_OBJECTID)
			is_extent = 1;
	}
	return is_extent;
}


static int record_bad_block_io(struct btrfs_fs_info *info,
			       struct cache_tree *extent_cache,
			       u64 start, u64 len)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	struct btrfs_key key;

	cache = lookup_cache_extent(extent_cache, start, len);
	if (!cache)
		return 0;

	rec = container_of(cache, struct extent_record, cache);
	if (!is_extent_tree_record(rec))
		return 0;

	btrfs_disk_key_to_cpu(&key, &rec->parent_key);
	return btrfs_add_corrupt_extent_record(info, &key, start, len, 0);
}

static int swap_values(struct btrfs_root *root, struct btrfs_path *path,
		       struct extent_buffer *buf, int slot)
{
	if (btrfs_header_level(buf)) {
		struct btrfs_key_ptr ptr1, ptr2;

		read_extent_buffer(buf, &ptr1, btrfs_node_key_ptr_offset(slot),
				   sizeof(struct btrfs_key_ptr));
		read_extent_buffer(buf, &ptr2,
				   btrfs_node_key_ptr_offset(slot + 1),
				   sizeof(struct btrfs_key_ptr));
		write_extent_buffer(buf, &ptr1,
				    btrfs_node_key_ptr_offset(slot + 1),
				    sizeof(struct btrfs_key_ptr));
		write_extent_buffer(buf, &ptr2,
				    btrfs_node_key_ptr_offset(slot),
				    sizeof(struct btrfs_key_ptr));
		if (slot == 0) {
			struct btrfs_disk_key key;
			btrfs_node_key(buf, &key, 0);
			btrfs_fixup_low_keys(root, path, &key,
					     btrfs_header_level(buf) + 1);
		}
	} else {
		struct btrfs_item *item1, *item2;
		struct btrfs_key k1, k2;
		char *item1_data, *item2_data;
		u32 item1_offset, item2_offset, item1_size, item2_size;

		item1 = btrfs_item_nr(slot);
		item2 = btrfs_item_nr(slot + 1);
		btrfs_item_key_to_cpu(buf, &k1, slot);
		btrfs_item_key_to_cpu(buf, &k2, slot + 1);
		item1_offset = btrfs_item_offset(buf, item1);
		item2_offset = btrfs_item_offset(buf, item2);
		item1_size = btrfs_item_size(buf, item1);
		item2_size = btrfs_item_size(buf, item2);

		item1_data = malloc(item1_size);
		if (!item1_data)
			return -ENOMEM;
		item2_data = malloc(item2_size);
		if (!item2_data) {
			free(item1_data);
			return -ENOMEM;
		}

		read_extent_buffer(buf, item1_data, item1_offset, item1_size);
		read_extent_buffer(buf, item2_data, item2_offset, item2_size);

		write_extent_buffer(buf, item1_data, item2_offset, item2_size);
		write_extent_buffer(buf, item2_data, item1_offset, item1_size);
		free(item1_data);
		free(item2_data);

		btrfs_set_item_offset(buf, item1, item2_offset);
		btrfs_set_item_offset(buf, item2, item1_offset);
		btrfs_set_item_size(buf, item1, item2_size);
		btrfs_set_item_size(buf, item2, item1_size);

		path->slots[0] = slot;
		btrfs_set_item_key_unsafe(root, path, &k2);
		path->slots[0] = slot + 1;
		btrfs_set_item_key_unsafe(root, path, &k1);
	}
	return 0;
}

static int fix_key_order(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 struct btrfs_path *path)
{
	struct extent_buffer *buf;
	struct btrfs_key k1, k2;
	int i;
	int level = path->lowest_level;
	int ret = -EIO;

	buf = path->nodes[level];
	for (i = 0; i < btrfs_header_nritems(buf) - 1; i++) {
		if (level) {
			btrfs_node_key_to_cpu(buf, &k1, i);
			btrfs_node_key_to_cpu(buf, &k2, i + 1);
		} else {
			btrfs_item_key_to_cpu(buf, &k1, i);
			btrfs_item_key_to_cpu(buf, &k2, i + 1);
		}
		if (btrfs_comp_cpu_keys(&k1, &k2) < 0)
			continue;
		ret = swap_values(root, path, buf, i);
		if (ret)
			break;
		btrfs_mark_buffer_dirty(buf);
		i = 0;
	}
	return ret;
}

static int delete_bogus_item(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     struct btrfs_path *path,
			     struct extent_buffer *buf, int slot)
{
	struct btrfs_key key;
	int nritems = btrfs_header_nritems(buf);

	btrfs_item_key_to_cpu(buf, &key, slot);

	/* These are all the keys we can deal with missing. */
	if (key.type != BTRFS_DIR_INDEX_KEY &&
	    key.type != BTRFS_EXTENT_ITEM_KEY &&
	    key.type != BTRFS_METADATA_ITEM_KEY &&
	    key.type != BTRFS_TREE_BLOCK_REF_KEY &&
	    key.type != BTRFS_EXTENT_DATA_REF_KEY)
		return -1;

	printf("Deleting bogus item [%llu,%u,%llu] at slot %d on block %llu\n",
	       (unsigned long long)key.objectid, key.type,
	       (unsigned long long)key.offset, slot, buf->start);
	memmove_extent_buffer(buf, btrfs_item_nr_offset(slot),
			      btrfs_item_nr_offset(slot + 1),
			      sizeof(struct btrfs_item) *
			      (nritems - slot - 1));
	btrfs_set_header_nritems(buf, nritems - 1);
	if (slot == 0) {
		struct btrfs_disk_key disk_key;

		btrfs_item_key(buf, &disk_key, 0);
		btrfs_fixup_low_keys(root, path, &disk_key, 1);
	}
	btrfs_mark_buffer_dirty(buf);
	return 0;
}

static int fix_item_offset(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct btrfs_path *path)
{
	struct extent_buffer *buf;
	int i;
	int ret = 0;

	/* We should only get this for leaves */
	BUG_ON(path->lowest_level);
	buf = path->nodes[0];
again:
	for (i = 0; i < btrfs_header_nritems(buf); i++) {
		unsigned int shift = 0, offset;

		if (i == 0 && btrfs_item_end_nr(buf, i) !=
		    BTRFS_LEAF_DATA_SIZE(root)) {
			if (btrfs_item_end_nr(buf, i) >
			    BTRFS_LEAF_DATA_SIZE(root)) {
				ret = delete_bogus_item(trans, root, path,
							buf, i);
				if (!ret)
					goto again;
				fprintf(stderr, "item is off the end of the "
					"leaf, can't fix\n");
				ret = -EIO;
				break;
			}
			shift = BTRFS_LEAF_DATA_SIZE(root) -
				btrfs_item_end_nr(buf, i);
		} else if (i > 0 && btrfs_item_end_nr(buf, i) !=
			   btrfs_item_offset_nr(buf, i - 1)) {
			if (btrfs_item_end_nr(buf, i) >
			    btrfs_item_offset_nr(buf, i - 1)) {
				ret = delete_bogus_item(trans, root, path,
							buf, i);
				if (!ret)
					goto again;
				fprintf(stderr, "items overlap, can't fix\n");
				ret = -EIO;
				break;
			}
			shift = btrfs_item_offset_nr(buf, i - 1) -
				btrfs_item_end_nr(buf, i);
		}
		if (!shift)
			continue;

		printf("Shifting item nr %d by %u bytes in block %llu\n",
		       i, shift, (unsigned long long)buf->start);
		offset = btrfs_item_offset_nr(buf, i);
		memmove_extent_buffer(buf,
				      btrfs_leaf_data(buf) + offset + shift,
				      btrfs_leaf_data(buf) + offset,
				      btrfs_item_size_nr(buf, i));
		btrfs_set_item_offset(buf, btrfs_item_nr(i),
				      offset + shift);
		btrfs_mark_buffer_dirty(buf);
	}

	/*
	 * We may have moved things, in which case we want to exit so we don't
	 * write those changes out.  Once we have proper abort functionality in
	 * progs this can be changed to something nicer.
	 */
	BUG_ON(ret);
	return ret;
}

/*
 * Attempt to fix basic block failures.  If we can't fix it for whatever reason
 * then just return -EIO.
 */
static int try_to_fix_bad_block(struct btrfs_root *root,
				struct extent_buffer *buf,
				enum btrfs_tree_block_status status)
{
	struct btrfs_trans_handle *trans;
	struct ulist *roots;
	struct ulist_node *node;
	struct btrfs_root *search_root;
	struct btrfs_path *path;
	struct ulist_iterator iter;
	struct btrfs_key root_key, key;
	int ret;

	if (status != BTRFS_TREE_BLOCK_BAD_KEY_ORDER &&
	    status != BTRFS_TREE_BLOCK_INVALID_OFFSETS)
		return -EIO;

	path = btrfs_alloc_path();
	if (!path)
		return -EIO;

	ret = btrfs_find_all_roots(NULL, root->fs_info, buf->start,
				   0, &roots);
	if (ret) {
		btrfs_free_path(path);
		return -EIO;
	}

	ULIST_ITER_INIT(&iter);
	while ((node = ulist_next(roots, &iter))) {
		root_key.objectid = node->val;
		root_key.type = BTRFS_ROOT_ITEM_KEY;
		root_key.offset = (u64)-1;

		search_root = btrfs_read_fs_root(root->fs_info, &root_key);
		if (IS_ERR(root)) {
			ret = -EIO;
			break;
		}


		trans = btrfs_start_transaction(search_root, 0);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			break;
		}

		path->lowest_level = btrfs_header_level(buf);
		path->skip_check_block = 1;
		if (path->lowest_level)
			btrfs_node_key_to_cpu(buf, &key, 0);
		else
			btrfs_item_key_to_cpu(buf, &key, 0);
		ret = btrfs_search_slot(trans, search_root, &key, path, 0, 1);
		if (ret) {
			ret = -EIO;
			btrfs_commit_transaction(trans, search_root);
			break;
		}
		if (status == BTRFS_TREE_BLOCK_BAD_KEY_ORDER)
			ret = fix_key_order(trans, search_root, path);
		else if (status == BTRFS_TREE_BLOCK_INVALID_OFFSETS)
			ret = fix_item_offset(trans, search_root, path);
		if (ret) {
			btrfs_commit_transaction(trans, search_root);
			break;
		}
		btrfs_release_path(path);
		btrfs_commit_transaction(trans, search_root);
	}
	ulist_free(roots);
	btrfs_free_path(path);
	return ret;
}

static int check_block(struct btrfs_root *root,
		       struct cache_tree *extent_cache,
		       struct extent_buffer *buf, u64 flags)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	struct btrfs_key key;
	enum btrfs_tree_block_status status;
	int ret = 0;
	int level;

	cache = lookup_cache_extent(extent_cache, buf->start, buf->len);
	if (!cache)
		return 1;
	rec = container_of(cache, struct extent_record, cache);
	rec->generation = btrfs_header_generation(buf);

	level = btrfs_header_level(buf);
	if (btrfs_header_nritems(buf) > 0) {

		if (level == 0)
			btrfs_item_key_to_cpu(buf, &key, 0);
		else
			btrfs_node_key_to_cpu(buf, &key, 0);

		rec->info_objectid = key.objectid;
	}
	rec->info_level = level;

	if (btrfs_is_leaf(buf))
		status = btrfs_check_leaf(root, &rec->parent_key, buf);
	else
		status = btrfs_check_node(root, &rec->parent_key, buf);

	if (status != BTRFS_TREE_BLOCK_CLEAN) {
		if (repair)
			status = try_to_fix_bad_block(root, buf, status);
		if (status != BTRFS_TREE_BLOCK_CLEAN) {
			ret = -EIO;
			fprintf(stderr, "bad block %llu\n",
				(unsigned long long)buf->start);
		} else {
			/*
			 * Signal to callers we need to start the scan over
			 * again since we'll have cow'ed blocks.
			 */
			ret = -EAGAIN;
		}
	} else {
		rec->content_checked = 1;
		if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)
			rec->owner_ref_checked = 1;
		else {
			ret = check_owner_ref(root, rec, buf);
			if (!ret)
				rec->owner_ref_checked = 1;
		}
	}
	if (!ret)
		maybe_free_extent_rec(extent_cache, rec);
	return ret;
}

static struct tree_backref *find_tree_backref(struct extent_record *rec,
						u64 parent, u64 root)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *node;
	struct tree_backref *back;

	while(cur != &rec->backrefs) {
		node = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (node->is_data)
			continue;
		back = (struct tree_backref *)node;
		if (parent > 0) {
			if (!node->full_backref)
				continue;
			if (parent == back->parent)
				return back;
		} else {
			if (node->full_backref)
				continue;
			if (back->root == root)
				return back;
		}
	}
	return NULL;
}

static struct tree_backref *alloc_tree_backref(struct extent_record *rec,
						u64 parent, u64 root)
{
	struct tree_backref *ref = malloc(sizeof(*ref));
	memset(&ref->node, 0, sizeof(ref->node));
	if (parent > 0) {
		ref->parent = parent;
		ref->node.full_backref = 1;
	} else {
		ref->root = root;
		ref->node.full_backref = 0;
	}
	list_add_tail(&ref->node.list, &rec->backrefs);

	return ref;
}

static struct data_backref *find_data_backref(struct extent_record *rec,
						u64 parent, u64 root,
						u64 owner, u64 offset,
						int found_ref,
						u64 disk_bytenr, u64 bytes)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *node;
	struct data_backref *back;

	while(cur != &rec->backrefs) {
		node = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (!node->is_data)
			continue;
		back = (struct data_backref *)node;
		if (parent > 0) {
			if (!node->full_backref)
				continue;
			if (parent == back->parent)
				return back;
		} else {
			if (node->full_backref)
				continue;
			if (back->root == root && back->owner == owner &&
			    back->offset == offset) {
				if (found_ref && node->found_ref &&
				    (back->bytes != bytes ||
				    back->disk_bytenr != disk_bytenr))
					continue;
				return back;
			}
		}
	}
	return NULL;
}

static struct data_backref *alloc_data_backref(struct extent_record *rec,
						u64 parent, u64 root,
						u64 owner, u64 offset,
						u64 max_size)
{
	struct data_backref *ref = malloc(sizeof(*ref));
	memset(&ref->node, 0, sizeof(ref->node));
	ref->node.is_data = 1;

	if (parent > 0) {
		ref->parent = parent;
		ref->owner = 0;
		ref->offset = 0;
		ref->node.full_backref = 1;
	} else {
		ref->root = root;
		ref->owner = owner;
		ref->offset = offset;
		ref->node.full_backref = 0;
	}
	ref->bytes = max_size;
	ref->found_ref = 0;
	ref->num_refs = 0;
	list_add_tail(&ref->node.list, &rec->backrefs);
	if (max_size > rec->max_size)
		rec->max_size = max_size;
	return ref;
}

/* Check if the type of extent matches with its chunk */
static void check_extent_type(struct extent_record *rec)
{
	struct btrfs_block_group_cache *bg_cache;

	bg_cache = btrfs_lookup_first_block_group(global_info, rec->start);
	if (!bg_cache)
		return;

	/* data extent, check chunk directly*/
	if (!rec->metadata) {
		if (!(bg_cache->flags & BTRFS_BLOCK_GROUP_DATA))
			rec->wrong_chunk_type = 1;
		return;
	}

	/* metadata extent, check the obvious case first */
	if (!(bg_cache->flags & (BTRFS_BLOCK_GROUP_SYSTEM |
				 BTRFS_BLOCK_GROUP_METADATA))) {
		rec->wrong_chunk_type = 1;
		return;
	}

	/*
	 * Check SYSTEM extent, as it's also marked as metadata, we can only
	 * make sure it's a SYSTEM extent by its backref
	 */
	if (!list_empty(&rec->backrefs)) {
		struct extent_backref *node;
		struct tree_backref *tback;
		u64 bg_type;

		node = list_entry(rec->backrefs.next, struct extent_backref,
				  list);
		if (node->is_data) {
			/* tree block shouldn't have data backref */
			rec->wrong_chunk_type = 1;
			return;
		}
		tback = container_of(node, struct tree_backref, node);

		if (tback->root == BTRFS_CHUNK_TREE_OBJECTID)
			bg_type = BTRFS_BLOCK_GROUP_SYSTEM;
		else
			bg_type = BTRFS_BLOCK_GROUP_METADATA;
		if (!(bg_cache->flags & bg_type))
			rec->wrong_chunk_type = 1;
	}
}

static int add_extent_rec(struct cache_tree *extent_cache,
			  struct btrfs_key *parent_key, u64 parent_gen,
			  u64 start, u64 nr, u64 extent_item_refs,
			  int is_root, int inc_ref, int set_checked,
			  int metadata, int extent_rec, u64 max_size)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int ret = 0;
	int dup = 0;

	cache = lookup_cache_extent(extent_cache, start, nr);
	if (cache) {
		rec = container_of(cache, struct extent_record, cache);
		if (inc_ref)
			rec->refs++;
		if (rec->nr == 1)
			rec->nr = max(nr, max_size);

		/*
		 * We need to make sure to reset nr to whatever the extent
		 * record says was the real size, this way we can compare it to
		 * the backrefs.
		 */
		if (extent_rec) {
			if (start != rec->start || rec->found_rec) {
				struct extent_record *tmp;

				dup = 1;
				if (list_empty(&rec->list))
					list_add_tail(&rec->list,
						      &duplicate_extents);

				/*
				 * We have to do this song and dance in case we
				 * find an extent record that falls inside of
				 * our current extent record but does not have
				 * the same objectid.
				 */
				tmp = malloc(sizeof(*tmp));
				if (!tmp)
					return -ENOMEM;
				tmp->start = start;
				tmp->max_size = max_size;
				tmp->nr = nr;
				tmp->found_rec = 1;
				tmp->metadata = metadata;
				tmp->extent_item_refs = extent_item_refs;
				INIT_LIST_HEAD(&tmp->list);
				list_add_tail(&tmp->list, &rec->dups);
				rec->num_duplicates++;
			} else {
				rec->nr = nr;
				rec->found_rec = 1;
			}
		}

		if (extent_item_refs && !dup) {
			if (rec->extent_item_refs) {
				fprintf(stderr, "block %llu rec "
					"extent_item_refs %llu, passed %llu\n",
					(unsigned long long)start,
					(unsigned long long)
							rec->extent_item_refs,
					(unsigned long long)extent_item_refs);
			}
			rec->extent_item_refs = extent_item_refs;
		}
		if (is_root)
			rec->is_root = 1;
		if (set_checked) {
			rec->content_checked = 1;
			rec->owner_ref_checked = 1;
		}

		if (parent_key)
			btrfs_cpu_key_to_disk(&rec->parent_key, parent_key);
		if (parent_gen)
			rec->parent_generation = parent_gen;

		if (rec->max_size < max_size)
			rec->max_size = max_size;

		/*
		 * A metadata extent can't cross stripe_len boundary, otherwise
		 * kernel scrub won't be able to handle it.
		 * As now stripe_len is fixed to BTRFS_STRIPE_LEN, just check
		 * it.
		 */
		if (metadata && check_crossing_stripes(rec->start,
						       rec->max_size))
				rec->crossing_stripes = 1;
		check_extent_type(rec);
		maybe_free_extent_rec(extent_cache, rec);
		return ret;
	}
	rec = malloc(sizeof(*rec));
	rec->start = start;
	rec->max_size = max_size;
	rec->nr = max(nr, max_size);
	rec->found_rec = !!extent_rec;
	rec->content_checked = 0;
	rec->owner_ref_checked = 0;
	rec->num_duplicates = 0;
	rec->metadata = metadata;
	rec->flag_block_full_backref = -1;
	rec->bad_full_backref = 0;
	rec->crossing_stripes = 0;
	rec->wrong_chunk_type = 0;
	INIT_LIST_HEAD(&rec->backrefs);
	INIT_LIST_HEAD(&rec->dups);
	INIT_LIST_HEAD(&rec->list);

	if (is_root)
		rec->is_root = 1;
	else
		rec->is_root = 0;

	if (inc_ref)
		rec->refs = 1;
	else
		rec->refs = 0;

	if (extent_item_refs)
		rec->extent_item_refs = extent_item_refs;
	else
		rec->extent_item_refs = 0;

	if (parent_key)
		btrfs_cpu_key_to_disk(&rec->parent_key, parent_key);
	else
		memset(&rec->parent_key, 0, sizeof(*parent_key));

	if (parent_gen)
		rec->parent_generation = parent_gen;
	else
		rec->parent_generation = 0;

	rec->cache.start = start;
	rec->cache.size = nr;
	ret = insert_cache_extent(extent_cache, &rec->cache);
	BUG_ON(ret);
	bytes_used += nr;
	if (set_checked) {
		rec->content_checked = 1;
		rec->owner_ref_checked = 1;
	}

	if (metadata)
		if (check_crossing_stripes(rec->start, rec->max_size))
			rec->crossing_stripes = 1;
	check_extent_type(rec);
	return ret;
}

static int add_tree_backref(struct cache_tree *extent_cache, u64 bytenr,
			    u64 parent, u64 root, int found_ref)
{
	struct extent_record *rec;
	struct tree_backref *back;
	struct cache_extent *cache;

	cache = lookup_cache_extent(extent_cache, bytenr, 1);
	if (!cache) {
		add_extent_rec(extent_cache, NULL, 0, bytenr,
			       1, 0, 0, 0, 0, 1, 0, 0);
		cache = lookup_cache_extent(extent_cache, bytenr, 1);
		if (!cache)
			abort();
	}

	rec = container_of(cache, struct extent_record, cache);
	if (rec->start != bytenr) {
		abort();
	}

	back = find_tree_backref(rec, parent, root);
	if (!back)
		back = alloc_tree_backref(rec, parent, root);

	if (found_ref) {
		if (back->node.found_ref) {
			fprintf(stderr, "Extent back ref already exists "
				"for %llu parent %llu root %llu \n",
				(unsigned long long)bytenr,
				(unsigned long long)parent,
				(unsigned long long)root);
		}
		back->node.found_ref = 1;
	} else {
		if (back->node.found_extent_tree) {
			fprintf(stderr, "Extent back ref already exists "
				"for %llu parent %llu root %llu \n",
				(unsigned long long)bytenr,
				(unsigned long long)parent,
				(unsigned long long)root);
		}
		back->node.found_extent_tree = 1;
	}
	check_extent_type(rec);
	maybe_free_extent_rec(extent_cache, rec);
	return 0;
}

static int add_data_backref(struct cache_tree *extent_cache, u64 bytenr,
			    u64 parent, u64 root, u64 owner, u64 offset,
			    u32 num_refs, int found_ref, u64 max_size)
{
	struct extent_record *rec;
	struct data_backref *back;
	struct cache_extent *cache;

	cache = lookup_cache_extent(extent_cache, bytenr, 1);
	if (!cache) {
		add_extent_rec(extent_cache, NULL, 0, bytenr, 1, 0, 0, 0, 0,
			       0, 0, max_size);
		cache = lookup_cache_extent(extent_cache, bytenr, 1);
		if (!cache)
			abort();
	}

	rec = container_of(cache, struct extent_record, cache);
	if (rec->max_size < max_size)
		rec->max_size = max_size;

	/*
	 * If found_ref is set then max_size is the real size and must match the
	 * existing refs.  So if we have already found a ref then we need to
	 * make sure that this ref matches the existing one, otherwise we need
	 * to add a new backref so we can notice that the backrefs don't match
	 * and we need to figure out who is telling the truth.  This is to
	 * account for that awful fsync bug I introduced where we'd end up with
	 * a btrfs_file_extent_item that would have its length include multiple
	 * prealloc extents or point inside of a prealloc extent.
	 */
	back = find_data_backref(rec, parent, root, owner, offset, found_ref,
				 bytenr, max_size);
	if (!back)
		back = alloc_data_backref(rec, parent, root, owner, offset,
					  max_size);

	if (found_ref) {
		BUG_ON(num_refs != 1);
		if (back->node.found_ref)
			BUG_ON(back->bytes != max_size);
		back->node.found_ref = 1;
		back->found_ref += 1;
		back->bytes = max_size;
		back->disk_bytenr = bytenr;
		rec->refs += 1;
		rec->content_checked = 1;
		rec->owner_ref_checked = 1;
	} else {
		if (back->node.found_extent_tree) {
			fprintf(stderr, "Extent back ref already exists "
				"for %llu parent %llu root %llu "
				"owner %llu offset %llu num_refs %lu\n",
				(unsigned long long)bytenr,
				(unsigned long long)parent,
				(unsigned long long)root,
				(unsigned long long)owner,
				(unsigned long long)offset,
				(unsigned long)num_refs);
		}
		back->num_refs = num_refs;
		back->node.found_extent_tree = 1;
	}
	maybe_free_extent_rec(extent_cache, rec);
	return 0;
}

static int add_pending(struct cache_tree *pending,
		       struct cache_tree *seen, u64 bytenr, u32 size)
{
	int ret;
	ret = add_cache_extent(seen, bytenr, size);
	if (ret)
		return ret;
	add_cache_extent(pending, bytenr, size);
	return 0;
}

static int pick_next_pending(struct cache_tree *pending,
			struct cache_tree *reada,
			struct cache_tree *nodes,
			u64 last, struct block_info *bits, int bits_nr,
			int *reada_bits)
{
	unsigned long node_start = last;
	struct cache_extent *cache;
	int ret;

	cache = search_cache_extent(reada, 0);
	if (cache) {
		bits[0].start = cache->start;
		bits[0].size = cache->size;
		*reada_bits = 1;
		return 1;
	}
	*reada_bits = 0;
	if (node_start > 32768)
		node_start -= 32768;

	cache = search_cache_extent(nodes, node_start);
	if (!cache)
		cache = search_cache_extent(nodes, 0);

	if (!cache) {
		 cache = search_cache_extent(pending, 0);
		 if (!cache)
			 return 0;
		 ret = 0;
		 do {
			 bits[ret].start = cache->start;
			 bits[ret].size = cache->size;
			 cache = next_cache_extent(cache);
			 ret++;
		 } while (cache && ret < bits_nr);
		 return ret;
	}

	ret = 0;
	do {
		bits[ret].start = cache->start;
		bits[ret].size = cache->size;
		cache = next_cache_extent(cache);
		ret++;
	} while (cache && ret < bits_nr);

	if (bits_nr - ret > 8) {
		u64 lookup = bits[0].start + bits[0].size;
		struct cache_extent *next;
		next = search_cache_extent(pending, lookup);
		while(next) {
			if (next->start - lookup > 32768)
				break;
			bits[ret].start = next->start;
			bits[ret].size = next->size;
			lookup = next->start + next->size;
			ret++;
			if (ret == bits_nr)
				break;
			next = next_cache_extent(next);
			if (!next)
				break;
		}
	}
	return ret;
}

static void free_chunk_record(struct cache_extent *cache)
{
	struct chunk_record *rec;

	rec = container_of(cache, struct chunk_record, cache);
	list_del_init(&rec->list);
	list_del_init(&rec->dextents);
	free(rec);
}

void free_chunk_cache_tree(struct cache_tree *chunk_cache)
{
	cache_tree_free_extents(chunk_cache, free_chunk_record);
}

static void free_device_record(struct rb_node *node)
{
	struct device_record *rec;

	rec = container_of(node, struct device_record, node);
	free(rec);
}

FREE_RB_BASED_TREE(device_cache, free_device_record);

int insert_block_group_record(struct block_group_tree *tree,
			      struct block_group_record *bg_rec)
{
	int ret;

	ret = insert_cache_extent(&tree->tree, &bg_rec->cache);
	if (ret)
		return ret;

	list_add_tail(&bg_rec->list, &tree->block_groups);
	return 0;
}

static void free_block_group_record(struct cache_extent *cache)
{
	struct block_group_record *rec;

	rec = container_of(cache, struct block_group_record, cache);
	list_del_init(&rec->list);
	free(rec);
}

void free_block_group_tree(struct block_group_tree *tree)
{
	cache_tree_free_extents(&tree->tree, free_block_group_record);
}

int insert_device_extent_record(struct device_extent_tree *tree,
				struct device_extent_record *de_rec)
{
	int ret;

	/*
	 * Device extent is a bit different from the other extents, because
	 * the extents which belong to the different devices may have the
	 * same start and size, so we need use the special extent cache
	 * search/insert functions.
	 */
	ret = insert_cache_extent2(&tree->tree, &de_rec->cache);
	if (ret)
		return ret;

	list_add_tail(&de_rec->chunk_list, &tree->no_chunk_orphans);
	list_add_tail(&de_rec->device_list, &tree->no_device_orphans);
	return 0;
}

static void free_device_extent_record(struct cache_extent *cache)
{
	struct device_extent_record *rec;

	rec = container_of(cache, struct device_extent_record, cache);
	if (!list_empty(&rec->chunk_list))
		list_del_init(&rec->chunk_list);
	if (!list_empty(&rec->device_list))
		list_del_init(&rec->device_list);
	free(rec);
}

void free_device_extent_tree(struct device_extent_tree *tree)
{
	cache_tree_free_extents(&tree->tree, free_device_extent_record);
}

#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
static int process_extent_ref_v0(struct cache_tree *extent_cache,
				 struct extent_buffer *leaf, int slot)
{
	struct btrfs_extent_ref_v0 *ref0;
	struct btrfs_key key;

	btrfs_item_key_to_cpu(leaf, &key, slot);
	ref0 = btrfs_item_ptr(leaf, slot, struct btrfs_extent_ref_v0);
	if (btrfs_ref_objectid_v0(leaf, ref0) < BTRFS_FIRST_FREE_OBJECTID) {
		add_tree_backref(extent_cache, key.objectid, key.offset, 0, 0);
	} else {
		add_data_backref(extent_cache, key.objectid, key.offset, 0,
				 0, 0, btrfs_ref_count_v0(leaf, ref0), 0, 0);
	}
	return 0;
}
#endif

struct chunk_record *btrfs_new_chunk_record(struct extent_buffer *leaf,
					    struct btrfs_key *key,
					    int slot)
{
	struct btrfs_chunk *ptr;
	struct chunk_record *rec;
	int num_stripes, i;

	ptr = btrfs_item_ptr(leaf, slot, struct btrfs_chunk);
	num_stripes = btrfs_chunk_num_stripes(leaf, ptr);

	rec = calloc(1, btrfs_chunk_record_size(num_stripes));
	if (!rec) {
		fprintf(stderr, "memory allocation failed\n");
		exit(-1);
	}

	INIT_LIST_HEAD(&rec->list);
	INIT_LIST_HEAD(&rec->dextents);
	rec->bg_rec = NULL;

	rec->cache.start = key->offset;
	rec->cache.size = btrfs_chunk_length(leaf, ptr);

	rec->generation = btrfs_header_generation(leaf);

	rec->objectid = key->objectid;
	rec->type = key->type;
	rec->offset = key->offset;

	rec->length = rec->cache.size;
	rec->owner = btrfs_chunk_owner(leaf, ptr);
	rec->stripe_len = btrfs_chunk_stripe_len(leaf, ptr);
	rec->type_flags = btrfs_chunk_type(leaf, ptr);
	rec->io_width = btrfs_chunk_io_width(leaf, ptr);
	rec->io_align = btrfs_chunk_io_align(leaf, ptr);
	rec->sector_size = btrfs_chunk_sector_size(leaf, ptr);
	rec->num_stripes = num_stripes;
	rec->sub_stripes = btrfs_chunk_sub_stripes(leaf, ptr);

	for (i = 0; i < rec->num_stripes; ++i) {
		rec->stripes[i].devid =
			btrfs_stripe_devid_nr(leaf, ptr, i);
		rec->stripes[i].offset =
			btrfs_stripe_offset_nr(leaf, ptr, i);
		read_extent_buffer(leaf, rec->stripes[i].dev_uuid,
				(unsigned long)btrfs_stripe_dev_uuid_nr(ptr, i),
				BTRFS_UUID_SIZE);
	}

	return rec;
}

static int process_chunk_item(struct cache_tree *chunk_cache,
			      struct btrfs_key *key, struct extent_buffer *eb,
			      int slot)
{
	struct chunk_record *rec;
	int ret = 0;

	rec = btrfs_new_chunk_record(eb, key, slot);
	ret = insert_cache_extent(chunk_cache, &rec->cache);
	if (ret) {
		fprintf(stderr, "Chunk[%llu, %llu] existed.\n",
			rec->offset, rec->length);
		free(rec);
	}

	return ret;
}

static int process_device_item(struct rb_root *dev_cache,
		struct btrfs_key *key, struct extent_buffer *eb, int slot)
{
	struct btrfs_dev_item *ptr;
	struct device_record *rec;
	int ret = 0;

	ptr = btrfs_item_ptr(eb,
		slot, struct btrfs_dev_item);

	rec = malloc(sizeof(*rec));
	if (!rec) {
		fprintf(stderr, "memory allocation failed\n");
		return -ENOMEM;
	}

	rec->devid = key->offset;
	rec->generation = btrfs_header_generation(eb);

	rec->objectid = key->objectid;
	rec->type = key->type;
	rec->offset = key->offset;

	rec->devid = btrfs_device_id(eb, ptr);
	rec->total_byte = btrfs_device_total_bytes(eb, ptr);
	rec->byte_used = btrfs_device_bytes_used(eb, ptr);

	ret = rb_insert(dev_cache, &rec->node, device_record_compare);
	if (ret) {
		fprintf(stderr, "Device[%llu] existed.\n", rec->devid);
		free(rec);
	}

	return ret;
}

struct block_group_record *
btrfs_new_block_group_record(struct extent_buffer *leaf, struct btrfs_key *key,
			     int slot)
{
	struct btrfs_block_group_item *ptr;
	struct block_group_record *rec;

	rec = calloc(1, sizeof(*rec));
	if (!rec) {
		fprintf(stderr, "memory allocation failed\n");
		exit(-1);
	}

	rec->cache.start = key->objectid;
	rec->cache.size = key->offset;

	rec->generation = btrfs_header_generation(leaf);

	rec->objectid = key->objectid;
	rec->type = key->type;
	rec->offset = key->offset;

	ptr = btrfs_item_ptr(leaf, slot, struct btrfs_block_group_item);
	rec->flags = btrfs_disk_block_group_flags(leaf, ptr);

	INIT_LIST_HEAD(&rec->list);

	return rec;
}

static int process_block_group_item(struct block_group_tree *block_group_cache,
				    struct btrfs_key *key,
				    struct extent_buffer *eb, int slot)
{
	struct block_group_record *rec;
	int ret = 0;

	rec = btrfs_new_block_group_record(eb, key, slot);
	ret = insert_block_group_record(block_group_cache, rec);
	if (ret) {
		fprintf(stderr, "Block Group[%llu, %llu] existed.\n",
			rec->objectid, rec->offset);
		free(rec);
	}

	return ret;
}

struct device_extent_record *
btrfs_new_device_extent_record(struct extent_buffer *leaf,
			       struct btrfs_key *key, int slot)
{
	struct device_extent_record *rec;
	struct btrfs_dev_extent *ptr;

	rec = calloc(1, sizeof(*rec));
	if (!rec) {
		fprintf(stderr, "memory allocation failed\n");
		exit(-1);
	}

	rec->cache.objectid = key->objectid;
	rec->cache.start = key->offset;

	rec->generation = btrfs_header_generation(leaf);

	rec->objectid = key->objectid;
	rec->type = key->type;
	rec->offset = key->offset;

	ptr = btrfs_item_ptr(leaf, slot, struct btrfs_dev_extent);
	rec->chunk_objecteid =
		btrfs_dev_extent_chunk_objectid(leaf, ptr);
	rec->chunk_offset =
		btrfs_dev_extent_chunk_offset(leaf, ptr);
	rec->length = btrfs_dev_extent_length(leaf, ptr);
	rec->cache.size = rec->length;

	INIT_LIST_HEAD(&rec->chunk_list);
	INIT_LIST_HEAD(&rec->device_list);

	return rec;
}

static int
process_device_extent_item(struct device_extent_tree *dev_extent_cache,
			   struct btrfs_key *key, struct extent_buffer *eb,
			   int slot)
{
	struct device_extent_record *rec;
	int ret;

	rec = btrfs_new_device_extent_record(eb, key, slot);
	ret = insert_device_extent_record(dev_extent_cache, rec);
	if (ret) {
		fprintf(stderr,
			"Device extent[%llu, %llu, %llu] existed.\n",
			rec->objectid, rec->offset, rec->length);
		free(rec);
	}

	return ret;
}

static int process_extent_item(struct btrfs_root *root,
			       struct cache_tree *extent_cache,
			       struct extent_buffer *eb, int slot)
{
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	struct btrfs_shared_data_ref *sref;
	struct btrfs_key key;
	unsigned long end;
	unsigned long ptr;
	int type;
	u32 item_size = btrfs_item_size_nr(eb, slot);
	u64 refs = 0;
	u64 offset;
	u64 num_bytes;
	int metadata = 0;

	btrfs_item_key_to_cpu(eb, &key, slot);

	if (key.type == BTRFS_METADATA_ITEM_KEY) {
		metadata = 1;
		num_bytes = root->leafsize;
	} else {
		num_bytes = key.offset;
	}

	if (item_size < sizeof(*ei)) {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
		struct btrfs_extent_item_v0 *ei0;
		BUG_ON(item_size != sizeof(*ei0));
		ei0 = btrfs_item_ptr(eb, slot, struct btrfs_extent_item_v0);
		refs = btrfs_extent_refs_v0(eb, ei0);
#else
		BUG();
#endif
		return add_extent_rec(extent_cache, NULL, 0, key.objectid,
				      num_bytes, refs, 0, 0, 0, metadata, 1,
				      num_bytes);
	}

	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
	refs = btrfs_extent_refs(eb, ei);

	add_extent_rec(extent_cache, NULL, 0, key.objectid, num_bytes,
		       refs, 0, 0, 0, metadata, 1, num_bytes);

	ptr = (unsigned long)(ei + 1);
	if (btrfs_extent_flags(eb, ei) & BTRFS_EXTENT_FLAG_TREE_BLOCK &&
	    key.type == BTRFS_EXTENT_ITEM_KEY)
		ptr += sizeof(struct btrfs_tree_block_info);

	end = (unsigned long)ei + item_size;
	while (ptr < end) {
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(eb, iref);
		offset = btrfs_extent_inline_ref_offset(eb, iref);
		switch (type) {
		case BTRFS_TREE_BLOCK_REF_KEY:
			add_tree_backref(extent_cache, key.objectid,
					 0, offset, 0);
			break;
		case BTRFS_SHARED_BLOCK_REF_KEY:
			add_tree_backref(extent_cache, key.objectid,
					 offset, 0, 0);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY:
			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			add_data_backref(extent_cache, key.objectid, 0,
					btrfs_extent_data_ref_root(eb, dref),
					btrfs_extent_data_ref_objectid(eb,
								       dref),
					btrfs_extent_data_ref_offset(eb, dref),
					btrfs_extent_data_ref_count(eb, dref),
					0, num_bytes);
			break;
		case BTRFS_SHARED_DATA_REF_KEY:
			sref = (struct btrfs_shared_data_ref *)(iref + 1);
			add_data_backref(extent_cache, key.objectid, offset,
					0, 0, 0,
					btrfs_shared_data_ref_count(eb, sref),
					0, num_bytes);
			break;
		default:
			fprintf(stderr, "corrupt extent record: key %Lu %u %Lu\n",
				key.objectid, key.type, num_bytes);
			goto out;
		}
		ptr += btrfs_extent_inline_ref_size(type);
	}
	WARN_ON(ptr > end);
out:
	return 0;
}

static int check_cache_range(struct btrfs_root *root,
			     struct btrfs_block_group_cache *cache,
			     u64 offset, u64 bytes)
{
	struct btrfs_free_space *entry;
	u64 *logical;
	u64 bytenr;
	int stripe_len;
	int i, nr, ret;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = btrfs_rmap_block(&root->fs_info->mapping_tree,
				       cache->key.objectid, bytenr, 0,
				       &logical, &nr, &stripe_len);
		if (ret)
			return ret;

		while (nr--) {
			if (logical[nr] + stripe_len <= offset)
				continue;
			if (offset + bytes <= logical[nr])
				continue;
			if (logical[nr] == offset) {
				if (stripe_len >= bytes) {
					kfree(logical);
					return 0;
				}
				bytes -= stripe_len;
				offset += stripe_len;
			} else if (logical[nr] < offset) {
				if (logical[nr] + stripe_len >=
				    offset + bytes) {
					kfree(logical);
					return 0;
				}
				bytes = (offset + bytes) -
					(logical[nr] + stripe_len);
				offset = logical[nr] + stripe_len;
			} else {
				/*
				 * Could be tricky, the super may land in the
				 * middle of the area we're checking.  First
				 * check the easiest case, it's at the end.
				 */
				if (logical[nr] + stripe_len >=
				    bytes + offset) {
					bytes = logical[nr] - offset;
					continue;
				}

				/* Check the left side */
				ret = check_cache_range(root, cache,
							offset,
							logical[nr] - offset);
				if (ret) {
					kfree(logical);
					return ret;
				}

				/* Now we continue with the right side */
				bytes = (offset + bytes) -
					(logical[nr] + stripe_len);
				offset = logical[nr] + stripe_len;
			}
		}

		kfree(logical);
	}

	entry = btrfs_find_free_space(cache->free_space_ctl, offset, bytes);
	if (!entry) {
		fprintf(stderr, "There is no free space entry for %Lu-%Lu\n",
			offset, offset+bytes);
		return -EINVAL;
	}

	if (entry->offset != offset) {
		fprintf(stderr, "Wanted offset %Lu, found %Lu\n", offset,
			entry->offset);
		return -EINVAL;
	}

	if (entry->bytes != bytes) {
		fprintf(stderr, "Wanted bytes %Lu, found %Lu for off %Lu\n",
			bytes, entry->bytes, offset);
		return -EINVAL;
	}

	unlink_free_space(cache->free_space_ctl, entry);
	free(entry);
	return 0;
}

static int verify_space_cache(struct btrfs_root *root,
			      struct btrfs_block_group_cache *cache)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 last;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	root = root->fs_info->extent_root;

	last = max_t(u64, cache->key.objectid, BTRFS_SUPER_INFO_OFFSET);

	key.objectid = last;
	key.offset = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	ret = 0;
	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0)
				goto out;
			if (ret > 0) {
				ret = 0;
				break;
			}
		}
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid >= cache->key.offset + cache->key.objectid)
			break;
		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY) {
			path->slots[0]++;
			continue;
		}

		if (last == key.objectid) {
			if (key.type == BTRFS_EXTENT_ITEM_KEY)
				last = key.objectid + key.offset;
			else
				last = key.objectid + root->leafsize;
			path->slots[0]++;
			continue;
		}

		ret = check_cache_range(root, cache, last,
					key.objectid - last);
		if (ret)
			break;
		if (key.type == BTRFS_EXTENT_ITEM_KEY)
			last = key.objectid + key.offset;
		else
			last = key.objectid + root->leafsize;
		path->slots[0]++;
	}

	if (last < cache->key.objectid + cache->key.offset)
		ret = check_cache_range(root, cache, last,
					cache->key.objectid +
					cache->key.offset - last);

out:
	btrfs_free_path(path);

	if (!ret &&
	    !RB_EMPTY_ROOT(&cache->free_space_ctl->free_space_offset)) {
		fprintf(stderr, "There are still entries left in the space "
			"cache\n");
		ret = -EINVAL;
	}

	return ret;
}

static int check_space_cache(struct btrfs_root *root)
{
	struct btrfs_block_group_cache *cache;
	u64 start = BTRFS_SUPER_INFO_OFFSET + BTRFS_SUPER_INFO_SIZE;
	int ret;
	int error = 0;

	if (btrfs_super_cache_generation(root->fs_info->super_copy) != -1ULL &&
	    btrfs_super_generation(root->fs_info->super_copy) !=
	    btrfs_super_cache_generation(root->fs_info->super_copy)) {
		printf("cache and super generation don't match, space cache "
		       "will be invalidated\n");
		return 0;
	}

	if (ctx.progress_enabled) {
		ctx.tp = TASK_FREE_SPACE;
		task_start(ctx.info);
	}

	while (1) {
		cache = btrfs_lookup_first_block_group(root->fs_info, start);
		if (!cache)
			break;

		start = cache->key.objectid + cache->key.offset;
		if (!cache->free_space_ctl) {
			if (btrfs_init_free_space_ctl(cache,
						      root->sectorsize)) {
				ret = -ENOMEM;
				break;
			}
		} else {
			btrfs_remove_free_space_cache(cache);
		}

		ret = load_free_space_cache(root->fs_info, cache);
		if (!ret)
			continue;

		ret = verify_space_cache(root, cache);
		if (ret) {
			fprintf(stderr, "cache appears valid but isnt %Lu\n",
				cache->key.objectid);
			error++;
		}
	}

	task_stop(ctx.info);

	return error ? -EINVAL : 0;
}

static int check_extent_csums(struct btrfs_root *root, u64 bytenr,
			u64 num_bytes, unsigned long leaf_offset,
			struct extent_buffer *eb) {

	u64 offset = 0;
	u16 csum_size = btrfs_super_csum_size(root->fs_info->super_copy);
	char *data;
	unsigned long csum_offset;
	u32 csum;
	u32 csum_expected;
	u64 read_len;
	u64 data_checked = 0;
	u64 tmp;
	int ret = 0;
	int mirror;
	int num_copies;

	if (num_bytes % root->sectorsize)
		return -EINVAL;

	data = malloc(num_bytes);
	if (!data)
		return -ENOMEM;

	while (offset < num_bytes) {
		mirror = 0;
again:
		read_len = num_bytes - offset;
		/* read as much space once a time */
		ret = read_extent_data(root, data + offset,
				bytenr + offset, &read_len, mirror);
		if (ret)
			goto out;
		data_checked = 0;
		/* verify every 4k data's checksum */
		while (data_checked < read_len) {
			csum = ~(u32)0;
			tmp = offset + data_checked;

			csum = btrfs_csum_data(NULL, (char *)data + tmp,
					       csum, root->sectorsize);
			btrfs_csum_final(csum, (char *)&csum);

			csum_offset = leaf_offset +
				 tmp / root->sectorsize * csum_size;
			read_extent_buffer(eb, (char *)&csum_expected,
					   csum_offset, csum_size);
			/* try another mirror */
			if (csum != csum_expected) {
				fprintf(stderr, "mirror %d bytenr %llu csum %u expected csum %u\n",
						mirror, bytenr + tmp,
						csum, csum_expected);
				num_copies = btrfs_num_copies(
						&root->fs_info->mapping_tree,
						bytenr, num_bytes);
				if (mirror < num_copies - 1) {
					mirror += 1;
					goto again;
				}
			}
			data_checked += root->sectorsize;
		}
		offset += read_len;
	}
out:
	free(data);
	return ret;
}

static int check_extent_exists(struct btrfs_root *root, u64 bytenr,
			       u64 num_bytes)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int ret;

	path = btrfs_alloc_path();
	if (!path) {
		fprintf(stderr, "Error allocing path\n");
		return -ENOMEM;
	}

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

again:
	ret = btrfs_search_slot(NULL, root->fs_info->extent_root, &key, path,
				0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error looking up extent record %d\n", ret);
		btrfs_free_path(path);
		return ret;
	} else if (ret) {
		if (path->slots[0] > 0) {
			path->slots[0]--;
		} else {
			ret = btrfs_prev_leaf(root, path);
			if (ret < 0) {
				goto out;
			} else if (ret > 0) {
				ret = 0;
				goto out;
			}
		}
	}

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

	/*
	 * Block group items come before extent items if they have the same
	 * bytenr, so walk back one more just in case.  Dear future traveler,
	 * first congrats on mastering time travel.  Now if it's not too much
	 * trouble could you go back to 2006 and tell Chris to make the
	 * BLOCK_GROUP_ITEM_KEY (and BTRFS_*_REF_KEY) lower than the
	 * EXTENT_ITEM_KEY please?
	 */
	while (key.type > BTRFS_EXTENT_ITEM_KEY) {
		if (path->slots[0] > 0) {
			path->slots[0]--;
		} else {
			ret = btrfs_prev_leaf(root, path);
			if (ret < 0) {
				goto out;
			} else if (ret > 0) {
				ret = 0;
				goto out;
			}
		}
		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	}

	while (num_bytes) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf "
					"%d\n", ret);
				btrfs_free_path(path);
				return ret;
			} else if (ret) {
				break;
			}
		}
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY) {
			path->slots[0]++;
			continue;
		}
		if (key.objectid + key.offset < bytenr) {
			path->slots[0]++;
			continue;
		}
		if (key.objectid > bytenr + num_bytes)
			break;

		if (key.objectid == bytenr) {
			if (key.offset >= num_bytes) {
				num_bytes = 0;
				break;
			}
			num_bytes -= key.offset;
			bytenr += key.offset;
		} else if (key.objectid < bytenr) {
			if (key.objectid + key.offset >= bytenr + num_bytes) {
				num_bytes = 0;
				break;
			}
			num_bytes = (bytenr + num_bytes) -
				(key.objectid + key.offset);
			bytenr = key.objectid + key.offset;
		} else {
			if (key.objectid + key.offset < bytenr + num_bytes) {
				u64 new_start = key.objectid + key.offset;
				u64 new_bytes = bytenr + num_bytes - new_start;

				/*
				 * Weird case, the extent is in the middle of
				 * our range, we'll have to search one side
				 * and then the other.  Not sure if this happens
				 * in real life, but no harm in coding it up
				 * anyway just in case.
				 */
				btrfs_release_path(path);
				ret = check_extent_exists(root, new_start,
							  new_bytes);
				if (ret) {
					fprintf(stderr, "Right section didn't "
						"have a record\n");
					break;
				}
				num_bytes = key.objectid - bytenr;
				goto again;
			}
			num_bytes = key.objectid - bytenr;
		}
		path->slots[0]++;
	}
	ret = 0;

out:
	if (num_bytes && !ret) {
		fprintf(stderr, "There are no extents for csum range "
			"%Lu-%Lu\n", bytenr, bytenr+num_bytes);
		ret = 1;
	}

	btrfs_free_path(path);
	return ret;
}

static int check_csums(struct btrfs_root *root)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 offset = 0, num_bytes = 0;
	u16 csum_size = btrfs_super_csum_size(root->fs_info->super_copy);
	int errors = 0;
	int ret;
	u64 data_len;
	unsigned long leaf_offset;

	root = root->fs_info->csum_root;
	if (!extent_buffer_uptodate(root->node)) {
		fprintf(stderr, "No valid csum tree found\n");
		return -ENOENT;
	}

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.type = BTRFS_EXTENT_CSUM_KEY;
	key.offset = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching csum tree %d\n", ret);
		btrfs_free_path(path);
		return ret;
	}

	if (ret > 0 && path->slots[0])
		path->slots[0]--;
	ret = 0;

	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf "
					"%d\n", ret);
				break;
			}
			if (ret)
				break;
		}
		leaf = path->nodes[0];

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type != BTRFS_EXTENT_CSUM_KEY) {
			path->slots[0]++;
			continue;
		}

		data_len = (btrfs_item_size_nr(leaf, path->slots[0]) /
			      csum_size) * root->sectorsize;
		if (!check_data_csum)
			goto skip_csum_check;
		leaf_offset = btrfs_item_ptr_offset(leaf, path->slots[0]);
		ret = check_extent_csums(root, key.offset, data_len,
					 leaf_offset, leaf);
		if (ret)
			break;
skip_csum_check:
		if (!num_bytes) {
			offset = key.offset;
		} else if (key.offset != offset + num_bytes) {
			ret = check_extent_exists(root, offset, num_bytes);
			if (ret) {
				fprintf(stderr, "Csum exists for %Lu-%Lu but "
					"there is no extent record\n",
					offset, offset+num_bytes);
				errors++;
			}
			offset = key.offset;
			num_bytes = 0;
		}
		num_bytes += data_len;
		path->slots[0]++;
	}

	btrfs_free_path(path);
	return errors;
}

static int is_dropped_key(struct btrfs_key *key,
			  struct btrfs_key *drop_key) {
	if (key->objectid < drop_key->objectid)
		return 1;
	else if (key->objectid == drop_key->objectid) {
		if (key->type < drop_key->type)
			return 1;
		else if (key->type == drop_key->type) {
			if (key->offset < drop_key->offset)
				return 1;
		}
	}
	return 0;
}

/*
 * Here are the rules for FULL_BACKREF.
 *
 * 1) If BTRFS_HEADER_FLAG_RELOC is set then we have FULL_BACKREF set.
 * 2) If btrfs_header_owner(buf) no longer points to buf then we have
 *	FULL_BACKREF set.
 * 3) We cow'ed the block walking down a reloc tree.  This is impossible to tell
 *    if it happened after the relocation occurred since we'll have dropped the
 *    reloc root, so it's entirely possible to have FULL_BACKREF set on buf and
 *    have no real way to know for sure.
 *
 * We process the blocks one root at a time, and we start from the lowest root
 * objectid and go to the highest.  So we can just lookup the owner backref for
 * the record and if we don't find it then we know it doesn't exist and we have
 * a FULL BACKREF.
 *
 * FIXME: if we ever start reclaiming root objectid's then we need to fix this
 * assumption and simply indicate that we _think_ that the FULL BACKREF needs to
 * be set or not and then we can check later once we've gathered all the refs.
 */
static int calc_extent_flag(struct btrfs_root *root,
			   struct cache_tree *extent_cache,
			   struct extent_buffer *buf,
			   struct root_item_record *ri,
			   u64 *flags)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	struct tree_backref *tback;
	u64 owner = 0;

	cache = lookup_cache_extent(extent_cache, buf->start, 1);
	/* we have added this extent before */
	BUG_ON(!cache);
	rec = container_of(cache, struct extent_record, cache);

	/*
	 * Except file/reloc tree, we can not have
	 * FULL BACKREF MODE
	 */
	if (ri->objectid < BTRFS_FIRST_FREE_OBJECTID)
		goto normal;
	/*
	 * root node
	 */
	if (buf->start == ri->bytenr)
		goto normal;

	if (btrfs_header_flag(buf, BTRFS_HEADER_FLAG_RELOC))
		goto full_backref;

	owner = btrfs_header_owner(buf);
	if (owner == ri->objectid)
		goto normal;

	tback = find_tree_backref(rec, 0, owner);
	if (!tback)
		goto full_backref;
normal:
	*flags = 0;
	if (rec->flag_block_full_backref != -1 &&
	    rec->flag_block_full_backref != 0)
		rec->bad_full_backref = 1;
	return 0;
full_backref:
	*flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
	if (rec->flag_block_full_backref != -1 &&
	    rec->flag_block_full_backref != 1)
		rec->bad_full_backref = 1;
	return 0;
}

static int run_next_block(struct btrfs_root *root,
			  struct block_info *bits,
			  int bits_nr,
			  u64 *last,
			  struct cache_tree *pending,
			  struct cache_tree *seen,
			  struct cache_tree *reada,
			  struct cache_tree *nodes,
			  struct cache_tree *extent_cache,
			  struct cache_tree *chunk_cache,
			  struct rb_root *dev_cache,
			  struct block_group_tree *block_group_cache,
			  struct device_extent_tree *dev_extent_cache,
			  struct root_item_record *ri)
{
	struct extent_buffer *buf;
	struct extent_record *rec = NULL;
	u64 bytenr;
	u32 size;
	u64 parent;
	u64 owner;
	u64 flags;
	u64 ptr;
	u64 gen = 0;
	int ret = 0;
	int i;
	int nritems;
	struct btrfs_key key;
	struct cache_extent *cache;
	int reada_bits;

	nritems = pick_next_pending(pending, reada, nodes, *last, bits,
				    bits_nr, &reada_bits);
	if (nritems == 0)
		return 1;

	if (!reada_bits) {
		for(i = 0; i < nritems; i++) {
			ret = add_cache_extent(reada, bits[i].start,
					       bits[i].size);
			if (ret == -EEXIST)
				continue;

			/* fixme, get the parent transid */
			readahead_tree_block(root, bits[i].start,
					     bits[i].size, 0);
		}
	}
	*last = bits[0].start;
	bytenr = bits[0].start;
	size = bits[0].size;

	cache = lookup_cache_extent(pending, bytenr, size);
	if (cache) {
		remove_cache_extent(pending, cache);
		free(cache);
	}
	cache = lookup_cache_extent(reada, bytenr, size);
	if (cache) {
		remove_cache_extent(reada, cache);
		free(cache);
	}
	cache = lookup_cache_extent(nodes, bytenr, size);
	if (cache) {
		remove_cache_extent(nodes, cache);
		free(cache);
	}
	cache = lookup_cache_extent(extent_cache, bytenr, size);
	if (cache) {
		rec = container_of(cache, struct extent_record, cache);
		gen = rec->parent_generation;
	}

	/* fixme, get the real parent transid */
	buf = read_tree_block(root, bytenr, size, gen);
	if (!extent_buffer_uptodate(buf)) {
		record_bad_block_io(root->fs_info,
				    extent_cache, bytenr, size);
		goto out;
	}

	nritems = btrfs_header_nritems(buf);

	flags = 0;
	if (!init_extent_tree) {
		ret = btrfs_lookup_extent_info(NULL, root, bytenr,
				       btrfs_header_level(buf), 1, NULL,
				       &flags);
		if (ret < 0) {
			ret = calc_extent_flag(root, extent_cache, buf, ri, &flags);
			if (ret < 0) {
				fprintf(stderr, "Couldn't calc extent flags\n");
				flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
			}
		}
	} else {
		flags = 0;
		ret = calc_extent_flag(root, extent_cache, buf, ri, &flags);
		if (ret < 0) {
			fprintf(stderr, "Couldn't calc extent flags\n");
			flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
		}
	}

	if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
		if (ri != NULL &&
		    ri->objectid != BTRFS_TREE_RELOC_OBJECTID &&
		    ri->objectid == btrfs_header_owner(buf)) {
			/*
			 * Ok we got to this block from it's original owner and
			 * we have FULL_BACKREF set.  Relocation can leave
			 * converted blocks over so this is altogether possible,
			 * however it's not possible if the generation > the
			 * last snapshot, so check for this case.
			 */
			if (!btrfs_header_flag(buf, BTRFS_HEADER_FLAG_RELOC) &&
			    btrfs_header_generation(buf) > ri->last_snapshot) {
				flags &= ~BTRFS_BLOCK_FLAG_FULL_BACKREF;
				rec->bad_full_backref = 1;
			}
		}
	} else {
		if (ri != NULL &&
		    (ri->objectid == BTRFS_TREE_RELOC_OBJECTID ||
		     btrfs_header_flag(buf, BTRFS_HEADER_FLAG_RELOC))) {
			flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
			rec->bad_full_backref = 1;
		}
	}

	if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
		rec->flag_block_full_backref = 1;
		parent = bytenr;
		owner = 0;
	} else {
		rec->flag_block_full_backref = 0;
		parent = 0;
		owner = btrfs_header_owner(buf);
	}

	ret = check_block(root, extent_cache, buf, flags);
	if (ret)
		goto out;

	if (btrfs_is_leaf(buf)) {
		btree_space_waste += btrfs_leaf_free_space(root, buf);
		for (i = 0; i < nritems; i++) {
			struct btrfs_file_extent_item *fi;
			btrfs_item_key_to_cpu(buf, &key, i);
			if (key.type == BTRFS_EXTENT_ITEM_KEY) {
				process_extent_item(root, extent_cache, buf,
						    i);
				continue;
			}
			if (key.type == BTRFS_METADATA_ITEM_KEY) {
				process_extent_item(root, extent_cache, buf,
						    i);
				continue;
			}
			if (key.type == BTRFS_EXTENT_CSUM_KEY) {
				total_csum_bytes +=
					btrfs_item_size_nr(buf, i);
				continue;
			}
			if (key.type == BTRFS_CHUNK_ITEM_KEY) {
				process_chunk_item(chunk_cache, &key, buf, i);
				continue;
			}
			if (key.type == BTRFS_DEV_ITEM_KEY) {
				process_device_item(dev_cache, &key, buf, i);
				continue;
			}
			if (key.type == BTRFS_BLOCK_GROUP_ITEM_KEY) {
				process_block_group_item(block_group_cache,
					&key, buf, i);
				continue;
			}
			if (key.type == BTRFS_DEV_EXTENT_KEY) {
				process_device_extent_item(dev_extent_cache,
					&key, buf, i);
				continue;

			}
			if (key.type == BTRFS_EXTENT_REF_V0_KEY) {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
				process_extent_ref_v0(extent_cache, buf, i);
#else
				BUG();
#endif
				continue;
			}

			if (key.type == BTRFS_TREE_BLOCK_REF_KEY) {
				add_tree_backref(extent_cache, key.objectid, 0,
						 key.offset, 0);
				continue;
			}
			if (key.type == BTRFS_SHARED_BLOCK_REF_KEY) {
				add_tree_backref(extent_cache, key.objectid,
						 key.offset, 0, 0);
				continue;
			}
			if (key.type == BTRFS_EXTENT_DATA_REF_KEY) {
				struct btrfs_extent_data_ref *ref;
				ref = btrfs_item_ptr(buf, i,
						struct btrfs_extent_data_ref);
				add_data_backref(extent_cache,
					key.objectid, 0,
					btrfs_extent_data_ref_root(buf, ref),
					btrfs_extent_data_ref_objectid(buf,
								       ref),
					btrfs_extent_data_ref_offset(buf, ref),
					btrfs_extent_data_ref_count(buf, ref),
					0, root->sectorsize);
				continue;
			}
			if (key.type == BTRFS_SHARED_DATA_REF_KEY) {
				struct btrfs_shared_data_ref *ref;
				ref = btrfs_item_ptr(buf, i,
						struct btrfs_shared_data_ref);
				add_data_backref(extent_cache,
					key.objectid, key.offset, 0, 0, 0,
					btrfs_shared_data_ref_count(buf, ref),
					0, root->sectorsize);
				continue;
			}
			if (key.type == BTRFS_ORPHAN_ITEM_KEY) {
				struct bad_item *bad;

				if (key.objectid == BTRFS_ORPHAN_OBJECTID)
					continue;
				if (!owner)
					continue;
				bad = malloc(sizeof(struct bad_item));
				if (!bad)
					continue;
				INIT_LIST_HEAD(&bad->list);
				memcpy(&bad->key, &key,
				       sizeof(struct btrfs_key));
				bad->root_id = owner;
				list_add_tail(&bad->list, &delete_items);
				continue;
			}
			if (key.type != BTRFS_EXTENT_DATA_KEY)
				continue;
			fi = btrfs_item_ptr(buf, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(buf, fi) ==
			    BTRFS_FILE_EXTENT_INLINE)
				continue;
			if (btrfs_file_extent_disk_bytenr(buf, fi) == 0)
				continue;

			data_bytes_allocated +=
				btrfs_file_extent_disk_num_bytes(buf, fi);
			if (data_bytes_allocated < root->sectorsize) {
				abort();
			}
			data_bytes_referenced +=
				btrfs_file_extent_num_bytes(buf, fi);
			add_data_backref(extent_cache,
				btrfs_file_extent_disk_bytenr(buf, fi),
				parent, owner, key.objectid, key.offset -
				btrfs_file_extent_offset(buf, fi), 1, 1,
				btrfs_file_extent_disk_num_bytes(buf, fi));
		}
	} else {
		int level;
		struct btrfs_key first_key;

		first_key.objectid = 0;

		if (nritems > 0)
			btrfs_item_key_to_cpu(buf, &first_key, 0);
		level = btrfs_header_level(buf);
		for (i = 0; i < nritems; i++) {
			ptr = btrfs_node_blockptr(buf, i);
			size = btrfs_level_size(root, level - 1);
			btrfs_node_key_to_cpu(buf, &key, i);
			if (ri != NULL) {
				if ((level == ri->drop_level)
				    && is_dropped_key(&key, &ri->drop_key)) {
					continue;
				}
			}
			ret = add_extent_rec(extent_cache, &key,
					     btrfs_node_ptr_generation(buf, i),
					     ptr, size, 0, 0, 1, 0, 1, 0,
					     size);
			BUG_ON(ret);

			add_tree_backref(extent_cache, ptr, parent, owner, 1);

			if (level > 1) {
				add_pending(nodes, seen, ptr, size);
			} else {
				add_pending(pending, seen, ptr, size);
			}
		}
		btree_space_waste += (BTRFS_NODEPTRS_PER_BLOCK(root) -
				      nritems) * sizeof(struct btrfs_key_ptr);
	}
	total_btree_bytes += buf->len;
	if (fs_root_objectid(btrfs_header_owner(buf)))
		total_fs_tree_bytes += buf->len;
	if (btrfs_header_owner(buf) == BTRFS_EXTENT_TREE_OBJECTID)
		total_extent_tree_bytes += buf->len;
	if (!found_old_backref &&
	    btrfs_header_owner(buf) == BTRFS_TREE_RELOC_OBJECTID &&
	    btrfs_header_backref_rev(buf) == BTRFS_MIXED_BACKREF_REV &&
	    !btrfs_header_flag(buf, BTRFS_HEADER_FLAG_RELOC))
		found_old_backref = 1;
out:
	free_extent_buffer(buf);
	return ret;
}

static int add_root_to_pending(struct extent_buffer *buf,
			       struct cache_tree *extent_cache,
			       struct cache_tree *pending,
			       struct cache_tree *seen,
			       struct cache_tree *nodes,
			       u64 objectid)
{
	if (btrfs_header_level(buf) > 0)
		add_pending(nodes, seen, buf->start, buf->len);
	else
		add_pending(pending, seen, buf->start, buf->len);
	add_extent_rec(extent_cache, NULL, 0, buf->start, buf->len,
		       0, 1, 1, 0, 1, 0, buf->len);

	if (objectid == BTRFS_TREE_RELOC_OBJECTID ||
	    btrfs_header_backref_rev(buf) < BTRFS_MIXED_BACKREF_REV)
		add_tree_backref(extent_cache, buf->start, buf->start,
				 0, 1);
	else
		add_tree_backref(extent_cache, buf->start, 0, objectid, 1);
	return 0;
}

/* as we fix the tree, we might be deleting blocks that
 * we're tracking for repair.  This hook makes sure we
 * remove any backrefs for blocks as we are fixing them.
 */
static int free_extent_hook(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    u64 bytenr, u64 num_bytes, u64 parent,
			    u64 root_objectid, u64 owner, u64 offset,
			    int refs_to_drop)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int is_data;
	struct cache_tree *extent_cache = root->fs_info->fsck_extent_cache;

	is_data = owner >= BTRFS_FIRST_FREE_OBJECTID;
	cache = lookup_cache_extent(extent_cache, bytenr, num_bytes);
	if (!cache)
		return 0;

	rec = container_of(cache, struct extent_record, cache);
	if (is_data) {
		struct data_backref *back;
		back = find_data_backref(rec, parent, root_objectid, owner,
					 offset, 1, bytenr, num_bytes);
		if (!back)
			goto out;
		if (back->node.found_ref) {
			back->found_ref -= refs_to_drop;
			if (rec->refs)
				rec->refs -= refs_to_drop;
		}
		if (back->node.found_extent_tree) {
			back->num_refs -= refs_to_drop;
			if (rec->extent_item_refs)
				rec->extent_item_refs -= refs_to_drop;
		}
		if (back->found_ref == 0)
			back->node.found_ref = 0;
		if (back->num_refs == 0)
			back->node.found_extent_tree = 0;

		if (!back->node.found_extent_tree && back->node.found_ref) {
			list_del(&back->node.list);
			free(back);
		}
	} else {
		struct tree_backref *back;
		back = find_tree_backref(rec, parent, root_objectid);
		if (!back)
			goto out;
		if (back->node.found_ref) {
			if (rec->refs)
				rec->refs--;
			back->node.found_ref = 0;
		}
		if (back->node.found_extent_tree) {
			if (rec->extent_item_refs)
				rec->extent_item_refs--;
			back->node.found_extent_tree = 0;
		}
		if (!back->node.found_extent_tree && back->node.found_ref) {
			list_del(&back->node.list);
			free(back);
		}
	}
	maybe_free_extent_rec(extent_cache, rec);
out:
	return 0;
}

static int delete_extent_records(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 u64 bytenr, u64 new_len)
{
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct extent_buffer *leaf;
	int ret;
	int slot;


	key.objectid = bytenr;
	key.type = (u8)-1;
	key.offset = (u64)-1;

	while(1) {
		ret = btrfs_search_slot(trans, root->fs_info->extent_root,
					&key, path, 0, 1);
		if (ret < 0)
			break;

		if (ret > 0) {
			ret = 0;
			if (path->slots[0] == 0)
				break;
			path->slots[0]--;
		}
		ret = 0;

		leaf = path->nodes[0];
		slot = path->slots[0];

		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		if (found_key.objectid != bytenr)
			break;

		if (found_key.type != BTRFS_EXTENT_ITEM_KEY &&
		    found_key.type != BTRFS_METADATA_ITEM_KEY &&
		    found_key.type != BTRFS_TREE_BLOCK_REF_KEY &&
		    found_key.type != BTRFS_EXTENT_DATA_REF_KEY &&
		    found_key.type != BTRFS_EXTENT_REF_V0_KEY &&
		    found_key.type != BTRFS_SHARED_BLOCK_REF_KEY &&
		    found_key.type != BTRFS_SHARED_DATA_REF_KEY) {
			btrfs_release_path(path);
			if (found_key.type == 0) {
				if (found_key.offset == 0)
					break;
				key.offset = found_key.offset - 1;
				key.type = found_key.type;
			}
			key.type = found_key.type - 1;
			key.offset = (u64)-1;
			continue;
		}

		fprintf(stderr, "repair deleting extent record: key %Lu %u %Lu\n",
			found_key.objectid, found_key.type, found_key.offset);

		ret = btrfs_del_item(trans, root->fs_info->extent_root, path);
		if (ret)
			break;
		btrfs_release_path(path);

		if (found_key.type == BTRFS_EXTENT_ITEM_KEY ||
		    found_key.type == BTRFS_METADATA_ITEM_KEY) {
			u64 bytes = (found_key.type == BTRFS_EXTENT_ITEM_KEY) ?
				found_key.offset : root->leafsize;

			ret = btrfs_update_block_group(trans, root, bytenr,
						       bytes, 0, 0);
			if (ret)
				break;
		}
	}

	btrfs_release_path(path);
	return ret;
}

/*
 * for a single backref, this will allocate a new extent
 * and add the backref to it.
 */
static int record_extent(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *info,
			 struct btrfs_path *path,
			 struct extent_record *rec,
			 struct extent_backref *back,
			 int allocated, u64 flags)
{
	int ret;
	struct btrfs_root *extent_root = info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_key ins_key;
	struct btrfs_extent_item *ei;
	struct tree_backref *tback;
	struct data_backref *dback;
	struct btrfs_tree_block_info *bi;

	if (!back->is_data)
		rec->max_size = max_t(u64, rec->max_size,
				    info->extent_root->leafsize);

	if (!allocated) {
		u32 item_size = sizeof(*ei);

		if (!back->is_data)
			item_size += sizeof(*bi);

		ins_key.objectid = rec->start;
		ins_key.offset = rec->max_size;
		ins_key.type = BTRFS_EXTENT_ITEM_KEY;

		ret = btrfs_insert_empty_item(trans, extent_root, path,
					&ins_key, item_size);
		if (ret)
			goto fail;

		leaf = path->nodes[0];
		ei = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_extent_item);

		btrfs_set_extent_refs(leaf, ei, 0);
		btrfs_set_extent_generation(leaf, ei, rec->generation);

		if (back->is_data) {
			btrfs_set_extent_flags(leaf, ei,
					       BTRFS_EXTENT_FLAG_DATA);
		} else {
			struct btrfs_disk_key copy_key;;

			tback = (struct tree_backref *)back;
			bi = (struct btrfs_tree_block_info *)(ei + 1);
			memset_extent_buffer(leaf, 0, (unsigned long)bi,
					     sizeof(*bi));

			btrfs_set_disk_key_objectid(&copy_key,
						    rec->info_objectid);
			btrfs_set_disk_key_type(&copy_key, 0);
			btrfs_set_disk_key_offset(&copy_key, 0);

			btrfs_set_tree_block_level(leaf, bi, rec->info_level);
			btrfs_set_tree_block_key(leaf, bi, &copy_key);

			btrfs_set_extent_flags(leaf, ei,
					       BTRFS_EXTENT_FLAG_TREE_BLOCK | flags);
		}

		btrfs_mark_buffer_dirty(leaf);
		ret = btrfs_update_block_group(trans, extent_root, rec->start,
					       rec->max_size, 1, 0);
		if (ret)
			goto fail;
		btrfs_release_path(path);
	}

	if (back->is_data) {
		u64 parent;
		int i;

		dback = (struct data_backref *)back;
		if (back->full_backref)
			parent = dback->parent;
		else
			parent = 0;

		for (i = 0; i < dback->found_ref; i++) {
			/* if parent != 0, we're doing a full backref
			 * passing BTRFS_FIRST_FREE_OBJECTID as the owner
			 * just makes the backref allocator create a data
			 * backref
			 */
			ret = btrfs_inc_extent_ref(trans, info->extent_root,
						   rec->start, rec->max_size,
						   parent,
						   dback->root,
						   parent ?
						   BTRFS_FIRST_FREE_OBJECTID :
						   dback->owner,
						   dback->offset);
			if (ret)
				break;
		}
		fprintf(stderr, "adding new data backref"
				" on %llu %s %llu owner %llu"
				" offset %llu found %d\n",
				(unsigned long long)rec->start,
				back->full_backref ?
				"parent" : "root",
				back->full_backref ?
				(unsigned long long)parent :
				(unsigned long long)dback->root,
				(unsigned long long)dback->owner,
				(unsigned long long)dback->offset,
				dback->found_ref);
	} else {
		u64 parent;

		tback = (struct tree_backref *)back;
		if (back->full_backref)
			parent = tback->parent;
		else
			parent = 0;

		ret = btrfs_inc_extent_ref(trans, info->extent_root,
					   rec->start, rec->max_size,
					   parent, tback->root, 0, 0);
		fprintf(stderr, "adding new tree backref on "
			"start %llu len %llu parent %llu root %llu\n",
			rec->start, rec->max_size, parent, tback->root);
	}
	if (ret)
		goto fail;
fail:
	btrfs_release_path(path);
	return ret;
}

struct extent_entry {
	u64 bytenr;
	u64 bytes;
	int count;
	int broken;
	struct list_head list;
};

static struct extent_entry *find_entry(struct list_head *entries,
				       u64 bytenr, u64 bytes)
{
	struct extent_entry *entry = NULL;

	list_for_each_entry(entry, entries, list) {
		if (entry->bytenr == bytenr && entry->bytes == bytes)
			return entry;
	}

	return NULL;
}

static struct extent_entry *find_most_right_entry(struct list_head *entries)
{
	struct extent_entry *entry, *best = NULL, *prev = NULL;

	list_for_each_entry(entry, entries, list) {
		if (!prev) {
			prev = entry;
			continue;
		}

		/*
		 * If there are as many broken entries as entries then we know
		 * not to trust this particular entry.
		 */
		if (entry->broken == entry->count)
			continue;

		/*
		 * If our current entry == best then we can't be sure our best
		 * is really the best, so we need to keep searching.
		 */
		if (best && best->count == entry->count) {
			prev = entry;
			best = NULL;
			continue;
		}

		/* Prev == entry, not good enough, have to keep searching */
		if (!prev->broken && prev->count == entry->count)
			continue;

		if (!best)
			best = (prev->count > entry->count) ? prev : entry;
		else if (best->count < entry->count)
			best = entry;
		prev = entry;
	}

	return best;
}

static int repair_ref(struct btrfs_fs_info *info, struct btrfs_path *path,
		      struct data_backref *dback, struct extent_entry *entry)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	struct btrfs_file_extent_item *fi;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 bytenr, bytes;
	int ret, err;

	key.objectid = dback->root;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	root = btrfs_read_fs_root(info, &key);
	if (IS_ERR(root)) {
		fprintf(stderr, "Couldn't find root for our ref\n");
		return -EINVAL;
	}

	/*
	 * The backref points to the original offset of the extent if it was
	 * split, so we need to search down to the offset we have and then walk
	 * forward until we find the backref we're looking for.
	 */
	key.objectid = dback->owner;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = dback->offset;
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error looking up ref %d\n", ret);
		return ret;
	}

	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(root, path);
			if (ret) {
				fprintf(stderr, "Couldn't find our ref, next\n");
				return -EINVAL;
			}
		}
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != dback->owner ||
		    key.type != BTRFS_EXTENT_DATA_KEY) {
			fprintf(stderr, "Couldn't find our ref, search\n");
			return -EINVAL;
		}
		fi = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_file_extent_item);
		bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
		bytes = btrfs_file_extent_disk_num_bytes(leaf, fi);

		if (bytenr == dback->disk_bytenr && bytes == dback->bytes)
			break;
		path->slots[0]++;
	}

	btrfs_release_path(path);

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	/*
	 * Ok we have the key of the file extent we want to fix, now we can cow
	 * down to the thing and fix it.
	 */
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret < 0) {
		fprintf(stderr, "Error cowing down to ref [%Lu, %u, %Lu]: %d\n",
			key.objectid, key.type, key.offset, ret);
		goto out;
	}
	if (ret > 0) {
		fprintf(stderr, "Well that's odd, we just found this key "
			"[%Lu, %u, %Lu]\n", key.objectid, key.type,
			key.offset);
		ret = -EINVAL;
		goto out;
	}
	leaf = path->nodes[0];
	fi = btrfs_item_ptr(leaf, path->slots[0],
			    struct btrfs_file_extent_item);

	if (btrfs_file_extent_compression(leaf, fi) &&
	    dback->disk_bytenr != entry->bytenr) {
		fprintf(stderr, "Ref doesn't match the record start and is "
			"compressed, please take a btrfs-image of this file "
			"system and send it to a btrfs developer so they can "
			"complete this functionality for bytenr %Lu\n",
			dback->disk_bytenr);
		ret = -EINVAL;
		goto out;
	}

	if (dback->node.broken && dback->disk_bytenr != entry->bytenr) {
		btrfs_set_file_extent_disk_bytenr(leaf, fi, entry->bytenr);
	} else if (dback->disk_bytenr > entry->bytenr) {
		u64 off_diff, offset;

		off_diff = dback->disk_bytenr - entry->bytenr;
		offset = btrfs_file_extent_offset(leaf, fi);
		if (dback->disk_bytenr + offset +
		    btrfs_file_extent_num_bytes(leaf, fi) >
		    entry->bytenr + entry->bytes) {
			fprintf(stderr, "Ref is past the entry end, please "
				"take a btrfs-image of this file system and "
				"send it to a btrfs developer, ref %Lu\n",
				dback->disk_bytenr);
			ret = -EINVAL;
			goto out;
		}
		offset += off_diff;
		btrfs_set_file_extent_disk_bytenr(leaf, fi, entry->bytenr);
		btrfs_set_file_extent_offset(leaf, fi, offset);
	} else if (dback->disk_bytenr < entry->bytenr) {
		u64 offset;

		offset = btrfs_file_extent_offset(leaf, fi);
		if (dback->disk_bytenr + offset < entry->bytenr) {
			fprintf(stderr, "Ref is before the entry start, please"
				" take a btrfs-image of this file system and "
				"send it to a btrfs developer, ref %Lu\n",
				dback->disk_bytenr);
			ret = -EINVAL;
			goto out;
		}

		offset += dback->disk_bytenr;
		offset -= entry->bytenr;
		btrfs_set_file_extent_disk_bytenr(leaf, fi, entry->bytenr);
		btrfs_set_file_extent_offset(leaf, fi, offset);
	}

	btrfs_set_file_extent_disk_num_bytes(leaf, fi, entry->bytes);

	/*
	 * Chances are if disk_num_bytes were wrong then so is ram_bytes, but
	 * only do this if we aren't using compression, otherwise it's a
	 * trickier case.
	 */
	if (!btrfs_file_extent_compression(leaf, fi))
		btrfs_set_file_extent_ram_bytes(leaf, fi, entry->bytes);
	else
		printf("ram bytes may be wrong?\n");
	btrfs_mark_buffer_dirty(leaf);
out:
	err = btrfs_commit_transaction(trans, root);
	btrfs_release_path(path);
	return ret ? ret : err;
}

static int verify_backrefs(struct btrfs_fs_info *info, struct btrfs_path *path,
			   struct extent_record *rec)
{
	struct extent_backref *back;
	struct data_backref *dback;
	struct extent_entry *entry, *best = NULL;
	LIST_HEAD(entries);
	int nr_entries = 0;
	int broken_entries = 0;
	int ret = 0;
	short mismatch = 0;

	/*
	 * Metadata is easy and the backrefs should always agree on bytenr and
	 * size, if not we've got bigger issues.
	 */
	if (rec->metadata)
		return 0;

	list_for_each_entry(back, &rec->backrefs, list) {
		if (back->full_backref || !back->is_data)
			continue;

		dback = (struct data_backref *)back;

		/*
		 * We only pay attention to backrefs that we found a real
		 * backref for.
		 */
		if (dback->found_ref == 0)
			continue;

		/*
		 * For now we only catch when the bytes don't match, not the
		 * bytenr.  We can easily do this at the same time, but I want
		 * to have a fs image to test on before we just add repair
		 * functionality willy-nilly so we know we won't screw up the
		 * repair.
		 */

		entry = find_entry(&entries, dback->disk_bytenr,
				   dback->bytes);
		if (!entry) {
			entry = malloc(sizeof(struct extent_entry));
			if (!entry) {
				ret = -ENOMEM;
				goto out;
			}
			memset(entry, 0, sizeof(*entry));
			entry->bytenr = dback->disk_bytenr;
			entry->bytes = dback->bytes;
			list_add_tail(&entry->list, &entries);
			nr_entries++;
		}

		/*
		 * If we only have on entry we may think the entries agree when
		 * in reality they don't so we have to do some extra checking.
		 */
		if (dback->disk_bytenr != rec->start ||
		    dback->bytes != rec->nr || back->broken)
			mismatch = 1;

		if (back->broken) {
			entry->broken++;
			broken_entries++;
		}

		entry->count++;
	}

	/* Yay all the backrefs agree, carry on good sir */
	if (nr_entries <= 1 && !mismatch)
		goto out;

	fprintf(stderr, "attempting to repair backref discrepency for bytenr "
		"%Lu\n", rec->start);

	/*
	 * First we want to see if the backrefs can agree amongst themselves who
	 * is right, so figure out which one of the entries has the highest
	 * count.
	 */
	best = find_most_right_entry(&entries);

	/*
	 * Ok so we may have an even split between what the backrefs think, so
	 * this is where we use the extent ref to see what it thinks.
	 */
	if (!best) {
		entry = find_entry(&entries, rec->start, rec->nr);
		if (!entry && (!broken_entries || !rec->found_rec)) {
			fprintf(stderr, "Backrefs don't agree with each other "
				"and extent record doesn't agree with anybody,"
				" so we can't fix bytenr %Lu bytes %Lu\n",
				rec->start, rec->nr);
			ret = -EINVAL;
			goto out;
		} else if (!entry) {
			/*
			 * Ok our backrefs were broken, we'll assume this is the
			 * correct value and add an entry for this range.
			 */
			entry = malloc(sizeof(struct extent_entry));
			if (!entry) {
				ret = -ENOMEM;
				goto out;
			}
			memset(entry, 0, sizeof(*entry));
			entry->bytenr = rec->start;
			entry->bytes = rec->nr;
			list_add_tail(&entry->list, &entries);
			nr_entries++;
		}
		entry->count++;
		best = find_most_right_entry(&entries);
		if (!best) {
			fprintf(stderr, "Backrefs and extent record evenly "
				"split on who is right, this is going to "
				"require user input to fix bytenr %Lu bytes "
				"%Lu\n", rec->start, rec->nr);
			ret = -EINVAL;
			goto out;
		}
	}

	/*
	 * I don't think this can happen currently as we'll abort() if we catch
	 * this case higher up, but in case somebody removes that we still can't
	 * deal with it properly here yet, so just bail out of that's the case.
	 */
	if (best->bytenr != rec->start) {
		fprintf(stderr, "Extent start and backref starts don't match, "
			"please use btrfs-image on this file system and send "
			"it to a btrfs developer so they can make fsck fix "
			"this particular case.  bytenr is %Lu, bytes is %Lu\n",
			rec->start, rec->nr);
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Ok great we all agreed on an extent record, let's go find the real
	 * references and fix up the ones that don't match.
	 */
	list_for_each_entry(back, &rec->backrefs, list) {
		if (back->full_backref || !back->is_data)
			continue;

		dback = (struct data_backref *)back;

		/*
		 * Still ignoring backrefs that don't have a real ref attached
		 * to them.
		 */
		if (dback->found_ref == 0)
			continue;

		if (dback->bytes == best->bytes &&
		    dback->disk_bytenr == best->bytenr)
			continue;

		ret = repair_ref(info, path, dback, best);
		if (ret)
			goto out;
	}

	/*
	 * Ok we messed with the actual refs, which means we need to drop our
	 * entire cache and go back and rescan.  I know this is a huge pain and
	 * adds a lot of extra work, but it's the only way to be safe.  Once all
	 * the backrefs agree we may not need to do anything to the extent
	 * record itself.
	 */
	ret = -EAGAIN;
out:
	while (!list_empty(&entries)) {
		entry = list_entry(entries.next, struct extent_entry, list);
		list_del_init(&entry->list);
		free(entry);
	}
	return ret;
}

static int process_duplicates(struct btrfs_root *root,
			      struct cache_tree *extent_cache,
			      struct extent_record *rec)
{
	struct extent_record *good, *tmp;
	struct cache_extent *cache;
	int ret;

	/*
	 * If we found a extent record for this extent then return, or if we
	 * have more than one duplicate we are likely going to need to delete
	 * something.
	 */
	if (rec->found_rec || rec->num_duplicates > 1)
		return 0;

	/* Shouldn't happen but just in case */
	BUG_ON(!rec->num_duplicates);

	/*
	 * So this happens if we end up with a backref that doesn't match the
	 * actual extent entry.  So either the backref is bad or the extent
	 * entry is bad.  Either way we want to have the extent_record actually
	 * reflect what we found in the extent_tree, so we need to take the
	 * duplicate out and use that as the extent_record since the only way we
	 * get a duplicate is if we find a real life BTRFS_EXTENT_ITEM_KEY.
	 */
	remove_cache_extent(extent_cache, &rec->cache);

	good = list_entry(rec->dups.next, struct extent_record, list);
	list_del_init(&good->list);
	INIT_LIST_HEAD(&good->backrefs);
	INIT_LIST_HEAD(&good->dups);
	good->cache.start = good->start;
	good->cache.size = good->nr;
	good->content_checked = 0;
	good->owner_ref_checked = 0;
	good->num_duplicates = 0;
	good->refs = rec->refs;
	list_splice_init(&rec->backrefs, &good->backrefs);
	while (1) {
		cache = lookup_cache_extent(extent_cache, good->start,
					    good->nr);
		if (!cache)
			break;
		tmp = container_of(cache, struct extent_record, cache);

		/*
		 * If we find another overlapping extent and it's found_rec is
		 * set then it's a duplicate and we need to try and delete
		 * something.
		 */
		if (tmp->found_rec || tmp->num_duplicates > 0) {
			if (list_empty(&good->list))
				list_add_tail(&good->list,
					      &duplicate_extents);
			good->num_duplicates += tmp->num_duplicates + 1;
			list_splice_init(&tmp->dups, &good->dups);
			list_del_init(&tmp->list);
			list_add_tail(&tmp->list, &good->dups);
			remove_cache_extent(extent_cache, &tmp->cache);
			continue;
		}

		/*
		 * Ok we have another non extent item backed extent rec, so lets
		 * just add it to this extent and carry on like we did above.
		 */
		good->refs += tmp->refs;
		list_splice_init(&tmp->backrefs, &good->backrefs);
		remove_cache_extent(extent_cache, &tmp->cache);
		free(tmp);
	}
	ret = insert_cache_extent(extent_cache, &good->cache);
	BUG_ON(ret);
	free(rec);
	return good->num_duplicates ? 0 : 1;
}

static int delete_duplicate_records(struct btrfs_root *root,
				    struct extent_record *rec)
{
	struct btrfs_trans_handle *trans;
	LIST_HEAD(delete_list);
	struct btrfs_path *path;
	struct extent_record *tmp, *good, *n;
	int nr_del = 0;
	int ret = 0, err;
	struct btrfs_key key;

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	good = rec;
	/* Find the record that covers all of the duplicates. */
	list_for_each_entry(tmp, &rec->dups, list) {
		if (good->start < tmp->start)
			continue;
		if (good->nr > tmp->nr)
			continue;

		if (tmp->start + tmp->nr < good->start + good->nr) {
			fprintf(stderr, "Ok we have overlapping extents that "
				"aren't completely covered by eachother, this "
				"is going to require more careful thought.  "
				"The extents are [%Lu-%Lu] and [%Lu-%Lu]\n",
				tmp->start, tmp->nr, good->start, good->nr);
			abort();
		}
		good = tmp;
	}

	if (good != rec)
		list_add_tail(&rec->list, &delete_list);

	list_for_each_entry_safe(tmp, n, &rec->dups, list) {
		if (tmp == good)
			continue;
		list_move_tail(&tmp->list, &delete_list);
	}

	root = root->fs_info->extent_root;
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}

	list_for_each_entry(tmp, &delete_list, list) {
		if (tmp->found_rec == 0)
			continue;
		key.objectid = tmp->start;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = tmp->nr;

		/* Shouldn't happen but just in case */
		if (tmp->metadata) {
			fprintf(stderr, "Well this shouldn't happen, extent "
				"record overlaps but is metadata? "
				"[%Lu, %Lu]\n", tmp->start, tmp->nr);
			abort();
		}

		ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret) {
			if (ret > 0)
				ret = -EINVAL;
			break;
		}
		ret = btrfs_del_item(trans, root, path);
		if (ret)
			break;
		btrfs_release_path(path);
		nr_del++;
	}
	err = btrfs_commit_transaction(trans, root);
	if (err && !ret)
		ret = err;
out:
	while (!list_empty(&delete_list)) {
		tmp = list_entry(delete_list.next, struct extent_record, list);
		list_del_init(&tmp->list);
		if (tmp == rec)
			continue;
		free(tmp);
	}

	while (!list_empty(&rec->dups)) {
		tmp = list_entry(rec->dups.next, struct extent_record, list);
		list_del_init(&tmp->list);
		free(tmp);
	}

	btrfs_free_path(path);

	if (!ret && !nr_del)
		rec->num_duplicates = 0;

	return ret ? ret : nr_del;
}

static int find_possible_backrefs(struct btrfs_fs_info *info,
				  struct btrfs_path *path,
				  struct cache_tree *extent_cache,
				  struct extent_record *rec)
{
	struct btrfs_root *root;
	struct extent_backref *back;
	struct data_backref *dback;
	struct cache_extent *cache;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key key;
	u64 bytenr, bytes;
	int ret;

	list_for_each_entry(back, &rec->backrefs, list) {
		/* Don't care about full backrefs (poor unloved backrefs) */
		if (back->full_backref || !back->is_data)
			continue;

		dback = (struct data_backref *)back;

		/* We found this one, we don't need to do a lookup */
		if (dback->found_ref)
			continue;

		key.objectid = dback->root;
		key.type = BTRFS_ROOT_ITEM_KEY;
		key.offset = (u64)-1;

		root = btrfs_read_fs_root(info, &key);

		/* No root, definitely a bad ref, skip */
		if (IS_ERR(root) && PTR_ERR(root) == -ENOENT)
			continue;
		/* Other err, exit */
		if (IS_ERR(root))
			return PTR_ERR(root);

		key.objectid = dback->owner;
		key.type = BTRFS_EXTENT_DATA_KEY;
		key.offset = dback->offset;
		ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
		if (ret) {
			btrfs_release_path(path);
			if (ret < 0)
				return ret;
			/* Didn't find it, we can carry on */
			ret = 0;
			continue;
		}

		fi = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_file_extent_item);
		bytenr = btrfs_file_extent_disk_bytenr(path->nodes[0], fi);
		bytes = btrfs_file_extent_disk_num_bytes(path->nodes[0], fi);
		btrfs_release_path(path);
		cache = lookup_cache_extent(extent_cache, bytenr, 1);
		if (cache) {
			struct extent_record *tmp;
			tmp = container_of(cache, struct extent_record, cache);

			/*
			 * If we found an extent record for the bytenr for this
			 * particular backref then we can't add it to our
			 * current extent record.  We only want to add backrefs
			 * that don't have a corresponding extent item in the
			 * extent tree since they likely belong to this record
			 * and we need to fix it if it doesn't match bytenrs.
			 */
			if  (tmp->found_rec)
				continue;
		}

		dback->found_ref += 1;
		dback->disk_bytenr = bytenr;
		dback->bytes = bytes;

		/*
		 * Set this so the verify backref code knows not to trust the
		 * values in this backref.
		 */
		back->broken = 1;
	}

	return 0;
}

/*
 * Record orphan data ref into corresponding root.
 *
 * Return 0 if the extent item contains data ref and recorded.
 * Return 1 if the extent item contains no useful data ref
 *   On that case, it may contains only shared_dataref or metadata backref
 *   or the file extent exists(this should be handled by the extent bytenr
 *   recovery routine)
 * Return <0 if something goes wrong.
 */
static int record_orphan_data_extents(struct btrfs_fs_info *fs_info,
				      struct extent_record *rec)
{
	struct btrfs_key key;
	struct btrfs_root *dest_root;
	struct extent_backref *back;
	struct data_backref *dback;
	struct orphan_data_extent *orphan;
	struct btrfs_path *path;
	int recorded_data_ref = 0;
	int ret = 0;

	if (rec->metadata)
		return 1;
	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	list_for_each_entry(back, &rec->backrefs, list) {
		if (back->full_backref || !back->is_data ||
		    !back->found_extent_tree)
			continue;
		dback = (struct data_backref *)back;
		if (dback->found_ref)
			continue;
		key.objectid = dback->root;
		key.type = BTRFS_ROOT_ITEM_KEY;
		key.offset = (u64)-1;

		dest_root = btrfs_read_fs_root(fs_info, &key);

		/* For non-exist root we just skip it */
		if (IS_ERR(dest_root) || !dest_root)
			continue;

		key.objectid = dback->owner;
		key.type = BTRFS_EXTENT_DATA_KEY;
		key.offset = dback->offset;

		ret = btrfs_search_slot(NULL, dest_root, &key, path, 0, 0);
		/*
		 * For ret < 0, it's OK since the fs-tree may be corrupted,
		 * we need to record it for inode/file extent rebuild.
		 * For ret > 0, we record it only for file extent rebuild.
		 * For ret == 0, the file extent exists but only bytenr
		 * mismatch, let the original bytenr fix routine to handle,
		 * don't record it.
		 */
		if (ret == 0)
			continue;
		ret = 0;
		orphan = malloc(sizeof(*orphan));
		if (!orphan) {
			ret = -ENOMEM;
			goto out;
		}
		INIT_LIST_HEAD(&orphan->list);
		orphan->root = dback->root;
		orphan->objectid = dback->owner;
		orphan->offset = dback->offset;
		orphan->disk_bytenr = rec->cache.start;
		orphan->disk_len = rec->cache.size;
		list_add(&dest_root->orphan_data_extents, &orphan->list);
		recorded_data_ref = 1;
	}
out:
	btrfs_free_path(path);
	if (!ret)
		return !recorded_data_ref;
	else
		return ret;
}

/*
 * when an incorrect extent item is found, this will delete
 * all of the existing entries for it and recreate them
 * based on what the tree scan found.
 */
static int fixup_extent_refs(struct btrfs_fs_info *info,
			     struct cache_tree *extent_cache,
			     struct extent_record *rec)
{
	struct btrfs_trans_handle *trans = NULL;
	int ret;
	struct btrfs_path *path;
	struct list_head *cur = rec->backrefs.next;
	struct cache_extent *cache;
	struct extent_backref *back;
	int allocated = 0;
	u64 flags = 0;

	if (rec->flag_block_full_backref)
		flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	if (rec->refs != rec->extent_item_refs && !rec->metadata) {
		/*
		 * Sometimes the backrefs themselves are so broken they don't
		 * get attached to any meaningful rec, so first go back and
		 * check any of our backrefs that we couldn't find and throw
		 * them into the list if we find the backref so that
		 * verify_backrefs can figure out what to do.
		 */
		ret = find_possible_backrefs(info, path, extent_cache, rec);
		if (ret < 0)
			goto out;
	}

	/* step one, make sure all of the backrefs agree */
	ret = verify_backrefs(info, path, rec);
	if (ret < 0)
		goto out;

	trans = btrfs_start_transaction(info->extent_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}

	/* step two, delete all the existing records */
	ret = delete_extent_records(trans, info->extent_root, path,
				    rec->start, rec->max_size);

	if (ret < 0)
		goto out;

	/* was this block corrupt?  If so, don't add references to it */
	cache = lookup_cache_extent(info->corrupt_blocks,
				    rec->start, rec->max_size);
	if (cache) {
		ret = 0;
		goto out;
	}

	/* step three, recreate all the refs we did find */
	while(cur != &rec->backrefs) {
		back = list_entry(cur, struct extent_backref, list);
		cur = cur->next;

		/*
		 * if we didn't find any references, don't create a
		 * new extent record
		 */
		if (!back->found_ref)
			continue;

		rec->bad_full_backref = 0;
		ret = record_extent(trans, info, path, rec, back, allocated, flags);
		allocated = 1;

		if (ret)
			goto out;
	}
out:
	if (trans) {
		int err = btrfs_commit_transaction(trans, info->extent_root);
		if (!ret)
			ret = err;
	}

	btrfs_free_path(path);
	return ret;
}

static int fixup_extent_flags(struct btrfs_fs_info *fs_info,
			      struct extent_record *rec)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root = fs_info->extent_root;
	struct btrfs_path *path;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;
	u64 flags;
	int ret = 0;

	key.objectid = rec->start;
	if (rec->metadata) {
		key.type = BTRFS_METADATA_ITEM_KEY;
		key.offset = rec->info_level;
	} else {
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = rec->max_size;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 0);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		return PTR_ERR(trans);
	}

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret < 0) {
		btrfs_free_path(path);
		btrfs_commit_transaction(trans, root);
		return ret;
	} else if (ret) {
		fprintf(stderr, "Didn't find extent for %llu\n",
			(unsigned long long)rec->start);
		btrfs_free_path(path);
		btrfs_commit_transaction(trans, root);
		return -ENOENT;
	}

	ei = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_extent_item);
	flags = btrfs_extent_flags(path->nodes[0], ei);
	if (rec->flag_block_full_backref) {
		fprintf(stderr, "setting full backref on %llu\n",
			(unsigned long long)key.objectid);
		flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
	} else {
		fprintf(stderr, "clearing full backref on %llu\n",
			(unsigned long long)key.objectid);
		flags &= ~BTRFS_BLOCK_FLAG_FULL_BACKREF;
	}
	btrfs_set_extent_flags(path->nodes[0], ei, flags);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	btrfs_free_path(path);
	return btrfs_commit_transaction(trans, root);
}

/* right now we only prune from the extent allocation tree */
static int prune_one_block(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *info,
			   struct btrfs_corrupt_block *corrupt)
{
	int ret;
	struct btrfs_path path;
	struct extent_buffer *eb;
	u64 found;
	int slot;
	int nritems;
	int level = corrupt->level + 1;

	btrfs_init_path(&path);
again:
	/* we want to stop at the parent to our busted block */
	path.lowest_level = level;

	ret = btrfs_search_slot(trans, info->extent_root,
				&corrupt->key, &path, -1, 1);

	if (ret < 0)
		goto out;

	eb = path.nodes[level];
	if (!eb) {
		ret = -ENOENT;
		goto out;
	}

	/*
	 * hopefully the search gave us the block we want to prune,
	 * lets try that first
	 */
	slot = path.slots[level];
	found =  btrfs_node_blockptr(eb, slot);
	if (found == corrupt->cache.start)
		goto del_ptr;

	nritems = btrfs_header_nritems(eb);

	/* the search failed, lets scan this node and hope we find it */
	for (slot = 0; slot < nritems; slot++) {
		found =  btrfs_node_blockptr(eb, slot);
		if (found == corrupt->cache.start)
			goto del_ptr;
	}
	/*
	 * we couldn't find the bad block.  TODO, search all the nodes for pointers
	 * to this block
	 */
	if (eb == info->extent_root->node) {
		ret = -ENOENT;
		goto out;
	} else {
		level++;
		btrfs_release_path(&path);
		goto again;
	}

del_ptr:
	printk("deleting pointer to block %Lu\n", corrupt->cache.start);
	ret = btrfs_del_ptr(trans, info->extent_root, &path, level, slot);

out:
	btrfs_release_path(&path);
	return ret;
}

static int prune_corrupt_blocks(struct btrfs_fs_info *info)
{
	struct btrfs_trans_handle *trans = NULL;
	struct cache_extent *cache;
	struct btrfs_corrupt_block *corrupt;

	while (1) {
		cache = search_cache_extent(info->corrupt_blocks, 0);
		if (!cache)
			break;
		if (!trans) {
			trans = btrfs_start_transaction(info->extent_root, 1);
			if (IS_ERR(trans))
				return PTR_ERR(trans);
		}
		corrupt = container_of(cache, struct btrfs_corrupt_block, cache);
		prune_one_block(trans, info, corrupt);
		remove_cache_extent(info->corrupt_blocks, cache);
	}
	if (trans)
		return btrfs_commit_transaction(trans, info->extent_root);
	return 0;
}

static void reset_cached_block_groups(struct btrfs_fs_info *fs_info)
{
	struct btrfs_block_group_cache *cache;
	u64 start, end;
	int ret;

	while (1) {
		ret = find_first_extent_bit(&fs_info->free_space_cache, 0,
					    &start, &end, EXTENT_DIRTY);
		if (ret)
			break;
		clear_extent_dirty(&fs_info->free_space_cache, start, end,
				   GFP_NOFS);
	}

	start = 0;
	while (1) {
		cache = btrfs_lookup_first_block_group(fs_info, start);
		if (!cache)
			break;
		if (cache->cached)
			cache->cached = 0;
		start = cache->key.objectid + cache->key.offset;
	}
}

static int check_extent_refs(struct btrfs_root *root,
			     struct cache_tree *extent_cache)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int err = 0;
	int ret = 0;
	int fixed = 0;
	int had_dups = 0;
	int recorded = 0;

	if (repair) {
		/*
		 * if we're doing a repair, we have to make sure
		 * we don't allocate from the problem extents.
		 * In the worst case, this will be all the
		 * extents in the FS
		 */
		cache = search_cache_extent(extent_cache, 0);
		while(cache) {
			rec = container_of(cache, struct extent_record, cache);
			set_extent_dirty(root->fs_info->excluded_extents,
					 rec->start,
					 rec->start + rec->max_size - 1,
					 GFP_NOFS);
			cache = next_cache_extent(cache);
		}

		/* pin down all the corrupted blocks too */
		cache = search_cache_extent(root->fs_info->corrupt_blocks, 0);
		while(cache) {
			set_extent_dirty(root->fs_info->excluded_extents,
					 cache->start,
					 cache->start + cache->size - 1,
					 GFP_NOFS);
			cache = next_cache_extent(cache);
		}
		prune_corrupt_blocks(root->fs_info);
		reset_cached_block_groups(root->fs_info);
	}

	reset_cached_block_groups(root->fs_info);

	/*
	 * We need to delete any duplicate entries we find first otherwise we
	 * could mess up the extent tree when we have backrefs that actually
	 * belong to a different extent item and not the weird duplicate one.
	 */
	while (repair && !list_empty(&duplicate_extents)) {
		rec = list_entry(duplicate_extents.next, struct extent_record,
				 list);
		list_del_init(&rec->list);

		/* Sometimes we can find a backref before we find an actual
		 * extent, so we need to process it a little bit to see if there
		 * truly are multiple EXTENT_ITEM_KEY's for the same range, or
		 * if this is a backref screwup.  If we need to delete stuff
		 * process_duplicates() will return 0, otherwise it will return
		 * 1 and we
		 */
		if (process_duplicates(root, extent_cache, rec))
			continue;
		ret = delete_duplicate_records(root, rec);
		if (ret < 0)
			return ret;
		/*
		 * delete_duplicate_records will return the number of entries
		 * deleted, so if it's greater than 0 then we know we actually
		 * did something and we need to remove.
		 */
		if (ret)
			had_dups = 1;
	}

	if (had_dups)
		return -EAGAIN;

	while(1) {
		int cur_err = 0;

		fixed = 0;
		recorded = 0;
		cache = search_cache_extent(extent_cache, 0);
		if (!cache)
			break;
		rec = container_of(cache, struct extent_record, cache);
		if (rec->num_duplicates) {
			fprintf(stderr, "extent item %llu has multiple extent "
				"items\n", (unsigned long long)rec->start);
			err = 1;
			cur_err = 1;
		}

		if (rec->refs != rec->extent_item_refs) {
			fprintf(stderr, "ref mismatch on [%llu %llu] ",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);
			fprintf(stderr, "extent item %llu, found %llu\n",
				(unsigned long long)rec->extent_item_refs,
				(unsigned long long)rec->refs);
			ret = record_orphan_data_extents(root->fs_info, rec);
			if (ret < 0)
				goto repair_abort;
			if (ret == 0) {
				recorded = 1;
			} else {
				/*
				 * we can't use the extent to repair file
				 * extent, let the fallback method handle it.
				 */
				if (!fixed && repair) {
					ret = fixup_extent_refs(
							root->fs_info,
							extent_cache, rec);
					if (ret)
						goto repair_abort;
					fixed = 1;
				}
			}
			err = 1;
			cur_err = 1;
		}
		if (all_backpointers_checked(rec, 1)) {
			fprintf(stderr, "backpointer mismatch on [%llu %llu]\n",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);

			if (!fixed && !recorded && repair) {
				ret = fixup_extent_refs(root->fs_info,
							extent_cache, rec);
				if (ret)
					goto repair_abort;
				fixed = 1;
			}
			cur_err = 1;
			err = 1;
		}
		if (!rec->owner_ref_checked) {
			fprintf(stderr, "owner ref check failed [%llu %llu]\n",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);
			if (!fixed && !recorded && repair) {
				ret = fixup_extent_refs(root->fs_info,
							extent_cache, rec);
				if (ret)
					goto repair_abort;
				fixed = 1;
			}
			err = 1;
			cur_err = 1;
		}
		if (rec->bad_full_backref) {
			fprintf(stderr, "bad full backref, on [%llu]\n",
				(unsigned long long)rec->start);
			if (repair) {
				ret = fixup_extent_flags(root->fs_info, rec);
				if (ret)
					goto repair_abort;
				fixed = 1;
			}
			err = 1;
			cur_err = 1;
		}
		/*
		 * Although it's not a extent ref's problem, we reuse this
		 * routine for error reporting.
		 * No repair function yet.
		 */
		if (rec->crossing_stripes) {
			fprintf(stderr,
				"bad metadata [%llu, %llu) crossing stripe boundary\n",
				rec->start, rec->start + rec->max_size);
			err = 1;
			cur_err = 1;
		}

		if (rec->wrong_chunk_type) {
			fprintf(stderr,
				"bad extent [%llu, %llu), type mismatch with chunk\n",
				rec->start, rec->start + rec->max_size);
			err = 1;
			cur_err = 1;
		}

		remove_cache_extent(extent_cache, cache);
		free_all_extent_backrefs(rec);
		if (!init_extent_tree && repair && (!cur_err || fixed))
			clear_extent_dirty(root->fs_info->excluded_extents,
					   rec->start,
					   rec->start + rec->max_size - 1,
					   GFP_NOFS);
		free(rec);
	}
repair_abort:
	if (repair) {
		if (ret && ret != -EAGAIN) {
			fprintf(stderr, "failed to repair damaged filesystem, aborting\n");
			exit(1);
		} else if (!ret) {
			struct btrfs_trans_handle *trans;

			root = root->fs_info->extent_root;
			trans = btrfs_start_transaction(root, 1);
			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				goto repair_abort;
			}

			btrfs_fix_block_accounting(trans, root);
			ret = btrfs_commit_transaction(trans, root);
			if (ret)
				goto repair_abort;
		}
		if (err)
			fprintf(stderr, "repaired damaged extent references\n");
		return ret;
	}
	return err;
}

u64 calc_stripe_length(u64 type, u64 length, int num_stripes)
{
	u64 stripe_size;

	if (type & BTRFS_BLOCK_GROUP_RAID0) {
		stripe_size = length;
		stripe_size /= num_stripes;
	} else if (type & BTRFS_BLOCK_GROUP_RAID10) {
		stripe_size = length * 2;
		stripe_size /= num_stripes;
	} else if (type & BTRFS_BLOCK_GROUP_RAID5) {
		stripe_size = length;
		stripe_size /= (num_stripes - 1);
	} else if (type & BTRFS_BLOCK_GROUP_RAID6) {
		stripe_size = length;
		stripe_size /= (num_stripes - 2);
	} else {
		stripe_size = length;
	}
	return stripe_size;
}

/*
 * Check the chunk with its block group/dev list ref:
 * Return 0 if all refs seems valid.
 * Return 1 if part of refs seems valid, need later check for rebuild ref
 * like missing block group and needs to search extent tree to rebuild them.
 * Return -1 if essential refs are missing and unable to rebuild.
 */
static int check_chunk_refs(struct chunk_record *chunk_rec,
			    struct block_group_tree *block_group_cache,
			    struct device_extent_tree *dev_extent_cache,
			    int silent)
{
	struct cache_extent *block_group_item;
	struct block_group_record *block_group_rec;
	struct cache_extent *dev_extent_item;
	struct device_extent_record *dev_extent_rec;
	u64 devid;
	u64 offset;
	u64 length;
	int metadump_v2 = 0;
	int i;
	int ret = 0;

	block_group_item = lookup_cache_extent(&block_group_cache->tree,
					       chunk_rec->offset,
					       chunk_rec->length);
	if (block_group_item) {
		block_group_rec = container_of(block_group_item,
					       struct block_group_record,
					       cache);
		if (chunk_rec->length != block_group_rec->offset ||
		    chunk_rec->offset != block_group_rec->objectid ||
		    (!metadump_v2 &&
		     chunk_rec->type_flags != block_group_rec->flags)) {
			if (!silent)
				fprintf(stderr,
					"Chunk[%llu, %u, %llu]: length(%llu), offset(%llu), type(%llu) mismatch with block group[%llu, %u, %llu]: offset(%llu), objectid(%llu), flags(%llu)\n",
					chunk_rec->objectid,
					chunk_rec->type,
					chunk_rec->offset,
					chunk_rec->length,
					chunk_rec->offset,
					chunk_rec->type_flags,
					block_group_rec->objectid,
					block_group_rec->type,
					block_group_rec->offset,
					block_group_rec->offset,
					block_group_rec->objectid,
					block_group_rec->flags);
			ret = -1;
		} else {
			list_del_init(&block_group_rec->list);
			chunk_rec->bg_rec = block_group_rec;
		}
	} else {
		if (!silent)
			fprintf(stderr,
				"Chunk[%llu, %u, %llu]: length(%llu), offset(%llu), type(%llu) is not found in block group\n",
				chunk_rec->objectid,
				chunk_rec->type,
				chunk_rec->offset,
				chunk_rec->length,
				chunk_rec->offset,
				chunk_rec->type_flags);
		ret = 1;
	}

	if (metadump_v2)
		return ret;

	length = calc_stripe_length(chunk_rec->type_flags, chunk_rec->length,
				    chunk_rec->num_stripes);
	for (i = 0; i < chunk_rec->num_stripes; ++i) {
		devid = chunk_rec->stripes[i].devid;
		offset = chunk_rec->stripes[i].offset;
		dev_extent_item = lookup_cache_extent2(&dev_extent_cache->tree,
						       devid, offset, length);
		if (dev_extent_item) {
			dev_extent_rec = container_of(dev_extent_item,
						struct device_extent_record,
						cache);
			if (dev_extent_rec->objectid != devid ||
			    dev_extent_rec->offset != offset ||
			    dev_extent_rec->chunk_offset != chunk_rec->offset ||
			    dev_extent_rec->length != length) {
				if (!silent)
					fprintf(stderr,
						"Chunk[%llu, %u, %llu] stripe[%llu, %llu] dismatch dev extent[%llu, %llu, %llu]\n",
						chunk_rec->objectid,
						chunk_rec->type,
						chunk_rec->offset,
						chunk_rec->stripes[i].devid,
						chunk_rec->stripes[i].offset,
						dev_extent_rec->objectid,
						dev_extent_rec->offset,
						dev_extent_rec->length);
				ret = -1;
			} else {
				list_move(&dev_extent_rec->chunk_list,
					  &chunk_rec->dextents);
			}
		} else {
			if (!silent)
				fprintf(stderr,
					"Chunk[%llu, %u, %llu] stripe[%llu, %llu] is not found in dev extent\n",
					chunk_rec->objectid,
					chunk_rec->type,
					chunk_rec->offset,
					chunk_rec->stripes[i].devid,
					chunk_rec->stripes[i].offset);
			ret = -1;
		}
	}
	return ret;
}

/* check btrfs_chunk -> btrfs_dev_extent / btrfs_block_group_item */
int check_chunks(struct cache_tree *chunk_cache,
		 struct block_group_tree *block_group_cache,
		 struct device_extent_tree *dev_extent_cache,
		 struct list_head *good, struct list_head *bad,
		 struct list_head *rebuild, int silent)
{
	struct cache_extent *chunk_item;
	struct chunk_record *chunk_rec;
	struct block_group_record *bg_rec;
	struct device_extent_record *dext_rec;
	int err;
	int ret = 0;

	chunk_item = first_cache_extent(chunk_cache);
	while (chunk_item) {
		chunk_rec = container_of(chunk_item, struct chunk_record,
					 cache);
		err = check_chunk_refs(chunk_rec, block_group_cache,
				       dev_extent_cache, silent);
		if (err < 0)
			ret = err;
		if (err == 0 && good)
			list_add_tail(&chunk_rec->list, good);
		if (err > 0 && rebuild)
			list_add_tail(&chunk_rec->list, rebuild);
		if (err < 0 && bad)
			list_add_tail(&chunk_rec->list, bad);
		chunk_item = next_cache_extent(chunk_item);
	}

	list_for_each_entry(bg_rec, &block_group_cache->block_groups, list) {
		if (!silent)
			fprintf(stderr,
				"Block group[%llu, %llu] (flags = %llu) didn't find the relative chunk.\n",
				bg_rec->objectid,
				bg_rec->offset,
				bg_rec->flags);
		if (!ret)
			ret = 1;
	}

	list_for_each_entry(dext_rec, &dev_extent_cache->no_chunk_orphans,
			    chunk_list) {
		if (!silent)
			fprintf(stderr,
				"Device extent[%llu, %llu, %llu] didn't find the relative chunk.\n",
				dext_rec->objectid,
				dext_rec->offset,
				dext_rec->length);
		if (!ret)
			ret = 1;
	}
	return ret;
}


static int check_device_used(struct device_record *dev_rec,
			     struct device_extent_tree *dext_cache)
{
	struct cache_extent *cache;
	struct device_extent_record *dev_extent_rec;
	u64 total_byte = 0;

	cache = search_cache_extent2(&dext_cache->tree, dev_rec->devid, 0);
	while (cache) {
		dev_extent_rec = container_of(cache,
					      struct device_extent_record,
					      cache);
		if (dev_extent_rec->objectid != dev_rec->devid)
			break;

		list_del_init(&dev_extent_rec->device_list);
		total_byte += dev_extent_rec->length;
		cache = next_cache_extent(cache);
	}

	if (total_byte != dev_rec->byte_used) {
		fprintf(stderr,
			"Dev extent's total-byte(%llu) is not equal to byte-used(%llu) in dev[%llu, %u, %llu]\n",
			total_byte, dev_rec->byte_used,	dev_rec->objectid,
			dev_rec->type, dev_rec->offset);
		return -1;
	} else {
		return 0;
	}
}

/* check btrfs_dev_item -> btrfs_dev_extent */
static int check_devices(struct rb_root *dev_cache,
			 struct device_extent_tree *dev_extent_cache)
{
	struct rb_node *dev_node;
	struct device_record *dev_rec;
	struct device_extent_record *dext_rec;
	int err;
	int ret = 0;

	dev_node = rb_first(dev_cache);
	while (dev_node) {
		dev_rec = container_of(dev_node, struct device_record, node);
		err = check_device_used(dev_rec, dev_extent_cache);
		if (err)
			ret = err;

		dev_node = rb_next(dev_node);
	}
	list_for_each_entry(dext_rec, &dev_extent_cache->no_device_orphans,
			    device_list) {
		fprintf(stderr,
			"Device extent[%llu, %llu, %llu] didn't find its device.\n",
			dext_rec->objectid, dext_rec->offset, dext_rec->length);
		if (!ret)
			ret = 1;
	}
	return ret;
}

static int add_root_item_to_list(struct list_head *head,
				  u64 objectid, u64 bytenr, u64 last_snapshot,
				  u8 level, u8 drop_level,
				  int level_size, struct btrfs_key *drop_key)
{

	struct root_item_record *ri_rec;
	ri_rec = malloc(sizeof(*ri_rec));
	if (!ri_rec)
		return -ENOMEM;
	ri_rec->bytenr = bytenr;
	ri_rec->objectid = objectid;
	ri_rec->level = level;
	ri_rec->level_size = level_size;
	ri_rec->drop_level = drop_level;
	ri_rec->last_snapshot = last_snapshot;
	if (drop_key)
		memcpy(&ri_rec->drop_key, drop_key, sizeof(*drop_key));
	list_add_tail(&ri_rec->list, head);

	return 0;
}

static void free_root_item_list(struct list_head *list)
{
	struct root_item_record *ri_rec;

	while (!list_empty(list)) {
		ri_rec = list_first_entry(list, struct root_item_record,
					  list);
		list_del_init(&ri_rec->list);
		free(ri_rec);
	}
}

static int deal_root_from_list(struct list_head *list,
			       struct btrfs_root *root,
			       struct block_info *bits,
			       int bits_nr,
			       struct cache_tree *pending,
			       struct cache_tree *seen,
			       struct cache_tree *reada,
			       struct cache_tree *nodes,
			       struct cache_tree *extent_cache,
			       struct cache_tree *chunk_cache,
			       struct rb_root *dev_cache,
			       struct block_group_tree *block_group_cache,
			       struct device_extent_tree *dev_extent_cache)
{
	int ret = 0;
	u64 last;

	while (!list_empty(list)) {
		struct root_item_record *rec;
		struct extent_buffer *buf;
		rec = list_entry(list->next,
				 struct root_item_record, list);
		last = 0;
		buf = read_tree_block(root->fs_info->tree_root,
				      rec->bytenr, rec->level_size, 0);
		if (!extent_buffer_uptodate(buf)) {
			free_extent_buffer(buf);
			ret = -EIO;
			break;
		}
		add_root_to_pending(buf, extent_cache, pending,
				    seen, nodes, rec->objectid);
		/*
		 * To rebuild extent tree, we need deal with snapshot
		 * one by one, otherwise we deal with node firstly which
		 * can maximize readahead.
		 */
		while (1) {
			ret = run_next_block(root, bits, bits_nr, &last,
					     pending, seen, reada, nodes,
					     extent_cache, chunk_cache,
					     dev_cache, block_group_cache,
					     dev_extent_cache, rec);
			if (ret != 0)
				break;
		}
		free_extent_buffer(buf);
		list_del(&rec->list);
		free(rec);
		if (ret < 0)
			break;
	}
	while (ret >= 0) {
		ret = run_next_block(root, bits, bits_nr, &last, pending, seen,
				     reada, nodes, extent_cache, chunk_cache,
				     dev_cache, block_group_cache,
				     dev_extent_cache, NULL);
		if (ret != 0) {
			if (ret > 0)
				ret = 0;
			break;
		}
	}
	return ret;
}

static int check_chunks_and_extents(struct btrfs_root *root)
{
	struct rb_root dev_cache;
	struct cache_tree chunk_cache;
	struct block_group_tree block_group_cache;
	struct device_extent_tree dev_extent_cache;
	struct cache_tree extent_cache;
	struct cache_tree seen;
	struct cache_tree pending;
	struct cache_tree reada;
	struct cache_tree nodes;
	struct extent_io_tree excluded_extents;
	struct cache_tree corrupt_blocks;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	int ret, err = 0;
	struct block_info *bits;
	int bits_nr;
	struct extent_buffer *leaf;
	int slot;
	struct btrfs_root_item ri;
	struct list_head dropping_trees;
	struct list_head normal_trees;
	struct btrfs_root *root1;
	u64 objectid;
	u32 level_size;
	u8 level;

	dev_cache = RB_ROOT;
	cache_tree_init(&chunk_cache);
	block_group_tree_init(&block_group_cache);
	device_extent_tree_init(&dev_extent_cache);

	cache_tree_init(&extent_cache);
	cache_tree_init(&seen);
	cache_tree_init(&pending);
	cache_tree_init(&nodes);
	cache_tree_init(&reada);
	cache_tree_init(&corrupt_blocks);
	extent_io_tree_init(&excluded_extents);
	INIT_LIST_HEAD(&dropping_trees);
	INIT_LIST_HEAD(&normal_trees);

	if (repair) {
		root->fs_info->excluded_extents = &excluded_extents;
		root->fs_info->fsck_extent_cache = &extent_cache;
		root->fs_info->free_extent_hook = free_extent_hook;
		root->fs_info->corrupt_blocks = &corrupt_blocks;
	}

	bits_nr = 1024;
	bits = malloc(bits_nr * sizeof(struct block_info));
	if (!bits) {
		perror("malloc");
		exit(1);
	}

	if (ctx.progress_enabled) {
		ctx.tp = TASK_EXTENTS;
		task_start(ctx.info);
	}

again:
	root1 = root->fs_info->tree_root;
	level = btrfs_header_level(root1->node);
	ret = add_root_item_to_list(&normal_trees, root1->root_key.objectid,
				    root1->node->start, 0, level, 0,
				    btrfs_level_size(root1, level), NULL);
	if (ret < 0)
		goto out;
	root1 = root->fs_info->chunk_root;
	level = btrfs_header_level(root1->node);
	ret = add_root_item_to_list(&normal_trees, root1->root_key.objectid,
				    root1->node->start, 0, level, 0,
				    btrfs_level_size(root1, level), NULL);
	if (ret < 0)
		goto out;
	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	btrfs_set_key_type(&key, BTRFS_ROOT_ITEM_KEY);
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root,
					&key, &path, 0, 0);
	if (ret < 0)
		goto out;
	while(1) {
		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret != 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (btrfs_key_type(&found_key) == BTRFS_ROOT_ITEM_KEY) {
			unsigned long offset;
			u64 last_snapshot;

			offset = btrfs_item_ptr_offset(leaf, path.slots[0]);
			read_extent_buffer(leaf, &ri, offset, sizeof(ri));
			last_snapshot = btrfs_root_last_snapshot(&ri);
			if (btrfs_disk_key_objectid(&ri.drop_progress) == 0) {
				level = btrfs_root_level(&ri);
				level_size = btrfs_level_size(root, level);
				ret = add_root_item_to_list(&normal_trees,
						found_key.objectid,
						btrfs_root_bytenr(&ri),
						last_snapshot, level,
						0, level_size, NULL);
				if (ret < 0)
					goto out;
			} else {
				level = btrfs_root_level(&ri);
				level_size = btrfs_level_size(root, level);
				objectid = found_key.objectid;
				btrfs_disk_key_to_cpu(&found_key,
						      &ri.drop_progress);
				ret = add_root_item_to_list(&dropping_trees,
						objectid,
						btrfs_root_bytenr(&ri),
						last_snapshot, level,
						ri.drop_level,
						level_size, &found_key);
				if (ret < 0)
					goto out;
			}
		}
		path.slots[0]++;
	}
	btrfs_release_path(&path);

	/*
	 * check_block can return -EAGAIN if it fixes something, please keep
	 * this in mind when dealing with return values from these functions, if
	 * we get -EAGAIN we want to fall through and restart the loop.
	 */
	ret = deal_root_from_list(&normal_trees, root, bits, bits_nr, &pending,
				  &seen, &reada, &nodes, &extent_cache,
				  &chunk_cache, &dev_cache, &block_group_cache,
				  &dev_extent_cache);
	if (ret < 0) {
		if (ret == -EAGAIN)
			goto loop;
		goto out;
	}
	ret = deal_root_from_list(&dropping_trees, root, bits, bits_nr,
				  &pending, &seen, &reada, &nodes,
				  &extent_cache, &chunk_cache, &dev_cache,
				  &block_group_cache, &dev_extent_cache);
	if (ret < 0) {
		if (ret == -EAGAIN)
			goto loop;
		goto out;
	}

	err = check_chunks(&chunk_cache, &block_group_cache,
			   &dev_extent_cache, NULL, NULL, NULL, 0);
	if (err) {
		if (err == -EAGAIN)
			goto loop;
		if (!ret)
			ret = err;
	}

	ret = check_extent_refs(root, &extent_cache);
	if (ret < 0) {
		if (ret == -EAGAIN)
			goto loop;
		goto out;
	}

	err = check_devices(&dev_cache, &dev_extent_cache);
	if (err && !ret)
		ret = err;

out:
	task_stop(ctx.info);
	if (repair) {
		free_corrupt_blocks_tree(root->fs_info->corrupt_blocks);
		extent_io_tree_cleanup(&excluded_extents);
		root->fs_info->fsck_extent_cache = NULL;
		root->fs_info->free_extent_hook = NULL;
		root->fs_info->corrupt_blocks = NULL;
		root->fs_info->excluded_extents = NULL;
	}
	free(bits);
	free_chunk_cache_tree(&chunk_cache);
	free_device_cache_tree(&dev_cache);
	free_block_group_tree(&block_group_cache);
	free_device_extent_tree(&dev_extent_cache);
	free_extent_cache_tree(&seen);
	free_extent_cache_tree(&pending);
	free_extent_cache_tree(&reada);
	free_extent_cache_tree(&nodes);
	return ret;
loop:
	free_corrupt_blocks_tree(root->fs_info->corrupt_blocks);
	free_extent_cache_tree(&seen);
	free_extent_cache_tree(&pending);
	free_extent_cache_tree(&reada);
	free_extent_cache_tree(&nodes);
	free_chunk_cache_tree(&chunk_cache);
	free_block_group_tree(&block_group_cache);
	free_device_cache_tree(&dev_cache);
	free_device_extent_tree(&dev_extent_cache);
	free_extent_record_cache(root->fs_info, &extent_cache);
	free_root_item_list(&normal_trees);
	free_root_item_list(&dropping_trees);
	extent_io_tree_cleanup(&excluded_extents);
	goto again;
}

static int btrfs_fsck_reinit_root(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root, int overwrite)
{
	struct extent_buffer *c;
	struct extent_buffer *old = root->node;
	int level;
	int ret;
	struct btrfs_disk_key disk_key = {0,0,0};

	level = 0;

	if (overwrite) {
		c = old;
		extent_buffer_get(c);
		goto init;
	}
	c = btrfs_alloc_free_block(trans, root,
				   btrfs_level_size(root, 0),
				   root->root_key.objectid,
				   &disk_key, level, 0, 0);
	if (IS_ERR(c)) {
		c = old;
		extent_buffer_get(c);
		overwrite = 1;
	}
init:
	memset_extent_buffer(c, 0, 0, sizeof(struct btrfs_header));
	btrfs_set_header_level(c, level);
	btrfs_set_header_bytenr(c, c->start);
	btrfs_set_header_generation(c, trans->transid);
	btrfs_set_header_backref_rev(c, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(c, root->root_key.objectid);

	write_extent_buffer(c, root->fs_info->fsid,
			    btrfs_header_fsid(), BTRFS_FSID_SIZE);

	write_extent_buffer(c, root->fs_info->chunk_tree_uuid,
			    btrfs_header_chunk_tree_uuid(c),
			    BTRFS_UUID_SIZE);

	btrfs_mark_buffer_dirty(c);
	/*
	 * this case can happen in the following case:
	 *
	 * 1.overwrite previous root.
	 *
	 * 2.reinit reloc data root, this is because we skip pin
	 * down reloc data tree before which means we can allocate
	 * same block bytenr here.
	 */
	if (old->start == c->start) {
		btrfs_set_root_generation(&root->root_item,
					  trans->transid);
		root->root_item.level = btrfs_header_level(root->node);
		ret = btrfs_update_root(trans, root->fs_info->tree_root,
					&root->root_key, &root->root_item);
		if (ret) {
			free_extent_buffer(c);
			return ret;
		}
	}
	free_extent_buffer(old);
	root->node = c;
	add_root_to_dirty_list(root);
	return 0;
}

static int pin_down_tree_blocks(struct btrfs_fs_info *fs_info,
				struct extent_buffer *eb, int tree_root)
{
	struct extent_buffer *tmp;
	struct btrfs_root_item *ri;
	struct btrfs_key key;
	u64 bytenr;
	u32 leafsize;
	int level = btrfs_header_level(eb);
	int nritems;
	int ret;
	int i;

	/*
	 * If we have pinned this block before, don't pin it again.
	 * This can not only avoid forever loop with broken filesystem
	 * but also give us some speedups.
	 */
	if (test_range_bit(&fs_info->pinned_extents, eb->start,
			   eb->start + eb->len - 1, EXTENT_DIRTY, 0))
		return 0;

	btrfs_pin_extent(fs_info, eb->start, eb->len);

	leafsize = btrfs_super_leafsize(fs_info->super_copy);
	nritems = btrfs_header_nritems(eb);
	for (i = 0; i < nritems; i++) {
		if (level == 0) {
			btrfs_item_key_to_cpu(eb, &key, i);
			if (key.type != BTRFS_ROOT_ITEM_KEY)
				continue;
			/* Skip the extent root and reloc roots */
			if (key.objectid == BTRFS_EXTENT_TREE_OBJECTID ||
			    key.objectid == BTRFS_TREE_RELOC_OBJECTID ||
			    key.objectid == BTRFS_DATA_RELOC_TREE_OBJECTID)
				continue;
			ri = btrfs_item_ptr(eb, i, struct btrfs_root_item);
			bytenr = btrfs_disk_root_bytenr(eb, ri);

			/*
			 * If at any point we start needing the real root we
			 * will have to build a stump root for the root we are
			 * in, but for now this doesn't actually use the root so
			 * just pass in extent_root.
			 */
			tmp = read_tree_block(fs_info->extent_root, bytenr,
					      leafsize, 0);
			if (!extent_buffer_uptodate(tmp)) {
				fprintf(stderr, "Error reading root block\n");
				return -EIO;
			}
			ret = pin_down_tree_blocks(fs_info, tmp, 0);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		} else {
			bytenr = btrfs_node_blockptr(eb, i);

			/* If we aren't the tree root don't read the block */
			if (level == 1 && !tree_root) {
				btrfs_pin_extent(fs_info, bytenr, leafsize);
				continue;
			}

			tmp = read_tree_block(fs_info->extent_root, bytenr,
					      leafsize, 0);
			if (!extent_buffer_uptodate(tmp)) {
				fprintf(stderr, "Error reading tree block\n");
				return -EIO;
			}
			ret = pin_down_tree_blocks(fs_info, tmp, tree_root);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int pin_metadata_blocks(struct btrfs_fs_info *fs_info)
{
	int ret;

	ret = pin_down_tree_blocks(fs_info, fs_info->chunk_root->node, 0);
	if (ret)
		return ret;

	return pin_down_tree_blocks(fs_info, fs_info->tree_root->node, 1);
}

static int reset_block_groups(struct btrfs_fs_info *fs_info)
{
	struct btrfs_block_group_cache *cache;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_chunk *chunk;
	struct btrfs_key key;
	int ret;
	u64 start;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = 0;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, fs_info->chunk_root, &key, path, 0, 0);
	if (ret < 0) {
		btrfs_free_path(path);
		return ret;
	}

	/*
	 * We do this in case the block groups were screwed up and had alloc
	 * bits that aren't actually set on the chunks.  This happens with
	 * restored images every time and could happen in real life I guess.
	 */
	fs_info->avail_data_alloc_bits = 0;
	fs_info->avail_metadata_alloc_bits = 0;
	fs_info->avail_system_alloc_bits = 0;

	/* First we need to create the in-memory block groups */
	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(fs_info->chunk_root, path);
			if (ret < 0) {
				btrfs_free_path(path);
				return ret;
			}
			if (ret) {
				ret = 0;
				break;
			}
		}
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type != BTRFS_CHUNK_ITEM_KEY) {
			path->slots[0]++;
			continue;
		}

		chunk = btrfs_item_ptr(leaf, path->slots[0],
				       struct btrfs_chunk);
		btrfs_add_block_group(fs_info, 0,
				      btrfs_chunk_type(leaf, chunk),
				      key.objectid, key.offset,
				      btrfs_chunk_length(leaf, chunk));
		set_extent_dirty(&fs_info->free_space_cache, key.offset,
				 key.offset + btrfs_chunk_length(leaf, chunk),
				 GFP_NOFS);
		path->slots[0]++;
	}
	start = 0;
	while (1) {
		cache = btrfs_lookup_first_block_group(fs_info, start);
		if (!cache)
			break;
		cache->cached = 1;
		start = cache->key.objectid + cache->key.offset;
	}

	btrfs_free_path(path);
	return 0;
}

static int reset_balance(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root = fs_info->tree_root;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int del_slot, del_nr = 0;
	int ret;
	int found = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = BTRFS_BALANCE_OBJECTID;
	key.type = BTRFS_BALANCE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret) {
		if (ret > 0)
			ret = 0;
		if (!ret)
			goto reinit_data_reloc;
		else
			goto out;
	}

	ret = btrfs_del_item(trans, root, path);
	if (ret)
		goto out;
	btrfs_release_path(path);

	key.objectid = BTRFS_TREE_RELOC_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret < 0)
		goto out;
	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			if (!found)
				break;

			if (del_nr) {
				ret = btrfs_del_items(trans, root, path,
						      del_slot, del_nr);
				del_nr = 0;
				if (ret)
					goto out;
			}
			key.offset++;
			btrfs_release_path(path);

			found = 0;
			ret = btrfs_search_slot(trans, root, &key, path,
						-1, 1);
			if (ret < 0)
				goto out;
			continue;
		}
		found = 1;
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid > BTRFS_TREE_RELOC_OBJECTID)
			break;
		if (key.objectid != BTRFS_TREE_RELOC_OBJECTID) {
			path->slots[0]++;
			continue;
		}
		if (!del_nr) {
			del_slot = path->slots[0];
			del_nr = 1;
		} else {
			del_nr++;
		}
		path->slots[0]++;
	}

	if (del_nr) {
		ret = btrfs_del_items(trans, root, path, del_slot, del_nr);
		if (ret)
			goto out;
	}
	btrfs_release_path(path);

reinit_data_reloc:
	key.objectid = BTRFS_DATA_RELOC_TREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root)) {
		fprintf(stderr, "Error reading data reloc tree\n");
		ret = PTR_ERR(root);
		goto out;
	}
	record_root_in_trans(trans, root);
	ret = btrfs_fsck_reinit_root(trans, root, 0);
	if (ret)
		goto out;
	ret = btrfs_make_root_dir(trans, root, BTRFS_FIRST_FREE_OBJECTID);
out:
	btrfs_free_path(path);
	return ret;
}

static int reinit_extent_tree(struct btrfs_trans_handle *trans,
			      struct btrfs_fs_info *fs_info)
{
	u64 start = 0;
	int ret;

	/*
	 * The only reason we don't do this is because right now we're just
	 * walking the trees we find and pinning down their bytes, we don't look
	 * at any of the leaves.  In order to do mixed groups we'd have to check
	 * the leaves of any fs roots and pin down the bytes for any file
	 * extents we find.  Not hard but why do it if we don't have to?
	 */
	if (btrfs_fs_incompat(fs_info, BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS)) {
		fprintf(stderr, "We don't support re-initing the extent tree "
			"for mixed block groups yet, please notify a btrfs "
			"developer you want to do this so they can add this "
			"functionality.\n");
		return -EINVAL;
	}

	/*
	 * first we need to walk all of the trees except the extent tree and pin
	 * down the bytes that are in use so we don't overwrite any existing
	 * metadata.
	 */
	ret = pin_metadata_blocks(fs_info);
	if (ret) {
		fprintf(stderr, "error pinning down used bytes\n");
		return ret;
	}

	/*
	 * Need to drop all the block groups since we're going to recreate all
	 * of them again.
	 */
	btrfs_free_block_groups(fs_info);
	ret = reset_block_groups(fs_info);
	if (ret) {
		fprintf(stderr, "error resetting the block groups\n");
		return ret;
	}

	/* Ok we can allocate now, reinit the extent root */
	ret = btrfs_fsck_reinit_root(trans, fs_info->extent_root, 0);
	if (ret) {
		fprintf(stderr, "extent root initialization failed\n");
		/*
		 * When the transaction code is updated we should end the
		 * transaction, but for now progs only knows about commit so
		 * just return an error.
		 */
		return ret;
	}

	/*
	 * Now we have all the in-memory block groups setup so we can make
	 * allocations properly, and the metadata we care about is safe since we
	 * pinned all of it above.
	 */
	while (1) {
		struct btrfs_block_group_cache *cache;

		cache = btrfs_lookup_first_block_group(fs_info, start);
		if (!cache)
			break;
		start = cache->key.objectid + cache->key.offset;
		ret = btrfs_insert_item(trans, fs_info->extent_root,
					&cache->key, &cache->item,
					sizeof(cache->item));
		if (ret) {
			fprintf(stderr, "Error adding block group\n");
			return ret;
		}
		btrfs_extent_post_op(trans, fs_info->extent_root);
	}

	ret = reset_balance(trans, fs_info);
	if (ret)
		fprintf(stderr, "error reseting the pending balance\n");

	return ret;
}

static int recow_extent_buffer(struct btrfs_root *root, struct extent_buffer *eb)
{
	struct btrfs_path *path;
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	int ret;

	printf("Recowing metadata block %llu\n", eb->start);
	key.objectid = btrfs_header_owner(eb);
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	root = btrfs_read_fs_root(root->fs_info, &key);
	if (IS_ERR(root)) {
		fprintf(stderr, "Couldn't find owner root %llu\n",
			key.objectid);
		return PTR_ERR(root);
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		return PTR_ERR(trans);
	}

	path->lowest_level = btrfs_header_level(eb);
	if (path->lowest_level)
		btrfs_node_key_to_cpu(eb, &key, 0);
	else
		btrfs_item_key_to_cpu(eb, &key, 0);

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	btrfs_commit_transaction(trans, root);
	btrfs_free_path(path);
	return ret;
}

static int delete_bad_item(struct btrfs_root *root, struct bad_item *bad)
{
	struct btrfs_path *path;
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	int ret;

	printf("Deleting bad item [%llu,%u,%llu]\n", bad->key.objectid,
	       bad->key.type, bad->key.offset);
	key.objectid = bad->root_id;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	root = btrfs_read_fs_root(root->fs_info, &key);
	if (IS_ERR(root)) {
		fprintf(stderr, "Couldn't find owner root %llu\n",
			key.objectid);
		return PTR_ERR(root);
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		return PTR_ERR(trans);
	}

	ret = btrfs_search_slot(trans, root, &bad->key, path, -1, 1);
	if (ret) {
		if (ret > 0)
			ret = 0;
		goto out;
	}
	ret = btrfs_del_item(trans, root, path);
out:
	btrfs_commit_transaction(trans, root);
	btrfs_free_path(path);
	return ret;
}

static int zero_log_tree(struct btrfs_root *root)
{
	struct btrfs_trans_handle *trans;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		return ret;
	}
	btrfs_set_super_log_root(root->fs_info->super_copy, 0);
	btrfs_set_super_log_root_level(root->fs_info->super_copy, 0);
	ret = btrfs_commit_transaction(trans, root);
	return ret;
}

static int populate_csum(struct btrfs_trans_handle *trans,
			 struct btrfs_root *csum_root, char *buf, u64 start,
			 u64 len)
{
	u64 offset = 0;
	u64 sectorsize;
	int ret = 0;

	while (offset < len) {
		sectorsize = csum_root->sectorsize;
		ret = read_extent_data(csum_root, buf, start + offset,
				       &sectorsize, 0);
		if (ret)
			break;
		ret = btrfs_csum_file_block(trans, csum_root, start + len,
					    start + offset, buf, sectorsize);
		if (ret)
			break;
		offset += sectorsize;
	}
	return ret;
}

static int fill_csum_tree_from_one_fs_root(struct btrfs_trans_handle *trans,
				      struct btrfs_root *csum_root,
				      struct btrfs_root *cur_root)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct extent_buffer *node;
	struct btrfs_file_extent_item *fi;
	char *buf = NULL;
	u64 start = 0;
	u64 len = 0;
	int slot = 0;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	buf = malloc(cur_root->fs_info->csum_root->sectorsize);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	key.objectid = 0;
	key.offset = 0;
	key.type = 0;

	ret = btrfs_search_slot(NULL, cur_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	/* Iterate all regular file extents and fill its csum */
	while (1) {
		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

		if (key.type != BTRFS_EXTENT_DATA_KEY)
			goto next;
		node = path->nodes[0];
		slot = path->slots[0];
		fi = btrfs_item_ptr(node, slot, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(node, fi) != BTRFS_FILE_EXTENT_REG)
			goto next;
		start = btrfs_file_extent_disk_bytenr(node, fi);
		len = btrfs_file_extent_disk_num_bytes(node, fi);

		ret = populate_csum(trans, csum_root, buf, start, len);
		if (ret == -EEXIST)
			ret = 0;
		if (ret < 0)
			goto out;
next:
		/*
		 * TODO: if next leaf is corrupted, jump to nearest next valid
		 * leaf.
		 */
		ret = btrfs_next_item(cur_root, path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}

out:
	btrfs_free_path(path);
	free(buf);
	return ret;
}

static int fill_csum_tree_from_fs(struct btrfs_trans_handle *trans,
				  struct btrfs_root *csum_root)
{
	struct btrfs_fs_info *fs_info = csum_root->fs_info;
	struct btrfs_path *path;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *cur_root;
	struct extent_buffer *node;
	struct btrfs_key key;
	int slot = 0;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.offset = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;

	ret = btrfs_search_slot(NULL, tree_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	while (1) {
		node = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);
		if (key.objectid > BTRFS_LAST_FREE_OBJECTID)
			goto out;
		if (key.type != BTRFS_ROOT_ITEM_KEY)
			goto next;
		if (!is_fstree(key.objectid))
			goto next;
		key.offset = (u64)-1;

		cur_root = btrfs_read_fs_root(fs_info, &key);
		if (IS_ERR(cur_root) || !cur_root) {
			fprintf(stderr, "Fail to read fs/subvol tree: %lld\n",
				key.objectid);
			goto out;
		}
		ret = fill_csum_tree_from_one_fs_root(trans, csum_root,
				cur_root);
		if (ret < 0)
			goto out;
next:
		ret = btrfs_next_item(tree_root, path);
		if (ret > 0) {
			ret = 0;
			goto out;
		}
		if (ret < 0)
			goto out;
	}

out:
	btrfs_free_path(path);
	return ret;
}

static int fill_csum_tree_from_extent(struct btrfs_trans_handle *trans,
				      struct btrfs_root *csum_root)
{
	struct btrfs_root *extent_root = csum_root->fs_info->extent_root;
	struct btrfs_path *path;
	struct btrfs_extent_item *ei;
	struct extent_buffer *leaf;
	char *buf;
	struct btrfs_key key;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0) {
		btrfs_free_path(path);
		return ret;
	}

	buf = malloc(csum_root->sectorsize);
	if (!buf) {
		btrfs_free_path(path);
		return -ENOMEM;
	}

	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(extent_root, path);
			if (ret < 0)
				break;
			if (ret) {
				ret = 0;
				break;
			}
		}
		leaf = path->nodes[0];

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY) {
			path->slots[0]++;
			continue;
		}

		ei = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_extent_item);
		if (!(btrfs_extent_flags(leaf, ei) &
		      BTRFS_EXTENT_FLAG_DATA)) {
			path->slots[0]++;
			continue;
		}

		ret = populate_csum(trans, csum_root, buf, key.objectid,
				    key.offset);
		if (ret)
			break;
		path->slots[0]++;
	}

	btrfs_free_path(path);
	free(buf);
	return ret;
}

/*
 * Recalculate the csum and put it into the csum tree.
 *
 * Extent tree init will wipe out all the extent info, so in that case, we
 * can't depend on extent tree, but use fs tree.  If search_fs_tree is set, we
 * will use fs/subvol trees to init the csum tree.
 */
static int fill_csum_tree(struct btrfs_trans_handle *trans,
			  struct btrfs_root *csum_root,
			  int search_fs_tree)
{
	if (search_fs_tree)
		return fill_csum_tree_from_fs(trans, csum_root);
	else
		return fill_csum_tree_from_extent(trans, csum_root);
}

struct root_item_info {
	/* level of the root */
	u8 level;
	/* number of nodes at this level, must be 1 for a root */
	int node_count;
	u64 bytenr;
	u64 gen;
	struct cache_extent cache_extent;
};

static struct cache_tree *roots_info_cache = NULL;

static void free_roots_info_cache(void)
{
	if (!roots_info_cache)
		return;

	while (!cache_tree_empty(roots_info_cache)) {
		struct cache_extent *entry;
		struct root_item_info *rii;

		entry = first_cache_extent(roots_info_cache);
		if (!entry)
			break;
		remove_cache_extent(roots_info_cache, entry);
		rii = container_of(entry, struct root_item_info, cache_extent);
		free(rii);
	}

	free(roots_info_cache);
	roots_info_cache = NULL;
}

static int build_roots_info_cache(struct btrfs_fs_info *info)
{
	int ret = 0;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_path *path;

	if (!roots_info_cache) {
		roots_info_cache = malloc(sizeof(*roots_info_cache));
		if (!roots_info_cache)
			return -ENOMEM;
		cache_tree_init(roots_info_cache);
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, info->extent_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	leaf = path->nodes[0];

	while (1) {
		struct btrfs_key found_key;
		struct btrfs_extent_item *ei;
		struct btrfs_extent_inline_ref *iref;
		int slot = path->slots[0];
		int type;
		u64 flags;
		u64 root_id;
		u8 level;
		struct cache_extent *entry;
		struct root_item_info *rii;

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(info->extent_root, path);
			if (ret < 0) {
				break;
			} else if (ret) {
				ret = 0;
				break;
			}
			leaf = path->nodes[0];
			slot = path->slots[0];
		}

		btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);

		if (found_key.type != BTRFS_EXTENT_ITEM_KEY &&
		    found_key.type != BTRFS_METADATA_ITEM_KEY)
			goto next;

		ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
		flags = btrfs_extent_flags(leaf, ei);

		if (found_key.type == BTRFS_EXTENT_ITEM_KEY &&
		    !(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK))
			goto next;

		if (found_key.type == BTRFS_METADATA_ITEM_KEY) {
			iref = (struct btrfs_extent_inline_ref *)(ei + 1);
			level = found_key.offset;
		} else {
			struct btrfs_tree_block_info *info;

			info = (struct btrfs_tree_block_info *)(ei + 1);
			iref = (struct btrfs_extent_inline_ref *)(info + 1);
			level = btrfs_tree_block_level(leaf, info);
		}

		/*
		 * For a root extent, it must be of the following type and the
		 * first (and only one) iref in the item.
		 */
		type = btrfs_extent_inline_ref_type(leaf, iref);
		if (type != BTRFS_TREE_BLOCK_REF_KEY)
			goto next;

		root_id = btrfs_extent_inline_ref_offset(leaf, iref);
		entry = lookup_cache_extent(roots_info_cache, root_id, 1);
		if (!entry) {
			rii = malloc(sizeof(struct root_item_info));
			if (!rii) {
				ret = -ENOMEM;
				goto out;
			}
			rii->cache_extent.start = root_id;
			rii->cache_extent.size = 1;
			rii->level = (u8)-1;
			entry = &rii->cache_extent;
			ret = insert_cache_extent(roots_info_cache, entry);
			ASSERT(ret == 0);
		} else {
			rii = container_of(entry, struct root_item_info,
					   cache_extent);
		}

		ASSERT(rii->cache_extent.start == root_id);
		ASSERT(rii->cache_extent.size == 1);

		if (level > rii->level || rii->level == (u8)-1) {
			rii->level = level;
			rii->bytenr = found_key.objectid;
			rii->gen = btrfs_extent_generation(leaf, ei);
			rii->node_count = 1;
		} else if (level == rii->level) {
			rii->node_count++;
		}
next:
		path->slots[0]++;
	}

out:
	btrfs_free_path(path);

	return ret;
}

static int maybe_repair_root_item(struct btrfs_fs_info *info,
				  struct btrfs_path *path,
				  const struct btrfs_key *root_key,
				  const int read_only_mode)
{
	const u64 root_id = root_key->objectid;
	struct cache_extent *entry;
	struct root_item_info *rii;
	struct btrfs_root_item ri;
	unsigned long offset;

	entry = lookup_cache_extent(roots_info_cache, root_id, 1);
	if (!entry) {
		fprintf(stderr,
			"Error: could not find extent items for root %llu\n",
			root_key->objectid);
		return -ENOENT;
	}

	rii = container_of(entry, struct root_item_info, cache_extent);
	ASSERT(rii->cache_extent.start == root_id);
	ASSERT(rii->cache_extent.size == 1);

	if (rii->node_count != 1) {
		fprintf(stderr,
			"Error: could not find btree root extent for root %llu\n",
			root_id);
		return -ENOENT;
	}

	offset = btrfs_item_ptr_offset(path->nodes[0], path->slots[0]);
	read_extent_buffer(path->nodes[0], &ri, offset, sizeof(ri));

	if (btrfs_root_bytenr(&ri) != rii->bytenr ||
	    btrfs_root_level(&ri) != rii->level ||
	    btrfs_root_generation(&ri) != rii->gen) {

		/*
		 * If we're in repair mode but our caller told us to not update
		 * the root item, i.e. just check if it needs to be updated, don't
		 * print this message, since the caller will call us again shortly
		 * for the same root item without read only mode (the caller will
		 * open a transaction first).
		 */
		if (!(read_only_mode && repair))
			fprintf(stderr,
				"%sroot item for root %llu,"
				" current bytenr %llu, current gen %llu, current level %u,"
				" new bytenr %llu, new gen %llu, new level %u\n",
				(read_only_mode ? "" : "fixing "),
				root_id,
				btrfs_root_bytenr(&ri), btrfs_root_generation(&ri),
				btrfs_root_level(&ri),
				rii->bytenr, rii->gen, rii->level);

		if (btrfs_root_generation(&ri) > rii->gen) {
			fprintf(stderr,
				"root %llu has a root item with a more recent gen (%llu) compared to the found root node (%llu)\n",
				root_id, btrfs_root_generation(&ri), rii->gen);
			return -EINVAL;
		}

		if (!read_only_mode) {
			btrfs_set_root_bytenr(&ri, rii->bytenr);
			btrfs_set_root_level(&ri, rii->level);
			btrfs_set_root_generation(&ri, rii->gen);
			write_extent_buffer(path->nodes[0], &ri,
					    offset, sizeof(ri));
		}

		return 1;
	}

	return 0;
}

/*
 * A regression introduced in the 3.17 kernel (more specifically in 3.17-rc2),
 * caused read-only snapshots to be corrupted if they were created at a moment
 * when the source subvolume/snapshot had orphan items. The issue was that the
 * on-disk root items became incorrect, referring to the pre orphan cleanup root
 * node instead of the post orphan cleanup root node.
 * So this function, and its callees, just detects and fixes those cases. Even
 * though the regression was for read-only snapshots, this function applies to
 * any snapshot/subvolume root.
 * This must be run before any other repair code - not doing it so, makes other
 * repair code delete or modify backrefs in the extent tree for example, which
 * will result in an inconsistent fs after repairing the root items.
 */
static int repair_root_items(struct btrfs_fs_info *info)
{
	struct btrfs_path *path = NULL;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_trans_handle *trans = NULL;
	int ret = 0;
	int bad_roots = 0;
	int need_trans = 0;

	ret = build_roots_info_cache(info);
	if (ret)
		goto out;

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;

again:
	/*
	 * Avoid opening and committing transactions if a leaf doesn't have
	 * any root items that need to be fixed, so that we avoid rotating
	 * backup roots unnecessarily.
	 */
	if (need_trans) {
		trans = btrfs_start_transaction(info->tree_root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			goto out;
		}
	}

	ret = btrfs_search_slot(trans, info->tree_root, &key, path,
				0, trans ? 1 : 0);
	if (ret < 0)
		goto out;
	leaf = path->nodes[0];

	while (1) {
		struct btrfs_key found_key;

		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			int no_more_keys = find_next_key(path, &key);

			btrfs_release_path(path);
			if (trans) {
				ret = btrfs_commit_transaction(trans,
							       info->tree_root);
				trans = NULL;
				if (ret < 0)
					goto out;
			}
			need_trans = 0;
			if (no_more_keys)
				break;
			goto again;
		}

		btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);

		if (found_key.type != BTRFS_ROOT_ITEM_KEY)
			goto next;
		if (found_key.objectid == BTRFS_TREE_RELOC_OBJECTID)
			goto next;

		ret = maybe_repair_root_item(info, path, &found_key,
					     trans ? 0 : 1);
		if (ret < 0)
			goto out;
		if (ret) {
			if (!trans && repair) {
				need_trans = 1;
				key = found_key;
				btrfs_release_path(path);
				goto again;
			}
			bad_roots++;
		}
next:
		path->slots[0]++;
	}
	ret = 0;
out:
	free_roots_info_cache();
	btrfs_free_path(path);
	if (trans)
		btrfs_commit_transaction(trans, info->tree_root);
	if (ret < 0)
		return ret;

	return bad_roots;
}

const char * const cmd_check_usage[] = {
	"btrfs check [options] <device>",
	"Check an unmounted btrfs filesystem.",
	"",
	"-s|--super <superblock>     use this superblock copy",
	"-b|--backup                 use the backup root copy",
	"--repair                    try to repair the filesystem",
	"--init-csum-tree            create a new CRC tree",
	"--init-extent-tree          create a new extent tree",
	"--check-data-csum           verify checkums of data blocks",
	"--qgroup-report             print a report on qgroup consistency",
	"--subvol-extents <subvolid> print subvolume extents and sharing state",
	"--tree-root <bytenr>        use the given bytenr for the tree root",
	"-p|--progress               indicate progress",
	NULL
};

int cmd_check(int argc, char **argv)
{
	struct cache_tree root_cache;
	struct btrfs_root *root;
	struct btrfs_fs_info *info;
	u64 bytenr = 0;
	u64 subvolid = 0;
	u64 tree_root_bytenr = 0;
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	int ret;
	u64 num;
	int init_csum_tree = 0;
	int readonly = 0;
	int qgroup_report = 0;
	enum btrfs_open_ctree_flags ctree_flags = OPEN_CTREE_EXCLUSIVE;

	while(1) {
		int c;
		enum { OPT_REPAIR = 257, OPT_INIT_CSUM, OPT_INIT_EXTENT,
			OPT_CHECK_CSUM, OPT_READONLY };
		static const struct option long_options[] = {
			{ "super", required_argument, NULL, 's' },
			{ "repair", no_argument, NULL, OPT_REPAIR },
			{ "readonly", no_argument, NULL, OPT_READONLY },
			{ "init-csum-tree", no_argument, NULL, OPT_INIT_CSUM },
			{ "init-extent-tree", no_argument, NULL, OPT_INIT_EXTENT },
			{ "check-data-csum", no_argument, NULL, OPT_CHECK_CSUM },
			{ "backup", no_argument, NULL, 'b' },
			{ "subvol-extents", required_argument, NULL, 'E' },
			{ "qgroup-report", no_argument, NULL, 'Q' },
			{ "tree-root", required_argument, NULL, 'r' },
			{ "progress", no_argument, NULL, 'p' },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "as:br:p", long_options, NULL);
		if (c < 0)
			break;
		switch(c) {
			case 'a': /* ignored */ break;
			case 'b':
				ctree_flags |= OPEN_CTREE_BACKUP_ROOT;
				break;
			case 's':
				num = arg_strtou64(optarg);
				if (num >= BTRFS_SUPER_MIRROR_MAX) {
					fprintf(stderr,
						"ERROR: super mirror should be less than: %d\n",
						BTRFS_SUPER_MIRROR_MAX);
					exit(1);
				}
				bytenr = btrfs_sb_offset(((int)num));
				printf("using SB copy %llu, bytenr %llu\n", num,
				       (unsigned long long)bytenr);
				break;
			case 'Q':
				qgroup_report = 1;
				break;
			case 'E':
				subvolid = arg_strtou64(optarg);
				break;
			case 'r':
				tree_root_bytenr = arg_strtou64(optarg);
				break;
			case 'p':
				ctx.progress_enabled = true;
				break;
			case '?':
			case 'h':
				usage(cmd_check_usage);
			case OPT_REPAIR:
				printf("enabling repair mode\n");
				repair = 1;
				ctree_flags |= OPEN_CTREE_WRITES;
				break;
			case OPT_READONLY:
				readonly = 1;
				break;
			case OPT_INIT_CSUM:
				printf("Creating a new CRC tree\n");
				init_csum_tree = 1;
				repair = 1;
				ctree_flags |= OPEN_CTREE_WRITES;
				break;
			case OPT_INIT_EXTENT:
				init_extent_tree = 1;
				ctree_flags |= (OPEN_CTREE_WRITES |
						OPEN_CTREE_NO_BLOCK_GROUPS);
				repair = 1;
				break;
			case OPT_CHECK_CSUM:
				check_data_csum = 1;
				break;
		}
	}
	argc = argc - optind;

	if (check_argc_exact(argc, 1))
		usage(cmd_check_usage);

	if (ctx.progress_enabled) {
		ctx.tp = TASK_NOTHING;
		ctx.info = task_init(print_status_check, print_status_return, &ctx);
	}

	/* This check is the only reason for --readonly to exist */
	if (readonly && repair) {
		fprintf(stderr, "Repair options are not compatible with --readonly\n");
		exit(1);
	}

	radix_tree_init();
	cache_tree_init(&root_cache);

	if((ret = check_mounted(argv[optind])) < 0) {
		fprintf(stderr, "Could not check mount status: %s\n", strerror(-ret));
		goto err_out;
	} else if(ret) {
		fprintf(stderr, "%s is currently mounted. Aborting.\n", argv[optind]);
		ret = -EBUSY;
		goto err_out;
	}

	/* only allow partial opening under repair mode */
	if (repair)
		ctree_flags |= OPEN_CTREE_PARTIAL;

	info = open_ctree_fs_info(argv[optind], bytenr, tree_root_bytenr,
				  ctree_flags);
	if (!info) {
		fprintf(stderr, "Couldn't open file system\n");
		ret = -EIO;
		goto err_out;
	}

	global_info = info;
	root = info->fs_root;

	/*
	 * repair mode will force us to commit transaction which
	 * will make us fail to load log tree when mounting.
	 */
	if (repair && btrfs_super_log_root(info->super_copy)) {
		ret = ask_user("repair mode will force to clear out log tree, Are you sure?");
		if (!ret) {
			ret = 1;
			goto close_out;
		}
		ret = zero_log_tree(root);
		if (ret) {
			fprintf(stderr, "fail to zero log tree\n");
			goto close_out;
		}
	}

	uuid_unparse(info->super_copy->fsid, uuidbuf);
	if (qgroup_report) {
		printf("Print quota groups for %s\nUUID: %s\n", argv[optind],
		       uuidbuf);
		ret = qgroup_verify_all(info);
		if (ret == 0)
			print_qgroup_report(1);
		goto close_out;
	}
	if (subvolid) {
		printf("Print extent state for subvolume %llu on %s\nUUID: %s\n",
		       subvolid, argv[optind], uuidbuf);
		ret = print_extent_state(info, subvolid);
		goto close_out;
	}
	printf("Checking filesystem on %s\nUUID: %s\n", argv[optind], uuidbuf);

	if (!extent_buffer_uptodate(info->tree_root->node) ||
	    !extent_buffer_uptodate(info->dev_root->node) ||
	    !extent_buffer_uptodate(info->chunk_root->node)) {
		fprintf(stderr, "Critical roots corrupted, unable to fsck the FS\n");
		ret = -EIO;
		goto close_out;
	}

	if (init_extent_tree || init_csum_tree) {
		struct btrfs_trans_handle *trans;

		trans = btrfs_start_transaction(info->extent_root, 0);
		if (IS_ERR(trans)) {
			fprintf(stderr, "Error starting transaction\n");
			ret = PTR_ERR(trans);
			goto close_out;
		}

		if (init_extent_tree) {
			printf("Creating a new extent tree\n");
			ret = reinit_extent_tree(trans, info);
			if (ret)
				goto close_out;
		}

		if (init_csum_tree) {
			fprintf(stderr, "Reinit crc root\n");
			ret = btrfs_fsck_reinit_root(trans, info->csum_root, 0);
			if (ret) {
				fprintf(stderr, "crc root initialization failed\n");
				ret = -EIO;
				goto close_out;
			}

			ret = fill_csum_tree(trans, info->csum_root,
					     init_extent_tree);
			if (ret) {
				fprintf(stderr, "crc refilling failed\n");
				return -EIO;
			}
		}
		/*
		 * Ok now we commit and run the normal fsck, which will add
		 * extent entries for all of the items it finds.
		 */
		ret = btrfs_commit_transaction(trans, info->extent_root);
		if (ret)
			goto close_out;
	}
	if (!extent_buffer_uptodate(info->extent_root->node)) {
		fprintf(stderr, "Critical roots corrupted, unable to fsck the FS\n");
		ret = -EIO;
		goto close_out;
	}
	if (!extent_buffer_uptodate(info->csum_root->node)) {
		fprintf(stderr, "Checksum root corrupted, rerun with --init-csum-tree option\n");
		ret = -EIO;
		goto close_out;
	}

	if (!ctx.progress_enabled)
		fprintf(stderr, "checking extents\n");
	ret = check_chunks_and_extents(root);
	if (ret)
		fprintf(stderr, "Errors found in extent allocation tree or chunk allocation\n");

	ret = repair_root_items(info);
	if (ret < 0)
		goto close_out;
	if (repair) {
		fprintf(stderr, "Fixed %d roots.\n", ret);
		ret = 0;
	} else if (ret > 0) {
		fprintf(stderr,
		       "Found %d roots with an outdated root item.\n",
		       ret);
		fprintf(stderr,
			"Please run a filesystem check with the option --repair to fix them.\n");
		ret = 1;
		goto close_out;
	}

	if (!ctx.progress_enabled)
		fprintf(stderr, "checking free space cache\n");
	ret = check_space_cache(root);
	if (ret)
		goto out;

	/*
	 * We used to have to have these hole extents in between our real
	 * extents so if we don't have this flag set we need to make sure there
	 * are no gaps in the file extents for inodes, otherwise we can just
	 * ignore it when this happens.
	 */
	no_holes = btrfs_fs_incompat(root->fs_info,
				     BTRFS_FEATURE_INCOMPAT_NO_HOLES);
	if (!ctx.progress_enabled)
		fprintf(stderr, "checking fs roots\n");
	ret = check_fs_roots(root, &root_cache);
	if (ret)
		goto out;

	fprintf(stderr, "checking csums\n");
	ret = check_csums(root);
	if (ret)
		goto out;

	fprintf(stderr, "checking root refs\n");
	ret = check_root_refs(root, &root_cache);
	if (ret)
		goto out;

	while (repair && !list_empty(&root->fs_info->recow_ebs)) {
		struct extent_buffer *eb;

		eb = list_first_entry(&root->fs_info->recow_ebs,
				      struct extent_buffer, recow);
		list_del_init(&eb->recow);
		ret = recow_extent_buffer(root, eb);
		if (ret)
			break;
	}

	while (!list_empty(&delete_items)) {
		struct bad_item *bad;

		bad = list_first_entry(&delete_items, struct bad_item, list);
		list_del_init(&bad->list);
		if (repair)
			ret = delete_bad_item(root, bad);
		free(bad);
	}

	if (info->quota_enabled) {
		int err;
		fprintf(stderr, "checking quota groups\n");
		err = qgroup_verify_all(info);
		if (err)
			goto out;
	}

	if (!list_empty(&root->fs_info->recow_ebs)) {
		fprintf(stderr, "Transid errors in file system\n");
		ret = 1;
	}
out:
	print_qgroup_report(0);
	if (found_old_backref) { /*
		 * there was a disk format change when mixed
		 * backref was in testing tree. The old format
		 * existed about one week.
		 */
		printf("\n * Found old mixed backref format. "
		       "The old format is not supported! *"
		       "\n * Please mount the FS in readonly mode, "
		       "backup data and re-format the FS. *\n\n");
		ret = 1;
	}
	printf("found %llu bytes used err is %d\n",
	       (unsigned long long)bytes_used, ret);
	printf("total csum bytes: %llu\n",(unsigned long long)total_csum_bytes);
	printf("total tree bytes: %llu\n",
	       (unsigned long long)total_btree_bytes);
	printf("total fs tree bytes: %llu\n",
	       (unsigned long long)total_fs_tree_bytes);
	printf("total extent tree bytes: %llu\n",
	       (unsigned long long)total_extent_tree_bytes);
	printf("btree space waste bytes: %llu\n",
	       (unsigned long long)btree_space_waste);
	printf("file data blocks allocated: %llu\n referenced %llu\n",
		(unsigned long long)data_bytes_allocated,
		(unsigned long long)data_bytes_referenced);
	printf("%s\n", PACKAGE_STRING);

	free_root_recs_tree(&root_cache);
close_out:
	close_ctree(root);
	btrfs_close_all_devices();
err_out:
	if (ctx.progress_enabled)
		task_deinit(ctx.info);

	return ret;
}
