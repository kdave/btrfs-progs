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

int btrfs_clear_free_space_tree(struct btrfs_fs_info *fs_info);
int load_free_space_tree(struct btrfs_fs_info *fs_info,
			 struct btrfs_block_group_cache *block_group);

#endif
