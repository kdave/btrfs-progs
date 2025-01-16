/*
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

#ifndef _EXTENT_TREE_UTILS_H_
#define _EXTENT_TREE_UTILS_H_

#include "kerncompat.h"

struct btrfs_inode_item;
struct btrfs_path;
struct btrfs_root;
struct btrfs_trans_handle;

int btrfs_next_extent_item(struct btrfs_root *root, struct btrfs_path *path,
			   u64 max_objectid);

#endif
