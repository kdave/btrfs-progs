/*
 * Copyright (C) 2013 Fujitsu.  All rights reserved.
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

#ifndef __CHUNK_CHECK_H__
#define __CHUNK_CHECK_H__

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#include "extent-cache.h"
#include "list.h"
#else
#include <btrfs/kerncompat.h>
#include <btrfs/extent-cache.h>
#include <btrfs/list.h>
#endif /* BTRFS_FLAT_INCLUDES */

struct block_group_record {
	struct cache_extent cache;
	/* Used to identify the orphan block groups */
	struct list_head list;

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
};

struct chunk_record {
	struct cache_extent cache;

	u64 objectid;
	u8  type;
	u64 offset;

	u64 length;
	u64 type_flags;
	u16 num_stripes;
	u16 sub_stripes;
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

	u64 objectid;
	u8  type;
	u64 offset;

	u64 chunk_objecteid;
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

#endif
