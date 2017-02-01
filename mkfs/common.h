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
 * Defines and function declarations for users of the mkfs API, no internal
 * defintions.
 */

#ifndef __BTRFS_MKFS_COMMON_H__
#define __BTRFS_MKFS_COMMON_H__

#include "kerncompat.h"
#include "common-defs.h"

#define BTRFS_MKFS_SYSTEM_GROUP_SIZE SZ_4M
#define BTRFS_MKFS_SMALL_VOLUME_SIZE SZ_1G

struct btrfs_mkfs_config {
	/* Label of the new filesystem */
	char *label;
	/* Blck sizes */
	u32 nodesize;
	u32 sectorsize;
	u32 stripesize;
	/* Bitfield of incompat features, BTRFS_FEATURE_INCOMPAT_* */
	u64 features;
	/* Size of the filesystem in bytes */
	u64 num_bytes;

	/* Output fields, set during creation */

	/* Logical addresses of superblock [0] and other tree roots */
	u64 blocks[8];
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE];
	char chunk_uuid[BTRFS_UUID_UNPARSED_SIZE];

	/* Superblock offset after make_btrfs */
	u64 super_bytenr;
};

int make_btrfs(int fd, struct btrfs_mkfs_config *cfg);
u64 btrfs_min_dev_size(u32 nodesize);
u64 btrfs_min_global_blk_rsv_size(u32 nodesize);
int test_minimum_size(const char *file, u32 nodesize);
int is_vol_small(const char *file);
int test_num_disk_vs_raid(u64 metadata_profile, u64 data_profile,
	u64 dev_cnt, int mixed, int ssd);
int test_dev_for_mkfs(const char *file, int force_overwrite);

#endif
