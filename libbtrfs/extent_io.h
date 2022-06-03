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

#ifndef __BTRFS_EXTENT_IO_H__
#define __BTRFS_EXTENT_IO_H__

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#include "libbtrfs/extent-cache.h"
#include "kernel-lib/list.h"
#else
#include <btrfs/kerncompat.h>
#include <btrfs/extent-cache.h>
#include <btrfs/list.h>
#endif /* BTRFS_FLAT_INCLUDES */

struct btrfs_fs_info;

struct extent_io_tree {
	struct cache_tree state;
	struct cache_tree cache;
	struct list_head lru;
	u64 cache_size;
	u64 max_cache_size;
};

struct extent_state {
	struct cache_extent cache_node;
	u64 start;
	u64 end;
	int refs;
	unsigned long state;
	u64 xprivate;
};

struct extent_buffer {
	struct cache_extent cache_node;
	u64 start;
	struct list_head lru;
	struct list_head recow;
	u32 len;
	int refs;
	u32 flags;
	struct btrfs_fs_info *fs_info;
	char data[] __attribute__((aligned(8)));
};

void read_extent_buffer(const struct extent_buffer *eb, void *dst,
			unsigned long start, unsigned long len);
void write_extent_buffer(struct extent_buffer *eb, const void *src,
			 unsigned long start, unsigned long len);
#endif
