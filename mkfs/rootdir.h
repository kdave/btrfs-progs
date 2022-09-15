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

/*
 * Defines and functions declarations for mkfs --rootdir
 */

#ifndef __BTRFS_MKFS_ROOTDIR_H__
#define __BTRFS_MKFS_ROOTDIR_H__

#include "kerncompat.h"
#include <sys/types.h>
#include <stdbool.h>
#include "kernel-lib/list.h"

struct btrfs_fs_info;
struct btrfs_root;

struct directory_name_entry {
	const char *dir_name;
	char *path;
	ino_t inum;
	struct list_head list;
};

int btrfs_mkfs_fill_dir(const char *source_dir, struct btrfs_root *root,
			bool verbose);
u64 btrfs_mkfs_size_dir(const char *dir_name, u32 sectorsize, u64 min_dev_size,
			u64 meta_profile, u64 data_profile);
int btrfs_mkfs_shrink_fs(struct btrfs_fs_info *fs_info, u64 *new_size_ret,
			 bool shrink_file_size);

#endif
