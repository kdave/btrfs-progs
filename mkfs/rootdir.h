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
#include <limits.h>
#include "kernel-lib/list.h"
#include "kernel-shared/compression.h"

#define ZLIB_BTRFS_DEFAULT_LEVEL		3
#define ZLIB_BTRFS_MAX_LEVEL			9

#define ZSTD_BTRFS_DEFAULT_LEVEL		3
#define ZSTD_BTRFS_MAX_LEVEL			15

struct btrfs_fs_info;
struct btrfs_root;

struct rootdir_subvol {
	struct list_head list;
	char dir[PATH_MAX];
	char full_path[PATH_MAX];
	bool is_default;
	bool readonly;
};

int btrfs_mkfs_fill_dir(struct btrfs_trans_handle *trans, const char *source_dir,
			struct btrfs_root *root, struct list_head *subvols,
			enum btrfs_compression_type compression,
			unsigned int compression_level);
u64 btrfs_mkfs_size_dir(const char *dir_name, u32 sectorsize, u64 min_dev_size,
			u64 meta_profile, u64 data_profile);
int btrfs_mkfs_shrink_fs(struct btrfs_fs_info *fs_info, u64 *new_size_ret,
			 bool shrink_file_size, u64 slack_size);

#endif
