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
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <fcntl.h>
#include <unistd.h>
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "print-tree.h"
#include "volumes.h"
#include "common/utils.h"
#include "kernel-lib/raid56.h"

const struct btrfs_raid_attr btrfs_raid_array[BTRFS_NR_RAID_TYPES] = {
	[BTRFS_RAID_RAID10] = {
		.sub_stripes	= 2,
		.dev_stripes	= 1,
		.devs_max	= 0,	/* 0 == as many as possible */
		.devs_min	= 4,
		.tolerated_failures = 1,
		.devs_increment	= 2,
		.ncopies	= 2,
		.nparity        = 0,
		.raid_name	= "raid10",
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
		.raid_name	= "raid1",
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
	},
	[BTRFS_RAID_RAID1C4] = {
		.sub_stripes	= 1,
		.dev_stripes	= 1,
		.devs_max	= 4,
		.devs_min	= 4,
		.tolerated_failures = 3,
		.devs_increment	= 4,
		.ncopies	= 4,
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
		.raid_name	= "dup",
		.bg_flag	= BTRFS_BLOCK_GROUP_DUP,
		.mindev_error	= 0,
	},
	[BTRFS_RAID_RAID0] = {
		.sub_stripes	= 1,
		.dev_stripes	= 1,
		.devs_max	= 0,
		.devs_min	= 2,
		.tolerated_failures = 0,
		.devs_increment	= 1,
		.ncopies	= 1,
		.nparity        = 0,
		.raid_name	= "raid0",
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
		.raid_name	= "single",
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
		.raid_name	= "raid5",
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
		.raid_name	= "raid6",
		.bg_flag	= BTRFS_BLOCK_GROUP_RAID6,
		.mindev_error	= BTRFS_ERROR_DEV_RAID6_MIN_NOT_MET,
	},
};

struct stripe {
	struct btrfs_device *dev;
	u64 physical;
};

static inline int nr_parity_stripes(struct map_lookup *map)
{
	if (map->type & BTRFS_BLOCK_GROUP_RAID5)
		return 1;
	else if (map->type & BTRFS_BLOCK_GROUP_RAID6)
		return 2;
	else
		return 0;
}

