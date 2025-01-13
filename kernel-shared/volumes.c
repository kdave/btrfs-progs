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

#include "kerncompat.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include "kernel-lib/raid56.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/tree-checker.h"
#include "kernel-shared/zoned.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/messages.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/device-utils.h"

const struct btrfs_raid_attr btrfs_raid_array[BTRFS_NR_RAID_TYPES] = {
	[BTRFS_RAID_RAID10] = {
		.sub_stripes	= 2,
		.dev_stripes	= 1,
		.devs_max	= 0,	/* 0 == as many as possible */
		.devs_min	= 2,
		.tolerated_failures = 1,
		.devs_increment	= 2,
		.ncopies	= 2,
		.nparity        = 0,
		.lower_name	= "raid10",
		.upper_name	= "RAID10",
		.bg_flag	= BTRFS_BLOCK_GROUP_RAID10,
		.mindev_error	= BTRFS_ERROR_DEV_RAID10_MIN_NOT_MET,
	},
	[BTRFS_RAID_RAID1] = {
		.sub_stripes	= 1,
		.dev_stripes	= 1,
		.devs_max	= 2,
		.devs_min	= 2,
		.tolerated_failures = 1,
		.devs_increment	= 2,
		.ncopies	= 2,
		.nparity        = 0,
		.lower_name	= "raid1",
		.upper_name	= "RAID1",
		.bg_flag	= BTRFS_BLOCK_GROUP_RAID1,
		.mindev_error	= BTRFS_ERROR_DEV_RAID1_MIN_NOT_MET,
	},
	[BTRFS_RAID_RAID1C3] = {
		.sub_stripes	= 1,
		.dev_stripes	= 1,
		.devs_max	= 3,
		.devs_min	= 3,
		.tolerated_failures = 2,
		.devs_increment	= 3,
		.ncopies	= 3,
		.nparity        = 0,
		.lower_name	= "raid1c3",
		.upper_name	= "RAID1C3",
		.bg_flag	= BTRFS_BLOCK_GROUP_RAID1C3,
		.mindev_error	= BTRFS_ERROR_DEV_RAID1C3_MIN_NOT_MET,
	},
	[BTRFS_RAID_RAID1C4] = {
		.sub_stripes	= 1,
		.dev_stripes	= 1,
		.devs_max	= 4,
		.devs_min	= 4,
		.tolerated_failures = 3,
		.devs_increment	= 4,
		.ncopies	= 4,
		.nparity        = 0,
		.lower_name	= "raid1c4",
		.upper_name	= "RAID1C4",
		.bg_flag	= BTRFS_BLOCK_GROUP_RAID1C4,
		.mindev_error	= BTRFS_ERROR_DEV_RAID1C4_MIN_NOT_MET,
	},
	[BTRFS_RAID_DUP] = {
		.sub_stripes	= 1,
		.dev_stripes	= 2,
		.devs_max	= 1,
		.devs_min	= 1,
		.tolerated_failures = 0,
		.devs_increment	= 1,
		.ncopies	= 2,
		.nparity        = 0,
		.lower_name	= "dup",
		.upper_name	= "DUP",
		.bg_flag	= BTRFS_BLOCK_GROUP_DUP,
		.mindev_error	= 0,
	},
	[BTRFS_RAID_RAID0] = {
		.sub_stripes	= 1,
		.dev_stripes	= 1,
		.devs_max	= 0,
		.devs_min	= 1,
		.tolerated_failures = 0,
		.devs_increment	= 1,
		.ncopies	= 1,
		.nparity        = 0,
		.lower_name	= "raid0",
		.upper_name	= "RAID0",
		.bg_flag	= BTRFS_BLOCK_GROUP_RAID0,
		.mindev_error	= 0,
	},
	[BTRFS_RAID_SINGLE] = {
		.sub_stripes	= 1,
		.dev_stripes	= 1,
		.devs_max	= 1,
		.devs_min	= 1,
		.tolerated_failures = 0,
		.devs_increment	= 1,
		.ncopies	= 1,
		.nparity        = 0,
		.lower_name	= "single",
		/*
		 * For historical reasons the single profile is lower case, this
		 * may change some day.
		 */
		.upper_name	= "single",
		.bg_flag	= 0,
		.mindev_error	= 0,
	},
	[BTRFS_RAID_RAID5] = {
		.sub_stripes	= 1,
		.dev_stripes	= 1,
		.devs_max	= 0,
		.devs_min	= 2,
		.tolerated_failures = 1,
		.devs_increment	= 1,
		.ncopies	= 1,
		.nparity        = 1,
		.lower_name	= "raid5",
		.upper_name	= "RAID5",
		.bg_flag	= BTRFS_BLOCK_GROUP_RAID5,
		.mindev_error	= BTRFS_ERROR_DEV_RAID5_MIN_NOT_MET,
	},
	[BTRFS_RAID_RAID6] = {
		.sub_stripes	= 1,
		.dev_stripes	= 1,
		.devs_max	= 0,
		.devs_min	= 3,
		.tolerated_failures = 2,
		.devs_increment	= 1,
		.ncopies	= 1,
		.nparity        = 2,
		.lower_name	= "raid6",
		.upper_name	= "RAID6",
		.bg_flag	= BTRFS_BLOCK_GROUP_RAID6,
		.mindev_error	= BTRFS_ERROR_DEV_RAID6_MIN_NOT_MET,
	},
};

struct alloc_chunk_ctl {
	u64 start;
	u64 type;
	int num_stripes;
	int max_stripes;
	int min_stripes;
	int sub_stripes;
	u64 stripe_size;
	u64 min_stripe_size;
	u64 num_bytes;
	u64 max_chunk_size;
	int total_devs;
	u64 dev_offset;
	int nparity;
	int ncopies;
};

struct stripe {
	struct btrfs_device *dev;
	u64 physical;
};

/*
 * Convert block group flags (BTRFS_BLOCK_GROUP_*) to btrfs_raid_types, which
 * can be used as index to access btrfs_raid_array[].
 */
enum btrfs_raid_types btrfs_bg_flags_to_raid_index(u64 flags)
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

const char *btrfs_bg_type_to_raid_name(u64 flags)
{
	const int index = btrfs_bg_flags_to_raid_index(flags);

	if (index >= BTRFS_NR_RAID_TYPES)
		return NULL;

	return btrfs_raid_array[index].upper_name;
}

int btrfs_bg_type_to_tolerated_failures(u64 flags)
{
	const int index = btrfs_bg_flags_to_raid_index(flags);

	return btrfs_raid_array[index].tolerated_failures;
}

int btrfs_bg_type_to_devs_min(u64 flags)
{
	const int index = btrfs_bg_flags_to_raid_index(flags);

	return btrfs_raid_array[index].devs_min;
}

int btrfs_bg_type_to_ncopies(u64 flags)
{
	const int index = btrfs_bg_flags_to_raid_index(flags);

	return btrfs_raid_array[index].ncopies;
}

int btrfs_bg_type_to_nparity(u64 flags)
{
	const int index = btrfs_bg_flags_to_raid_index(flags);

	return btrfs_raid_array[index].nparity;
}

int btrfs_bg_type_to_sub_stripes(u64 flags)
{
	const int index = btrfs_bg_flags_to_raid_index(flags);

	return btrfs_raid_array[index].sub_stripes;
}

/*
 * Number of stripes is not fixed and depends on the number of devices,
 * utilizing as many as possible (RAID0/RAID10/RAID5/RAID6/...).
 */
bool btrfs_bg_type_is_stripey(u64 flags)
{
	const int index = btrfs_bg_flags_to_raid_index(flags);

	return btrfs_raid_array[index].devs_max == 0;
}

u64 btrfs_bg_flags_for_device_num(int number)
{
	int i;
	u64 ret = 0;

	for (i = 0; i < ARRAY_SIZE(btrfs_raid_array); i++) {
		if (number >= btrfs_raid_array[i].devs_min)
			ret |= btrfs_raid_array[i].bg_flag;
	}

	return ret;
}

static inline int nr_data_stripes(struct map_lookup *map)
{
	return map->num_stripes - btrfs_bg_type_to_nparity(map->type);
}

#define is_parity_stripe(x) ( ((x) == BTRFS_RAID5_P_STRIPE) || ((x) == BTRFS_RAID6_Q_STRIPE) )

static LIST_HEAD(fs_uuids);

/*
 * Find a device specified by @devid or @uuid in the list of @fs_devices, or
 * return NULL.
 *
 * If devid and uuid are both specified, the match must be exact, otherwise
 * only devid is used.
 */
static struct btrfs_device *find_device(struct btrfs_fs_devices *fs_devices,
		u64 devid, u8 *uuid)
{
	struct list_head *head = &fs_devices->devices;
	struct btrfs_device *dev;

	list_for_each_entry(dev, head, dev_list) {
		if (dev->devid == devid &&
		    (!uuid || !memcmp(dev->uuid, uuid, BTRFS_UUID_SIZE))) {
			return dev;
		}
	}
	return NULL;
}

static struct btrfs_fs_devices *find_fsid(u8 *fsid, u8 *metadata_uuid)
{
	struct btrfs_fs_devices *fs_devices;

	list_for_each_entry(fs_devices, &fs_uuids, fs_list) {
		if (metadata_uuid && (memcmp(fsid, fs_devices->fsid,
					     BTRFS_FSID_SIZE) == 0) &&
		    (memcmp(metadata_uuid, fs_devices->metadata_uuid,
			    BTRFS_FSID_SIZE) == 0)) {
			return fs_devices;
		} else if (memcmp(fsid, fs_devices->fsid, BTRFS_FSID_SIZE) == 0){
			return fs_devices;
		}
	}
	return NULL;
}

static u8 *btrfs_sb_fsid_ptr(struct btrfs_super_block *sb)
{
	if (btrfs_super_incompat_flags(sb) & BTRFS_FEATURE_INCOMPAT_METADATA_UUID)
		return sb->metadata_uuid;
	else
		return sb->fsid;
}

static bool match_fsid_fs_devices(const struct btrfs_fs_devices *fs_devices,
				  const u8 *fsid, const u8 *metadata_fsid)
{
	if (memcmp(fsid, fs_devices->fsid, BTRFS_FSID_SIZE) != 0)
		return false;

	if (!metadata_fsid)
		return true;

	if (memcmp(metadata_fsid, fs_devices->metadata_uuid, BTRFS_FSID_SIZE) != 0)
		return false;

	return true;
}

/*
 * First check if the metadata_uuid is different from the fsid in the given
 * fs_devices. Then check if the given fsid is the same as the metadata_uuid
 * in the fs_devices. If it is, return true; otherwise, return false.
 */
static inline bool check_fsid_changed(const struct btrfs_fs_devices *fs_devices,
				      const u8 *fsid)
{
	return memcmp(fs_devices->fsid, fs_devices->metadata_uuid,
		      BTRFS_FSID_SIZE) != 0 &&
	       memcmp(fs_devices->metadata_uuid, fsid, BTRFS_FSID_SIZE) == 0;
}

static struct btrfs_fs_devices *find_fsid_with_metadata_uuid(
				struct btrfs_super_block *disk_super)
{
	struct btrfs_fs_devices *fs_devices;

	/*
	 * Handle scanned device having completed its fsid change but belonging
	 * to a fs_devices that was created by first scanning a device which
	 * didn't have its fsid/metadata_uuid changed at all and the
	 * CHANGING_FSID_V2 flag set.
	 */
	list_for_each_entry(fs_devices, &fs_uuids, fs_list) {
		if (!fs_devices->changing_fsid)
			continue;

		if (match_fsid_fs_devices(fs_devices, disk_super->metadata_uuid,
					  fs_devices->fsid))
			return fs_devices;
	}

	/*
	 * Handle scanned device having completed its fsid change but belonging
	 * to a fs_devices that was created by a device that has an outdated
	 * pair of fsid/metadata_uuid and CHANGING_FSID_V2 flag set.
	 */
	list_for_each_entry(fs_devices, &fs_uuids, fs_list) {
		if (!fs_devices->changing_fsid)
			continue;

		if (check_fsid_changed(fs_devices, disk_super->metadata_uuid))
			return fs_devices;
	}

	return find_fsid(disk_super->fsid, disk_super->metadata_uuid);
}

/*
 * Handle scanned device having its CHANGING_FSID_V2 flag set and the fs_devices
 * being created with a disk that has already completed its fsid change. Such
 * disk can belong to an fs which has its fsid changed or to one which doesn't.
 * Handle both cases here.
 */
static struct btrfs_fs_devices *find_fsid_inprogress(struct btrfs_super_block *disk_super)
{
	struct btrfs_fs_devices *fs_devices;

	list_for_each_entry(fs_devices, &fs_uuids, fs_list) {
		if (fs_devices->changing_fsid)
			continue;

		if (check_fsid_changed(fs_devices,  disk_super->fsid))
			return fs_devices;
	}

