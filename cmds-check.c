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
#include "free-space-tree.h"
#include "btrfsck.h"
#include "qgroup-verify.h"
#include "rbtree-utils.h"
#include "backref.h"
#include "kernel-shared/ulist.h"
#include "hash.h"
#include "help.h"

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
static LIST_HEAD(duplicate_extents);
static LIST_HEAD(delete_items);
static int no_holes = 0;
static int init_extent_tree = 0;
static int check_data_csum = 0;
static struct btrfs_fs_info *global_info;
static struct task_ctx ctx = { 0 };
static struct cache_tree *roots_info_cache = NULL;

enum btrfs_check_mode {
	CHECK_MODE_ORIGINAL,
	CHECK_MODE_LOWMEM,
	CHECK_MODE_UNKNOWN,
	CHECK_MODE_DEFAULT = CHECK_MODE_ORIGINAL
};

static enum btrfs_check_mode check_mode = CHECK_MODE_DEFAULT;

struct extent_backref {
	struct rb_node node;
	unsigned int is_data:1;
	unsigned int found_extent_tree:1;
	unsigned int full_backref:1;
	unsigned int found_ref:1;
	unsigned int broken:1;
};

static inline struct extent_backref* rb_node_to_extent_backref(struct rb_node *node)
{
	return rb_entry(node, struct extent_backref, node);
}

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

#define ROOT_DIR_ERROR		(1<<1)	/* bad ROOT_DIR */
#define DIR_ITEM_MISSING	(1<<2)	/* DIR_ITEM not found */
#define DIR_ITEM_MISMATCH	(1<<3)	/* DIR_ITEM found but not match */
#define INODE_REF_MISSING	(1<<4)	/* INODE_REF/INODE_EXTREF not found */
#define INODE_ITEM_MISSING	(1<<5)	/* INODE_ITEM not found */
#define INODE_ITEM_MISMATCH	(1<<6)	/* INODE_ITEM found but not match */
#define FILE_EXTENT_ERROR	(1<<7)	/* bad FILE_EXTENT */
#define ODD_CSUM_ITEM		(1<<8)	/* CSUM_ITEM error */
#define CSUM_ITEM_MISSING	(1<<9)	/* CSUM_ITEM not found */
#define LINK_COUNT_ERROR	(1<<10)	/* INODE_ITEM nlink count error */
#define NBYTES_ERROR		(1<<11)	/* INODE_ITEM nbytes count error */
#define ISIZE_ERROR		(1<<12)	/* INODE_ITEM size count error */
#define ORPHAN_ITEM		(1<<13) /* INODE_ITEM no reference */
#define NO_INODE_ITEM		(1<<14) /* no inode_item */
#define LAST_ITEM		(1<<15)	/* Complete this tree traversal */
#define ROOT_REF_MISSING	(1<<16)	/* ROOT_REF not found */
#define ROOT_REF_MISMATCH	(1<<17)	/* ROOT_REF found but not match */
#define DIR_INDEX_MISSING       (1<<18) /* INODE_INDEX not found */
#define DIR_INDEX_MISMATCH      (1<<19) /* INODE_INDEX found but not match */
#define DIR_COUNT_AGAIN         (1<<20) /* DIR isize should be recalculated */
#define BG_ACCOUNTING_ERROR     (1<<21) /* Block group accounting error */

static inline struct data_backref* to_data_backref(struct extent_backref *back)
{
	return container_of(back, struct data_backref, node);
}

static int compare_data_backref(struct rb_node *node1, struct rb_node *node2)
{
	struct extent_backref *ext1 = rb_node_to_extent_backref(node1);
	struct extent_backref *ext2 = rb_node_to_extent_backref(node2);
	struct data_backref *back1 = to_data_backref(ext1);
	struct data_backref *back2 = to_data_backref(ext2);

	WARN_ON(!ext1->is_data);
	WARN_ON(!ext2->is_data);

	/* parent and root are a union, so this covers both */
	if (back1->parent > back2->parent)
		return 1;
	if (back1->parent < back2->parent)
		return -1;

	/* This is a full backref and the parents match. */
	if (back1->node.full_backref)
		return 0;

	if (back1->owner > back2->owner)
		return 1;
	if (back1->owner < back2->owner)
		return -1;

	if (back1->offset > back2->offset)
		return 1;
	if (back1->offset < back2->offset)
		return -1;

	if (back1->found_ref && back2->found_ref) {
		if (back1->disk_bytenr > back2->disk_bytenr)
			return 1;
		if (back1->disk_bytenr < back2->disk_bytenr)
			return -1;

		if (back1->bytes > back2->bytes)
			return 1;
		if (back1->bytes < back2->bytes)
			return -1;
	}

	return 0;
}

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

static inline struct tree_backref* to_tree_backref(struct extent_backref *back)
{
	return container_of(back, struct tree_backref, node);
}

static int compare_tree_backref(struct rb_node *node1, struct rb_node *node2)
{
	struct extent_backref *ext1 = rb_node_to_extent_backref(node1);
	struct extent_backref *ext2 = rb_node_to_extent_backref(node2);
	struct tree_backref *back1 = to_tree_backref(ext1);
	struct tree_backref *back2 = to_tree_backref(ext2);

	WARN_ON(ext1->is_data);
	WARN_ON(ext2->is_data);

	/* parent and root are a union, so this covers both */
	if (back1->parent > back2->parent)
		return 1;
	if (back1->parent < back2->parent)
		return -1;

	return 0;
}

static int compare_extent_backref(struct rb_node *node1, struct rb_node *node2)
{
	struct extent_backref *ext1 = rb_node_to_extent_backref(node1);
	struct extent_backref *ext2 = rb_node_to_extent_backref(node2);

	if (ext1->is_data > ext2->is_data)
		return 1;

	if (ext1->is_data < ext2->is_data)
		return -1;

	if (ext1->full_backref > ext2->full_backref)
		return 1;
	if (ext1->full_backref < ext2->full_backref)
		return -1;

	if (ext1->is_data)
		return compare_data_backref(node1, node2);
	else
		return compare_tree_backref(node1, node2);
}

/* Explicit initialization for extent_record::flag_block_full_backref */
enum { FLAG_UNSET = 2 };

struct extent_record {
	struct list_head backrefs;
	struct list_head dups;
	struct rb_root backref_tree;
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
	unsigned int flag_block_full_backref:2;
	unsigned int found_rec:1;
	unsigned int content_checked:1;
	unsigned int owner_ref_checked:1;
	unsigned int is_root:1;
	unsigned int metadata:1;
	unsigned int bad_full_backref:1;
	unsigned int crossing_stripes:1;
	unsigned int wrong_chunk_type:1;
};

static inline struct extent_record* to_extent_record(struct list_head *entry)
{
	return container_of(entry, struct extent_record, list);
}

struct inode_backref {
	struct list_head list;
	unsigned int found_dir_item:1;
	unsigned int found_dir_index:1;
	unsigned int found_inode_ref:1;
	u8 filetype;
	u8 ref_type;
	int errors;
	u64 dir;
	u64 index;
	u16 namelen;
	char name[0];
};

static inline struct inode_backref* to_inode_backref(struct list_head *entry)
{
	return list_entry(entry, struct inode_backref, list);
}

struct root_item_record {
	struct list_head list;
	u64 objectid;
	u64 bytenr;
	u64 last_snapshot;
	u8 level;
	u8 drop_level;
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

static inline struct root_backref* to_root_backref(struct list_head *entry)
{
	return list_entry(entry, struct root_backref, list);
}

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

struct extent_entry {
	u64 bytenr;
	u64 bytes;
	int count;
	int broken;
	struct list_head list;
};

struct root_item_info {
	/* level of the root */
	u8 level;
	/* number of nodes at this level, must be 1 for a root */
	int node_count;
	u64 bytenr;
	u64 gen;
	struct cache_extent cache_extent;
};

/*
 * Error bit for low memory mode check.
 *
 * Currently no caller cares about it yet.  Just internal use for error
 * classification.
 */
#define BACKREF_MISSING		(1 << 0) /* Backref missing in extent tree */
#define BACKREF_MISMATCH	(1 << 1) /* Backref exists but does not match */
#define BYTES_UNALIGNED		(1 << 2) /* Some bytes are not aligned */
#define REFERENCER_MISSING	(1 << 3) /* Referencer not found */
#define REFERENCER_MISMATCH	(1 << 4) /* Referenceer found but does not match */
#define CROSSING_STRIPE_BOUNDARY (1 << 4) /* For kernel scrub workaround */
#define ITEM_SIZE_MISMATCH	(1 << 5) /* Bad item size */
#define UNKNOWN_TYPE		(1 << 6) /* Unknown type */
#define ACCOUNTING_MISMATCH	(1 << 7) /* Used space accounting error */
#define CHUNK_TYPE_MISMATCH	(1 << 8)

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

static enum btrfs_check_mode parse_check_mode(const char *str)
{
	if (strcmp(str, "lowmem") == 0)
		return CHECK_MODE_LOWMEM;
	if (strcmp(str, "orig") == 0)
		return CHECK_MODE_ORIGINAL;
	if (strcmp(str, "original") == 0)
		return CHECK_MODE_ORIGINAL;

	return CHECK_MODE_UNKNOWN;
}

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
	 * Now there will be no overlap, delete the hole and re-add the
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
	struct inode_backref *tmp;
	struct orphan_data_extent *src_orphan;
	struct orphan_data_extent *dst_orphan;
	struct rb_node *rb;
	size_t size;
	int ret;

	rec = malloc(sizeof(*rec));
	if (!rec)
		return ERR_PTR(-ENOMEM);
	memcpy(rec, orig_rec, sizeof(*rec));
	rec->refs = 1;
	INIT_LIST_HEAD(&rec->backrefs);
	INIT_LIST_HEAD(&rec->orphan_extents);
	rec->holes = RB_ROOT;

	list_for_each_entry(orig, &orig_rec->backrefs, list) {
		size = sizeof(*orig) + orig->namelen + 1;
		backref = malloc(size);
		if (!backref) {
			ret = -ENOMEM;
			goto cleanup;
		}
		memcpy(backref, orig, size);
		list_add_tail(&backref->list, &rec->backrefs);
	}
	list_for_each_entry(src_orphan, &orig_rec->orphan_extents, list) {
		dst_orphan = malloc(sizeof(*dst_orphan));
		if (!dst_orphan) {
			ret = -ENOMEM;
			goto cleanup;
		}
		memcpy(dst_orphan, src_orphan, sizeof(*src_orphan));
		list_add_tail(&dst_orphan->list, &rec->orphan_extents);
	}
	ret = copy_file_extent_holes(&rec->holes, &orig_rec->holes);
	if (ret < 0)
		goto cleanup_rb;

	return rec;

cleanup_rb:
	rb = rb_first(&rec->holes);
	while (rb) {
		struct file_extent_hole *hole;

		hole = rb_entry(rb, struct file_extent_hole, node);
		rb = rb_next(rb);
		free(hole);
	}

cleanup:
	if (!list_empty(&rec->backrefs))
		list_for_each_entry_safe(orig, tmp, &rec->backrefs, list) {
			list_del(&orig->list);
			free(orig);
		}

	if (!list_empty(&rec->orphan_extents))
		list_for_each_entry_safe(orig, tmp, &rec->orphan_extents, list) {
			list_del(&orig->list);
			free(orig);
		}

	free(rec);

	return ERR_PTR(ret);
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
				round_up(rec->isize,
					 root->fs_info->sectorsize));
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
		fprintf(stderr, ", index mismatch");
	if (errors & REF_ERR_FILETYPE_UNMATCH)
		fprintf(stderr, ", filetype mismatch");
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
			if (IS_ERR(node->data))
				return node->data;
			rec->refs--;
			rec = node->data;
		}
	} else if (mod) {
		rec = calloc(1, sizeof(*rec));
		if (!rec)
			return ERR_PTR(-ENOMEM);
		rec->ino = ino;
		rec->extent_start = (u64)-1;
		rec->refs = 1;
		INIT_LIST_HEAD(&rec->backrefs);
		INIT_LIST_HEAD(&rec->orphan_extents);
		rec->holes = RB_ROOT;

		node = malloc(sizeof(*node));
		if (!node) {
			free(rec);
			return ERR_PTR(-ENOMEM);
		}
		node->cache.start = ino;
		node->cache.size = 1;
		node->data = rec;

		if (ino == BTRFS_FREE_INO_OBJECTID)
			rec->found_link = 1;

		ret = insert_cache_extent(inode_cache, &node->cache);
		if (ret)
			return ERR_PTR(-EEXIST);
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
		backref = to_inode_backref(rec->backrefs.next);
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
	u8 filetype;

	if (!rec->found_inode_item)
		return;

	filetype = imode_to_type(rec->imode);
	list_for_each_entry_safe(backref, tmp, &rec->backrefs, list) {
		if (backref->found_dir_item && backref->found_dir_index) {
			if (backref->filetype != filetype)
				backref->errors |= REF_ERR_FILETYPE_UNMATCH;
			if (!backref->errors && backref->found_inode_ref &&
			    rec->nlink == rec->found_link) {
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
	if (!backref)
		return NULL;
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
			     u8 filetype, u8 itemtype, int errors)
{
	struct inode_record *rec;
	struct inode_backref *backref;

	rec = get_inode_rec(inode_cache, ino, 1);
	BUG_ON(IS_ERR(rec));
	backref = get_inode_backref(rec, name, namelen, dir);
	BUG_ON(!backref);
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
			BUG_ON(!ins);
			ins->cache.start = node->cache.start;
			ins->cache.size = node->cache.size;
			ins->data = rec;
			rec->refs++;
		}
		ret = insert_cache_extent(dst, &ins->cache);
		if (ret == -EEXIST) {
			conflict = get_inode_rec(dst, rec->ino, 1);
			BUG_ON(IS_ERR(conflict));
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
		BUG_ON(IS_ERR(dst_node->current));
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
	if (!node)
		return -ENOMEM;
	node->cache.start = bytenr;
	node->cache.size = 1;
	cache_tree_init(&node->root_cache);
	cache_tree_init(&node->inode_cache);
	node->refs = refs;

	ret = insert_cache_extent(shared, &node->cache);

	return ret;
}

static int enter_shared_node(struct btrfs_root *root, u64 bytenr, u32 refs,
			     struct walk_control *wc, int level)
{
	struct shared_node *node;
	struct shared_node *dest;
	int ret;

	if (level == wc->active_node)
		return 0;

	BUG_ON(wc->active_node <= level);
	node = find_shared_node(&wc->shared, bytenr);
	if (!node) {
		ret = add_shared_node(&wc->shared, bytenr, refs);
		BUG_ON(ret);
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

static int process_dir_item(struct extent_buffer *eb,
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
	u8 filetype;
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
		if (cur + sizeof(*di) + name_len > total ||
		    name_len > BTRFS_NAME_LEN) {
			error = REF_ERR_NAME_TOO_LONG;

			if (cur + sizeof(*di) > total)
				break;
			len = min_t(u32, total - cur - sizeof(*di),
				    BTRFS_NAME_LEN);
		} else {
			len = name_len;
			error = 0;
		}

		read_extent_buffer(eb, namebuf, (unsigned long)(di + 1), len);

		if (key->type == BTRFS_DIR_ITEM_KEY &&
		    key->offset != btrfs_name_hash(namebuf, len)) {
			rec->errors |= I_ERR_ODD_DIR_ITEM;
			error("DIR_ITEM[%llu %llu] name %s namelen %u filetype %u mismatch with its hash, wanted %llu have %llu",
			key->objectid, key->offset, namebuf, len, filetype,
			key->offset, btrfs_name_hash(namebuf, len));
		}

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

		/* inode_ref + namelen should not cross item boundary */
		if (cur + sizeof(*ref) + name_len > total ||
		    name_len > BTRFS_NAME_LEN) {
			if (total < cur + sizeof(*ref))
				break;

			/* Still try to read out the remaining part */
			len = min_t(u32, total - cur - sizeof(*ref),
				    BTRFS_NAME_LEN);
			error = REF_ERR_NAME_TOO_LONG;
		} else {
			len = name_len;
			error = 0;
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
		csum_end = key.offset + (size / csum_size) *
			   root->fs_info->sectorsize;
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
	u64 mask = root->fs_info->sectorsize - 1;
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
			BUG_ON(IS_ERR(active_node->current));
		}
		switch (key.type) {
		case BTRFS_DIR_ITEM_KEY:
		case BTRFS_DIR_INDEX_KEY:
			ret = process_dir_item(eb, i, &key, active_node);
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

struct node_refs {
	u64 bytenr[BTRFS_MAX_LEVEL];
	u64 refs[BTRFS_MAX_LEVEL];
	int need_check[BTRFS_MAX_LEVEL];
	/* field for checking all trees */
	int checked[BTRFS_MAX_LEVEL];
	/* the corresponding extent should be marked as full backref or not */
	int full_backref[BTRFS_MAX_LEVEL];
};

static int update_nodes_refs(struct btrfs_root *root, u64 bytenr,
			     struct extent_buffer *eb, struct node_refs *nrefs,
			     u64 level, int check_all);
static int check_inode_item(struct btrfs_root *root, struct btrfs_path *path,
			    unsigned int ext_ref);

/*
 * Returns >0  Found error, not fatal, should continue
 * Returns <0  Fatal error, must exit the whole check
 * Returns 0   No errors found
 */
static int process_one_leaf_v2(struct btrfs_root *root, struct btrfs_path *path,
			       struct node_refs *nrefs, int *level, int ext_ref)
{
	struct extent_buffer *cur = path->nodes[0];
	struct btrfs_key key;
	u64 cur_bytenr;
	u32 nritems;
	u64 first_ino = 0;
	int root_level = btrfs_header_level(root->node);
	int i;
	int ret = 0; /* Final return value */
	int err = 0; /* Positive error bitmap */

	cur_bytenr = cur->start;

	/* skip to first inode item or the first inode number change */
	nritems = btrfs_header_nritems(cur);
	for (i = 0; i < nritems; i++) {
		btrfs_item_key_to_cpu(cur, &key, i);
		if (i == 0)
			first_ino = key.objectid;
		if (key.type == BTRFS_INODE_ITEM_KEY ||
		    (first_ino && first_ino != key.objectid))
			break;
	}
	if (i == nritems) {
		path->slots[0] = nritems;
		return 0;
	}
	path->slots[0] = i;

again:
	err |= check_inode_item(root, path, ext_ref);

	/* modify cur since check_inode_item may change path */
	cur = path->nodes[0];

	if (err & LAST_ITEM)
		goto out;

	/* still have inode items in thie leaf */
	if (cur->start == cur_bytenr)
		goto again;

	/*
	 * we have switched to another leaf, above nodes may
	 * have changed, here walk down the path, if a node
	 * or leaf is shared, check whether we can skip this
	 * node or leaf.
	 */
	for (i = root_level; i >= 0; i--) {
		if (path->nodes[i]->start == nrefs->bytenr[i])
			continue;

		ret = update_nodes_refs(root, path->nodes[i]->start,
				path->nodes[i], nrefs, i, 0);
		if (ret)
			goto out;

		if (!nrefs->need_check[i]) {
			*level += 1;
			break;
		}
	}

	for (i = 0; i < *level; i++) {
		free_extent_buffer(path->nodes[i]);
		path->nodes[i] = NULL;
	}
out:
	err &= ~LAST_ITEM;
	if (err && !ret)
		ret = err;
	return ret;
}

static void reada_walk_down(struct btrfs_root *root,
			    struct extent_buffer *node, int slot)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 bytenr;
	u64 ptr_gen;
	u32 nritems;
	int i;
	int level;

	level = btrfs_header_level(node);
	if (level != 1)
		return;

	nritems = btrfs_header_nritems(node);
	for (i = slot; i < nritems; i++) {
		bytenr = btrfs_node_blockptr(node, i);
		ptr_gen = btrfs_node_ptr_generation(node, i);
		readahead_tree_block(fs_info, bytenr, ptr_gen);
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
static int check_child_node(struct extent_buffer *parent, int slot,
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

/*
 * for a tree node or leaf, if it's shared, indeed we don't need to iterate it
 * in every fs or file tree check. Here we find its all root ids, and only check
 * it in the fs or file tree which has the smallest root id.
 */
static int need_check(struct btrfs_root *root, struct ulist *roots)
{
	struct rb_node *node;
	struct ulist_node *u;

	/*
	 * @roots can be empty if it belongs to tree reloc tree
	 * In that case, we should always check the leaf, as we can't use
	 * the tree owner to ensure some other root will check it.
	 */
	if (roots->nnodes == 1 || roots->nnodes == 0)
		return 1;

	node = rb_first(&roots->root);
	u = rb_entry(node, struct ulist_node, rb_node);
	/*
	 * current root id is not smallest, we skip it and let it be checked
	 * in the fs or file tree who hash the smallest root id.
	 */
	if (root->objectid != u->val)
		return 0;

	return 1;
}

static int calc_extent_flag_v2(struct btrfs_root *root, struct extent_buffer *eb,
			       u64 *flags_ret)
{
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	struct btrfs_root_item *ri = &root->root_item;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;
	struct btrfs_path *path = NULL;
	unsigned long ptr;
	unsigned long end;
	u64 flags;
	u64 owner = 0;
	u64 offset;
	int slot;
	int type;
	int ret = 0;

	/*
	 * Except file/reloc tree, we can not have FULL BACKREF MODE
	 */
	if (root->objectid < BTRFS_FIRST_FREE_OBJECTID)
		goto normal;

	/* root node */
	if (eb->start == btrfs_root_bytenr(ri))
		goto normal;

	if (btrfs_header_flag(eb, BTRFS_HEADER_FLAG_RELOC))
		goto full_backref;

	owner = btrfs_header_owner(eb);
	if (owner == root->objectid)
		goto normal;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = btrfs_header_bytenr(eb);
	key.type = (u8)-1;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret <= 0) {
		ret = -EIO;
		goto out;
	}

	if (ret > 0) {
		ret = btrfs_previous_extent_item(extent_root, path,
						 key.objectid);
		if (ret)
			goto full_backref;

	}
	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

	eb = path->nodes[0];
	slot = path->slots[0];
	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);

	flags = btrfs_extent_flags(eb, ei);
	if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)
		goto full_backref;

	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + btrfs_item_size_nr(eb, slot);

	if (key.type == BTRFS_EXTENT_ITEM_KEY)
		ptr += sizeof(struct btrfs_tree_block_info);

next:
	/* Reached extent item ends normally */
	if (ptr == end)
		goto full_backref;

	/* Beyond extent item end, wrong item size */
	if (ptr > end) {
		error("extent item at bytenr %llu slot %d has wrong size",
			eb->start, slot);
		goto full_backref;
	}

	iref = (struct btrfs_extent_inline_ref *)ptr;
	offset = btrfs_extent_inline_ref_offset(eb, iref);
	type = btrfs_extent_inline_ref_type(eb, iref);

	if (type == BTRFS_TREE_BLOCK_REF_KEY && offset == owner)
		goto normal;
	ptr += btrfs_extent_inline_ref_size(type);
	goto next;

normal:
	*flags_ret &= ~BTRFS_BLOCK_FLAG_FULL_BACKREF;
	goto out;

full_backref:
	*flags_ret |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
out:
	btrfs_free_path(path);
	return ret;
}

/*
 * for a tree node or leaf, we record its reference count, so later if we still
 * process this node or leaf, don't need to compute its reference count again.
 *
 * @bytenr  if @bytenr == (u64)-1, only update nrefs->full_backref[level]
 */
static int update_nodes_refs(struct btrfs_root *root, u64 bytenr,
			     struct extent_buffer *eb, struct node_refs *nrefs,
			     u64 level, int check_all)
{
	struct ulist *roots;
	u64 refs = 0;
	u64 flags = 0;
	int root_level = btrfs_header_level(root->node);
	int check;
	int ret;

	if (nrefs->bytenr[level] == bytenr)
		return 0;

	if (bytenr != (u64)-1) {
		/* the return value of this function seems a mistake */
		ret = btrfs_lookup_extent_info(NULL, root, bytenr,
				       level, 1, &refs, &flags);
		/* temporary fix */
		if (ret < 0 && !check_all)
			return ret;

		nrefs->bytenr[level] = bytenr;
		nrefs->refs[level] = refs;
		nrefs->full_backref[level] = 0;
		nrefs->checked[level] = 0;

		if (refs > 1) {
			ret = btrfs_find_all_roots(NULL, root->fs_info, bytenr,
						   0, &roots);
			if (ret)
				return -EIO;

			check = need_check(root, roots);
			ulist_free(roots);
			nrefs->need_check[level] = check;
		} else {
			if (!check_all) {
				nrefs->need_check[level] = 1;
			} else {
				if (level == root_level) {
					nrefs->need_check[level] = 1;
				} else {
					/*
					 * The node refs may have not been
					 * updated if upper needs checking (the
					 * lowest root_objectid) the node can
					 * be checked.
					 */
					nrefs->need_check[level] =
						nrefs->need_check[level + 1];
				}
			}
		}
	}

	if (check_all && eb) {
		calc_extent_flag_v2(root, eb, &flags);
		if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)
			nrefs->full_backref[level] = 1;
	}

	return 0;
}

/*
 * @level           if @level == -1 means extent data item
 *                  else normal treeblocl.
 */
static int should_check_extent_strictly(struct btrfs_root *root,
					struct node_refs *nrefs, int level)
{
	int root_level = btrfs_header_level(root->node);

	if (level > root_level || level < -1)
		return 1;
	if (level == root_level)
		return 1;
	/*
	 * if the upper node is marked full backref, it should contain shared
	 * backref of the parent (except owner == root->objectid).
	 */
	while (++level <= root_level)
		if (nrefs->refs[level] > 1)
			return 0;

	return 1;
}

static int walk_down_tree(struct btrfs_root *root, struct btrfs_path *path,
			  struct walk_control *wc, int *level,
			  struct node_refs *nrefs)
{
	enum btrfs_tree_block_status status;
	u64 bytenr;
	u64 ptr_gen;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *next;
	struct extent_buffer *cur;
	int ret, err = 0;
	u64 refs;

	WARN_ON(*level < 0);
	WARN_ON(*level >= BTRFS_MAX_LEVEL);

	if (path->nodes[*level]->start == nrefs->bytenr[*level]) {
		refs = nrefs->refs[*level];
		ret = 0;
	} else {
		ret = btrfs_lookup_extent_info(NULL, root,
				       path->nodes[*level]->start,
				       *level, 1, &refs, NULL);
		if (ret < 0) {
			err = ret;
			goto out;
		}
		nrefs->bytenr[*level] = path->nodes[*level]->start;
		nrefs->refs[*level] = refs;
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

		if (bytenr == nrefs->bytenr[*level - 1]) {
			refs = nrefs->refs[*level - 1];
		} else {
			ret = btrfs_lookup_extent_info(NULL, root, bytenr,
					*level - 1, 1, &refs, NULL);
			if (ret < 0) {
				refs = 0;
			} else {
				nrefs->bytenr[*level - 1] = bytenr;
				nrefs->refs[*level - 1] = refs;
			}
		}

		if (refs > 1) {
			ret = enter_shared_node(root, bytenr, refs,
						wc, *level - 1);
			if (ret > 0) {
				path->slots[*level]++;
				continue;
			}
		}

		next = btrfs_find_tree_block(fs_info, bytenr, fs_info->nodesize);
		if (!next || !btrfs_buffer_uptodate(next, ptr_gen)) {
			free_extent_buffer(next);
			reada_walk_down(root, cur, path->slots[*level]);
			next = read_tree_block(root->fs_info, bytenr, ptr_gen);
			if (!extent_buffer_uptodate(next)) {
				struct btrfs_key node_key;

				btrfs_node_key_to_cpu(path->nodes[*level],
						      &node_key,
						      path->slots[*level]);
				btrfs_add_corrupt_extent_record(root->fs_info,
						&node_key,
						path->nodes[*level]->start,
						root->fs_info->nodesize,
						*level);
				err = -EIO;
				goto out;
			}
		}

		ret = check_child_node(cur, path->slots[*level], next);
		if (ret) {
			free_extent_buffer(next);
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

static int fs_root_objectid(u64 objectid);

/*
 * Update global fs information.
 */
static void account_bytes(struct btrfs_root *root, struct btrfs_path *path,
			 int level)
{
	u32 free_nrs;
	struct extent_buffer *eb = path->nodes[level];

	total_btree_bytes += eb->len;
	if (fs_root_objectid(root->objectid))
		total_fs_tree_bytes += eb->len;
	if (btrfs_header_owner(eb) == BTRFS_EXTENT_TREE_OBJECTID)
		total_extent_tree_bytes += eb->len;

	if (level == 0) {
		btree_space_waste += btrfs_leaf_free_space(root, eb);
	} else {
		free_nrs = (BTRFS_NODEPTRS_PER_BLOCK(root) -
			    btrfs_header_nritems(eb));
		btree_space_waste += free_nrs * sizeof(struct btrfs_key_ptr);
	}
}

/*
 * This function only handles BACKREF_MISSING,
 * If corresponding extent item exists, increase the ref, else insert an extent
 * item and backref.
 *
 * Returns error bits after repair.
 */
static int repair_tree_block_ref(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct extent_buffer *node,
				 struct node_refs *nrefs, int level, int err)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_root *extent_root = fs_info->extent_root;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	struct btrfs_tree_block_info *bi;
	struct btrfs_key key;
	struct extent_buffer *eb;
	u32 size = sizeof(*ei);
	u32 node_size = root->fs_info->nodesize;
	int insert_extent = 0;
	int skinny_metadata = btrfs_fs_incompat(fs_info, SKINNY_METADATA);
	int root_level = btrfs_header_level(root->node);
	int generation;
	int ret;
	u64 owner;
	u64 bytenr;
	u64 flags = BTRFS_EXTENT_FLAG_TREE_BLOCK;
	u64 parent = 0;

	if ((err & BACKREF_MISSING) == 0)
		return err;

	WARN_ON(level > BTRFS_MAX_LEVEL);
	WARN_ON(level < 0);

	btrfs_init_path(&path);
	bytenr = btrfs_header_bytenr(node);
	owner = btrfs_header_owner(node);
	generation = btrfs_header_generation(node);

	key.objectid = bytenr;
	key.type = (u8)-1;
	key.offset = (u64)-1;

	/* Search for the extent item */
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret <= 0) {
		ret = -EIO;
		goto out;
	}

	ret = btrfs_previous_extent_item(extent_root, &path, bytenr);
	if (ret)
		insert_extent = 1;

	/* calculate if the extent item flag is full backref or not */
	if (nrefs->full_backref[level] != 0)
		flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;

	/* insert an extent item */
	if (insert_extent) {
		struct btrfs_disk_key copy_key;

		generation = btrfs_header_generation(node);

		if (level < root_level && nrefs->full_backref[level + 1] &&
		    owner != root->objectid) {
			flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
		}

		key.objectid = bytenr;
		if (!skinny_metadata) {
			key.type = BTRFS_EXTENT_ITEM_KEY;
			key.offset = node_size;
			size += sizeof(*bi);
		} else {
			key.type = BTRFS_METADATA_ITEM_KEY;
			key.offset = level;
		}

		btrfs_release_path(&path);
		ret = btrfs_insert_empty_item(trans, extent_root, &path, &key,
					      size);
		if (ret)
			goto out;

		eb = path.nodes[0];
		ei = btrfs_item_ptr(eb, path.slots[0], struct btrfs_extent_item);

		btrfs_set_extent_refs(eb, ei, 0);
		btrfs_set_extent_generation(eb, ei, generation);
		btrfs_set_extent_flags(eb, ei, flags);

		if (!skinny_metadata) {
			bi = (struct btrfs_tree_block_info *)(ei + 1);
			memset_extent_buffer(eb, 0, (unsigned long)bi,
					     sizeof(*bi));
			btrfs_set_disk_key_objectid(&copy_key, root->objectid);
			btrfs_set_disk_key_type(&copy_key, 0);
			btrfs_set_disk_key_offset(&copy_key, 0);

			btrfs_set_tree_block_level(eb, bi, level);
			btrfs_set_tree_block_key(eb, bi, &copy_key);
		}
		btrfs_mark_buffer_dirty(eb);
		printf("Added an extent item [%llu %u]\n", bytenr, node_size);
		btrfs_update_block_group(trans, extent_root, bytenr, node_size,
					 1, 0);

		nrefs->refs[level] = 0;
		nrefs->full_backref[level] =
			flags & BTRFS_BLOCK_FLAG_FULL_BACKREF;
		btrfs_release_path(&path);
	}

	if (level < root_level && nrefs->full_backref[level + 1] &&
	    owner != root->objectid)
		parent = nrefs->bytenr[level + 1];

	/* increase the ref */
	ret = btrfs_inc_extent_ref(trans, extent_root, bytenr, node_size,
			parent, root->objectid, level, 0);

	nrefs->refs[level]++;
out:
	btrfs_release_path(&path);
	if (ret) {
		error(
	"failed to repair tree block ref start %llu root %llu due to %s",
		      bytenr, root->objectid, strerror(-ret));
	} else {
		printf("Added one tree block ref start %llu %s %llu\n",
		       bytenr, parent ? "parent" : "root",
		       parent ? parent : root->objectid);
		err &= ~BACKREF_MISSING;
	}

	return err;
}

static int check_inode_item(struct btrfs_root *root, struct btrfs_path *path,
			    unsigned int ext_ref);
static int check_tree_block_ref(struct btrfs_root *root,
				struct extent_buffer *eb, u64 bytenr,
				int level, u64 owner, struct node_refs *nrefs);
static int check_leaf_items(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root, struct btrfs_path *path,
			    struct node_refs *nrefs, int account_bytes);

/*
 * @trans      just for lowmem repair mode
 * @check all  if not 0 then check all tree block backrefs and items
 *             0 then just check relationship of items in fs tree(s)
 *
 * Returns >0  Found error, should continue
 * Returns <0  Fatal error, must exit the whole check
 * Returns 0   No errors found
 */
static int walk_down_tree_v2(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, struct btrfs_path *path,
			     int *level, struct node_refs *nrefs, int ext_ref,
			     int check_all)

{
	enum btrfs_tree_block_status status;
	u64 bytenr;
	u64 ptr_gen;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *next;
	struct extent_buffer *cur;
	int ret;
	int err = 0;
	int check;
	int account_file_data = 0;

	WARN_ON(*level < 0);
	WARN_ON(*level >= BTRFS_MAX_LEVEL);

	ret = update_nodes_refs(root, btrfs_header_bytenr(path->nodes[*level]),
				path->nodes[*level], nrefs, *level, check_all);
	if (ret < 0)
		return ret;

	while (*level >= 0) {
		WARN_ON(*level < 0);
		WARN_ON(*level >= BTRFS_MAX_LEVEL);
		cur = path->nodes[*level];
		bytenr = btrfs_header_bytenr(cur);
		check = nrefs->need_check[*level];

		if (btrfs_header_level(cur) != *level)
			WARN_ON(1);
	       /*
		* Update bytes accounting and check tree block ref
		* NOTE: Doing accounting and check before checking nritems
		* is necessary because of empty node/leaf.
		*/
		if ((check_all && !nrefs->checked[*level]) ||
		    (!check_all && nrefs->need_check[*level])) {
			ret = check_tree_block_ref(root, cur,
			   btrfs_header_bytenr(cur), btrfs_header_level(cur),
			   btrfs_header_owner(cur), nrefs);

			if (repair && ret)
				ret = repair_tree_block_ref(trans, root,
				    path->nodes[*level], nrefs, *level, ret);
			err |= ret;

			if (check_all && nrefs->need_check[*level] &&
				nrefs->refs[*level]) {
				account_bytes(root, path, *level);
				account_file_data = 1;
			}
			nrefs->checked[*level] = 1;
		}

		if (path->slots[*level] >= btrfs_header_nritems(cur))
			break;

		/* Don't forgot to check leaf/node validation */
		if (*level == 0) {
			/* skip duplicate check */
			if (check || !check_all) {
				ret = btrfs_check_leaf(root, NULL, cur);
				if (ret != BTRFS_TREE_BLOCK_CLEAN) {
					err |= -EIO;
					break;
				}
			}

			ret = 0;
			if (!check_all)
				ret = process_one_leaf_v2(root, path, nrefs,
							  level, ext_ref);
			else
				ret = check_leaf_items(trans, root, path,
					       nrefs, account_file_data);
			err |= ret;
			break;
		} else {
			if (check || !check_all) {
				ret = btrfs_check_node(root, NULL, cur);
				if (ret != BTRFS_TREE_BLOCK_CLEAN) {
					err |= -EIO;
					break;
				}
			}
		}

		bytenr = btrfs_node_blockptr(cur, path->slots[*level]);
		ptr_gen = btrfs_node_ptr_generation(cur, path->slots[*level]);

		ret = update_nodes_refs(root, bytenr, NULL, nrefs, *level - 1,
					check_all);
		if (ret < 0)
			break;
		/*
		 * check all trees in check_chunks_and_extent_v2
		 * check shared node once in check_fs_roots
		 */
		if (!check_all && !nrefs->need_check[*level - 1]) {
			path->slots[*level]++;
			continue;
		}

		next = btrfs_find_tree_block(fs_info, bytenr, fs_info->nodesize);
		if (!next || !btrfs_buffer_uptodate(next, ptr_gen)) {
			free_extent_buffer(next);
			reada_walk_down(root, cur, path->slots[*level]);
			next = read_tree_block(fs_info, bytenr, ptr_gen);
			if (!extent_buffer_uptodate(next)) {
				struct btrfs_key node_key;

				btrfs_node_key_to_cpu(path->nodes[*level],
						      &node_key,
						      path->slots[*level]);
				btrfs_add_corrupt_extent_record(fs_info,
					&node_key, path->nodes[*level]->start,
					fs_info->nodesize, *level);
				err |= -EIO;
				break;
			}
		}

		ret = check_child_node(cur, path->slots[*level], next);
		err |= ret;
		if (ret < 0) 
			break;

		if (btrfs_is_leaf(next))
			status = btrfs_check_leaf(root, NULL, next);
		else
			status = btrfs_check_node(root, NULL, next);
		if (status != BTRFS_TREE_BLOCK_CLEAN) {
			free_extent_buffer(next);
			err |= -EIO;
			break;
		}

		*level = *level - 1;
		free_extent_buffer(path->nodes[*level]);
		path->nodes[*level] = next;
		path->slots[*level] = 0;
		account_file_data = 0;

		update_nodes_refs(root, (u64)-1, next, nrefs, *level, check_all);
	}
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

static int walk_up_tree_v2(struct btrfs_root *root, struct btrfs_path *path,
			   int *level)
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
	backref = to_inode_backref(rec->backrefs.next);
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
	struct btrfs_path path;
	struct btrfs_trans_handle *trans;
	struct btrfs_dir_item *dir_item;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_disk_key disk_key;
	struct inode_record *dir_rec;
	unsigned long name_ptr;
	u32 data_size = sizeof(*dir_item) + backref->namelen;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	fprintf(stderr, "repairing missing dir index item for inode %llu\n",
		(unsigned long long)rec->ino);

	btrfs_init_path(&path);
	key.objectid = backref->dir;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = backref->index;
	ret = btrfs_insert_empty_item(trans, root, &path, &key, data_size);
	BUG_ON(ret);

	leaf = path.nodes[0];
	dir_item = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_dir_item);

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
	btrfs_release_path(&path);
	btrfs_commit_transaction(trans, root);

	backref->found_dir_index = 1;
	dir_rec = get_inode_rec(inode_cache, backref->dir, 0);
	BUG_ON(IS_ERR(dir_rec));
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
			    struct inode_backref *backref)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_dir_item *di;
	struct btrfs_path path;
	int ret = 0;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	fprintf(stderr, "Deleting bad dir index [%llu,%u,%llu] root %llu\n",
		(unsigned long long)backref->dir,
		BTRFS_DIR_INDEX_KEY, (unsigned long long)backref->index,
		(unsigned long long)root->objectid);

	btrfs_init_path(&path);
	di = btrfs_lookup_dir_index(trans, root, &path, backref->dir,
				    backref->name, backref->namelen,
				    backref->index, -1);
	if (IS_ERR(di)) {
		ret = PTR_ERR(di);
		btrfs_release_path(&path);
		btrfs_commit_transaction(trans, root);
		if (ret == -ENOENT)
			return 0;
		return ret;
	}

	if (!di)
		ret = btrfs_del_item(trans, root, &path);
	else
		ret = btrfs_delete_one_dir_name(trans, root, &path, di);
	BUG_ON(ret);
	btrfs_release_path(&path);
	btrfs_commit_transaction(trans, root);
	return ret;
}

static int __create_inode_item(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 ino, u64 size,
			       u64 nbytes, u64 nlink, u32 mode)
{
	struct btrfs_inode_item ii;
	time_t now = time(NULL);
	int ret;

	btrfs_set_stack_inode_size(&ii, size);
	btrfs_set_stack_inode_nbytes(&ii, nbytes);
	btrfs_set_stack_inode_nlink(&ii, nlink);
	btrfs_set_stack_inode_mode(&ii, mode);
	btrfs_set_stack_inode_generation(&ii, trans->transid);
	btrfs_set_stack_timespec_nsec(&ii.atime, 0);
	btrfs_set_stack_timespec_sec(&ii.ctime, now);
	btrfs_set_stack_timespec_nsec(&ii.ctime, 0);
	btrfs_set_stack_timespec_sec(&ii.mtime, now);
	btrfs_set_stack_timespec_nsec(&ii.mtime, 0);
	btrfs_set_stack_timespec_sec(&ii.otime, 0);
	btrfs_set_stack_timespec_nsec(&ii.otime, 0);

	ret = btrfs_insert_inode(trans, root, ino, &ii);
	ASSERT(!ret);

	warning("root %llu inode %llu recreating inode item, this may "
		"be incomplete, please check permissions and content after "
		"the fsck completes.\n", (unsigned long long)root->objectid,
		(unsigned long long)ino);

	return 0;
}

static int create_inode_item_lowmem(struct btrfs_trans_handle *trans,
				    struct btrfs_root *root, u64 ino,
				    u8 filetype)
{
	u32 mode = (filetype == BTRFS_FT_DIR ? S_IFDIR : S_IFREG) | 0755;

	return __create_inode_item(trans, root, ino, 0, 0, 0, mode);
}

static int create_inode_item(struct btrfs_root *root,
			     struct inode_record *rec, int root_dir)
{
	struct btrfs_trans_handle *trans;
	u64 nlink = 0;
	u32 mode = 0;
	u64 size = 0;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		return ret;
	}

	nlink = root_dir ? 1 : rec->found_link;
	if (rec->found_dir_item) {
		if (rec->found_file_extent)
			fprintf(stderr, "root %llu inode %llu has both a dir "
				"item and extents, unsure if it is a dir or a "
				"regular file so setting it as a directory\n",
				(unsigned long long)root->objectid,
				(unsigned long long)rec->ino);
		mode = S_IFDIR | 0755;
		size = rec->found_size;
	} else if (!rec->found_dir_item) {
		size = rec->extent_end;
		mode =  S_IFREG | 0755;
	}

	ret = __create_inode_item(trans, root, rec->ino, size, rec->nbytes,
				  nlink, mode);
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
				ret = create_inode_item(root, rec, 1);
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
			ret = delete_dir_index(root, backref);
			if (ret)
				break;
			repaired++;
			list_del(&backref->list);
			free(backref);
			continue;
		}

		if (!delete && !backref->found_dir_index &&
		    backref->found_dir_item && backref->found_inode_ref) {
			ret = add_missing_dir_index(root, inode_cache, rec,
						    backref);
			if (ret)
				break;
			repaired++;
			if (backref->found_dir_item &&
			    backref->found_dir_index) {
				if (!backref->errors &&
				    backref->found_inode_ref) {
					list_del(&backref->list);
					free(backref);
					continue;
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
			ret = create_inode_item(root, rec, 0);
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
				     backref->filetype, &backref->index, 1, 0);
		if (ret < 0)
			goto out;
	}
out:
	btrfs_release_path(path);
	return ret;
}

static int get_highest_inode(struct btrfs_trans_handle *trans,
				struct btrfs_root *root,
				struct btrfs_path *path,
				u64 *highest_ino)
{
	struct btrfs_key key, found_key;
	int ret;

	btrfs_init_path(path);
	key.objectid = BTRFS_LAST_FREE_OBJECTID;
	key.offset = -1;
	key.type = BTRFS_INODE_ITEM_KEY;
	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret == 1) {
		btrfs_item_key_to_cpu(path->nodes[0], &found_key,
				path->slots[0] - 1);
		*highest_ino = found_key.objectid;
		ret = 0;
	}
	if (*highest_ino >= BTRFS_LAST_FREE_OBJECTID)
		ret = -EOVERFLOW;
	btrfs_release_path(path);
	return ret;
}

/*
 * Link inode to dir 'lost+found'. Increase @ref_count.
 *
 * Returns 0 means success.
 * Returns <0 means failure.
 */
static int link_inode_to_lostfound(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct btrfs_path *path,
				   u64 ino, char *namebuf, u32 name_len,
				   u8 filetype, u64 *ref_count)
{
	char *dir_name = "lost+found";
	u64 lost_found_ino;
	int ret;
	u32 mode = 0700;

	btrfs_release_path(path);
	ret = get_highest_inode(trans, root, path, &lost_found_ino);
	if (ret < 0)
		goto out;
	lost_found_ino++;

	ret = btrfs_mkdir(trans, root, dir_name, strlen(dir_name),
			  BTRFS_FIRST_FREE_OBJECTID, &lost_found_ino,
			  mode);
	if (ret < 0) {
		error("failed to create '%s' dir: %s", dir_name, strerror(-ret));
		goto out;
	}
	ret = btrfs_add_link(trans, root, ino, lost_found_ino,
			     namebuf, name_len, filetype, NULL, 1, 0);
	/*
	 * Add ".INO" suffix several times to handle case where
	 * "FILENAME.INO" is already taken by another file.
	 */
	while (ret == -EEXIST) {
		/*
		 * Conflicting file name, add ".INO" as suffix * +1 for '.'
		 */
		if (name_len + count_digits(ino) + 1 > BTRFS_NAME_LEN) {
			ret = -EFBIG;
			goto out;
		}
		snprintf(namebuf + name_len, BTRFS_NAME_LEN - name_len,
			 ".%llu", ino);
		name_len += count_digits(ino) + 1;
		ret = btrfs_add_link(trans, root, ino, lost_found_ino, namebuf,
				     name_len, filetype, NULL, 1, 0);
	}
	if (ret < 0) {
		error("failed to link the inode %llu to %s dir: %s",
		      ino, dir_name, strerror(-ret));
		goto out;
	}

	++*ref_count;
	printf("Moving file '%.*s' to '%s' dir since it has no valid backref\n",
	       name_len, namebuf, dir_name);
out:
	btrfs_release_path(path);
	if (ret)
		error("failed to move file '%.*s' to '%s' dir", name_len,
				namebuf, dir_name);
	return ret;
}

static int repair_inode_nlinks(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root,
			       struct btrfs_path *path,
			       struct inode_record *rec)
{
	char namebuf[BTRFS_NAME_LEN] = {0};
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
		ret = link_inode_to_lostfound(trans, root, path, rec->ino,
					      namebuf, namelen, type,
					      (u64 *)&rec->found_link);
		if (ret)
			goto out;
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
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_file_extent_item *fi;
	u8 type;
	int ret = 0;

	btrfs_init_path(&path);
	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		ret = 0;
		goto out;
	}
	if (ret && path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
		ret = btrfs_next_leaf(root, &path);
		if (ret) {
			ret = 0;
			goto out;
		}
	}
	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &found_key,
				      path.slots[0]);
		if (found_key.objectid != ino ||
		    found_key.type != BTRFS_EXTENT_DATA_KEY)
			break;
		fi = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_file_extent_item);
		type = btrfs_file_extent_type(path.nodes[0], fi);
		if (type != BTRFS_FILE_EXTENT_INLINE) {
			ret = 1;
			goto out;
		}
	}
