/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
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

#ifndef __BTRFS_VOLUMES_H__
#define __BTRFS_VOLUMES_H__

#include "kerncompat.h"
#include "kernel-shared/ctree.h"

#define BTRFS_STRIPE_LEN	SZ_64K

struct btrfs_device {
	struct list_head dev_list;
	struct btrfs_root *dev_root;
	struct btrfs_fs_devices *fs_devices;
	struct btrfs_fs_info *fs_info;

	u64 total_ios;

	int fd;

	int writeable;

	char *name;

	/* these are read off the super block, only in the progs */
	char *label;
	u64 total_devs;
	u64 super_bytes_used;

	u64 generation;

	struct btrfs_zoned_device_info *zone_info;

	/* the internal btrfs device id */
	u64 devid;

	/* size of the device */
	u64 total_bytes;

	/* bytes used */
	u64 bytes_used;

	/* optimal io alignment for this device */
	u32 io_align;

	/* optimal io width for this device */
	u32 io_width;

	/* minimal io size for this device */
	u32 sector_size;

	/* type and info about this device */
	u64 type;

	/* physical drive uuid (or lvm uuid) */
	u8 uuid[BTRFS_UUID_SIZE];
};

enum btrfs_chunk_allocation_policy {
	BTRFS_CHUNK_ALLOC_REGULAR,
	BTRFS_CHUNK_ALLOC_ZONED,
};

struct btrfs_fs_devices {
	u8 fsid[BTRFS_FSID_SIZE]; /* FS specific uuid */
	u8 metadata_uuid[BTRFS_FSID_SIZE]; /* FS specific uuid */

	/* the device with this id has the most recent copy of the super */
	u64 latest_devid;
	u64 latest_trans;
	u64 lowest_devid;

	u64 total_rw_bytes;

	int latest_bdev;
	int lowest_bdev;
	struct list_head devices;
	struct list_head list;

	int seeding;
	struct btrfs_fs_devices *seed;

	enum btrfs_chunk_allocation_policy chunk_alloc_policy;
};

struct btrfs_bio_stripe {
	struct btrfs_device *dev;
	u64 physical;
};

struct btrfs_multi_bio {
	int error;
	int num_stripes;
	struct btrfs_bio_stripe stripes[];
};

struct map_lookup {
	struct cache_extent ce;
	u64 type;
	int io_align;
	int io_width;
	int stripe_len;
	int sector_size;
	int num_stripes;
	int sub_stripes;
	struct btrfs_bio_stripe stripes[];
};

struct btrfs_raid_attr {
	int sub_stripes;	/* sub_stripes info for map */
	int dev_stripes;	/* stripes per dev */
	int devs_max;		/* max devs to use */
	int devs_min;		/* min devs needed */
	int tolerated_failures; /* max tolerated fail devs */
	int devs_increment;	/* ndevs has to be a multiple of this */
	int ncopies;		/* how many copies to data has */
	int nparity;		/* number of stripes worth of bytes to store
				 * parity information */
	int mindev_error;	/* error code if min devs requisite is unmet */
	const char raid_name[8]; /* name of the raid */
	u64 bg_flag;		/* block group flag of the raid */
};

extern const struct btrfs_raid_attr btrfs_raid_array[BTRFS_NR_RAID_TYPES];

static inline enum btrfs_raid_types btrfs_bg_flags_to_raid_index(u64 flags)
{
	if (flags & BTRFS_BLOCK_GROUP_RAID10)
		return BTRFS_RAID_RAID10;
	else if (flags & BTRFS_BLOCK_GROUP_RAID1)
		return BTRFS_RAID_RAID1;
	else if (flags & BTRFS_BLOCK_GROUP_RAID1C3)
		return BTRFS_RAID_RAID1C3;
	else if (flags & BTRFS_BLOCK_GROUP_RAID1C4)
		return BTRFS_RAID_RAID1C4;
	else if (flags & BTRFS_BLOCK_GROUP_DUP)
		return BTRFS_RAID_DUP;
	else if (flags & BTRFS_BLOCK_GROUP_RAID0)
		return BTRFS_RAID_RAID0;
	else if (flags & BTRFS_BLOCK_GROUP_RAID5)
		return BTRFS_RAID_RAID5;
	else if (flags & BTRFS_BLOCK_GROUP_RAID6)
		return BTRFS_RAID_RAID6;

	return BTRFS_RAID_SINGLE; /* BTRFS_BLOCK_GROUP_SINGLE */
}

