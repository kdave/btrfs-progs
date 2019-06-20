/*
 * Copyright (C) 2013 FUJITSU LIMITED.  All rights reserved.
 * Written by Miao Xie <miaox@cn.fujitsu.com>
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

#ifndef __BTRFS_CHECK_H__
#define __BTRFS_CHECK_H__

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#include "ctree.h"
#include "extent-cache.h"
#include "kernel-lib/list.h"
#else
#include <btrfs/kerncompat.h>
#include <btrfs/ctree.h>
#include <btrfs/extent-cache.h>
#include <btrfs/list.h>
#endif /* BTRFS_FLAT_INCLUDES */

struct block_group_record {
	struct cache_extent cache;
	/* Used to identify the orphan block groups */
	struct list_head list;

	u64 generation;

	u64 objectid;
	u8  type;
	u64 offset;

	u64 flags;
};

struct block_group_tree {
	struct cache_tree tree;
	struct list_head block_groups;
};

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

struct stripe {
	u64 devid;
	u64 offset;
	u8 dev_uuid[BTRFS_UUID_SIZE];
};

struct chunk_record {
	struct cache_extent cache;

	struct list_head list;
	struct list_head dextents;
	struct block_group_record *bg_rec;

	u64 generation;

	u64 objectid;
	u8  type;
	u64 offset;

	u64 owner;
	u64 length;
	u64 type_flags;
	u64 stripe_len;
	u16 num_stripes;
	u16 sub_stripes;
	u32 io_align;
	u32 io_width;
	u32 sector_size;
	struct stripe stripes[0];
};

struct device_extent_record {
	struct cache_extent cache;
	/*
	 * Used to identify the orphan device extents (the device extents
	 * don't belong to a chunk or a device)
	 */
	struct list_head chunk_list;
	struct list_head device_list;

	u64 generation;

	u64 objectid;
	u8  type;
	u64 offset;

	u64 chunk_objectid;
	u64 chunk_offset;
	u64 length;
};

struct device_extent_tree {
	struct cache_tree tree;
	/*
	 * The idea is:
	 * When checking the chunk information, we move the device extents
	 * that has its chunk to the chunk's device extents list. After the
	 * check, if there are still some device extents in no_chunk_orphans,
	 * it means there are some device extents which don't belong to any
	 * chunk.
	 *
	 * The usage of no_device_orphans is the same as the first one, but it
	 * is for the device information check.
	 */
	struct list_head no_chunk_orphans;
	struct list_head no_device_orphans;
};

static inline unsigned long btrfs_chunk_record_size(int num_stripes)
{
	return sizeof(struct chunk_record) +
	       sizeof(struct stripe) * num_stripes;
}
void free_chunk_cache_tree(struct cache_tree *chunk_cache);

/*
 * Function to check validation for num_stripes, or it can call
 * float point error for 0 division
 * return < 0 for invalid combination
 * return 0 for valid combination
 */
static inline int check_num_stripes(u64 type, int num_stripes)
{
	if (num_stripes == 0)
		return -1;
	if (type & BTRFS_BLOCK_GROUP_RAID5 && num_stripes <= 1)
		return -1;
	if (type & BTRFS_BLOCK_GROUP_RAID6 && num_stripes <= 2)
		return -1;
	return 0;
}

u64 calc_stripe_length(u64 type, u64 length, int num_stripes);
/* For block group tree */
static inline void block_group_tree_init(struct block_group_tree *tree)
{
	cache_tree_init(&tree->tree);
	INIT_LIST_HEAD(&tree->block_groups);
}

int insert_block_group_record(struct block_group_tree *tree,
			      struct block_group_record *bg_rec);
void free_block_group_tree(struct block_group_tree *tree);

/* For device extent tree */
static inline void device_extent_tree_init(struct device_extent_tree *tree)
{
	cache_tree_init(&tree->tree);
	INIT_LIST_HEAD(&tree->no_chunk_orphans);
	INIT_LIST_HEAD(&tree->no_device_orphans);
}

int insert_device_extent_record(struct device_extent_tree *tree,
				struct device_extent_record *de_rec);
void free_device_extent_tree(struct device_extent_tree *tree);


/* Create various in-memory record by on-disk data */
struct chunk_record *btrfs_new_chunk_record(struct extent_buffer *leaf,
					    struct btrfs_key *key,
					    int slot);
struct block_group_record *
btrfs_new_block_group_record(struct extent_buffer *leaf, struct btrfs_key *key,
			     int slot);
struct device_extent_record *
btrfs_new_device_extent_record(struct extent_buffer *leaf,
			       struct btrfs_key *key, int slot);

int check_chunks(struct cache_tree *chunk_cache,
		 struct block_group_tree *block_group_cache,
		 struct device_extent_tree *dev_extent_cache,
		 struct list_head *good, struct list_head *bad,
		 struct list_head *rebuild, int silent);
#endif