out:
	btrfs_release_path(&path);
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
			printf("Can't determine the filetype for inode %llu, assume it is a normal file\n",
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
				       round_up(rec->isize,
					        root->fs_info->sectorsize));
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
	struct btrfs_path path;
	int ret = 0;

	if (!(rec->errors & (I_ERR_DIR_ISIZE_WRONG |
			     I_ERR_NO_ORPHAN_ITEM |
			     I_ERR_LINK_COUNT_WRONG |
			     I_ERR_NO_INODE_ITEM |
			     I_ERR_FILE_EXTENT_ORPHAN |
			     I_ERR_FILE_EXTENT_DISCOUNT|
			     I_ERR_FILE_NBYTES_WRONG)))
		return rec->errors;

	/*
	 * For nlink repair, it may create a dir and add link, so
	 * 2 for parent(256)'s dir_index and dir_item
	 * 2 for lost+found dir's inode_item and inode_ref
	 * 1 for the new inode_ref of the file
	 * 2 for lost+found dir's dir_index and dir_item for the file
	 */
	trans = btrfs_start_transaction(root, 7);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	btrfs_init_path(&path);
	if (rec->errors & I_ERR_NO_INODE_ITEM)
		ret = repair_inode_no_item(trans, root, &path, rec);
	if (!ret && rec->errors & I_ERR_FILE_EXTENT_ORPHAN)
		ret = repair_inode_orphan_extent(trans, root, &path, rec);
	if (!ret && rec->errors & I_ERR_FILE_EXTENT_DISCOUNT)
		ret = repair_inode_discount_extent(trans, root, &path, rec);
	if (!ret && rec->errors & I_ERR_DIR_ISIZE_WRONG)
		ret = repair_inode_isize(trans, root, &path, rec);
	if (!ret && rec->errors & I_ERR_NO_ORPHAN_ITEM)
		ret = repair_inode_orphan_item(trans, root, &path, rec);
	if (!ret && rec->errors & I_ERR_LINK_COUNT_WRONG)
		ret = repair_inode_nlinks(trans, root, &path, rec);
	if (!ret && rec->errors & I_ERR_FILE_NBYTES_WRONG)
		ret = repair_inode_nbytes(trans, root, &path, rec);
	btrfs_commit_transaction(trans, root);
	btrfs_release_path(&path);
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
	BUG_ON(IS_ERR(rec));
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
		if (!rec)
			return ERR_PTR(-ENOMEM);
		rec->objectid = objectid;
		INIT_LIST_HEAD(&rec->backrefs);
		rec->cache.start = objectid;
		rec->cache.size = 1;

		ret = insert_cache_extent(root_cache, &rec->cache);
		if (ret)
			return ERR_PTR(-EEXIST);
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
	if (!backref)
		return NULL;
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
		backref = to_root_backref(rec->backrefs.next);
		list_del(&backref->list);
		free(backref);
	}

	free(rec);
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
	BUG_ON(IS_ERR(rec));
	backref = get_root_backref(rec, ref_root, dir, index, name, namelen);
	BUG_ON(!backref);

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
	BUG_ON(IS_ERR(rec));
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
				BUG_ON(IS_ERR(ref_root));
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
	struct btrfs_path path;
	struct btrfs_corrupt_block *corrupt;
	struct cache_extent *cache;
	struct btrfs_key key;
	u64 offset;
	int level;
	int ret = 0;

	if (cache_tree_empty(corrupt_blocks))
		return 0;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		fprintf(stderr, "Error starting transaction: %s\n",
			strerror(-ret));
		return ret;
	}
	btrfs_init_path(&path);
	cache = first_cache_extent(corrupt_blocks);
	while (cache) {
		corrupt = container_of(cache, struct btrfs_corrupt_block,
				       cache);
		level = corrupt->level;
		path.lowest_level = level;
		key.objectid = corrupt->key.objectid;
		key.type = corrupt->key.type;
		key.offset = corrupt->key.offset;

		/*
		 * Here we don't want to do any tree balance, since it may
		 * cause a balance with corrupted brother leaf/node,
		 * so ins_len set to 0 here.
		 * Balance will be done after all corrupt node/leaf is deleted.
		 */
		ret = btrfs_search_slot(trans, root, &key, &path, 0, 1);
		if (ret < 0)
			goto out;
		offset = btrfs_node_blockptr(path.nodes[level],
					     path.slots[level]);

		/* Remove the ptr */
		ret = btrfs_del_ptr(root, &path, level, path.slots[level]);
		if (ret < 0)
			goto out;
		/*
		 * Remove the corresponding extent
		 * return value is not concerned.
		 */
		btrfs_release_path(&path);
		ret = btrfs_free_extent(trans, root, offset,
				root->fs_info->nodesize, 0,
				root->root_key.objectid, level - 1, 0);
		cache = next_cache_extent(cache);
	}

	/* Balance the btree using btrfs_search_slot() */
	cache = first_cache_extent(corrupt_blocks);
	while (cache) {
		corrupt = container_of(cache, struct btrfs_corrupt_block,
				       cache);
		memcpy(&key, &corrupt->key, sizeof(key));
		ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
		if (ret < 0)
			goto out;
		/* return will always >0 since it won't find the item */
		ret = 0;
		btrfs_release_path(&path);
		cache = next_cache_extent(cache);
	}