	return find_fsid(disk_super->fsid, NULL);
}

static struct btrfs_fs_devices *find_fsid_changed(struct btrfs_super_block *disk_super)
{
	struct btrfs_fs_devices *fs_devices;

	/*
	 * Handle the case where scanned device is part of an fs that had
	 * multiple successful changes of FSID but currently device didn't
	 * observe it. Meaning our fsid will be different than theirs. We need
	 * to handle two subcases :
	 *
	 *  1 - The fs still continues to have different METADATA/FSID uuids.
	 *  2 - The fs is switched back to its original FSID (METADATA/FSID are equal).
	 */
	list_for_each_entry(fs_devices, &fs_uuids, fs_list) {
		/* Changed UUIDs. */
		if (check_fsid_changed(fs_devices, disk_super->metadata_uuid) &&
		    memcmp(fs_devices->fsid, disk_super->fsid, BTRFS_FSID_SIZE) != 0)
			return fs_devices;

		/* Unchanged UUIDs. */
		if (memcmp(fs_devices->metadata_uuid, fs_devices->fsid,
			   BTRFS_FSID_SIZE) == 0 &&
		    memcmp(fs_devices->fsid, disk_super->metadata_uuid,
			   BTRFS_FSID_SIZE) == 0)
			return fs_devices;
	}

	return NULL;
}

static struct btrfs_fs_devices *find_fsid_reverted_metadata(struct btrfs_super_block *disk_super)
{
	struct btrfs_fs_devices *fs_devices;

	/*
	 * Handle the case where the scanned device is part of an fs whose last
	 * metadata UUID change reverted it to the original FSID. At the same
	 * time fs_devices was first created by another constituent device
	 * which didn't fully observe the operation. This results in an
	 * btrfs_fs_devices created with metadata/fsid different AND
	 * btrfs_fs_devices::fsid_change set AND the metadata_uuid of the
	 * fs_devices equal to the FSID of the disk.
	 */
	list_for_each_entry(fs_devices, &fs_uuids, fs_list) {
		if (!fs_devices->changing_fsid)
			continue;

		if (check_fsid_changed(fs_devices, disk_super->fsid))
			return fs_devices;
	}

	return NULL;
}

static int device_list_add(const char *path,
			   struct btrfs_super_block *disk_super,
			   struct btrfs_fs_devices **fs_devices_ret)
{
	struct btrfs_device *device;
	struct btrfs_fs_devices *fs_devices;
	u64 found_transid = btrfs_super_generation(disk_super);
	u64 devid = btrfs_stack_device_id(&disk_super->dev_item);
	bool metadata_uuid = (btrfs_super_incompat_flags(disk_super) &
		BTRFS_FEATURE_INCOMPAT_METADATA_UUID);
	bool changing_fsid = (btrfs_super_flags(disk_super) &
			      (BTRFS_SUPER_FLAG_CHANGING_FSID |
			       BTRFS_SUPER_FLAG_CHANGING_FSID_V2));

	if (changing_fsid) {
		if (!metadata_uuid)
			fs_devices = find_fsid_inprogress(disk_super);
		else
			fs_devices = find_fsid_changed(disk_super);
	} else if (metadata_uuid) {
		fs_devices = find_fsid_with_metadata_uuid(disk_super);
	} else {
		fs_devices = find_fsid_reverted_metadata(disk_super);
		if (!fs_devices)
			fs_devices = find_fsid(disk_super->fsid, NULL);
	}

	if (!fs_devices) {
		fs_devices = kzalloc(sizeof(*fs_devices), GFP_NOFS);
		if (!fs_devices)
			return -ENOMEM;
		INIT_LIST_HEAD(&fs_devices->devices);
		list_add(&fs_devices->fs_list, &fs_uuids);
		memcpy(fs_devices->fsid, disk_super->fsid, BTRFS_FSID_SIZE);
		if (metadata_uuid)
			memcpy(fs_devices->metadata_uuid,
			       disk_super->metadata_uuid, BTRFS_FSID_SIZE);
		else
			memcpy(fs_devices->metadata_uuid, fs_devices->fsid,
			       BTRFS_FSID_SIZE);

		fs_devices->latest_devid = devid;
		/* Below we would set this to found_transid */
		fs_devices->latest_generation = 0;
		fs_devices->lowest_devid = (u64)-1;
		fs_devices->chunk_alloc_policy = BTRFS_CHUNK_ALLOC_REGULAR;
		device = NULL;
	} else {
		device = find_device(fs_devices, devid,
				       disk_super->dev_item.uuid);
		/*
		 * If this disk has been pulled into an fs devices created by
		 * a device which had the CHANGING_FSID_V2 flag then replace the
		 * metadata_uuid/fsid values of the fs_devices.
		 */
		if (fs_devices->changing_fsid &&
		    found_transid > fs_devices->latest_generation) {
			memcpy(fs_devices->fsid, disk_super->fsid, BTRFS_FSID_SIZE);
			memcpy(fs_devices->metadata_uuid,
			       btrfs_sb_fsid_ptr(disk_super), BTRFS_FSID_SIZE);
		}
	}

	if (!device) {
		device = kzalloc(sizeof(*device), GFP_NOFS);
		if (!device) {
			/* we can safely leave the fs_devices entry around */
			return -ENOMEM;
		}
		device->fd = -1;
		device->devid = devid;
		device->generation = found_transid;
		memcpy(device->uuid, disk_super->dev_item.uuid,
		       BTRFS_UUID_SIZE);
		device->name = kstrdup(path, GFP_NOFS);
		if (!device->name) {
			kfree(device);
			return -ENOMEM;
		}
		device->label = kstrdup(disk_super->label, GFP_NOFS);
		if (!device->label) {
			kfree(device->name);
			kfree(device);
			return -ENOMEM;
		}
		device->total_devs = btrfs_super_num_devices(disk_super);
		device->super_bytes_used = btrfs_super_bytes_used(disk_super);
		device->total_bytes =
			btrfs_stack_device_total_bytes(&disk_super->dev_item);
		device->bytes_used =
			btrfs_stack_device_bytes_used(&disk_super->dev_item);
		list_add(&device->dev_list, &fs_devices->devices);
		device->fs_devices = fs_devices;
		fs_devices->num_devices++;
	} else if (!device->name || strcmp(device->name, path)) {
		char *name;

		/*
		 * The existing device has newer generation, so this one could
		 * be a stale one, don't add it.
		 */
		if (found_transid < device->generation) {
			warning(
	"adding device %s gen %llu but found an existing device %s gen %llu",
				path, found_transid, device->name,
				device->generation);
			return -EEXIST;
		}

		name = strdup(path);
                if (!name)
                        return -ENOMEM;
                kfree(device->name);
                device->name = name;
        }

	if (changing_fsid)
		fs_devices->inconsistent_super = changing_fsid;

	if (found_transid > fs_devices->latest_generation) {
		fs_devices->latest_devid = devid;
		fs_devices->latest_generation = found_transid;
		fs_devices->total_devices = device->total_devs;
		fs_devices->active_metadata_uuid = metadata_uuid;
		fs_devices->changing_fsid = changing_fsid;
	}
	if (fs_devices->lowest_devid > devid) {
		fs_devices->lowest_devid = devid;
	}
	*fs_devices_ret = fs_devices;
	return 0;
}

int btrfs_close_devices(struct btrfs_fs_devices *fs_devices)
{
	struct btrfs_fs_devices *seed_devices;
	struct btrfs_device *device;
	int ret = 0;

again:
	if (!fs_devices)
		return 0;
	while (!list_empty(&fs_devices->devices)) {
		device = list_entry(fs_devices->devices.next,
				    struct btrfs_device, dev_list);
		if (device->fd != -1) {
			if (device->writeable && fsync(device->fd) == -1) {
				warning("fsync on device %llu failed: %m",
					device->devid);
				ret = -errno;
			}
			if (posix_fadvise(device->fd, 0, 0, POSIX_FADV_DONTNEED))
				fprintf(stderr, "Warning, could not drop caches\n");
			close(device->fd);
			device->fd = -1;
		}
		device->writeable = 0;
		list_del(&device->dev_list);
		/* free the memory */
		kfree(device->name);
		kfree(device->label);
		kfree(device->zone_info);
		kfree(device);
	}

	seed_devices = fs_devices->seed;
	fs_devices->seed = NULL;
	if (seed_devices) {
		struct btrfs_fs_devices *orig;

		orig = fs_devices;
		fs_devices = seed_devices;
		list_del(&orig->fs_list);
		kfree(orig);
		goto again;
	} else {
		list_del(&fs_devices->fs_list);
		kfree(fs_devices);
	}

	return ret;
}

void btrfs_close_all_devices(void)
{
	struct btrfs_fs_devices *fs_devices;

	while (!list_empty(&fs_uuids)) {
		fs_devices = list_entry(fs_uuids.next, struct btrfs_fs_devices,
					fs_list);
		btrfs_close_devices(fs_devices);
	}
}

int btrfs_open_devices(struct btrfs_fs_info *fs_info,
		       struct btrfs_fs_devices *fs_devices, int flags)
{
	int fd;
	struct btrfs_device *device;
	int ret;

	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		if (!device->fs_info)
			device->fs_info = fs_info;

		if (!device->name) {
			printk("no name for device %llu, skip it now\n", device->devid);
			continue;
		}

		if ((flags & O_RDWR) && zoned_model(device->name) == ZONED_HOST_MANAGED)
			flags |= O_DIRECT;

		fd = open(device->name, flags);
		if (fd < 0) {
			ret = -errno;
			error("cannot open device '%s': %m", device->name);
			goto fail;
		}

		if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED))
			fprintf(stderr, "Warning, could not drop caches\n");

		if (device->devid == fs_devices->latest_devid)
			fs_devices->latest_bdev = fd;
		if (device->devid == fs_devices->lowest_devid)
			fs_devices->lowest_bdev = fd;
		device->fd = fd;
		if (flags & O_RDWR)
			device->writeable = 1;
	}
	return 0;
fail:
	btrfs_close_devices(fs_devices);
	return ret;
}

int btrfs_scan_one_device(int fd, const char *path,
			  struct btrfs_fs_devices **fs_devices_ret,
			  u64 *total_devs, u64 super_offset, unsigned sbflags)
{
	struct btrfs_super_block disk_super;
	int ret;

	ret = btrfs_read_dev_super(fd, &disk_super, super_offset, sbflags);
	if (ret < 0)
		return -EIO;

	if (btrfs_super_flags(&disk_super) & BTRFS_SUPER_FLAG_METADUMP)
		*total_devs = 1;
	else
		*total_devs = btrfs_super_num_devices(&disk_super);

	ret = device_list_add(path, &disk_super, fs_devices_ret);

	return ret;
}

static u64 dev_extent_search_start(struct btrfs_device *device, u64 start)
{
	u64 zone_size;

	switch (device->fs_devices->chunk_alloc_policy) {
	case BTRFS_CHUNK_ALLOC_REGULAR:
		/*
		 * We don't want to overwrite the superblock on the drive nor
		 * any area used by the boot loader (grub for example), so we
		 * make sure to start at an offset of at least 1MB.
		 */
		return max(start, BTRFS_BLOCK_RESERVED_1M_FOR_SUPER);
	case BTRFS_CHUNK_ALLOC_ZONED:
		zone_size = device->zone_info->zone_size;
		return ALIGN(max_t(u64, start, zone_size), zone_size);
	default:
		BUG();
	}
}

static bool dev_extent_hole_check_zoned(struct btrfs_device *device,
					u64 *hole_start, u64 *hole_size,
					u64 num_bytes)
{
	u64 pos;

	ASSERT(IS_ALIGNED(*hole_start, device->zone_info->zone_size));

	pos = btrfs_find_allocatable_zones(device, *hole_start,
					   *hole_start + *hole_size, num_bytes);
	if (pos != *hole_start) {
		*hole_size = *hole_start + *hole_size - pos;
		*hole_start = pos;
		return true;
	}

	return false;
}

/**
 * Check if specified hole is suitable for allocation
 *
 * @device:	the device which we have the hole
 * @hole_start: starting position of the hole
 * @hole_size:	the size of the hole
 * @num_bytes:	the size of the free space that we need
 *
 * This function may modify @hole_start and @hole_size to reflect the suitable
 * position for allocation. Returns true if hole position is updated, false
 * otherwise.
 */
static bool dev_extent_hole_check(struct btrfs_device *device, u64 *hole_start,
				  u64 *hole_size, u64 num_bytes)
{
	switch (device->fs_devices->chunk_alloc_policy) {
	case BTRFS_CHUNK_ALLOC_REGULAR:
		/* No check */
		break;
	case BTRFS_CHUNK_ALLOC_ZONED:
		return dev_extent_hole_check_zoned(device, hole_start,
						   hole_size, num_bytes);
	default:
		BUG();
	}

	return false;
}