#define btrfs_multi_bio_size(n) (sizeof(struct btrfs_multi_bio) + \
			    (sizeof(struct btrfs_bio_stripe) * (n)))
#define btrfs_map_lookup_size(n) (sizeof(struct map_lookup) + \
				 (sizeof(struct btrfs_bio_stripe) * (n)))

/*
 * Restriper's general type filter
 */
#define BTRFS_BALANCE_DATA		(1ULL << 0)
#define BTRFS_BALANCE_SYSTEM		(1ULL << 1)
#define BTRFS_BALANCE_METADATA		(1ULL << 2)

#define BTRFS_BALANCE_TYPE_MASK		(BTRFS_BALANCE_DATA |	    \
					 BTRFS_BALANCE_SYSTEM |	    \
					 BTRFS_BALANCE_METADATA)

#define BTRFS_BALANCE_FORCE		(1ULL << 3)
#define BTRFS_BALANCE_RESUME		(1ULL << 4)

/*
 * Balance filters
 */
#define BTRFS_BALANCE_ARGS_PROFILES	(1ULL << 0)
#define BTRFS_BALANCE_ARGS_USAGE	(1ULL << 1)
#define BTRFS_BALANCE_ARGS_DEVID	(1ULL << 2)
#define BTRFS_BALANCE_ARGS_DRANGE	(1ULL << 3)
#define BTRFS_BALANCE_ARGS_VRANGE	(1ULL << 4)
#define BTRFS_BALANCE_ARGS_LIMIT	(1ULL << 5)
#define BTRFS_BALANCE_ARGS_LIMIT_RANGE	(1ULL << 6)
#define BTRFS_BALANCE_ARGS_STRIPES_RANGE (1ULL << 7)
#define BTRFS_BALANCE_ARGS_USAGE_RANGE	(1ULL << 10)

/*
 * Profile changing flags.  When SOFT is set we won't relocate chunk if
 * it already has the target profile (even though it may be
 * half-filled).
 */
#define BTRFS_BALANCE_ARGS_CONVERT	(1ULL << 8)
#define BTRFS_BALANCE_ARGS_SOFT		(1ULL << 9)

#define BTRFS_RAID5_P_STRIPE ((u64)-2)
#define BTRFS_RAID6_Q_STRIPE ((u64)-1)

/*
 * Check if the given range cross stripes.
 * To ensure kernel scrub won't causing bug on with METADATA in mixed
 * block group
 *
 * Return 1 if the range crosses STRIPE boundary
 * Return 0 if the range doesn't cross STRIPE boundary or it
 * doesn't belong to any block group (no boundary to cross)
 */
static inline int check_crossing_stripes(struct btrfs_fs_info *fs_info,
					 u64 start, u64 len)
{
	struct btrfs_block_group *bg_cache;
	u64 bg_offset;

	bg_cache = btrfs_lookup_block_group(fs_info, start);
	/*
	 * Does not belong to block group, no boundary to cross
	 * although it's a bigger problem, but here we don't care.
	 */
	if (!bg_cache)
		return 0;
	bg_offset = start - bg_cache->start;

	return (bg_offset / BTRFS_STRIPE_LEN !=
		(bg_offset + len - 1) / BTRFS_STRIPE_LEN);
}

static inline u64 calc_stripe_length(u64 type, u64 length, int num_stripes)
{
	u64 stripe_size;

	if (type & BTRFS_BLOCK_GROUP_RAID0) {
		stripe_size = length;
		stripe_size /= num_stripes;
	} else if (type & BTRFS_BLOCK_GROUP_RAID10) {
		stripe_size = length * 2;
		stripe_size /= num_stripes;
	} else if (type & BTRFS_BLOCK_GROUP_RAID5) {
		stripe_size = length;
		stripe_size /= (num_stripes - 1);
	} else if (type & BTRFS_BLOCK_GROUP_RAID6) {
		stripe_size = length;
		stripe_size /= (num_stripes - 2);
	} else {
		stripe_size = length;
	}
	return stripe_size;
}