out:
	btrfs_commit_transaction(trans, root);
	btrfs_release_path(&path);
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
	struct node_refs nrefs;

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
		BUG_ON(IS_ERR(rec));
		if (btrfs_root_refs(root_item) > 0)
			rec->found_root_item = 1;
	}

	btrfs_init_path(&path);
	memset(&root_node, 0, sizeof(root_node));
	cache_tree_init(&root_node.root_cache);
	cache_tree_init(&root_node.inode_cache);
	memset(&nrefs, 0, sizeof(nrefs));

	/* Move the orphan extent record to corresponding inode_record */
	list_for_each_entry_safe(orphan, tmp,
				 &root->orphan_data_extents, list) {
		struct inode_record *inode;

		inode = get_inode_rec(&root_node.inode_cache, orphan->objectid,
				      1);
		BUG_ON(IS_ERR(inode));
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
		if (level > btrfs_header_level(root->node) ||
		    level >= BTRFS_MAX_LEVEL) {
			error("ignoring invalid drop level: %u", level);
			goto skip_walking;
		}
		wret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
		if (wret < 0)
			goto skip_walking;
		btrfs_node_key(path.nodes[level], &found_key,
				path.slots[level]);
		WARN_ON(memcmp(&found_key, &root_item->drop_progress,
					sizeof(found_key)));
	}

	while (1) {
		wret = walk_down_tree(root, &path, wc, &level, &nrefs);
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

static int check_fs_roots(struct btrfs_fs_info *fs_info,
			  struct cache_tree *root_cache)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct walk_control wc;
	struct extent_buffer *leaf, *tree_node;
	struct btrfs_root *tmp_root;
	struct btrfs_root *tree_root = fs_info->tree_root;
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
		reset_cached_block_groups(fs_info);
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
						fs_info, &key);
			} else {
				key.offset = (u64)-1;
				tmp_root = btrfs_read_fs_root(
						fs_info, &key);
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

/*
 * Find the @index according by @ino and name.
 * Notice:time efficiency is O(N)
 *
 * @root:	the root of the fs/file tree
 * @index_ret:	the index as return value
 * @namebuf:	the name to match
 * @name_len:	the length of name to match
 * @file_type:	the file_type of INODE_ITEM to match
 *
 * Returns 0 if found and *@index_ret will be modified with right value
 * Returns< 0 not found and *@index_ret will be (u64)-1
 */
static int find_dir_index(struct btrfs_root *root, u64 dirid, u64 location_id,
			  u64 *index_ret, char *namebuf, u32 name_len,
			  u8 file_type)
{
	struct btrfs_path path;
	struct extent_buffer *node;
	struct btrfs_dir_item *di;
	struct btrfs_key key;
	struct btrfs_key location;
	char name[BTRFS_NAME_LEN] = {0};

	u32 total;
	u32 cur = 0;
	u32 len;
	u32 data_len;
	u8 filetype;
	int slot;
	int ret;

	ASSERT(index_ret);

	/* search from the last index */
	key.objectid = dirid;
	key.offset = (u64)-1;
	key.type = BTRFS_DIR_INDEX_KEY;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

loop:
	ret = btrfs_previous_item(root, &path, dirid, BTRFS_DIR_INDEX_KEY);
	if (ret) {
		ret = -ENOENT;
		*index_ret = (64)-1;
		goto out;
	}
	/* Check whether inode_id/filetype/name match */
	node = path.nodes[0];
	slot = path.slots[0];
	di = btrfs_item_ptr(node, slot, struct btrfs_dir_item);
	total = btrfs_item_size_nr(node, slot);
	while (cur < total) {
		ret = -ENOENT;
		len = btrfs_dir_name_len(node, di);
		data_len = btrfs_dir_data_len(node, di);

		btrfs_dir_item_key_to_cpu(node, di, &location);
		if (location.objectid != location_id ||
		    location.type != BTRFS_INODE_ITEM_KEY ||
		    location.offset != 0)
			goto next;

		filetype = btrfs_dir_type(node, di);
		if (file_type != filetype)
			goto next;

		if (len > BTRFS_NAME_LEN)
			len = BTRFS_NAME_LEN;

		read_extent_buffer(node, name, (unsigned long)(di + 1), len);
		if (len != name_len || strncmp(namebuf, name, len))
			goto next;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		*index_ret = key.offset;
		ret = 0;
		goto out;
next:
		len += sizeof(*di) + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}
	goto loop;

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Find DIR_ITEM/DIR_INDEX for the given key and check it with the specified
 * INODE_REF/INODE_EXTREF match.
 *
 * @root:	the root of the fs/file tree
 * @key:	the key of the DIR_ITEM/DIR_INDEX, key->offset will be right
 *              value while find index
 * @location_key: location key of the struct btrfs_dir_item to match
 * @name:	the name to match
 * @namelen:	the length of name
 * @file_type:	the type of file to math
 *
 * Return 0 if no error occurred.
 * Return DIR_ITEM_MISSING/DIR_INDEX_MISSING if couldn't find
 * DIR_ITEM/DIR_INDEX
 * Return DIR_ITEM_MISMATCH/DIR_INDEX_MISMATCH if INODE_REF/INODE_EXTREF
 * and DIR_ITEM/DIR_INDEX mismatch
 */
static int find_dir_item(struct btrfs_root *root, struct btrfs_key *key,
			 struct btrfs_key *location_key, char *name,
			 u32 namelen, u8 file_type)
{
	struct btrfs_path path;
	struct extent_buffer *node;
	struct btrfs_dir_item *di;
	struct btrfs_key location;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 data_len;
	u8 filetype;
	int slot;
	int ret;

	/* get the index by traversing all index */
	if (key->type == BTRFS_DIR_INDEX_KEY && key->offset == (u64)-1) {
		ret = find_dir_index(root, key->objectid,
				     location_key->objectid, &key->offset,
				     name, namelen, file_type);
		if (ret)
			ret = DIR_INDEX_MISSING;
		return ret;
	}

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, key, &path, 0, 0);
	if (ret) {
		ret = key->type == BTRFS_DIR_ITEM_KEY ? DIR_ITEM_MISSING :
			DIR_INDEX_MISSING;
		goto out;
	}

	/* Check whether inode_id/filetype/name match */
	node = path.nodes[0];
	slot = path.slots[0];
	di = btrfs_item_ptr(node, slot, struct btrfs_dir_item);
	total = btrfs_item_size_nr(node, slot);
	while (cur < total) {
		ret = key->type == BTRFS_DIR_ITEM_KEY ?
			DIR_ITEM_MISMATCH : DIR_INDEX_MISMATCH;

		len = btrfs_dir_name_len(node, di);
		data_len = btrfs_dir_data_len(node, di);

		btrfs_dir_item_key_to_cpu(node, di, &location);
		if (location.objectid != location_key->objectid ||
		    location.type != location_key->type ||
		    location.offset != location_key->offset)
			goto next;

		filetype = btrfs_dir_type(node, di);
		if (file_type != filetype)
			goto next;

		if (len > BTRFS_NAME_LEN) {
			len = BTRFS_NAME_LEN;
			warning("root %llu %s[%llu %llu] name too long %u, trimmed",
			root->objectid,
			key->type == BTRFS_DIR_ITEM_KEY ?
			"DIR_ITEM" : "DIR_INDEX",
			key->objectid, key->offset, len);
		}
		read_extent_buffer(node, namebuf, (unsigned long)(di + 1),
				   len);
		if (len != namelen || strncmp(namebuf, name, len))
			goto next;

		ret = 0;
		goto out;
next:
		len += sizeof(*di) + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Prints inode ref error message
 */
static void print_inode_ref_err(struct btrfs_root *root, struct btrfs_key *key,
				u64 index, const char *namebuf, int name_len,
				u8 filetype, int err)
{
	if (!err)
		return;

	/* root dir error */
	if (key->objectid == BTRFS_FIRST_FREE_OBJECTID) {
		error(
	"root %llu root dir shouldn't have INODE REF[%llu %llu] name %s",
		      root->objectid, key->objectid, key->offset, namebuf);
		return;
	}

	/* normal error */
	if (err & (DIR_ITEM_MISMATCH | DIR_ITEM_MISSING))
		error("root %llu DIR ITEM[%llu %llu] %s name %s filetype %u",
		      root->objectid, key->offset,
		      btrfs_name_hash(namebuf, name_len),
		      err & DIR_ITEM_MISMATCH ? "mismatch" : "missing",
		      namebuf, filetype);
	if (err & (DIR_INDEX_MISMATCH | DIR_INDEX_MISSING))
		error("root %llu DIR INDEX[%llu %llu] %s name %s filetype %u",
		      root->objectid, key->offset, index,
		      err & DIR_ITEM_MISMATCH ? "mismatch" : "missing",
		      namebuf, filetype);
}

/*
 * Insert the missing inode item.
 *
 * Returns 0 means success.
 * Returns <0 means error.
 */
static int repair_inode_item_missing(struct btrfs_root *root, u64 ino,
				     u8 filetype)
{
	struct btrfs_key key;
	struct btrfs_trans_handle *trans;
	struct btrfs_path path;
	int ret;

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	btrfs_init_path(&path);
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = -EIO;
		goto out;
	}

	ret = btrfs_search_slot(trans, root, &key, &path, 1, 1);
	if (ret < 0 || !ret)
		goto fail;

	/* insert inode item */
	create_inode_item_lowmem(trans, root, ino, filetype);
	ret = 0;
fail:
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error("failed to repair root %llu INODE ITEM[%llu] missing",
		      root->objectid, ino);
	btrfs_release_path(&path);
	return ret;
}

/*
 * The ternary means dir item, dir index and relative inode ref.
 * The function handles errs: INODE_MISSING, DIR_INDEX_MISSING
 * DIR_INDEX_MISMATCH, DIR_ITEM_MISSING, DIR_ITEM_MISMATCH by the follow
 * strategy:
 * If two of three is missing or mismatched, delete the existing one.
 * If one of three is missing or mismatched, add the missing one.
 *
 * returns 0 means success.
 * returns not 0 means on error;
 */
int repair_ternary_lowmem(struct btrfs_root *root, u64 dir_ino, u64 ino,
			  u64 index, char *name, int name_len, u8 filetype,
			  int err)
{
	struct btrfs_trans_handle *trans;
	int stage = 0;
	int ret = 0;

	/*
	 * stage shall be one of following valild values:
	 *	0: Fine, nothing to do.
	 *	1: One of three is wrong, so add missing one.
	 *	2: Two of three is wrong, so delete existed one.
	 */
	if (err & (DIR_INDEX_MISMATCH | DIR_INDEX_MISSING))
		stage++;
	if (err & (DIR_ITEM_MISMATCH | DIR_ITEM_MISSING))
		stage++;
	if (err & (INODE_REF_MISSING))
		stage++;

	/* stage must be smllarer than 3 */
	ASSERT(stage < 3);

	trans = btrfs_start_transaction(root, 1);
	if (stage == 2) {
		ret = btrfs_unlink(trans, root, ino, dir_ino, index, name,
				   name_len, 0);
		goto out;
	}
	if (stage == 1) {
		ret = btrfs_add_link(trans, root, ino, dir_ino, name, name_len,
			       filetype, &index, 1, 1);
		goto out;
	}
out:
	btrfs_commit_transaction(trans, root);

	if (ret)
		error("fail to repair inode %llu name %s filetype %u",
		      ino, name, filetype);
	else
		printf("%s ref/dir_item of inode %llu name %s filetype %u\n",
		       stage == 2 ? "Delete" : "Add",
		       ino, name, filetype);

	return ret;
}

/*
 * Traverse the given INODE_REF and call find_dir_item() to find related
 * DIR_ITEM/DIR_INDEX.
 *
 * @root:	the root of the fs/file tree
 * @ref_key:	the key of the INODE_REF
 * @path        the path provides node and slot
 * @refs:	the count of INODE_REF
 * @mode:	the st_mode of INODE_ITEM
 * @name_ret:   returns with the first ref's name
 * @name_len_ret:    len of the name_ret
 *
 * Return 0 if no error occurred.
 */
static int check_inode_ref(struct btrfs_root *root, struct btrfs_key *ref_key,
			   struct btrfs_path *path, char *name_ret,
			   u32 *namelen_ret, u64 *refs_ret, int mode)
{
	struct btrfs_key key;
	struct btrfs_key location;
	struct btrfs_inode_ref *ref;
	struct extent_buffer *node;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	int ret;
	int err = 0;
	int tmp_err;
	int slot;
	int need_research = 0;
	u64 refs;

begin:
	err = 0;
	cur = 0;
	refs = *refs_ret;

	/* since after repair, path and the dir item may be changed */
	if (need_research) {
		need_research = 0;
		btrfs_release_path(path);
		ret = btrfs_search_slot(NULL, root, ref_key, path, 0, 0);
		/* the item was deleted, let path point to the last checked item */
		if (ret > 0) {
			if (path->slots[0] == 0)
				btrfs_prev_leaf(root, path);
			else
				path->slots[0]--;
		}
		if (ret)
			goto out;
	}

	location.objectid = ref_key->objectid;
	location.type = BTRFS_INODE_ITEM_KEY;
	location.offset = 0;
	node = path->nodes[0];
	slot = path->slots[0];

	memset(namebuf, 0, sizeof(namebuf) / sizeof(*namebuf));
	ref = btrfs_item_ptr(node, slot, struct btrfs_inode_ref);
	total = btrfs_item_size_nr(node, slot);

next:
	/* Update inode ref count */
	refs++;
	tmp_err = 0;
	index = btrfs_inode_ref_index(node, ref);
	name_len = btrfs_inode_ref_name_len(node, ref);

	if (name_len <= BTRFS_NAME_LEN) {
		len = name_len;
	} else {
		len = BTRFS_NAME_LEN;
		warning("root %llu INODE_REF[%llu %llu] name too long",
			root->objectid, ref_key->objectid, ref_key->offset);
	}

	read_extent_buffer(node, namebuf, (unsigned long)(ref + 1), len);

	/* copy the first name found to name_ret */
	if (refs == 1 && name_ret) {
		memcpy(name_ret, namebuf, len);
		*namelen_ret = len;
	}

	/* Check root dir ref */
	if (ref_key->objectid == BTRFS_FIRST_FREE_OBJECTID) {
		if (index != 0 || len != strlen("..") ||
		    strncmp("..", namebuf, len) ||
		    ref_key->offset != BTRFS_FIRST_FREE_OBJECTID) {
			/* set err bits then repair will delete the ref */
			err |= DIR_INDEX_MISSING;
			err |= DIR_ITEM_MISSING;
		}
		goto end;
	}

	/* Find related DIR_INDEX */
	key.objectid = ref_key->offset;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = index;
	tmp_err |= find_dir_item(root, &key, &location, namebuf, len,
			    imode_to_type(mode));

	/* Find related dir_item */
	key.objectid = ref_key->offset;
	key.type = BTRFS_DIR_ITEM_KEY;
	key.offset = btrfs_name_hash(namebuf, len);
	tmp_err |= find_dir_item(root, &key, &location, namebuf, len,
			    imode_to_type(mode));
end:
	if (tmp_err && repair) {
		ret = repair_ternary_lowmem(root, ref_key->offset,
					    ref_key->objectid, index, namebuf,
					    name_len, imode_to_type(mode),
					    tmp_err);
		if (!ret) {
			need_research = 1;
			goto begin;
		}
	}
	print_inode_ref_err(root, ref_key, index, namebuf, name_len,
			    imode_to_type(mode), tmp_err);
	err |= tmp_err;
	len = sizeof(*ref) + name_len;
	ref = (struct btrfs_inode_ref *)((char *)ref + len);
	cur += len;
	if (cur < total)
		goto next;

out:
	*refs_ret = refs;
	return err;
}

/*
 * Traverse the given INODE_EXTREF and call find_dir_item() to find related
 * DIR_ITEM/DIR_INDEX.
 *
 * @root:	the root of the fs/file tree
 * @ref_key:	the key of the INODE_EXTREF
 * @refs:	the count of INODE_EXTREF
 * @mode:	the st_mode of INODE_ITEM
 *
 * Return 0 if no error occurred.
 */
static int check_inode_extref(struct btrfs_root *root,
			      struct btrfs_key *ref_key,
			      struct extent_buffer *node, int slot, u64 *refs,
			      int mode)
{
	struct btrfs_key key;
	struct btrfs_key location;
	struct btrfs_inode_extref *extref;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	u64 parent;
	int ret;
	int err = 0;

	location.objectid = ref_key->objectid;
	location.type = BTRFS_INODE_ITEM_KEY;
	location.offset = 0;

	extref = btrfs_item_ptr(node, slot, struct btrfs_inode_extref);
	total = btrfs_item_size_nr(node, slot);

next:
	/* update inode ref count */
	(*refs)++;
	name_len = btrfs_inode_extref_name_len(node, extref);
	index = btrfs_inode_extref_index(node, extref);
	parent = btrfs_inode_extref_parent(node, extref);
	if (name_len <= BTRFS_NAME_LEN) {
		len = name_len;
	} else {
		len = BTRFS_NAME_LEN;
		warning("root %llu INODE_EXTREF[%llu %llu] name too long",
			root->objectid, ref_key->objectid, ref_key->offset);
	}
	read_extent_buffer(node, namebuf, (unsigned long)(extref + 1), len);

	/* Check root dir ref name */
	if (index == 0 && strncmp(namebuf, "..", name_len)) {
		error("root %llu INODE_EXTREF[%llu %llu] ROOT_DIR name shouldn't be %s",
		      root->objectid, ref_key->objectid, ref_key->offset,
		      namebuf);
		err |= ROOT_DIR_ERROR;
	}

	/* find related dir_index */
	key.objectid = parent;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = index;
	ret = find_dir_item(root, &key, &location, namebuf, len, mode);
	err |= ret;

	/* find related dir_item */
	key.objectid = parent;
	key.type = BTRFS_DIR_ITEM_KEY;
	key.offset = btrfs_name_hash(namebuf, len);
	ret = find_dir_item(root, &key, &location, namebuf, len, mode);
	err |= ret;

	len = sizeof(*extref) + name_len;
	extref = (struct btrfs_inode_extref *)((char *)extref + len);
	cur += len;

	if (cur < total)
		goto next;

	return err;
}

/*
 * Find INODE_REF/INODE_EXTREF for the given key and check it with the specified
 * DIR_ITEM/DIR_INDEX match.
 * Return with @index_ret.
 *
 * @root:	the root of the fs/file tree
 * @key:	the key of the INODE_REF/INODE_EXTREF
 * @name:	the name in the INODE_REF/INODE_EXTREF
 * @namelen:	the length of name in the INODE_REF/INODE_EXTREF
 * @index_ret:	the index in the INODE_REF/INODE_EXTREF,
 *              value (64)-1 means do not check index
 * @ext_ref:	the EXTENDED_IREF feature
 *
 * Return 0 if no error occurred.
 * Return >0 for error bitmap
 */
static int find_inode_ref(struct btrfs_root *root, struct btrfs_key *key,
			  char *name, int namelen, u64 *index_ret,
			  unsigned int ext_ref)
{
	struct btrfs_path path;
	struct btrfs_inode_ref *ref;
	struct btrfs_inode_extref *extref;
	struct extent_buffer *node;
	char ref_namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 ref_namelen;
	u64 ref_index;
	u64 parent;
	u64 dir_id;
	int slot;
	int ret;

	ASSERT(index_ret);

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, key, &path, 0, 0);
	if (ret) {
		ret = INODE_REF_MISSING;
		goto extref;
	}

	node = path.nodes[0];
	slot = path.slots[0];

	ref = btrfs_item_ptr(node, slot, struct btrfs_inode_ref);
	total = btrfs_item_size_nr(node, slot);

	/* Iterate all entry of INODE_REF */
	while (cur < total) {
		ret = INODE_REF_MISSING;

		ref_namelen = btrfs_inode_ref_name_len(node, ref);
		ref_index = btrfs_inode_ref_index(node, ref);
		if (*index_ret != (u64)-1 && *index_ret != ref_index)
			goto next_ref;

		if (cur + sizeof(*ref) + ref_namelen > total ||
		    ref_namelen > BTRFS_NAME_LEN) {
			warning("root %llu INODE %s[%llu %llu] name too long",
				root->objectid,
				key->type == BTRFS_INODE_REF_KEY ?
					"REF" : "EXTREF",
				key->objectid, key->offset);

			if (cur + sizeof(*ref) > total)
				break;
			len = min_t(u32, total - cur - sizeof(*ref),
				    BTRFS_NAME_LEN);
		} else {
			len = ref_namelen;
		}

		read_extent_buffer(node, ref_namebuf, (unsigned long)(ref + 1),
				   len);

		if (len != namelen || strncmp(ref_namebuf, name, len))
			goto next_ref;

		*index_ret = ref_index;
		ret = 0;
		goto out;
next_ref:
		len = sizeof(*ref) + ref_namelen;
		ref = (struct btrfs_inode_ref *)((char *)ref + len);
		cur += len;
	}

extref:
	/* Skip if not support EXTENDED_IREF feature */
	if (!ext_ref)
		goto out;

	btrfs_release_path(&path);
	btrfs_init_path(&path);

	dir_id = key->offset;
	key->type = BTRFS_INODE_EXTREF_KEY;
	key->offset = btrfs_extref_hash(dir_id, name, namelen);

	ret = btrfs_search_slot(NULL, root, key, &path, 0, 0);
	if (ret) {
		ret = INODE_REF_MISSING;
		goto out;
	}

	node = path.nodes[0];
	slot = path.slots[0];

	extref = btrfs_item_ptr(node, slot, struct btrfs_inode_extref);
	cur = 0;
	total = btrfs_item_size_nr(node, slot);

	/* Iterate all entry of INODE_EXTREF */
	while (cur < total) {
		ret = INODE_REF_MISSING;

		ref_namelen = btrfs_inode_extref_name_len(node, extref);
		ref_index = btrfs_inode_extref_index(node, extref);
		parent = btrfs_inode_extref_parent(node, extref);
		if (*index_ret != (u64)-1 && *index_ret != ref_index)
			goto next_extref;

		if (parent != dir_id)
			goto next_extref;

		if (ref_namelen <= BTRFS_NAME_LEN) {
			len = ref_namelen;
		} else {
			len = BTRFS_NAME_LEN;
			warning("root %llu INODE %s[%llu %llu] name too long",
				root->objectid,
				key->type == BTRFS_INODE_REF_KEY ?
					"REF" : "EXTREF",
				key->objectid, key->offset);
		}
		read_extent_buffer(node, ref_namebuf,
				   (unsigned long)(extref + 1), len);

		if (len != namelen || strncmp(ref_namebuf, name, len))
			goto next_extref;

		*index_ret = ref_index;
		ret = 0;
		goto out;

next_extref:
		len = sizeof(*extref) + ref_namelen;
		extref = (struct btrfs_inode_extref *)((char *)extref + len);
		cur += len;

	}
out:
	btrfs_release_path(&path);
	return ret;
}

static void print_dir_item_err(struct btrfs_root *root, struct btrfs_key *key,
			       u64 ino, u64 index, const char *namebuf,
			       int name_len, u8 filetype, int err)
{
	if (err & (DIR_ITEM_MISMATCH | DIR_ITEM_MISSING)) {
		error("root %llu DIR ITEM[%llu %llu] name %s filetype %d %s",
		      root->objectid, key->objectid, key->offset, namebuf,
		      filetype,
		      err & DIR_ITEM_MISMATCH ? "mismath" : "missing");
	}

	if (err & (DIR_INDEX_MISMATCH | DIR_INDEX_MISSING)) {
		error("root %llu DIR INDEX[%llu %llu] name %s filetype %d %s",
		      root->objectid, key->objectid, index, namebuf, filetype,
		      err & DIR_ITEM_MISMATCH ? "mismath" : "missing");
	}

	if (err & (INODE_ITEM_MISSING | INODE_ITEM_MISMATCH)) {
		error(
		"root %llu INODE_ITEM[%llu] index %llu name %s filetype %d %s",
		      root->objectid, ino, index, namebuf, filetype,
		      err & INODE_ITEM_MISMATCH ? "mismath" : "missing");
	}

	if (err & INODE_REF_MISSING)
		error(
		"root %llu INODE REF[%llu, %llu] name %s filetype %u missing",
		      root->objectid, ino, key->objectid, namebuf, filetype);

}

/*
 * Call repair_inode_item_missing and repair_ternary_lowmem to repair
 *
 * Returns error after repair
 */
static int repair_dir_item(struct btrfs_root *root, u64 dirid, u64 ino,
			   u64 index, u8 filetype, char *namebuf, u32 name_len,
			   int err)
{
	int ret;

	if (err & INODE_ITEM_MISSING) {
		ret = repair_inode_item_missing(root, ino, filetype);
		if (!ret)
			err &= ~(INODE_ITEM_MISMATCH | INODE_ITEM_MISSING);
	}

	if (err & ~(INODE_ITEM_MISMATCH | INODE_ITEM_MISSING)) {
		ret = repair_ternary_lowmem(root, dirid, ino, index, namebuf,
					    name_len, filetype, err);
		if (!ret) {
			err &= ~(DIR_INDEX_MISMATCH | DIR_INDEX_MISSING);
			err &= ~(DIR_ITEM_MISMATCH | DIR_ITEM_MISSING);
			err &= ~(INODE_REF_MISSING);
		}
	}
	return err;
}

static int __count_dir_isize(struct btrfs_root *root, u64 ino, int type,
		u64 *size_ret)
{
	struct btrfs_key key;
	struct btrfs_path path;
	u32 len;
	struct btrfs_dir_item *di;
	int ret;
	int cur = 0;
	int total = 0;

	ASSERT(size_ret);
	*size_ret = 0;

	key.objectid = ino;
	key.type = type;
	key.offset = (u64)-1;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		ret = -EIO;
		goto out;
	}
	/* if found, go to spacial case */
	if (ret == 0)
		goto special_case;

loop:
	ret = btrfs_previous_item(root, &path, ino, type);

	if (ret) {
		ret = 0;
		goto out;
	}

special_case:
	di = btrfs_item_ptr(path.nodes[0], path.slots[0], struct btrfs_dir_item);
	cur = 0;
	total = btrfs_item_size_nr(path.nodes[0], path.slots[0]);

	while (cur < total) {
		len = btrfs_dir_name_len(path.nodes[0], di);
		if (len > BTRFS_NAME_LEN)
			len = BTRFS_NAME_LEN;
		*size_ret += len;

		len += btrfs_dir_data_len(path.nodes[0], di);
		len += sizeof(*di);
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}
	goto loop;

out:
	btrfs_release_path(&path);
	return ret;
}

static int count_dir_isize(struct btrfs_root *root, u64 ino, u64 *size)
{
	u64 item_size;
	u64 index_size;
	int ret;

	ASSERT(size);
	ret = __count_dir_isize(root, ino, BTRFS_DIR_ITEM_KEY, &item_size);
	if (ret)
		goto out;

	ret = __count_dir_isize(root, ino, BTRFS_DIR_INDEX_KEY, &index_size);
	if (ret)
		goto out;

	*size = item_size + index_size;

out:
	if (ret)
		error("failed to count root %llu INODE[%llu] root size",
		      root->objectid, ino);
	return ret;
}

/*
 * Traverse the given DIR_ITEM/DIR_INDEX and check related INODE_ITEM and
 * call find_inode_ref() to check related INODE_REF/INODE_EXTREF.
 *
 * @root:	the root of the fs/file tree
 * @key:	the key of the INODE_REF/INODE_EXTREF
 * @path:       the path
 * @size:	the st_size of the INODE_ITEM
 * @ext_ref:	the EXTENDED_IREF feature
 *
 * Return 0 if no error occurred.
 * Return DIR_COUNT_AGAIN if the isize of the inode should be recalculated.
 */
static int check_dir_item(struct btrfs_root *root, struct btrfs_key *di_key,
			  struct btrfs_path *path, u64 *size,
			  unsigned int ext_ref)
{
	struct btrfs_dir_item *di;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key location;
	struct extent_buffer *node;
	int slot;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u32 data_len;
	u8 filetype;
	u32 mode = 0;
	u64 index;
	int ret;
	int err;
	int tmp_err;
	int need_research = 0;

	/*
	 * For DIR_ITEM set index to (u64)-1, so that find_inode_ref
	 * ignore index check.
	 */
	if (di_key->type == BTRFS_DIR_INDEX_KEY)
		index = di_key->offset;
	else
		index = (u64)-1;
begin:
	err = 0;
	cur = 0;

	/* since after repair, path and the dir item may be changed */
	if (need_research) {
		need_research = 0;
		err |= DIR_COUNT_AGAIN;
		btrfs_release_path(path);
		ret = btrfs_search_slot(NULL, root, di_key, path, 0, 0);
		/* the item was deleted, let path point the last checked item */
		if (ret > 0) {
			if (path->slots[0] == 0)
				btrfs_prev_leaf(root, path);
			else
				path->slots[0]--;
		}
		if (ret)
			goto out;
	}

	node = path->nodes[0];
	slot = path->slots[0];

	di = btrfs_item_ptr(node, slot, struct btrfs_dir_item);
	total = btrfs_item_size_nr(node, slot);
	memset(namebuf, 0, sizeof(namebuf) / sizeof(*namebuf));

	while (cur < total) {
		data_len = btrfs_dir_data_len(node, di);
		tmp_err = 0;
		if (data_len)
			error("root %llu %s[%llu %llu] data_len shouldn't be %u",
			      root->objectid,
	      di_key->type == BTRFS_DIR_ITEM_KEY ? "DIR_ITEM" : "DIR_INDEX",
			      di_key->objectid, di_key->offset, data_len);

		name_len = btrfs_dir_name_len(node, di);
		if (name_len <= BTRFS_NAME_LEN) {
			len = name_len;
		} else {
			len = BTRFS_NAME_LEN;
			warning("root %llu %s[%llu %llu] name too long",
				root->objectid,
		di_key->type == BTRFS_DIR_ITEM_KEY ? "DIR_ITEM" : "DIR_INDEX",
				di_key->objectid, di_key->offset);
		}
		(*size) += name_len;
		read_extent_buffer(node, namebuf, (unsigned long)(di + 1),
				   len);
		filetype = btrfs_dir_type(node, di);

		if (di_key->type == BTRFS_DIR_ITEM_KEY &&
		    di_key->offset != btrfs_name_hash(namebuf, len)) {
			err |= -EIO;
			error("root %llu DIR_ITEM[%llu %llu] name %s namelen %u filetype %u mismatch with its hash, wanted %llu have %llu",
			root->objectid, di_key->objectid, di_key->offset,
			namebuf, len, filetype, di_key->offset,
			btrfs_name_hash(namebuf, len));
		}

		btrfs_dir_item_key_to_cpu(node, di, &location);
		/* Ignore related ROOT_ITEM check */
		if (location.type == BTRFS_ROOT_ITEM_KEY)
			goto next;

		btrfs_release_path(path);
		/* Check relative INODE_ITEM(existence/filetype) */
		ret = btrfs_search_slot(NULL, root, &location, path, 0, 0);
		if (ret) {
			tmp_err |= INODE_ITEM_MISSING;
			goto next;
		}

		ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_inode_item);
		mode = btrfs_inode_mode(path->nodes[0], ii);
		if (imode_to_type(mode) != filetype) {
			tmp_err |= INODE_ITEM_MISMATCH;
			goto next;
		}

		/* Check relative INODE_REF/INODE_EXTREF */
		key.objectid = location.objectid;
		key.type = BTRFS_INODE_REF_KEY;
		key.offset = di_key->objectid;
		tmp_err |= find_inode_ref(root, &key, namebuf, len,
					  &index, ext_ref);

		/* check relative INDEX/ITEM */
		key.objectid = di_key->objectid;
		if (key.type == BTRFS_DIR_ITEM_KEY) {
			key.type = BTRFS_DIR_INDEX_KEY;
			key.offset = index;
		} else {
			key.type = BTRFS_DIR_ITEM_KEY;
			key.offset = btrfs_name_hash(namebuf, name_len);
		}

		tmp_err |= find_dir_item(root, &key, &location, namebuf,
					 name_len, filetype);
		/* find_dir_item may find index */
		if (key.type == BTRFS_DIR_INDEX_KEY)
			index = key.offset;
next:

		if (tmp_err && repair) {
			ret = repair_dir_item(root, di_key->objectid,
					      location.objectid, index,
					      imode_to_type(mode), namebuf,
					      name_len, tmp_err);
			if (ret != tmp_err) {
				need_research = 1;
				goto begin;
			}
		}
		btrfs_release_path(path);
		print_dir_item_err(root, di_key, location.objectid, index,
				   namebuf, name_len, filetype, tmp_err);
		err |= tmp_err;
		len = sizeof(*di) + name_len + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;

		if (di_key->type == BTRFS_DIR_INDEX_KEY && cur < total) {
			error("root %llu DIR_INDEX[%llu %llu] should contain only one entry",
			      root->objectid, di_key->objectid,
			      di_key->offset);
			break;
		}
	}
out:
	/* research path */
	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, di_key, path, 0, 0);
	if (ret)
		err |= ret > 0 ? -ENOENT : ret;
	return err;
}

/*
 * Wrapper function of btrfs_punch_hole.
 *
 * Returns 0 means success.
 * Returns not 0 means error.
 */
static int punch_extent_hole(struct btrfs_root *root, u64 ino, u64 start,
			     u64 len)
{
	struct btrfs_trans_handle *trans;
	int ret = 0;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	ret = btrfs_punch_hole(trans, root, ino, start, len);
	if (ret)
		error("failed to add hole [%llu, %llu] in inode [%llu]",
		      start, len, ino);
	else
		printf("Add a hole [%llu, %llu] in inode [%llu]\n", start, len,
		       ino);

	btrfs_commit_transaction(trans, root);
	return ret;
}

/*
 * Check file extent datasum/hole, update the size of the file extents,
 * check and update the last offset of the file extent.
 *
 * @root:	the root of fs/file tree.
 * @fkey:	the key of the file extent.
 * @nodatasum:	INODE_NODATASUM feature.
 * @size:	the sum of all EXTENT_DATA items size for this inode.
 * @end:	the offset of the last extent.
 *
 * Return 0 if no error occurred.
 */
static int check_file_extent(struct btrfs_root *root, struct btrfs_key *fkey,
			     struct extent_buffer *node, int slot,
			     unsigned int nodatasum, u64 *size, u64 *end)
{
	struct btrfs_file_extent_item *fi;
	u64 disk_bytenr;
	u64 disk_num_bytes;
	u64 extent_num_bytes;
	u64 extent_offset;
	u64 csum_found;		/* In byte size, sectorsize aligned */
	u64 search_start;	/* Logical range start we search for csum */
	u64 search_len;		/* Logical range len we search for csum */
	unsigned int extent_type;
	unsigned int is_hole;
	int compressed = 0;
	int ret;
	int err = 0;

	fi = btrfs_item_ptr(node, slot, struct btrfs_file_extent_item);

	/* Check inline extent */
	extent_type = btrfs_file_extent_type(node, fi);
	if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
		struct btrfs_item *e = btrfs_item_nr(slot);
		u32 item_inline_len;

		item_inline_len = btrfs_file_extent_inline_item_len(node, e);
		extent_num_bytes = btrfs_file_extent_inline_len(node, slot, fi);
		compressed = btrfs_file_extent_compression(node, fi);
		if (extent_num_bytes == 0) {
			error(
		"root %llu EXTENT_DATA[%llu %llu] has empty inline extent",
				root->objectid, fkey->objectid, fkey->offset);
			err |= FILE_EXTENT_ERROR;
		}
		if (!compressed && extent_num_bytes != item_inline_len) {
			error(
		"root %llu EXTENT_DATA[%llu %llu] wrong inline size, have: %llu, expected: %u",
				root->objectid, fkey->objectid, fkey->offset,
				extent_num_bytes, item_inline_len);
			err |= FILE_EXTENT_ERROR;
		}
		*end += extent_num_bytes;
		*size += extent_num_bytes;
		return err;
	}

	/* Check extent type */
	if (extent_type != BTRFS_FILE_EXTENT_REG &&
			extent_type != BTRFS_FILE_EXTENT_PREALLOC) {
		err |= FILE_EXTENT_ERROR;
		error("root %llu EXTENT_DATA[%llu %llu] type bad",
		      root->objectid, fkey->objectid, fkey->offset);
		return err;
	}

	/* Check REG_EXTENT/PREALLOC_EXTENT */
	disk_bytenr = btrfs_file_extent_disk_bytenr(node, fi);
	disk_num_bytes = btrfs_file_extent_disk_num_bytes(node, fi);
	extent_num_bytes = btrfs_file_extent_num_bytes(node, fi);
	extent_offset = btrfs_file_extent_offset(node, fi);
	compressed = btrfs_file_extent_compression(node, fi);
	is_hole = (disk_bytenr == 0) && (disk_num_bytes == 0);

	/*
	 * Check EXTENT_DATA csum
	 *
	 * For plain (uncompressed) extent, we should only check the range
	 * we're referring to, as it's possible that part of prealloc extent
	 * has been written, and has csum:
	 *
	 * |<--- Original large preallocated extent A ---->|
	 * |<- Prealloc File Extent ->|<- Regular Extent ->|
	 *	No csum				Has csum
	 *
	 * For compressed extent, we should check the whole range.
	 */
	if (!compressed) {
		search_start = disk_bytenr + extent_offset;
		search_len = extent_num_bytes;
	} else {
		search_start = disk_bytenr;
		search_len = disk_num_bytes;
	}
	ret = count_csum_range(root, search_start, search_len, &csum_found);
	if (csum_found > 0 && nodatasum) {
		err |= ODD_CSUM_ITEM;
		error("root %llu EXTENT_DATA[%llu %llu] nodatasum shouldn't have datasum",
		      root->objectid, fkey->objectid, fkey->offset);
	} else if (extent_type == BTRFS_FILE_EXTENT_REG && !nodatasum &&
		   !is_hole && (ret < 0 || csum_found < search_len)) {
		err |= CSUM_ITEM_MISSING;
		error("root %llu EXTENT_DATA[%llu %llu] csum missing, have: %llu, expected: %llu",
		      root->objectid, fkey->objectid, fkey->offset,
		      csum_found, search_len);
	} else if (extent_type == BTRFS_FILE_EXTENT_PREALLOC && csum_found > 0) {
		err |= ODD_CSUM_ITEM;
		error("root %llu EXTENT_DATA[%llu %llu] prealloc shouldn't have csum, but has: %llu",
		      root->objectid, fkey->objectid, fkey->offset, csum_found);
	}

	/* Check EXTENT_DATA hole */
	if (!no_holes && *end != fkey->offset) {
		if (repair)
			ret = punch_extent_hole(root, fkey->objectid,
						*end, fkey->offset - *end);
		if (!repair || ret) {
			err |= FILE_EXTENT_ERROR;
			error(
		"root %llu EXTENT_DATA[%llu %llu] interrupt, should start at %llu",
			root->objectid, fkey->objectid, fkey->offset, *end);
		}
	}

	*end += extent_num_bytes;
	if (!is_hole)
		*size += extent_num_bytes;

	return err;
}