/*
 * find_free_dev_extent_start - find free space in the specified device
 * @device:	  the device which we search the free space in
 * @num_bytes:	  the size of the free space that we need
 * @search_start: the position from which to begin the search
 * @start:	  store the start of the free space.
 * @len:	  the size of the free space. that we find, or the size
 *		  of the max free space if we don't find suitable free space
 *
 * this uses a pretty simple search, the expectation is that it is
 * called very infrequently and that a given device has a small number
 * of extents
 *
 * @start is used to store the start of the free space if we find. But if we
 * don't find suitable free space, it will be used to store the start position
 * of the max free space.
 *
 * @len is used to store the size of the free space that we find.
 * But if we don't find suitable free space, it is used to store the size of
 * the max free space.
 */
static int find_free_dev_extent_start(struct btrfs_device *device,
				      u64 num_bytes, u64 search_start,
				      u64 *start, u64 *len)
{
	struct btrfs_key key;
	struct btrfs_root *root = device->dev_root;
	struct btrfs_dev_extent *dev_extent;
	struct btrfs_path *path;
	u64 hole_size;
	u64 max_hole_start;
	u64 max_hole_size;
	u64 extent_end;
	u64 search_end = device->total_bytes;
	int ret;
	int slot;
	struct extent_buffer *l;
	u64 zone_size = 0;

	if (device->zone_info)
		zone_size = device->zone_info->zone_size;

	search_start = dev_extent_search_start(device, search_start);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	max_hole_start = search_start;
	max_hole_size = 0;

again:
	if (search_start >= search_end) {
		ret = -ENOSPC;
		goto out;
	}

	path->reada = READA_FORWARD;

	key.objectid = device->devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = search_start;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = btrfs_previous_item(root, path, key.objectid, key.type);
		if (ret < 0)
			goto out;
	}

	while (1) {
		l = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(l)) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto out;

			break;
		}
		btrfs_item_key_to_cpu(l, &key, slot);

		if (key.objectid < device->devid)
			goto next;

		if (key.objectid > device->devid)
			break;

		if (key.type != BTRFS_DEV_EXTENT_KEY)
			goto next;

		if (key.offset > search_start) {
			hole_size = key.offset - search_start;
			dev_extent_hole_check(device, &search_start, &hole_size,
					      num_bytes);

			if (hole_size > max_hole_size) {
				max_hole_start = search_start;
				max_hole_size = hole_size;
			}

			/*
			 * If this free space is greater than which we need,
			 * it must be the max free space that we have found
			 * until now, so max_hole_start must point to the start
			 * of this free space and the length of this free space
			 * is stored in max_hole_size. Thus, we return
			 * max_hole_start and max_hole_size and go back to the
			 * caller.
			 */
			if (hole_size >= num_bytes) {
				ret = 0;
				goto out;
			}
		}

		dev_extent = btrfs_item_ptr(l, slot, struct btrfs_dev_extent);
		extent_end = key.offset + btrfs_dev_extent_length(l,
								  dev_extent);
		if (extent_end > search_start)
			search_start = extent_end;
next:
		path->slots[0]++;
		cond_resched();
	}

	/*
	 * At this point, search_start should be the end of
	 * allocated dev extents, and when shrinking the device,
	 * search_end may be smaller than search_start.
	 */
	if (search_end > search_start) {
		hole_size = search_end - search_start;
		if (dev_extent_hole_check(device, &search_start, &hole_size,
					  num_bytes)) {
			btrfs_release_path(path);
			goto again;
		}

		if (hole_size > max_hole_size) {
			max_hole_start = search_start;
			max_hole_size = hole_size;
		}
	}

	/* See above. */
	if (max_hole_size < num_bytes)
		ret = -ENOSPC;
	else
		ret = 0;

out:
	ASSERT(zone_size == 0 || IS_ALIGNED(max_hole_start, zone_size));
	btrfs_free_path(path);
	*start = max_hole_start;
	if (len)
		*len = max_hole_size;
	return ret;
}

static int find_free_dev_extent(struct btrfs_device *device, u64 num_bytes,
				u64 *start, u64 *len)
{
	/* FIXME use last free of some kind */
	return find_free_dev_extent_start(device, num_bytes, 0, start, len);
}

/*
 * Insert one device extent into the fs.
 */
int btrfs_insert_dev_extent(struct btrfs_trans_handle *trans,
			    struct btrfs_device *device,
			    u64 chunk_offset, u64 num_bytes, u64 start)
{
	int ret;
	struct btrfs_path *path;
	struct btrfs_root *root = device->dev_root;
	struct btrfs_dev_extent *extent;
	struct extent_buffer *leaf;
	struct btrfs_key key;

	/* Check alignment to zone for a zoned block device */
	ASSERT(!device->zone_info ||
	       device->zone_info->model != ZONED_HOST_MANAGED ||
	       IS_ALIGNED(start, device->zone_info->zone_size));

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = device->devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = start;
	ret = btrfs_insert_empty_item(trans, root, path, &key,
				      sizeof(*extent));
	if (ret < 0)
		goto err;

	leaf = path->nodes[0];
	extent = btrfs_item_ptr(leaf, path->slots[0],
				struct btrfs_dev_extent);
	btrfs_set_dev_extent_chunk_tree(leaf, extent, BTRFS_CHUNK_TREE_OBJECTID);
	btrfs_set_dev_extent_chunk_objectid(leaf, extent,
					    BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_dev_extent_chunk_offset(leaf, extent, chunk_offset);

	write_extent_buffer(leaf, root->fs_info->chunk_tree_uuid,
		    (unsigned long)btrfs_dev_extent_chunk_tree_uuid(extent),
		    BTRFS_UUID_SIZE);

	btrfs_set_dev_extent_length(leaf, extent, num_bytes);
	btrfs_mark_buffer_dirty(leaf);
err:
	btrfs_free_path(path);
	return ret;
}

/*
 * Allocate one free dev extent and insert it into the fs.
 */
static int btrfs_alloc_dev_extent(struct btrfs_trans_handle *trans,
				  struct btrfs_device *device,
				  u64 chunk_offset, u64 num_bytes, u64 *start)
{
	int ret;

	ret = find_free_dev_extent(device, num_bytes, start, NULL);
	if (ret)
		return ret;
	return btrfs_insert_dev_extent(trans, device, chunk_offset, num_bytes,
					*start);
}

static int find_next_chunk(struct btrfs_fs_info *fs_info, u64 *offset)
{
	struct btrfs_root *root = fs_info->chunk_root;
	struct btrfs_path *path;
	int ret;
	struct btrfs_key key;
	struct btrfs_chunk *chunk;
	struct btrfs_key found_key;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto error;

	BUG_ON(ret == 0);

	ret = btrfs_previous_item(root, path, 0, BTRFS_CHUNK_ITEM_KEY);
	if (ret) {
		*offset = 0;
	} else {
		btrfs_item_key_to_cpu(path->nodes[0], &found_key,
				      path->slots[0]);
		if (found_key.objectid != BTRFS_FIRST_CHUNK_TREE_OBJECTID)
			*offset = 0;
		else {
			chunk = btrfs_item_ptr(path->nodes[0], path->slots[0],
					       struct btrfs_chunk);
			*offset = found_key.offset +
				btrfs_chunk_length(path->nodes[0], chunk);
		}
	}
	ret = 0;
error:
	btrfs_free_path(path);
	return ret;
}

static int find_next_devid(struct btrfs_root *root, struct btrfs_path *path,
			   u64 *objectid)
{
	int ret;
	struct btrfs_key key;
	struct btrfs_key found_key;

	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto error;

	BUG_ON(ret == 0);

	ret = btrfs_previous_item(root, path, BTRFS_DEV_ITEMS_OBJECTID,
				  BTRFS_DEV_ITEM_KEY);
	if (ret) {
		*objectid = 1;
	} else {
		btrfs_item_key_to_cpu(path->nodes[0], &found_key,
				      path->slots[0]);
		*objectid = found_key.offset + 1;
	}
	ret = 0;
error:
	btrfs_release_path(path);
	return ret;
}

/*
 * the device information is stored in the chunk root
 * the btrfs_device struct should be fully filled in
 */
int btrfs_add_device(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info,
		     struct btrfs_device *device)
{
	int ret;
	struct btrfs_path *path;
	struct btrfs_dev_item *dev_item;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_root *root = fs_info->chunk_root;
	unsigned long ptr;
	u64 free_devid = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = find_next_devid(root, path, &free_devid);
	if (ret)
		goto out;

	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = free_devid;

	ret = btrfs_insert_empty_item(trans, root, path, &key,
				      sizeof(*dev_item));
	if (ret)
		goto out;

	leaf = path->nodes[0];
	dev_item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_dev_item);

	device->devid = free_devid;
	btrfs_set_device_id(leaf, dev_item, device->devid);
	btrfs_set_device_generation(leaf, dev_item, 0);
	btrfs_set_device_type(leaf, dev_item, device->type);
	btrfs_set_device_io_align(leaf, dev_item, device->io_align);
	btrfs_set_device_io_width(leaf, dev_item, device->io_width);
	btrfs_set_device_sector_size(leaf, dev_item, device->sector_size);
	btrfs_set_device_total_bytes(leaf, dev_item, device->total_bytes);
	btrfs_set_device_bytes_used(leaf, dev_item, device->bytes_used);
	btrfs_set_device_group(leaf, dev_item, 0);
	btrfs_set_device_seek_speed(leaf, dev_item, 0);
	btrfs_set_device_bandwidth(leaf, dev_item, 0);
	btrfs_set_device_start_offset(leaf, dev_item, 0);

	ptr = (unsigned long)btrfs_device_uuid(dev_item);
	write_extent_buffer(leaf, device->uuid, ptr, BTRFS_UUID_SIZE);
	ptr = (unsigned long)btrfs_device_fsid(dev_item);
	write_extent_buffer(leaf, fs_info->fs_devices->metadata_uuid, ptr,
			    BTRFS_UUID_SIZE);
	btrfs_mark_buffer_dirty(leaf);
	fs_info->fs_devices->total_rw_bytes += device->total_bytes;
	ret = 0;

out:
	btrfs_free_path(path);
	return ret;
}

int btrfs_update_device(struct btrfs_trans_handle *trans,
			struct btrfs_device *device)
{
	int ret;
	struct btrfs_path *path;
	struct btrfs_root *root;
	struct btrfs_dev_item *dev_item;
	struct extent_buffer *leaf;
	struct btrfs_key key;

	root = device->dev_root->fs_info->chunk_root;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = device->devid;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret < 0)
		goto out;

	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	leaf = path->nodes[0];
	dev_item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_dev_item);

	btrfs_set_device_id(leaf, dev_item, device->devid);
	btrfs_set_device_type(leaf, dev_item, device->type);
	btrfs_set_device_io_align(leaf, dev_item, device->io_align);
	btrfs_set_device_io_width(leaf, dev_item, device->io_width);
	btrfs_set_device_sector_size(leaf, dev_item, device->sector_size);
	btrfs_set_device_total_bytes(leaf, dev_item, device->total_bytes);
	btrfs_set_device_bytes_used(leaf, dev_item, device->bytes_used);
	btrfs_mark_buffer_dirty(leaf);

out:
	btrfs_free_path(path);
	return ret;
}

int btrfs_add_system_chunk(struct btrfs_fs_info *fs_info, struct btrfs_key *key,
			   struct btrfs_chunk *chunk, int item_size)
{
	struct btrfs_super_block *super_copy = fs_info->super_copy;
	struct btrfs_disk_key disk_key;
	u32 array_size;
	u8 *ptr;

	array_size = btrfs_super_sys_array_size(super_copy);
	if (array_size + item_size + sizeof(disk_key)
			> BTRFS_SYSTEM_CHUNK_ARRAY_SIZE)
		return -EFBIG;

	ptr = super_copy->sys_chunk_array + array_size;
	btrfs_cpu_key_to_disk(&disk_key, key);
	memcpy(ptr, &disk_key, sizeof(disk_key));
	ptr += sizeof(disk_key);
	memcpy(ptr, chunk, item_size);
	item_size += sizeof(disk_key);
	btrfs_set_super_sys_array_size(super_copy, array_size + item_size);
	return 0;
}

static u64 chunk_bytes_by_type(struct alloc_chunk_ctl *ctl)
{
	u64 type = ctl->type;
	u64 stripe_size = ctl->stripe_size;

	if (type & (BTRFS_BLOCK_GROUP_RAID1_MASK | BTRFS_BLOCK_GROUP_DUP))
		return stripe_size;
	else if (type & BTRFS_BLOCK_GROUP_RAID10)
		return stripe_size * (ctl->num_stripes / ctl->sub_stripes);
	else if (type & BTRFS_BLOCK_GROUP_RAID56_MASK)
		return stripe_size * (ctl->num_stripes - btrfs_bg_type_to_nparity(type));
	else
		return stripe_size * ctl->num_stripes;
}

