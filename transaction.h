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

#ifndef __BTRFS_TRANSACTION_H__
#define __BTRFS_TRANSACTION_H__

#include "kerncompat.h"
#include "ctree.h"
#include "kernel-lib/list.h"
#include "kernel-shared/delayed-ref.h"

struct btrfs_trans_handle {
	struct btrfs_fs_info *fs_info;
	u64 transid;
	u64 alloc_exclude_start;
	u64 alloc_exclude_nr;
	bool reinit_extent_tree;
	unsigned int allocating_chunk:1;
	u64 delayed_ref_updates;
	unsigned long blocks_reserved;
	unsigned long blocks_used;
	struct btrfs_block_group *block_group;
	struct btrfs_delayed_ref_root delayed_refs;
	struct list_head dirty_bgs;
};

struct btrfs_trans_handle* btrfs_start_transaction(struct btrfs_root *root,
		int num_blocks);
int __commit_transaction(struct btrfs_trans_handle *trans,
				struct btrfs_root *root);
int commit_tree_roots(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info);
int btrfs_commit_transaction(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root);
void btrfs_abort_transaction(struct btrfs_trans_handle *trans, int error);

#endif