/*
 * Set inode item nbytes to @nbytes
 *
 * Returns  0     on success
 * Returns  != 0  on error
 */
static int repair_inode_nbytes_lowmem(struct btrfs_root *root,
				      struct btrfs_path *path,
				      u64 ino, u64 nbytes)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key research_key;
	int err = 0;
	int ret;

	btrfs_item_key_to_cpu(path->nodes[0], &research_key, path->slots[0]);

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		err |= ret;
		goto out;
	}

	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret) {
		err |= ret;
		goto fail;
	}

	ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_nbytes(path->nodes[0], ii, nbytes);
	btrfs_mark_buffer_dirty(path->nodes[0]);
fail:
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error("failed to set nbytes in inode %llu root %llu",
		      ino, root->root_key.objectid);
	else
		printf("Set nbytes in inode item %llu root %llu\n to %llu", ino,
		       root->root_key.objectid, nbytes);

	/* research path */
	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, &research_key, path, 0, 0);
	err |= ret;

	return err;
}

/*
 * Set directory inode isize to @isize.
 *
 * Returns 0     on success.
 * Returns != 0  on error.
 */
static int repair_dir_isize_lowmem(struct btrfs_root *root,
				   struct btrfs_path *path,
				   u64 ino, u64 isize)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key research_key;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(path->nodes[0], &research_key, path->slots[0]);

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		err |= ret;
		goto out;
	}

	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret) {
		err |= ret;
		goto fail;
	}

	ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_size(path->nodes[0], ii, isize);
	btrfs_mark_buffer_dirty(path->nodes[0]);
fail:
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error("failed to set isize in inode %llu root %llu",
		      ino, root->root_key.objectid);
	else
		printf("Set isize in inode %llu root %llu to %llu\n",
		       ino, root->root_key.objectid, isize);

	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, &research_key, path, 0, 0);
	err |= ret;

	return err;
}

/*
 * Wrapper function for btrfs_add_orphan_item().
 *
 * Returns 0     on success.
 * Returns != 0  on error.
 */
static int repair_inode_orphan_item_lowmem(struct btrfs_root *root,
					   struct btrfs_path *path, u64 ino)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key research_key;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(path->nodes[0], &research_key, path->slots[0]);

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		err |= ret;
		goto out;
	}

	btrfs_release_path(path);
	ret = btrfs_add_orphan_item(trans, root, path, ino);
	err |= ret;
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error("failed to add inode %llu as orphan item root %llu",
		      ino, root->root_key.objectid);
	else
		printf("Added inode %llu as orphan item root %llu\n",
		       ino, root->root_key.objectid);

	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, &research_key, path, 0, 0);
	err |= ret;

	return err;
}

/* Set inode_item nlink to @ref_count.
 * If @ref_count == 0, move it to "lost+found" and increase @ref_count.
 *
 * Returns 0 on success
 */
static int repair_inode_nlinks_lowmem(struct btrfs_root *root,
				      struct btrfs_path *path, u64 ino,
				      const char *name, u32 namelen,
				      u64 ref_count, u8 filetype, u64 *nlink)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key old_key;
	char namebuf[BTRFS_NAME_LEN] = {0};
	int name_len;
	int ret;
	int ret2;

	/* save the key */
	btrfs_item_key_to_cpu(path->nodes[0], &old_key, path->slots[0]);

	if (name && namelen) {
		ASSERT(namelen <= BTRFS_NAME_LEN);
		memcpy(namebuf, name, namelen);
		name_len = namelen;
	} else {
		sprintf(namebuf, "%llu", ino);
		name_len = count_digits(ino);
		printf("Can't find file name for inode %llu, use %s instead\n",
		       ino, namebuf);
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}

	btrfs_release_path(path);
	/* if refs is 0, put it into lostfound */
	if (ref_count == 0) {
		ret = link_inode_to_lostfound(trans, root, path, ino, namebuf,
					      name_len, filetype, &ref_count);
		if (ret)
			goto fail;
	}

	/* reset inode_item's nlink to ref_count */
	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret)
		goto fail;

	ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_nlink(path->nodes[0], ii, ref_count);
	btrfs_mark_buffer_dirty(path->nodes[0]);

	if (nlink)
		*nlink = ref_count;
fail:
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error(
	"fail to repair nlink of inode %llu root %llu name %s filetype %u",
		       root->objectid, ino, namebuf, filetype);
	else
		printf("Fixed nlink of inode %llu root %llu name %s filetype %u\n",
		       root->objectid, ino, namebuf, filetype);

	/* research */
	btrfs_release_path(path);
	ret2 = btrfs_search_slot(NULL, root, &old_key, path, 0, 0);
	if (ret2 < 0)
		return ret |= ret2;
	return ret;
}

/*
 * Check INODE_ITEM and related ITEMs (the same inode number)
 * 1. check link count
 * 2. check inode ref/extref
 * 3. check dir item/index
 *
 * @ext_ref:	the EXTENDED_IREF feature
 *
 * Return 0 if no error occurred.
 * Return >0 for error or hit the traversal is done(by error bitmap)
 */
static int check_inode_item(struct btrfs_root *root, struct btrfs_path *path,
			    unsigned int ext_ref)
{
	struct extent_buffer *node;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key last_key;
	u64 inode_id;
	u32 mode;
	u64 nlink;
	u64 nbytes;
	u64 isize;
	u64 size = 0;
	u64 refs = 0;
	u64 extent_end = 0;
	u64 extent_size = 0;
	unsigned int dir;
	unsigned int nodatasum;
	int slot;
	int ret;
	int err = 0;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 name_len = 0;

	node = path->nodes[0];
	slot = path->slots[0];

	btrfs_item_key_to_cpu(node, &key, slot);
	inode_id = key.objectid;

	if (inode_id == BTRFS_ORPHAN_OBJECTID) {
		ret = btrfs_next_item(root, path);
		if (ret > 0)
			err |= LAST_ITEM;
		return err;
	}

	ii = btrfs_item_ptr(node, slot, struct btrfs_inode_item);
	isize = btrfs_inode_size(node, ii);
	nbytes = btrfs_inode_nbytes(node, ii);
	mode = btrfs_inode_mode(node, ii);
	dir = imode_to_type(mode) == BTRFS_FT_DIR;
	nlink = btrfs_inode_nlink(node, ii);
	nodatasum = btrfs_inode_flags(node, ii) & BTRFS_INODE_NODATASUM;

	while (1) {
		btrfs_item_key_to_cpu(path->nodes[0], &last_key, path->slots[0]);
		ret = btrfs_next_item(root, path);
		if (ret < 0) {
			/* out will fill 'err' rusing current statistics */
			goto out;
		} else if (ret > 0) {
			err |= LAST_ITEM;
			goto out;
		}

		node = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);
		if (key.objectid != inode_id)
			goto out;

		switch (key.type) {
		case BTRFS_INODE_REF_KEY:
			ret = check_inode_ref(root, &key, path, namebuf,
					      &name_len, &refs, mode);
			err |= ret;
			break;
		case BTRFS_INODE_EXTREF_KEY:
			if (key.type == BTRFS_INODE_EXTREF_KEY && !ext_ref)
				warning("root %llu EXTREF[%llu %llu] isn't supported",
					root->objectid, key.objectid,
					key.offset);
			ret = check_inode_extref(root, &key, node, slot, &refs,
						 mode);
			err |= ret;
			break;
		case BTRFS_DIR_ITEM_KEY:
		case BTRFS_DIR_INDEX_KEY:
			if (!dir) {
				warning("root %llu INODE[%llu] mode %u shouldn't have DIR_INDEX[%llu %llu]",
					root->objectid,	inode_id,
					imode_to_type(mode), key.objectid,
					key.offset);
			}
			ret = check_dir_item(root, &key, path, &size, ext_ref);
			err |= ret;
			break;
		case BTRFS_EXTENT_DATA_KEY:
			if (dir) {
				warning("root %llu DIR INODE[%llu] shouldn't EXTENT_DATA[%llu %llu]",
					root->objectid, inode_id, key.objectid,
					key.offset);
			}
			ret = check_file_extent(root, &key, node, slot,
						nodatasum, &extent_size,
						&extent_end);
			err |= ret;
			break;
		case BTRFS_XATTR_ITEM_KEY:
			break;
		default:
			error("ITEM[%llu %u %llu] UNKNOWN TYPE",
			      key.objectid, key.type, key.offset);
		}
	}

out:
	if (err & LAST_ITEM) {
		btrfs_release_path(path);
		ret = btrfs_search_slot(NULL, root, &last_key, path, 0, 0);
		if (ret)
			return err;
	}

	/* verify INODE_ITEM nlink/isize/nbytes */
	if (dir) {
		if (repair && (err & DIR_COUNT_AGAIN)) {
			err &= ~DIR_COUNT_AGAIN;
			count_dir_isize(root, inode_id, &size);
		}

		if ((nlink != 1 || refs != 1) && repair) {
			ret = repair_inode_nlinks_lowmem(root, path, inode_id,
				namebuf, name_len, refs, imode_to_type(mode),
				&nlink);
		}

		if (nlink != 1) {
			err |= LINK_COUNT_ERROR;
			error("root %llu DIR INODE[%llu] shouldn't have more than one link(%llu)",
			      root->objectid, inode_id, nlink);
		}

		/*
		 * Just a warning, as dir inode nbytes is just an
		 * instructive value.
		 */
		if (!IS_ALIGNED(nbytes, root->fs_info->nodesize)) {
			warning("root %llu DIR INODE[%llu] nbytes should be aligned to %u",
				root->objectid, inode_id,
				root->fs_info->nodesize);
		}

		if (isize != size) {
			if (repair)
				ret = repair_dir_isize_lowmem(root, path,
							      inode_id, size);
			if (!repair || ret) {
				err |= ISIZE_ERROR;
				error(
		"root %llu DIR INODE [%llu] size %llu not equal to %llu",
				      root->objectid, inode_id, isize, size);
			}
		}
	} else {
		if (nlink != refs) {
			if (repair)
				ret = repair_inode_nlinks_lowmem(root, path,
					 inode_id, namebuf, name_len, refs,
					 imode_to_type(mode), &nlink);
			if (!repair || ret) {
				err |= LINK_COUNT_ERROR;
				error(
		"root %llu INODE[%llu] nlink(%llu) not equal to inode_refs(%llu)",
				      root->objectid, inode_id, nlink, refs);
			}
		} else if (!nlink) {
			if (repair)
				ret = repair_inode_orphan_item_lowmem(root,
							      path, inode_id);
			if (!repair || ret) {
				err |= ORPHAN_ITEM;
				error("root %llu INODE[%llu] is orphan item",
				      root->objectid, inode_id);
			}
		}

		if (!nbytes && !no_holes && extent_end < isize) {
			if (repair)
				ret = punch_extent_hole(root, inode_id,
						extent_end, isize - extent_end);
			if (!repair || ret) {
				err |= NBYTES_ERROR;
				error(
	"root %llu INODE[%llu] size %llu should have a file extent hole",
				      root->objectid, inode_id, isize);
			}
		}

		if (nbytes != extent_size) {
			if (repair)
				ret = repair_inode_nbytes_lowmem(root, path,
							 inode_id, extent_size);
			if (!repair || ret) {
				err |= NBYTES_ERROR;
				error(
	"root %llu INODE[%llu] nbytes %llu not equal to extent_size %llu",
				      root->objectid, inode_id, nbytes,
				      extent_size);
			}
		}
	}

	if (err & LAST_ITEM)
		btrfs_next_item(root, path);
	return err;
}

/*
 * Insert the missing inode item and inode ref.
 *
 * Normal INODE_ITEM_MISSING and INODE_REF_MISSING are handled in backref * dir.
 * Root dir should be handled specially because root dir is the root of fs.
 *
 * returns err (>0 or 0) after repair
 */
static int repair_fs_first_inode(struct btrfs_root *root, int err)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	struct btrfs_path path;
	int filetype = BTRFS_FT_DIR;
	int ret = 0;

	btrfs_init_path(&path);

	if (err & INODE_REF_MISSING) {
		key.objectid = BTRFS_FIRST_FREE_OBJECTID;
		key.type = BTRFS_INODE_REF_KEY;
		key.offset = BTRFS_FIRST_FREE_OBJECTID;

		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			goto out;
		}

		btrfs_release_path(&path);
		ret = btrfs_search_slot(trans, root, &key, &path, 1, 1);
		if (ret)
			goto trans_fail;

		ret = btrfs_insert_inode_ref(trans, root, "..", 2,
					     BTRFS_FIRST_FREE_OBJECTID,
					     BTRFS_FIRST_FREE_OBJECTID, 0);
		if (ret)
			goto trans_fail;

		printf("Add INODE_REF[%llu %llu] name %s\n",
		       BTRFS_FIRST_FREE_OBJECTID, BTRFS_FIRST_FREE_OBJECTID,
		       "..");
		err &= ~INODE_REF_MISSING;
trans_fail:
		if (ret)
			error("fail to insert first inode's ref");
		btrfs_commit_transaction(trans, root);
	}

	if (err & INODE_ITEM_MISSING) {
		ret = repair_inode_item_missing(root,
					BTRFS_FIRST_FREE_OBJECTID, filetype);
		if (ret)
			goto out;
		err &= ~INODE_ITEM_MISSING;
	}
out:
	if (ret)
		error("fail to repair first inode");
	btrfs_release_path(&path);
	return err;
}

/*
 * check first root dir's inode_item and inode_ref
 *
 * returns 0 means no error
 * returns >0 means error
 * returns <0 means fatal error
 */
static int check_fs_first_inode(struct btrfs_root *root, unsigned int ext_ref)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_inode_item *ii;
	u64 index;
	u32 mode;
	int err = 0;
	int ret;

	key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	/* For root being dropped, we don't need to check first inode */
	if (btrfs_root_refs(&root->root_item) == 0 &&
	    btrfs_disk_key_objectid(&root->root_item.drop_progress) >=
	    BTRFS_FIRST_FREE_OBJECTID)
		return 0;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = 0;
		err |= INODE_ITEM_MISSING;
	} else {
		ii = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_inode_item);
		mode = btrfs_inode_mode(path.nodes[0], ii);
		if (imode_to_type(mode) != BTRFS_FT_DIR)
			err |= INODE_ITEM_MISMATCH;
	}

	/* lookup first inode ref */
	key.offset = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_INODE_REF_KEY;
	/* special index value */
	index = 0;

	ret = find_inode_ref(root, &key, "..", strlen(".."), &index, ext_ref);
	if (ret < 0)
		goto out;
	err |= ret;

out:
	btrfs_release_path(&path);

	if (err && repair)
		err = repair_fs_first_inode(root, err);

	if (err & (INODE_ITEM_MISSING | INODE_ITEM_MISMATCH))
		error("root dir INODE_ITEM is %s",
		      err & INODE_ITEM_MISMATCH ? "mismatch" : "missing");
	if (err & INODE_REF_MISSING)
		error("root dir INODE_REF is missing");

	return ret < 0 ? ret : err;
}

static struct tree_backref *find_tree_backref(struct extent_record *rec,
						u64 parent, u64 root)
{
	struct rb_node *node;
	struct tree_backref *back = NULL;
	struct tree_backref match = {
		.node = {
			.is_data = 0,
		},
	};

	if (parent) {
		match.parent = parent;
		match.node.full_backref = 1;
	} else {
		match.root = root;
	}

	node = rb_search(&rec->backref_tree, &match.node.node,
			 (rb_compare_keys)compare_extent_backref, NULL);
	if (node)
		back = to_tree_backref(rb_node_to_extent_backref(node));

	return back;
}

static struct data_backref *find_data_backref(struct extent_record *rec,
						u64 parent, u64 root,
						u64 owner, u64 offset,
						int found_ref,
						u64 disk_bytenr, u64 bytes)
{
	struct rb_node *node;
	struct data_backref *back = NULL;
	struct data_backref match = {
		.node = {
			.is_data = 1,
		},
		.owner = owner,
		.offset = offset,
		.bytes = bytes,
		.found_ref = found_ref,
		.disk_bytenr = disk_bytenr,
	};

	if (parent) {
		match.parent = parent;
		match.node.full_backref = 1;
	} else {
		match.root = root;
	}

	node = rb_search(&rec->backref_tree, &match.node.node,
			 (rb_compare_keys)compare_extent_backref, NULL);
	if (node)
		back = to_data_backref(rb_node_to_extent_backref(node));

	return back;
}
/*
 * This function calls walk_down_tree_v2 and walk_up_tree_v2 to check tree
 * blocks and integrity of fs tree items.
 *
 * @root:         the root of the tree to be checked.
 * @ext_ref       feature EXTENDED_IREF is enable or not.
 * @account       if NOT 0 means check the tree (including tree)'s treeblocks.
 *                otherwise means check fs tree(s) items relationship and
 *		  @root MUST be a fs tree root.
 * Returns 0      represents OK.
 * Returns not 0  represents error.
 */
static int check_btrfs_root(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root, unsigned int ext_ref,
			    int check_all)

{
	struct btrfs_path path;
	struct node_refs nrefs;
	struct btrfs_root_item *root_item = &root->root_item;
	int ret;
	int level;
	int err = 0;

	memset(&nrefs, 0, sizeof(nrefs));
	if (!check_all) {
		/*
		 * We need to manually check the first inode item (256)
		 * As the following traversal function will only start from
		 * the first inode item in the leaf, if inode item (256) is
		 * missing we will skip it forever.
		 */
		ret = check_fs_first_inode(root, ext_ref);
		if (ret < 0)
			return ret;
	}


	level = btrfs_header_level(root->node);
	btrfs_init_path(&path);

	if (btrfs_root_refs(root_item) > 0 ||
	    btrfs_disk_key_objectid(&root_item->drop_progress) == 0) {
		path.nodes[level] = root->node;
		path.slots[level] = 0;
		extent_buffer_get(root->node);
	} else {
		struct btrfs_key key;

		btrfs_disk_key_to_cpu(&key, &root_item->drop_progress);
		level = root_item->drop_level;
		path.lowest_level = level;
		ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
		if (ret < 0)
			goto out;
		ret = 0;
	}

	while (1) {
		ret = walk_down_tree_v2(trans, root, &path, &level, &nrefs,
					ext_ref, check_all);

		err |= !!ret;

		/* if ret is negative, walk shall stop */
		if (ret < 0) {
			ret = err;
			break;
		}

		ret = walk_up_tree_v2(root, &path, &level);
		if (ret != 0) {
			/* Normal exit, reset ret to err */
			ret = err;
			break;
		}
	}

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Iterate all items in the tree and call check_inode_item() to check.
 *
 * @root:	the root of the tree to be checked.
 * @ext_ref:	the EXTENDED_IREF feature
 *
 * Return 0 if no error found.
 * Return <0 for error.
 */
static int check_fs_root_v2(struct btrfs_root *root, unsigned int ext_ref)
{
	reset_cached_block_groups(root->fs_info);
	return check_btrfs_root(NULL, root, ext_ref, 0);
}

/*
 * Find the relative ref for root_ref and root_backref.
 *
 * @root:	the root of the root tree.
 * @ref_key:	the key of the root ref.
 *
 * Return 0 if no error occurred.
 */
static int check_root_ref(struct btrfs_root *root, struct btrfs_key *ref_key,
			  struct extent_buffer *node, int slot)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root_ref *ref;
	struct btrfs_root_ref *backref;
	char ref_name[BTRFS_NAME_LEN] = {0};
	char backref_name[BTRFS_NAME_LEN] = {0};
	u64 ref_dirid;
	u64 ref_seq;
	u32 ref_namelen;
	u64 backref_dirid;
	u64 backref_seq;
	u32 backref_namelen;
	u32 len;
	int ret;
	int err = 0;

	ref = btrfs_item_ptr(node, slot, struct btrfs_root_ref);
	ref_dirid = btrfs_root_ref_dirid(node, ref);
	ref_seq = btrfs_root_ref_sequence(node, ref);
	ref_namelen = btrfs_root_ref_name_len(node, ref);

	if (ref_namelen <= BTRFS_NAME_LEN) {
		len = ref_namelen;
	} else {
		len = BTRFS_NAME_LEN;
		warning("%s[%llu %llu] ref_name too long",
			ref_key->type == BTRFS_ROOT_REF_KEY ?
			"ROOT_REF" : "ROOT_BACKREF", ref_key->objectid,
			ref_key->offset);
	}
	read_extent_buffer(node, ref_name, (unsigned long)(ref + 1), len);

	/* Find relative root_ref */
	key.objectid = ref_key->offset;
	key.type = BTRFS_ROOT_BACKREF_KEY + BTRFS_ROOT_REF_KEY - ref_key->type;
	key.offset = ref_key->objectid;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret) {
		err |= ROOT_REF_MISSING;
		error("%s[%llu %llu] couldn't find relative ref",
		      ref_key->type == BTRFS_ROOT_REF_KEY ?
		      "ROOT_REF" : "ROOT_BACKREF",
		      ref_key->objectid, ref_key->offset);
		goto out;
	}

	backref = btrfs_item_ptr(path.nodes[0], path.slots[0],
				 struct btrfs_root_ref);
	backref_dirid = btrfs_root_ref_dirid(path.nodes[0], backref);
	backref_seq = btrfs_root_ref_sequence(path.nodes[0], backref);
	backref_namelen = btrfs_root_ref_name_len(path.nodes[0], backref);

	if (backref_namelen <= BTRFS_NAME_LEN) {
		len = backref_namelen;
	} else {
		len = BTRFS_NAME_LEN;
		warning("%s[%llu %llu] ref_name too long",
			key.type == BTRFS_ROOT_REF_KEY ?
			"ROOT_REF" : "ROOT_BACKREF",
			key.objectid, key.offset);
	}
	read_extent_buffer(path.nodes[0], backref_name,
			   (unsigned long)(backref + 1), len);

	if (ref_dirid != backref_dirid || ref_seq != backref_seq ||
	    ref_namelen != backref_namelen ||
	    strncmp(ref_name, backref_name, len)) {
		err |= ROOT_REF_MISMATCH;
		error("%s[%llu %llu] mismatch relative ref",
		      ref_key->type == BTRFS_ROOT_REF_KEY ?
		      "ROOT_REF" : "ROOT_BACKREF",
		      ref_key->objectid, ref_key->offset);
	}
out:
	btrfs_release_path(&path);
	return err;
}

/*
 * Check all fs/file tree in low_memory mode.
 *
 * 1. for fs tree root item, call check_fs_root_v2()
 * 2. for fs tree root ref/backref, call check_root_ref()
 *
 * Return 0 if no error occurred.
 */
static int check_fs_roots_v2(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *cur_root = NULL;
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *node;
	unsigned int ext_ref;
	int slot;
	int ret;
	int err = 0;

	ext_ref = btrfs_fs_incompat(fs_info, EXTENDED_IREF);

	btrfs_init_path(&path);
	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.offset = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;

	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	if (ret < 0) {
		err = ret;
		goto out;
	} else if (ret > 0) {
		err = -ENOENT;
		goto out;
	}

	while (1) {
		node = path.nodes[0];
		slot = path.slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);
		if (key.objectid > BTRFS_LAST_FREE_OBJECTID)
			goto out;
		if (key.type == BTRFS_ROOT_ITEM_KEY &&
		    fs_root_objectid(key.objectid)) {
			if (key.objectid == BTRFS_TREE_RELOC_OBJECTID) {
				cur_root = btrfs_read_fs_root_no_cache(fs_info,
								       &key);
			} else {
				key.offset = (u64)-1;
				cur_root = btrfs_read_fs_root(fs_info, &key);
			}

			if (IS_ERR(cur_root)) {
				error("Fail to read fs/subvol tree: %lld",
				      key.objectid);
				err = -EIO;
				goto next;
			}

			ret = check_fs_root_v2(cur_root, ext_ref);
			err |= ret;

			if (key.objectid == BTRFS_TREE_RELOC_OBJECTID)
				btrfs_free_fs_root(cur_root);
		} else if (key.type == BTRFS_ROOT_REF_KEY ||
				key.type == BTRFS_ROOT_BACKREF_KEY) {
			ret = check_root_ref(tree_root, &key, node, slot);
			err |= ret;
		}
next:
		ret = btrfs_next_item(tree_root, &path);
		if (ret > 0)
			goto out;
		if (ret < 0) {
			err = ret;
			goto out;
		}
	}

out:
	btrfs_release_path(&path);
	return err;
}

static int do_check_fs_roots(struct btrfs_fs_info *fs_info,
			  struct cache_tree *root_cache)
{
	int ret;

	if (!ctx.progress_enabled)
		fprintf(stderr, "checking fs roots\n");
	if (check_mode == CHECK_MODE_LOWMEM)
		ret = check_fs_roots_v2(fs_info);
	else
		ret = check_fs_roots(fs_info, root_cache);

	return ret;
}