/*
 * btrfs_device_avail_bytes - count bytes available for alloc_chunk
 *
 * It is not equal to "device->total_bytes - device->bytes_used".
 * We do not allocate any chunk in 1M at beginning of device, and not
 * allowed to allocate any chunk before alloc_start if it is specified.
 * So search holes from 1M to device->total_bytes.
 */
static int btrfs_device_avail_bytes(struct btrfs_trans_handle *trans,
				    struct btrfs_device *device,
				    u64 *avail_bytes)
{
	struct btrfs_path *path;
	struct btrfs_root *root = device->dev_root;
	struct btrfs_key key;
	struct btrfs_dev_extent *dev_extent = NULL;
	struct extent_buffer *l;
	u64 search_start = BTRFS_BLOCK_RESERVED_1M_FOR_SUPER;;
	u64 search_end = device->total_bytes;
	u64 extent_end = 0;
	u64 free_bytes = 0;
	int ret;
	int slot = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = device->devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = search_start;

	path->reada = READA_FORWARD;
	ret = btrfs_search_slot(trans, root, &key, path, 0, 0);
	if (ret < 0)
		goto error;
	ret = btrfs_previous_item(root, path, 0, key.type);
	if (ret < 0)
		goto error;

	while (1) {
		l = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(l)) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto error;
			break;
		}
		btrfs_item_key_to_cpu(l, &key, slot);

		if (key.objectid < device->devid)
			goto next;
		if (key.objectid > device->devid)
			break;
		if (key.type != BTRFS_DEV_EXTENT_KEY)
			goto next;
		if (key.offset > search_end)
			break;
		if (key.offset > search_start)
			free_bytes += key.offset - search_start;

		dev_extent = btrfs_item_ptr(l, slot, struct btrfs_dev_extent);
		extent_end = key.offset + btrfs_dev_extent_length(l,
								  dev_extent);
		if (extent_end > search_start)
			search_start = extent_end;
		if (search_start > search_end)
			break;
next:
		path->slots[0]++;
		cond_resched();
	}

	if (search_start < search_end)
		free_bytes += search_end - search_start;

	*avail_bytes = free_bytes;
	ret = 0;
error:
	btrfs_free_path(path);
	return ret;
}

#define BTRFS_MAX_DEVS(info) ((BTRFS_LEAF_DATA_SIZE(info)	\
			- sizeof(struct btrfs_item)		\
			- sizeof(struct btrfs_chunk))		\
			/ sizeof(struct btrfs_stripe) + 1)

#define BTRFS_MAX_DEVS_SYS_CHUNK ((BTRFS_SYSTEM_CHUNK_ARRAY_SIZE	\
				- 2 * sizeof(struct btrfs_disk_key)	\
				- 2 * sizeof(struct btrfs_chunk))	\
				/ sizeof(struct btrfs_stripe) + 1)

static void init_alloc_chunk_ctl_policy_regular(struct btrfs_fs_info *info,
						struct alloc_chunk_ctl *ctl)
{
	u64 type = ctl->type;
	u64 percent_max;

	if (type & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
		if (type & BTRFS_BLOCK_GROUP_SYSTEM) {
			ctl->stripe_size = SZ_8M;
			ctl->max_chunk_size = ctl->stripe_size * 2;
			ctl->min_stripe_size = SZ_1M;
			ctl->max_stripes = BTRFS_MAX_DEVS_SYS_CHUNK;
		} else if (type & BTRFS_BLOCK_GROUP_DATA) {
			ctl->stripe_size = SZ_1G;
			ctl->max_chunk_size = 10 * ctl->stripe_size;
			ctl->min_stripe_size = SZ_64M;
			ctl->max_stripes = BTRFS_MAX_DEVS(info);
		} else if (type & BTRFS_BLOCK_GROUP_METADATA) {
			/* For larger filesystems, use larger metadata chunks */
			if (info->fs_devices->total_rw_bytes > 50ULL * SZ_1G)
				ctl->max_chunk_size = SZ_1G;
			else
				ctl->max_chunk_size = SZ_256M;
			ctl->stripe_size = ctl->max_chunk_size;
			ctl->min_stripe_size = SZ_32M;
			ctl->max_stripes = BTRFS_MAX_DEVS(info);
		}
	}

	/* We don't want a chunk larger than 10% of the FS */
	percent_max = div_factor(btrfs_super_total_bytes(info->super_copy), 1);
	ctl->max_chunk_size = min(percent_max, ctl->max_chunk_size);
}

static void init_alloc_chunk_ctl_policy_zoned(struct btrfs_fs_info *info,
					      struct alloc_chunk_ctl *ctl)
{
	u64 type = ctl->type;
	u64 zone_size = info->zone_size;
	int min_num_stripes = ctl->min_stripes * ctl->num_stripes;
	int min_data_stripes = (min_num_stripes - ctl->nparity) / ctl->ncopies;
	u64 min_chunk_size = min_data_stripes * zone_size;

	ctl->stripe_size = zone_size;
	ctl->min_stripe_size = zone_size;
	if (type & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
		if (type & BTRFS_BLOCK_GROUP_SYSTEM) {
			ctl->max_chunk_size = SZ_16M;
			ctl->max_stripes = BTRFS_MAX_DEVS_SYS_CHUNK;
		} else if (type & BTRFS_BLOCK_GROUP_DATA) {
			ctl->max_chunk_size = 10ULL * SZ_1G;
			ctl->max_stripes = BTRFS_MAX_DEVS(info);
		} else if (type & BTRFS_BLOCK_GROUP_METADATA) {
			/* For larger filesystems, use larger metadata chunks */
			if (info->fs_devices->total_rw_bytes > 50ULL * SZ_1G)
				ctl->max_chunk_size = SZ_1G;
			else
				ctl->max_chunk_size = SZ_256M;
			ctl->max_stripes = BTRFS_MAX_DEVS(info);
		}
	}

	ctl->max_chunk_size = round_down(ctl->max_chunk_size, zone_size);
	ctl->max_chunk_size = max(ctl->max_chunk_size, min_chunk_size);
}

static void init_alloc_chunk_ctl(struct btrfs_fs_info *info,
				 struct alloc_chunk_ctl *ctl)
{
	enum btrfs_raid_types type = btrfs_bg_flags_to_raid_index(ctl->type);

	ctl->num_stripes = btrfs_raid_array[type].dev_stripes;
	ctl->min_stripes = btrfs_raid_array[type].devs_min;
	ctl->max_stripes = 0;
	ctl->sub_stripes = btrfs_raid_array[type].sub_stripes;
	ctl->stripe_size = SZ_8M;
	ctl->min_stripe_size = SZ_1M;
	ctl->max_chunk_size = 4 * ctl->stripe_size;
	ctl->total_devs = btrfs_super_num_devices(info->super_copy);
	ctl->dev_offset = 0;
	ctl->nparity = btrfs_raid_array[type].nparity;
	ctl->ncopies = btrfs_raid_array[type].ncopies;

	switch (info->fs_devices->chunk_alloc_policy) {
	case BTRFS_CHUNK_ALLOC_REGULAR:
		init_alloc_chunk_ctl_policy_regular(info, ctl);
		break;
	case BTRFS_CHUNK_ALLOC_ZONED:
		init_alloc_chunk_ctl_policy_zoned(info, ctl);
		break;
	default:
		BUG();
	}

	switch (type) {
	case BTRFS_RAID_DUP:
		ctl->min_stripes = 2;
		break;
	case BTRFS_RAID_RAID1:
	case BTRFS_RAID_RAID1C3:
	case BTRFS_RAID_RAID1C4:
		ctl->num_stripes = min(ctl->min_stripes, ctl->total_devs);
		break;
	case BTRFS_RAID_RAID0:
	case BTRFS_RAID_RAID10:
	case BTRFS_RAID_RAID5:
	case BTRFS_RAID_RAID6:
		ctl->num_stripes = min(ctl->max_stripes, ctl->total_devs);
		if (type == BTRFS_RAID_RAID10)
			ctl->num_stripes &= ~(u32)1;
		break;
	default:
		break;
	}
}

static int decide_stripe_size_regular(struct alloc_chunk_ctl *ctl)
{
	if (chunk_bytes_by_type(ctl) > ctl->max_chunk_size) {
		ctl->stripe_size = ctl->max_chunk_size;
		ctl->stripe_size /= ctl->num_stripes;
		ctl->stripe_size = round_down(ctl->stripe_size, BTRFS_STRIPE_LEN);
	}
	/* We don't want tiny stripes */
	ctl->stripe_size = max_t(u64, ctl->stripe_size, ctl->min_stripe_size);

	/* Align to the stripe length */
	ctl->stripe_size = round_down(ctl->stripe_size, BTRFS_STRIPE_LEN);

	return 0;
}

static int decide_stripe_size_zoned(struct alloc_chunk_ctl *ctl)
{
	if (chunk_bytes_by_type(ctl) > ctl->max_chunk_size) {
		/* stripe_size is fixed in ZONED, reduce num_stripes instead */
		ctl->num_stripes = ctl->max_chunk_size * ctl->ncopies /
			ctl->stripe_size;
		if (ctl->num_stripes < ctl->min_stripes)
			return -ENOSPC;
	}

	return 0;
}

static int decide_stripe_size(struct btrfs_fs_info *info,
			      struct alloc_chunk_ctl *ctl)
{
	switch (info->fs_devices->chunk_alloc_policy) {
	case BTRFS_CHUNK_ALLOC_REGULAR:
		return decide_stripe_size_regular(ctl);
	case BTRFS_CHUNK_ALLOC_ZONED:
		return decide_stripe_size_zoned(ctl);
	default:
		BUG();
	}
}

static int create_chunk(struct btrfs_trans_handle *trans,
			struct btrfs_fs_info *info, struct alloc_chunk_ctl *ctl,
			struct list_head *private_devs)
{
	struct btrfs_root *chunk_root = info->chunk_root;
	struct btrfs_stripe *stripes;
	struct btrfs_device *device = NULL;
	struct btrfs_chunk *chunk;
	struct list_head *dev_list = &info->fs_devices->devices;
	struct list_head *cur;
	struct map_lookup *map;
	int ret;
	int index;
	struct btrfs_key key;
	u64 offset;
	u64 zone_size = info->zone_size;

	if (!ctl->start) {
		ret = find_next_chunk(info, &offset);
		if (ret)
			return ret;
	} else {
		offset = ctl->start;
	}

	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = offset;

	chunk = kmalloc(btrfs_chunk_item_size(ctl->num_stripes), GFP_NOFS);
	if (!chunk)
		return -ENOMEM;

	map = kmalloc(btrfs_map_lookup_size(ctl->num_stripes), GFP_NOFS);
	if (!map) {
		kfree(chunk);
		return -ENOMEM;
	}

	stripes = &chunk->stripe;
	ctl->num_bytes = chunk_bytes_by_type(ctl);
	index = 0;
	while (index < ctl->num_stripes) {
		u64 dev_offset;
		struct btrfs_stripe *stripe;

		BUG_ON(list_empty(private_devs));
		cur = private_devs->next;
		device = list_entry(cur, struct btrfs_device, dev_list);

		/* loop over this device again if we're doing a dup group */
		if (!(ctl->type & BTRFS_BLOCK_GROUP_DUP) ||
		    (index == ctl->num_stripes - 1))
			list_move(&device->dev_list, dev_list);

		if (!ctl->dev_offset) {
			ret = btrfs_alloc_dev_extent(trans, device, key.offset,
					ctl->stripe_size, &dev_offset);
			if (ret < 0)
				goto out_chunk_map;
		} else {
			dev_offset = ctl->dev_offset;
			ret = btrfs_insert_dev_extent(trans, device, key.offset,
						      ctl->stripe_size,
						      ctl->dev_offset);
			BUG_ON(ret);
		}

		ASSERT(!zone_size || IS_ALIGNED(dev_offset, zone_size));

		device->bytes_used += ctl->stripe_size;
		ret = btrfs_update_device(trans, device);
		if (ret < 0)
			goto out_chunk_map;

		map->stripes[index].dev = device;
		map->stripes[index].physical = dev_offset;
		stripe = stripes + index;
		btrfs_set_stack_stripe_devid(stripe, device->devid);
		btrfs_set_stack_stripe_offset(stripe, dev_offset);
		memcpy(stripe->dev_uuid, device->uuid, BTRFS_UUID_SIZE);
		index++;
	}
	BUG_ON(!list_empty(private_devs));

