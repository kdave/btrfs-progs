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
#include "kerncompat.h"
#include "rbtree.h"

struct pending_tree {
	struct rb_root root;
};

struct pending_extent {
	struct rb_node rb_node;
	u64 start;
	u64 size;
};

void pending_tree_init(struct pending_tree *tree);
void remove_pending_extent(struct pending_tree *tree,
			  struct pending_extent *pe);
struct pending_extent *find_first_pending_extent(struct pending_tree *tree,
						 u64 start);
struct pending_extent *next_pending_extent(struct pending_extent *pe);
struct pending_extent *find_pending_extent(struct pending_tree *tree,
					   u64 start, u64 size);
int insert_pending_extent(struct pending_tree *tree, u64 start, u64 size);

static inline void free_pending_extent(struct pending_extent *pe)
{
	free(pe);
}

struct pending_extent *alloc_pending_extent(u64 start, u64 size);

#endif
