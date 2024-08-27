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

#ifndef __ROOT_TREE_UTILS_H__
#define __ROOT_TREE_UTILS_H__

#include "kernel-shared/transaction.h"

int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid);
int btrfs_make_subvolume(struct btrfs_trans_handle *trans, u64 objectid,
			 bool readonly);
int btrfs_link_subvolume(struct btrfs_trans_handle *trans,
			 struct btrfs_root *parent_root,
			 u64 parent_dir, const char *name,
			 int namelen, struct btrfs_root *subvol);
int btrfs_rebuild_uuid_tree(struct btrfs_fs_info *fs_info);

#endif
