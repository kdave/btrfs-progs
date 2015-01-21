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

#define BTRFS_STRIPE_LEN	(64 * 1024)

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

/*
 * Profile changing flags.  When SOFT is set we won't relocate chunk if
 * it already has the target profile (even though it may be
 * half-filled).
 */
#define BTRFS_BALANCE_ARGS_CONVERT	(1ULL << 8)
#define BTRFS_BALANCE_ARGS_SOFT		(1ULL << 9)

#define BTRFS_RAID5_P_STRIPE ((u64)-2)
#define BTRFS_RAID6_Q_STRIPE ((u64)-1)


int __btrfs_map_block(struct btrfs_mapping_tree *map_tree, int rw,
		      u64 logical, u64 *length, u64 *type,
		      struct btrfs_multi_bio **multi_ret, int mirror_num,
		      u64 **raid_map);
int btrfs_map_block(struct btrfs_mapping_tree *map_tree, int rw,
		    u64 logical, u64 *length,
		    struct btrfs_multi_bio **multi_ret, int mirror_num,
		    u64 **raid_map_ret);
int btrfs_next_metadata(struct btrfs_mapping_tree *map_tree, u64 *logical,
			u64 *size);
int btrfs_rmap_block(struct btrfs_mapping_tree *map_tree,
		     u64 chunk_start, u64 physical, u64 devid,
		     u64 **logical, int *naddrs, int *stripe_len);
int btrfs_read_sys_array(struct btrfs_root *root);
int btrfs_read_chunk_tree(struct btrfs_root *root);
int btrfs_alloc_chunk(struct btrfs_trans_handle *trans,
		      struct btrfs_root *extent_root, u64 *start,
		      u64 *num_bytes, u64 type);
int btrfs_alloc_data_chunk(struct btrfs_trans_handle *trans,
			   struct btrfs_root *extent_root, u64 *start,
			   u64 num_bytes, u64 type);
int btrfs_read_super_device(struct btrfs_root *root, struct extent_buffer *buf);
int btrfs_add_device(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root,
		     struct btrfs_device *device);
int btrfs_open_devices(struct btrfs_fs_devices *fs_devices,
		       int flags);
int btrfs_close_devices(struct btrfs_fs_devices *fs_devices);
int btrfs_add_device(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root,
		     struct btrfs_device *device);
int btrfs_update_device(struct btrfs_trans_handle *trans,
			struct btrfs_device *device);
int btrfs_scan_one_device(int fd, const char *path,
			  struct btrfs_fs_devices **fs_devices_ret,
			  u64 *total_devs, u64 super_offset, int super_recover);
int btrfs_num_copies(struct btrfs_mapping_tree *map_tree, u64 logical, u64 len);
struct list_head *btrfs_scanned_uuids(void);
int btrfs_add_system_chunk(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root, struct btrfs_key *key,
			   struct btrfs_chunk *chunk, int item_size);
int btrfs_chunk_readonly(struct btrfs_root *root, u64 chunk_offset);
struct btrfs_device *
btrfs_find_device_by_devid(struct btrfs_fs_devices *fs_devices,
			   u64 devid, int instance);
struct btrfs_device *btrfs_find_device(struct btrfs_root *root, u64 devid,
				       u8 *uuid, u8 *fsid);
int write_raid56_with_parity(struct btrfs_fs_info *info,
			     struct extent_buffer *eb,
			     struct btrfs_multi_bio *multi,
			     u64 stripe_len, u64 *raid_map);
#endif
