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
#include <time.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "common/repair.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "common/task-utils.h"
#include "kernel-shared/transaction.h"
#include "common/utils.h"
#include "cmds/commands.h"
#include "kernel-shared/free-space-cache.h"
#include "kernel-shared/free-space-tree.h"
#include "common/rbtree-utils.h"
#include "kernel-shared/backref.h"
#include "kernel-shared/ulist.h"
#include "common/help.h"
#include "check/common.h"
#include "check/mode-common.h"
#include "check/mode-original.h"
#include "check/mode-lowmem.h"
#include "check/qgroup-verify.h"
#include "common/open-utils.h"
#include "mkfs/common.h"

u64 bytes_used = 0;
u64 total_csum_bytes = 0;
u64 total_btree_bytes = 0;
u64 total_fs_tree_bytes = 0;
u64 total_extent_tree_bytes = 0;
u64 btree_space_waste = 0;
u64 data_bytes_allocated = 0;
u64 data_bytes_referenced = 0;
LIST_HEAD(duplicate_extents);
LIST_HEAD(delete_items);
int no_holes = 0;
static int is_free_space_tree = 0;
int init_extent_tree = 0;
int check_data_csum = 0;
struct btrfs_fs_info *gfs_info;
struct task_ctx ctx = { 0 };
struct cache_tree *roots_info_cache = NULL;

enum btrfs_check_mode {
	CHECK_MODE_ORIGINAL,
	CHECK_MODE_LOWMEM,
	CHECK_MODE_UNKNOWN,
	CHECK_MODE_DEFAULT = CHECK_MODE_ORIGINAL
};

static enum btrfs_check_mode check_mode = CHECK_MODE_DEFAULT;

struct device_record {
	struct rb_node node;
	u64 devid;

	u64 generation;

	u64 objectid;
	u8  type;
	u64 offset;

	u64 total_byte;
	u64 byte_used;

	u64 real_used;
};

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

static void print_status_check_line(void *p)
{
	struct task_ctx *priv = p;
	const char *task_position_string[] = {
		"[1/7] checking root items                     ",
		"[2/7] checking extents                        ",
		is_free_space_tree ?
		"[3/7] checking free space tree                " :
		"[3/7] checking free space cache               ",
		"[4/7] checking fs roots                       ",
		check_data_csum ?
		"[5/7] checking csums against data             " :
		"[5/7] checking csums (without verifying data) ",
		"[6/7] checking root refs                      ",
		"[7/7] checking quota groups                   ",
	};
	time_t elapsed;
	int hours;
	int minutes;
	int seconds;

	elapsed = time(NULL) - priv->start_time;
	hours   = elapsed  / 3600;
	elapsed -= hours   * 3600;
	minutes = elapsed  / 60;
	elapsed -= minutes * 60;
	seconds = elapsed;

	printf("%s (%d:%02d:%02d elapsed", task_position_string[priv->tp],
			hours, minutes, seconds);
	if (priv->item_count > 0)
		printf(", %llu items checked)\r", priv->item_count);
	else
		printf(")\r");
	fflush(stdout);
}

static void *print_status_check(void *p)
{
	struct task_ctx *priv = p;

	/* 1 second */
	task_period_start(priv->info, 1000);

	if (priv->tp == TASK_NOTHING)
		return NULL;

	while (1) {
		print_status_check_line(p);
		task_period_wait(priv->info);
	}
	return NULL;
}

