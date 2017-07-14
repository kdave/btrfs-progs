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
#include "ctree.h"

#define BTRFS_STRIPE_LEN	SZ_64K

struct btrfs_device {
	struct list_head dev_list;
	struct btrfs_root *dev_root;
	struct btrfs_fs_devices *fs_devices;

	u64 total_ios;

	int fd;

	int writeable;

	char *name;

	/* these are read off the super block, only in the progs */
	char *label;
	u64 total_devs;
	u64 super_bytes_used;

	u64 generation;

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

struct btrfs_fs_devices {
	u8 fsid[BTRFS_FSID_SIZE]; /* FS specific uuid */

	/* the device with this id has the most recent copy of the super */
	u64 latest_devid;
	u64 latest_trans;
	u64 lowest_devid;
	int latest_bdev;
	int lowest_bdev;
	struct list_head devices;
	struct list_head list;

	int seeding;
	struct btrfs_fs_devices *seed;
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

struct btrfs_map_stripe {
	struct btrfs_device *dev;

	/*
	 * Logical address of the stripe start.
	 * Caller should check if this logical is the desired map start.
	 * It's possible that the logical is smaller or larger than desired
	 * map range.
	 *
	 * For P/Q stipre, it will be BTRFS_RAID5_P_STRIPE
	 * and BTRFS_RAID6_Q_STRIPE.
	 */
	u64 logical;

	u64 physical;

	/* The length of the stripe */
	u64 length;
};

struct btrfs_map_block {
	/*
	 * The logical start of the whole map block.
	 * For RAID5/6 it will be the bytenr of the full stripe start,
	 * so it's possible that @start is smaller than desired map range
	 * start.
	 */
	u64 start;

	/*
	 * The logical length of the map block.
	 * For RAID5/6 it will be total data stripe size
	 */
	u64 length;

	/* Block group type */
	u64 type;

	/* Stripe length, for non-stripped mode, it will be 0 */
	u32 stripe_len;

	int num_stripes;
	struct btrfs_map_stripe stripes[];
};

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
	struct btrfs_block_group_cache *bg_cache;
	u64 bg_offset;

	bg_cache = btrfs_lookup_block_group(fs_info, start);
	/*
	 * Does not belong to block group, no boundary to cross
	 * although it's a bigger problem, but here we don't care.
	 */
	if (!bg_cache)
		return 0;
	bg_offset = start - bg_cache->key.objectid;

	return (bg_offset / BTRFS_STRIPE_LEN !=
		(bg_offset + len - 1) / BTRFS_STRIPE_LEN);
}

int __btrfs_map_block(struct btrfs_fs_info *fs_info, int rw,
		      u64 logical, u64 *length, u64 *type,
		      struct btrfs_multi_bio **multi_ret, int mirror_num,
		      u64 **raid_map);
int btrfs_map_block(struct btrfs_fs_info *fs_info, int rw,
		    u64 logical, u64 *length,
		    struct btrfs_multi_bio **multi_ret, int mirror_num,
		    u64 **raid_map_ret);

/*
 * TODO: Use this map_block_v2 to replace __btrfs_map_block()
 *
 * New btrfs_map_block(), unlike old one, each stripe will contain the
 * physical offset *AND* logical address.
 * So caller won't ever need to care about how the stripe/mirror is organized.
 * Which makes csum check quite easy.
 *
 * Only P/Q based profile needs to care their P/Q stripe.
 *
 * @map_ret example:
 * Raid1:
 * Map block: logical=128M len=10M type=RAID1 stripe_len=0 nr_stripes=2
 * Stripe 0: logical=128M physical=X len=10M dev=devid1
 * Stripe 1: logical=128M physical=Y len=10M dev=devid2
 *
 * Raid10:
 * Map block: logical=64K len=128K type=RAID10 stripe_len=64K nr_stripes=4
 * Stripe 0: logical=64K physical=X len=64K dev=devid1
 * Stripe 1: logical=64K physical=Y len=64K dev=devid2
 * Stripe 2: logical=128K physical=Z len=64K dev=devid3
 * Stripe 3: logical=128K physical=W len=64K dev=devid4
 *
 * Raid6:
 * Map block: logical=64K len=128K type=RAID6 stripe_len=64K nr_stripes=4
 * Stripe 0: logical=64K physical=X len=64K dev=devid1
 * Stripe 1: logical=128K physical=Y len=64K dev=devid2
 * Stripe 2: logical=RAID5_P physical=Z len=64K dev=devid3
 * Stripe 3: logical=RAID6_Q physical=W len=64K dev=devid4
 */
int __btrfs_map_block_v2(struct btrfs_fs_info *fs_info, int rw, u64 logical,
			 u64 length, struct btrfs_map_block **map_ret);
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
		     u64 chunk_start, u64 physical, u64 devid,
		     u64 **logical, int *naddrs, int *stripe_len);
int btrfs_read_sys_array(struct btrfs_fs_info *fs_info);
int btrfs_read_chunk_tree(struct btrfs_fs_info *fs_info);
int btrfs_alloc_chunk(struct btrfs_trans_handle *trans,
		      struct btrfs_fs_info *fs_info, u64 *start,
		      u64 *num_bytes, u64 type);
int btrfs_alloc_data_chunk(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *fs_info, u64 *start,
			   u64 num_bytes, u64 type, int convert);
int btrfs_open_devices(struct btrfs_fs_devices *fs_devices,
		       int flags);
int btrfs_close_devices(struct btrfs_fs_devices *fs_devices);
void btrfs_close_all_devices(void);
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
#endif
