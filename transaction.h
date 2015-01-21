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

	BUG_ON(!h);
	BUG_ON(root->commit_root);
	BUG_ON(fs_info->running_transaction);
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

static inline void btrfs_free_transaction(struct btrfs_root *root,
					  struct btrfs_trans_handle *handle)
{
	memset(handle, 0, sizeof(*handle));
	free(handle);
}

#endif
