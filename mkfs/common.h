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
 * definitions.
 */

#ifndef __BTRFS_MKFS_COMMON_H__
#define __BTRFS_MKFS_COMMON_H__

#include "kerncompat.h"
#include "common/defs.h"

#define BTRFS_MKFS_SYSTEM_GROUP_SIZE SZ_4M
#define BTRFS_MKFS_SMALL_VOLUME_SIZE SZ_1G

/*
 * Default settings for block group types
 */
#define BTRFS_MKFS_DEFAULT_DATA_ONE_DEVICE	0	/* SINGLE */
#define BTRFS_MKFS_DEFAULT_META_ONE_DEVICE	BTRFS_BLOCK_GROUP_DUP

#define BTRFS_MKFS_DEFAULT_DATA_MULTI_DEVICE	0	/* SINGLE */
#define BTRFS_MKFS_DEFAULT_META_MULTI_DEVICE	BTRFS_BLOCK_GROUP_RAID1

struct btrfs_trans_handle;
struct btrfs_root;

/*
 * Tree root blocks created during mkfs
 */
enum btrfs_mkfs_block {
	MKFS_ROOT_TREE,
	MKFS_EXTENT_TREE,
	MKFS_CHUNK_TREE,
	MKFS_DEV_TREE,
	MKFS_FS_TREE,
	MKFS_CSUM_TREE,
	MKFS_FREE_SPACE_TREE,
	MKFS_BLOCK_GROUP_TREE,

	/* MKFS_BLOCK_COUNT should be the max blocks we can have at mkfs time. */
	MKFS_BLOCK_COUNT
};

static const enum btrfs_mkfs_block default_blocks[] = {
	MKFS_ROOT_TREE,
	MKFS_EXTENT_TREE,
	MKFS_CHUNK_TREE,
	MKFS_DEV_TREE,
	MKFS_FS_TREE,
	MKFS_CSUM_TREE,
	MKFS_FREE_SPACE_TREE,
};

struct btrfs_mkfs_config {
	/* Label of the new filesystem */
	const char *label;
	/* Block sizes */
	u32 nodesize;
	u32 sectorsize;
	u32 stripesize;
	u32 leaf_data_size;
	/* Bitfield of incompat features, BTRFS_FEATURE_INCOMPAT_* */
	u64 features;
	/* Bitfield of BTRFS_RUNTIME_FEATURE_* */
	u64 runtime_features;
	/* Size of the filesystem in bytes */
	u64 num_bytes;
	/* checksum algorithm to use */
	enum btrfs_csum_type csum_type;
	u64 zone_size;

	/* Output fields, set during creation */

	/* Logical addresses of superblock [0] and other tree roots */
	u64 blocks[MKFS_BLOCK_COUNT + 1];
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE];
	char chunk_uuid[BTRFS_UUID_UNPARSED_SIZE];

	/* Superblock offset after make_btrfs */
	u64 super_bytenr;
};

int make_btrfs(int fd, struct btrfs_mkfs_config *cfg);
int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid);
u64 btrfs_min_dev_size(u32 nodesize, int mixed, u64 meta_profile,
		       u64 data_profile);
int test_minimum_size(const char *file, u64 min_dev_size);
int is_vol_small(const char *file);
int test_num_disk_vs_raid(u64 metadata_profile, u64 data_profile,
	u64 dev_cnt, int mixed, int ssd);
int test_status_for_mkfs(const char *file, bool force_overwrite);
int test_dev_for_mkfs(const char *file, int force_overwrite);

#endif