static int all_backpointers_checked(struct extent_record *rec, int print_errs)
{
	struct extent_backref *back, *tmp;
	struct tree_backref *tback;
	struct data_backref *dback;
	u64 found = 0;
	int err = 0;

	rbtree_postorder_for_each_entry_safe(back, tmp,
					     &rec->backref_tree, node) {
		if (!back->found_extent_tree) {
			err = 1;
			if (!print_errs)
				goto out;
			if (back->is_data) {
				dback = to_data_backref(back);
				fprintf(stderr, "Data backref %llu %s %llu"
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
				tback = to_tree_backref(back);
				fprintf(stderr, "Tree backref %llu parent %llu"
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
			tback = to_tree_backref(back);
			fprintf(stderr, "Backref %llu %s %llu not referenced back %p\n",
				(unsigned long long)rec->start,
				back->full_backref ? "parent" : "root",
				back->full_backref ?
				(unsigned long long)tback->parent :
				(unsigned long long)tback->root, back);
		}
		if (back->is_data) {
			dback = to_data_backref(back);
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
			dback = to_data_backref(back);
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

static void __free_one_backref(struct rb_node *node)
{
	struct extent_backref *back = rb_node_to_extent_backref(node);

	free(back);
}

static void free_all_extent_backrefs(struct extent_record *rec)
{
	rb_free_nodes(&rec->backref_tree, __free_one_backref);
}

static void free_extent_record_cache(struct cache_tree *extent_cache)
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
	struct extent_backref *node, *tmp;
	struct tree_backref *back;
	struct btrfs_root *ref_root;
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *parent;
	int level;
	int found = 0;
	int ret;

	rbtree_postorder_for_each_entry_safe(node, tmp,
					     &rec->backref_tree, node) {
		if (node->is_data)
			continue;
		if (!node->found_ref)
			continue;
		if (node->full_backref)
			continue;
		back = to_tree_backref(node);
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
	struct extent_backref *node, *tmp;
	struct tree_backref *back;
	int is_extent = 0;

	rbtree_postorder_for_each_entry_safe(node, tmp,
					     &rec->backref_tree, node) {
		if (node->is_data)
			return 0;
		back = to_tree_backref(node);
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

static int fix_key_order(struct btrfs_root *root, struct btrfs_path *path)
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

static int delete_bogus_item(struct btrfs_root *root,
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

static int fix_item_offset(struct btrfs_root *root, struct btrfs_path *path)
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
				ret = delete_bogus_item(root, path, buf, i);
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
				ret = delete_bogus_item(root, path, buf, i);
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
	struct btrfs_path path;
	struct ulist_iterator iter;
	struct btrfs_key root_key, key;
	int ret;

	if (status != BTRFS_TREE_BLOCK_BAD_KEY_ORDER &&
	    status != BTRFS_TREE_BLOCK_INVALID_OFFSETS)
		return -EIO;

	ret = btrfs_find_all_roots(NULL, root->fs_info, buf->start, 0, &roots);
	if (ret)
		return -EIO;

	btrfs_init_path(&path);
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

		path.lowest_level = btrfs_header_level(buf);
		path.skip_check_block = 1;
		if (path.lowest_level)
			btrfs_node_key_to_cpu(buf, &key, 0);
		else
			btrfs_item_key_to_cpu(buf, &key, 0);
		ret = btrfs_search_slot(trans, search_root, &key, &path, 0, 1);
		if (ret) {
			ret = -EIO;
			btrfs_commit_transaction(trans, search_root);
			break;
		}
		if (status == BTRFS_TREE_BLOCK_BAD_KEY_ORDER)
			ret = fix_key_order(search_root, &path);
		else if (status == BTRFS_TREE_BLOCK_INVALID_OFFSETS)
			ret = fix_item_offset(search_root, &path);
		if (ret) {
			btrfs_commit_transaction(trans, search_root);
			break;
		}
		btrfs_release_path(&path);
		btrfs_commit_transaction(trans, search_root);
	}
	ulist_free(roots);
	btrfs_release_path(&path);
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
			 * again since we'll have cowed blocks.
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

#if 0
static struct tree_backref *find_tree_backref(struct extent_record *rec,
						u64 parent, u64 root)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *node;
	struct tree_backref *back;

	while(cur != &rec->backrefs) {
		node = to_extent_backref(cur);
		cur = cur->next;
		if (node->is_data)
			continue;
		back = to_tree_backref(node);
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
#endif

static struct tree_backref *alloc_tree_backref(struct extent_record *rec,
						u64 parent, u64 root)
{
	struct tree_backref *ref = malloc(sizeof(*ref));

	if (!ref)
		return NULL;
	memset(&ref->node, 0, sizeof(ref->node));
	if (parent > 0) {
		ref->parent = parent;
		ref->node.full_backref = 1;
	} else {
		ref->root = root;
		ref->node.full_backref = 0;
	}

	return ref;
}

#if 0
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
		node = to_extent_backref(cur);
		cur = cur->next;
		if (!node->is_data)
			continue;
		back = to_data_backref(node);
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
#endif

static struct data_backref *alloc_data_backref(struct extent_record *rec,
						u64 parent, u64 root,
						u64 owner, u64 offset,
						u64 max_size)
{
	struct data_backref *ref = malloc(sizeof(*ref));

	if (!ref)
		return NULL;
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
	if (!RB_EMPTY_ROOT(&rec->backref_tree)) {
		struct extent_backref *node;
		struct tree_backref *tback;
		u64 bg_type;

		node = rb_node_to_extent_backref(rb_first(&rec->backref_tree));
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

/*
 * Allocate a new extent record, fill default values from @tmpl and insert int
 * @extent_cache. Caller is supposed to make sure the [start,nr) is not in
 * the cache, otherwise it fails.
 */
static int add_extent_rec_nolookup(struct cache_tree *extent_cache,
		struct extent_record *tmpl)
{
	struct extent_record *rec;
	int ret = 0;

	BUG_ON(tmpl->max_size == 0);
	rec = malloc(sizeof(*rec));
	if (!rec)
		return -ENOMEM;
	rec->start = tmpl->start;
	rec->max_size = tmpl->max_size;
	rec->nr = max(tmpl->nr, tmpl->max_size);
	rec->found_rec = tmpl->found_rec;
	rec->content_checked = tmpl->content_checked;
	rec->owner_ref_checked = tmpl->owner_ref_checked;
	rec->num_duplicates = 0;
	rec->metadata = tmpl->metadata;
	rec->flag_block_full_backref = FLAG_UNSET;
	rec->bad_full_backref = 0;
	rec->crossing_stripes = 0;
	rec->wrong_chunk_type = 0;
	rec->is_root = tmpl->is_root;
	rec->refs = tmpl->refs;
	rec->extent_item_refs = tmpl->extent_item_refs;
	rec->parent_generation = tmpl->parent_generation;
	INIT_LIST_HEAD(&rec->backrefs);
	INIT_LIST_HEAD(&rec->dups);
	INIT_LIST_HEAD(&rec->list);
	rec->backref_tree = RB_ROOT;
	memcpy(&rec->parent_key, &tmpl->parent_key, sizeof(tmpl->parent_key));
	rec->cache.start = tmpl->start;
	rec->cache.size = tmpl->nr;
	ret = insert_cache_extent(extent_cache, &rec->cache);
	if (ret) {
		free(rec);
		return ret;
	}
	bytes_used += rec->nr;

	if (tmpl->metadata)
		rec->crossing_stripes = check_crossing_stripes(global_info,
				rec->start, global_info->nodesize);
	check_extent_type(rec);
	return ret;
}

/*
 * Lookup and modify an extent, some values of @tmpl are interpreted verbatim,
 * some are hints:
 * - refs              - if found, increase refs
 * - is_root           - if found, set
 * - content_checked   - if found, set
 * - owner_ref_checked - if found, set
 *
 * If not found, create a new one, initialize and insert.
 */
static int add_extent_rec(struct cache_tree *extent_cache,
		struct extent_record *tmpl)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int ret = 0;
	int dup = 0;

	cache = lookup_cache_extent(extent_cache, tmpl->start, tmpl->nr);
	if (cache) {
		rec = container_of(cache, struct extent_record, cache);
		if (tmpl->refs)
			rec->refs++;
		if (rec->nr == 1)
			rec->nr = max(tmpl->nr, tmpl->max_size);

		/*
		 * We need to make sure to reset nr to whatever the extent
		 * record says was the real size, this way we can compare it to
		 * the backrefs.
		 */
		if (tmpl->found_rec) {
			if (tmpl->start != rec->start || rec->found_rec) {
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
				tmp->start = tmpl->start;
				tmp->max_size = tmpl->max_size;
				tmp->nr = tmpl->nr;
				tmp->found_rec = 1;
				tmp->metadata = tmpl->metadata;
				tmp->extent_item_refs = tmpl->extent_item_refs;
				INIT_LIST_HEAD(&tmp->list);
				list_add_tail(&tmp->list, &rec->dups);
				rec->num_duplicates++;
			} else {
				rec->nr = tmpl->nr;
				rec->found_rec = 1;
			}
		}

		if (tmpl->extent_item_refs && !dup) {
			if (rec->extent_item_refs) {
				fprintf(stderr, "block %llu rec "
					"extent_item_refs %llu, passed %llu\n",
					(unsigned long long)tmpl->start,
					(unsigned long long)
							rec->extent_item_refs,
					(unsigned long long)tmpl->extent_item_refs);
			}
			rec->extent_item_refs = tmpl->extent_item_refs;
		}
		if (tmpl->is_root)
			rec->is_root = 1;
		if (tmpl->content_checked)
			rec->content_checked = 1;
		if (tmpl->owner_ref_checked)
			rec->owner_ref_checked = 1;
		memcpy(&rec->parent_key, &tmpl->parent_key,
				sizeof(tmpl->parent_key));
		if (tmpl->parent_generation)
			rec->parent_generation = tmpl->parent_generation;
		if (rec->max_size < tmpl->max_size)
			rec->max_size = tmpl->max_size;

		/*
		 * A metadata extent can't cross stripe_len boundary, otherwise
		 * kernel scrub won't be able to handle it.
		 * As now stripe_len is fixed to BTRFS_STRIPE_LEN, just check
		 * it.
		 */
		if (tmpl->metadata)
			rec->crossing_stripes = check_crossing_stripes(
					global_info, rec->start,
					global_info->nodesize);
		check_extent_type(rec);
		maybe_free_extent_rec(extent_cache, rec);
		return ret;
	}

	ret = add_extent_rec_nolookup(extent_cache, tmpl);

	return ret;
}

static int add_tree_backref(struct cache_tree *extent_cache, u64 bytenr,
			    u64 parent, u64 root, int found_ref)
{
	struct extent_record *rec;
	struct tree_backref *back;
	struct cache_extent *cache;
	int ret;
	bool insert = false;

	cache = lookup_cache_extent(extent_cache, bytenr, 1);
	if (!cache) {
		struct extent_record tmpl;

		memset(&tmpl, 0, sizeof(tmpl));
		tmpl.start = bytenr;
		tmpl.nr = 1;
		tmpl.metadata = 1;
		tmpl.max_size = 1;

		ret = add_extent_rec_nolookup(extent_cache, &tmpl);
		if (ret)
			return ret;

		/* really a bug in cache_extent implement now */
		cache = lookup_cache_extent(extent_cache, bytenr, 1);
		if (!cache)
			return -ENOENT;
	}

	rec = container_of(cache, struct extent_record, cache);
	if (rec->start != bytenr) {
		/*
		 * Several cause, from unaligned bytenr to over lapping extents
		 */
		return -EEXIST;
	}

	back = find_tree_backref(rec, parent, root);
	if (!back) {
		back = alloc_tree_backref(rec, parent, root);
		if (!back)
			return -ENOMEM;
		insert = true;
	}

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
	if (insert)
		WARN_ON(rb_insert(&rec->backref_tree, &back->node.node,
			compare_extent_backref));
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
	int ret;
	bool insert = false;

	cache = lookup_cache_extent(extent_cache, bytenr, 1);
	if (!cache) {
		struct extent_record tmpl;

		memset(&tmpl, 0, sizeof(tmpl));
		tmpl.start = bytenr;
		tmpl.nr = 1;
		tmpl.max_size = max_size;

		ret = add_extent_rec_nolookup(extent_cache, &tmpl);
		if (ret)
			return ret;

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
	if (!back) {
		back = alloc_data_backref(rec, parent, root, owner, offset,
					  max_size);
		BUG_ON(!back);
		insert = true;
	}

	if (found_ref) {
		BUG_ON(num_refs != 1);
		if (back->node.found_ref)
			BUG_ON(back->bytes != max_size);
		back->node.found_ref = 1;
		back->found_ref += 1;
		if (back->bytes != max_size || back->disk_bytenr != bytenr) {
			back->bytes = max_size;
			back->disk_bytenr = bytenr;

			/* Need to reinsert if not already in the tree */
			if (!insert) {
				rb_erase(&back->node.node, &rec->backref_tree);
				insert = true;
			}
		}
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
	if (insert)
		WARN_ON(rb_insert(&rec->backref_tree, &back->node.node,
			compare_extent_backref));

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
	int ret;

	btrfs_item_key_to_cpu(leaf, &key, slot);
	ref0 = btrfs_item_ptr(leaf, slot, struct btrfs_extent_ref_v0);
	if (btrfs_ref_objectid_v0(leaf, ref0) < BTRFS_FIRST_FREE_OBJECTID) {
		ret = add_tree_backref(extent_cache, key.objectid, key.offset,
				0, 0);
	} else {
		ret = add_data_backref(extent_cache, key.objectid, key.offset,
				0, 0, 0, btrfs_ref_count_v0(leaf, ref0), 0, 0);
	}
	return ret;
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
	struct btrfs_chunk *chunk;
	int ret = 0;

	chunk = btrfs_item_ptr(eb, slot, struct btrfs_chunk);
	/*
	 * Do extra check for this chunk item,
	 *
	 * It's still possible one can craft a leaf with CHUNK_ITEM, with
	 * wrong onwer(3) out of chunk tree, to pass both chunk tree check
	 * and owner<->key_type check.
	 */
	ret = btrfs_check_chunk_valid(global_info, eb, chunk, slot,
				      key->offset);
	if (ret < 0) {
		error("chunk(%llu, %llu) is not valid, ignore it",
		      key->offset, btrfs_chunk_length(eb, chunk));
		return 0;
	}
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
	struct extent_record tmpl;
	unsigned long end;
	unsigned long ptr;
	int ret;
	int type;
	u32 item_size = btrfs_item_size_nr(eb, slot);
	u64 refs = 0;
	u64 offset;
	u64 num_bytes;
	int metadata = 0;

	btrfs_item_key_to_cpu(eb, &key, slot);

	if (key.type == BTRFS_METADATA_ITEM_KEY) {
		metadata = 1;
		num_bytes = root->fs_info->nodesize;
	} else {
		num_bytes = key.offset;
	}

	if (!IS_ALIGNED(key.objectid, root->fs_info->sectorsize)) {
		error("ignoring invalid extent, bytenr %llu is not aligned to %u",
		      key.objectid, root->fs_info->sectorsize);
		return -EIO;
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
		memset(&tmpl, 0, sizeof(tmpl));
		tmpl.start = key.objectid;
		tmpl.nr = num_bytes;
		tmpl.extent_item_refs = refs;
		tmpl.metadata = metadata;
		tmpl.found_rec = 1;
		tmpl.max_size = num_bytes;

		return add_extent_rec(extent_cache, &tmpl);
	}

	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
	refs = btrfs_extent_refs(eb, ei);
	if (btrfs_extent_flags(eb, ei) & BTRFS_EXTENT_FLAG_TREE_BLOCK)
		metadata = 1;
	else
		metadata = 0;
	if (metadata && num_bytes != root->fs_info->nodesize) {
		error("ignore invalid metadata extent, length %llu does not equal to %u",
		      num_bytes, root->fs_info->nodesize);
		return -EIO;
	}
	if (!metadata && !IS_ALIGNED(num_bytes, root->fs_info->sectorsize)) {
		error("ignore invalid data extent, length %llu is not aligned to %u",
		      num_bytes, root->fs_info->sectorsize);
		return -EIO;
	}

	memset(&tmpl, 0, sizeof(tmpl));
	tmpl.start = key.objectid;
	tmpl.nr = num_bytes;
	tmpl.extent_item_refs = refs;
	tmpl.metadata = metadata;
	tmpl.found_rec = 1;
	tmpl.max_size = num_bytes;
	add_extent_rec(extent_cache, &tmpl);

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
			ret = add_tree_backref(extent_cache, key.objectid,
					0, offset, 0);
			if (ret < 0)
				error(
			"add_tree_backref failed (extent items tree block): %s",
				      strerror(-ret));
			break;
		case BTRFS_SHARED_BLOCK_REF_KEY:
			ret = add_tree_backref(extent_cache, key.objectid,
					offset, 0, 0);
			if (ret < 0)
				error(
			"add_tree_backref failed (extent items shared block): %s",
				      strerror(-ret));
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
		ret = btrfs_rmap_block(root->fs_info,
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
					free(logical);
					return 0;
				}
				bytes -= stripe_len;
				offset += stripe_len;
			} else if (logical[nr] < offset) {
				if (logical[nr] + stripe_len >=
				    offset + bytes) {
					free(logical);
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
					free(logical);
					return ret;
				}

				/* Now we continue with the right side */
				bytes = (offset + bytes) -
					(logical[nr] + stripe_len);
				offset = logical[nr] + stripe_len;
			}
		}

		free(logical);
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
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 last;
	int ret = 0;

	root = root->fs_info->extent_root;

	last = max_t(u64, cache->key.objectid, BTRFS_SUPER_INFO_OFFSET);

	btrfs_init_path(&path);
	key.objectid = last;
	key.offset = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	ret = 0;
	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0)
				goto out;
			if (ret > 0) {
				ret = 0;
				break;
			}
		}
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid >= cache->key.offset + cache->key.objectid)
			break;
		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}

		if (last == key.objectid) {
			if (key.type == BTRFS_EXTENT_ITEM_KEY)
				last = key.objectid + key.offset;
			else
				last = key.objectid + root->fs_info->nodesize;
			path.slots[0]++;
			continue;
		}

		ret = check_cache_range(root, cache, last,
					key.objectid - last);
		if (ret)
			break;
		if (key.type == BTRFS_EXTENT_ITEM_KEY)
			last = key.objectid + key.offset;
		else
			last = key.objectid + root->fs_info->nodesize;
		path.slots[0]++;
	}

	if (last < cache->key.objectid + cache->key.offset)
		ret = check_cache_range(root, cache, last,
					cache->key.objectid +
					cache->key.offset - last);

out:
	btrfs_release_path(&path);

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
						root->fs_info->sectorsize)) {
				ret = -ENOMEM;
				break;
			}
		} else {
			btrfs_remove_free_space_cache(cache);
		}

		if (btrfs_fs_compat_ro(root->fs_info, FREE_SPACE_TREE)) {
			ret = exclude_super_stripes(root, cache);
			if (ret) {
				fprintf(stderr, "could not exclude super stripes: %s\n",
					strerror(-ret));
				error++;
				continue;
			}
			ret = load_free_space_tree(root->fs_info, cache);
			free_excluded_extents(root, cache);
			if (ret < 0) {
				fprintf(stderr, "could not load free space tree: %s\n",
					strerror(-ret));
				error++;
				continue;
			}
			error += ret;
		} else {
			ret = load_free_space_cache(root->fs_info, cache);
			if (!ret)
				continue;
		}

		ret = verify_space_cache(root, cache);
		if (ret) {
			fprintf(stderr, "cache appears valid but isn't %Lu\n",
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

	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 offset = 0;
	u16 csum_size = btrfs_super_csum_size(fs_info->super_copy);
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

	if (num_bytes % fs_info->sectorsize)
		return -EINVAL;

	data = malloc(num_bytes);
	if (!data)
		return -ENOMEM;

	while (offset < num_bytes) {
		mirror = 0;
again:
		read_len = num_bytes - offset;
		/* read as much space once a time */
		ret = read_extent_data(fs_info, data + offset,
				bytenr + offset, &read_len, mirror);
		if (ret)
			goto out;
		data_checked = 0;
		/* verify every 4k data's checksum */
		while (data_checked < read_len) {
			csum = ~(u32)0;
			tmp = offset + data_checked;

			csum = btrfs_csum_data((char *)data + tmp,
					       csum, fs_info->sectorsize);
			btrfs_csum_final(csum, (u8 *)&csum);

			csum_offset = leaf_offset +
				 tmp / fs_info->sectorsize * csum_size;
			read_extent_buffer(eb, (char *)&csum_expected,
					   csum_offset, csum_size);
			/* try another mirror */
			if (csum != csum_expected) {
				fprintf(stderr, "mirror %d bytenr %llu csum %u expected csum %u\n",
						mirror, bytenr + tmp,
						csum, csum_expected);
				num_copies = btrfs_num_copies(root->fs_info,
						bytenr, num_bytes);
				if (mirror < num_copies - 1) {
					mirror += 1;
					goto again;
				}
			}
			data_checked += fs_info->sectorsize;
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
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int ret;

	btrfs_init_path(&path);
	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

again:
	ret = btrfs_search_slot(NULL, root->fs_info->extent_root, &key, &path,
				0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error looking up extent record %d\n", ret);
		btrfs_release_path(&path);
		return ret;
	} else if (ret) {
		if (path.slots[0] > 0) {
			path.slots[0]--;
		} else {
			ret = btrfs_prev_leaf(root, &path);
			if (ret < 0) {
				goto out;
			} else if (ret > 0) {
				ret = 0;
				goto out;
			}
		}
	}

	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

	/*
	 * Block group items come before extent items if they have the same
	 * bytenr, so walk back one more just in case.  Dear future traveller,
	 * first congrats on mastering time travel.  Now if it's not too much
	 * trouble could you go back to 2006 and tell Chris to make the
	 * BLOCK_GROUP_ITEM_KEY (and BTRFS_*_REF_KEY) lower than the
	 * EXTENT_ITEM_KEY please?
	 */
	while (key.type > BTRFS_EXTENT_ITEM_KEY) {
		if (path.slots[0] > 0) {
			path.slots[0]--;
		} else {
			ret = btrfs_prev_leaf(root, &path);
			if (ret < 0) {
				goto out;
			} else if (ret > 0) {
				ret = 0;
				goto out;
			}
		}
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	}

	while (num_bytes) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf "
					"%d\n", ret);
				btrfs_release_path(&path);
				return ret;
			} else if (ret) {
				break;
			}
		}
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}
		if (key.objectid + key.offset < bytenr) {
			path.slots[0]++;
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
				btrfs_release_path(&path);
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
		path.slots[0]++;
	}
	ret = 0;

out:
	if (num_bytes && !ret) {
		fprintf(stderr, "There are no extents for csum range "
			"%Lu-%Lu\n", bytenr, bytenr+num_bytes);
		ret = 1;
	}

	btrfs_release_path(&path);
	return ret;
}

static int check_csums(struct btrfs_root *root)
{
	struct btrfs_path path;
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

	btrfs_init_path(&path);
	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.type = BTRFS_EXTENT_CSUM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching csum tree %d\n", ret);
		btrfs_release_path(&path);
		return ret;
	}

	if (ret > 0 && path.slots[0])
		path.slots[0]--;
	ret = 0;

	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf "
					"%d\n", ret);
				break;
			}
			if (ret)
				break;
		}
		leaf = path.nodes[0];

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_CSUM_KEY) {
			path.slots[0]++;
			continue;
		}

		data_len = (btrfs_item_size_nr(leaf, path.slots[0]) /
			      csum_size) * root->fs_info->sectorsize;
		if (!check_data_csum)
			goto skip_csum_check;
		leaf_offset = btrfs_item_ptr_offset(leaf, path.slots[0]);
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
		path.slots[0]++;
	}

	btrfs_release_path(&path);
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
 * 3) We cowed the block walking down a reloc tree.  This is impossible to tell
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
static int calc_extent_flag(struct cache_tree *extent_cache,
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
	if (!cache)
		return -ENOENT;

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
	if (rec->flag_block_full_backref != FLAG_UNSET &&
	    rec->flag_block_full_backref != 0)
		rec->bad_full_backref = 1;
	return 0;
full_backref:
	*flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
	if (rec->flag_block_full_backref != FLAG_UNSET &&
	    rec->flag_block_full_backref != 1)
		rec->bad_full_backref = 1;
	return 0;
}

static void report_mismatch_key_root(u8 key_type, u64 rootid)
{
	fprintf(stderr, "Invalid key type(");
	print_key_type(stderr, 0, key_type);
	fprintf(stderr, ") found in root(");
	print_objectid(stderr, rootid, 0);
	fprintf(stderr, ")\n");
}

/*
 * Check if the key is valid with its extent buffer.
 *
 * This is a early check in case invalid key exists in a extent buffer
 * This is not comprehensive yet, but should prevent wrong key/item passed
 * further
 */