	/* key was set above */
	btrfs_set_stack_chunk_length(chunk, ctl->num_bytes);
	btrfs_set_stack_chunk_owner(chunk, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_stack_chunk_stripe_len(chunk, BTRFS_STRIPE_LEN);
	btrfs_set_stack_chunk_type(chunk, ctl->type);
	btrfs_set_stack_chunk_num_stripes(chunk, ctl->num_stripes);
	btrfs_set_stack_chunk_io_align(chunk, BTRFS_STRIPE_LEN);
	btrfs_set_stack_chunk_io_width(chunk, BTRFS_STRIPE_LEN);
	btrfs_set_stack_chunk_sector_size(chunk, info->sectorsize);
	btrfs_set_stack_chunk_sub_stripes(chunk, ctl->sub_stripes);
	map->sector_size = info->sectorsize;
	map->stripe_len = BTRFS_STRIPE_LEN;
	map->io_align = BTRFS_STRIPE_LEN;
	map->io_width = BTRFS_STRIPE_LEN;
	map->type = ctl->type;
	map->num_stripes = ctl->num_stripes;
	map->sub_stripes = ctl->sub_stripes;

	ret = btrfs_insert_item(trans, chunk_root, &key, chunk,
				btrfs_chunk_item_size(ctl->num_stripes));
	BUG_ON(ret);
	ctl->start = key.offset;

	map->ce.start = key.offset;
	map->ce.size = ctl->num_bytes;

	ret = insert_cache_extent(&info->mapping_tree.cache_tree, &map->ce);
	if (ret < 0)
		goto out_chunk_map;

	if (ctl->type & BTRFS_BLOCK_GROUP_SYSTEM) {
		ret = btrfs_add_system_chunk(info, &key,
			    chunk, btrfs_chunk_item_size(ctl->num_stripes));
		if (ret < 0)
			goto out_chunk;
	}

	kfree(chunk);
	return ret;

out_chunk_map:
	kfree(map);
out_chunk:
	kfree(chunk);
	return ret;
}

int btrfs_alloc_chunk(struct btrfs_trans_handle *trans,
		      struct btrfs_fs_info *info, u64 *start,
		      u64 *num_bytes, u64 type)
{
	struct btrfs_device *device = NULL;
	struct list_head private_devs;
	struct list_head *dev_list = &info->fs_devices->devices;
	struct list_head *cur;
	u64 min_free;
	u64 avail = 0;
	u64 max_avail = 0;
	struct alloc_chunk_ctl ctl;
	int looped = 0;
	int ret;
	int index;

	if (list_empty(dev_list))
		return -ENOSPC;

	ctl.type = type;
	/* start and num_bytes will be set by create_chunk() */
	ctl.start = 0;
	ctl.num_bytes = 0;
	init_alloc_chunk_ctl(info, &ctl);
	if (ctl.num_stripes < ctl.min_stripes)
		return -ENOSPC;

again:
	ret = decide_stripe_size(info, &ctl);
	if (ret < 0)
		return ret;

	INIT_LIST_HEAD(&private_devs);
	cur = dev_list->next;
	index = 0;

	if (type & BTRFS_BLOCK_GROUP_DUP)
		min_free = ctl.stripe_size * 2;
	else
		min_free = ctl.stripe_size;

	/* Build a private list of devices we will allocate from */
	while (index < ctl.num_stripes) {
		device = list_entry(cur, struct btrfs_device, dev_list);
		ret = btrfs_device_avail_bytes(trans, device, &avail);
		if (ret)
			return ret;
		cur = cur->next;
		if (avail >= min_free) {
			list_move(&device->dev_list, &private_devs);
			index++;
			if (type & BTRFS_BLOCK_GROUP_DUP)
				index++;
		} else if (avail > max_avail)
			max_avail = avail;
		if (cur == dev_list)
			break;
	}
	if (index < ctl.num_stripes) {
		list_splice(&private_devs, dev_list);
		if (index >= ctl.min_stripes) {
			ctl.num_stripes = index;
			if (type & (BTRFS_BLOCK_GROUP_RAID10)) {
				/* We know this should be 2, but just in case */
				ASSERT(is_power_of_2(ctl.sub_stripes));
				ctl.num_stripes = round_down(ctl.num_stripes,
							     ctl.sub_stripes);
			}
			looped = 1;
			goto again;
		}
		if (!looped && max_avail > 0) {
			looped = 1;
			if (ctl.type & BTRFS_BLOCK_GROUP_DUP)
				ctl.stripe_size = max_avail / 2;
			else
				ctl.stripe_size = max_avail;
			goto again;
		}
		return -ENOSPC;
	}

	ret = create_chunk(trans, info, &ctl, &private_devs);

	/*
	 * This can happen if above create_chunk() failed, we need to move all
	 * devices back to dev_list.
	 */
	while (!list_empty(&private_devs)) {
		device = list_entry(private_devs.next, struct btrfs_device,
				    dev_list);
		list_move(&device->dev_list, dev_list);
	}
	/*
	 * All private devs moved back to @dev_list, now dev_list should not be
	 * empty.
	 */
	ASSERT(!list_empty(dev_list));
	*start = ctl.start;
	*num_bytes = ctl.num_bytes;

	return ret;
}

/*
 * Alloc a DATA chunk with SINGLE profile.
 *
 * It allocates a chunk with 1:1 mapping (btrfs logical bytenr == on-disk bytenr)
 * Caller must make sure the chunk and dev_extent are not occupied.
 */
int btrfs_alloc_data_chunk(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *info, u64 *start, u64 num_bytes)
{
	struct list_head *dev_list = &info->fs_devices->devices;
	struct list_head private_devs;
	struct btrfs_device *device;
	struct alloc_chunk_ctl ctl;

	if (*start != round_down(*start, info->sectorsize)) {
		error("DATA chunk start not sectorsize aligned: %llu",
				(unsigned long long)*start);
		return -EINVAL;
	}

	ctl.start = *start;
	ctl.type = BTRFS_BLOCK_GROUP_DATA;
	ctl.num_stripes = 1;
	ctl.max_stripes = 1;
	ctl.min_stripes = 1;
	ctl.sub_stripes = 1;
	ctl.stripe_size = num_bytes;
	ctl.min_stripe_size = num_bytes;
	ctl.num_bytes = num_bytes;
	ctl.max_chunk_size = num_bytes;
	ctl.total_devs = btrfs_super_num_devices(info->super_copy);
	ctl.dev_offset = *start;

	INIT_LIST_HEAD(&private_devs);
	/* Build a list containing one device */
	device = list_entry(dev_list->next, struct btrfs_device, dev_list);
	list_move(&device->dev_list, &private_devs);

	return create_chunk(trans, info, &ctl, &private_devs);
}

int btrfs_num_copies(struct btrfs_fs_info *fs_info, u64 logical, u64 len)
{
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	struct cache_extent *ce;
	struct map_lookup *map;
	int ret;

	ce = search_cache_extent(&map_tree->cache_tree, logical);
	if (!ce) {
		fprintf(stderr, "No mapping for %llu-%llu\n",
			(unsigned long long)logical,
			(unsigned long long)logical+len);
		return 1;
	}
	if (ce->start > logical || ce->start + ce->size < logical) {
		fprintf(stderr, "Invalid mapping for %llu-%llu, got "
			"%llu-%llu\n", (unsigned long long)logical,
			(unsigned long long)logical+len,
			(unsigned long long)ce->start,
			(unsigned long long)ce->start + ce->size);
		return 1;
	}
	map = container_of(ce, struct map_lookup, ce);

	if (map->type & (BTRFS_BLOCK_GROUP_DUP | BTRFS_BLOCK_GROUP_RAID1_MASK))
		ret = map->num_stripes;
	else if (map->type & BTRFS_BLOCK_GROUP_RAID10)
		ret = map->sub_stripes;
	else if (map->type & BTRFS_BLOCK_GROUP_RAID5)
		ret = 2;
	else if (map->type & BTRFS_BLOCK_GROUP_RAID6)
		ret = 3;
	else
		ret = 1;
	return ret;
}

int btrfs_next_bg(struct btrfs_fs_info *fs_info, u64 *logical,
		  u64 *size, u64 type)
{
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	struct cache_extent *ce;
	struct map_lookup *map;
	u64 cur = *logical;

	ce = search_cache_extent(&map_tree->cache_tree, cur);

	while (ce) {
		/*
		 * only jump to next bg if our cur is not 0
		 * As the initial logical for btrfs_next_bg() is 0, and
		 * if we jump to next bg, we skipped a valid bg.
		 */
		if (cur) {
			ce = next_cache_extent(ce);
			if (!ce)
				return -ENOENT;
		}

		cur = ce->start;
		map = container_of(ce, struct map_lookup, ce);
		if (map->type & type) {
			*logical = ce->start;
			*size = ce->size;
			return 0;
		}
		if (!cur)
			ce = next_cache_extent(ce);
	}

	return -ENOENT;
}

int btrfs_rmap_block(struct btrfs_fs_info *fs_info, u64 chunk_start,
		     u64 physical, u64 **logical, int *naddrs, int *stripe_len)
{
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	struct cache_extent *ce;
	struct map_lookup *map;
	u64 *buf;
	u64 bytenr;
	u64 length;
	u64 stripe_nr;
	u64 rmap_len;
	int i, j, nr = 0;

	ce = search_cache_extent(&map_tree->cache_tree, chunk_start);
	BUG_ON(!ce);
	map = container_of(ce, struct map_lookup, ce);

	length = ce->size;
	rmap_len = map->stripe_len;
	if (map->type & BTRFS_BLOCK_GROUP_RAID10)
		length = ce->size / (map->num_stripes / map->sub_stripes);
	else if (map->type & BTRFS_BLOCK_GROUP_RAID0)
		length = ce->size / map->num_stripes;
	else if (map->type & BTRFS_BLOCK_GROUP_RAID56_MASK) {
		length = ce->size / nr_data_stripes(map);
		rmap_len = map->stripe_len * nr_data_stripes(map);
	}

	buf = kzalloc(sizeof(u64) * map->num_stripes, GFP_NOFS);

	for (i = 0; i < map->num_stripes; i++) {
		if (map->stripes[i].physical > physical ||
		    map->stripes[i].physical + length <= physical)
			continue;

		stripe_nr = (physical - map->stripes[i].physical) /
			    map->stripe_len;

		if (map->type & BTRFS_BLOCK_GROUP_RAID10) {
			stripe_nr = (stripe_nr * map->num_stripes + i) /
				    map->sub_stripes;
		} else if (map->type & BTRFS_BLOCK_GROUP_RAID0) {
			stripe_nr = stripe_nr * map->num_stripes + i;
		} /* else if RAID[56], multiply by nr_data_stripes().
		   * Alternatively, just use rmap_len below instead of
		   * map->stripe_len */

		bytenr = ce->start + stripe_nr * rmap_len;
		for (j = 0; j < nr; j++) {
			if (buf[j] == bytenr)
				break;
		}
		if (j == nr)
			buf[nr++] = bytenr;
	}

	*logical = buf;
	*naddrs = nr;
	*stripe_len = rmap_len;

	return 0;
}

static inline int parity_smaller(u64 a, u64 b)
{
	return a > b;
}

/* Bubble-sort the stripe set to put the parity/syndrome stripes last */
static void sort_parity_stripes(struct btrfs_multi_bio *bbio, u64 *raid_map)
{
	struct btrfs_bio_stripe s;
	int i;
	u64 l;
	int again = 1;

	while (again) {
		again = 0;
		for (i = 0; i < bbio->num_stripes - 1; i++) {
			if (parity_smaller(raid_map[i], raid_map[i+1])) {
				s = bbio->stripes[i];
				l = raid_map[i];
				bbio->stripes[i] = bbio->stripes[i+1];
				raid_map[i] = raid_map[i+1];
				bbio->stripes[i+1] = s;
				raid_map[i+1] = l;
				again = 1;
			}
		}
	}
}

int btrfs_map_block(struct btrfs_fs_info *fs_info, int rw,
		    u64 logical, u64 *length,
		    struct btrfs_multi_bio **multi_ret, int mirror_num,
		    u64 **raid_map_ret)
{
	return __btrfs_map_block(fs_info, rw, logical, length, NULL,
				 multi_ret, mirror_num, raid_map_ret);
}

static bool btrfs_need_stripe_tree_update(struct btrfs_fs_info *fs_info, u64 map_type)
{
#if EXPERIMENTAL
	const bool is_data = (map_type & BTRFS_BLOCK_GROUP_DATA);

	if (!btrfs_fs_incompat(fs_info, RAID_STRIPE_TREE))
		return false;
	if (!fs_info->stripe_root)
		return false;
	if (!is_data)
		return false;

	if (map_type & BTRFS_BLOCK_GROUP_DUP)
		return true;
	if (map_type & BTRFS_BLOCK_GROUP_RAID1_MASK)
		return true;
	if (map_type & BTRFS_BLOCK_GROUP_RAID0)
		return true;
	if (map_type & BTRFS_BLOCK_GROUP_RAID10)
		return true;

#endif
	return false;
}

static int btrfs_stripe_tree_logical_to_physical(struct btrfs_fs_info *fs_info,
						u64 logical,
						struct btrfs_bio_stripe *stripe)
{
	struct btrfs_root *root = fs_info->stripe_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	struct extent_buffer *leaf;
	int slot;
	int ret;

