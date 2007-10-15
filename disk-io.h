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

#ifndef __DISKIO__
#define __DISKIO__
#include "list.h"

struct btrfs_buffer {
	u64 bytenr;
	u64 dev_bytenr;
	u32 size;
	int count;
	int fd;
	struct list_head dirty;
	struct list_head cache;
	union {
		struct btrfs_node node;
		struct btrfs_leaf leaf;
	};
};

struct btrfs_buffer *read_tree_block(struct btrfs_root *root, u64 bytenr,
				     u32 blocksize);
struct btrfs_buffer *find_tree_block(struct btrfs_root *root, u64 bytenr,
				     u32 blocksize);
int write_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct btrfs_buffer *buf);
int dirty_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct btrfs_buffer *buf);
int clean_tree_block(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root, struct btrfs_buffer *buf);
int btrfs_commit_transaction(struct btrfs_trans_handle *trans, struct btrfs_root
			     *root, struct btrfs_super_block *s);
struct btrfs_root *open_ctree(char *filename, struct btrfs_super_block *s);
struct btrfs_root *open_ctree_fd(int fp, struct btrfs_super_block *super);
int close_ctree(struct btrfs_root *root, struct btrfs_super_block *s);
void btrfs_block_release(struct btrfs_root *root, struct btrfs_buffer *buf);
int write_ctree_super(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		      struct btrfs_super_block *s);
int btrfs_map_bh_to_logical(struct btrfs_root *root, struct btrfs_buffer *bh,
			     u64 logical);
int btrfs_csum_super(struct btrfs_root *root, struct btrfs_super_block *super);
int btrfs_csum_node(struct btrfs_root *root, struct btrfs_node *node);
#define BTRFS_SUPER_INFO_OFFSET (16 * 1024)

#endif