static int check_type_with_root(u64 rootid, u8 key_type)
{
	switch (key_type) {
	/* Only valid in chunk tree */
	case BTRFS_DEV_ITEM_KEY:
	case BTRFS_CHUNK_ITEM_KEY:
		if (rootid != BTRFS_CHUNK_TREE_OBJECTID)
			goto err;
		break;
	/* valid in csum and log tree */
	case BTRFS_CSUM_TREE_OBJECTID:
		if (!(rootid == BTRFS_TREE_LOG_OBJECTID ||
		      is_fstree(rootid)))
			goto err;
		break;
	case BTRFS_EXTENT_ITEM_KEY:
	case BTRFS_METADATA_ITEM_KEY:
	case BTRFS_BLOCK_GROUP_ITEM_KEY:
		if (rootid != BTRFS_EXTENT_TREE_OBJECTID)
			goto err;
		break;
	case BTRFS_ROOT_ITEM_KEY:
		if (rootid != BTRFS_ROOT_TREE_OBJECTID)
			goto err;
		break;
	case BTRFS_DEV_EXTENT_KEY:
		if (rootid != BTRFS_DEV_TREE_OBJECTID)
			goto err;
		break;
	}
	return 0;
err:
	report_mismatch_key_root(key_type, rootid);
	return -EINVAL;
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
	struct btrfs_fs_info *fs_info = root->fs_info;
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
			readahead_tree_block(fs_info, bits[i].start, 0);
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
	buf = read_tree_block(root->fs_info, bytenr, gen);
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
			ret = calc_extent_flag(extent_cache, buf, ri, &flags);
			if (ret < 0) {
				fprintf(stderr, "Couldn't calc extent flags\n");
				flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
			}
		}
	} else {
		flags = 0;
		ret = calc_extent_flag(extent_cache, buf, ri, &flags);
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
			/*
			 * Check key type against the leaf owner.
			 * Could filter quite a lot of early error if
			 * owner is correct
			 */
			if (check_type_with_root(btrfs_header_owner(buf),
						 key.type)) {
				fprintf(stderr, "ignoring invalid key\n");
				continue;
			}
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
				ret = add_tree_backref(extent_cache,
						key.objectid, 0, key.offset, 0);
				if (ret < 0)
					error(
				"add_tree_backref failed (leaf tree block): %s",
					      strerror(-ret));
				continue;
			}
			if (key.type == BTRFS_SHARED_BLOCK_REF_KEY) {
				ret = add_tree_backref(extent_cache,
						key.objectid, key.offset, 0, 0);
				if (ret < 0)
					error(
				"add_tree_backref failed (leaf shared block): %s",
					      strerror(-ret));
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
					0, root->fs_info->sectorsize);
				continue;
			}
			if (key.type == BTRFS_SHARED_DATA_REF_KEY) {
				struct btrfs_shared_data_ref *ref;
				ref = btrfs_item_ptr(buf, i,
						struct btrfs_shared_data_ref);
				add_data_backref(extent_cache,
					key.objectid, key.offset, 0, 0, 0,
					btrfs_shared_data_ref_count(buf, ref),
					0, root->fs_info->sectorsize);
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
			if (data_bytes_allocated < root->fs_info->sectorsize) {
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
			struct extent_record tmpl;

			ptr = btrfs_node_blockptr(buf, i);
			size = root->fs_info->nodesize;
			btrfs_node_key_to_cpu(buf, &key, i);
			if (ri != NULL) {
				if ((level == ri->drop_level)
				    && is_dropped_key(&key, &ri->drop_key)) {
					continue;
				}
			}

			memset(&tmpl, 0, sizeof(tmpl));
			btrfs_cpu_key_to_disk(&tmpl.parent_key, &key);
			tmpl.parent_generation = btrfs_node_ptr_generation(buf, i);
			tmpl.start = ptr;
			tmpl.nr = size;
			tmpl.refs = 1;
			tmpl.metadata = 1;
			tmpl.max_size = size;
			ret = add_extent_rec(extent_cache, &tmpl);
			if (ret < 0)
				goto out;

			ret = add_tree_backref(extent_cache, ptr, parent,
					owner, 1);
			if (ret < 0) {
				error(
				"add_tree_backref failed (non-leaf block): %s",
				      strerror(-ret));
				continue;
			}

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
	struct extent_record tmpl;
	int ret;

	if (btrfs_header_level(buf) > 0)
		add_pending(nodes, seen, buf->start, buf->len);
	else
		add_pending(pending, seen, buf->start, buf->len);

	memset(&tmpl, 0, sizeof(tmpl));
	tmpl.start = buf->start;
	tmpl.nr = buf->len;
	tmpl.is_root = 1;
	tmpl.refs = 1;
	tmpl.metadata = 1;
	tmpl.max_size = buf->len;
	add_extent_rec(extent_cache, &tmpl);

	if (objectid == BTRFS_TREE_RELOC_OBJECTID ||
	    btrfs_header_backref_rev(buf) < BTRFS_MIXED_BACKREF_REV)
		ret = add_tree_backref(extent_cache, buf->start, buf->start,
				0, 1);
	else
		ret = add_tree_backref(extent_cache, buf->start, 0, objectid,
				1);
	return ret;
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
			rb_erase(&back->node.node, &rec->backref_tree);
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
			rb_erase(&back->node.node, &rec->backref_tree);
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
				 u64 bytenr)
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
				found_key.offset : root->fs_info->nodesize;

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
	int ret = 0;
	struct btrfs_root *extent_root = info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_key ins_key;
	struct btrfs_extent_item *ei;
	struct data_backref *dback;
	struct btrfs_tree_block_info *bi;

	if (!back->is_data)
		rec->max_size = max_t(u64, rec->max_size,
				    info->nodesize);

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

		dback = to_data_backref(back);
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
		struct tree_backref *tback;

		tback = to_tree_backref(back);
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
fail:
	btrfs_release_path(path);
	return ret;
}

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
		/*
		 * If there are as many broken entries as entries then we know
		 * not to trust this particular entry.
		 */
		if (entry->broken == entry->count)
			continue;

		/*
		 * Special case, when there are only two entries and 'best' is
		 * the first one
		 */
		if (!prev) {
			best = entry;
			prev = entry;
			continue;
		}

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
	struct extent_backref *back, *tmp;
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

	rbtree_postorder_for_each_entry_safe(back, tmp,
					     &rec->backref_tree, node) {
		if (back->full_backref || !back->is_data)
			continue;

		dback = to_data_backref(back);

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
	rbtree_postorder_for_each_entry_safe(back, tmp,
					     &rec->backref_tree, node) {
		if (back->full_backref || !back->is_data)
			continue;

		dback = to_data_backref(back);

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

static int process_duplicates(struct cache_tree *extent_cache,
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

	good = to_extent_record(rec->dups.next);
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
	struct btrfs_path path;
	struct extent_record *tmp, *good, *n;
	int nr_del = 0;
	int ret = 0, err;
	struct btrfs_key key;

	btrfs_init_path(&path);

	good = rec;
	/* Find the record that covers all of the duplicates. */
	list_for_each_entry(tmp, &rec->dups, list) {
		if (good->start < tmp->start)
			continue;
		if (good->nr > tmp->nr)
			continue;

		if (tmp->start + tmp->nr < good->start + good->nr) {
			fprintf(stderr, "Ok we have overlapping extents that "
				"aren't completely covered by each other, this "
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

		ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
		if (ret) {
			if (ret > 0)
				ret = -EINVAL;
			break;
		}
		ret = btrfs_del_item(trans, root, &path);
		if (ret)
			break;
		btrfs_release_path(&path);
		nr_del++;
	}
	err = btrfs_commit_transaction(trans, root);
	if (err && !ret)
		ret = err;
out:
	while (!list_empty(&delete_list)) {
		tmp = to_extent_record(delete_list.next);
		list_del_init(&tmp->list);
		if (tmp == rec)
			continue;
		free(tmp);
	}

	while (!list_empty(&rec->dups)) {
		tmp = to_extent_record(rec->dups.next);
		list_del_init(&tmp->list);
		free(tmp);
	}

	btrfs_release_path(&path);

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
	struct extent_backref *back, *tmp;
	struct data_backref *dback;
	struct cache_extent *cache;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key key;
	u64 bytenr, bytes;
	int ret;

	rbtree_postorder_for_each_entry_safe(back, tmp,
					     &rec->backref_tree, node) {
		/* Don't care about full backrefs (poor unloved backrefs) */
		if (back->full_backref || !back->is_data)
			continue;

		dback = to_data_backref(back);

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
	struct extent_backref *back, *tmp;
	struct data_backref *dback;
	struct orphan_data_extent *orphan;
	struct btrfs_path path;
	int recorded_data_ref = 0;
	int ret = 0;

	if (rec->metadata)
		return 1;
	btrfs_init_path(&path);
	rbtree_postorder_for_each_entry_safe(back, tmp,
					     &rec->backref_tree, node) {
		if (back->full_backref || !back->is_data ||
		    !back->found_extent_tree)
			continue;
		dback = to_data_backref(back);
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

		ret = btrfs_search_slot(NULL, dest_root, &key, &path, 0, 0);
		btrfs_release_path(&path);
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
	btrfs_release_path(&path);
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
	struct btrfs_path path;
	struct cache_extent *cache;
	struct extent_backref *back, *tmp;
	int allocated = 0;
	u64 flags = 0;

	if (rec->flag_block_full_backref)
		flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;

	btrfs_init_path(&path);
	if (rec->refs != rec->extent_item_refs && !rec->metadata) {
		/*
		 * Sometimes the backrefs themselves are so broken they don't
		 * get attached to any meaningful rec, so first go back and
		 * check any of our backrefs that we couldn't find and throw
		 * them into the list if we find the backref so that
		 * verify_backrefs can figure out what to do.
		 */
		ret = find_possible_backrefs(info, &path, extent_cache, rec);
		if (ret < 0)
			goto out;
	}

	/* step one, make sure all of the backrefs agree */
	ret = verify_backrefs(info, &path, rec);
	if (ret < 0)
		goto out;

	trans = btrfs_start_transaction(info->extent_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}

	/* step two, delete all the existing records */
	ret = delete_extent_records(trans, info->extent_root, &path,
				    rec->start);

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
	rbtree_postorder_for_each_entry_safe(back, tmp,
					     &rec->backref_tree, node) {
		/*
		 * if we didn't find any references, don't create a
		 * new extent record
		 */
		if (!back->found_ref)
			continue;

		rec->bad_full_backref = 0;
		ret = record_extent(trans, info, &path, rec, back, allocated, flags);
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

	if (!ret)
		fprintf(stderr, "Repaired extent references for %llu\n",
				(unsigned long long)rec->start);

	btrfs_release_path(&path);
	return ret;
}

static int fixup_extent_flags(struct btrfs_fs_info *fs_info,
			      struct extent_record *rec)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root = fs_info->extent_root;
	struct btrfs_path path;
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

	trans = btrfs_start_transaction(root, 0);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	btrfs_init_path(&path);
	ret = btrfs_search_slot(trans, root, &key, &path, 0, 1);
	if (ret < 0) {
		btrfs_release_path(&path);
		btrfs_commit_transaction(trans, root);
		return ret;
	} else if (ret) {
		fprintf(stderr, "Didn't find extent for %llu\n",
			(unsigned long long)rec->start);
		btrfs_release_path(&path);
		btrfs_commit_transaction(trans, root);
		return -ENOENT;
	}

	ei = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_extent_item);
	flags = btrfs_extent_flags(path.nodes[0], ei);
	if (rec->flag_block_full_backref) {
		fprintf(stderr, "setting full backref on %llu\n",
			(unsigned long long)key.objectid);
		flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
	} else {
		fprintf(stderr, "clearing full backref on %llu\n",
			(unsigned long long)key.objectid);
		flags &= ~BTRFS_BLOCK_FLAG_FULL_BACKREF;
	}
	btrfs_set_extent_flags(path.nodes[0], ei, flags);
	btrfs_mark_buffer_dirty(path.nodes[0]);
	btrfs_release_path(&path);
	ret = btrfs_commit_transaction(trans, root);
	if (!ret)
		fprintf(stderr, "Repaired extent flags for %llu\n",
				(unsigned long long)rec->start);

	return ret;
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
	ret = btrfs_del_ptr(info->extent_root, &path, level, slot);

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
		clear_extent_dirty(&fs_info->free_space_cache, start, end);
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
	int ret = 0;
	int had_dups = 0;
	int err = 0;

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
					 rec->start + rec->max_size - 1);
			cache = next_cache_extent(cache);
		}

		/* pin down all the corrupted blocks too */
		cache = search_cache_extent(root->fs_info->corrupt_blocks, 0);
		while(cache) {
			set_extent_dirty(root->fs_info->excluded_extents,
					 cache->start,
					 cache->start + cache->size - 1);
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
		rec = to_extent_record(duplicate_extents.next);
		list_del_init(&rec->list);

		/* Sometimes we can find a backref before we find an actual
		 * extent, so we need to process it a little bit to see if there
		 * truly are multiple EXTENT_ITEM_KEY's for the same range, or
		 * if this is a backref screwup.  If we need to delete stuff
		 * process_duplicates() will return 0, otherwise it will return
		 * 1 and we
		 */
		if (process_duplicates(extent_cache, rec))
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
		int fix = 0;

		cache = search_cache_extent(extent_cache, 0);
		if (!cache)
			break;
		rec = container_of(cache, struct extent_record, cache);
		if (rec->num_duplicates) {
			fprintf(stderr, "extent item %llu has multiple extent "
				"items\n", (unsigned long long)rec->start);
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
			fix = ret;
			cur_err = 1;
		}
		if (all_backpointers_checked(rec, 1)) {
			fprintf(stderr, "backpointer mismatch on [%llu %llu]\n",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);
			fix = 1;
			cur_err = 1;
		}
		if (!rec->owner_ref_checked) {
			fprintf(stderr, "owner ref check failed [%llu %llu]\n",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);
			fix = 1;
			cur_err = 1;
		}

		if (repair && fix) {
			ret = fixup_extent_refs(root->fs_info, extent_cache, rec);
			if (ret)
				goto repair_abort;
		}


		if (rec->bad_full_backref) {
			fprintf(stderr, "bad full backref, on [%llu]\n",
				(unsigned long long)rec->start);
			if (repair) {
				ret = fixup_extent_flags(root->fs_info, rec);
				if (ret)
					goto repair_abort;
				fix = 1;
			}
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
			cur_err = 1;
		}

		if (rec->wrong_chunk_type) {
			fprintf(stderr,
				"bad extent [%llu, %llu), type mismatch with chunk\n",
				rec->start, rec->start + rec->max_size);
			cur_err = 1;
		}

		err = cur_err;
		remove_cache_extent(extent_cache, cache);
		free_all_extent_backrefs(rec);
		if (!init_extent_tree && repair && (!cur_err || fix))
			clear_extent_dirty(root->fs_info->excluded_extents,
					   rec->start,
					   rec->start + rec->max_size - 1);
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

			ret = btrfs_fix_block_accounting(trans, root);
			if (ret)
				goto repair_abort;
			ret = btrfs_commit_transaction(trans, root);
			if (ret)
				goto repair_abort;
		}
		return ret;
	}

	if (err)
		err = -EIO;
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

/*
 * Extra (optional) check for dev_item size to report possbile problem on a new
 * kernel.
 */
static void check_dev_size_alignment(u64 devid, u64 total_bytes, u32 sectorsize)
{
	if (!IS_ALIGNED(total_bytes, sectorsize)) {
		warning(
"unaligned total_bytes detected for devid %llu, have %llu should be aligned to %u",
			devid, total_bytes, sectorsize);
		warning(
"this is OK for older kernel, but may cause kernel warning for newer kernels");
		warning("this can be fixed by 'btrfs rescue fix-device-size'");
	}
}

/*
 * Unlike device size alignment check above, some super total_bytes check
 * failure can lead to mount failure for newer kernel.
 *
 * So this function will return the error for a fatal super total_bytes problem.
 */
static bool is_super_size_valid(struct btrfs_fs_info *fs_info)
{
	struct btrfs_device *dev;
	struct list_head *dev_list = &fs_info->fs_devices->devices;
	u64 total_bytes = 0;
	u64 super_bytes = btrfs_super_total_bytes(fs_info->super_copy);

	list_for_each_entry(dev, dev_list, dev_list)
		total_bytes += dev->total_bytes;

	/* Important check, which can cause unmountable fs */
	if (super_bytes < total_bytes) {
		error("super total bytes %llu smaller than real device(s) size %llu",
			super_bytes, total_bytes);
		error("mounting this fs may fail for newer kernels");
		error("this can be fixed by 'btrfs rescue fix-device-size'");
		return false;
	}

	/*
	 * Optional check, just to make everything aligned and match with each
	 * other.
	 *
	 * For a btrfs-image restored fs, we don't need to check it anyway.
	 */
	if (btrfs_super_flags(fs_info->super_copy) &
	    (BTRFS_SUPER_FLAG_METADUMP | BTRFS_SUPER_FLAG_METADUMP_V2))
		return true;
	if (!IS_ALIGNED(super_bytes, fs_info->sectorsize) ||
	    !IS_ALIGNED(total_bytes, fs_info->sectorsize) ||
	    super_bytes != total_bytes) {
		warning("minor unaligned/mismatch device size detected");
		warning(
		"recommended to use 'btrfs rescue fix-device-size' to fix it");
	}
	return true;
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

		check_dev_size_alignment(dev_rec->devid, dev_rec->total_byte,
					 global_info->sectorsize);
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
				  struct btrfs_key *drop_key)
{

	struct root_item_record *ri_rec;
	ri_rec = malloc(sizeof(*ri_rec));
	if (!ri_rec)
		return -ENOMEM;
	ri_rec->bytenr = bytenr;
	ri_rec->objectid = objectid;
	ri_rec->level = level;
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
		buf = read_tree_block(root->fs_info, rec->bytenr, 0);
		if (!extent_buffer_uptodate(buf)) {
			free_extent_buffer(buf);
			ret = -EIO;
			break;
		}
		ret = add_root_to_pending(buf, extent_cache, pending,
				    seen, nodes, rec->objectid);
		if (ret < 0)
			break;
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

static int check_chunks_and_extents(struct btrfs_fs_info *fs_info)
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
	struct btrfs_root *root;
	u64 objectid;
	u8 level;

	root = fs_info->fs_root;
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
		fs_info->excluded_extents = &excluded_extents;
		fs_info->fsck_extent_cache = &extent_cache;
		fs_info->free_extent_hook = free_extent_hook;
		fs_info->corrupt_blocks = &corrupt_blocks;
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
	root1 = fs_info->tree_root;
	level = btrfs_header_level(root1->node);
	ret = add_root_item_to_list(&normal_trees, root1->root_key.objectid,
				    root1->node->start, 0, level, 0, NULL);
	if (ret < 0)
		goto out;
	root1 = fs_info->chunk_root;
	level = btrfs_header_level(root1->node);
	ret = add_root_item_to_list(&normal_trees, root1->root_key.objectid,
				    root1->node->start, 0, level, 0, NULL);
	if (ret < 0)
		goto out;
	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, fs_info->tree_root, &key, &path, 0, 0);
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
		if (found_key.type == BTRFS_ROOT_ITEM_KEY) {
			unsigned long offset;
			u64 last_snapshot;

			offset = btrfs_item_ptr_offset(leaf, path.slots[0]);
			read_extent_buffer(leaf, &ri, offset, sizeof(ri));
			last_snapshot = btrfs_root_last_snapshot(&ri);
			if (btrfs_disk_key_objectid(&ri.drop_progress) == 0) {
				level = btrfs_root_level(&ri);
				ret = add_root_item_to_list(&normal_trees,
						found_key.objectid,
						btrfs_root_bytenr(&ri),
						last_snapshot, level,
						0, NULL);
				if (ret < 0)
					goto out;
			} else {
				level = btrfs_root_level(&ri);
				objectid = found_key.objectid;
				btrfs_disk_key_to_cpu(&found_key,
						      &ri.drop_progress);
				ret = add_root_item_to_list(&dropping_trees,
						objectid,
						btrfs_root_bytenr(&ri),
						last_snapshot, level,
						ri.drop_level, &found_key);
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

	ret = check_chunks(&chunk_cache, &block_group_cache,
			   &dev_extent_cache, NULL, NULL, NULL, 0);
	if (ret) {
		if (ret == -EAGAIN)
			goto loop;
		err = ret;
	}

	ret = check_extent_refs(root, &extent_cache);
	if (ret < 0) {
		if (ret == -EAGAIN)
			goto loop;
		goto out;
	}

	ret = check_devices(&dev_cache, &dev_extent_cache);
	if (ret && err)
		ret = err;

out:
	task_stop(ctx.info);
	if (repair) {
		free_corrupt_blocks_tree(fs_info->corrupt_blocks);
		extent_io_tree_cleanup(&excluded_extents);
		fs_info->fsck_extent_cache = NULL;
		fs_info->free_extent_hook = NULL;
		fs_info->corrupt_blocks = NULL;
		fs_info->excluded_extents = NULL;
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
	free_root_item_list(&normal_trees);
	free_root_item_list(&dropping_trees);
	return ret;
loop:
	free_corrupt_blocks_tree(fs_info->corrupt_blocks);
	free_extent_cache_tree(&seen);
	free_extent_cache_tree(&pending);
	free_extent_cache_tree(&reada);
	free_extent_cache_tree(&nodes);
	free_chunk_cache_tree(&chunk_cache);
	free_block_group_tree(&block_group_cache);
	free_device_cache_tree(&dev_cache);
	free_device_extent_tree(&dev_extent_cache);
	free_extent_record_cache(&extent_cache);
	free_root_item_list(&normal_trees);
	free_root_item_list(&dropping_trees);
	extent_io_tree_cleanup(&excluded_extents);
	goto again;
}

static int check_extent_inline_ref(struct extent_buffer *eb,
		   struct btrfs_key *key, struct btrfs_extent_inline_ref *iref)
{
	int ret;
	u8 type = btrfs_extent_inline_ref_type(eb, iref);

	switch (type) {
	case BTRFS_TREE_BLOCK_REF_KEY:
	case BTRFS_EXTENT_DATA_REF_KEY:
	case BTRFS_SHARED_BLOCK_REF_KEY:
	case BTRFS_SHARED_DATA_REF_KEY:
		ret = 0;
		break;
	default:
		error("extent[%llu %u %llu] has unknown ref type: %d",
		      key->objectid, key->type, key->offset, type);
		ret = UNKNOWN_TYPE;
		break;
	}

	return ret;
}

/*
 * Check backrefs of a tree block given by @bytenr or @eb.
 *
 * @root:	the root containing the @bytenr or @eb
 * @eb:		tree block extent buffer, can be NULL
 * @bytenr:	bytenr of the tree block to search
 * @level:	tree level of the tree block
 * @owner:	owner of the tree block
 *
 * Return >0 for any error found and output error message
 * Return 0 for no error found
 */
static int check_tree_block_ref(struct btrfs_root *root,
				struct extent_buffer *eb, u64 bytenr,
				int level, u64 owner, struct node_refs *nrefs)
{
	struct btrfs_key key;
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct extent_buffer *leaf;
	unsigned long end;
	unsigned long ptr;
	int slot;
	int skinny_level;
	int root_level = btrfs_header_level(root->node);
	int type;
	u32 nodesize = root->fs_info->nodesize;
	u32 item_size;
	u64 offset;
	int found_ref = 0;
	int err = 0;
	int ret;
	int strict = 1;
	int parent = 0;

	btrfs_init_path(&path);
	key.objectid = bytenr;
	if (btrfs_fs_incompat(root->fs_info, SKINNY_METADATA))
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

	/* Search for the backref in extent tree */
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0) {
		err |= BACKREF_MISSING;
		goto out;
	}
	ret = btrfs_previous_extent_item(extent_root, &path, bytenr);
	if (ret) {
		err |= BACKREF_MISSING;
		goto out;
	}

	leaf = path.nodes[0];
	slot = path.slots[0];
	btrfs_item_key_to_cpu(leaf, &key, slot);

	ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);

	if (key.type == BTRFS_METADATA_ITEM_KEY) {
		skinny_level = (int)key.offset;
		iref = (struct btrfs_extent_inline_ref *)(ei + 1);
	} else {
		struct btrfs_tree_block_info *info;

		info = (struct btrfs_tree_block_info *)(ei + 1);
		skinny_level = btrfs_tree_block_level(leaf, info);
		iref = (struct btrfs_extent_inline_ref *)(info + 1);
	}


	if (eb) {
		u64 header_gen;
		u64 extent_gen;

		/*
		 * Due to the feature of shared tree blocks, if the upper node
		 * is a fs root or shared node, the extent of checked node may
		 * not be updated until the next CoW.
		 */
		if (nrefs)
			strict = should_check_extent_strictly(root, nrefs,
					level);
		if (!(btrfs_extent_flags(leaf, ei) &
		      BTRFS_EXTENT_FLAG_TREE_BLOCK)) {
			error(
		"extent[%llu %u] backref type mismatch, missing bit: %llx",
				key.objectid, nodesize,
				BTRFS_EXTENT_FLAG_TREE_BLOCK);
			err = BACKREF_MISMATCH;
		}
		header_gen = btrfs_header_generation(eb);
		extent_gen = btrfs_extent_generation(leaf, ei);
		if (header_gen != extent_gen) {
			error(
	"extent[%llu %u] backref generation mismatch, wanted: %llu, have: %llu",
				key.objectid, nodesize, header_gen,
				extent_gen);
			err = BACKREF_MISMATCH;
		}
		if (level != skinny_level) {
			error(
			"extent[%llu %u] level mismatch, wanted: %u, have: %u",
				key.objectid, nodesize, level, skinny_level);
			err = BACKREF_MISMATCH;
		}
		if (!is_fstree(owner) && btrfs_extent_refs(leaf, ei) != 1) {
			error(
			"extent[%llu %u] is referred by other roots than %llu",
				key.objectid, nodesize, root->objectid);
			err = BACKREF_MISMATCH;
		}
	}

	/*
	 * Iterate the extent/metadata item to find the exact backref
	 */
	item_size = btrfs_item_size_nr(leaf, slot);
	ptr = (unsigned long)iref;
	end = (unsigned long)ei + item_size;

	while (ptr < end) {
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		offset = btrfs_extent_inline_ref_offset(leaf, iref);

		ret = check_extent_inline_ref(leaf, &key, iref);
		if (ret) {
			err |= ret;
			break;
		}
		if (type == BTRFS_TREE_BLOCK_REF_KEY) {
			if (offset == root->objectid)
				found_ref = 1;
			if (!strict && owner == offset)
				found_ref = 1;
		} else if (type == BTRFS_SHARED_BLOCK_REF_KEY) {
			/*
			 * Backref of tree reloc root points to itself, no need
			 * to check backref any more.
			 *
			 * This may be an error of loop backref, but extent tree
			 * checker should have already handled it.
			 * Here we only need to avoid infinite iteration.
			 */
			if (offset == bytenr) {
				found_ref = 1;
			} else {
				/*
				 * Check if the backref points to valid
				 * referencer
				 */
				found_ref = !check_tree_block_ref( root, NULL,
						offset, level + 1, owner,
						NULL);
			}
		}

		if (found_ref)
			break;
		ptr += btrfs_extent_inline_ref_size(type);
	}

	/*
	 * Inlined extent item doesn't have what we need, check
	 * TREE_BLOCK_REF_KEY
	 */
	if (!found_ref) {
		btrfs_release_path(&path);
		key.objectid = bytenr;
		key.type = BTRFS_TREE_BLOCK_REF_KEY;
		key.offset = root->objectid;

		ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
		if (!ret)
			found_ref = 1;
	}
	/*
	 * Finally check SHARED BLOCK REF, any found will be good
	 * Here we're not doing comprehensive extent backref checking,
	 * only need to ensure there is some extent referring to this
	 * tree block.
	 */
	if (!found_ref) {
		btrfs_release_path(&path);
		key.objectid = bytenr;
		key.type = BTRFS_SHARED_BLOCK_REF_KEY;
		key.offset = (u64)-1;

		ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
		if (ret < 0) {
			err |= BACKREF_MISSING;
			goto out;
		}
		ret = btrfs_previous_extent_item(extent_root, &path, bytenr);
		if (ret) {
			err |= BACKREF_MISSING;
			goto out;
		}
		found_ref = 1;
	}
	if (!found_ref)
		err |= BACKREF_MISSING;
out:
	btrfs_release_path(&path);
	if (nrefs && strict &&
	    level < root_level && nrefs->full_backref[level + 1])
		parent = nrefs->bytenr[level + 1];
	if (eb && (err & BACKREF_MISSING))
		error(
	"extent[%llu %u] backref lost (owner: %llu, level: %u) %s %llu",
		      bytenr, nodesize, owner, level,
		      parent ? "parent" : "root",
		      parent ? parent : root->objectid);
	return err;
}

/*
 * If @err contains BACKREF_MISSING then add extent of the
 * file_extent_data_item.
 *
 * Returns error bits after reapir.
 */
static int repair_extent_data_item(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct btrfs_path *pathp,
				   struct node_refs *nrefs,
				   int err)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_key fi_key;
	struct btrfs_key key;
	struct btrfs_extent_item *ei;
	struct btrfs_path path;
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	struct extent_buffer *eb;
	u64 size;
	u64 disk_bytenr;
	u64 num_bytes;
	u64 parent;
	u64 offset;
	u64 extent_offset;
	u64 file_offset;
	int generation;
	int slot;
	int ret = 0;

	eb = pathp->nodes[0];
	slot = pathp->slots[0];
	btrfs_item_key_to_cpu(eb, &fi_key, slot);
	fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);

	if (btrfs_file_extent_type(eb, fi) == BTRFS_FILE_EXTENT_INLINE ||
	    btrfs_file_extent_disk_bytenr(eb, fi) == 0)
		return err;

	file_offset = fi_key.offset;
	generation = btrfs_file_extent_generation(eb, fi);
	disk_bytenr = btrfs_file_extent_disk_bytenr(eb, fi);
	num_bytes = btrfs_file_extent_disk_num_bytes(eb, fi);
	extent_offset = btrfs_file_extent_offset(eb, fi);
	offset = file_offset - extent_offset;

	/* now repair only adds backref */
	if ((err & BACKREF_MISSING) == 0)
		return err;

	/* search extent item */
	key.objectid = disk_bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = num_bytes;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0) {
		ret = -EIO;
		goto out;
	}

	/* insert an extent item */
	if (ret > 0) {
		key.objectid = disk_bytenr;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = num_bytes;
		size = sizeof(*ei);

		btrfs_release_path(&path);
		ret = btrfs_insert_empty_item(trans, extent_root, &path, &key,
					      size);
		if (ret)
			goto out;
		eb = path.nodes[0];
		ei = btrfs_item_ptr(eb, path.slots[0], struct btrfs_extent_item);

		btrfs_set_extent_refs(eb, ei, 0);
		btrfs_set_extent_generation(eb, ei, generation);
		btrfs_set_extent_flags(eb, ei, BTRFS_EXTENT_FLAG_DATA);

		btrfs_mark_buffer_dirty(eb);
		ret = btrfs_update_block_group(trans, extent_root, disk_bytenr,
					       num_bytes, 1, 0);
		btrfs_release_path(&path);
	}

	if (nrefs->full_backref[0])
		parent = btrfs_header_bytenr(eb);
	else
		parent = 0;

	ret = btrfs_inc_extent_ref(trans, root, disk_bytenr, num_bytes, parent,
				   root->objectid,
		   parent ? BTRFS_FIRST_FREE_OBJECTID : fi_key.objectid,
				   offset);
	if (ret) {
		error(
		"failed to increase extent data backref[%llu %llu] root %llu",
		      disk_bytenr, num_bytes, root->objectid);
		goto out;
	} else {
		printf("Add one extent data backref [%llu %llu]\n",
		       disk_bytenr, num_bytes);
	}

	err &= ~BACKREF_MISSING;
out:
	if (ret)
		error("can't repair root %llu extent data item[%llu %llu]",
		      root->objectid, disk_bytenr, num_bytes);
	return err;
}

/*
 * Check EXTENT_DATA item, mainly for its dbackref in extent tree
 *
 * Return >0 any error found and output error message
 * Return 0 for no error found
 */
static int check_extent_data_item(struct btrfs_root *root,
				  struct btrfs_path *pathp,
				  struct node_refs *nrefs,  int account_bytes)
{
	struct btrfs_file_extent_item *fi;
	struct extent_buffer *eb = pathp->nodes[0];
	struct btrfs_path path;
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	struct btrfs_key fi_key;
	struct btrfs_key dbref_key;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	u64 owner;
	u64 disk_bytenr;
	u64 disk_num_bytes;
	u64 extent_num_bytes;
	u64 extent_flags;
	u64 offset;
	u32 item_size;
	unsigned long end;
	unsigned long ptr;
	int type;
	int found_dbackref = 0;
	int slot = pathp->slots[0];
	int err = 0;
	int ret;
	int strict;

	btrfs_item_key_to_cpu(eb, &fi_key, slot);
	fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);

	/* Nothing to check for hole and inline data extents */
	if (btrfs_file_extent_type(eb, fi) == BTRFS_FILE_EXTENT_INLINE ||
	    btrfs_file_extent_disk_bytenr(eb, fi) == 0)
		return 0;

	disk_bytenr = btrfs_file_extent_disk_bytenr(eb, fi);
	disk_num_bytes = btrfs_file_extent_disk_num_bytes(eb, fi);
	extent_num_bytes = btrfs_file_extent_num_bytes(eb, fi);
	offset = btrfs_file_extent_offset(eb, fi);

	/* Check unaligned disk_num_bytes and num_bytes */
	if (!IS_ALIGNED(disk_num_bytes, root->fs_info->sectorsize)) {
		error(
"file extent [%llu, %llu] has unaligned disk num bytes: %llu, should be aligned to %u",
			fi_key.objectid, fi_key.offset, disk_num_bytes,
			root->fs_info->sectorsize);
		err |= BYTES_UNALIGNED;
	} else if (account_bytes) {
		data_bytes_allocated += disk_num_bytes;
	}
	if (!IS_ALIGNED(extent_num_bytes, root->fs_info->sectorsize)) {
		error(
"file extent [%llu, %llu] has unaligned num bytes: %llu, should be aligned to %u",
			fi_key.objectid, fi_key.offset, extent_num_bytes,
			root->fs_info->sectorsize);
		err |= BYTES_UNALIGNED;
	} else if (account_bytes) {
		data_bytes_referenced += extent_num_bytes;
	}
	owner = btrfs_header_owner(eb);

	/* Check the extent item of the file extent in extent tree */
	btrfs_init_path(&path);
	dbref_key.objectid = btrfs_file_extent_disk_bytenr(eb, fi);
	dbref_key.type = BTRFS_EXTENT_ITEM_KEY;
	dbref_key.offset = btrfs_file_extent_disk_num_bytes(eb, fi);

	ret = btrfs_search_slot(NULL, extent_root, &dbref_key, &path, 0, 0);
	if (ret)
		goto out;

	leaf = path.nodes[0];
	slot = path.slots[0];
	ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);

	extent_flags = btrfs_extent_flags(leaf, ei);

	if (!(extent_flags & BTRFS_EXTENT_FLAG_DATA)) {
		error(
		    "extent[%llu %llu] backref type mismatch, wanted bit: %llx",
		    disk_bytenr, disk_num_bytes,
		    BTRFS_EXTENT_FLAG_DATA);
		err |= BACKREF_MISMATCH;
	}

	/* Check data backref inside that extent item */
	item_size = btrfs_item_size_nr(leaf, path.slots[0]);
	iref = (struct btrfs_extent_inline_ref *)(ei + 1);
	ptr = (unsigned long)iref;
	end = (unsigned long)ei + item_size;
	strict = should_check_extent_strictly(root, nrefs, -1);

	while (ptr < end) {
		u64 ref_root;
		u64 ref_objectid;
		u64 ref_offset;
		bool match = false;

		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		dref = (struct btrfs_extent_data_ref *)(&iref->offset);

		ret = check_extent_inline_ref(leaf, &dbref_key, iref);
		if (ret) {
			err |= ret;
			break;
		}
		if (type == BTRFS_EXTENT_DATA_REF_KEY) {
			ref_root = btrfs_extent_data_ref_root(leaf, dref);
			ref_objectid = btrfs_extent_data_ref_objectid(leaf, dref);
			ref_offset = btrfs_extent_data_ref_offset(leaf, dref);

			if (ref_objectid == fi_key.objectid &&
			    ref_offset == fi_key.offset - offset)
				match = true;
			if (ref_root == root->objectid && match)
				found_dbackref = 1;
			else if (!strict && owner == ref_root && match)
				found_dbackref = 1;
		} else if (type == BTRFS_SHARED_DATA_REF_KEY) {
			found_dbackref = !check_tree_block_ref(root, NULL,
				btrfs_extent_inline_ref_offset(leaf, iref),
				0, owner, NULL);
		}

		if (found_dbackref)
			break;
		ptr += btrfs_extent_inline_ref_size(type);
	}

	if (!found_dbackref) {
		btrfs_release_path(&path);

		/* Didn't find inlined data backref, try EXTENT_DATA_REF_KEY */
		dbref_key.objectid = btrfs_file_extent_disk_bytenr(eb, fi);
		dbref_key.type = BTRFS_EXTENT_DATA_REF_KEY;
		dbref_key.offset = hash_extent_data_ref(root->objectid,
				fi_key.objectid, fi_key.offset - offset);

		ret = btrfs_search_slot(NULL, root->fs_info->extent_root,
					&dbref_key, &path, 0, 0);
		if (!ret) {
			found_dbackref = 1;
			goto out;
		}

		btrfs_release_path(&path);

		/*
		 * Neither inlined nor EXTENT_DATA_REF found, try
		 * SHARED_DATA_REF as last chance.
		 */
		dbref_key.objectid = disk_bytenr;
		dbref_key.type = BTRFS_SHARED_DATA_REF_KEY;
		dbref_key.offset = eb->start;

		ret = btrfs_search_slot(NULL, root->fs_info->extent_root,
					&dbref_key, &path, 0, 0);
		if (!ret) {
			found_dbackref = 1;
			goto out;
		}
	}

out:
	if (!found_dbackref)
		err |= BACKREF_MISSING;
	btrfs_release_path(&path);
	if (err & BACKREF_MISSING) {
		error("data extent[%llu %llu] backref lost",
		      disk_bytenr, disk_num_bytes);
	}
	return err;
}

/*
 * Get real tree block level for the case like shared block
 * Return >= 0 as tree level
 * Return <0 for error
 */
static int query_tree_block_level(struct btrfs_fs_info *fs_info, u64 bytenr)
{
	struct extent_buffer *eb;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_extent_item *ei;
	u64 flags;
	u64 transid;
	u8 backref_level;
	u8 header_level;
	int ret;

	/* Search extent tree for extent generation and level */
	key.objectid = bytenr;
	key.type = BTRFS_METADATA_ITEM_KEY;
	key.offset = (u64)-1;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, fs_info->extent_root, &key, &path, 0, 0);
	if (ret < 0)
		goto release_out;
	ret = btrfs_previous_extent_item(fs_info->extent_root, &path, bytenr);
	if (ret < 0)
		goto release_out;
	if (ret > 0) {
		ret = -ENOENT;
		goto release_out;
	}

	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	ei = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_extent_item);
	flags = btrfs_extent_flags(path.nodes[0], ei);
	if (!(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)) {
		ret = -ENOENT;
		goto release_out;
	}

	/* Get transid for later read_tree_block() check */
	transid = btrfs_extent_generation(path.nodes[0], ei);

	/* Get backref level as one source */
	if (key.type == BTRFS_METADATA_ITEM_KEY) {
		backref_level = key.offset;
	} else {
		struct btrfs_tree_block_info *info;

		info = (struct btrfs_tree_block_info *)(ei + 1);
		backref_level = btrfs_tree_block_level(path.nodes[0], info);
	}
	btrfs_release_path(&path);

	/* Get level from tree block as an alternative source */
	eb = read_tree_block(fs_info, bytenr, transid);
	if (!extent_buffer_uptodate(eb)) {
		free_extent_buffer(eb);
		return -EIO;
	}
	header_level = btrfs_header_level(eb);
	free_extent_buffer(eb);

	if (header_level != backref_level)
		return -EIO;
	return header_level;

release_out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Check if a tree block backref is valid (points to a valid tree block)
 * if level == -1, level will be resolved
 * Return >0 for any error found and print error message
 */