	key.objectid = logical;
	key.type = BTRFS_RAID_STRIPE_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

	while (1) {
		struct btrfs_key found_key;
		struct btrfs_stripe_extent *extent;
		int num_stripes;
		u32 item_size;

		leaf = path.nodes[0];
		slot = path.slots[0];

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto error;
			break;
		}

		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		if (found_key.type != BTRFS_RAID_STRIPE_KEY)
			goto next;

		extent = btrfs_item_ptr(leaf, slot, struct btrfs_stripe_extent);
		item_size = btrfs_item_size(leaf, slot);
		num_stripes = (item_size -
			       offsetof(struct btrfs_stripe_extent, strides)) /
			      sizeof(struct btrfs_raid_stride);

		for (int i = 0; i < num_stripes; i++) {
			if (stripe->dev->devid !=
			    btrfs_raid_stride_devid_nr(leaf, extent, i))
				continue;
			stripe->physical = btrfs_raid_stride_offset_nr(leaf, extent, i);
			btrfs_release_path(&path);
			return 0;
		}
next:
		path.slots[0]++;
	}

	btrfs_release_path(&path);
error:
	return ret;
}

int __btrfs_map_block(struct btrfs_fs_info *fs_info, int rw,
		      u64 logical, u64 *length, u64 *type,
		      struct btrfs_multi_bio **multi_ret, int mirror_num,
		      u64 **raid_map_ret)
{
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	struct cache_extent *ce;
	struct map_lookup *map;
	u64 offset;
	u64 stripe_offset;
	u64 stripe_nr;
	u64 *raid_map = NULL;
	int stripes_allocated = 8;
	int stripes_required = 1;
	int stripe_index;
	int i;
	bool need_raid_map = false;
	struct btrfs_multi_bio *multi = NULL;

	if (multi_ret && rw == READ) {
		stripes_allocated = 1;
	}
again:
	ce = search_cache_extent(&map_tree->cache_tree, logical);
	if (!ce) {
		kfree(multi);
		*length = (u64)-1;
		return -ENOENT;
	}
	if (ce->start > logical) {
		kfree(multi);
		*length = ce->start - logical;
		return -ENOENT;
	}

	if (multi_ret) {
		multi = kzalloc(btrfs_multi_bio_size(stripes_allocated),
				GFP_NOFS);
		if (!multi)
			return -ENOMEM;
	}
	map = container_of(ce, struct map_lookup, ce);
	offset = logical - ce->start;

	if (rw == WRITE) {
		if (map->type & (BTRFS_BLOCK_GROUP_RAID1_MASK |
				 BTRFS_BLOCK_GROUP_DUP)) {
			stripes_required = map->num_stripes;
		} else if (map->type & BTRFS_BLOCK_GROUP_RAID10) {
			stripes_required = map->sub_stripes;
		}
	}
	if (map->type & BTRFS_BLOCK_GROUP_RAID56_MASK
	    && multi_ret && ((rw & WRITE) || mirror_num > 1) && raid_map_ret) {
		need_raid_map = true;
		/* RAID[56] write or recovery. Return all stripes */
		stripes_required = map->num_stripes;

		/* Only allocate the map if we've already got a large enough multi_ret */
		if (stripes_allocated >= stripes_required) {
			raid_map = kmalloc(sizeof(u64) * map->num_stripes, GFP_NOFS);
			if (!raid_map) {
				kfree(multi);
				return -ENOMEM;
			}
		}
	}

	/* if our multi bio struct is too small, back off and try again */
	if (multi_ret && stripes_allocated < stripes_required) {
		stripes_allocated = stripes_required;
		kfree(multi);
		multi = NULL;
		goto again;
	}
	stripe_nr = offset;
	/*
	 * stripe_nr counts the total number of stripes we have to stride
	 * to get to this block
	 */
	stripe_nr = stripe_nr / map->stripe_len;

	stripe_offset = stripe_nr * map->stripe_len;
	BUG_ON(offset < stripe_offset);

	/* stripe_offset is the offset of this block in its stripe*/
	stripe_offset = offset - stripe_offset;

	if (map->type & (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1_MASK |
			 BTRFS_BLOCK_GROUP_RAID56_MASK |
			 BTRFS_BLOCK_GROUP_RAID10 |
			 BTRFS_BLOCK_GROUP_DUP)) {
		/* we limit the length of each bio to what fits in a stripe */
		*length = min_t(u64, ce->size - offset,
			      map->stripe_len - stripe_offset);
	} else {
		*length = ce->size - offset;
	}

	if (!multi_ret)
		goto out;

	multi->num_stripes = 1;
	multi->type = map->type;
	stripe_index = 0;
	if (map->type & BTRFS_BLOCK_GROUP_RAID1_MASK) {
		if (rw == WRITE)
			multi->num_stripes = map->num_stripes;
		else if (mirror_num)
			stripe_index = mirror_num - 1;
		else
			stripe_index = stripe_nr % map->num_stripes;
	} else if (map->type & BTRFS_BLOCK_GROUP_RAID10) {
		int factor = map->num_stripes / map->sub_stripes;

		stripe_index = stripe_nr % factor;
		stripe_index *= map->sub_stripes;

		if (rw == WRITE)
			multi->num_stripes = map->sub_stripes;
		else if (mirror_num)
			stripe_index += mirror_num - 1;

		stripe_nr = stripe_nr / factor;
	} else if (map->type & BTRFS_BLOCK_GROUP_DUP) {
		if (rw == WRITE)
			multi->num_stripes = map->num_stripes;
		else if (mirror_num)
			stripe_index = mirror_num - 1;
	} else if (map->type & BTRFS_BLOCK_GROUP_RAID56_MASK) {
		if (need_raid_map && raid_map) {
			int rot;
			u64 tmp;
			u64 raid56_full_stripe_start;
			u64 full_stripe_len = nr_data_stripes(map) * map->stripe_len;

			/*
			 * align the start of our data stripe in the logical
			 * address space
			 */
			raid56_full_stripe_start = offset / full_stripe_len;
			raid56_full_stripe_start *= full_stripe_len;

			/* get the data stripe number */
			stripe_nr = raid56_full_stripe_start / map->stripe_len;
			stripe_nr = stripe_nr / nr_data_stripes(map);

			/* Work out the disk rotation on this stripe-set */
			rot = stripe_nr % map->num_stripes;

			/* Fill in the logical address of each stripe */
			tmp = stripe_nr * nr_data_stripes(map);

			for (i = 0; i < nr_data_stripes(map); i++)
				raid_map[(i+rot) % map->num_stripes] =
					ce->start + (tmp + i) * map->stripe_len;

			raid_map[(i+rot) % map->num_stripes] = BTRFS_RAID5_P_STRIPE;
			if (map->type & BTRFS_BLOCK_GROUP_RAID6)
				raid_map[(i+rot+1) % map->num_stripes] = BTRFS_RAID6_Q_STRIPE;

			*length = map->stripe_len;
			stripe_index = 0;
			stripe_offset = 0;
			multi->num_stripes = map->num_stripes;
		} else {
			stripe_index = stripe_nr % nr_data_stripes(map);
			stripe_nr = stripe_nr / nr_data_stripes(map);

			/*
			 * Mirror #0 or #1 means the original data block.
			 * Mirror #2 is RAID5 parity block.
			 * Mirror #3 is RAID6 Q block.
			 */
			if (mirror_num > 1)
				stripe_index = nr_data_stripes(map) + mirror_num - 2;

			/* We distribute the parity blocks across stripes */
			stripe_index = (stripe_nr + stripe_index) % map->num_stripes;
		}
	} else {
		/*
		 * after this do_div call, stripe_nr is the number of stripes
		 * on this device we have to walk to find the data, and
		 * stripe_index is the number of our device in the stripe array
		 */
		stripe_index = stripe_nr % map->num_stripes;
		stripe_nr = stripe_nr / map->num_stripes;
	}
	BUG_ON(stripe_index >= map->num_stripes);

	for (i = 0; i < multi->num_stripes; i++) {
		multi->stripes[i].dev = map->stripes[stripe_index].dev;

		if (stripes_allocated &&
		    btrfs_need_stripe_tree_update(fs_info, map->type)) {
			int ret;

			ret = btrfs_stripe_tree_logical_to_physical(fs_info, logical,
								    &multi->stripes[i]);
			if (ret)
				return ret;
		} else {
			multi->stripes[i].physical =
				map->stripes[stripe_index].physical +
				stripe_offset + stripe_nr * map->stripe_len;
		}
		stripe_index++;
	}
	*multi_ret = multi;

	if (type)
		*type = map->type;

	if (raid_map) {
		sort_parity_stripes(multi, raid_map);
		*raid_map_ret = raid_map;
	}
out:
	return 0;
}

struct btrfs_device *btrfs_find_device(struct btrfs_fs_info *fs_info, u64 devid,
				       u8 *uuid, u8 *fsid)
{
	struct btrfs_device *device;
	struct btrfs_fs_devices *cur_devices;

	cur_devices = fs_info->fs_devices;
	while (cur_devices) {
		if (!fsid ||
		    (!memcmp(cur_devices->metadata_uuid, fsid, BTRFS_FSID_SIZE) ||
		     fs_info->ignore_fsid_mismatch)) {
			device = find_device(cur_devices, devid, uuid);
			if (device)
				return device;
		}
		cur_devices = cur_devices->seed;
	}
	return NULL;
}

struct btrfs_device *
btrfs_find_device_by_devid(struct btrfs_fs_devices *fs_devices,
			   u64 devid, int instance)
{
	struct list_head *head = &fs_devices->devices;
	struct btrfs_device *dev;
	int num_found = 0;

	list_for_each_entry(dev, head, dev_list) {
		if (dev->devid == devid && num_found++ == instance)
			return dev;
	}
	return NULL;
}

/*
 * Return 0 if the chunk at @chunk_offset exists and is not read-only.
 * Return 1 if the chunk at @chunk_offset exists and is read-only.
 * Return <0 if we can't find chunk at @chunk_offset.
 */
int btrfs_chunk_readonly(struct btrfs_fs_info *fs_info, u64 chunk_offset)
{
	struct cache_extent *ce;
	struct map_lookup *map;
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	int readonly = 0;
	int i;

	/*
	 * During chunk recovering, we may fail to find block group's
	 * corresponding chunk, we will rebuild it later
	 */
	if (fs_info->is_chunk_recover)
		return 0;

	ce = search_cache_extent(&map_tree->cache_tree, chunk_offset);
	if (!ce)
		return -ENOENT;

	map = container_of(ce, struct map_lookup, ce);
	for (i = 0; i < map->num_stripes; i++) {
		if (!map->stripes[i].dev->writeable) {
			readonly = 1;
			break;
		}
	}

	return readonly;
}

static struct btrfs_device *fill_missing_device(u64 devid, const u8 *uuid)
{
	struct btrfs_device *device;

	device = kzalloc(sizeof(*device), GFP_NOFS);
	device->devid = devid;
	memcpy(device->uuid, uuid, BTRFS_UUID_SIZE);
	device->fd = -1;
	return device;
}

/*
 * Slot is used to verify the chunk item is valid
 *
 * For sys chunk in superblock, pass -1 to indicate sys chunk.
 */
static int read_one_chunk(struct btrfs_fs_info *fs_info, struct btrfs_key *key,
			  struct extent_buffer *leaf,
			  struct btrfs_chunk *chunk, int slot)
{
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	struct map_lookup *map;
	struct cache_extent *ce;
	u64 logical;
	u64 length;
	u64 devid;
	u8 uuid[BTRFS_UUID_SIZE];
	int num_stripes;
	int ret;
	int i;

	logical = key->offset;
	length = btrfs_chunk_length(leaf, chunk);
	num_stripes = btrfs_chunk_num_stripes(leaf, chunk);
	/* Validation check */
	ret = btrfs_check_chunk_valid(leaf, chunk, logical);
	if (ret) {
		error("%s checksums match, but it has an invalid chunk, %s",
		      (slot == -1) ? "Superblock" : "Metadata",
		      (slot == -1) ? "try btrfsck --repair -s <superblock> ie, 0,1,2" : "");
		return ret;
	}

	ce = search_cache_extent(&map_tree->cache_tree, logical);

	/* already mapped? */
	if (ce && ce->start <= logical && ce->start + ce->size > logical) {
		return 0;
	}

