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

#ifndef __BTRFS_TUNE_H__
#define __BTRFS_TUNE_H__

#include <uuid/uuid.h>

struct btrfs_root;
struct btrfs_fs_info;

int update_seeding_flag(struct btrfs_root *root, const char *device, int set_flag, int force);

int change_uuid(struct btrfs_fs_info *fs_info, const char *new_fsid_str);
int set_metadata_uuid(struct btrfs_root *root, const char *uuid_string);

int convert_to_bg_tree(struct btrfs_fs_info *fs_info);
int convert_to_extent_tree(struct btrfs_fs_info *fs_info);

int btrfs_change_csum_type(struct btrfs_fs_info *fs_info, u16 new_csum_type);

int enable_quota(struct btrfs_fs_info *fs_info, bool simple);
int remove_squota(struct btrfs_fs_info *fs_info);

#endif