static int check_tree_block_backref(struct btrfs_fs_info *fs_info, u64 root_id,
				    u64 bytenr, int level)
{
	struct btrfs_root *root;
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *eb;
	struct extent_buffer *node;
	u32 nodesize = btrfs_super_nodesize(fs_info->super_copy);
	int err = 0;
	int ret;

	/* Query level for level == -1 special case */
	if (level == -1)
		level = query_tree_block_level(fs_info, bytenr);
	if (level < 0) {
		err |= REFERENCER_MISSING;
		goto out;
	}

	key.objectid = root_id;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root)) {
		err |= REFERENCER_MISSING;
		goto out;
	}

	/* Read out the tree block to get item/node key */
	eb = read_tree_block(fs_info, bytenr, 0);
	if (!extent_buffer_uptodate(eb)) {
		err |= REFERENCER_MISSING;
		free_extent_buffer(eb);
		goto out;
	}

	/* Empty tree, no need to check key */
	if (!btrfs_header_nritems(eb) && !level) {
		free_extent_buffer(eb);
		goto out;
	}

	if (level)
		btrfs_node_key_to_cpu(eb, &key, 0);
	else
		btrfs_item_key_to_cpu(eb, &key, 0);

	free_extent_buffer(eb);

	btrfs_init_path(&path);
	path.lowest_level = level;
	/* Search with the first key, to ensure we can reach it */
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		err |= REFERENCER_MISSING;
		goto release_out;
	}

	node = path.nodes[level];
	if (btrfs_header_bytenr(node) != bytenr) {
		error(
	"extent [%llu %d] referencer bytenr mismatch, wanted: %llu, have: %llu",
			bytenr, nodesize, bytenr,
			btrfs_header_bytenr(node));
		err |= REFERENCER_MISMATCH;
	}
	if (btrfs_header_level(node) != level) {
		error(
	"extent [%llu %d] referencer level mismatch, wanted: %d, have: %d",
			bytenr, nodesize, level,
			btrfs_header_level(node));
		err |= REFERENCER_MISMATCH;
	}

release_out:
	btrfs_release_path(&path);
out:
	if (err & REFERENCER_MISSING) {
		if (level < 0)
			error("extent [%llu %d] lost referencer (owner: %llu)",
				bytenr, nodesize, root_id);
		else
			error(
		"extent [%llu %d] lost referencer (owner: %llu, level: %u)",
				bytenr, nodesize, root_id, level);
	}

	return err;
}

/*
 * Check if tree block @eb is tree reloc root.
 * Return 0 if it's not or any problem happens
 * Return 1 if it's a tree reloc root
 */
static int is_tree_reloc_root(struct btrfs_fs_info *fs_info,
				 struct extent_buffer *eb)
{
	struct btrfs_root *tree_reloc_root;
	struct btrfs_key key;
	u64 bytenr = btrfs_header_bytenr(eb);
	u64 owner = btrfs_header_owner(eb);
	int ret = 0;

	key.objectid = BTRFS_TREE_RELOC_OBJECTID;
	key.offset = owner;
	key.type = BTRFS_ROOT_ITEM_KEY;

	tree_reloc_root = btrfs_read_fs_root_no_cache(fs_info, &key);
	if (IS_ERR(tree_reloc_root))
		return 0;

	if (bytenr == btrfs_header_bytenr(tree_reloc_root->node))
		ret = 1;
	btrfs_free_fs_root(tree_reloc_root);
	return ret;
}

/*
 * Check referencer for shared block backref
 * If level == -1, this function will resolve the level.
 */
static int check_shared_block_backref(struct btrfs_fs_info *fs_info,
				     u64 parent, u64 bytenr, int level)
{
	struct extent_buffer *eb;
	u32 nr;
	int found_parent = 0;
	int i;

	eb = read_tree_block(fs_info, parent, 0);
	if (!extent_buffer_uptodate(eb))
		goto out;

	if (level == -1)
		level = query_tree_block_level(fs_info, bytenr);
	if (level < 0)
		goto out;

	/* It's possible it's a tree reloc root */
	if (parent == bytenr) {
		if (is_tree_reloc_root(fs_info, eb))
			found_parent = 1;
		goto out;
	}

	if (level + 1 != btrfs_header_level(eb))
		goto out;

	nr = btrfs_header_nritems(eb);
	for (i = 0; i < nr; i++) {
		if (bytenr == btrfs_node_blockptr(eb, i)) {
			found_parent = 1;
			break;
		}
	}
out:
	free_extent_buffer(eb);
	if (!found_parent) {
		error(
	"shared extent[%llu %u] lost its parent (parent: %llu, level: %u)",
			bytenr, fs_info->nodesize, parent, level);
		return REFERENCER_MISSING;
	}
	return 0;
}

/*
 * Check referencer for normal (inlined) data ref
 * If len == 0, it will be resolved by searching in extent tree
 */
static int check_extent_data_backref(struct btrfs_fs_info *fs_info,
				     u64 root_id, u64 objectid, u64 offset,
				     u64 bytenr, u64 len, u32 count)
{
	struct btrfs_root *root;
	struct btrfs_root *extent_root = fs_info->extent_root;
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	u32 found_count = 0;
	int slot;
	int ret = 0;

	if (!len) {
		key.objectid = bytenr;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = (u64)-1;

		btrfs_init_path(&path);
		ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
		if (ret < 0)
			goto out;
		ret = btrfs_previous_extent_item(extent_root, &path, bytenr);
		if (ret)
			goto out;
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.objectid != bytenr ||
		    key.type != BTRFS_EXTENT_ITEM_KEY)
			goto out;
		len = key.offset;
		btrfs_release_path(&path);
	}
	key.objectid = root_id;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	btrfs_init_path(&path);

	root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root))
		goto out;

	key.objectid = objectid;
	key.type = BTRFS_EXTENT_DATA_KEY;
	/*
	 * It can be nasty as data backref offset is
	 * file offset - file extent offset, which is smaller or
	 * equal to original backref offset.  The only special case is
	 * overflow.  So we need to special check and do further search.
	 */
	key.offset = offset & (1ULL << 63) ? 0 : offset;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;

	/*
	 * Search afterwards to get correct one
	 * NOTE: As we must do a comprehensive check on the data backref to
	 * make sure the dref count also matches, we must iterate all file
	 * extents for that inode.
	 */
	while (1) {
		leaf = path.nodes[0];
		slot = path.slots[0];

		if (slot >= btrfs_header_nritems(leaf) ||
		    btrfs_header_owner(leaf) != root_id)
			goto next;
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid != objectid || key.type != BTRFS_EXTENT_DATA_KEY)
			break;
		fi = btrfs_item_ptr(leaf, slot, struct btrfs_file_extent_item);
		/*
		 * Except normal disk bytenr and disk num bytes, we still
		 * need to do extra check on dbackref offset as
		 * dbackref offset = file_offset - file_extent_offset
		 *
		 * Also, we must check the leaf owner.
		 * In case of shared tree blocks (snapshots) we can inherit
		 * leaves from source snapshot.
		 * In that case, reference from source snapshot should not
		 * count.
		 */
		if (btrfs_file_extent_disk_bytenr(leaf, fi) == bytenr &&
		    btrfs_file_extent_disk_num_bytes(leaf, fi) == len &&
		    (u64)(key.offset - btrfs_file_extent_offset(leaf, fi)) ==
		    offset && btrfs_header_owner(leaf) == root_id)
			found_count++;

next:
		ret = btrfs_next_item(root, &path);
		if (ret)
			break;
	}
out:
	btrfs_release_path(&path);
	if (found_count != count) {
		error(
"extent[%llu, %llu] referencer count mismatch (root: %llu, owner: %llu, offset: %llu) wanted: %u, have: %u",
			bytenr, len, root_id, objectid, offset, count, found_count);
		return REFERENCER_MISSING;
	}
	return 0;
}

/*
 * Check if the referencer of a shared data backref exists
 */
static int check_shared_data_backref(struct btrfs_fs_info *fs_info,
				     u64 parent, u64 bytenr)
{
	struct extent_buffer *eb;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	u32 nr;
	int found_parent = 0;
	int i;

	eb = read_tree_block(fs_info, parent, 0);
	if (!extent_buffer_uptodate(eb))
		goto out;

	nr = btrfs_header_nritems(eb);
	for (i = 0; i < nr; i++) {
		btrfs_item_key_to_cpu(eb, &key, i);
		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;

		fi = btrfs_item_ptr(eb, i, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(eb, fi) == BTRFS_FILE_EXTENT_INLINE)
			continue;

		if (btrfs_file_extent_disk_bytenr(eb, fi) == bytenr) {
			found_parent = 1;
			break;
		}
	}

out:
	free_extent_buffer(eb);
	if (!found_parent) {
		error("shared extent %llu referencer lost (parent: %llu)",
			bytenr, parent);
		return REFERENCER_MISSING;
	}
	return 0;
}

/*
 * Only delete backref if REFERENCER_MISSING now
 *
 * Returns <0   the extent was deleted
 * Returns >0   the backref was deleted but extent still exists, returned value
 *               means error after repair
 * Returns  0   nothing happened
 */
static int repair_extent_item(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, struct btrfs_path *path,
		      u64 bytenr, u64 num_bytes, u64 parent, u64 root_objectid,
		      u64 owner, u64 offset, int err)
{
	struct btrfs_key old_key;
	int freed = 0;
	int ret;

	btrfs_item_key_to_cpu(path->nodes[0], &old_key, path->slots[0]);

	if (err & (REFERENCER_MISSING | REFERENCER_MISMATCH)) {
		/* delete the backref */
		ret = btrfs_free_extent(trans, root->fs_info->fs_root, bytenr,
			  num_bytes, parent, root_objectid, owner, offset);
		if (!ret) {
			freed = 1;
			err &= ~REFERENCER_MISSING;
			printf("Delete backref in extent [%llu %llu]\n",
			       bytenr, num_bytes);
		} else {
			error("fail to delete backref in extent [%llu %llu]",
			       bytenr, num_bytes);
		}
	}

	/* btrfs_free_extent may delete the extent */
	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, &old_key, path, 0, 0);

	if (ret)
		ret = -ENOENT;
	else if (freed)
		ret = err;
	return ret;
}

/*
 * This function will check a given extent item, including its backref and
 * itself (like crossing stripe boundary and type)
 *
 * Since we don't use extent_record anymore, introduce new error bit
 */
static int check_extent_item(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info,
			     struct btrfs_path *path)
{
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	struct extent_buffer *eb = path->nodes[0];
	unsigned long end;
	unsigned long ptr;
	int slot = path->slots[0];
	int type;
	u32 nodesize = btrfs_super_nodesize(fs_info->super_copy);
	u32 item_size = btrfs_item_size_nr(eb, slot);
	u64 flags;
	u64 offset;
	u64 parent;
	u64 num_bytes;
	u64 root_objectid;
	u64 owner;
	u64 owner_offset;
	int metadata = 0;
	int level;
	struct btrfs_key key;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(eb, &key, slot);
	if (key.type == BTRFS_EXTENT_ITEM_KEY) {
		bytes_used += key.offset;
		num_bytes = key.offset;
	} else {
		bytes_used += nodesize;
		num_bytes = nodesize;
	}

	if (item_size < sizeof(*ei)) {
		/*
		 * COMPAT_EXTENT_TREE_V0 case, but it's already a super
		 * old thing when on disk format is still un-determined.
		 * No need to care about it anymore
		 */
		error("unsupported COMPAT_EXTENT_TREE_V0 detected");
		return -ENOTTY;
	}

	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
	flags = btrfs_extent_flags(eb, ei);

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
		metadata = 1;
	if (metadata && check_crossing_stripes(global_info, key.objectid,
					       eb->len)) {
		error("bad metadata [%llu, %llu) crossing stripe boundary",
		      key.objectid, key.objectid + nodesize);
		err |= CROSSING_STRIPE_BOUNDARY;
	}

	ptr = (unsigned long)(ei + 1);

	if (metadata && key.type == BTRFS_EXTENT_ITEM_KEY) {
		/* Old EXTENT_ITEM metadata */
		struct btrfs_tree_block_info *info;

		info = (struct btrfs_tree_block_info *)ptr;
		level = btrfs_tree_block_level(eb, info);
		ptr += sizeof(struct btrfs_tree_block_info);
	} else {
		/* New METADATA_ITEM */
		level = key.offset;
	}
	end = (unsigned long)ei + item_size;

next:
	/* Reached extent item end normally */
	if (ptr == end)
		goto out;

	/* Beyond extent item end, wrong item size */
	if (ptr > end) {
		err |= ITEM_SIZE_MISMATCH;
		error("extent item at bytenr %llu slot %d has wrong size",
			eb->start, slot);
		goto out;
	}

	parent = 0;
	root_objectid = 0;
	owner = 0;
	owner_offset = 0;
	/* Now check every backref in this extent item */
	iref = (struct btrfs_extent_inline_ref *)ptr;
	type = btrfs_extent_inline_ref_type(eb, iref);
	offset = btrfs_extent_inline_ref_offset(eb, iref);
	switch (type) {
	case BTRFS_TREE_BLOCK_REF_KEY:
		root_objectid = offset;
		owner = level;
		ret = check_tree_block_backref(fs_info, offset, key.objectid,
					       level);
		err |= ret;
		break;
	case BTRFS_SHARED_BLOCK_REF_KEY:
		parent = offset;
		ret = check_shared_block_backref(fs_info, offset, key.objectid,
						 level);
		err |= ret;
		break;
	case BTRFS_EXTENT_DATA_REF_KEY:
		dref = (struct btrfs_extent_data_ref *)(&iref->offset);
		root_objectid = btrfs_extent_data_ref_root(eb, dref);
		owner = btrfs_extent_data_ref_objectid(eb, dref);
		owner_offset = btrfs_extent_data_ref_offset(eb, dref);
		ret = check_extent_data_backref(fs_info, root_objectid, owner,
					owner_offset, key.objectid, key.offset,
					btrfs_extent_data_ref_count(eb, dref));
		err |= ret;
		break;
	case BTRFS_SHARED_DATA_REF_KEY:
		parent = offset;
		ret = check_shared_data_backref(fs_info, offset, key.objectid);
		err |= ret;
		break;
	default:
		error("extent[%llu %d %llu] has unknown ref type: %d",
			key.objectid, key.type, key.offset, type);
		ret = UNKNOWN_TYPE;
		err |= ret;
		goto out;
	}

	if (err && repair) {
		ret = repair_extent_item(trans, fs_info->extent_root, path,
			 key.objectid, num_bytes, parent, root_objectid,
			 owner, owner_offset, ret);
		if (ret < 0)
			goto out;
		if (ret) {
			goto next;
			err = ret;
		}
	}

	ptr += btrfs_extent_inline_ref_size(type);
	goto next;

out:
	return err;
}

/*
 * Check if a dev extent item is referred correctly by its chunk
 */
static int check_dev_extent_item(struct btrfs_fs_info *fs_info,
				 struct extent_buffer *eb, int slot)
{
	struct btrfs_root *chunk_root = fs_info->chunk_root;
	struct btrfs_dev_extent *ptr;
	struct btrfs_path path;
	struct btrfs_key chunk_key;
	struct btrfs_key devext_key;
	struct btrfs_chunk *chunk;
	struct extent_buffer *l;
	int num_stripes;
	u64 length;
	int i;
	int found_chunk = 0;
	int ret;

	btrfs_item_key_to_cpu(eb, &devext_key, slot);
	ptr = btrfs_item_ptr(eb, slot, struct btrfs_dev_extent);
	length = btrfs_dev_extent_length(eb, ptr);

	chunk_key.objectid = btrfs_dev_extent_chunk_objectid(eb, ptr);
	chunk_key.type = BTRFS_CHUNK_ITEM_KEY;
	chunk_key.offset = btrfs_dev_extent_chunk_offset(eb, ptr);

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, chunk_root, &chunk_key, &path, 0, 0);
	if (ret)
		goto out;

	l = path.nodes[0];
	chunk = btrfs_item_ptr(l, path.slots[0], struct btrfs_chunk);
	ret = btrfs_check_chunk_valid(fs_info, l, chunk, path.slots[0],
				      chunk_key.offset);
	if (ret < 0)
		goto out;

	if (btrfs_stripe_length(fs_info, l, chunk) != length)
		goto out;

	num_stripes = btrfs_chunk_num_stripes(l, chunk);
	for (i = 0; i < num_stripes; i++) {
		u64 devid = btrfs_stripe_devid_nr(l, chunk, i);
		u64 offset = btrfs_stripe_offset_nr(l, chunk, i);

		if (devid == devext_key.objectid &&
		    offset == devext_key.offset) {
			found_chunk = 1;
			break;
		}
	}
out:
	btrfs_release_path(&path);
	if (!found_chunk) {
		error(
		"device extent[%llu, %llu, %llu] did not find the related chunk",
			devext_key.objectid, devext_key.offset, length);
		return REFERENCER_MISSING;
	}
	return 0;
}

/*
 * Check if the used space is correct with the dev item
 */
static int check_dev_item(struct btrfs_fs_info *fs_info,
			  struct extent_buffer *eb, int slot)
{
	struct btrfs_root *dev_root = fs_info->dev_root;
	struct btrfs_dev_item *dev_item;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_dev_extent *ptr;
	u64 total_bytes;
	u64 dev_id;
	u64 used;
	u64 total = 0;
	int ret;

	dev_item = btrfs_item_ptr(eb, slot, struct btrfs_dev_item);
	dev_id = btrfs_device_id(eb, dev_item);
	used = btrfs_device_bytes_used(eb, dev_item);
	total_bytes = btrfs_device_total_bytes(eb, dev_item);

	key.objectid = dev_id;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = 0;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, dev_root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_item_key_to_cpu(eb, &key, slot);
		error("cannot find any related dev extent for dev[%llu, %u, %llu]",
			key.objectid, key.type, key.offset);
		btrfs_release_path(&path);
		return REFERENCER_MISSING;
	}

	/* Iterate dev_extents to calculate the used space of a device */
	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0]))
			goto next;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.objectid > dev_id)
			break;
		if (key.type != BTRFS_DEV_EXTENT_KEY || key.objectid != dev_id)
			goto next;

		ptr = btrfs_item_ptr(path.nodes[0], path.slots[0],
				     struct btrfs_dev_extent);
		total += btrfs_dev_extent_length(path.nodes[0], ptr);
next:
		ret = btrfs_next_item(dev_root, &path);
		if (ret)
			break;
	}
	btrfs_release_path(&path);

	if (used != total) {
		btrfs_item_key_to_cpu(eb, &key, slot);
		error(
"Dev extent's total-byte %llu is not equal to bytes-used %llu in dev[%llu, %u, %llu]",
			total, used, BTRFS_ROOT_TREE_OBJECTID,
			BTRFS_DEV_EXTENT_KEY, dev_id);
		return ACCOUNTING_MISMATCH;
	}
	check_dev_size_alignment(dev_id, total_bytes, fs_info->sectorsize);

	return 0;
}

/*
 * Check a block group item with its referener (chunk) and its used space
 * with extent/metadata item
 */
static int check_block_group_item(struct btrfs_fs_info *fs_info,
				  struct extent_buffer *eb, int slot)
{
	struct btrfs_root *extent_root = fs_info->extent_root;
	struct btrfs_root *chunk_root = fs_info->chunk_root;
	struct btrfs_block_group_item *bi;
	struct btrfs_block_group_item bg_item;
	struct btrfs_path path;
	struct btrfs_key bg_key;
	struct btrfs_key chunk_key;
	struct btrfs_key extent_key;
	struct btrfs_chunk *chunk;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	u32 nodesize = btrfs_super_nodesize(fs_info->super_copy);
	u64 flags;
	u64 bg_flags;
	u64 used;
	u64 total = 0;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(eb, &bg_key, slot);
	bi = btrfs_item_ptr(eb, slot, struct btrfs_block_group_item);
	read_extent_buffer(eb, &bg_item, (unsigned long)bi, sizeof(bg_item));
	used = btrfs_block_group_used(&bg_item);
	bg_flags = btrfs_block_group_flags(&bg_item);

	chunk_key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	chunk_key.type = BTRFS_CHUNK_ITEM_KEY;
	chunk_key.offset = bg_key.objectid;

	btrfs_init_path(&path);
	/* Search for the referencer chunk */
	ret = btrfs_search_slot(NULL, chunk_root, &chunk_key, &path, 0, 0);
	if (ret) {
		error(
		"block group[%llu %llu] did not find the related chunk item",
			bg_key.objectid, bg_key.offset);
		err |= REFERENCER_MISSING;
	} else {
		chunk = btrfs_item_ptr(path.nodes[0], path.slots[0],
					struct btrfs_chunk);
		if (btrfs_chunk_length(path.nodes[0], chunk) !=
						bg_key.offset) {
			error(
	"block group[%llu %llu] related chunk item length does not match",
				bg_key.objectid, bg_key.offset);
			err |= REFERENCER_MISMATCH;
		}
	}
	btrfs_release_path(&path);

	/* Search from the block group bytenr */
	extent_key.objectid = bg_key.objectid;
	extent_key.type = 0;
	extent_key.offset = 0;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, extent_root, &extent_key, &path, 0, 0);
	if (ret < 0)
		goto out;

	/* Iterate extent tree to account used space */
	while (1) {
		leaf = path.nodes[0];

		/* Search slot can point to the last item beyond leaf nritems */
		if (path.slots[0] >= btrfs_header_nritems(leaf))
			goto next;

		btrfs_item_key_to_cpu(leaf, &extent_key, path.slots[0]);
		if (extent_key.objectid >= bg_key.objectid + bg_key.offset)
			break;

		if (extent_key.type != BTRFS_METADATA_ITEM_KEY &&
		    extent_key.type != BTRFS_EXTENT_ITEM_KEY)
			goto next;
		if (extent_key.objectid < bg_key.objectid)
			goto next;

		if (extent_key.type == BTRFS_METADATA_ITEM_KEY)
			total += nodesize;
		else
			total += extent_key.offset;

		ei = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_extent_item);
		flags = btrfs_extent_flags(leaf, ei);
		if (flags & BTRFS_EXTENT_FLAG_DATA) {
			if (!(bg_flags & BTRFS_BLOCK_GROUP_DATA)) {
				error(
			"bad extent[%llu, %llu) type mismatch with chunk",
					extent_key.objectid,
					extent_key.objectid + extent_key.offset);
				err |= CHUNK_TYPE_MISMATCH;
			}
		} else if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
			if (!(bg_flags & (BTRFS_BLOCK_GROUP_SYSTEM |
				    BTRFS_BLOCK_GROUP_METADATA))) {
				error(
			"bad extent[%llu, %llu) type mismatch with chunk",
					extent_key.objectid,
					extent_key.objectid + nodesize);
				err |= CHUNK_TYPE_MISMATCH;
			}
		}
next:
		ret = btrfs_next_item(extent_root, &path);
		if (ret)
			break;
	}

out:
	btrfs_release_path(&path);

	if (total != used) {
		error(
		"block group[%llu %llu] used %llu but extent items used %llu",
			bg_key.objectid, bg_key.offset, used, total);
		err |= BG_ACCOUNTING_ERROR;
	}
	return err;
}

/*
 * Add block group item to the extent tree if @err contains REFERENCER_MISSING.
 * FIXME: We still need to repair error of dev_item.
 *
 * Returns error after repair.
 */
static int repair_chunk_item(struct btrfs_trans_handle *trans,
			     struct btrfs_root *chunk_root,
			     struct btrfs_path *path, int err)
{
	struct btrfs_chunk *chunk;
	struct btrfs_key chunk_key;
	struct extent_buffer *eb = path->nodes[0];
	u64 length;
	int slot = path->slots[0];
	u64 type;
	int ret = 0;

	btrfs_item_key_to_cpu(eb, &chunk_key, slot);
	if (chunk_key.type != BTRFS_CHUNK_ITEM_KEY)
		return err;
	chunk = btrfs_item_ptr(eb, slot, struct btrfs_chunk);
	type = btrfs_chunk_type(path->nodes[0], chunk);
	length = btrfs_chunk_length(eb, chunk);

	if (err & REFERENCER_MISSING) {
		ret = btrfs_make_block_group(trans, chunk_root->fs_info, 0,
		     type, chunk_key.objectid, chunk_key.offset, length);
		if (ret) {
			error("fail to add block group item[%llu %llu]",
			      chunk_key.offset, length);
			goto out;
		} else {
			err &= ~REFERENCER_MISSING;
			printf("Added block group item[%llu %llu]\n",
			       chunk_key.offset, length);
		}
	}

out:
	return err;
}

/*
 * Check a chunk item.
 * Including checking all referred dev_extents and block group
 */
static int check_chunk_item(struct btrfs_fs_info *fs_info,
			    struct extent_buffer *eb, int slot)
{
	struct btrfs_root *extent_root = fs_info->extent_root;
	struct btrfs_root *dev_root = fs_info->dev_root;
	struct btrfs_path path;
	struct btrfs_key chunk_key;
	struct btrfs_key bg_key;
	struct btrfs_key devext_key;
	struct btrfs_chunk *chunk;
	struct extent_buffer *leaf;
	struct btrfs_block_group_item *bi;
	struct btrfs_block_group_item bg_item;
	struct btrfs_dev_extent *ptr;
	u64 length;
	u64 chunk_end;
	u64 stripe_len;
	u64 type;
	int num_stripes;
	u64 offset;
	u64 objectid;
	int i;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(eb, &chunk_key, slot);
	chunk = btrfs_item_ptr(eb, slot, struct btrfs_chunk);
	length = btrfs_chunk_length(eb, chunk);
	chunk_end = chunk_key.offset + length;
	ret = btrfs_check_chunk_valid(fs_info, eb, chunk, slot,
				      chunk_key.offset);
	if (ret < 0) {
		error("chunk[%llu %llu) is invalid", chunk_key.offset,
			chunk_end);
		err |= BYTES_UNALIGNED | UNKNOWN_TYPE;
		goto out;
	}
	type = btrfs_chunk_type(eb, chunk);

	bg_key.objectid = chunk_key.offset;
	bg_key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	bg_key.offset = length;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, extent_root, &bg_key, &path, 0, 0);
	if (ret) {
		error(
		"chunk[%llu %llu) did not find the related block group item",
			chunk_key.offset, chunk_end);
		err |= REFERENCER_MISSING;
	} else{
		leaf = path.nodes[0];
		bi = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_block_group_item);
		read_extent_buffer(leaf, &bg_item, (unsigned long)bi,
				   sizeof(bg_item));
		if (btrfs_block_group_flags(&bg_item) != type) {
			error(
"chunk[%llu %llu) related block group item flags mismatch, wanted: %llu, have: %llu",
				chunk_key.offset, chunk_end, type,
				btrfs_block_group_flags(&bg_item));
			err |= REFERENCER_MISSING;
		}
	}

	num_stripes = btrfs_chunk_num_stripes(eb, chunk);
	stripe_len = btrfs_stripe_length(fs_info, eb, chunk);
	for (i = 0; i < num_stripes; i++) {
		btrfs_release_path(&path);
		btrfs_init_path(&path);
		devext_key.objectid = btrfs_stripe_devid_nr(eb, chunk, i);
		devext_key.type = BTRFS_DEV_EXTENT_KEY;
		devext_key.offset = btrfs_stripe_offset_nr(eb, chunk, i);

		ret = btrfs_search_slot(NULL, dev_root, &devext_key, &path,
					0, 0);
		if (ret)
			goto not_match_dev;

		leaf = path.nodes[0];
		ptr = btrfs_item_ptr(leaf, path.slots[0],
				     struct btrfs_dev_extent);
		objectid = btrfs_dev_extent_chunk_objectid(leaf, ptr);
		offset = btrfs_dev_extent_chunk_offset(leaf, ptr);
		if (objectid != chunk_key.objectid ||
		    offset != chunk_key.offset ||
		    btrfs_dev_extent_length(leaf, ptr) != stripe_len)
			goto not_match_dev;
		continue;
not_match_dev:
		err |= BACKREF_MISSING;
		error(
		"chunk[%llu %llu) stripe %d did not find the related dev extent",
			chunk_key.objectid, chunk_end, i);
		continue;
	}
	btrfs_release_path(&path);
out:
	return err;
}

static int delete_extent_tree_item(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct btrfs_path *path)
{
	struct btrfs_key key;
	int ret = 0;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret) {
		ret = -ENOENT;
		goto out;
	}

	ret = btrfs_del_item(trans, root, path);
	if (ret)
		goto out;

	if (path->slots[0] == 0)
		btrfs_prev_leaf(root, path);
	else
		path->slots[0]--;
out:
	if (ret)
		error("failed to delete root %llu item[%llu, %u, %llu]",
		      root->objectid, key.objectid, key.type, key.offset);
	else
		printf("Deleted root %llu item[%llu, %u, %llu]\n",
		       root->objectid, key.objectid, key.type, key.offset);
	return ret;
}

/*
 * Main entry function to check known items and update related accounting info
 */
static int check_leaf_items(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root, struct btrfs_path *path,
			    struct node_refs *nrefs, int account_bytes)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_key key;
	struct extent_buffer *eb;
	int slot;
	int type;
	struct btrfs_extent_data_ref *dref;
	int ret = 0;
	int err = 0;

again:
	eb = path->nodes[0];
	slot = path->slots[0];
	if (slot >= btrfs_header_nritems(eb)) {
		if (slot == 0) {
			error("empty leaf [%llu %u] root %llu", eb->start,
				root->fs_info->nodesize, root->objectid);
			err |= EIO;
		}
		goto out;
	}

	btrfs_item_key_to_cpu(eb, &key, slot);
	type = key.type;

	switch (type) {
	case BTRFS_EXTENT_DATA_KEY:
		ret = check_extent_data_item(root, path, nrefs, account_bytes);
		if (repair && ret)
			ret = repair_extent_data_item(trans, root, path, nrefs,
						      ret);
		err |= ret;
		break;
	case BTRFS_BLOCK_GROUP_ITEM_KEY:
		ret = check_block_group_item(fs_info, eb, slot);
		if (repair &&
		    ret & REFERENCER_MISSING)
			ret = delete_extent_tree_item(trans, root, path);
		err |= ret;
		break;
	case BTRFS_DEV_ITEM_KEY:
		ret = check_dev_item(fs_info, eb, slot);
		err |= ret;
		break;
	case BTRFS_CHUNK_ITEM_KEY:
		ret = check_chunk_item(fs_info, eb, slot);
		if (repair && ret)
			ret = repair_chunk_item(trans, root, path, ret);
		err |= ret;
		break;
	case BTRFS_DEV_EXTENT_KEY:
		ret = check_dev_extent_item(fs_info, eb, slot);
		err |= ret;
		break;
	case BTRFS_EXTENT_ITEM_KEY:
	case BTRFS_METADATA_ITEM_KEY:
		ret = check_extent_item(trans, fs_info, path);
		err |= ret;
		break;
	case BTRFS_EXTENT_CSUM_KEY:
		total_csum_bytes += btrfs_item_size_nr(eb, slot);
		err |= ret;
		break;
	case BTRFS_TREE_BLOCK_REF_KEY:
		ret = check_tree_block_backref(fs_info, key.offset,
					       key.objectid, -1);
		if (repair &&
		    ret & (REFERENCER_MISMATCH | REFERENCER_MISSING))
			ret = delete_extent_tree_item(trans, root, path);
		err |= ret;
		break;
	case BTRFS_EXTENT_DATA_REF_KEY:
		dref = btrfs_item_ptr(eb, slot, struct btrfs_extent_data_ref);
		ret = check_extent_data_backref(fs_info,
				btrfs_extent_data_ref_root(eb, dref),
				btrfs_extent_data_ref_objectid(eb, dref),
				btrfs_extent_data_ref_offset(eb, dref),
				key.objectid, 0,
				btrfs_extent_data_ref_count(eb, dref));
		if (repair &&
		    ret & (REFERENCER_MISMATCH | REFERENCER_MISSING))
			ret = delete_extent_tree_item(trans, root, path);
		err |= ret;
		break;
	case BTRFS_SHARED_BLOCK_REF_KEY:
		ret = check_shared_block_backref(fs_info, key.offset,
						 key.objectid, -1);
		if (repair &&
		    ret & (REFERENCER_MISMATCH | REFERENCER_MISSING))
			ret = delete_extent_tree_item(trans, root, path);
		err |= ret;
		break;
	case BTRFS_SHARED_DATA_REF_KEY:
		ret = check_shared_data_backref(fs_info, key.offset,
						key.objectid);
		if (repair &&
		    ret & (REFERENCER_MISMATCH | REFERENCER_MISSING))
			ret = delete_extent_tree_item(trans, root, path);
		err |= ret;
		break;
	default:
		break;
	}

	++path->slots[0];
	goto again;
