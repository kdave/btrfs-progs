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

#ifndef __PENDING_EXTENT__
#define __PENDING_EXTENT__

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#include "rbtree.h"
#else
#include <btrfs/kerncompat.h>
#include <btrfs/rbtree.h>
#endif /* BTRFS_FLAT_INCLUDES */

struct cache_tree {
	struct rb_root root;
};

struct cache_extent {
	struct rb_node rb_node;
	u64 start;
	u64 size;
};

void cache_tree_init(struct cache_tree *tree);
void remove_cache_extent(struct cache_tree *tree,
			  struct cache_extent *pe);
struct cache_extent *find_first_cache_extent(struct cache_tree *tree,
						 u64 start);
struct cache_extent *prev_cache_extent(struct cache_extent *pe);
struct cache_extent *next_cache_extent(struct cache_extent *pe);
struct cache_extent *find_cache_extent(struct cache_tree *tree,
					   u64 start, u64 size);
int insert_cache_extent(struct cache_tree *tree, u64 start, u64 size);
int insert_existing_cache_extent(struct cache_tree *tree,
				 struct cache_extent *pe);

static inline int cache_tree_empty(struct cache_tree *tree)
{
	return RB_EMPTY_ROOT(&tree->root);
}

static inline void free_cache_extent(struct cache_extent *pe)
{
	free(pe);
}

struct cache_extent *alloc_pending_extent(u64 start, u64 size);

#endif
