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
	/* The path inside the source_dir. */
	char dir[PATH_MAX];
	/* st_dev and st_ino is going to uniquely determine an inode inside the host fs. */
	dev_t st_dev;
	ino_t st_ino;
	bool is_default;
	bool readonly;
};

/*
 * Represent a flag for specified inode at @full_path.
 */
struct rootdir_inode_flags_entry {
	struct list_head list;
	/* Fully canonicalized path to the source file. */
	char full_path[PATH_MAX];
	/* Path inside the source directory. */
	char inode_path[PATH_MAX];

	bool nodatacow;
	bool nodatasum;
};

int btrfs_mkfs_validate_subvols(const char *source_dir, struct list_head *subvols);
int btrfs_mkfs_validate_inode_flags(const char *source_dir, struct list_head *inode_flags);
int btrfs_mkfs_fill_dir(struct btrfs_trans_handle *trans, const char *source_dir,
			struct btrfs_root *root, struct list_head *subvols,
			struct list_head *inode_flags_list,
			enum btrfs_compression_type compression,
			unsigned int compression_level);
u64 btrfs_mkfs_size_dir(const char *dir_name, u32 sectorsize, u64 min_dev_size,
			u64 meta_profile, u64 data_profile);
int btrfs_mkfs_shrink_fs(struct btrfs_fs_info *fs_info, u64 *new_size_ret,
			 bool shrink_file_size);

#endif