out:
	return err;
}

/*
 * Low memory usage version check_chunks_and_extents.
 */
static int check_chunks_and_extents_v2(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans = NULL;
	struct btrfs_path path;
	struct btrfs_key old_key;
	struct btrfs_key key;
	struct btrfs_root *root1;
	struct btrfs_root *root;
	struct btrfs_root *cur_root;
	int err = 0;
	int ret;

	root = fs_info->fs_root;

	if (repair) {
		trans = btrfs_start_transaction(fs_info->extent_root, 1);
		if (IS_ERR(trans)) {
			error("failed to start transaction before check");
			return PTR_ERR(trans);
		}
	}

	root1 = root->fs_info->chunk_root;
	ret = check_btrfs_root(trans, root1, 0, 1);
	err |= ret;

	root1 = root->fs_info->tree_root;
	ret = check_btrfs_root(trans, root1, 0, 1);
	err |= ret;

	btrfs_init_path(&path);
	key.objectid = BTRFS_EXTENT_TREE_OBJECTID;
	key.offset = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;

	ret = btrfs_search_slot(NULL, root1, &key, &path, 0, 0);
	if (ret) {
		error("cannot find extent tree in tree_root");
		goto out;
	}

	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.type != BTRFS_ROOT_ITEM_KEY)
			goto next;
		old_key = key;
		key.offset = (u64)-1;

		if (key.objectid == BTRFS_TREE_RELOC_OBJECTID)
			cur_root = btrfs_read_fs_root_no_cache(root->fs_info,
					&key);
		else
			cur_root = btrfs_read_fs_root(root->fs_info, &key);
		if (IS_ERR(cur_root) || !cur_root) {
			error("failed to read tree: %lld", key.objectid);
			goto next;
		}

		ret = check_btrfs_root(trans, cur_root, 0, 1);
		err |= ret;

		if (key.objectid == BTRFS_TREE_RELOC_OBJECTID)
			btrfs_free_fs_root(cur_root);

		btrfs_release_path(&path);
		ret = btrfs_search_slot(NULL, root->fs_info->tree_root,
					&old_key, &path, 0, 0);
		if (ret)
			goto out;
next:
		ret = btrfs_next_item(root1, &path);
		if (ret)
			goto out;
	}
out:

	/* if repair, update block accounting */
	if (repair) {
		ret = btrfs_fix_block_accounting(trans, root);
		if (ret)
			err |= ret;
		else
			err &= ~BG_ACCOUNTING_ERROR;
	}

	if (trans)
		btrfs_commit_transaction(trans, root->fs_info->extent_root);

	btrfs_release_path(&path);

	return err;
}

static int do_check_chunks_and_extents(struct btrfs_fs_info *fs_info)
{
	int ret;

	if (!ctx.progress_enabled)
		fprintf(stderr, "checking extents\n");
	if (check_mode == CHECK_MODE_LOWMEM)
		ret = check_chunks_and_extents_v2(fs_info);
	else
		ret = check_chunks_and_extents(fs_info);

	/* Also repair device size related problems */
	if (repair && !ret) {
		ret = btrfs_fix_device_and_super_size(fs_info);
		if (ret > 0)
			ret = 0;
	}
	return ret;
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
				   root->fs_info->nodesize,
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
			tmp = read_tree_block(fs_info, bytenr, 0);
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
				btrfs_pin_extent(fs_info, bytenr,
						fs_info->nodesize);
				continue;
			}

			tmp = read_tree_block(fs_info, bytenr, 0);
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
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_chunk *chunk;
	struct btrfs_key key;
	int ret;
	u64 start;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, fs_info->chunk_root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_release_path(&path);
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
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(fs_info->chunk_root, &path);
			if (ret < 0) {
				btrfs_release_path(&path);
				return ret;
			}
			if (ret) {
				ret = 0;
				break;
			}
		}
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_CHUNK_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}

		chunk = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_chunk);
		btrfs_add_block_group(fs_info, 0,
				      btrfs_chunk_type(leaf, chunk),
				      key.objectid, key.offset,
				      btrfs_chunk_length(leaf, chunk));
		set_extent_dirty(&fs_info->free_space_cache, key.offset,
				 key.offset + btrfs_chunk_length(leaf, chunk));
		path.slots[0]++;
	}
	start = 0;
	while (1) {
		cache = btrfs_lookup_first_block_group(fs_info, start);
		if (!cache)
			break;
		cache->cached = 1;
		start = cache->key.objectid + cache->key.offset;
	}

	btrfs_release_path(&path);
	return 0;
}

static int reset_balance(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root = fs_info->tree_root;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int del_slot, del_nr = 0;
	int ret;
	int found = 0;

	btrfs_init_path(&path);
	key.objectid = BTRFS_BALANCE_OBJECTID;
	key.type = BTRFS_BALANCE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
	if (ret) {
		if (ret > 0)
			ret = 0;
		if (!ret)
			goto reinit_data_reloc;
		else
			goto out;
	}

	ret = btrfs_del_item(trans, root, &path);
	if (ret)
		goto out;
	btrfs_release_path(&path);

	key.objectid = BTRFS_TREE_RELOC_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
	if (ret < 0)
		goto out;
	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			if (!found)
				break;

			if (del_nr) {
				ret = btrfs_del_items(trans, root, &path,
						      del_slot, del_nr);
				del_nr = 0;
				if (ret)
					goto out;
			}
			key.offset++;
			btrfs_release_path(&path);

			found = 0;
			ret = btrfs_search_slot(trans, root, &key, &path,
						-1, 1);
			if (ret < 0)
				goto out;
			continue;
		}
		found = 1;
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid > BTRFS_TREE_RELOC_OBJECTID)
			break;
		if (key.objectid != BTRFS_TREE_RELOC_OBJECTID) {
			path.slots[0]++;
			continue;
		}
		if (!del_nr) {
			del_slot = path.slots[0];
			del_nr = 1;
		} else {
			del_nr++;
		}
		path.slots[0]++;
	}

	if (del_nr) {
		ret = btrfs_del_items(trans, root, &path, del_slot, del_nr);
		if (ret)
			goto out;
	}
	btrfs_release_path(&path);

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
	btrfs_release_path(&path);
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
	if (btrfs_fs_incompat(fs_info, MIXED_GROUPS)) {
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
		fprintf(stderr, "error resetting the pending balance\n");

	return ret;
}

static int recow_extent_buffer(struct btrfs_root *root, struct extent_buffer *eb)
{
	struct btrfs_path path;
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

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	btrfs_init_path(&path);
	path.lowest_level = btrfs_header_level(eb);
	if (path.lowest_level)
		btrfs_node_key_to_cpu(eb, &key, 0);
	else
		btrfs_item_key_to_cpu(eb, &key, 0);

	ret = btrfs_search_slot(trans, root, &key, &path, 0, 1);
	btrfs_commit_transaction(trans, root);
	btrfs_release_path(&path);
	return ret;
}

static int delete_bad_item(struct btrfs_root *root, struct bad_item *bad)
{
	struct btrfs_path path;
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

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	btrfs_init_path(&path);
	ret = btrfs_search_slot(trans, root, &bad->key, &path, -1, 1);
	if (ret) {
		if (ret > 0)
			ret = 0;
		goto out;
	}
	ret = btrfs_del_item(trans, root, &path);
out:
	btrfs_commit_transaction(trans, root);
	btrfs_release_path(&path);
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
	struct btrfs_fs_info *fs_info = csum_root->fs_info;
	u64 offset = 0;
	u64 sectorsize;
	int ret = 0;

	while (offset < len) {
		sectorsize = fs_info->sectorsize;
		ret = read_extent_data(fs_info, buf, start + offset,
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
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *node;
	struct btrfs_file_extent_item *fi;
	char *buf = NULL;
	u64 start = 0;
	u64 len = 0;
	int slot = 0;
	int ret = 0;

	buf = malloc(cur_root->fs_info->sectorsize);
	if (!buf)
		return -ENOMEM;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.offset = 0;
	key.type = 0;
	ret = btrfs_search_slot(NULL, cur_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	/* Iterate all regular file extents and fill its csum */
	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

		if (key.type != BTRFS_EXTENT_DATA_KEY)
			goto next;
		node = path.nodes[0];
		slot = path.slots[0];
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
		ret = btrfs_next_item(cur_root, &path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}

out:
	btrfs_release_path(&path);
	free(buf);
	return ret;
}

static int fill_csum_tree_from_fs(struct btrfs_trans_handle *trans,
				  struct btrfs_root *csum_root)
{
	struct btrfs_fs_info *fs_info = csum_root->fs_info;
	struct btrfs_path path;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *cur_root;
	struct extent_buffer *node;
	struct btrfs_key key;
	int slot = 0;
	int ret = 0;

	btrfs_init_path(&path);
	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.offset = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	while (1) {
		node = path.nodes[0];
		slot = path.slots[0];
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
		ret = btrfs_next_item(tree_root, &path);
		if (ret > 0) {
			ret = 0;
			goto out;
		}
		if (ret < 0)
			goto out;
	}

out:
	btrfs_release_path(&path);
	return ret;
}

static int fill_csum_tree_from_extent(struct btrfs_trans_handle *trans,
				      struct btrfs_root *csum_root)
{
	struct btrfs_root *extent_root = csum_root->fs_info->extent_root;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	struct extent_buffer *leaf;
	char *buf;
	struct btrfs_key key;
	int ret;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}

	buf = malloc(csum_root->fs_info->sectorsize);
	if (!buf) {
		btrfs_release_path(&path);
		return -ENOMEM;
	}

	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(extent_root, &path);
			if (ret < 0)
				break;
			if (ret) {
				ret = 0;
				break;
			}
		}
		leaf = path.nodes[0];

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}

		ei = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_extent_item);
		if (!(btrfs_extent_flags(leaf, ei) &
		      BTRFS_EXTENT_FLAG_DATA)) {
			path.slots[0]++;
			continue;
		}

		ret = populate_csum(trans, csum_root, buf, key.objectid,
				    key.offset);
		if (ret)
			break;
		path.slots[0]++;
	}

	btrfs_release_path(&path);
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
	struct btrfs_path path;

	if (!roots_info_cache) {
		roots_info_cache = malloc(sizeof(*roots_info_cache));
		if (!roots_info_cache)
			return -ENOMEM;
		cache_tree_init(roots_info_cache);
	}

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, info->extent_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	leaf = path.nodes[0];

	while (1) {
		struct btrfs_key found_key;
		struct btrfs_extent_item *ei;
		struct btrfs_extent_inline_ref *iref;
		int slot = path.slots[0];
		int type;
		u64 flags;
		u64 root_id;
		u8 level;
		struct cache_extent *entry;
		struct root_item_info *rii;

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(info->extent_root, &path);
			if (ret < 0) {
				break;
			} else if (ret) {
				ret = 0;
				break;
			}
			leaf = path.nodes[0];
			slot = path.slots[0];
		}

		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);

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
			struct btrfs_tree_block_info *binfo;

			binfo = (struct btrfs_tree_block_info *)(ei + 1);
			iref = (struct btrfs_extent_inline_ref *)(binfo + 1);
			level = btrfs_tree_block_level(leaf, binfo);
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
		path.slots[0]++;
	}

out:
	btrfs_release_path(&path);

	return ret;
}

static int maybe_repair_root_item(struct btrfs_path *path,
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
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_trans_handle *trans = NULL;
	int ret = 0;
	int bad_roots = 0;
	int need_trans = 0;

	btrfs_init_path(&path);

	ret = build_roots_info_cache(info);
	if (ret)
		goto out;

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

	ret = btrfs_search_slot(trans, info->tree_root, &key, &path,
				0, trans ? 1 : 0);
	if (ret < 0)
		goto out;
	leaf = path.nodes[0];

	while (1) {
		struct btrfs_key found_key;

		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			int no_more_keys = find_next_key(&path, &key);

			btrfs_release_path(&path);
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

		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);

		if (found_key.type != BTRFS_ROOT_ITEM_KEY)
			goto next;
		if (found_key.objectid == BTRFS_TREE_RELOC_OBJECTID)
			goto next;

		ret = maybe_repair_root_item(&path, &found_key, trans ? 0 : 1);
		if (ret < 0)
			goto out;
		if (ret) {
			if (!trans && repair) {
				need_trans = 1;
				key = found_key;
				btrfs_release_path(&path);
				goto again;
			}
			bad_roots++;
		}
next:
		path.slots[0]++;
	}
	ret = 0;
out:
	free_roots_info_cache();
	btrfs_release_path(&path);
	if (trans)
		btrfs_commit_transaction(trans, info->tree_root);
	if (ret < 0)
		return ret;

	return bad_roots;
}

static int clear_free_space_cache(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_block_group_cache *bg_cache;
	u64 current = 0;
	int ret = 0;

	/* Clear all free space cache inodes and its extent data */
	while (1) {
		bg_cache = btrfs_lookup_first_block_group(fs_info, current);
		if (!bg_cache)
			break;
		ret = btrfs_clear_free_space_cache(fs_info, bg_cache);
		if (ret < 0)
			return ret;
		current = bg_cache->key.objectid + bg_cache->key.offset;
	}

	/* Don't forget to set cache_generation to -1 */
	trans = btrfs_start_transaction(fs_info->tree_root, 0);
	if (IS_ERR(trans)) {
		error("failed to update super block cache generation");
		return PTR_ERR(trans);
	}
	btrfs_set_super_cache_generation(fs_info->super_copy, (u64)-1);
	btrfs_commit_transaction(trans, fs_info->tree_root);

	return ret;
}

static int do_clear_free_space_cache(struct btrfs_fs_info *fs_info,
		int clear_version)
{
	int ret = 0;

	if (clear_version == 1) {
		if (btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE)) {
			error(
		"free space cache v2 detected, use --clear-space-cache v2");
			ret = 1;
			goto close_out;
		}
		printf("Clearing free space cache\n");
		ret = clear_free_space_cache(fs_info);
		if (ret) {
			error("failed to clear free space cache");
			ret = 1;
		} else {
			printf("Free space cache cleared\n");
		}
	} else if (clear_version == 2) {
		if (!btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE)) {
			printf("no free space cache v2 to clear\n");
			ret = 0;
			goto close_out;
		}
		printf("Clear free space cache v2\n");
		ret = btrfs_clear_free_space_tree(fs_info);
		if (ret) {
			error("failed to clear free space cache v2: %d", ret);
			ret = 1;
		} else {
			printf("free space cache v2 cleared\n");
		}
	}
close_out:
	return ret;
}

const char * const cmd_check_usage[] = {
	"btrfs check [options] <device>",
	"Check structural integrity of a filesystem (unmounted).",
	"Check structural integrity of an unmounted filesystem. Verify internal",
	"trees' consistency and item connectivity. In the repair mode try to",
	"fix the problems found. ",
	"WARNING: the repair mode is considered dangerous",
	"",
	"-s|--super <superblock>     use this superblock copy",
	"-b|--backup                 use the first valid backup root copy",
	"--force                     skip mount checks, repair is not possible",
	"--repair                    try to repair the filesystem",
	"--readonly                  run in read-only mode (default)",
	"--init-csum-tree            create a new CRC tree",
	"--init-extent-tree          create a new extent tree",
	"--mode <MODE>               allows choice of memory/IO trade-offs",
	"                            where MODE is one of:",
	"                            original - read inodes and extents to memory (requires",
	"                                       more memory, does less IO)",
	"                            lowmem   - try to use less memory but read blocks again",
	"                                       when needed",
	"--check-data-csum           verify checksums of data blocks",
	"-Q|--qgroup-report          print a report on qgroup consistency",
	"-E|--subvol-extents <subvolid>",
	"                            print subvolume extents and sharing state",
	"-r|--tree-root <bytenr>     use the given bytenr for the tree root",
	"--chunk-root <bytenr>       use the given bytenr for the chunk tree root",
	"-p|--progress               indicate progress",
	"--clear-space-cache v1|v2   clear space cache for v1 or v2",
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
	u64 chunk_root_bytenr = 0;
	char uuidbuf[BTRFS_UUID_UNPARSED_SIZE];
	int ret = 0;
	int err = 0;
	u64 num;
	int init_csum_tree = 0;
	int readonly = 0;
	int clear_space_cache = 0;
	int qgroup_report = 0;
	int qgroups_repaired = 0;
	unsigned ctree_flags = OPEN_CTREE_EXCLUSIVE;
	int force = 0;

	while(1) {
		int c;
		enum { GETOPT_VAL_REPAIR = 257, GETOPT_VAL_INIT_CSUM,
			GETOPT_VAL_INIT_EXTENT, GETOPT_VAL_CHECK_CSUM,
			GETOPT_VAL_READONLY, GETOPT_VAL_CHUNK_TREE,
			GETOPT_VAL_MODE, GETOPT_VAL_CLEAR_SPACE_CACHE,
			GETOPT_VAL_FORCE };
		static const struct option long_options[] = {
			{ "super", required_argument, NULL, 's' },
			{ "repair", no_argument, NULL, GETOPT_VAL_REPAIR },
			{ "readonly", no_argument, NULL, GETOPT_VAL_READONLY },
			{ "init-csum-tree", no_argument, NULL,
				GETOPT_VAL_INIT_CSUM },
			{ "init-extent-tree", no_argument, NULL,
				GETOPT_VAL_INIT_EXTENT },
			{ "check-data-csum", no_argument, NULL,
				GETOPT_VAL_CHECK_CSUM },
			{ "backup", no_argument, NULL, 'b' },
			{ "subvol-extents", required_argument, NULL, 'E' },
			{ "qgroup-report", no_argument, NULL, 'Q' },
			{ "tree-root", required_argument, NULL, 'r' },
			{ "chunk-root", required_argument, NULL,
				GETOPT_VAL_CHUNK_TREE },
			{ "progress", no_argument, NULL, 'p' },
			{ "mode", required_argument, NULL,
				GETOPT_VAL_MODE },
			{ "clear-space-cache", required_argument, NULL,
				GETOPT_VAL_CLEAR_SPACE_CACHE},
			{ "force", no_argument, NULL, GETOPT_VAL_FORCE },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "as:br:pEQ", long_options, NULL);
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
					error(
					"super mirror should be less than %d",
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
			case GETOPT_VAL_CHUNK_TREE:
				chunk_root_bytenr = arg_strtou64(optarg);
				break;
			case 'p':
				ctx.progress_enabled = true;
				break;
			case '?':
			case 'h':
				usage(cmd_check_usage);
			case GETOPT_VAL_REPAIR:
				printf("enabling repair mode\n");
				repair = 1;
				ctree_flags |= OPEN_CTREE_WRITES;
				break;
			case GETOPT_VAL_READONLY:
				readonly = 1;
				break;
			case GETOPT_VAL_INIT_CSUM:
				printf("Creating a new CRC tree\n");
				init_csum_tree = 1;
				repair = 1;
				ctree_flags |= OPEN_CTREE_WRITES;
				break;
			case GETOPT_VAL_INIT_EXTENT:
				init_extent_tree = 1;
				ctree_flags |= (OPEN_CTREE_WRITES |
						OPEN_CTREE_NO_BLOCK_GROUPS);
				repair = 1;
				break;
			case GETOPT_VAL_CHECK_CSUM:
				check_data_csum = 1;
				break;
			case GETOPT_VAL_MODE:
				check_mode = parse_check_mode(optarg);
				if (check_mode == CHECK_MODE_UNKNOWN) {
					error("unknown mode: %s", optarg);
					exit(1);
				}
				break;
			case GETOPT_VAL_CLEAR_SPACE_CACHE:
				if (strcmp(optarg, "v1") == 0) {
					clear_space_cache = 1;
				} else if (strcmp(optarg, "v2") == 0) {
					clear_space_cache = 2;
					ctree_flags |= OPEN_CTREE_INVALIDATE_FST;
				} else {
					error(
		"invalid argument to --clear-space-cache, must be v1 or v2");
					exit(1);
				}
				ctree_flags |= OPEN_CTREE_WRITES;
				break;
			case GETOPT_VAL_FORCE:
				force = 1;
				break;
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_check_usage);

	if (ctx.progress_enabled) {
		ctx.tp = TASK_NOTHING;
		ctx.info = task_init(print_status_check, print_status_return, &ctx);
	}

	/* This check is the only reason for --readonly to exist */
	if (readonly && repair) {
		error("repair options are not compatible with --readonly");
		exit(1);
	}

	/*
	 * experimental and dangerous
	 */
	if (repair && check_mode == CHECK_MODE_LOWMEM)
		warning("low-memory mode repair support is only partial");

	radix_tree_init();
	cache_tree_init(&root_cache);

	ret = check_mounted(argv[optind]);
	if (!force) {
		if (ret < 0) {
			error("could not check mount status: %s",
					strerror(-ret));
			err |= !!ret;
			goto err_out;
		} else if (ret) {
			error(
"%s is currently mounted, use --force if you really intend to check the filesystem",
				argv[optind]);
			ret = -EBUSY;
			err |= !!ret;
			goto err_out;
		}
	} else {
		if (repair) {
			error("repair and --force is not yet supported");
			ret = 1;
			err |= !!ret;
			goto err_out;
		}
		if (ret < 0) {
			warning(
"cannot check mount status of %s, the filesystem could be mounted, continuing because of --force",
				argv[optind]);
		} else if (ret) {
			warning(
			"filesystem mounted, continuing because of --force");
		}
		/* A block device is mounted in exclusive mode by kernel */
		ctree_flags &= ~OPEN_CTREE_EXCLUSIVE;
	}

	/* only allow partial opening under repair mode */
	if (repair)
		ctree_flags |= OPEN_CTREE_PARTIAL;

	info = open_ctree_fs_info(argv[optind], bytenr, tree_root_bytenr,
				  chunk_root_bytenr, ctree_flags);
	if (!info) {
		error("cannot open file system");
		ret = -EIO;
		err |= !!ret;
		goto err_out;
	}

	global_info = info;
	root = info->fs_root;
	uuid_unparse(info->super_copy->fsid, uuidbuf);

	printf("Checking filesystem on %s\nUUID: %s\n", argv[optind], uuidbuf);

	/*
	 * Check the bare minimum before starting anything else that could rely
	 * on it, namely the tree roots, any local consistency checks
	 */
	if (!extent_buffer_uptodate(info->tree_root->node) ||
	    !extent_buffer_uptodate(info->dev_root->node) ||
	    !extent_buffer_uptodate(info->chunk_root->node)) {
		error("critical roots corrupted, unable to check the filesystem");
		err |= !!ret;
		ret = -EIO;
		goto close_out;
	}

	if (clear_space_cache) {
		ret = do_clear_free_space_cache(info, clear_space_cache);
		err |= !!ret;
		goto close_out;
	}

	/*
	 * repair mode will force us to commit transaction which
	 * will make us fail to load log tree when mounting.
	 */
	if (repair && btrfs_super_log_root(info->super_copy)) {
		ret = ask_user("repair mode will force to clear out log tree, are you sure?");
		if (!ret) {
			ret = 1;
			err |= !!ret;
			goto close_out;
		}
		ret = zero_log_tree(root);
		err |= !!ret;
		if (ret) {
			error("failed to zero log tree: %d", ret);
			goto close_out;
		}
	}

	if (qgroup_report) {
		printf("Print quota groups for %s\nUUID: %s\n", argv[optind],
		       uuidbuf);
		ret = qgroup_verify_all(info);
		err |= !!ret;
		if (ret == 0)
			report_qgroups(1);
		goto close_out;
	}
	if (subvolid) {
		printf("Print extent state for subvolume %llu on %s\nUUID: %s\n",
		       subvolid, argv[optind], uuidbuf);
		ret = print_extent_state(info, subvolid);
		err |= !!ret;
		goto close_out;
	}

	if (init_extent_tree || init_csum_tree) {
		struct btrfs_trans_handle *trans;

		trans = btrfs_start_transaction(info->extent_root, 0);
		if (IS_ERR(trans)) {
			error("error starting transaction");
			ret = PTR_ERR(trans);
			err |= !!ret;
			goto close_out;
		}

		if (init_extent_tree) {
			printf("Creating a new extent tree\n");
			ret = reinit_extent_tree(trans, info);
			err |= !!ret;
			if (ret)
				goto close_out;
		}

		if (init_csum_tree) {
			printf("Reinitialize checksum tree\n");
			ret = btrfs_fsck_reinit_root(trans, info->csum_root, 0);
			if (ret) {
				error("checksum tree initialization failed: %d",
						ret);
				ret = -EIO;
				err |= !!ret;
				goto close_out;
			}

			ret = fill_csum_tree(trans, info->csum_root,
					     init_extent_tree);
			err |= !!ret;
			if (ret) {
				error("checksum tree refilling failed: %d", ret);
				return -EIO;
			}
		}
		/*
		 * Ok now we commit and run the normal fsck, which will add
		 * extent entries for all of the items it finds.
		 */
		ret = btrfs_commit_transaction(trans, info->extent_root);
		err |= !!ret;
		if (ret)
			goto close_out;
	}
	if (!extent_buffer_uptodate(info->extent_root->node)) {
		error("critical: extent_root, unable to check the filesystem");
		ret = -EIO;
		err |= !!ret;
		goto close_out;
	}
	if (!extent_buffer_uptodate(info->csum_root->node)) {
		error("critical: csum_root, unable to check the filesystem");
		ret = -EIO;
		err |= !!ret;
		goto close_out;
	}

	if (!init_extent_tree) {
		ret = repair_root_items(info);
		if (ret < 0) {
			err = !!ret;
			error("failed to repair root items: %s", strerror(-ret));
			goto close_out;
		}
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
			err |= ret;
			goto close_out;
		}
	}

	ret = do_check_chunks_and_extents(info);
	err |= !!ret;
	if (ret)
		error(
		"errors found in extent allocation tree or chunk allocation");

	/* Only re-check super size after we checked and repaired the fs */
	err |= !is_super_size_valid(info);

	if (!ctx.progress_enabled) {
		if (btrfs_fs_compat_ro(info, FREE_SPACE_TREE))
			fprintf(stderr, "checking free space tree\n");
		else
			fprintf(stderr, "checking free space cache\n");
	}
	ret = check_space_cache(root);
	err |= !!ret;
	if (ret) {
		if (btrfs_fs_compat_ro(info, FREE_SPACE_TREE))
			error("errors found in free space tree");
		else
			error("errors found in free space cache");
		goto out;
	}

	/*
	 * We used to have to have these hole extents in between our real
	 * extents so if we don't have this flag set we need to make sure there
	 * are no gaps in the file extents for inodes, otherwise we can just
	 * ignore it when this happens.
	 */
	no_holes = btrfs_fs_incompat(root->fs_info, NO_HOLES);
	ret = do_check_fs_roots(info, &root_cache);
	err |= !!ret;
	if (ret) {
		error("errors found in fs roots");
		goto out;
	}

	fprintf(stderr, "checking csums\n");
	ret = check_csums(root);
	err |= !!ret;
	if (ret) {
		error("errors found in csum tree");
		goto out;
	}

	fprintf(stderr, "checking root refs\n");
	/* For low memory mode, check_fs_roots_v2 handles root refs */
	if (check_mode != CHECK_MODE_LOWMEM) {
		ret = check_root_refs(root, &root_cache);
		err |= !!ret;
		if (ret) {
			error("errors found in root refs");
			goto out;
		}
	}

	while (repair && !list_empty(&root->fs_info->recow_ebs)) {
		struct extent_buffer *eb;

		eb = list_first_entry(&root->fs_info->recow_ebs,
				      struct extent_buffer, recow);
		list_del_init(&eb->recow);
		ret = recow_extent_buffer(root, eb);
		err |= !!ret;
		if (ret) {
			error("fails to fix transid errors");
			break;
		}
	}

	while (!list_empty(&delete_items)) {
		struct bad_item *bad;

		bad = list_first_entry(&delete_items, struct bad_item, list);
		list_del_init(&bad->list);
		if (repair) {
			ret = delete_bad_item(root, bad);
			err |= !!ret;
		}
		free(bad);
	}

	if (info->quota_enabled) {
		fprintf(stderr, "checking quota groups\n");
		ret = qgroup_verify_all(info);
		err |= !!ret;
		if (ret) {
			error("failed to check quota groups");
			goto out;
		}
		report_qgroups(0);
		ret = repair_qgroups(info, &qgroups_repaired);
		err |= !!ret;
		if (err) {
			error("failed to repair quota groups");
			goto out;
		}
		ret = 0;
	}

	if (!list_empty(&root->fs_info->recow_ebs)) {
		error("transid errors in file system");
		ret = 1;
		err |= !!ret;
	}
out:
	printf("found %llu bytes used, ",
	       (unsigned long long)bytes_used);
	if (err)
		printf("error(s) found\n");
	else
		printf("no error found\n");
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

	free_qgroup_counts();
	free_root_recs_tree(&root_cache);
close_out:
	close_ctree(root);
err_out:
	if (ctx.progress_enabled)
		task_deinit(ctx.info);

	return err;
}