	map = kmalloc(btrfs_map_lookup_size(num_stripes), GFP_NOFS);
	if (!map)
		return -ENOMEM;

	map->ce.start = logical;
	map->ce.size = length;
	map->num_stripes = num_stripes;
	map->io_width = btrfs_chunk_io_width(leaf, chunk);
	map->io_align = btrfs_chunk_io_align(leaf, chunk);
	map->sector_size = btrfs_chunk_sector_size(leaf, chunk);
	map->stripe_len = btrfs_chunk_stripe_len(leaf, chunk);
	map->type = btrfs_chunk_type(leaf, chunk);
	map->sub_stripes = btrfs_chunk_sub_stripes(leaf, chunk);

	for (i = 0; i < num_stripes; i++) {
		map->stripes[i].physical =
			btrfs_stripe_offset_nr(leaf, chunk, i);
		devid = btrfs_stripe_devid_nr(leaf, chunk, i);
		read_extent_buffer(leaf, uuid, (unsigned long)
				   btrfs_stripe_dev_uuid_nr(chunk, i),
				   BTRFS_UUID_SIZE);
		map->stripes[i].dev = btrfs_find_device(fs_info, devid, uuid,
							NULL);
		if (!map->stripes[i].dev) {
			map->stripes[i].dev = fill_missing_device(devid, uuid);
			warning("warning, device %llu is missing\n",
			       (unsigned long long)devid);
			list_add(&map->stripes[i].dev->dev_list,
				 &fs_info->fs_devices->devices);
			fs_info->fs_devices->missing_devices++;
		}

	}
	ret = insert_cache_extent(&map_tree->cache_tree, &map->ce);
	if (ret < 0) {
		errno = -ret;
		error("failed to add chunk map start=%llu len=%llu: %d (%m)",
		      map->ce.start, map->ce.size, ret);
	}

	return ret;
}

static int fill_device_from_item(struct extent_buffer *leaf,
				 struct btrfs_dev_item *dev_item,
				 struct btrfs_device *device)
{
	unsigned long ptr;

	device->devid = btrfs_device_id(leaf, dev_item);
	device->total_bytes = btrfs_device_total_bytes(leaf, dev_item);
	device->bytes_used = btrfs_device_bytes_used(leaf, dev_item);
	device->type = btrfs_device_type(leaf, dev_item);
	device->io_align = btrfs_device_io_align(leaf, dev_item);
	device->io_width = btrfs_device_io_width(leaf, dev_item);
	device->sector_size = btrfs_device_sector_size(leaf, dev_item);

	ptr = (unsigned long)btrfs_device_uuid(dev_item);
	read_extent_buffer(leaf, device->uuid, ptr, BTRFS_UUID_SIZE);

	return 0;
}

static int open_seed_devices(struct btrfs_fs_info *fs_info, u8 *fsid)
{
	struct btrfs_fs_devices *fs_devices;
	int ret;

	fs_devices = fs_info->fs_devices->seed;
	while (fs_devices) {
		if (!memcmp(fs_devices->fsid, fsid, BTRFS_UUID_SIZE)) {
			ret = 0;
			goto out;
		}
		fs_devices = fs_devices->seed;
	}

	fs_devices = find_fsid(fsid, NULL);
	if (!fs_devices) {
		/* missing all seed devices */
		fs_devices = kzalloc(sizeof(*fs_devices), GFP_NOFS);
		if (!fs_devices) {
			ret = -ENOMEM;
			goto out;
		}
		INIT_LIST_HEAD(&fs_devices->devices);
		list_add(&fs_devices->fs_list, &fs_uuids);
		memcpy(fs_devices->fsid, fsid, BTRFS_FSID_SIZE);
	}

	ret = btrfs_open_devices(fs_info, fs_devices, O_RDONLY);
	if (ret)
		goto out;

	fs_devices->seed = fs_info->fs_devices->seed;
	fs_info->fs_devices->seed = fs_devices;
out:
	return ret;
}

static int read_one_dev(struct btrfs_fs_info *fs_info,
			struct extent_buffer *leaf,
			struct btrfs_dev_item *dev_item)
{
	struct btrfs_device *device;
	u64 devid;
	int ret = 0;
	u8 fs_uuid[BTRFS_UUID_SIZE];
	u8 dev_uuid[BTRFS_UUID_SIZE];

	devid = btrfs_device_id(leaf, dev_item);
	read_extent_buffer(leaf, dev_uuid,
			   (unsigned long)btrfs_device_uuid(dev_item),
			   BTRFS_UUID_SIZE);
	read_extent_buffer(leaf, fs_uuid,
			   (unsigned long)btrfs_device_fsid(dev_item),
			   BTRFS_FSID_SIZE);

	if (memcmp(fs_uuid, fs_info->fs_devices->fsid, BTRFS_UUID_SIZE)) {
		ret = open_seed_devices(fs_info, fs_uuid);
		if (ret)
			return ret;
	}

	device = btrfs_find_device(fs_info, devid, dev_uuid, fs_uuid);
	if (!device) {
		device = kzalloc(sizeof(*device), GFP_NOFS);
		if (!device)
			return -ENOMEM;
		device->fd = -1;
		list_add(&device->dev_list,
			 &fs_info->fs_devices->devices);
		fs_info->fs_devices->missing_devices++;
	}

	fill_device_from_item(leaf, dev_item, device);
	device->dev_root = fs_info->dev_root;
	fs_info->fs_devices->total_rw_bytes +=
		btrfs_device_total_bytes(leaf, dev_item);
	return ret;
}

int btrfs_read_sys_array(struct btrfs_fs_info *fs_info)
{
	struct btrfs_super_block *super_copy = fs_info->super_copy;
	struct extent_buffer *sb;
	struct btrfs_disk_key *disk_key;
	struct btrfs_chunk *chunk;
	u8 *array_ptr;
	unsigned long sb_array_offset;
	int ret = 0;
	u32 num_stripes;
	u32 array_size;
	u32 len = 0;
	u32 cur_offset;
	struct btrfs_key key;

	if (fs_info->nodesize < BTRFS_SUPER_INFO_SIZE) {
		printf("ERROR: nodesize %u too small to read superblock\n",
				fs_info->nodesize);
		return -EINVAL;
	}
	sb = alloc_dummy_extent_buffer(fs_info, BTRFS_SUPER_INFO_OFFSET,
				       BTRFS_SUPER_INFO_SIZE);
	if (!sb)
		return -ENOMEM;
	btrfs_set_buffer_uptodate(sb);
	write_extent_buffer(sb, super_copy, 0, sizeof(*super_copy));
	array_size = btrfs_super_sys_array_size(super_copy);

	array_ptr = super_copy->sys_chunk_array;
	sb_array_offset = offsetof(struct btrfs_super_block, sys_chunk_array);
	cur_offset = 0;

	while (cur_offset < array_size) {
		disk_key = (struct btrfs_disk_key *)array_ptr;
		len = sizeof(*disk_key);
		if (cur_offset + len > array_size)
			goto out_short_read;

		btrfs_disk_key_to_cpu(&key, disk_key);

		array_ptr += len;
		sb_array_offset += len;
		cur_offset += len;

		if (key.type == BTRFS_CHUNK_ITEM_KEY) {
			chunk = (struct btrfs_chunk *)sb_array_offset;
			/*
			 * At least one btrfs_chunk with one stripe must be
			 * present, exact stripe count check comes afterwards
			 */
			len = btrfs_chunk_item_size(1);
			if (cur_offset + len > array_size)
				goto out_short_read;

			num_stripes = btrfs_chunk_num_stripes(sb, chunk);
			if (!num_stripes) {
				printk(
	    "ERROR: invalid number of stripes %u in sys_array at offset %u\n",
					num_stripes, cur_offset);
				ret = -EIO;
				break;
			}

			len = btrfs_chunk_item_size(num_stripes);
			if (cur_offset + len > array_size)
				goto out_short_read;

			ret = read_one_chunk(fs_info, &key, sb, chunk, -1);
			if (ret)
				break;
		} else {
			printk(
		"ERROR: unexpected item type %u in sys_array at offset %u\n",
				(u32)key.type, cur_offset);
 			ret = -EIO;
 			break;
		}
		array_ptr += len;
		sb_array_offset += len;
		cur_offset += len;
	}
	free_extent_buffer(sb);
	return ret;

out_short_read:
	printk("ERROR: sys_array too short to read %u bytes at offset %u\n",
			len, cur_offset);
	free_extent_buffer(sb);
	return -EIO;
}

int btrfs_read_chunk_tree(struct btrfs_fs_info *fs_info)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_root *root = fs_info->chunk_root;
	int ret;
	int slot;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Read all device items, and then all the chunk items. All
	 * device items are found before any chunk item (their object id
	 * is smaller than the lowest possible object id for a chunk
	 * item - BTRFS_FIRST_CHUNK_TREE_OBJECTID).
	 */
	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = 0;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto error;
	while(1) {
		leaf = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto error;
			break;
		}
		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		if (found_key.type == BTRFS_DEV_ITEM_KEY) {
			struct btrfs_dev_item *dev_item;
			dev_item = btrfs_item_ptr(leaf, slot,
						  struct btrfs_dev_item);
			ret = read_one_dev(fs_info, leaf, dev_item);
			if (ret < 0)
				goto error;
		} else if (found_key.type == BTRFS_CHUNK_ITEM_KEY) {
			struct btrfs_chunk *chunk;
			chunk = btrfs_item_ptr(leaf, slot, struct btrfs_chunk);
			ret = read_one_chunk(fs_info, &found_key, leaf, chunk,
					     slot);
			if (ret < 0)
				goto error;
		}
		path->slots[0]++;
	}

	ret = 0;
error:
	btrfs_free_path(path);
	return ret;
}

struct list_head *btrfs_scanned_uuids(void)
{
	return &fs_uuids;
}

static int rmw_eb(struct btrfs_fs_info *info,
		  struct extent_buffer *eb, struct extent_buffer *orig_eb)
{
	int ret;
	unsigned long orig_off = 0;
	unsigned long dest_off = 0;
	unsigned long copy_len = eb->len;

	ret = read_whole_eb(info, eb, 0);
	if (ret)
		return ret;

	if (eb->start + eb->len <= orig_eb->start ||
	    eb->start >= orig_eb->start + orig_eb->len)
		return 0;
	/*
	 * | ----- orig_eb ------- |
	 *         | ----- stripe -------  |
	 *         | ----- orig_eb ------- |
	 *              | ----- orig_eb ------- |
	 */
	if (eb->start > orig_eb->start)
		orig_off = eb->start - orig_eb->start;
	if (orig_eb->start > eb->start)
		dest_off = orig_eb->start - eb->start;

	if (copy_len > orig_eb->len - orig_off)
		copy_len = orig_eb->len - orig_off;
	if (copy_len > eb->len - dest_off)
		copy_len = eb->len - dest_off;

	memcpy(eb->data + dest_off, orig_eb->data + orig_off, copy_len);
	return 0;
}

static int split_eb_for_raid56(struct btrfs_fs_info *info,
			       struct extent_buffer *orig_eb,
			       struct extent_buffer **ebs,
			       u64 stripe_len, u64 *raid_map,
			       int num_stripes)
{
	struct extent_buffer **tmp_ebs;
	u64 start = orig_eb->start;
	u64 this_eb_start;
	int i;
	int ret = 0;

	tmp_ebs = calloc(num_stripes, sizeof(*tmp_ebs));
	if (!tmp_ebs)
		return -ENOMEM;

	/* Alloc memory in a row for data stripes */
	for (i = 0; i < num_stripes; i++) {
		if (raid_map[i] >= BTRFS_RAID5_P_STRIPE)
			break;

		tmp_ebs[i] = calloc(1, sizeof(**tmp_ebs) + stripe_len);
		if (!tmp_ebs[i]) {
			ret = -ENOMEM;
			goto clean_up;
		}
	}

	for (i = 0; i < num_stripes; i++) {
		struct extent_buffer *eb = tmp_ebs[i];

		if (raid_map[i] >= BTRFS_RAID5_P_STRIPE)
			break;

		eb->start = raid_map[i];
		eb->len = stripe_len;
		eb->refs = 1;
		eb->flags = 0;
		eb->fs_info = info;

		this_eb_start = raid_map[i];

		if (start > this_eb_start ||
		    start + orig_eb->len < this_eb_start + stripe_len) {
			ret = rmw_eb(info, eb, orig_eb);
			if (ret)
				goto clean_up;
		} else {
			memcpy(eb->data, orig_eb->data + eb->start - start,
			       stripe_len);
		}
		ebs[i] = eb;
	}
	kfree(tmp_ebs);
	return ret;
clean_up:
	for (i = 0; i < num_stripes; i++)
		kfree(tmp_ebs[i]);
	kfree(tmp_ebs);
	return ret;
}