static int print_status_return(void *p)
{
	print_status_check_line(p);
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
	struct mismatch_dir_hash_record *hash_record;
	struct mismatch_dir_hash_record *new_record;
	struct unaligned_extent_rec_t *src;
	struct unaligned_extent_rec_t *dst;
	struct rb_node *rb;
	size_t size;
	int ret;

	rec = malloc(sizeof(*rec));
	if (!rec)
		return ERR_PTR(-ENOMEM);
	memcpy(rec, orig_rec, sizeof(*rec));
	rec->refs = 1;
	INIT_LIST_HEAD(&rec->backrefs);
	INIT_LIST_HEAD(&rec->mismatch_dir_hash);
	INIT_LIST_HEAD(&rec->unaligned_extent_recs);
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
	list_for_each_entry(hash_record, &orig_rec->mismatch_dir_hash, list) {
		size = sizeof(*hash_record) + hash_record->namelen;
		new_record = malloc(size);
		if (!new_record) {
			ret = -ENOMEM;
			goto cleanup;
		}
		memcpy(&new_record, hash_record, size);
		list_add_tail(&new_record->list, &rec->mismatch_dir_hash);
	}
	list_for_each_entry(src, &orig_rec->unaligned_extent_recs, list) {
		size = sizeof(*src);
		dst = malloc(size);
		if (!dst) {
			ret = -ENOMEM;
			goto cleanup;
		}
		memcpy(dst, src, size);
		list_add_tail(&dst->list, &rec->unaligned_extent_recs);
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

	if (!list_empty(&rec->mismatch_dir_hash)) {
		list_for_each_entry_safe(hash_record, new_record,
				&rec->mismatch_dir_hash, list) {
			list_del(&hash_record->list);
			free(hash_record);
		}
	}
	if (!list_empty(&rec->unaligned_extent_recs))
		list_for_each_entry_safe(src, dst, &rec->unaligned_extent_recs,
				list) {
			list_del(&src->list);
			free(src);
		}

	free(rec);

	return ERR_PTR(ret);
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
	if (errors & I_ERR_FILE_EXTENT_TOO_LARGE)
		fprintf(stderr, ", inline file extent too large");
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
	if (errors & I_ERR_ODD_INODE_FLAGS)
		fprintf(stderr, ", odd inode flags");
	if (errors & I_ERR_INLINE_RAM_BYTES_WRONG)
		fprintf(stderr, ", invalid inline ram bytes");
	if (errors & I_ERR_INVALID_IMODE)
		fprintf(stderr, ", invalid inode mode bit 0%o",
			rec->imode & ~07777);
	if (errors & I_ERR_INVALID_GEN)
		fprintf(stderr, ", invalid inode generation or transid");
	if (errors & I_ERR_INVALID_NLINK)
		fprintf(stderr, ", directory has invalid nlink %d",
			rec->nlink);
	fprintf(stderr, "\n");

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
		if (!found) {
			u64 start, len;
			if (rec->extent_end < rec->isize) {
				start = rec->extent_end;
				len = round_up(rec->isize,
					       gfs_info->sectorsize) - start;
			} else {
				start = 0;
				len = rec->extent_start;
			}
			fprintf(stderr, "\tstart: %llu, len: %llu\n", start,
				len);
		}
	}

	/* Print dir item with mismatch hash */
	if (errors & I_ERR_MISMATCH_DIR_HASH) {
		struct mismatch_dir_hash_record *hash_record;

		fprintf(stderr, "Dir items with mismatch hash:\n");
		list_for_each_entry(hash_record, &rec->mismatch_dir_hash,
				list) {
			char *namebuf = (char *)(hash_record + 1);
			u32 crc;

			crc = btrfs_name_hash(namebuf, hash_record->namelen);
			fprintf(stderr,
			"\tname: %.*s namelen: %u wanted 0x%08x has 0x%08llx\n",
				hash_record->namelen, namebuf,
				hash_record->namelen, crc,
				hash_record->key.offset);
		}
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
		INIT_LIST_HEAD(&rec->mismatch_dir_hash);
		INIT_LIST_HEAD(&rec->unaligned_extent_recs);
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

static void free_unaligned_extent_recs(struct list_head *unaligned_extent_recs)
{
	struct unaligned_extent_rec_t *urec;

	while (!list_empty(unaligned_extent_recs)) {
		urec = list_entry(unaligned_extent_recs->next,
				struct unaligned_extent_rec_t, list);
		list_del(&urec->list);
		free(urec);
	}
}

static void free_inode_rec(struct inode_record *rec)
{
	struct inode_backref *backref;
	struct mismatch_dir_hash_record *hash;
	struct mismatch_dir_hash_record *next;

	if (--rec->refs > 0)
		return;

	while (!list_empty(&rec->backrefs)) {
		backref = to_inode_backref(rec->backrefs.next);
		list_del(&backref->list);
		free(backref);
	}
	list_for_each_entry_safe(hash, next, &rec->mismatch_dir_hash, list)
		free(hash);
	free_unaligned_extent_recs(&rec->unaligned_extent_recs);
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

	if (!is_valid_imode(rec->imode))
		rec->errors |= I_ERR_INVALID_IMODE;
	if (S_ISDIR(rec->imode)) {
		if (rec->found_size != rec->isize)
			rec->errors |= I_ERR_DIR_ISIZE_WRONG;
		if (rec->found_file_extent)
			rec->errors |= I_ERR_ODD_FILE_EXTENT;
	} else if (S_ISREG(rec->imode) || S_ISLNK(rec->imode)) {
		if (rec->found_dir_item)
			rec->errors |= I_ERR_ODD_DIR_ITEM;
		/* Orphan inodes don't have correct nbytes */
		if (rec->nlink > 0 && rec->found_size != rec->nbytes)
			rec->errors |= I_ERR_FILE_NBYTES_WRONG;
		if (rec->nlink > 0 && !no_holes && rec->isize &&
		    (rec->extent_end < rec->isize ||
		     rec->extent_start != 0 ||
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
	u64 gen_uplimit;
	u64 flags;

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
	flags = btrfs_inode_flags(eb, item);
	if (S_ISLNK(rec->imode) &&
	    flags & (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND))
		rec->errors |= I_ERR_ODD_INODE_FLAGS;

	/* Directory should never have hard link */
	if (S_ISDIR(rec->imode) && rec->nlink >= 2)
		rec->errors |= I_ERR_INVALID_NLINK;
	/*
	 * We don't have accurate root info to determine the correct
	 * inode generation uplimit, use super_generation + 1 anyway
	 */
	gen_uplimit = btrfs_super_generation(gfs_info->super_copy) + 1;
	if (btrfs_inode_generation(eb, item) > gen_uplimit ||
	    btrfs_inode_transid(eb, item) > gen_uplimit)
		rec->errors |= I_ERR_INVALID_GEN;
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
	ret = btrfs_search_slot(NULL, gfs_info->tree_root, &key, &path,
				0, 0);
	if (ret < 0)
		return ret;
	btrfs_release_path(&path);
	if (!ret)
		return 1;

	key.objectid = child_root_id;
	key.type = BTRFS_ROOT_BACKREF_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, gfs_info->tree_root, &key, &path,
				0, 0);
	if (ret < 0)
		goto out;

	while (1) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(gfs_info->tree_root, &path);
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

static int add_mismatch_dir_hash(struct inode_record *dir_rec,
				 struct btrfs_key *key, const char *namebuf,
				 int namelen)
{
	struct mismatch_dir_hash_record *hash_record;

	hash_record = malloc(sizeof(*hash_record) + namelen);
	if (!hash_record) {
		error("failed to allocate memory for mismatch dir hash rec");
		return -ENOMEM;
	}
	memcpy(&hash_record->key, key, sizeof(*key));
	memcpy(hash_record + 1, namebuf, namelen);
	hash_record->namelen = namelen;

	list_add(&hash_record->list, &dir_rec->mismatch_dir_hash);
	return 0;
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
		int ret;

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
			rec->errors |= I_ERR_MISMATCH_DIR_HASH;
			ret = add_mismatch_dir_hash(rec, key, namebuf, len);
			/* Fatal error, ENOMEM */
			if (ret < 0)
				return ret;
			goto next;
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
			fprintf(stderr,
				"unknown location type %d in DIR_ITEM[%llu %llu]\n",
				location.type, key->objectid, key->offset);
			add_inode_backref(inode_cache, BTRFS_MULTIPLE_OBJECTIDS,
					  key->objectid, key->offset, namebuf,
					  len, filetype, key->type, error);
		}

next:
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
	u64 mask = gfs_info->sectorsize - 1;
	u32 max_inline_size = min_t(u32, mask,
				BTRFS_MAX_INLINE_DATA_SIZE(gfs_info));
	u8 compression;
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
	compression = btrfs_file_extent_compression(eb, fi);

	if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
		struct btrfs_item *item = btrfs_item_nr(slot);

		num_bytes = btrfs_file_extent_ram_bytes(eb, fi);
		if (num_bytes == 0)
			rec->errors |= I_ERR_BAD_FILE_EXTENT;
		if (compression) {
			if (btrfs_file_extent_inline_item_len(eb, item) >
			    max_inline_size ||
			    num_bytes > gfs_info->sectorsize)
				rec->errors |= I_ERR_FILE_EXTENT_TOO_LARGE;
		} else {
			if (num_bytes > max_inline_size)
				rec->errors |= I_ERR_FILE_EXTENT_TOO_LARGE;
			if (btrfs_file_extent_inline_item_len(eb, item) !=
			    num_bytes)
				rec->errors |= I_ERR_INLINE_RAM_BYTES_WRONG;
		}
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
		if (compression && rec->nodatasum)
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

		if (compression)
			num_bytes = btrfs_file_extent_disk_num_bytes(eb, fi);
		else
			disk_bytenr += extent_offset;

		ret = count_csum_range(disk_bytenr, num_bytes, &found);
		if (ret < 0)
			return ret;
		if (extent_type == BTRFS_FILE_EXTENT_REG) {
			if (found > 0)
				rec->found_csum_item = 1;
			if (found < num_bytes)
				rec->some_csum_missing = 1;
			if (compression && found < num_bytes)
				rec->errors |= I_ERR_SOME_CSUM_MISSING;
		} else if (extent_type == BTRFS_FILE_EXTENT_PREALLOC) {
			if (found > 0) {
				ret = check_prealloc_extent_written(disk_bytenr,
								    num_bytes);
				if (ret < 0)
					return ret;
				if (ret == 0)
					rec->errors |= I_ERR_ODD_CSUM_ITEM;
			}
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

static int walk_down_tree(struct btrfs_root *root, struct btrfs_path *path,
			  struct walk_control *wc, int *level,
			  struct node_refs *nrefs)
{
	enum btrfs_tree_block_status status;
	u64 bytenr;
	u64 ptr_gen;
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
		ret = btrfs_lookup_extent_info(NULL, gfs_info,
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
			ret = btrfs_lookup_extent_info(NULL, gfs_info, bytenr,
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

		next = btrfs_find_tree_block(gfs_info, bytenr, gfs_info->nodesize);
		if (!next || !btrfs_buffer_uptodate(next, ptr_gen)) {
			free_extent_buffer(next);
			reada_walk_down(root, cur, path->slots[*level]);
			next = read_tree_block(gfs_info, bytenr, ptr_gen);
			if (!extent_buffer_uptodate(next)) {
				struct btrfs_key node_key;

				btrfs_node_key_to_cpu(path->nodes[*level],
						      &node_key,
						      path->slots[*level]);
				btrfs_add_corrupt_extent_record(gfs_info,
						&node_key,
						path->nodes[*level]->start,
						gfs_info->nodesize,
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
			status = btrfs_check_leaf(gfs_info, NULL, next);
		else
			status = btrfs_check_node(gfs_info, NULL, next);
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
		}
		free_extent_buffer(path->nodes[*level]);
		path->nodes[*level] = NULL;
		BUG_ON(*level > wc->active_node);
		if (*level == wc->active_node)
			leave_shared_node(root, wc, *level);
		*level = i + 1;
	}
	return 1;
}

static int check_root_dir(struct inode_record *rec)
{
	struct inode_backref *backref;
	int ret = -1;

	if (rec->errors)
		goto out;

	if (!rec->found_inode_item) {
		rec->errors |= I_ERR_NO_INODE_ITEM;
		goto out;
	}

	if (rec->nlink != 1 || rec->found_link != 0) {
		rec->errors |= I_ERR_LINK_COUNT_WRONG;
		goto out;
	}

	if (list_empty(&rec->backrefs)) {
		rec->errors |= REF_ERR_NO_ROOT_BACKREF;
		goto out;
	}

	backref = to_inode_backref(rec->backrefs.next);
	if (!backref->found_inode_ref) {
		rec->errors |= REF_ERR_NO_INODE_REF;
		goto out;
	}

	if (backref->index != 0 || backref->namelen != 2 ||
	    memcmp(backref->name, "..", 2)) {
		rec->errors |= I_ERR_ODD_DIR_ITEM;
		goto out;
	}

	if (backref->found_dir_index) {
		rec->errors |= REF_ERR_DUP_DIR_INDEX;
		goto out;
	}

	if (backref->found_dir_item) {
		rec->errors |= REF_ERR_DUP_DIR_ITEM;
		goto out;
	}
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
	printf("reset isize for dir %llu root %llu\n", rec->ino,
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
	di = btrfs_lookup_dir_index_item(trans, root, &path, backref->dir,
					 backref->index, backref->name,
					 backref->namelen, -1);
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

	ret = insert_inode_item(trans, root, rec->ino, size, rec->nbytes,
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
		errno = -ret;
		fprintf(stderr,
			"Failed to reset nlink for inode %llu: %m\n", rec->ino);
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
				       round_up(rec->isize, gfs_info->sectorsize));
		if (ret < 0)
			goto out;
	}
	printf("Fixed discount file extents for inode: %llu in root: %llu\n",
	       rec->ino, root->objectid);
out:
	return ret;
}

static int repair_inline_ram_bytes(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct btrfs_path *path,
				   struct inode_record *rec)
{
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	struct btrfs_item *i;
	u64 on_disk_item_len;
	int ret;

	key.objectid = rec->ino;
	key.offset = 0;
	key.type = BTRFS_EXTENT_DATA_KEY;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto out;

	i = btrfs_item_nr(path->slots[0]);
	on_disk_item_len = btrfs_file_extent_inline_item_len(path->nodes[0], i);
	fi = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_file_extent_item);
	btrfs_set_file_extent_ram_bytes(path->nodes[0], fi, on_disk_item_len);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	printf("Repaired inline ram_bytes for root %llu ino %llu\n",
		root->objectid, rec->ino);
	rec->errors &= ~I_ERR_INLINE_RAM_BYTES_WRONG;
out:
	btrfs_release_path(path);
	return ret;
}

static int repair_mismatch_dir_hash(struct btrfs_trans_handle *trans,
				    struct btrfs_root *root,
				    struct inode_record *rec)
{
	struct mismatch_dir_hash_record *hash;
	int ret = -EUCLEAN;

	printf(
	"Deleting bad dir items with invalid hash for root %llu ino %llu\n",
		root->root_key.objectid, rec->ino);
	while (!list_empty(&rec->mismatch_dir_hash)) {
		char *namebuf;

		hash = list_entry(rec->mismatch_dir_hash.next,
				struct mismatch_dir_hash_record, list);
		namebuf = (char *)(hash + 1);

		ret = delete_corrupted_dir_item(trans, root, &hash->key,
						namebuf, hash->namelen);
		if (ret < 0)
			break;

		/* Also reduce dir isize */
		rec->found_size -= hash->namelen;
		list_del(&hash->list);
		free(hash);
	}
	if (!ret) {
		rec->errors &= ~I_ERR_MISMATCH_DIR_HASH;
		/* We rely on later dir isize repair to reset dir isize */
		rec->errors |= I_ERR_DIR_ISIZE_WRONG;
	}
	return ret;
}

static int btrfs_delete_item(struct btrfs_trans_handle *trans,
		struct btrfs_root *root, struct btrfs_key *key)
{
	struct btrfs_path path;
	int ret = 0;

	btrfs_init_path(&path);

	ret = btrfs_search_slot(trans, root, key, &path, -1, 1);
	if (ret) {
		if (ret > 0)
			ret = -ENOENT;

		btrfs_release_path(&path);
		return ret;
	}

	ret = btrfs_del_item(trans, root, &path);

	btrfs_release_path(&path);
	return ret;
}

static int find_file_extent_offset_by_bytenr(struct btrfs_root *root,
		u64 owner, u64 bytenr, u64 *offset_ret)
{
	int ret = 0;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_file_extent_item *fi;
	struct extent_buffer *leaf;
	u64 disk_bytenr;
	int slot;

	btrfs_init_path(&path);

	key.objectid = owner;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret) {
		if (ret > 0)
			ret = -ENOENT;
		btrfs_release_path(&path);
		return ret;
	}

	btrfs_release_path(&path);

	key.objectid = owner;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}

	while (1) {
		leaf = path.nodes[0];
		slot = path.slots[0];

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret) {
				if (ret > 0)
					ret = 0;
				break;
			}

			leaf = path.nodes[0];
			slot = path.slots[0];
		}

		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		if ((found_key.objectid != owner) ||
			(found_key.type != BTRFS_EXTENT_DATA_KEY))
			break;

		fi = btrfs_item_ptr(leaf, slot,
				struct btrfs_file_extent_item);

		disk_bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
		if (disk_bytenr == bytenr) {
			*offset_ret = found_key.offset;
			ret = 0;
			break;
		}
		path.slots[0]++;
	}

	btrfs_release_path(&path);
	return ret;
}

static int repair_unaligned_extent_recs(struct btrfs_trans_handle *trans,
				struct btrfs_root *root,
				struct btrfs_path *path,
				struct inode_record *rec)
{
	int ret = 0;
	struct btrfs_key key;
	struct unaligned_extent_rec_t *urec;
	struct unaligned_extent_rec_t *tmp;

	list_for_each_entry_safe(urec, tmp, &rec->unaligned_extent_recs, list) {

		key.objectid = urec->owner;
		key.type = BTRFS_EXTENT_DATA_KEY;
		key.offset = urec->offset;
		fprintf(stderr, "delete file extent item [%llu,%llu]\n",
					urec->owner, urec->offset);
		ret = btrfs_delete_item(trans, root, &key);
		if (ret)
			return ret;

		list_del(&urec->list);
		free(urec);
	}
	rec->errors &= ~I_ERR_UNALIGNED_EXTENT_REC;

	return ret;
}

static int repair_imode_original(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct inode_record *rec)
{
	struct btrfs_key key;
	int ret;
	u32 imode;

	key.objectid = rec->ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		return ret;

	if (root->objectid == BTRFS_ROOT_TREE_OBJECTID) {
		/* In root tree we only have two possible imode */
		if (rec->ino == BTRFS_ROOT_TREE_OBJECTID)
			imode = S_IFDIR | 0755;
		else
			imode = S_IFREG | 0600;
	} else {
		ret = detect_imode(root, path, &imode);
		if (ret < 0) {
			btrfs_release_path(path);
			return ret;
		}
	}
	btrfs_release_path(path);
	ret = reset_imode(trans, root, path, rec->ino, imode);
	btrfs_release_path(path);
	if (ret < 0)
		return ret;
	rec->errors &= ~I_ERR_INVALID_IMODE;
	rec->imode = imode;
	return ret;
}

static int repair_inode_gen_original(struct btrfs_trans_handle *trans,
				     struct btrfs_root *root,
				     struct btrfs_path *path,
				     struct inode_record *rec)
{
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	int ret;

	key.objectid = rec->ino;
	key.offset = 0;
	key.type = BTRFS_INODE_ITEM_KEY;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0) {
		ret = -ENOENT;
		error("no inode item found for ino %llu", rec->ino);
		return ret;
	}
	if (ret < 0) {
		errno = -ret;
		error("failed to search inode item for ino %llu: %m", rec->ino);
		return ret;
	}
	ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_generation(path->nodes[0], ii, trans->transid);
	btrfs_set_inode_transid(path->nodes[0], ii, trans->transid);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	btrfs_release_path(path);
	printf("resetting inode generation/transid to %llu for ino %llu\n",
		trans->transid, rec->ino);
	rec->errors &= ~I_ERR_INVALID_GEN;
	return 0;
}

static int try_repair_inode(struct btrfs_root *root, struct inode_record *rec)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_path path;
	int ret = 0;

	/* unaligned extent recs always lead to csum missing error, clean it */
	if ((rec->errors & I_ERR_SOME_CSUM_MISSING) &&
			(rec->errors & I_ERR_UNALIGNED_EXTENT_REC))
		rec->errors &= ~I_ERR_SOME_CSUM_MISSING;


	if (!(rec->errors & (I_ERR_DIR_ISIZE_WRONG |
			     I_ERR_NO_ORPHAN_ITEM |
			     I_ERR_LINK_COUNT_WRONG |
			     I_ERR_NO_INODE_ITEM |
			     I_ERR_FILE_EXTENT_DISCOUNT |
			     I_ERR_FILE_NBYTES_WRONG |
			     I_ERR_INLINE_RAM_BYTES_WRONG |
			     I_ERR_MISMATCH_DIR_HASH |
			     I_ERR_UNALIGNED_EXTENT_REC |
			     I_ERR_INVALID_IMODE |
			     I_ERR_INVALID_GEN)))
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
	if (!ret && rec->errors & I_ERR_MISMATCH_DIR_HASH)
		ret = repair_mismatch_dir_hash(trans, root, rec);
	if (!ret && rec->errors & I_ERR_INVALID_IMODE)
		ret = repair_imode_original(trans, root, &path, rec);
	if (rec->errors & I_ERR_NO_INODE_ITEM)
		ret = repair_inode_no_item(trans, root, &path, rec);
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
	if (!ret && rec->errors & I_ERR_INLINE_RAM_BYTES_WRONG)
		ret = repair_inline_ram_bytes(trans, root, &path, rec);
	if (!ret && rec->errors & I_ERR_UNALIGNED_EXTENT_REC)
		ret = repair_unaligned_extent_recs(trans, root, &path, rec);
	if (!ret && rec->errors & I_ERR_INVALID_GEN)
		ret = repair_inode_gen_original(trans, root, &path, rec);
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
		if (repair) {
			ret = try_repair_inode(root, rec);
			if (ret < 0)
				error++;
		}
		ret = check_root_dir(rec);
		if (ret) {
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
			if (ret < 0) {
				btrfs_abort_transaction(trans, ret);
				return ret;
			}

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
			ctx.item_count++;
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
			ret = check_orphan_item(gfs_info->tree_root, rec->objectid);
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
		errno = -ret;
		fprintf(stderr, "Error starting transaction: %m\n");
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
				gfs_info->nodesize, 0,
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
	bool generation_err = false;
	int wret;
	int level;
	u64 super_generation;
	struct btrfs_path path;
	struct shared_node root_node;
	struct root_record *rec;
	struct btrfs_root_item *root_item = &root->root_item;
	struct cache_tree corrupt_blocks;
	enum btrfs_tree_block_status status;
	struct node_refs nrefs;
	struct unaligned_extent_rec_t *urec;
	struct unaligned_extent_rec_t *tmp;

	super_generation = btrfs_super_generation(gfs_info->super_copy);
	if (btrfs_root_generation(root_item) > super_generation + 1) {
		error(
	"invalid generation for root %llu, have %llu expect (0, %llu]",
		      root->root_key.objectid, btrfs_root_generation(root_item),
		      super_generation + 1);
		generation_err = true;
		if (repair) {
			root->node->flags |= EXTENT_BAD_TRANSID;
			ret = recow_extent_buffer(root, root->node);
			if (!ret) {
				printf("Reset generation for root %llu\n",
					root->root_key.objectid);
				generation_err = false;
			}
		}
	}
	/*
	 * Reuse the corrupt_block cache tree to record corrupted tree block
	 *
	 * Unlike the usage in extent tree check, here we do it in a per
	 * fs/subvol tree base.
	 */
	cache_tree_init(&corrupt_blocks);
	gfs_info->corrupt_blocks = &corrupt_blocks;

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

	/* Mode unaligned extent recs to corresponding inode record */
	list_for_each_entry_safe(urec, tmp,
			&root->unaligned_extent_recs, list) {
		struct inode_record *inode;

		inode = get_inode_rec(&root_node.inode_cache, urec->owner, 1);

		if (IS_ERR_OR_NULL(inode)) {
			fprintf(stderr,
				"fail to get inode rec on [%llu,%llu]\n",
				urec->objectid, urec->owner);

			list_del(&urec->list);
			free(urec);

			continue;
		}

		inode->errors |= I_ERR_UNALIGNED_EXTENT_REC;
		list_move(&urec->list, &inode->unaligned_extent_recs);
	}

	level = btrfs_header_level(root->node);
	memset(wc->nodes, 0, sizeof(wc->nodes));
	wc->nodes[level] = &root_node;
	wc->active_node = level;
	wc->root_level = level;

	/* We may not have checked the root block, lets do that now */
	if (btrfs_is_leaf(root->node))
		status = btrfs_check_leaf(gfs_info, NULL, root->node);
	else
		status = btrfs_check_node(gfs_info, NULL, root->node);
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
		ctx.item_count++;
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
			if (ret < 0) {
				errno = -ret;
				fprintf(stderr, "Failed to repair btree: %m\n");
			}
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
	gfs_info->corrupt_blocks = NULL;
	if (!ret && generation_err)
		ret = -1;
	return ret;
}

static int check_fs_roots(struct cache_tree *root_cache)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct walk_control wc;
	struct extent_buffer *leaf, *tree_node;
	struct btrfs_root *tmp_root;
	struct btrfs_root *tree_root = gfs_info->tree_root;
	u64 skip_root = 0;
	int ret;
	int err = 0;

	/*
	 * Just in case we made any changes to the extent tree that weren't
	 * reflected into the free space cache yet.
	 */
	if (repair)
		reset_cached_block_groups();
	memset(&wc, 0, sizeof(wc));
	cache_tree_init(&wc.shared);
	btrfs_init_path(&path);

again:
	key.offset = 0;
	if (skip_root)
		key.objectid = skip_root + 1;
	else
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
						gfs_info, &key);
			} else {
				key.offset = (u64)-1;
				tmp_root = btrfs_read_fs_root(
						gfs_info, &key);
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
			if (ret) {
				err = 1;

				/*
				 * We failed to repair this root but modified
				 * tree root, after again: label we will still
				 * hit this root and fail to repair, so we must
				 * skip it to avoid infinite loop.
				 */
				if (repair)
					skip_root = key.objectid;
			}
			if (key.objectid == BTRFS_TREE_RELOC_OBJECTID)
				btrfs_free_fs_root(tmp_root);
		} else if (key.type == BTRFS_ROOT_REF_KEY ||
			   key.type == BTRFS_ROOT_BACKREF_KEY) {
			process_root_ref(leaf, path.slots[0], &key,
					 root_cache);
		} else if (key.type == BTRFS_INODE_ITEM_KEY &&
			   is_fstree(key.objectid)) {
			ret = check_repair_free_space_inode(&path);
			if (ret < 0 && !path.nodes[0]) {
				err = 1;
				goto out;
			}
			if (ret < 0 && path.nodes[0]) {
				err = 1;
				goto next;
			}
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

	return err;
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

static int do_check_fs_roots(struct cache_tree *root_cache)
{
	int ret;

	if (check_mode == CHECK_MODE_LOWMEM)
		ret = check_fs_roots_lowmem();
	else
		ret = check_fs_roots(root_cache);

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
				fprintf(stderr,
"data backref %llu %s %llu owner %llu offset %llu num_refs %lu not found in extent tree\n",
					(unsigned long long)rec->start,
					back->full_backref ?
					"parent" : "root",
					back->full_backref ?
					(unsigned long long)dback->parent :
					(unsigned long long)dback->root,
					(unsigned long long)dback->owner,
					(unsigned long long)dback->offset,
					(unsigned long)dback->num_refs);
			} else {
				tback = to_tree_backref(back);
				fprintf(stderr,
"tree backref %llu %s %llu not found in extent tree\n",
					(unsigned long long)rec->start,
					back->full_backref ? "parent" : "root",
					back->full_backref ?
					(unsigned long long)tback->parent :
					(unsigned long long)tback->root);
			}
		}
		if (!back->is_data && !back->found_ref) {
			err = 1;
			if (!print_errs)
				goto out;
			tback = to_tree_backref(back);
			fprintf(stderr,
				"backref %llu %s %llu not referenced back %p\n",
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
				fprintf(stderr,
"incorrect local backref count on %llu %s %llu owner %llu offset %llu found %u wanted %u back %p\n",
					(unsigned long long)rec->start,
					back->full_backref ?
					"parent" : "root",
					back->full_backref ?
					(unsigned long long)dback->parent :
					(unsigned long long)dback->root,
					(unsigned long long)dback->owner,
					(unsigned long long)dback->offset,
					dback->found_ref, dback->num_refs,
					back);
			}
			if (dback->disk_bytenr != rec->start) {
				err = 1;
				if (!print_errs)
					goto out;
				fprintf(stderr,
"backref disk bytenr does not match extent record, bytenr=%llu, ref bytenr=%llu\n",
					(unsigned long long)rec->start,
					(unsigned long long)dback->disk_bytenr);
			}

			if (dback->bytes != rec->nr) {
				err = 1;
				if (!print_errs)
					goto out;
				fprintf(stderr,
"backref bytes do not match extent backref, bytenr=%llu, ref bytes=%llu, backref bytes=%llu\n",
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
		fprintf(stderr,
	"incorrect global backref count on %llu found %llu wanted %llu\n",
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
	u64 super_gen = btrfs_super_generation(gfs_info->super_copy);

	if (rec->content_checked && rec->owner_ref_checked &&
	    rec->extent_item_refs == rec->refs && rec->refs > 0 &&
	    rec->num_duplicates == 0 && !all_backpointers_checked(rec, 0) &&
	    !rec->bad_full_backref && !rec->crossing_stripes &&
	    rec->generation <= super_gen + 1 &&
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
	/*
	 * Some unexpected root item referring to this one, return 1 to
	 * indicate owner not found
	 */
	if (rec->is_root)
		return 1;

	/* try to find the block by search corresponding fs tree */
	key.objectid = btrfs_header_owner(buf);
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	ref_root = btrfs_read_fs_root(gfs_info, &key);
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


static int record_bad_block_io(struct cache_tree *extent_cache,
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
	return btrfs_add_corrupt_extent_record(gfs_info, &key, start, len, 0);
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
		    BTRFS_LEAF_DATA_SIZE(gfs_info)) {
			if (btrfs_item_end_nr(buf, i) >
			    BTRFS_LEAF_DATA_SIZE(gfs_info)) {
				ret = delete_bogus_item(root, path, buf, i);
				if (!ret)
					goto again;
				fprintf(stderr,
				"item is off the end of the leaf, can't fix\n");
				ret = -EIO;
				break;
			}
			shift = BTRFS_LEAF_DATA_SIZE(gfs_info) -
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

	ret = btrfs_find_all_roots(NULL, gfs_info, buf->start, 0, &roots);
	if (ret)
		return -EIO;

	btrfs_init_path(&path);
	ULIST_ITER_INIT(&iter);
	/*
	 * If we found no roots referencing to this tree block, there is no
	 * chance to fix. So our default ret is -EIO.
	 */
	ret = -EIO;
	while ((node = ulist_next(roots, &iter))) {
		root_key.objectid = node->val;
		root_key.type = BTRFS_ROOT_ITEM_KEY;
		root_key.offset = (u64)-1;

		search_root = btrfs_read_fs_root(gfs_info, &root_key);
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
	if (rec->generation < btrfs_header_generation(buf))
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
		status = btrfs_check_leaf(gfs_info, &rec->parent_key, buf);
	else
		status = btrfs_check_node(gfs_info, &rec->parent_key, buf);

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

static struct data_backref *alloc_data_backref(struct extent_record *rec,
						u64 parent, u64 root,
						u64 owner, u64 offset,
						u64 max_size)
{
	struct data_backref *ref = malloc(sizeof(*ref));

	if (!ref)
		return NULL;
	memset(ref, 0, sizeof(*ref));
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
	struct btrfs_block_group *bg_cache;

	bg_cache = btrfs_lookup_first_block_group(gfs_info, rec->start);
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
	rec->generation = tmpl->generation;
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
		rec->crossing_stripes = check_crossing_stripes(gfs_info,
				rec->start, gfs_info->nodesize);
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
				fprintf(stderr,
			"block %llu rec extent_item_refs %llu, passed %llu\n",
					(unsigned long long)tmpl->start,
					(unsigned long long)
							rec->extent_item_refs,
					(unsigned long long)
							tmpl->extent_item_refs);
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
					gfs_info, rec->start,
					gfs_info->nodesize);
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
			fprintf(stderr,
	"Extent back ref already exists for %llu parent %llu root %llu\n",
				(unsigned long long)bytenr,
				(unsigned long long)parent,
				(unsigned long long)root);
		}
		back->node.found_ref = 1;
	} else {
		if (back->node.found_extent_tree) {
			fprintf(stderr,
	"extent back ref already exists for %llu parent %llu root %llu\n",
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
			    u32 num_refs, u64 gen, int found_ref, u64 max_size)
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
		tmpl.generation = gen;

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

	if (rec->generation < gen)
		rec->generation = gen;
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
			fprintf(stderr,
"Extent back ref already exists for %llu parent %llu root %llu owner %llu offset %llu num_refs %lu\n",
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
	ret = add_cache_extent(pending, bytenr, size);
	if (ret) {
		struct cache_extent *entry;

		entry = lookup_cache_extent(seen, bytenr, size);
		if (entry && entry->start == bytenr && entry->size == size) {
			remove_cache_extent(seen, entry);
			free(entry);
		}
		return ret;
	}
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
		while (next) {
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
	ret = btrfs_check_chunk_valid(gfs_info, eb, chunk, slot,
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
	rec->flags = btrfs_block_group_flags(leaf, ptr);

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
	rec->chunk_objectid =
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
	u64 gen;
	int metadata = 0;

	btrfs_item_key_to_cpu(eb, &key, slot);

	if (key.type == BTRFS_METADATA_ITEM_KEY) {
		metadata = 1;
		num_bytes = gfs_info->nodesize;
	} else {
		num_bytes = key.offset;
	}

	if (!IS_ALIGNED(key.objectid, gfs_info->sectorsize)) {
		error("ignoring invalid extent, bytenr %llu is not aligned to %u",
		      key.objectid, gfs_info->sectorsize);
		return -EIO;
	}
	if (item_size < sizeof(*ei)) {
		error(
"corrupted or unsupported extent item found, item size=%u expect minimal size=%zu",
		      item_size, sizeof(*ei));
		return -EIO;
	}

	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
	refs = btrfs_extent_refs(eb, ei);
	gen = btrfs_extent_generation(eb, ei);
	if (btrfs_extent_flags(eb, ei) & BTRFS_EXTENT_FLAG_TREE_BLOCK)
		metadata = 1;
	else
		metadata = 0;
	if (metadata && num_bytes != gfs_info->nodesize) {
		error("ignore invalid metadata extent, length %llu does not equal to %u",
		      num_bytes, gfs_info->nodesize);
		return -EIO;
	}
	if (!metadata && !IS_ALIGNED(num_bytes, gfs_info->sectorsize)) {
		error("ignore invalid data extent, length %llu is not aligned to %u",
		      num_bytes, gfs_info->sectorsize);
		return -EIO;
	}
	if (metadata)
		btrfs_check_subpage_eb_alignment(key.objectid, num_bytes);

	memset(&tmpl, 0, sizeof(tmpl));
	tmpl.start = key.objectid;
	tmpl.nr = num_bytes;
	tmpl.extent_item_refs = refs;
	tmpl.metadata = metadata;
	tmpl.found_rec = 1;
	tmpl.max_size = num_bytes;
	tmpl.generation = gen;
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
			if (ret < 0) {
				errno = -ret;
				error(
			"add_tree_backref failed (extent items tree block): %m");
			}
			break;
		case BTRFS_SHARED_BLOCK_REF_KEY:
			ret = add_tree_backref(extent_cache, key.objectid,
					offset, 0, 0);
			if (ret < 0) {
				errno = -ret;
				error(
		"add_tree_backref failed (extent items shared block): %m");
			}
			break;
		case BTRFS_EXTENT_DATA_REF_KEY:
			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			add_data_backref(extent_cache, key.objectid, 0,
					btrfs_extent_data_ref_root(eb, dref),
					btrfs_extent_data_ref_objectid(eb,
								       dref),
					btrfs_extent_data_ref_offset(eb, dref),
					btrfs_extent_data_ref_count(eb, dref),
					gen, 0, num_bytes);
			break;
		case BTRFS_SHARED_DATA_REF_KEY:
			sref = (struct btrfs_shared_data_ref *)(iref + 1);
			add_data_backref(extent_cache, key.objectid, offset,
					0, 0, 0,
					btrfs_shared_data_ref_count(eb, sref),
					gen, 0, num_bytes);
			break;
		default:
			fprintf(stderr,
				"corrupt extent record: key [%llu,%u,%llu]\n",
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
			     struct btrfs_block_group *cache,
			     u64 offset, u64 bytes)
{
	struct btrfs_free_space *entry;
	u64 *logical;
	u64 bytenr;
	int stripe_len;
	int i, nr, ret;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = btrfs_rmap_block(gfs_info,
				       cache->start, bytenr,
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
		fprintf(stderr, "there is no free space entry for %llu-%llu\n",
			offset, offset+bytes);
		return -EINVAL;
	}

	if (entry->offset != offset) {
		fprintf(stderr, "wanted offset %llu, found %llu\n", offset,
			entry->offset);
		return -EINVAL;
	}

	if (entry->bytes != bytes) {
		fprintf(stderr, "wanted bytes %llu, found %llu for off %llu\n",
			bytes, entry->bytes, offset);
		return -EINVAL;
	}

	unlink_free_space(cache->free_space_ctl, entry);
	free(entry);
	return 0;
}

static int verify_space_cache(struct btrfs_root *root,
			      struct btrfs_block_group *cache)
{
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 last;
	int ret = 0;

	root = gfs_info->extent_root;

	last = max_t(u64, cache->start, BTRFS_SUPER_INFO_OFFSET);

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
		if (key.objectid >= cache->start + cache->length)
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
				last = key.objectid + gfs_info->nodesize;
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
			last = key.objectid + gfs_info->nodesize;
		path.slots[0]++;
	}

	if (last < cache->start + cache->length)
		ret = check_cache_range(root, cache, last,
					cache->start + cache->length - last);

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
	struct btrfs_block_group *cache;
	u64 start = BTRFS_SUPER_INFO_OFFSET + BTRFS_SUPER_INFO_SIZE;
	int ret;
	int error = 0;

	while (1) {
		ctx.item_count++;
		cache = btrfs_lookup_first_block_group(gfs_info, start);
		if (!cache)
			break;

		start = cache->start + cache->length;
		if (!cache->free_space_ctl) {
			if (btrfs_init_free_space_ctl(cache,
						gfs_info->sectorsize)) {
				ret = -ENOMEM;
				break;
			}
		} else {
			btrfs_remove_free_space_cache(cache);
		}

		if (btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE)) {
			ret = exclude_super_stripes(gfs_info, cache);
			if (ret) {
				errno = -ret;
				fprintf(stderr,
					"could not exclude super stripes: %m\n");
				error++;
				continue;
			}
			ret = load_free_space_tree(gfs_info, cache);
			free_excluded_extents(gfs_info, cache);
			if (ret < 0) {
				errno = -ret;
				fprintf(stderr,
					"could not load free space tree: %m\n");
				error++;
				continue;
			}
			error += ret;
		} else {
			ret = load_free_space_cache(gfs_info, cache);
			if (ret < 0)
				error++;
			if (ret <= 0)
				continue;
		}

		ret = verify_space_cache(root, cache);
		if (ret) {
			fprintf(stderr, "cache appears valid but isn't %llu\n",
				cache->start);
			error++;
		}
	}

	return error ? -EINVAL : 0;
}

/*
 * Check data checksum for [@bytenr, @bytenr + @num_bytes).
 *
 * Return <0 for fatal error (fails to read checksum/data or allocate memory).
 * Return >0 for csum mismatch for any copy.
 * Return 0 if everything is OK.
 */
static int check_extent_csums(struct btrfs_root *root, u64 bytenr,
			u64 num_bytes, unsigned long leaf_offset,
			struct extent_buffer *eb)
{
	u64 offset = 0;
	u16 csum_size = btrfs_super_csum_size(gfs_info->super_copy);
	u16 csum_type = btrfs_super_csum_type(gfs_info->super_copy);
	u8 *data;
	unsigned long csum_offset;
	u8 result[BTRFS_CSUM_SIZE];
	u8 csum_expected[BTRFS_CSUM_SIZE];
	u64 read_len;
	u64 data_checked = 0;
	u64 tmp;
	int ret = 0;
	int mirror;
	int num_copies;
	bool csum_mismatch = false;

	if (num_bytes % gfs_info->sectorsize)
		return -EINVAL;

	data = malloc(num_bytes);
	if (!data)
		return -ENOMEM;

	num_copies = btrfs_num_copies(gfs_info, bytenr, num_bytes);
	while (offset < num_bytes) {
		/*
		 * Mirror 0 means 'read from any valid copy', so it's skipped.
		 * The indexes 1-N represent the n-th copy for levels with
		 * redundancy.
		 */
		for (mirror = 1; mirror <= num_copies; mirror++) {
			read_len = num_bytes - offset;
			/* read as much space once a time */
			ret = read_extent_data(gfs_info, (char *)data + offset,
					bytenr + offset, &read_len, mirror);
			if (ret)
				goto out;

			data_checked = 0;
			/* verify every 4k data's checksum */
			while (data_checked < read_len) {
				tmp = offset + data_checked;

				btrfs_csum_data(gfs_info, csum_type, data + tmp,
						result, gfs_info->sectorsize);

				csum_offset = leaf_offset +
					 tmp / gfs_info->sectorsize * csum_size;
				read_extent_buffer(eb, (char *)&csum_expected,
						   csum_offset, csum_size);
				if (memcmp(result, csum_expected, csum_size) != 0) {
					csum_mismatch = true;
					/* FIXME: format of the checksum value */
					fprintf(stderr,
			"mirror %d bytenr %llu csum %u expected csum %u\n",
						mirror, bytenr + tmp,
						result[0], csum_expected[0]);
				}
				data_checked += gfs_info->sectorsize;
			}
		}
		offset += read_len;
	}
out:
	free(data);
	if (!ret && csum_mismatch)
		ret = 1;
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
	ret = btrfs_search_slot(NULL, gfs_info->extent_root, &key, &path,
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
		fprintf(stderr,
			"there are no extents for csum range %llu-%llu\n",
			bytenr, bytenr+num_bytes);
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
	u64 last_data_end = 0;
	u64 offset = 0, num_bytes = 0;
	u16 csum_size = btrfs_super_csum_size(gfs_info->super_copy);
	int errors = 0;
	int ret;
	u64 data_len;
	unsigned long leaf_offset;
	bool verify_csum = !!check_data_csum;

	root = gfs_info->csum_root;
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

	/*
	 * For metadata dump (btrfs-image) all data is wiped so verifying data
	 * csum is meaningless and will always report csum error.
	 */
	if (check_data_csum && (btrfs_super_flags(gfs_info->super_copy) &
	    (BTRFS_SUPER_FLAG_METADUMP | BTRFS_SUPER_FLAG_METADUMP_V2))) {
		printf("skip data csum verification for metadata dump\n");
		verify_csum = false;
	}

	while (1) {
		ctx.item_count++;
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

		if (key.offset < last_data_end) {
			error(
	"csum overlap, current bytenr=%llu prev_end=%llu, eb=%llu slot=%u",
				key.offset, last_data_end, leaf->start,
				path.slots[0]);
			errors++;
		}
		data_len = (btrfs_item_size_nr(leaf, path.slots[0]) /
			      csum_size) * gfs_info->sectorsize;
		if (!verify_csum)
			goto skip_csum_check;
		leaf_offset = btrfs_item_ptr_offset(leaf, path.slots[0]);
		ret = check_extent_csums(root, key.offset, data_len,
					 leaf_offset, leaf);
		/*
		 * Only break for fatal errors, if mismatch is found, continue
		 * checking until all extents are checked.
		 */
		if (ret < 0)
			break;
		if (ret > 0)
			errors++;
skip_csum_check:
		if (!num_bytes) {
			offset = key.offset;
		} else if (key.offset != offset + num_bytes) {
			ret = check_extent_exists(root, offset, num_bytes);
			if (ret) {
				fprintf(stderr,
		"csum exists for %llu-%llu but there is no extent record\n",
					offset, offset+num_bytes);
				errors++;
			}
			offset = key.offset;
			num_bytes = 0;
		}
		num_bytes += data_len;
		last_data_end = key.offset + data_len;
		path.slots[0]++;
	}

	btrfs_release_path(&path);
	return errors;
}

static int is_dropped_key(struct btrfs_key *key,
			  struct btrfs_key *drop_key)
{
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
		for (i = 0; i < nritems; i++) {
			ret = add_cache_extent(reada, bits[i].start,
					       bits[i].size);
			if (ret == -EEXIST)
				continue;

			/* fixme, get the parent transid */
			readahead_tree_block(gfs_info, bits[i].start, 0);
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
	buf = read_tree_block(gfs_info, bytenr, gen);
	if (!extent_buffer_uptodate(buf)) {
		record_bad_block_io(extent_cache, bytenr, size);
		goto out;
	}

	nritems = btrfs_header_nritems(buf);

	flags = 0;
	if (!init_extent_tree) {
		ret = btrfs_lookup_extent_info(NULL, gfs_info, bytenr,
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
		btree_space_waste += btrfs_leaf_free_space(buf);
		for (i = 0; i < nritems; i++) {
			struct btrfs_file_extent_item *fi;
			unsigned long inline_offset;

			inline_offset = offsetof(struct btrfs_file_extent_item,
						 disk_bytenr);
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

			/* Skip deprecated extent ref */
			if (key.type == BTRFS_EXTENT_REF_V0_KEY)
				continue;

			if (key.type == BTRFS_TREE_BLOCK_REF_KEY) {
				ret = add_tree_backref(extent_cache,
						key.objectid, 0, key.offset, 0);
				if (ret < 0) {
					errno = -ret;
					error(
				"add_tree_backref failed (leaf tree block): %m");
				}
				continue;
			}
			if (key.type == BTRFS_SHARED_BLOCK_REF_KEY) {
				ret = add_tree_backref(extent_cache,
						key.objectid, key.offset, 0, 0);
				if (ret < 0) {
					errno = -ret;
					error(
			"add_tree_backref failed (leaf shared block): %m");
				}
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
					0, 0, gfs_info->sectorsize);
				continue;
			}
			if (key.type == BTRFS_SHARED_DATA_REF_KEY) {
				struct btrfs_shared_data_ref *ref;

				ref = btrfs_item_ptr(buf, i,
						struct btrfs_shared_data_ref);
				add_data_backref(extent_cache,
					key.objectid, key.offset, 0, 0, 0,
					btrfs_shared_data_ref_count(buf, ref),
					0, 0, gfs_info->sectorsize);
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
			/* Check itemsize before we continue */
			if (btrfs_item_size_nr(buf, i) < inline_offset) {
				ret = -EUCLEAN;
				error(
		"invalid file extent item size, have %u expect (%lu, %lu]",
					btrfs_item_size_nr(buf, i),
					inline_offset,
					BTRFS_LEAF_DATA_SIZE(gfs_info));
				continue;
			}
			fi = btrfs_item_ptr(buf, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(buf, fi) ==
			    BTRFS_FILE_EXTENT_INLINE)
				continue;

			/* Prealloc/regular extent must have fixed item size */
			if (btrfs_item_size_nr(buf, i) !=
			    sizeof(struct btrfs_file_extent_item)) {
				ret = -EUCLEAN;
				error(
			"invalid file extent item size, have %u expect %zu",
					btrfs_item_size_nr(buf, i),
					sizeof(struct btrfs_file_extent_item));
				continue;
			}
			/* key.offset (file offset) must be aligned */
			if (!IS_ALIGNED(key.offset, gfs_info->sectorsize)) {
				ret = -EUCLEAN;
				error(
			"invalid file offset, have %llu expect aligned to %u",
					key.offset, gfs_info->sectorsize);
				continue;
			}
			if (btrfs_file_extent_disk_bytenr(buf, fi) == 0)
				continue;

			data_bytes_allocated +=
				btrfs_file_extent_disk_num_bytes(buf, fi);

			data_bytes_referenced +=
				btrfs_file_extent_num_bytes(buf, fi);
			add_data_backref(extent_cache,
				btrfs_file_extent_disk_bytenr(buf, fi),
				parent, owner, key.objectid, key.offset -
				btrfs_file_extent_offset(buf, fi), 1,
				btrfs_file_extent_generation(buf, fi), 1,
				btrfs_file_extent_disk_num_bytes(buf, fi));
		}
	} else {
		int level;

		level = btrfs_header_level(buf);
		i = 0;

		/*
		 * If we have a drop key we need to not walk down any slots we
		 * would have ignored when mounting the fs.  These blocks are
		 * technically unreferenced and don't need to be worried about.
		 */
		if (ri != NULL && ri->drop_level && level > ri->drop_level) {
			ret = btrfs_bin_search(buf, &ri->drop_key, &i);
			if (ret && i > 0)
				i--;
		}

		for (; i < nritems; i++) {
			struct extent_record tmpl;

			ptr = btrfs_node_blockptr(buf, i);
			size = gfs_info->nodesize;
			btrfs_node_key_to_cpu(buf, &key, i);
			if (ri != NULL) {
				if ((level == ri->drop_level) &&
				    is_dropped_key(&key, &ri->drop_key)) {
					continue;
				}
			}

			memset(&tmpl, 0, sizeof(tmpl));
			btrfs_cpu_key_to_disk(&tmpl.parent_key, &key);
			tmpl.parent_generation =
				btrfs_node_ptr_generation(buf, i);
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
				errno = -ret;
				error(
				"add_tree_backref failed (non-leaf block): %m");
				continue;
			}

			if (level > 1)
				add_pending(nodes, seen, ptr, size);
			else
				add_pending(pending, seen, ptr, size);
		}
		btree_space_waste += (BTRFS_NODEPTRS_PER_BLOCK(gfs_info) -
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
static int free_extent_hook(u64 bytenr, u64 num_bytes, u64 parent,
			    u64 root_objectid, u64 owner, u64 offset,
			    int refs_to_drop)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int is_data;
	struct cache_tree *extent_cache = gfs_info->fsck_extent_cache;

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

	while (1) {
		ret = btrfs_search_slot(trans, gfs_info->extent_root, &key,
					path, 0, 1);
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

		fprintf(stderr,
			"repair deleting extent record: key [%llu,%u,%llu]\n",
			found_key.objectid, found_key.type, found_key.offset);

		ret = btrfs_del_item(trans, gfs_info->extent_root, path);
		if (ret)
			break;
		btrfs_release_path(path);

		if (found_key.type == BTRFS_EXTENT_ITEM_KEY ||
		    found_key.type == BTRFS_METADATA_ITEM_KEY) {
			u64 bytes = (found_key.type == BTRFS_EXTENT_ITEM_KEY) ?
				found_key.offset : gfs_info->nodesize;

			ret = btrfs_update_block_group(trans, bytenr, bytes,
						       0, 0);
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
			 struct btrfs_path *path,
			 struct extent_record *rec,
			 struct extent_backref *back,
			 int allocated, u64 flags)
{
	int ret = 0;
	struct btrfs_root *extent_root = gfs_info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_key ins_key;
	struct btrfs_extent_item *ei;
	struct data_backref *dback;
	struct btrfs_tree_block_info *bi;

	if (!back->is_data)
		rec->max_size = max_t(u64, rec->max_size, gfs_info->nodesize);

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
		if (rec->generation)
			btrfs_set_extent_generation(leaf, ei, rec->generation);
		else
			btrfs_set_extent_generation(leaf, ei, trans->transid);

		if (back->is_data) {
			btrfs_set_extent_flags(leaf, ei,
					       BTRFS_EXTENT_FLAG_DATA);
		} else {
			struct btrfs_disk_key copy_key;

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
					flags | BTRFS_EXTENT_FLAG_TREE_BLOCK);
		}

		btrfs_mark_buffer_dirty(leaf);
		ret = btrfs_update_block_group(trans, rec->start,
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
			ret = btrfs_inc_extent_ref(trans, gfs_info->extent_root,
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
		fprintf(stderr,
"adding new data backref on %llu %s %llu owner %llu offset %llu found %d\n",
			(unsigned long long)rec->start,
			back->full_backref ? "parent" : "root",
			back->full_backref ? (unsigned long long)parent :
					     (unsigned long long)dback->root,
			(unsigned long long)dback->owner,
			(unsigned long long)dback->offset, dback->found_ref);
	} else {
		u64 parent;
		struct tree_backref *tback;

		tback = to_tree_backref(back);
		if (back->full_backref)
			parent = tback->parent;
		else
			parent = 0;

		ret = btrfs_inc_extent_ref(trans, gfs_info->extent_root,
					   rec->start, rec->max_size,
					   parent, tback->root, 0, 0);
		fprintf(stderr,
"adding new tree backref on start %llu len %llu parent %llu root %llu\n",
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

static int repair_ref(struct btrfs_path *path, struct data_backref *dback,
		      struct extent_entry *entry)
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
	root = btrfs_read_fs_root(gfs_info, &key);
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
		fprintf(stderr, "error cowing down to ref [%llu,%u,%llu]: %d\n",
			key.objectid, key.type, key.offset, ret);
		goto out;
	}
	if (ret > 0) {
		fprintf(stderr,
		"well that's odd, we just found this key [%llu,%u,%llu]\n",
			key.objectid, key.type, key.offset);
		ret = -EINVAL;
		goto out;
	}
	leaf = path->nodes[0];
	fi = btrfs_item_ptr(leaf, path->slots[0],
			    struct btrfs_file_extent_item);

	if (btrfs_file_extent_compression(leaf, fi) &&
	    dback->disk_bytenr != entry->bytenr) {
		fprintf(stderr,
"ref doesn't match the record start and is compressed, please take a btrfs-image of this file system and send it to a btrfs developer so they can complete this functionality for bytenr %llu\n",
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
			fprintf(stderr,
"ref is past the entry end, please take a btrfs-image of this file system and send it to a btrfs developer, ref %llu\n",
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
			fprintf(stderr,
"ref is before the entry start, please take a btrfs-image of this file system and send it to a btrfs developer, ref %llu\n",
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

static int verify_backrefs(struct btrfs_path *path, struct extent_record *rec)
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

	fprintf(stderr,
		"attempting to repair backref discrepancy for bytenr %llu\n",
		rec->start);

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
			fprintf(stderr,
"backrefs don't agree with each other and extent record doesn't agree with anybody, so we can't fix bytenr %llu bytes %llu\n",
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
			fprintf(stderr,
"backrefs and extent record evenly split on who is right, this is going to require user input to fix bytenr %llu bytes %llu\n",
				rec->start, rec->nr);
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
		fprintf(stderr,
"extent start and backref starts don't match, please use btrfs-image on this file system and send it to a btrfs developer so they can make fsck fix this particular case.  bytenr is %llu, bytes is %llu\n",
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

		ret = repair_ref(path, dback, best);
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
			fprintf(stderr,
"Ok we have overlapping extents that aren't completely covered by each other, this is going to require more careful thought. The extents are [%llu-%llu] and [%llu-%llu]\n",
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

	root = gfs_info->extent_root;
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
			fprintf(stderr,
"well this shouldn't happen, extent record overlaps but is metadata? [%llu, %llu]\n",
				tmp->start, tmp->nr);
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

/*
 * Based extent backref item, we find all file extent items in the fs tree. By
 * the info we can rebuild the extent backref item
 */
static int __find_possible_backrefs(struct btrfs_root *root,
		u64 owner, u64 offset, u64 bytenr, u64 *refs_ret,
		u64 *bytes_ret)
{
	int ret = 0;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_file_extent_item *fi;
	struct extent_buffer *leaf;
	u64 backref_offset, disk_bytenr;
	int slot;

	btrfs_init_path(&path);

	key.objectid = owner;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret > 0)
		ret = -ENOENT;
	if (ret) {
		btrfs_release_path(&path);
		return ret;
	}

	btrfs_release_path(&path);

	key.objectid = owner;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}

	while (1) {
		leaf = path.nodes[0];
		slot = path.slots[0];

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret) {
				if (ret > 0)
					ret = 0;
				break;
			}

			leaf = path.nodes[0];
			slot = path.slots[0];
		}

		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		if ((found_key.objectid != owner) ||
			(found_key.type != BTRFS_EXTENT_DATA_KEY))
			break;

		fi = btrfs_item_ptr(leaf, slot,
				struct btrfs_file_extent_item);

		backref_offset = found_key.offset -
			btrfs_file_extent_offset(leaf, fi);
		disk_bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
		*bytes_ret = btrfs_file_extent_disk_num_bytes(leaf,
								fi);
		if ((disk_bytenr == bytenr) &&
			(backref_offset == offset)) {
			(*refs_ret)++;
		}
		path.slots[0]++;
	}

	btrfs_release_path(&path);
	return ret;
}

static int find_possible_backrefs(struct btrfs_path *path,
				  struct cache_tree *extent_cache,
				  struct extent_record *rec)
{
	struct btrfs_root *root;
	struct extent_backref *back, *tmp;
	struct data_backref *dback;
	struct cache_extent *cache;
	struct btrfs_key key;
	u64 bytenr, bytes;
	u64 refs;
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

		root = btrfs_read_fs_root(gfs_info, &key);

		/* No root, definitely a bad ref, skip */
		if (IS_ERR(root) && PTR_ERR(root) == -ENOENT)
			continue;
		/* Other err, exit */
		if (IS_ERR(root))
			return PTR_ERR(root);

		refs = 0;
		bytes = 0;
		ret = __find_possible_backrefs(root, dback->owner,
				dback->offset, rec->start, &refs, &bytes);
		if (ret)
			continue;

		bytenr = rec->start;

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

		dback->found_ref += refs;
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
 * when an incorrect extent item is found, this will delete
 * all of the existing entries for it and recreate them
 * based on what the tree scan found.
 */
static int fixup_extent_refs(struct cache_tree *extent_cache,
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
		ret = find_possible_backrefs(&path, extent_cache, rec);
		if (ret < 0)
			goto out;
	}

	/* step one, make sure all of the backrefs agree */
	ret = verify_backrefs(&path, rec);
	if (ret < 0)
		goto out;

	trans = btrfs_start_transaction(gfs_info->extent_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}

	/* step two, delete all the existing records */
	ret = delete_extent_records(trans, &path, rec->start);

	if (ret < 0)
		goto out;

	/* was this block corrupt?  If so, don't add references to it */
	cache = lookup_cache_extent(gfs_info->corrupt_blocks,
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
		ret = record_extent(trans, &path, rec, back, allocated, flags);
		allocated = 1;

		if (ret)
			goto out;
	}
out:
	if (trans) {
		int err = btrfs_commit_transaction(trans, gfs_info->extent_root);

		if (!ret)
			ret = err;
	}

	if (!ret)
		fprintf(stderr, "Repaired extent references for %llu\n",
				(unsigned long long)rec->start);

	btrfs_release_path(&path);
	return ret;
}

static int fixup_extent_flags(struct extent_record *rec)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root = gfs_info->extent_root;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;
	u64 flags;
	int ret = 0;
	bool metadata_item = rec->metadata;

	if (!btrfs_fs_incompat(gfs_info, SKINNY_METADATA))
		metadata_item = false;
retry:
	key.objectid = rec->start;
	if (metadata_item) {
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
		if (key.type == BTRFS_METADATA_ITEM_KEY) {
			metadata_item = false;
			goto retry;
		}
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

	ret = btrfs_search_slot(trans, gfs_info->extent_root,
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
	 * We couldn't find the bad block.
	 * TODO: search all the nodes for pointers to this block
	 */
	if (eb == gfs_info->extent_root->node) {
		ret = -ENOENT;
		goto out;
	} else {
		level++;
		btrfs_release_path(&path);
		goto again;
	}

del_ptr:
	printk("deleting pointer to block %llu\n", corrupt->cache.start);
	ret = btrfs_del_ptr(gfs_info->extent_root, &path, level, slot);

out:
	btrfs_release_path(&path);
	return ret;
}

static int prune_corrupt_blocks(void)
{
	struct btrfs_trans_handle *trans = NULL;
	struct cache_extent *cache;
	struct btrfs_corrupt_block *corrupt;

	while (1) {
		cache = search_cache_extent(gfs_info->corrupt_blocks, 0);
		if (!cache)
			break;
		if (!trans) {
			trans = btrfs_start_transaction(gfs_info->extent_root, 1);
			if (IS_ERR(trans))
				return PTR_ERR(trans);
		}
		corrupt = container_of(cache, struct btrfs_corrupt_block, cache);
		prune_one_block(trans, corrupt);
		remove_cache_extent(gfs_info->corrupt_blocks, cache);
	}
	if (trans)
		return btrfs_commit_transaction(trans, gfs_info->extent_root);
	return 0;
}

static int record_unaligned_extent_rec(struct extent_record *rec)
{

	struct extent_backref *back, *tmp;
	struct data_backref *dback;
	struct btrfs_root *dest_root;
	struct btrfs_key key;
	struct unaligned_extent_rec_t *urec;
	LIST_HEAD(entries);
	int ret = 0;

	fprintf(stderr, "record unaligned extent record on %llu %llu\n",
			rec->start, rec->nr);

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

		key.objectid = dback->root;
		key.type = BTRFS_ROOT_ITEM_KEY;
		key.offset = (u64)-1;

		dest_root = btrfs_read_fs_root(gfs_info, &key);

		/* For non-exist root we just skip it */
		if (IS_ERR_OR_NULL(dest_root))
			continue;

		urec = malloc(sizeof(struct unaligned_extent_rec_t));
		if (!urec)
			return -ENOMEM;

		INIT_LIST_HEAD(&urec->list);
		urec->objectid = dest_root->objectid;
		urec->owner = dback->owner;
		urec->offset = 0;
		urec->bytenr = rec->start;
		ret = find_file_extent_offset_by_bytenr(dest_root,
				dback->owner, rec->start, &urec->offset);
		if (ret) {
			free(urec);
			return ret;
		}
		list_add(&urec->list, &dest_root->unaligned_extent_recs);
	}

	return ret;
}

static int repair_extent_item_generation(struct extent_record *rec)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_extent_item *ei;
	struct btrfs_root *extent_root = gfs_info->extent_root;
	u64 new_gen = 0;;
	int ret;

	key.objectid = rec->start;
	key.type = BTRFS_METADATA_ITEM_KEY;
	key.offset = (u64)-1;

	get_extent_item_generation(rec->start, &new_gen);
	trans = btrfs_start_transaction(extent_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error("failed to start transaction: %m");
		return ret;
	}
	btrfs_init_path(&path);
	ret = btrfs_search_slot(trans, extent_root, &key, &path, 0, 1);
	/* Not possible */
	if (ret == 0)
		ret = -EUCLEAN;
	if (ret < 0)
		goto out;
	ret = btrfs_previous_extent_item(extent_root, &path, rec->start);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto out;

	if (!new_gen)
		new_gen = trans->transid;
	ei = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_extent_item);
	btrfs_set_extent_generation(path.nodes[0], ei, new_gen);
	ret = btrfs_commit_transaction(trans, extent_root);
	if (ret < 0) {
		errno = -ret;
		error("failed to commit transaction: %m");
		goto out;
	}
	printf("Reset extent item (%llu) generation to %llu\n",
		key.objectid, new_gen);
	rec->generation = new_gen;
out:
	btrfs_release_path(&path);
	if (ret < 0)
		btrfs_abort_transaction(trans, ret);
	return ret;
}

static int check_extent_refs(struct btrfs_root *root,
			     struct cache_tree *extent_cache)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	u64 super_gen;
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
		while (cache) {
			rec = container_of(cache, struct extent_record, cache);
			set_extent_dirty(gfs_info->excluded_extents,
					 rec->start,
					 rec->start + rec->max_size - 1);
			cache = next_cache_extent(cache);
		}

		/* pin down all the corrupted blocks too */
		cache = search_cache_extent(gfs_info->corrupt_blocks, 0);
		while (cache) {
			set_extent_dirty(gfs_info->excluded_extents,
					 cache->start,
					 cache->start + cache->size - 1);
			cache = next_cache_extent(cache);
		}
		prune_corrupt_blocks();
		reset_cached_block_groups();
	}

	reset_cached_block_groups();

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

	super_gen = btrfs_super_generation(gfs_info->super_copy);
	while (1) {
		int cur_err = 0;
		int fix = 0;

		cache = search_cache_extent(extent_cache, 0);
		if (!cache)
			break;
		rec = container_of(cache, struct extent_record, cache);
		if (rec->num_duplicates) {
			fprintf(stderr,
				"extent item %llu has multiple extent items\n",
				(unsigned long long)rec->start);
			cur_err = 1;
		}

		if (rec->generation > super_gen + 1) {
			bool repaired = false;

			if (repair) {
				ret = repair_extent_item_generation(rec);
				if (ret == 0)
					repaired = true;
			}
			if (!repaired) {
				error(
	"invalid generation for extent %llu, have %llu expect (0, %llu]",
					rec->start, rec->generation,
					super_gen + 1);
				cur_err = 1;
			}
		}
		if (rec->refs != rec->extent_item_refs) {
			fprintf(stderr, "ref mismatch on [%llu %llu] ",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);
			fprintf(stderr, "extent item %llu, found %llu\n",
				(unsigned long long)rec->extent_item_refs,
				(unsigned long long)rec->refs);
			fix = 1;
			cur_err = 1;
		}

		if (!IS_ALIGNED(rec->start, gfs_info->sectorsize)) {
			fprintf(stderr, "unaligned extent rec on [%llu %llu]\n",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);
			ret = record_unaligned_extent_rec(rec);
			if (ret)
				goto repair_abort;

			/* No need to check backref */
			goto next;
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
			ret = fixup_extent_refs(extent_cache, rec);
			if (ret)
				goto repair_abort;
		}


		if (rec->bad_full_backref) {
			fprintf(stderr, "bad full backref, on [%llu]\n",
				(unsigned long long)rec->start);
			if (repair) {
				ret = fixup_extent_flags(rec);
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
next:
		err = cur_err;
		remove_cache_extent(extent_cache, cache);
		free_all_extent_backrefs(rec);
		if (!init_extent_tree && repair && (!cur_err || fix))
			clear_extent_dirty(gfs_info->excluded_extents,
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

			root = gfs_info->extent_root;
			trans = btrfs_start_transaction(root, 1);
			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				goto repair_abort;
			}

			ret = btrfs_fix_block_accounting(trans);
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
"Chunk[%llu, %u, %llu] stripe[%llu, %llu] mismatch dev extent[%llu, %llu, %llu]\n",
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

	if (dev_rec->byte_used > dev_rec->total_byte) {
		error(
		"device %llu has incorrect used bytes %llu > total bytes %llu",
		      dev_rec->devid, dev_rec->byte_used, dev_rec->total_byte);
		return -EUCLEAN;
	}

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
		int ret = -1;

		fprintf(stderr,
			"Dev extent's total-byte(%llu) is not equal to byte-used(%llu) in dev[%llu, %u, %llu]\n",
			total_byte, dev_rec->byte_used,	dev_rec->objectid,
			dev_rec->type, dev_rec->offset);
		if (repair) {
			ret = repair_dev_item_bytes_used(gfs_info,
					dev_rec->devid, total_byte);
		}
		return ret;
	} else {
		return 0;
	}
}

/*
 * Unlike device size alignment check above, some super total_bytes check
 * failure can lead to mount failure for newer kernel.
 *
 * So this function will return the error for a fatal super total_bytes problem.
 */
static bool is_super_size_valid(void)
{
	struct btrfs_device *dev;
	struct list_head *dev_list = &gfs_info->fs_devices->devices;
	u64 total_bytes = 0;
	u64 super_bytes = btrfs_super_total_bytes(gfs_info->super_copy);

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
	if (btrfs_super_flags(gfs_info->super_copy) &
	    (BTRFS_SUPER_FLAG_METADUMP | BTRFS_SUPER_FLAG_METADUMP_V2))
		return true;
	if (!IS_ALIGNED(super_bytes, gfs_info->sectorsize) ||
	    !IS_ALIGNED(total_bytes, gfs_info->sectorsize) ||
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
					 gfs_info->sectorsize);
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
	u64 last = 0;

	while (!list_empty(list)) {
		struct root_item_record *rec;
		struct extent_buffer *buf;

		rec = list_entry(list->next,
				 struct root_item_record, list);
		last = 0;
		buf = read_tree_block(gfs_info, rec->bytenr, 0);
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
			ctx.item_count++;
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

/**
 * parse_tree_roots - Go over all roots in the tree root and add each one to
 *		      a list.
 *
 * @normal_trees   - list to contains all roots which don't have a drop
 *		     operation in progress
 *
 * @dropping_trees - list containing all roots which have a drop operation
 *		     pending
 *
 * Returns 0 on success or a negative value indicating an error.
 */
static int parse_tree_roots(struct list_head *normal_trees,
			    struct list_head *dropping_trees)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_root_item ri;
	struct extent_buffer *leaf;
	int slot;
	int ret = 0;

	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, gfs_info->tree_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	while (1) {
		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(gfs_info->tree_root, &path);
			if (ret != 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (found_key.type == BTRFS_ROOT_ITEM_KEY) {
			unsigned long offset;
			u64 last_snapshot;
			u8 level;

			offset = btrfs_item_ptr_offset(leaf, path.slots[0]);
			read_extent_buffer(leaf, &ri, offset, sizeof(ri));
			last_snapshot = btrfs_root_last_snapshot(&ri);
			level = btrfs_root_level(&ri);
			if (btrfs_disk_key_objectid(&ri.drop_progress) == 0) {
				ret = add_root_item_to_list(normal_trees,
						found_key.objectid,
						btrfs_root_bytenr(&ri),
						last_snapshot, level,
						0, NULL);
				if (ret < 0)
					break;
			} else {
				u64 objectid = found_key.objectid;

				btrfs_disk_key_to_cpu(&found_key,
						      &ri.drop_progress);
				ret = add_root_item_to_list(dropping_trees,
						objectid,
						btrfs_root_bytenr(&ri),
						last_snapshot, level,
						ri.drop_level, &found_key);
				if (ret < 0)
					break;
			}
		}
		path.slots[0]++;
	}

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Check if all dev extents are valid (not overlapping nor beyond device
 * boundary).
 *
 * Dev extents <-> chunk cross checking is already done in check_chunks().
 */
static int check_dev_extents(void)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root *dev_root = gfs_info->dev_root;
	int ret;
	u64 prev_devid = 0;
	u64 prev_dev_ext_end = 0;

	btrfs_init_path(&path);

	key.objectid = 1;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, dev_root, &key, &path, 0, 0);
	if (ret < 0) {
		errno = -ret;
		error("failed to search device tree: %m");
		goto out;
	}
	if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
		ret = btrfs_next_leaf(dev_root, &path);
		if (ret < 0) {
			errno = -ret;
			error("failed to find next leaf: %m");
			goto out;
		}
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}

	while (1) {
		struct btrfs_dev_extent *dev_ext;
		struct btrfs_device *dev;
		u64 devid;
		u64 physical_offset;
		u64 physical_len;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.type != BTRFS_DEV_EXTENT_KEY)
			break;
		dev_ext = btrfs_item_ptr(path.nodes[0], path.slots[0],
					 struct btrfs_dev_extent);
		devid = key.objectid;
		physical_offset = key.offset;
		physical_len = btrfs_dev_extent_length(path.nodes[0], dev_ext);

		dev = btrfs_find_device(gfs_info, devid, NULL, NULL);
		if (!dev) {
			error("failed to find device with devid %llu", devid);
			ret = -EUCLEAN;
			goto out;
		}
		if (prev_devid == devid && prev_dev_ext_end > physical_offset) {
			error(
"dev extent devid %llu physical offset %llu overlap with previous dev extent end %llu",
			      devid, physical_offset, prev_dev_ext_end);
			ret = -EUCLEAN;
			goto out;
		}
		if (physical_offset + physical_len > dev->total_bytes) {
			error(
"dev extent devid %llu physical offset %llu len %llu is beyond device boundary %llu",
			      devid, physical_offset, physical_len,
			      dev->total_bytes);
			ret = -EUCLEAN;
			goto out;
		}
		prev_devid = devid;
		prev_dev_ext_end = physical_offset + physical_len;

		ret = btrfs_next_item(dev_root, &path);
		if (ret < 0) {
			errno = -ret;
			error("failed to find next leaf: %m");
			goto out;
		}
		if (ret > 0) {
			ret = 0;
			break;
		}
	}
out:
	btrfs_release_path(&path);
	return ret;
}

static int check_chunks_and_extents(void)
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
	int ret, err = 0;
	struct block_info *bits;
	int bits_nr;
	struct list_head dropping_trees;
	struct list_head normal_trees;
	struct btrfs_root *root1;
	struct btrfs_root *root;
	u8 level;

	root = gfs_info->fs_root;
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
		gfs_info->excluded_extents = &excluded_extents;
		gfs_info->fsck_extent_cache = &extent_cache;
		gfs_info->free_extent_hook = free_extent_hook;
		gfs_info->corrupt_blocks = &corrupt_blocks;
	}

	bits_nr = 1024;
	bits = malloc(bits_nr * sizeof(struct block_info));
	if (!bits) {
		perror("malloc");
		exit(1);
	}

again:
	root1 = gfs_info->tree_root;
	level = btrfs_header_level(root1->node);
	ret = add_root_item_to_list(&normal_trees, root1->root_key.objectid,
				    root1->node->start, 0, level, 0, NULL);
	if (ret < 0)
		goto out;
	root1 = gfs_info->chunk_root;
	level = btrfs_header_level(root1->node);
	ret = add_root_item_to_list(&normal_trees, root1->root_key.objectid,
				    root1->node->start, 0, level, 0, NULL);
	if (ret < 0)
		goto out;

	ret = parse_tree_roots(&normal_trees, &dropping_trees);
	if (ret < 0)
		goto out;

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

	ret = check_dev_extents();
	if (ret < 0) {
		err = ret;
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
	if (repair) {
		free_corrupt_blocks_tree(gfs_info->corrupt_blocks);
		extent_io_tree_cleanup(&excluded_extents);
		gfs_info->fsck_extent_cache = NULL;
		gfs_info->free_extent_hook = NULL;
		gfs_info->corrupt_blocks = NULL;
		gfs_info->excluded_extents = NULL;
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
	free_corrupt_blocks_tree(gfs_info->corrupt_blocks);
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

static int do_check_chunks_and_extents(void)
{
	int ret;

	if (check_mode == CHECK_MODE_LOWMEM)
		ret = check_chunks_and_extents_lowmem();
	else
		ret = check_chunks_and_extents();

	/* Also repair device size related problems */
	if (repair && !ret) {
		ret = btrfs_fix_device_and_super_size(gfs_info);
		if (ret > 0)
			ret = 0;
	}
	return ret;
}

static int btrfs_fsck_reinit_root(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root)
{
	struct extent_buffer *c;
	struct extent_buffer *old = root->node;
	int level;
	int ret;
	struct btrfs_disk_key disk_key = {0,0,0};

	level = 0;

	c = btrfs_alloc_free_block(trans, root, gfs_info->nodesize,
				   root->root_key.objectid,
				   &disk_key, level, 0, 0);
	if (IS_ERR(c))
		return PTR_ERR(c);

	memset_extent_buffer(c, 0, 0, sizeof(struct btrfs_header));
	btrfs_set_header_level(c, level);
	btrfs_set_header_bytenr(c, c->start);
	btrfs_set_header_generation(c, trans->transid);
	btrfs_set_header_backref_rev(c, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(c, root->root_key.objectid);

	write_extent_buffer(c, gfs_info->fs_devices->metadata_uuid,
			    btrfs_header_fsid(), BTRFS_FSID_SIZE);

	write_extent_buffer(c, gfs_info->chunk_tree_uuid,
			    btrfs_header_chunk_tree_uuid(c),
			    BTRFS_UUID_SIZE);

	btrfs_mark_buffer_dirty(c);
	/*
	 * this case can happen in the following case:
	 *
	 * reinit reloc data root, this is because we skip pin
	 * down reloc data tree before which means we can allocate
	 * same block bytenr here.
	 */
	if (old->start == c->start) {
		btrfs_set_root_generation(&root->root_item,
					  trans->transid);
		root->root_item.level = btrfs_header_level(root->node);
		ret = btrfs_update_root(trans, gfs_info->tree_root,
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

static int reset_block_groups(void)
{
	struct btrfs_block_group *cache;
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
	ret = btrfs_search_slot(NULL, gfs_info->chunk_root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}

	/*
	 * We do this in case the block groups were screwed up and had alloc
	 * bits that aren't actually set on the chunks.  This happens with
	 * restored images every time and could happen in real life I guess.
	 */
	gfs_info->avail_data_alloc_bits = 0;
	gfs_info->avail_metadata_alloc_bits = 0;
	gfs_info->avail_system_alloc_bits = 0;

	/* First we need to create the in-memory block groups */
	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(gfs_info->chunk_root, &path);
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
		btrfs_add_block_group(gfs_info, 0,
				      btrfs_chunk_type(leaf, chunk), key.offset,
				      btrfs_chunk_length(leaf, chunk));
		set_extent_dirty(&gfs_info->free_space_cache, key.offset,
				 key.offset + btrfs_chunk_length(leaf, chunk));
		path.slots[0]++;
	}
	start = 0;
	while (1) {
		cache = btrfs_lookup_first_block_group(gfs_info, start);
		if (!cache)
			break;
		cache->cached = 1;
		start = cache->start + cache->length;
	}

	btrfs_release_path(&path);
	return 0;
}

static int reset_balance(struct btrfs_trans_handle *trans)
{
	struct btrfs_root *root = gfs_info->tree_root;
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
	root = btrfs_read_fs_root(gfs_info, &key);
	if (IS_ERR(root)) {
		fprintf(stderr, "Error reading data reloc tree\n");
		ret = PTR_ERR(root);
		goto out;
	}
	record_root_in_trans(trans, root);
	ret = btrfs_fsck_reinit_root(trans, root);
	if (ret)
		goto out;
	ret = btrfs_make_root_dir(trans, root, BTRFS_FIRST_FREE_OBJECTID);
out:
	btrfs_release_path(&path);
	return ret;
}

static int reinit_extent_tree(struct btrfs_trans_handle *trans, bool pin)
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
	if (btrfs_fs_incompat(gfs_info, MIXED_GROUPS)) {
		fprintf(stderr, "We don't support re-initing the extent tree "
			"for mixed block groups yet, please notify a btrfs "
			"developer you want to do this so they can add this "
			"functionality.\n");
		return -EINVAL;
	}

	/*
	 * first we need to walk all of the trees except the extent tree and pin
	 * down/exclude the bytes that are in use so we don't overwrite any
	 * existing metadata.
	 * If pinned, unpin will be done in the end of transaction.
	 * If excluded, cleanup will be done in check_chunks_and_extents_lowmem.
	 */
again:
	if (pin) {
		ret = pin_metadata_blocks();
		if (ret) {
			fprintf(stderr, "error pinning down used bytes\n");
			return ret;
		}
	} else {
		ret = exclude_metadata_blocks();
		if (ret) {
			fprintf(stderr, "error excluding used bytes\n");
			printf("try to pin down used bytes\n");
			pin = true;
			goto again;
		}
	}

	/*
	 * Need to drop all the block groups since we're going to recreate all
	 * of them again.
	 */
	btrfs_free_block_groups(gfs_info);
	ret = reset_block_groups();
	if (ret) {
		fprintf(stderr, "error resetting the block groups\n");
		return ret;
	}

	/* Ok we can allocate now, reinit the extent root */
	ret = btrfs_fsck_reinit_root(trans, gfs_info->extent_root);
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
		struct btrfs_block_group_item bgi;
		struct btrfs_block_group *cache;
		struct btrfs_key key;

		cache = btrfs_lookup_first_block_group(gfs_info, start);
		if (!cache)
			break;
		start = cache->start + cache->length;
		btrfs_set_stack_block_group_used(&bgi, cache->used);
		btrfs_set_stack_block_group_chunk_objectid(&bgi,
					BTRFS_FIRST_CHUNK_TREE_OBJECTID);
		btrfs_set_stack_block_group_flags(&bgi, cache->flags);
		key.objectid = cache->start;
		key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
		key.offset = cache->length;
		ret = btrfs_insert_item(trans, gfs_info->extent_root, &key,
					&bgi, sizeof(bgi));
		if (ret) {
			fprintf(stderr, "Error adding block group\n");
			return ret;
		}
		btrfs_run_delayed_refs(trans, -1);
	}

	ret = reset_balance(trans);
	if (ret)
		fprintf(stderr, "error resetting the pending balance\n");

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

	root = btrfs_read_fs_root(gfs_info, &key);
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
	btrfs_set_super_log_root(gfs_info->super_copy, 0);
	btrfs_set_super_log_root_level(gfs_info->super_copy, 0);
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
		sectorsize = gfs_info->sectorsize;
		ret = read_extent_data(gfs_info, buf, start + offset,
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

	buf = malloc(gfs_info->sectorsize);
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
	struct btrfs_path path;
	struct btrfs_root *tree_root = gfs_info->tree_root;
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

		cur_root = btrfs_read_fs_root(gfs_info, &key);
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
	struct btrfs_root *extent_root = gfs_info->extent_root;
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

	buf = malloc(gfs_info->sectorsize);
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

static int build_roots_info_cache(void)
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
	ret = btrfs_search_slot(NULL, gfs_info->extent_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	leaf = path.nodes[0];

	while (1) {
		struct btrfs_key found_key;
		struct btrfs_extent_item *ei;
		struct btrfs_extent_inline_ref *iref;
		unsigned long item_end;
		int slot = path.slots[0];
		int type;
		u64 flags;
		u64 root_id;
		u8 level;
		struct cache_extent *entry;
		struct root_item_info *rii;

		ctx.item_count++;
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(gfs_info->extent_root, &path);
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
		item_end = (unsigned long)ei + btrfs_item_size_nr(leaf, slot);

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
		 * It's a valid extent/metadata item that has no inline ref,
		 * but SHARED_BLOCK_REF or other shared references.
		 * So we need to do extra check to avoid reading beyond leaf
		 * boundary.
		 */
		if ((unsigned long)iref >= item_end)
			goto next;

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
static int repair_root_items(void)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_trans_handle *trans = NULL;
	int ret = 0;
	int bad_roots = 0;
	int need_trans = 0;

	btrfs_init_path(&path);

	ret = build_roots_info_cache();
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
		trans = btrfs_start_transaction(gfs_info->tree_root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			goto out;
		}
	}

	ret = btrfs_search_slot(trans, gfs_info->tree_root, &key, &path,
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
							       gfs_info->tree_root);
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
		btrfs_commit_transaction(trans, gfs_info->tree_root);
	if (ret < 0)
		return ret;

	return bad_roots;
}

/*
 * Number of free space cache inodes to delete in one transaction.
 *
 * This is to speedup the v1 space cache deletion for large fs.
 */
#define NR_BLOCK_GROUP_CLUSTER		(16)

static int clear_free_space_cache(void)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_block_group *bg_cache;
	int nr_handled = 0;
	u64 current = 0;
	int ret = 0;

	trans = btrfs_start_transaction(gfs_info->tree_root, 0);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error("failed to start a transaction: %m");
		return ret;
	}

	/* Clear all free space cache inodes and its extent data */
	while (1) {
		bg_cache = btrfs_lookup_first_block_group(gfs_info, current);
		if (!bg_cache)
			break;
		ret = btrfs_clear_free_space_cache(trans, bg_cache);
		if (ret < 0) {
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		nr_handled++;

		if (nr_handled == NR_BLOCK_GROUP_CLUSTER) {
			ret = btrfs_commit_transaction(trans, gfs_info->tree_root);
			if (ret < 0) {
				errno = -ret;
				error("failed to start a transaction: %m");
				return ret;
			}
			trans = btrfs_start_transaction(gfs_info->tree_root, 0);
			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				errno = -ret;
				error("failed to start a transaction: %m");
				return ret;
			}
		}
		current = bg_cache->start + bg_cache->length;
	}

	btrfs_set_super_cache_generation(gfs_info->super_copy, (u64)-1);
	ret = btrfs_commit_transaction(trans, gfs_info->tree_root);
	if (ret < 0) {
		errno = -ret;
		error("failed to start a transaction: %m");
	}
	return ret;
}

static int do_clear_free_space_cache(int clear_version)
{
	int ret = 0;

	if (clear_version == 1) {
		if (btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE))
			warning(
"free space cache v2 detected, use --clear-space-cache v2, proceeding with clearing v1");

		ret = clear_free_space_cache();
		if (ret) {
			error("failed to clear free space cache");
			ret = 1;
		} else {
			printf("Free space cache cleared\n");
		}
	} else if (clear_version == 2) {
		if (!btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE)) {
			printf("no free space cache v2 to clear\n");
			ret = 0;
			goto close_out;
		}
		printf("Clear free space cache v2\n");
		ret = btrfs_clear_free_space_tree(gfs_info);
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

static int validate_free_space_cache(struct btrfs_root *root)
{
	int ret;

	/*
	 * If cache generation is between 0 and -1ULL, sb generation must be
	 * equal to sb cache generation or the v1 space caches are outdated.
	 */
	if (btrfs_super_cache_generation(gfs_info->super_copy) != -1ULL &&
	    btrfs_super_cache_generation(gfs_info->super_copy) != 0 &&
	    btrfs_super_generation(gfs_info->super_copy) !=
	    btrfs_super_cache_generation(gfs_info->super_copy)) {
		printf(
"cache and super generation don't match, space cache will be invalidated\n");
		return 0;
	}

	ret = check_space_cache(root);
	if (ret && btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE) &&
	    repair) {
		ret = do_clear_free_space_cache(2);
		if (ret)
			goto out;

		ret = btrfs_create_free_space_tree(gfs_info);
		if (ret)
			error("couldn't repair freespace tree");
	}

out:
	return ret ? -EINVAL : 0;
}

int truncate_free_ino_items(struct btrfs_root *root)
{
	struct btrfs_path path;
	struct btrfs_key key = { .objectid = BTRFS_FREE_INO_OBJECTID,
				 .type = (u8)-1,
				 .offset = (u64)-1 };
	struct btrfs_trans_handle *trans;
	int ret;

	trans = btrfs_start_transaction(root, 0);
	if (IS_ERR(trans)) {
		error("Unable to start ino removal transaction");
		return PTR_ERR(trans);
	}

	while (1) {
		struct extent_buffer *leaf;
		struct btrfs_file_extent_item *fi;
		struct btrfs_key found_key;
		u8 found_type;

		btrfs_init_path(&path);
		ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
		if (ret < 0) {
			btrfs_abort_transaction(trans, ret);
			goto out;
		} else if (ret > 0) {
			ret = 0;
			/* No more items, finished truncating */
			if (path.slots[0] == 0) {
				btrfs_release_path(&path);
				goto out;
			}
			path.slots[0]--;
		}
		fi = NULL;
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		found_type = found_key.type;

		/* Ino cache also has free space bitmaps in the fs stree */
		if (found_key.objectid != BTRFS_FREE_INO_OBJECTID &&
		    found_key.objectid != BTRFS_FREE_SPACE_OBJECTID) {
			btrfs_release_path(&path);
			/* Now delete the FREE_SPACE_OBJECTID */
			if (key.objectid == BTRFS_FREE_INO_OBJECTID) {
				key.objectid = BTRFS_FREE_SPACE_OBJECTID;
				continue;
			}
			break;
		}

		if (found_type == BTRFS_EXTENT_DATA_KEY) {
			int extent_type;
			u64 extent_disk_bytenr;
			u64 extent_num_bytes;
			u64 extent_offset;

			fi = btrfs_item_ptr(leaf, path.slots[0],
					    struct btrfs_file_extent_item);
			extent_type = btrfs_file_extent_type(leaf, fi);
			ASSERT(extent_type == BTRFS_FILE_EXTENT_REG);
			extent_disk_bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
			extent_num_bytes = btrfs_file_extent_disk_num_bytes (leaf, fi);
			extent_offset = found_key.offset -
					btrfs_file_extent_offset(leaf, fi);
			ASSERT(extent_offset == 0);
			ret = btrfs_free_extent(trans, root, extent_disk_bytenr,
						extent_num_bytes, 0, root->objectid,
						BTRFS_FREE_INO_OBJECTID, 0);
			if (ret < 0) {
				btrfs_abort_transaction(trans, ret);
				btrfs_release_path(&path);
				goto out;
			}

			ret = btrfs_del_csums(trans, extent_disk_bytenr,
					      extent_num_bytes);
			if (ret < 0) {
				btrfs_abort_transaction(trans, ret);
				btrfs_release_path(&path);
				goto out;
			}
		}

		ret = btrfs_del_item(trans, root, &path);
		BUG_ON(ret);
		btrfs_release_path(&path);
	}

	btrfs_commit_transaction(trans, root);
out:
	return ret;
}

int clear_ino_cache_items(void)
{
	int ret;
	struct btrfs_path path;
	struct btrfs_key key;

	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, gfs_info->tree_root, &key, &path,	0, 0);
	if (ret < 0)
		return ret;

	while(1) {
		struct btrfs_key found_key;

		btrfs_item_key_to_cpu(path.nodes[0], &found_key, path.slots[0]);
		if (found_key.type == BTRFS_ROOT_ITEM_KEY &&
		    is_fstree(found_key.objectid)) {
			struct btrfs_root *root;

			found_key.offset = (u64)-1;
			root = btrfs_read_fs_root(gfs_info, &found_key);
			if (IS_ERR(root))
				goto next;
			ret = truncate_free_ino_items(root);
			if (ret)
				goto out;
			printf("Successfully cleaned up ino cache for root id: %lld\n",
					root->objectid);
		} else {
			/* If we get a negative tree this means it's the last one */
			if ((s64)found_key.objectid < 0 &&
			    found_key.type == BTRFS_ROOT_ITEM_KEY)
				goto out;
		}

		/*
		 * Only fs roots contain an ino cache information - either
		 * FS_TREE_OBJECTID or subvol id >= BTRFS_FIRST_FREE_OBJECTID
		 */
next:
		if (key.objectid == BTRFS_FS_TREE_OBJECTID) {
			key.objectid = BTRFS_FIRST_FREE_OBJECTID;
			btrfs_release_path(&path);
			ret = btrfs_search_slot(NULL, gfs_info->tree_root, &key,
						&path,	0, 0);
			if (ret < 0)
				return ret;
		} else {
			ret = btrfs_next_item(gfs_info->tree_root, &path);
			if (ret < 0) {
				goto out;
			} else if (ret > 0) {
				ret = 0;
				goto out;
			}
		}
	}

out:
	btrfs_release_path(&path);
	return ret;
}

static const char * const cmd_check_usage[] = {
	"btrfs check [options] <device>",
	"Check structural integrity of a filesystem (unmounted).",
	"Check structural integrity of an unmounted filesystem. Verify internal",
	"trees' consistency and item connectivity. In the repair mode try to",
	"fix the problems found. ",
	"WARNING: the repair mode is considered dangerous and should not be used",
	"         without prior analysis of problems found on the filesystem."
	"",
	"Options:",
	"  starting point selection:",
	"       -s|--super <superblock>     use this superblock copy",
	"       -b|--backup                 use the first valid backup root copy",
	"       -r|--tree-root <bytenr>     use the given bytenr for the tree root",
	"       --chunk-root <bytenr>       use the given bytenr for the chunk tree root",
	"  operation modes:",
	"       --readonly                  run in read-only mode (default)",
	"       --repair                    try to repair the filesystem",
	"       --force                     skip mount checks, repair is not possible",
	"       --mode <MODE>               allows choice of memory/IO trade-offs",
	"                                   where MODE is one of:",
	"                                   original - read inodes and extents to memory (requires",
	"                                              more memory, does less IO)",
	"                                   lowmem   - try to use less memory but read blocks again",
	"                                              when needed (experimental)",
	"  repair options:",
	"       --init-csum-tree            create a new CRC tree",
	"       --init-extent-tree          create a new extent tree",
	"       --clear-space-cache v1|v2   clear space cache for v1 or v2",
	"       --clear-ino-cache 	    clear ino cache leftover items",
	"  check and reporting options:",
	"       --check-data-csum           verify checksums of data blocks",
	"       -Q|--qgroup-report          print a report on qgroup consistency",
	"       -E|--subvol-extents <subvolid>",
	"                                   print subvolume extents and sharing state",
	"       -p|--progress               indicate progress",
	NULL
};

static int cmd_check(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct cache_tree root_cache;
	struct btrfs_root *root;
	struct open_ctree_flags ocf = { 0 };
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
	int clear_ino_cache = 0;
	int qgroup_report = 0;
	int qgroups_repaired = 0;
	int qgroup_verify_ret;
	unsigned ctree_flags = OPEN_CTREE_EXCLUSIVE;
	int force = 0;

	while(1) {
		int c;
		enum { GETOPT_VAL_REPAIR = 257, GETOPT_VAL_INIT_CSUM,
			GETOPT_VAL_INIT_EXTENT, GETOPT_VAL_CHECK_CSUM,
			GETOPT_VAL_READONLY, GETOPT_VAL_CHUNK_TREE,
			GETOPT_VAL_MODE, GETOPT_VAL_CLEAR_SPACE_CACHE,
			GETOPT_VAL_CLEAR_INO_CACHE, GETOPT_VAL_FORCE };
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
			{ "clear-ino-cache", no_argument , NULL,
				GETOPT_VAL_CLEAR_INO_CACHE},
			{ "force", no_argument, NULL, GETOPT_VAL_FORCE },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "as:br:pE:Q", long_options, NULL);
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
				usage(cmd);
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
			case GETOPT_VAL_CLEAR_INO_CACHE:
				clear_ino_cache = 1;
				ctree_flags |= OPEN_CTREE_WRITES;
				break;
			case GETOPT_VAL_FORCE:
				force = 1;
				break;
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd);

	if (ctx.progress_enabled) {
		ctx.tp = TASK_NOTHING;
		ctx.info = task_init(print_status_check, print_status_return, &ctx);
	}

	/* This check is the only reason for --readonly to exist */
	if (readonly && repair) {
		error("repair options are not compatible with --readonly");
		exit(1);
	}

	if (repair && !force) {
		int delay = 10;

		printf("WARNING:\n\n");
		printf("\tDo not use --repair unless you are advised to do so by a developer\n");
		printf("\tor an experienced user, and then only after having accepted that no\n");
		printf("\tfsck can successfully repair all types of filesystem corruption. Eg.\n");
		printf("\tsome software or hardware bugs can fatally damage a volume.\n");
		printf("\tThe operation will start in %d seconds.\n", delay);
		printf("\tUse Ctrl-C to stop it.\n");
		while (delay) {
			printf("%2d", delay--);
			fflush(stdout);
			sleep(1);
		}
		printf("\nStarting repair.\n");
	}

	/*
	 * experimental and dangerous
	 */
	if (repair && check_mode == CHECK_MODE_LOWMEM)
		warning("low-memory mode repair support is only partial");

	printf("Opening filesystem to check...\n");

	radix_tree_init();
	cache_tree_init(&root_cache);
	qgroup_set_item_count_ptr(&ctx.item_count);

	ret = check_mounted(argv[optind]);
	if (!force) {
		if (ret < 0) {
			errno = -ret;
			error("could not check mount status: %m");
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

	ocf.filename = argv[optind];
	ocf.sb_bytenr = bytenr;
	ocf.root_tree_bytenr = tree_root_bytenr;
	ocf.chunk_tree_bytenr = chunk_root_bytenr;
	ocf.flags = ctree_flags;
	gfs_info = open_ctree_fs_info(&ocf);
	if (!gfs_info) {
		error("cannot open file system");
		ret = -EIO;
		err |= !!ret;
		goto err_out;
	}

	root = gfs_info->fs_root;
	uuid_unparse(gfs_info->super_copy->fsid, uuidbuf);

	printf("Checking filesystem on %s\nUUID: %s\n", argv[optind], uuidbuf);

	/*
	 * Check the bare minimum before starting anything else that could rely
	 * on it, namely the tree roots, any local consistency checks
	 */
	if (!extent_buffer_uptodate(gfs_info->tree_root->node) ||
	    !extent_buffer_uptodate(gfs_info->dev_root->node) ||
	    !extent_buffer_uptodate(gfs_info->chunk_root->node)) {
		error("critical roots corrupted, unable to check the filesystem");
		err |= !!ret;
		ret = -EIO;
		goto close_out;
	}

	if (clear_space_cache) {
		ret = do_clear_free_space_cache(clear_space_cache);
		err |= !!ret;
		goto close_out;
	}

	if (clear_ino_cache) {
		ret = clear_ino_cache_items();
		err = ret;
		goto close_out;
	}

	/*
	 * repair mode will force us to commit transaction which
	 * will make us fail to load log tree when mounting.
	 */
	if (repair && btrfs_super_log_root(gfs_info->super_copy)) {
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
		ret = qgroup_verify_all(gfs_info);
		err |= !!ret;
		if (ret >= 0)
			report_qgroups(1);
		goto close_out;
	}
	if (subvolid) {
		printf("Print extent state for subvolume %llu on %s\nUUID: %s\n",
		       subvolid, argv[optind], uuidbuf);
		ret = print_extent_state(gfs_info, subvolid);
		err |= !!ret;
		goto close_out;
	}

	if (init_extent_tree || init_csum_tree) {
		struct btrfs_trans_handle *trans;

		trans = btrfs_start_transaction(gfs_info->extent_root, 0);
		if (IS_ERR(trans)) {
			error("error starting transaction");
			ret = PTR_ERR(trans);
			err |= !!ret;
			goto close_out;
		}

		trans->reinit_extent_tree = true;
		if (init_extent_tree) {
			printf("Creating a new extent tree\n");
			ret = reinit_extent_tree(trans,
					 check_mode == CHECK_MODE_ORIGINAL);
			err |= !!ret;
			if (ret)
				goto close_out;
		}

		if (init_csum_tree) {
			printf("Reinitialize checksum tree\n");
			ret = btrfs_fsck_reinit_root(trans, gfs_info->csum_root);
			if (ret) {
				error("checksum tree initialization failed: %d",
						ret);
				ret = -EIO;
				err |= !!ret;
				goto close_out;
			}

			ret = fill_csum_tree(trans, gfs_info->csum_root,
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
		ret = btrfs_commit_transaction(trans, gfs_info->extent_root);
		err |= !!ret;
		if (ret)
			goto close_out;
	}
	if (!extent_buffer_uptodate(gfs_info->extent_root->node)) {
		error("critical: extent_root, unable to check the filesystem");
		ret = -EIO;
		err |= !!ret;
		goto close_out;
	}
	if (!extent_buffer_uptodate(gfs_info->csum_root->node)) {
		error("critical: csum_root, unable to check the filesystem");
		ret = -EIO;
		err |= !!ret;
		goto close_out;
	}

	if (!init_extent_tree) {
		if (!ctx.progress_enabled) {
			fprintf(stderr, "[1/7] checking root items\n");
		} else {
			ctx.tp = TASK_ROOT_ITEMS;
			task_start(ctx.info, &ctx.start_time, &ctx.item_count);
		}
		ret = repair_root_items();
		task_stop(ctx.info);
		if (ret < 0) {
			err = !!ret;
			errno = -ret;
			error("failed to repair root items: %m");
			/*
			 * For repair, if we can't repair root items, it's
			 * fatal.  But for non-repair, it's pretty rare to hit
			 * such v3.17 era bug, we want to continue check.
			 */
			if (repair)
				goto close_out;
			err |= 1;
		} else {
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
			}
		}
	} else {
		fprintf(stderr, "[1/7] checking root items... skipped\n");
	}

	if (!ctx.progress_enabled) {
		fprintf(stderr, "[2/7] checking extents\n");
	} else {
		ctx.tp = TASK_EXTENTS;
		task_start(ctx.info, &ctx.start_time, &ctx.item_count);
	}
	ret = do_check_chunks_and_extents();
	task_stop(ctx.info);
	err |= !!ret;
	if (ret)
		error(
		"errors found in extent allocation tree or chunk allocation");

	/* Only re-check super size after we checked and repaired the fs */
	err |= !is_super_size_valid();

	is_free_space_tree = btrfs_fs_compat_ro(gfs_info, FREE_SPACE_TREE);

	if (!ctx.progress_enabled) {
		if (is_free_space_tree)
			fprintf(stderr, "[3/7] checking free space tree\n");
		else
			fprintf(stderr, "[3/7] checking free space cache\n");
	} else {
		ctx.tp = TASK_FREE_SPACE;
		task_start(ctx.info, &ctx.start_time, &ctx.item_count);
	}

	ret = validate_free_space_cache(root);
	task_stop(ctx.info);
	err |= !!ret;

	/*
	 * We used to have to have these hole extents in between our real
	 * extents so if we don't have this flag set we need to make sure there
	 * are no gaps in the file extents for inodes, otherwise we can just
	 * ignore it when this happens.
	 */
	no_holes = btrfs_fs_incompat(gfs_info, NO_HOLES);
	if (!ctx.progress_enabled) {
		fprintf(stderr, "[4/7] checking fs roots\n");
	} else {
		ctx.tp = TASK_FS_ROOTS;
		task_start(ctx.info, &ctx.start_time, &ctx.item_count);
	}

	ret = do_check_fs_roots(&root_cache);
	task_stop(ctx.info);
	err |= !!ret;
	if (ret) {
		error("errors found in fs roots");
		goto out;
	}

	if (!ctx.progress_enabled) {
		if (check_data_csum)
			fprintf(stderr, "[5/7] checking csums against data\n");
		else
			fprintf(stderr,
		"[5/7] checking only csums items (without verifying data)\n");
	} else {
		ctx.tp = TASK_CSUMS;
		task_start(ctx.info, &ctx.start_time, &ctx.item_count);
	}

	ret = check_csums(root);
	task_stop(ctx.info);
	/*
	 * Data csum error is not fatal, and it may indicate more serious
	 * corruption, continue checking.
	 */
	if (ret)
		error("errors found in csum tree");
	err |= !!ret;

	/* For low memory mode, check_fs_roots_v2 handles root refs */
        if (check_mode != CHECK_MODE_LOWMEM) {
		if (!ctx.progress_enabled) {
			fprintf(stderr, "[6/7] checking root refs\n");
		} else {
			ctx.tp = TASK_ROOT_REFS;
			task_start(ctx.info, &ctx.start_time, &ctx.item_count);
		}

		ret = check_root_refs(root, &root_cache);
		task_stop(ctx.info);
		err |= !!ret;
		if (ret) {
			error("errors found in root refs");
			goto out;
		}
	} else {
		fprintf(stderr,
	"[6/7] checking root refs done with fs roots in lowmem mode, skipping\n");
	}

	while (repair && !list_empty(&gfs_info->recow_ebs)) {
		struct extent_buffer *eb;

		eb = list_first_entry(&gfs_info->recow_ebs,
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

	if (gfs_info->quota_enabled) {
		if (!ctx.progress_enabled) {
			fprintf(stderr, "[7/7] checking quota groups\n");
		} else {
			ctx.tp = TASK_QGROUPS;
			task_start(ctx.info, &ctx.start_time, &ctx.item_count);
		}
		qgroup_verify_ret = qgroup_verify_all(gfs_info);
		task_stop(ctx.info);
		if (qgroup_verify_ret < 0) {
			error("failed to check quota groups");
			err |= !!qgroup_verify_ret;
			goto out;
		}
		report_qgroups(0);
		ret = repair_qgroups(gfs_info, &qgroups_repaired, false);
		if (ret) {
			error("failed to repair quota groups");
			goto out;
		}
		if (qgroup_verify_ret && (!qgroups_repaired || ret))
			err |= !!qgroup_verify_ret;
		ret = 0;
	} else {
		fprintf(stderr,
		"[7/7] checking quota groups skipped (not enabled on this FS)\n");
	}

	if (!list_empty(&gfs_info->recow_ebs)) {
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
DEFINE_SIMPLE_COMMAND(check, "check");