static inline int nr_data_stripes(struct map_lookup *map)
{
	return map->num_stripes - nr_parity_stripes(map);
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

	list_for_each_entry(fs_devices, &fs_uuids, list) {
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

static int device_list_add(const char *path,
			   struct btrfs_super_block *disk_super,
			   u64 devid, struct btrfs_fs_devices **fs_devices_ret)
{
	struct btrfs_device *device;
	struct btrfs_fs_devices *fs_devices;
	u64 found_transid = btrfs_super_generation(disk_super);
	bool metadata_uuid = (btrfs_super_incompat_flags(disk_super) &
		BTRFS_FEATURE_INCOMPAT_METADATA_UUID);

	if (metadata_uuid)
		fs_devices = find_fsid(disk_super->fsid,
				       disk_super->metadata_uuid);
	else
		fs_devices = find_fsid(disk_super->fsid, NULL);

	if (!fs_devices) {
		fs_devices = kzalloc(sizeof(*fs_devices), GFP_NOFS);
		if (!fs_devices)
			return -ENOMEM;
		INIT_LIST_HEAD(&fs_devices->devices);
		list_add(&fs_devices->list, &fs_uuids);
		memcpy(fs_devices->fsid, disk_super->fsid, BTRFS_FSID_SIZE);
		if (metadata_uuid)
			memcpy(fs_devices->metadata_uuid,
			       disk_super->metadata_uuid, BTRFS_FSID_SIZE);
		else
			memcpy(fs_devices->metadata_uuid, fs_devices->fsid,
			       BTRFS_FSID_SIZE);

		fs_devices->latest_devid = devid;
		fs_devices->latest_trans = found_transid;
		fs_devices->lowest_devid = (u64)-1;
		device = NULL;
	} else {
		device = find_device(fs_devices, devid,
				       disk_super->dev_item.uuid);
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


	if (found_transid > fs_devices->latest_trans) {
		fs_devices->latest_devid = devid;
		fs_devices->latest_trans = found_transid;
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
		free(device->name);
		free(device->label);
		free(device);
	}

	seed_devices = fs_devices->seed;
	fs_devices->seed = NULL;
	if (seed_devices) {
		struct btrfs_fs_devices *orig;

		orig = fs_devices;
		fs_devices = seed_devices;
		list_del(&orig->list);
		free(orig);
		goto again;
	} else {
		list_del(&fs_devices->list);
		free(fs_devices);
	}

	return ret;
}

void btrfs_close_all_devices(void)
{
	struct btrfs_fs_devices *fs_devices;

	while (!list_empty(&fs_uuids)) {
		fs_devices = list_entry(fs_uuids.next, struct btrfs_fs_devices,
					list);
		btrfs_close_devices(fs_devices);
	}
}

int btrfs_open_devices(struct btrfs_fs_devices *fs_devices, int flags)
{
	int fd;
	struct btrfs_device *device;
	int ret;

	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		if (!device->name) {
			printk("no name for device %llu, skip it now\n", device->devid);
			continue;
		}

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
	struct btrfs_super_block *disk_super;
	char buf[BTRFS_SUPER_INFO_SIZE];
	int ret;
	u64 devid;

	disk_super = (struct btrfs_super_block *)buf;
	ret = btrfs_read_dev_super(fd, disk_super, super_offset, sbflags);
	if (ret < 0)
		return -EIO;
	devid = btrfs_stack_device_id(&disk_super->dev_item);
	if (btrfs_super_flags(disk_super) & BTRFS_SUPER_FLAG_METADUMP)
		*total_devs = 1;
	else
		*total_devs = btrfs_super_num_devices(disk_super);

	ret = device_list_add(path, disk_super, devid, fs_devices_ret);

	return ret;
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
	u64 min_search_start;

	/*
	 * We don't want to overwrite the superblock on the drive nor any area
	 * used by the boot loader (grub for example), so we make sure to start
	 * at an offset of at least 1MB.
	 */
	min_search_start = max(root->fs_info->alloc_start, (u64)SZ_1M);
	search_start = max(search_start, min_search_start);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	max_hole_start = search_start;
	max_hole_size = 0;

	if (search_start >= search_end) {
		ret = -ENOSPC;
		goto out;
	}

	path->reada = READA_FORWARD;

	key.objectid = device->devid;
	key.offset = search_start;
	key.type = BTRFS_DEV_EXTENT_KEY;

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

			/*
			 * Have to check before we set max_hole_start, otherwise
			 * we could end up sending back this offset anyway.
			 */
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

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = device->devid;
	key.offset = start;
	key.type = BTRFS_DEV_EXTENT_KEY;
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
	key.offset = (u64)-1;
	key.type = BTRFS_CHUNK_ITEM_KEY;

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

static u64 chunk_bytes_by_type(u64 type, u64 calc_size, int num_stripes,
			       int sub_stripes)
{
	if (type & (BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_DUP))
		return calc_size;
	else if (type & (BTRFS_BLOCK_GROUP_RAID1C3 | BTRFS_BLOCK_GROUP_RAID1C4))
		return calc_size;
	else if (type & BTRFS_BLOCK_GROUP_RAID10)
		return calc_size * (num_stripes / sub_stripes);
	else if (type & BTRFS_BLOCK_GROUP_RAID5)
		return calc_size * (num_stripes - 1);
	else if (type & BTRFS_BLOCK_GROUP_RAID6)
		return calc_size * (num_stripes - 2);
	else
		return calc_size * num_stripes;
}


static u32 find_raid56_stripe_len(u32 data_devices, u32 dev_stripe_target)
{
	/* TODO, add a way to store the preferred stripe size */
	return BTRFS_STRIPE_LEN;
}

/*
 * btrfs_device_avail_bytes - count bytes available for alloc_chunk
 *
 * It is not equal to "device->total_bytes - device->bytes_used".
 * We do not allocate any chunk in 1M at beginning of device, and not
 * allowed to allocate any chunk before alloc_start if it is specified.
 * So search holes from max(1M, alloc_start) to device->total_bytes.
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
	u64 search_start = root->fs_info->alloc_start;
	u64 search_end = device->total_bytes;
	u64 extent_end = 0;
	u64 free_bytes = 0;
	int ret;
	int slot = 0;

	search_start = max(BTRFS_BLOCK_RESERVED_1M_FOR_SUPER, search_start);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = device->devid;
	key.offset = root->fs_info->alloc_start;
	key.type = BTRFS_DEV_EXTENT_KEY;

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

int btrfs_alloc_chunk(struct btrfs_trans_handle *trans,
		      struct btrfs_fs_info *info, u64 *start,
		      u64 *num_bytes, u64 type)
{
	u64 dev_offset;
	struct btrfs_root *extent_root = info->extent_root;
	struct btrfs_root *chunk_root = info->chunk_root;
	struct btrfs_stripe *stripes;
	struct btrfs_device *device = NULL;
	struct btrfs_chunk *chunk;
	struct list_head private_devs;
	struct list_head *dev_list = &info->fs_devices->devices;
	struct list_head *cur;
	struct map_lookup *map;
	int min_stripe_size = SZ_1M;
	u64 calc_size = SZ_8M;
	u64 min_free;
	u64 max_chunk_size = 4 * calc_size;
	u64 avail = 0;
	u64 max_avail = 0;
	u64 percent_max;
	int num_stripes = 1;
	int max_stripes = 0;
	int min_stripes = 1;
	int sub_stripes = 1;
	int looped = 0;
	int ret;
	int index;
	int stripe_len = BTRFS_STRIPE_LEN;
	struct btrfs_key key;
	u64 offset;

	if (list_empty(dev_list)) {
		return -ENOSPC;
	}

	if (type & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
		if (type & BTRFS_BLOCK_GROUP_SYSTEM) {
			calc_size = SZ_8M;
			max_chunk_size = calc_size * 2;
			min_stripe_size = SZ_1M;
			max_stripes = BTRFS_MAX_DEVS_SYS_CHUNK;
		} else if (type & BTRFS_BLOCK_GROUP_DATA) {
			calc_size = SZ_1G;
			max_chunk_size = 10 * calc_size;
			min_stripe_size = SZ_64M;
			max_stripes = BTRFS_MAX_DEVS(info);
		} else if (type & BTRFS_BLOCK_GROUP_METADATA) {
			/* for larger filesystems, use larger metadata chunks */
			if (info->fs_devices->total_rw_bytes > 50ULL * SZ_1G)
				max_chunk_size = SZ_1G;
			else
				max_chunk_size = SZ_256M;
			calc_size = max_chunk_size;
			min_stripe_size = SZ_32M;
			max_stripes = BTRFS_MAX_DEVS(info);
		}
	}
	if (type & BTRFS_BLOCK_GROUP_RAID1) {
		num_stripes = min_t(u64, 2,
				  btrfs_super_num_devices(info->super_copy));
		if (num_stripes < 2)
			return -ENOSPC;
		min_stripes = 2;
	}
	if (type & BTRFS_BLOCK_GROUP_RAID1C3) {
		num_stripes = min_t(u64, 3,
				  btrfs_super_num_devices(info->super_copy));
		if (num_stripes < 3)
			return -ENOSPC;
		min_stripes = 3;
	}
	if (type & BTRFS_BLOCK_GROUP_RAID1C4) {
		num_stripes = min_t(u64, 4,
				  btrfs_super_num_devices(info->super_copy));
		if (num_stripes < 4)
			return -ENOSPC;
		min_stripes = 4;
	}
	if (type & BTRFS_BLOCK_GROUP_DUP) {
		num_stripes = 2;
		min_stripes = 2;
	}
	if (type & (BTRFS_BLOCK_GROUP_RAID0)) {
		num_stripes = btrfs_super_num_devices(info->super_copy);
		if (num_stripes > max_stripes)
			num_stripes = max_stripes;
		min_stripes = 2;
	}
	if (type & (BTRFS_BLOCK_GROUP_RAID10)) {
		num_stripes = btrfs_super_num_devices(info->super_copy);
		if (num_stripes > max_stripes)
			num_stripes = max_stripes;
		if (num_stripes < 4)
			return -ENOSPC;
		num_stripes &= ~(u32)1;
		sub_stripes = 2;
		min_stripes = 4;
	}
	if (type & (BTRFS_BLOCK_GROUP_RAID5)) {
		num_stripes = btrfs_super_num_devices(info->super_copy);
		if (num_stripes > max_stripes)
			num_stripes = max_stripes;
		if (num_stripes < 2)
			return -ENOSPC;
		min_stripes = 2;
		stripe_len = find_raid56_stripe_len(num_stripes - 1,
				    btrfs_super_stripesize(info->super_copy));
	}
	if (type & (BTRFS_BLOCK_GROUP_RAID6)) {
		num_stripes = btrfs_super_num_devices(info->super_copy);
		if (num_stripes > max_stripes)
			num_stripes = max_stripes;
		if (num_stripes < 3)
			return -ENOSPC;
		min_stripes = 3;
		stripe_len = find_raid56_stripe_len(num_stripes - 2,
				    btrfs_super_stripesize(info->super_copy));
	}

	/* we don't want a chunk larger than 10% of the FS */
	percent_max = div_factor(btrfs_super_total_bytes(info->super_copy), 1);
	max_chunk_size = min(percent_max, max_chunk_size);

again:
	if (chunk_bytes_by_type(type, calc_size, num_stripes, sub_stripes) >
	    max_chunk_size) {
		calc_size = max_chunk_size;
		calc_size /= num_stripes;
		calc_size /= stripe_len;
		calc_size *= stripe_len;
	}
	/* we don't want tiny stripes */
	calc_size = max_t(u64, calc_size, min_stripe_size);

	calc_size /= stripe_len;
	calc_size *= stripe_len;
	INIT_LIST_HEAD(&private_devs);
	cur = dev_list->next;
	index = 0;

	if (type & BTRFS_BLOCK_GROUP_DUP)
		min_free = calc_size * 2;
	else
		min_free = calc_size;

	/* build a private list of devices we will allocate from */
	while(index < num_stripes) {
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
	if (index < num_stripes) {
		list_splice(&private_devs, dev_list);
		if (index >= min_stripes) {
			num_stripes = index;
			if (type & (BTRFS_BLOCK_GROUP_RAID10)) {
				num_stripes /= sub_stripes;
				num_stripes *= sub_stripes;
			}
			looped = 1;
			goto again;
		}
		if (!looped && max_avail > 0) {
			looped = 1;
			calc_size = max_avail;
			goto again;
		}
		return -ENOSPC;
	}
	ret = find_next_chunk(info, &offset);
	if (ret)
		return ret;
	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = offset;

	chunk = kmalloc(btrfs_chunk_item_size(num_stripes), GFP_NOFS);
	if (!chunk)
		return -ENOMEM;

	map = kmalloc(btrfs_map_lookup_size(num_stripes), GFP_NOFS);
	if (!map) {
		kfree(chunk);
		return -ENOMEM;
	}

	stripes = &chunk->stripe;
	*num_bytes = chunk_bytes_by_type(type, calc_size,
					 num_stripes, sub_stripes);
	index = 0;
	while(index < num_stripes) {
		struct btrfs_stripe *stripe;
		BUG_ON(list_empty(&private_devs));
		cur = private_devs.next;
		device = list_entry(cur, struct btrfs_device, dev_list);

		/* loop over this device again if we're doing a dup group */
		if (!(type & BTRFS_BLOCK_GROUP_DUP) ||
		    (index == num_stripes - 1))
			list_move(&device->dev_list, dev_list);

		ret = btrfs_alloc_dev_extent(trans, device, key.offset,
			     calc_size, &dev_offset);
		if (ret < 0)
			goto out_chunk_map;

		device->bytes_used += calc_size;
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
	BUG_ON(!list_empty(&private_devs));

	/* key was set above */
	btrfs_set_stack_chunk_length(chunk, *num_bytes);
	btrfs_set_stack_chunk_owner(chunk, extent_root->root_key.objectid);
	btrfs_set_stack_chunk_stripe_len(chunk, stripe_len);
	btrfs_set_stack_chunk_type(chunk, type);
	btrfs_set_stack_chunk_num_stripes(chunk, num_stripes);
	btrfs_set_stack_chunk_io_align(chunk, stripe_len);
	btrfs_set_stack_chunk_io_width(chunk, stripe_len);
	btrfs_set_stack_chunk_sector_size(chunk, info->sectorsize);
	btrfs_set_stack_chunk_sub_stripes(chunk, sub_stripes);
	map->sector_size = info->sectorsize;
	map->stripe_len = stripe_len;
	map->io_align = stripe_len;
	map->io_width = stripe_len;
	map->type = type;
	map->num_stripes = num_stripes;
	map->sub_stripes = sub_stripes;

	ret = btrfs_insert_item(trans, chunk_root, &key, chunk,
				btrfs_chunk_item_size(num_stripes));
	BUG_ON(ret);
	*start = key.offset;;

	map->ce.start = key.offset;
	map->ce.size = *num_bytes;

	ret = insert_cache_extent(&info->mapping_tree.cache_tree, &map->ce);
	if (ret < 0)
		goto out_chunk_map;

	if (type & BTRFS_BLOCK_GROUP_SYSTEM) {
		ret = btrfs_add_system_chunk(info, &key,
				    chunk, btrfs_chunk_item_size(num_stripes));
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

/*
 * Alloc a DATA chunk with SINGLE profile.
 *
 * It allocates a chunk with 1:1 mapping (btrfs logical bytenr == on-disk bytenr)
 * Caller must make sure the chunk and dev_extent are not occupied.
 */
int btrfs_alloc_data_chunk(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *info, u64 *start, u64 num_bytes)
{
	u64 dev_offset;
	struct btrfs_root *extent_root = info->extent_root;
	struct btrfs_root *chunk_root = info->chunk_root;
	struct btrfs_stripe *stripes;
	struct btrfs_device *device = NULL;
	struct btrfs_chunk *chunk;
	struct list_head *dev_list = &info->fs_devices->devices;
	struct list_head *cur;
	struct map_lookup *map;
	u64 calc_size = SZ_8M;
	int num_stripes = 1;
	int sub_stripes = 1;
	int ret;
	int index;
	int stripe_len = BTRFS_STRIPE_LEN;
	struct btrfs_key key;


	if (*start != round_down(*start, info->sectorsize)) {
		error("DATA chunk start not sectorsize aligned: %llu",
				(unsigned long long)*start);
		return -EINVAL;
	}

	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = *start;
	dev_offset = *start;

	chunk = kmalloc(btrfs_chunk_item_size(num_stripes), GFP_NOFS);
	if (!chunk)
		return -ENOMEM;

	map = kmalloc(btrfs_map_lookup_size(num_stripes), GFP_NOFS);
	if (!map) {
		kfree(chunk);
		return -ENOMEM;
	}

	stripes = &chunk->stripe;
	calc_size = num_bytes;

	index = 0;
	cur = dev_list->next;
	device = list_entry(cur, struct btrfs_device, dev_list);

	while (index < num_stripes) {
		struct btrfs_stripe *stripe;

		ret = btrfs_insert_dev_extent(trans, device, key.offset, calc_size,
				dev_offset);
		BUG_ON(ret);

		device->bytes_used += calc_size;
		ret = btrfs_update_device(trans, device);
		BUG_ON(ret);

		map->stripes[index].dev = device;
		map->stripes[index].physical = dev_offset;
		stripe = stripes + index;
		btrfs_set_stack_stripe_devid(stripe, device->devid);
		btrfs_set_stack_stripe_offset(stripe, dev_offset);
		memcpy(stripe->dev_uuid, device->uuid, BTRFS_UUID_SIZE);
		index++;
	}

	/* key was set above */
	btrfs_set_stack_chunk_length(chunk, num_bytes);
	btrfs_set_stack_chunk_owner(chunk, extent_root->root_key.objectid);
	btrfs_set_stack_chunk_stripe_len(chunk, stripe_len);
	btrfs_set_stack_chunk_type(chunk, BTRFS_BLOCK_GROUP_DATA);
	btrfs_set_stack_chunk_num_stripes(chunk, num_stripes);
	btrfs_set_stack_chunk_io_align(chunk, stripe_len);
	btrfs_set_stack_chunk_io_width(chunk, stripe_len);
	btrfs_set_stack_chunk_sector_size(chunk, info->sectorsize);
	btrfs_set_stack_chunk_sub_stripes(chunk, sub_stripes);
	map->sector_size = info->sectorsize;
	map->stripe_len = stripe_len;
	map->io_align = stripe_len;
	map->io_width = stripe_len;
	map->type = BTRFS_BLOCK_GROUP_DATA;
	map->num_stripes = num_stripes;
	map->sub_stripes = sub_stripes;

	ret = btrfs_insert_item(trans, chunk_root, &key, chunk,
				btrfs_chunk_item_size(num_stripes));
	BUG_ON(ret);

	map->ce.start = key.offset;
	map->ce.size = num_bytes;

	ret = insert_cache_extent(&info->mapping_tree.cache_tree, &map->ce);
	BUG_ON(ret);

	kfree(chunk);
	return ret;
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

	if (map->type & (BTRFS_BLOCK_GROUP_DUP | BTRFS_BLOCK_GROUP_RAID1 |
			 BTRFS_BLOCK_GROUP_RAID1C3 | BTRFS_BLOCK_GROUP_RAID1C4))
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
	else if (map->type & (BTRFS_BLOCK_GROUP_RAID5 |
			      BTRFS_BLOCK_GROUP_RAID6)) {
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
		if (map->type & (BTRFS_BLOCK_GROUP_RAID1 |
				 BTRFS_BLOCK_GROUP_RAID1C3 |
				 BTRFS_BLOCK_GROUP_RAID1C4 |
				 BTRFS_BLOCK_GROUP_DUP)) {
			stripes_required = map->num_stripes;
		} else if (map->type & BTRFS_BLOCK_GROUP_RAID10) {
			stripes_required = map->sub_stripes;
		}
	}
	if (map->type & (BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)
	    && multi_ret && ((rw & WRITE) || mirror_num > 1) && raid_map_ret) {
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

	if (map->type & (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 |
			 BTRFS_BLOCK_GROUP_RAID1C3 | BTRFS_BLOCK_GROUP_RAID1C4 |
			 BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6 |
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
	stripe_index = 0;
	if (map->type & (BTRFS_BLOCK_GROUP_RAID1 |
			 BTRFS_BLOCK_GROUP_RAID1C3 |
			 BTRFS_BLOCK_GROUP_RAID1C4)) {
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
	} else if (map->type & (BTRFS_BLOCK_GROUP_RAID5 |
				BTRFS_BLOCK_GROUP_RAID6)) {

		if (raid_map) {
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
		multi->stripes[i].physical =
			map->stripes[stripe_index].physical + stripe_offset +
			stripe_nr * map->stripe_len;
		multi->stripes[i].dev = map->stripes[stripe_index].dev;
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

static struct btrfs_device *fill_missing_device(u64 devid)
{
	struct btrfs_device *device;

	device = kzalloc(sizeof(*device), GFP_NOFS);
	device->devid = devid;
	device->fd = -1;
	return device;
}

/*
 * slot == -1: SYSTEM chunk
 * return -EIO on error, otherwise return 0
 */
int btrfs_check_chunk_valid(struct btrfs_fs_info *fs_info,
			    struct extent_buffer *leaf,
			    struct btrfs_chunk *chunk,
			    int slot, u64 logical)
{
	u64 length;
	u64 stripe_len;
	u16 num_stripes;
	u16 sub_stripes;
	u64 type;
	u32 chunk_ondisk_size;
	u32 sectorsize = fs_info->sectorsize;

	/*
	 * Basic chunk item size check.  Note that btrfs_chunk already contains
	 * one stripe, so no "==" check.
	 */
	if (slot >= 0 &&
	    btrfs_item_size_nr(leaf, slot) < sizeof(struct btrfs_chunk)) {
		error("invalid chunk item size, have %u expect [%zu, %lu)",
			btrfs_item_size_nr(leaf, slot),
			sizeof(struct btrfs_chunk),
			BTRFS_LEAF_DATA_SIZE(fs_info));
		return -EUCLEAN;
	}
	length = btrfs_chunk_length(leaf, chunk);
	stripe_len = btrfs_chunk_stripe_len(leaf, chunk);
	num_stripes = btrfs_chunk_num_stripes(leaf, chunk);
	sub_stripes = btrfs_chunk_sub_stripes(leaf, chunk);
	type = btrfs_chunk_type(leaf, chunk);

	if (num_stripes == 0) {
		error("invalid num_stripes, have %u expect non-zero",
			num_stripes);
		return -EUCLEAN;
	}
	if (slot >= 0 && btrfs_chunk_item_size(num_stripes) !=
	    btrfs_item_size_nr(leaf, slot)) {
		error("invalid chunk item size, have %u expect %lu",
			btrfs_item_size_nr(leaf, slot),
			btrfs_chunk_item_size(num_stripes));
		return -EUCLEAN;
	}

	/*
	 * These valid checks may be insufficient to cover every corner cases.
	 */
	if (!IS_ALIGNED(logical, sectorsize)) {
		error("invalid chunk logical %llu",  logical);
		return -EIO;
	}
	if (btrfs_chunk_sector_size(leaf, chunk) != sectorsize) {
		error("invalid chunk sectorsize %llu",
		      (unsigned long long)btrfs_chunk_sector_size(leaf, chunk));
		return -EIO;
	}
	if (!length || !IS_ALIGNED(length, sectorsize)) {
		error("invalid chunk length %llu",  length);
		return -EIO;
	}
	if (stripe_len != BTRFS_STRIPE_LEN) {
		error("invalid chunk stripe length: %llu", stripe_len);
		return -EIO;
	}
	/* Check on chunk item type */
	if (slot == -1 && (type & BTRFS_BLOCK_GROUP_SYSTEM) == 0) {
		error("invalid chunk type %llu", type);
		return -EIO;
	}
	if (type & ~(BTRFS_BLOCK_GROUP_TYPE_MASK |
		     BTRFS_BLOCK_GROUP_PROFILE_MASK)) {
		error("unrecognized chunk type: %llu",
		      ~(BTRFS_BLOCK_GROUP_TYPE_MASK |
			BTRFS_BLOCK_GROUP_PROFILE_MASK) & type);
		return -EIO;
	}
	if (!(type & BTRFS_BLOCK_GROUP_TYPE_MASK)) {
		error("missing chunk type flag: %llu", type);
		return -EIO;
	}
	if (!(is_power_of_2(type & BTRFS_BLOCK_GROUP_PROFILE_MASK) ||
	      (type & BTRFS_BLOCK_GROUP_PROFILE_MASK) == 0)) {
		error("conflicting chunk type detected: %llu", type);
		return -EIO;
	}
	if ((type & BTRFS_BLOCK_GROUP_PROFILE_MASK) &&
	    !is_power_of_2(type & BTRFS_BLOCK_GROUP_PROFILE_MASK)) {
		error("conflicting chunk profile detected: %llu", type);
		return -EIO;
	}

	chunk_ondisk_size = btrfs_chunk_item_size(num_stripes);
	/*
	 * Btrfs_chunk contains at least one stripe, and for sys_chunk
	 * it can't exceed the system chunk array size
	 * For normal chunk, it should match its chunk item size.
	 */
	if (num_stripes < 1 ||
	    (slot == -1 && chunk_ondisk_size > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE) ||
	    (slot >= 0 && chunk_ondisk_size > btrfs_item_size_nr(leaf, slot))) {
		error("invalid num_stripes: %u", num_stripes);
		return -EIO;
	}
	/*
	 * Device number check against profile
	 */
	if ((type & BTRFS_BLOCK_GROUP_RAID10 && (sub_stripes != 2 ||
		  !IS_ALIGNED(num_stripes, sub_stripes))) ||
	    (type & BTRFS_BLOCK_GROUP_RAID1 && num_stripes < 1) ||
	    (type & BTRFS_BLOCK_GROUP_RAID1C3 && num_stripes < 3) ||
	    (type & BTRFS_BLOCK_GROUP_RAID1C4 && num_stripes < 4) ||
	    (type & BTRFS_BLOCK_GROUP_RAID5 && num_stripes < 2) ||
	    (type & BTRFS_BLOCK_GROUP_RAID6 && num_stripes < 3) ||
	    (type & BTRFS_BLOCK_GROUP_DUP && num_stripes > 2) ||
	    ((type & BTRFS_BLOCK_GROUP_PROFILE_MASK) == 0 &&
	     num_stripes != 1)) {
		error("Invalid num_stripes:sub_stripes %u:%u for profile %llu",
		      num_stripes, sub_stripes,
		      type & BTRFS_BLOCK_GROUP_PROFILE_MASK);
		return -EIO;
	}

	return 0;
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
	ret = btrfs_check_chunk_valid(fs_info, leaf, chunk, slot, logical);
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
			map->stripes[i].dev = fill_missing_device(devid);
			printf("warning, device %llu is missing\n",
			       (unsigned long long)devid);
			list_add(&map->stripes[i].dev->dev_list,
				 &fs_info->fs_devices->devices);
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
		list_add(&fs_devices->list, &fs_uuids);
		memcpy(fs_devices->fsid, fsid, BTRFS_FSID_SIZE);
	}

	ret = btrfs_open_devices(fs_devices, O_RDONLY);
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
	key.offset = 0;
	key.type = 0;
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
		eb->fd = -1;
		eb->dev_bytenr = (u64)-1;

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
	free(tmp_ebs);
	return ret;
clean_up:
	for (i = 0; i < num_stripes; i++)
		free(tmp_ebs[i]);
	free(tmp_ebs);
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

	ebs = malloc(sizeof(*ebs) * multi->num_stripes);
	pointers = malloc(sizeof(*pointers) * multi->num_stripes);
	if (!ebs || !pointers) {
		free(ebs);
		free(pointers);
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
			ebs[i]->dev_bytenr = multi->stripes[i].physical;
			ebs[i]->fd = multi->stripes[i].dev->fd;
			multi->stripes[i].dev->total_ios++;
			if (ebs[i]->start != raid_map[i]) {
				ret = -EINVAL;
				goto out_free_split;
			}
			continue;
		}
		new_eb = malloc(sizeof(*eb) + alloc_size);
		if (!new_eb) {
			ret = -ENOMEM;
			goto out_free_split;
		}
		new_eb->dev_bytenr = multi->stripes[i].physical;
		new_eb->fd = multi->stripes[i].dev->fd;
		multi->stripes[i].dev->total_ios++;
		new_eb->len = stripe_len;

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
		ret = write_extent_to_disk(ebs[i]);
		if (ret < 0)
			goto out_free_split;
	}

out_free_split:
	for (i = 0; i < multi->num_stripes; i++) {
		if (ebs[i] != eb)
			free(ebs[i]);
	}
out:
	free(ebs);
	free(pointers);

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

	switch (profile) {
	case 0: /* Single profile */
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID1C3:
	case BTRFS_BLOCK_GROUP_RAID1C4:
	case BTRFS_BLOCK_GROUP_DUP:
		stripe_len = chunk_len;
		break;
	case BTRFS_BLOCK_GROUP_RAID0:
		stripe_len = chunk_len / num_stripes;
		break;
	case BTRFS_BLOCK_GROUP_RAID5:
		stripe_len = chunk_len / (num_stripes - 1);
		break;
	case BTRFS_BLOCK_GROUP_RAID6:
		stripe_len = chunk_len / (num_stripes - 2);
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
 * Return 0 if size of @device is already good
 * Return >0 if size of @device is not aligned but fixed without problems
 * Return <0 if something wrong happened when aligning the size of @device
 */
int btrfs_fix_device_size(struct btrfs_fs_info *fs_info,
			  struct btrfs_device *device)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	struct btrfs_path path;
	struct btrfs_root *chunk_root = fs_info->chunk_root;
	struct btrfs_dev_item *di;
	u64 old_bytes = device->total_bytes;
	int ret;

	if (IS_ALIGNED(old_bytes, fs_info->sectorsize))
		return 0;

	/* Align the in-memory total_bytes first, and use it as correct size */
	device->total_bytes = round_down(device->total_bytes,
					 fs_info->sectorsize);

	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = device->devid;

	trans = btrfs_start_transaction(chunk_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error("error starting transaction: %d (%m)", ret);
		return ret;
	}

	btrfs_init_path(&path);
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
		error("failed to commit current transaction: %d (%m)", ret);
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

/*
 * Return 0 if super block total_bytes matches all devices' total_bytes
 * Return >0 if super block total_bytes mismatch but fixed without problem
 * Return <0 if we failed to fix super block total_bytes
 */
int btrfs_fix_super_size(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
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

	/* Commit transaction to update all super blocks */
	trans = btrfs_start_transaction(fs_info->tree_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error("error starting transaction: %d (%m)", ret);
		return ret;
	}
	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret < 0) {
		errno = -ret;
		error("failed to commit current transaction: %d (%m)", ret);
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