int write_raid56_with_parity(struct btrfs_fs_info *info,
			     struct extent_buffer *eb,
			     struct btrfs_multi_bio *multi,
			     u64 stripe_len, u64 *raid_map)
{
	struct extent_buffer **ebs, *p_eb = NULL, *q_eb = NULL;
	int i;
	int ret;
	int alloc_size = eb->len;
	void **pointers;

	ebs = kmalloc(sizeof(*ebs) * multi->num_stripes, GFP_KERNEL);
	pointers = kmalloc(sizeof(*pointers) * multi->num_stripes, GFP_KERNEL);
	if (!ebs || !pointers) {
		kfree(ebs);
		kfree(pointers);
		return -ENOMEM;
	}

	if (stripe_len > alloc_size)
		alloc_size = stripe_len;

	ret = split_eb_for_raid56(info, eb, ebs, stripe_len, raid_map,
				  multi->num_stripes);
	if (ret)
		goto out;

	for (i = 0; i < multi->num_stripes; i++) {
		struct extent_buffer *new_eb;
		if (raid_map[i] < BTRFS_RAID5_P_STRIPE) {
			if (ebs[i]->start != raid_map[i]) {
				ret = -EINVAL;
				goto out_free_split;
			}
			continue;
		}
		new_eb = kmalloc(sizeof(*eb) + alloc_size, GFP_KERNEL);
		if (!new_eb) {
			ret = -ENOMEM;
			goto out_free_split;
		}
		multi->stripes[i].dev->total_ios++;
		new_eb->len = stripe_len;
		new_eb->fs_info = info;

		if (raid_map[i] == BTRFS_RAID5_P_STRIPE)
			p_eb = new_eb;
		else if (raid_map[i] == BTRFS_RAID6_Q_STRIPE)
			q_eb = new_eb;
	}
	if (q_eb) {
		ebs[multi->num_stripes - 2] = p_eb;
		ebs[multi->num_stripes - 1] = q_eb;

		for (i = 0; i < multi->num_stripes; i++)
			pointers[i] = ebs[i]->data;

		raid6_gen_syndrome(multi->num_stripes, stripe_len, pointers);
	} else {
		ebs[multi->num_stripes - 1] = p_eb;
		for (i = 0; i < multi->num_stripes; i++)
			pointers[i] = ebs[i]->data;
		ret = raid5_gen_result(multi->num_stripes, stripe_len,
				       multi->num_stripes - 1, pointers);
		if (ret < 0)
			goto out_free_split;
	}

	for (i = 0; i < multi->num_stripes; i++) {
		multi->stripes[i].dev->total_ios++;
		ret = btrfs_pwrite(multi->stripes[i].dev->fd, ebs[i]->data, ebs[i]->len,
				   multi->stripes[i].physical, info->zoned);
		if (ret < 0)
			goto out_free_split;
	}

out_free_split:
	for (i = 0; i < multi->num_stripes; i++) {
		if (ebs[i] != eb)
			kfree(ebs[i]);
	}
out:
	kfree(ebs);
	kfree(pointers);

	return ret;
}

/*
 * Get stripe length from chunk item and its stripe items
 *
 * Caller should only call this function after validating the chunk item
 * by using btrfs_check_chunk_valid().
 */
u64 btrfs_stripe_length(struct btrfs_fs_info *fs_info,
			struct extent_buffer *leaf,
			struct btrfs_chunk *chunk)
{
	u64 stripe_len;
	u64 chunk_len;
	u32 num_stripes = btrfs_chunk_num_stripes(leaf, chunk);
	u64 profile = btrfs_chunk_type(leaf, chunk) &
		      BTRFS_BLOCK_GROUP_PROFILE_MASK;

	chunk_len = btrfs_chunk_length(leaf, chunk);
	stripe_len = chunk_len;

	switch (profile) {
	case 0: /* Single profile */
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID1C3:
	case BTRFS_BLOCK_GROUP_RAID1C4:
	case BTRFS_BLOCK_GROUP_DUP:
		/* The default value is already fine. */
		break;
	case BTRFS_BLOCK_GROUP_RAID0:
		stripe_len = chunk_len / num_stripes;
		break;
	case BTRFS_BLOCK_GROUP_RAID5:
	case BTRFS_BLOCK_GROUP_RAID6:
		stripe_len = chunk_len / (num_stripes - btrfs_bg_type_to_nparity(profile));
		break;
	case BTRFS_BLOCK_GROUP_RAID10:
		stripe_len = chunk_len / (num_stripes /
				btrfs_chunk_sub_stripes(leaf, chunk));
		break;
	default:
		/* Invalid chunk profile found */
		BUG_ON(1);
	}
	return stripe_len;
}

/*
 * Return <0 for error.
 * Return >0 if we can not find any dev extent beyond @physical
 * REturn 0 if we can find any dev extent beyond @physical or covers @physical.
 */
static int check_dev_extent_beyond_bytenr(struct btrfs_fs_info *fs_info,
					  struct btrfs_device *device,
					  u64 physical)
{
	struct btrfs_root *root = fs_info->dev_root;
	struct btrfs_path path = { 0 };
	struct btrfs_dev_extent *dext;
	struct btrfs_key key;
	u64 dext_len;
	u64 last_dev_extent_end = 0;
	int ret;

	key.objectid = device->devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;
	if (ret == 0) {
		ret = -EUCLEAN;
		error("invalid dev extent found for devid %llu", device->devid);
		goto out;
	}

	ret = btrfs_previous_item(root, &path, device->devid, BTRFS_DEV_EXTENT_KEY);
	/*
	 * Either <0 we error out, or ret > 0 we can not find any dev extent
	 * for this device, then last_dev_extent_end will be 0 and we will
	 * return 1.
	 */
	if (ret)
		goto out;

	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	dext = btrfs_item_ptr(path.nodes[0], path.slots[0], struct btrfs_dev_extent);
	dext_len = btrfs_dev_extent_length(path.nodes[0], dext);
	last_dev_extent_end = dext_len + key.offset;

out:
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;
	if (last_dev_extent_end <= physical)
		return 1;
	return 0;
}

static int reset_device_item_total_bytes(struct btrfs_fs_info *fs_info,
					 struct btrfs_device *device,
					 u64 new_size)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	struct btrfs_path path = { 0 };
	struct btrfs_root *chunk_root = fs_info->chunk_root;
	struct btrfs_dev_item *di;
	u64 old_bytes = device->total_bytes;
	int ret;

	ASSERT(IS_ALIGNED(new_size, fs_info->sectorsize));

	/* Align the in-memory total_bytes first, and use it as correct size */
	device->total_bytes = new_size;

	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = device->devid;

	trans = btrfs_start_transaction(chunk_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	ret = btrfs_search_slot(trans, chunk_root, &key, &path, 0, 1);
	if (ret > 0) {
		error("failed to find DEV_ITEM for devid %llu", device->devid);
		ret = -ENOENT;
		goto err;
	}
	if (ret < 0) {
		errno = -ret;
		error("failed to search chunk root: %d (%m)", ret);
		goto err;
	}
	di = btrfs_item_ptr(path.nodes[0], path.slots[0], struct btrfs_dev_item);
	btrfs_set_device_total_bytes(path.nodes[0], di, device->total_bytes);
	btrfs_mark_buffer_dirty(path.nodes[0]);
	ret = btrfs_commit_transaction(trans, chunk_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		btrfs_release_path(&path);
		return ret;
	}
	btrfs_release_path(&path);
	printf("Fixed device size for devid %llu, old size: %llu new size: %llu\n",
		device->devid, old_bytes, device->total_bytes);
	return 1;

err:
	/* We haven't modified anything, it's OK to commit current trans */
	btrfs_commit_transaction(trans, chunk_root);
	btrfs_release_path(&path);
	return ret;
}

static int btrfs_fix_block_device_size(struct btrfs_fs_info *fs_info,
				       struct btrfs_device *device)
{
	struct stat st;
	u64 block_dev_size;
	int ret;

	if (device->fd < 0 || !device->writeable) {
		error("devid %llu is missing or not writable", device->devid);
		return -EINVAL;
	}

	ret = fstat(device->fd, &st);
	if (ret < 0) {
		error("failed to get block device size for devid %llu: %m",
		      device->devid);
		return -errno;
	}

	block_dev_size = round_down(device_get_partition_size_fd_stat(device->fd, &st),
				    fs_info->sectorsize);

	/*
	 * Total_bytes in device item is no larger than the device block size,
	 * already the correct case.
	 */
	if (device->total_bytes <= block_dev_size)
		return 0;

	/*
	 * Now we need to check if there is any device extent beyond
	 * @block_dev_size.
	 */
	ret = check_dev_extent_beyond_bytenr(fs_info, device, block_dev_size);
	if (ret < 0)
		return ret;

	if (ret == 0) {
		error(
"found dev extents covering or beyond bytenr %llu, can not shrink the device without losing data",
			device->devid);
		return -EINVAL;
	}

	/* Now we can shrink the device item total_bytes to @block_dev_size. */
	return reset_device_item_total_bytes(fs_info, device, block_dev_size);
}

/*
 * Return 0 if size of @device is already good
 * Return >0 if size of @device is not aligned but fixed without problems
 * Return <0 if something wrong happened when aligning the size of @device
 */
int btrfs_fix_device_size(struct btrfs_fs_info *fs_info, struct btrfs_device *device)
{
	u64 old_bytes = device->total_bytes;

	/*
	 * Our value is already good, then check if it's device item mismatch against
	 * block device size.
	 */
	if (IS_ALIGNED(old_bytes, fs_info->sectorsize))
		return btrfs_fix_block_device_size(fs_info, device);

	return reset_device_item_total_bytes(fs_info, device,
			round_down(old_bytes, fs_info->sectorsize));
}

/*
 * Return 0 if super block total_bytes matches all devices' total_bytes
 * Return >0 if super block total_bytes mismatch but fixed without problem
 * Return <0 if we failed to fix super block total_bytes
 */
int btrfs_fix_super_size(struct btrfs_fs_info *fs_info)
{
	struct btrfs_device *device;
	struct list_head *dev_list = &fs_info->fs_devices->devices;
	u64 total_bytes = 0;
	u64 old_bytes = btrfs_super_total_bytes(fs_info->super_copy);
	int ret;

	list_for_each_entry(device, dev_list, dev_list) {
		/*
		 * Caller should ensure this function is called after aligning
		 * all devices' total_bytes.
		 */
		if (!IS_ALIGNED(device->total_bytes, fs_info->sectorsize)) {
			error("device %llu total_bytes %llu not aligned to %u",
				device->devid, device->total_bytes,
				fs_info->sectorsize);
			return -EUCLEAN;
		}
		total_bytes += device->total_bytes;
	}

	if (total_bytes == old_bytes)
		return 0;

	btrfs_set_super_total_bytes(fs_info->super_copy, total_bytes);
	/* Do not use transaction for overwriting only the super block */
	ret = write_all_supers(fs_info);
	if (ret < 0) {
		errno = -ret;
		error("failed to write super blocks: %d (%m)", ret);
		return ret;
	}
	printf("Fixed super total bytes, old size: %llu new size: %llu\n",
		old_bytes, total_bytes);
	return 1;
}

/*
 * Return 0 if all devices and super block sizes are good
 * Return >0 if any device/super size problem was found, but fixed
 * Return <0 if something wrong happened during fixing
 */
int btrfs_fix_device_and_super_size(struct btrfs_fs_info *fs_info)
{
	struct btrfs_device *device;
	struct list_head *dev_list = &fs_info->fs_devices->devices;
	bool have_bad_value = false;
	int ret;

	/* Seed device is not supported yet */
	if (fs_info->fs_devices->seed) {
		error("fixing device size with seed device is not supported yet");
		return -EOPNOTSUPP;
	}

	/* All devices must be set up before repairing */
	if (list_empty(dev_list)) {
		error("no device found");
		return -ENODEV;
	}
	list_for_each_entry(device, dev_list, dev_list) {
		if (device->fd == -1 || !device->writeable) {
			error("devid %llu is missing or not writeable",
			      device->devid);
			error(
	"fixing device size needs all device(s) to be present and writeable");
			return -ENODEV;
		}
	}

	/* Repair total_bytes of each device */
	list_for_each_entry(device, dev_list, dev_list) {
		ret = btrfs_fix_device_size(fs_info, device);
		if (ret < 0)
			return ret;
		if (ret > 0)
			have_bad_value = true;
	}

	/* Repair super total_byte */
	ret = btrfs_fix_super_size(fs_info);
	if (ret > 0)
		have_bad_value = true;
	if (have_bad_value) {
		printf(
	"Fixed unaligned/mismatched total_bytes for super block and device items\n");
		ret = 1;
	} else {
		printf("No device size related problem found\n");
		ret = 0;
	}
	return ret;
}
