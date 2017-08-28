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

#include "messages.h"

struct btrfs_trans_handle {
	u64 transid;
	u64 alloc_exclude_start;
	u64 alloc_exclude_nr;
	unsigned long blocks_reserved;
	unsigned long blocks_used;
	struct btrfs_block_group_cache *block_group;
};

static inline struct btrfs_trans_handle *
btrfs_start_transaction(struct btrfs_root *root, int num_blocks)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *h = malloc(sizeof(*h));

	if (!h)
		return ERR_PTR(-ENOMEM);
	if (root->commit_root) {
		error("commit_root aleady set when starting transaction");
		kfree(h);
		return ERR_PTR(-EINVAL);
	}
	if (fs_info->running_transaction) {
		error("attempt to start transaction over already running one");
		kfree(h);
		return ERR_PTR(-EINVAL);
	}
	fs_info->running_transaction = h;
	fs_info->generation++;
	h->transid = fs_info->generation;
	h->alloc_exclude_start = 0;
	h->alloc_exclude_nr = 0;
	h->blocks_reserved = num_blocks;
	h->blocks_used = 0;
	h->block_group = NULL;
	root->last_trans = h->transid;
	root->commit_root = root->node;
	extent_buffer_get(root->node);
	return h;
}

#endif
