/*
 * Copyright (C) 2012 Oracle.  All rights reserved.
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

#ifndef __BTRFS_REPAIR_H__
#define __BTRFS_REPAIR_H__

#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "common/extent-cache.h"

struct btrfs_trans_handle;
struct extent_io_tree;

/* Repair mode */
extern int opt_check_repair;

struct btrfs_corrupt_block {
	struct cache_extent cache;
	struct btrfs_key key;
	int level;
};

int btrfs_add_corrupt_extent_record(struct btrfs_fs_info *info,
				    struct btrfs_key *first_key,
				    u64 start, u64 len, int level);
int btrfs_fix_block_accounting(struct btrfs_trans_handle *trans);
int btrfs_mark_used_tree_blocks(struct btrfs_fs_info *fs_info,
				struct extent_io_tree *tree);
int btrfs_mark_used_blocks(struct btrfs_fs_info *fs_info,
			   struct extent_io_tree *tree);

#endif
