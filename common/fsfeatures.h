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

#ifndef __BTRFS_FSFEATURES_H__
#define __BTRFS_FSFEATURES_H__

#include "kerncompat.h"

#define BTRFS_MKFS_DEFAULT_NODE_SIZE SZ_16K
#define BTRFS_MKFS_DEFAULT_FEATURES 				\
		(BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF		\
		| BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA)

/*
 * Avoid multi-device features (RAID56), mixed block groups, and zoned mode
 */
#define BTRFS_CONVERT_ALLOWED_FEATURES				\
	(BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF			\
	| BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL			\
	| BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO			\
	| BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD			\
	| BTRFS_FEATURE_INCOMPAT_BIG_METADATA			\
	| BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF			\
	| BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA		\
	| BTRFS_FEATURE_INCOMPAT_NO_HOLES			\
	| BTRFS_FEATURE_INCOMPAT_ZONED)

#define BTRFS_FEATURE_LIST_ALL		(1ULL << 63)

#define BTRFS_RUNTIME_FEATURE_QUOTA		(1ULL << 0)
#define BTRFS_RUNTIME_FEATURE_FREE_SPACE_TREE	(1ULL << 1)

void btrfs_list_all_fs_features(u64 mask_disallowed);
void btrfs_list_all_runtime_features(u64 mask_disallowed);
char *btrfs_parse_fs_features(char *namelist, u64 *flags);
char *btrfs_parse_runtime_features(char *namelist, u64 *flags);
void btrfs_process_fs_features(u64 flags);
void btrfs_process_runtime_features(u64 flags);
void btrfs_parse_fs_features_to_string(char *buf, u64 flags);
void btrfs_parse_runtime_features_to_string(char *buf, u64 flags);
void print_kernel_version(FILE *stream, u32 version);
u32 get_running_kernel_version(void);
int btrfs_check_nodesize(u32 nodesize, u32 sectorsize, u64 features);
int btrfs_check_sectorsize(u32 sectorsize);
int btrfs_tree_search2_ioctl_supported(int fd);

#endif