int __btrfs_map_block(struct btrfs_fs_info *fs_info, int rw,
		      u64 logical, u64 *length, u64 *type,
		      struct btrfs_multi_bio **multi_ret, int mirror_num,
		      u64 **raid_map);
int btrfs_map_block(struct btrfs_fs_info *fs_info, int rw,
		    u64 logical, u64 *length,
		    struct btrfs_multi_bio **multi_ret, int mirror_num,
		    u64 **raid_map_ret);
int btrfs_next_bg(struct btrfs_fs_info *map_tree, u64 *logical,
		     u64 *size, u64 type);
static inline int btrfs_next_bg_metadata(struct btrfs_fs_info *fs_info,
					 u64 *logical, u64 *size)
{
	return btrfs_next_bg(fs_info, logical, size,
			BTRFS_BLOCK_GROUP_METADATA);
}
static inline int btrfs_next_bg_system(struct btrfs_fs_info *fs_info,
				       u64 *logical, u64 *size)
{
	return btrfs_next_bg(fs_info, logical, size,
			BTRFS_BLOCK_GROUP_SYSTEM);
}
int btrfs_rmap_block(struct btrfs_fs_info *fs_info,
		     u64 chunk_start, u64 physical, u64 **logical,
		     int *naddrs, int *stripe_len);
int btrfs_read_sys_array(struct btrfs_fs_info *fs_info);
int btrfs_read_chunk_tree(struct btrfs_fs_info *fs_info);
int btrfs_alloc_chunk(struct btrfs_trans_handle *trans,
		      struct btrfs_fs_info *fs_info, u64 *start,
		      u64 *num_bytes, u64 type);
int btrfs_alloc_data_chunk(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *fs_info, u64 *start, u64 num_bytes);
int btrfs_open_devices(struct btrfs_fs_info *fs_info,
		       struct btrfs_fs_devices *fs_devices, int flags);
int btrfs_close_devices(struct btrfs_fs_devices *fs_devices);
void btrfs_close_all_devices(void);
int btrfs_insert_dev_extent(struct btrfs_trans_handle *trans,
			    struct btrfs_device *device,
			    u64 chunk_offset, u64 num_bytes, u64 start);
int btrfs_add_device(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info,
		     struct btrfs_device *device);
int btrfs_update_device(struct btrfs_trans_handle *trans,
			struct btrfs_device *device);
int btrfs_scan_one_device(int fd, const char *path,
			  struct btrfs_fs_devices **fs_devices_ret,
			  u64 *total_devs, u64 super_offset, unsigned sbflags);
int btrfs_num_copies(struct btrfs_fs_info *fs_info, u64 logical, u64 len);
struct list_head *btrfs_scanned_uuids(void);
int btrfs_add_system_chunk(struct btrfs_fs_info *fs_info, struct btrfs_key *key,
			   struct btrfs_chunk *chunk, int item_size);
int btrfs_chunk_readonly(struct btrfs_fs_info *fs_info, u64 chunk_offset);
struct btrfs_device *
btrfs_find_device_by_devid(struct btrfs_fs_devices *fs_devices,
			   u64 devid, int instance);
struct btrfs_device *btrfs_find_device(struct btrfs_fs_info *fs_info, u64 devid,
				       u8 *uuid, u8 *fsid);
int write_raid56_with_parity(struct btrfs_fs_info *info,
			     struct extent_buffer *eb,
			     struct btrfs_multi_bio *multi,
			     u64 stripe_len, u64 *raid_map);
int btrfs_check_chunk_valid(struct btrfs_fs_info *fs_info,
			    struct extent_buffer *leaf,
			    struct btrfs_chunk *chunk,
			    int slot, u64 logical);
u64 btrfs_stripe_length(struct btrfs_fs_info *fs_info,
			struct extent_buffer *leaf,
			struct btrfs_chunk *chunk);
int btrfs_fix_device_size(struct btrfs_fs_info *fs_info,
			  struct btrfs_device *device);
int btrfs_fix_super_size(struct btrfs_fs_info *fs_info);
int btrfs_fix_device_and_super_size(struct btrfs_fs_info *fs_info);
#endif
