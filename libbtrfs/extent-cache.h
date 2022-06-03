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

#ifndef __BTRFS_EXTENT_CACHE_H__
#define __BTRFS_EXTENT_CACHE_H__

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#include "kernel-lib/rbtree.h"
#else
#include <btrfs/kerncompat.h>
#include <btrfs/rbtree.h>
#endif /* BTRFS_FLAT_INCLUDES */

struct cache_tree {
	struct rb_root root;
};

struct cache_extent {
	struct rb_node rb_node;
	u64 objectid;
	u64 start;
	u64 size;
};

#endif
