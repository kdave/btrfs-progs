/*
 * Copyright (C) 2015 Facebook.  All rights reserved.
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

#ifndef __BTRFS_FREE_SPACE_TREE_H__
#define __BTRFS_FREE_SPACE_TREE_H__

#define BTRFS_FREE_SPACE_BITMAP_SIZE 256
#define BTRFS_FREE_SPACE_BITMAP_BITS (BTRFS_FREE_SPACE_BITMAP_SIZE * BITS_PER_BYTE)

int btrfs_clear_free_space_tree(struct btrfs_fs_info *fs_info);
int load_free_space_tree(struct btrfs_fs_info *fs_info,
			 struct btrfs_block_group *block_group);
int populate_free_space_tree(struct btrfs_trans_handle *trans,
			     struct btrfs_block_group *block_group);
int remove_block_group_free_space(struct btrfs_trans_handle *trans,
				  struct btrfs_block_group *block_group);
int add_to_free_space_tree(struct btrfs_trans_handle *trans, u64 start,
			   u64 size);
int remove_from_free_space_tree(struct btrfs_trans_handle *trans, u64 start,
				u64 size);
int btrfs_create_free_space_tree(struct btrfs_fs_info *info);
int add_block_group_free_space(struct btrfs_trans_handle *trans,
			       struct btrfs_block_group *block_group);

#endif
