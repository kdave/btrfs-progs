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
#include <stdio.h>
#include "kernel-lib/sizes.h"

#define BTRFS_MKFS_DEFAULT_NODE_SIZE SZ_16K

/*
 * Since one feature can set at least one bit in either
 * incompat/compat_or/runtime flags, all mkfs features users should
 * use this structure to parse the features.
 */
struct btrfs_mkfs_features {
	u64 incompat_flags;
	u64 compat_ro_flags;
	u64 runtime_flags;
};

#define BTRFS_FEATURE_RUNTIME_QUOTA		(1ULL << 0)
#define BTRFS_FEATURE_RUNTIME_LIST_ALL		(1ULL << 1)

/*
 * Such buffer size should be able to contain all feature string, with extra
 * ", " for each feature.
 */
#define BTRFS_FEATURE_STRING_BUF_SIZE		(512)

static const struct btrfs_mkfs_features btrfs_mkfs_default_features = {
	.compat_ro_flags = BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |
			   BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID,
	.incompat_flags	 = BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF |
			   BTRFS_FEATURE_INCOMPAT_NO_HOLES |
			   BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA,
};

/*
 * Avoid multi-device features (RAID56 and RAID1C34), mixed bgs, and zoned
 * mode for btrfs-convert, as all supported fses are single device fses.
 *
 * Features like compression is disabled in btrfs-convert by default, as
 * data is reusing the old data from the source fs.
 * Corresponding flag will be set when the first compression write happens.
 */
static const struct btrfs_mkfs_features btrfs_convert_allowed_features = {
	.compat_ro_flags = BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |
			   BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID |
			   BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE,
	.incompat_flags  = BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF |
			   BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL |
			   BTRFS_FEATURE_INCOMPAT_BIG_METADATA |
			   BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF |
			   BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA |
			   BTRFS_FEATURE_INCOMPAT_NO_HOLES,
	.runtime_flags   = BTRFS_FEATURE_RUNTIME_QUOTA,
};

void btrfs_list_all_fs_features(const struct btrfs_mkfs_features *allowed);
void btrfs_list_all_runtime_features(const struct btrfs_mkfs_features *allowed);
char *btrfs_parse_fs_features(char *namelist,
		struct btrfs_mkfs_features *features);
char *btrfs_parse_runtime_features(char *namelist,
		struct btrfs_mkfs_features *features);
void btrfs_process_fs_features(struct btrfs_mkfs_features *features);
void btrfs_process_runtime_features(struct btrfs_mkfs_features *features);
void btrfs_parse_fs_features_to_string(char *buf,
		const struct btrfs_mkfs_features *features);
void btrfs_parse_runtime_features_to_string(char *buf,
		const struct btrfs_mkfs_features *features);
void print_kernel_version(FILE *stream, u32 version);
u32 get_running_kernel_version(void);
int btrfs_check_nodesize(u32 nodesize, u32 sectorsize,
			 struct btrfs_mkfs_features *features);
int btrfs_check_sectorsize(u32 sectorsize);
int btrfs_check_features(const struct btrfs_mkfs_features *features,
			 const struct btrfs_mkfs_features *allowed);
int btrfs_tree_search2_ioctl_supported(int fd);
void btrfs_assert_feature_buf_size(void);

#endif
