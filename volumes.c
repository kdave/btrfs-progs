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
#define _XOPEN_SOURCE 600
#define __USE_XOPEN2K
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

struct stripe {
	struct btrfs_device *dev;
	u64 physical;
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

#define map_lookup_size(n) (sizeof(struct map_lookup) + \
			    (sizeof(struct btrfs_bio_stripe) * (n)))

static LIST_HEAD(fs_uuids);

static struct btrfs_device *__find_device(struct list_head *head, u64 devid,
					  u8 *uuid)
{
	struct btrfs_device *dev;
	struct list_head *cur;

	list_for_each(cur, head) {
		dev = list_entry(cur, struct btrfs_device, dev_list);
		if (dev->devid == devid &&
		    !memcmp(dev->uuid, uuid, BTRFS_UUID_SIZE)) {
			return dev;
		}
	}
	return NULL;
}

static struct btrfs_fs_devices *find_fsid(u8 *fsid)
{
	struct list_head *cur;
	struct btrfs_fs_devices *fs_devices;

	list_for_each(cur, &fs_uuids) {
		fs_devices = list_entry(cur, struct btrfs_fs_devices, list);
		if (memcmp(fsid, fs_devices->fsid, BTRFS_FSID_SIZE) == 0)
			return fs_devices;
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

	fs_devices = find_fsid(disk_super->fsid);
	if (!fs_devices) {
		fs_devices = kzalloc(sizeof(*fs_devices), GFP_NOFS);
		if (!fs_devices)
			return -ENOMEM;
		INIT_LIST_HEAD(&fs_devices->devices);
		list_add(&fs_devices->list, &fs_uuids);
		memcpy(fs_devices->fsid, disk_super->fsid, BTRFS_FSID_SIZE);
		fs_devices->latest_devid = devid;
		fs_devices->latest_trans = found_transid;
		fs_devices->lowest_devid = (u64)-1;
		device = NULL;
	} else {
		device = __find_device(&fs_devices->devices, devid,
				       disk_super->dev_item.uuid);
	}
	if (!device) {
		device = kzalloc(sizeof(*device), GFP_NOFS);
		if (!device) {
			/* we can safely leave the fs_devices entry around */
			return -ENOMEM;
		}
		device->devid = devid;
		memcpy(device->uuid, disk_super->dev_item.uuid,
		       BTRFS_UUID_SIZE);
		device->name = kstrdup(path, GFP_NOFS);
		if (!device->name) {
			kfree(device);
			return -ENOMEM;
		}
		device->label = kstrdup(disk_super->label, GFP_NOFS);
		device->total_devs = btrfs_super_num_devices(disk_super);
		device->super_bytes_used = btrfs_super_bytes_used(disk_super);
		device->total_bytes =
			btrfs_stack_device_total_bytes(&disk_super->dev_item);
		device->bytes_used =
			btrfs_stack_device_bytes_used(&disk_super->dev_item);
		list_add(&device->dev_list, &fs_devices->devices);
		device->fs_devices = fs_devices;
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
	struct list_head *cur;
	struct btrfs_device *device;
again:
	list_for_each(cur, &fs_devices->devices) {
		device = list_entry(cur, struct btrfs_device, dev_list);
		close(device->fd);
		device->fd = -1;
		device->writeable = 0;
	}

	seed_devices = fs_devices->seed;
	fs_devices->seed = NULL;
	if (seed_devices) {
		fs_devices = seed_devices;
		goto again;
	}

	return 0;
}

int btrfs_open_devices(struct btrfs_fs_devices *fs_devices, int flags)
{
	int fd;
	struct list_head *head = &fs_devices->devices;
	struct list_head *cur;
	struct btrfs_device *device;
	int ret;

	list_for_each(cur, head) {
		device = list_entry(cur, struct btrfs_device, dev_list);

		fd = open(device->name, flags);
		if (fd < 0) {
			ret = -errno;
			goto fail;
		}

		if (device->devid == fs_devices->latest_devid)
			fs_devices->latest_bdev = fd;
		if (device->devid == fs_devices->lowest_devid)
			fs_devices->lowest_bdev = fd;
		device->fd = fd;
		if (flags == O_RDWR)
			device->writeable = 1;
	}
	return 0;
fail:
	btrfs_close_devices(fs_devices);
	return ret;
}

int btrfs_scan_one_device(int fd, const char *path,
			  struct btrfs_fs_devices **fs_devices_ret,
			  u64 *total_devs, u64 super_offset)
{
	struct btrfs_super_block *disk_super;
	char *buf;
	int ret;
	u64 devid;
	char uuidbuf[37];

	buf = malloc(4096);
	if (!buf) {
		ret = -ENOMEM;
		goto error;
	}
	disk_super = (struct btrfs_super_block *)buf;
	ret = btrfs_read_dev_super(fd, disk_super, super_offset);
	if (ret < 0) {
		ret = -EIO;
		goto error_brelse;
	}
	devid = le64_to_cpu(disk_super->dev_item.devid);
	if (btrfs_super_flags(disk_super) & BTRFS_SUPER_FLAG_METADUMP)
		*total_devs = 1;
	else
		*total_devs = btrfs_super_num_devices(disk_super);
	uuid_unparse(disk_super->fsid, uuidbuf);

	ret = device_list_add(path, disk_super, devid, fs_devices_ret);

error_brelse:
	free(buf);
error:
	return ret;
}

/*
 * this uses a pretty simple search, the expectation is that it is
 * called very infrequently and that a given device has a small number
 * of extents
 */
static int find_free_dev_extent(struct btrfs_trans_handle *trans,
				struct btrfs_device *device,
				struct btrfs_path *path,
				u64 num_bytes, u64 *start)
{
	struct btrfs_key key;
	struct btrfs_root *root = device->dev_root;
	struct btrfs_dev_extent *dev_extent = NULL;
	u64 hole_size = 0;
	u64 last_byte = 0;
	u64 search_start = 0;
	u64 search_end = device->total_bytes;
	int ret;
	int slot = 0;
	int start_found;
	struct extent_buffer *l;

	start_found = 0;
	path->reada = 2;

	/* FIXME use last free of some kind */

	/* we don't want to overwrite the superblock on the drive,
	 * so we make sure to start at an offset of at least 1MB
	 */
	search_start = max((u64)1024 * 1024, search_start);

	if (root->fs_info->alloc_start + num_bytes <= device->total_bytes)
		search_start = max(root->fs_info->alloc_start, search_start);

	key.objectid = device->devid;
	key.offset = search_start;
	key.type = BTRFS_DEV_EXTENT_KEY;
	ret = btrfs_search_slot(trans, root, &key, path, 0, 0);
	if (ret < 0)
		goto error;
	ret = btrfs_previous_item(root, path, 0, key.type);
	if (ret < 0)
		goto error;
	l = path->nodes[0];
	btrfs_item_key_to_cpu(l, &key, path->slots[0]);
	while (1) {
		l = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(l)) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto error;
no_more_items:
			if (!start_found) {
				if (search_start >= search_end) {
					ret = -ENOSPC;
					goto error;
				}
				*start = search_start;
				start_found = 1;
				goto check_pending;
			}
			*start = last_byte > search_start ?
				last_byte : search_start;
			if (search_end <= *start) {
				ret = -ENOSPC;
				goto error;
			}
			goto check_pending;
		}
		btrfs_item_key_to_cpu(l, &key, slot);

		if (key.objectid < device->devid)
			goto next;

		if (key.objectid > device->devid)
			goto no_more_items;

		if (key.offset >= search_start && key.offset > last_byte &&
		    start_found) {
			if (last_byte < search_start)
				last_byte = search_start;
			hole_size = key.offset - last_byte;
			if (key.offset > last_byte &&
			    hole_size >= num_bytes) {
				*start = last_byte;
				goto check_pending;
			}
		}
		if (btrfs_key_type(&key) != BTRFS_DEV_EXTENT_KEY) {
			goto next;
		}

		start_found = 1;
		dev_extent = btrfs_item_ptr(l, slot, struct btrfs_dev_extent);
		last_byte = key.offset + btrfs_dev_extent_length(l, dev_extent);
next:
		path->slots[0]++;
		cond_resched();
	}
check_pending:
	/* we have to make sure we didn't find an extent that has already
	 * been allocated by the map tree or the original allocation
	 */
	btrfs_release_path(root, path);
	BUG_ON(*start < search_start);

	if (*start + num_bytes > search_end) {
		ret = -ENOSPC;
		goto error;
	}
	/* check for pending inserts here */
	return 0;

error:
	btrfs_release_path(root, path);
	return ret;
}

int btrfs_alloc_dev_extent(struct btrfs_trans_handle *trans,
			   struct btrfs_device *device,
			   u64 chunk_tree, u64 chunk_objectid,
			   u64 chunk_offset,
			   u64 num_bytes, u64 *start)
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

	ret = find_free_dev_extent(trans, device, path, num_bytes, start);
	if (ret) {
		goto err;
	}

	key.objectid = device->devid;
	key.offset = *start;
	key.type = BTRFS_DEV_EXTENT_KEY;
	ret = btrfs_insert_empty_item(trans, root, path, &key,
				      sizeof(*extent));
	BUG_ON(ret);

	leaf = path->nodes[0];
	extent = btrfs_item_ptr(leaf, path->slots[0],
				struct btrfs_dev_extent);
	btrfs_set_dev_extent_chunk_tree(leaf, extent, chunk_tree);
	btrfs_set_dev_extent_chunk_objectid(leaf, extent, chunk_objectid);
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

static int find_next_chunk(struct btrfs_root *root, u64 objectid, u64 *offset)
{
	struct btrfs_path *path;
	int ret;
	struct btrfs_key key;
	struct btrfs_chunk *chunk;
	struct btrfs_key found_key;

	path = btrfs_alloc_path();
	BUG_ON(!path);

	key.objectid = objectid;
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
		if (found_key.objectid != objectid)
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
	btrfs_release_path(root, path);
	return ret;
}

/*
 * the device information is stored in the chunk root
 * the btrfs_device struct should be fully filled in
 */
int btrfs_add_device(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root,
		     struct btrfs_device *device)
{
	int ret;
	struct btrfs_path *path;
	struct btrfs_dev_item *dev_item;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	unsigned long ptr;
	u64 free_devid = 0;

	root = root->fs_info->chunk_root;

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
	write_extent_buffer(leaf, root->fs_info->fsid, ptr, BTRFS_UUID_SIZE);
	btrfs_mark_buffer_dirty(leaf);
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

int btrfs_add_system_chunk(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct btrfs_key *key,
			   struct btrfs_chunk *chunk, int item_size)
{
	struct btrfs_super_block *super_copy = &root->fs_info->super_copy;
	struct btrfs_disk_key disk_key;
	u32 array_size;
	u8 *ptr;

	array_size = btrfs_super_sys_array_size(super_copy);
	if (array_size + item_size > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE)
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

static u64 div_factor(u64 num, int factor)
{
	if (factor == 10)
		return num;
	num *= factor;
	return num / 10;
}

static u64 chunk_bytes_by_type(u64 type, u64 calc_size, int num_stripes,
			       int sub_stripes)
{
	if (type & (BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_DUP))
		return calc_size;
	else if (type & BTRFS_BLOCK_GROUP_RAID10)
		return calc_size * (num_stripes / sub_stripes);
	else
		return calc_size * num_stripes;
}


int btrfs_alloc_chunk(struct btrfs_trans_handle *trans,
		      struct btrfs_root *extent_root, u64 *start,
		      u64 *num_bytes, u64 type)
{
	u64 dev_offset;
	struct btrfs_fs_info *info = extent_root->fs_info;
	struct btrfs_root *chunk_root = extent_root->fs_info->chunk_root;
	struct btrfs_stripe *stripes;
	struct btrfs_device *device = NULL;
	struct btrfs_chunk *chunk;
	struct list_head private_devs;
	struct list_head *dev_list = &extent_root->fs_info->fs_devices->devices;
	struct list_head *cur;
	struct map_lookup *map;
	int min_stripe_size = 1 * 1024 * 1024;
	u64 physical;
	u64 calc_size = 8 * 1024 * 1024;
	u64 min_free;
	u64 max_chunk_size = 4 * calc_size;
	u64 avail;
	u64 max_avail = 0;
	u64 percent_max;
	int num_stripes = 1;
	int min_stripes = 1;
	int sub_stripes = 0;
	int looped = 0;
	int ret;
	int index;
	int stripe_len = 64 * 1024;
	struct btrfs_key key;

	if (list_empty(dev_list)) {
		return -ENOSPC;
	}

	if (type & (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 |
		    BTRFS_BLOCK_GROUP_RAID10 |
		    BTRFS_BLOCK_GROUP_DUP)) {
		if (type & BTRFS_BLOCK_GROUP_SYSTEM) {
			calc_size = 8 * 1024 * 1024;
			max_chunk_size = calc_size * 2;
			min_stripe_size = 1 * 1024 * 1024;
		} else if (type & BTRFS_BLOCK_GROUP_DATA) {
			calc_size = 1024 * 1024 * 1024;
			max_chunk_size = 10 * calc_size;
			min_stripe_size = 64 * 1024 * 1024;
		} else if (type & BTRFS_BLOCK_GROUP_METADATA) {
			calc_size = 1024 * 1024 * 1024;
			max_chunk_size = 4 * calc_size;
			min_stripe_size = 32 * 1024 * 1024;
		}
	}
	if (type & BTRFS_BLOCK_GROUP_RAID1) {
		num_stripes = min_t(u64, 2,
				  btrfs_super_num_devices(&info->super_copy));
		if (num_stripes < 2)
			return -ENOSPC;
		min_stripes = 2;
	}
	if (type & BTRFS_BLOCK_GROUP_DUP) {
		num_stripes = 2;
		min_stripes = 2;
	}
	if (type & (BTRFS_BLOCK_GROUP_RAID0)) {
		num_stripes = btrfs_super_num_devices(&info->super_copy);
		min_stripes = 2;
	}
	if (type & (BTRFS_BLOCK_GROUP_RAID10)) {
		num_stripes = btrfs_super_num_devices(&info->super_copy);
		if (num_stripes < 4)
			return -ENOSPC;
		num_stripes &= ~(u32)1;
		sub_stripes = 2;
		min_stripes = 4;
	}

	/* we don't want a chunk larger than 10% of the FS */
	percent_max = div_factor(btrfs_super_total_bytes(&info->super_copy), 1);
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
		avail = device->total_bytes - device->bytes_used;
		cur = cur->next;
		if (avail >= min_free) {
			list_move_tail(&device->dev_list, &private_devs);
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
	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	ret = find_next_chunk(chunk_root, BTRFS_FIRST_CHUNK_TREE_OBJECTID,
			      &key.offset);
	if (ret)
		return ret;

	chunk = kmalloc(btrfs_chunk_item_size(num_stripes), GFP_NOFS);
	if (!chunk)
		return -ENOMEM;

	map = kmalloc(map_lookup_size(num_stripes), GFP_NOFS);
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
			list_move_tail(&device->dev_list, dev_list);

		ret = btrfs_alloc_dev_extent(trans, device,
			     info->chunk_root->root_key.objectid,
			     BTRFS_FIRST_CHUNK_TREE_OBJECTID, key.offset,
			     calc_size, &dev_offset);
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
		physical = dev_offset;
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
	btrfs_set_stack_chunk_sector_size(chunk, extent_root->sectorsize);
	btrfs_set_stack_chunk_sub_stripes(chunk, sub_stripes);
	map->sector_size = extent_root->sectorsize;
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

	ret = insert_existing_cache_extent(
			   &extent_root->fs_info->mapping_tree.cache_tree,
			   &map->ce);
	BUG_ON(ret);

	if (type & BTRFS_BLOCK_GROUP_SYSTEM) {
		ret = btrfs_add_system_chunk(trans, chunk_root, &key,
				    chunk, btrfs_chunk_item_size(num_stripes));
		BUG_ON(ret);
	}

	kfree(chunk);
	return ret;
}

void btrfs_mapping_init(struct btrfs_mapping_tree *tree)
{
	cache_tree_init(&tree->cache_tree);
}

int btrfs_num_copies(struct btrfs_mapping_tree *map_tree, u64 logical, u64 len)
{
	struct cache_extent *ce;
	struct map_lookup *map;
	int ret;
	u64 offset;

	ce = find_first_cache_extent(&map_tree->cache_tree, logical);
	BUG_ON(!ce);
	BUG_ON(ce->start > logical || ce->start + ce->size < logical);
	map = container_of(ce, struct map_lookup, ce);

	offset = logical - ce->start;
	if (map->type & (BTRFS_BLOCK_GROUP_DUP | BTRFS_BLOCK_GROUP_RAID1))
		ret = map->num_stripes;
	else if (map->type & BTRFS_BLOCK_GROUP_RAID10)
		ret = map->sub_stripes;
	else
		ret = 1;
	return ret;
}

int btrfs_rmap_block(struct btrfs_mapping_tree *map_tree,
		     u64 chunk_start, u64 physical, u64 devid,
		     u64 **logical, int *naddrs, int *stripe_len)
{
	struct cache_extent *ce;
	struct map_lookup *map;
	u64 *buf;
	u64 bytenr;
	u64 length;
	u64 stripe_nr;
	int i, j, nr = 0;

	ce = find_first_cache_extent(&map_tree->cache_tree, chunk_start);
	BUG_ON(!ce);
	map = container_of(ce, struct map_lookup, ce);

	length = ce->size;
	if (map->type & BTRFS_BLOCK_GROUP_RAID10)
		length = ce->size / (map->num_stripes / map->sub_stripes);
	else if (map->type & BTRFS_BLOCK_GROUP_RAID0)
		length = ce->size / map->num_stripes;

	buf = kzalloc(sizeof(u64) * map->num_stripes, GFP_NOFS);

	for (i = 0; i < map->num_stripes; i++) {
		if (devid && map->stripes[i].dev->devid != devid)
			continue;
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
		}
		bytenr = ce->start + stripe_nr * map->stripe_len;
		for (j = 0; j < nr; j++) {
			if (buf[j] == bytenr)
				break;
		}
		if (j == nr)
			buf[nr++] = bytenr;
	}

	*logical = buf;
	*naddrs = nr;
	*stripe_len = map->stripe_len;

	return 0;
}

int btrfs_map_block(struct btrfs_mapping_tree *map_tree, int rw,
		    u64 logical, u64 *length,
		    struct btrfs_multi_bio **multi_ret, int mirror_num)
{
	struct cache_extent *ce;
	struct map_lookup *map;
	u64 offset;
	u64 stripe_offset;
	u64 stripe_nr;
	int stripes_allocated = 8;
	int stripes_required = 1;
	int stripe_index;
	int i;
	struct btrfs_multi_bio *multi = NULL;

	if (multi_ret && rw == READ) {
		stripes_allocated = 1;
	}
again:
	if (multi_ret) {
		multi = kzalloc(btrfs_multi_bio_size(stripes_allocated),
				GFP_NOFS);
		if (!multi)
			return -ENOMEM;
	}

	ce = find_first_cache_extent(&map_tree->cache_tree, logical);
	BUG_ON(!ce);
	BUG_ON(ce->start > logical || ce->start + ce->size < logical);
	map = container_of(ce, struct map_lookup, ce);
	offset = logical - ce->start;

	if (rw == WRITE) {
		if (map->type & (BTRFS_BLOCK_GROUP_RAID1 |
				 BTRFS_BLOCK_GROUP_DUP)) {
			stripes_required = map->num_stripes;
		} else if (map->type & BTRFS_BLOCK_GROUP_RAID10) {
			stripes_required = map->sub_stripes;
		}
	}
	/* if our multi bio struct is too small, back off and try again */
	if (multi_ret && rw == WRITE &&
	    stripes_allocated < stripes_required) {
		stripes_allocated = map->num_stripes;
		kfree(multi);
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
	if (map->type & BTRFS_BLOCK_GROUP_RAID1) {
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
		else
			stripe_index = stripe_nr % map->sub_stripes;

		stripe_nr = stripe_nr / factor;
	} else if (map->type & BTRFS_BLOCK_GROUP_DUP) {
		if (rw == WRITE)
			multi->num_stripes = map->num_stripes;
		else if (mirror_num)
			stripe_index = mirror_num - 1;
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
out:
	return 0;
}

struct btrfs_device *btrfs_find_device(struct btrfs_root *root, u64 devid,
				       u8 *uuid, u8 *fsid)
{
	struct btrfs_device *device;
	struct btrfs_fs_devices *cur_devices;

	cur_devices = root->fs_info->fs_devices;
	while (cur_devices) {
		if (!fsid ||
		    !memcmp(cur_devices->fsid, fsid, BTRFS_UUID_SIZE)) {
			device = __find_device(&cur_devices->devices,
					       devid, uuid);
			if (device)
				return device;
		}
		cur_devices = cur_devices->seed;
	}
	return NULL;
}

int btrfs_bootstrap_super_map(struct btrfs_mapping_tree *map_tree,
			      struct btrfs_fs_devices *fs_devices)
{
	struct map_lookup *map;
	u64 logical = BTRFS_SUPER_INFO_OFFSET;
	u64 length = BTRFS_SUPER_INFO_SIZE;
	int num_stripes = 0;
	int sub_stripes = 0;
	int ret;
	int i;
	struct list_head *cur;

	list_for_each(cur, &fs_devices->devices) {
		num_stripes++;
	}
	map = kmalloc(map_lookup_size(num_stripes), GFP_NOFS);
	if (!map)
		return -ENOMEM;

	map->ce.start = logical;
	map->ce.size = length;
	map->num_stripes = num_stripes;
	map->sub_stripes = sub_stripes;
	map->io_width = length;
	map->io_align = length;
	map->sector_size = length;
	map->stripe_len = length;
	map->type = BTRFS_BLOCK_GROUP_RAID1;

	i = 0;
	list_for_each(cur, &fs_devices->devices) {
		struct btrfs_device *device = list_entry(cur,
							 struct btrfs_device,
							 dev_list);
		map->stripes[i].physical = logical;
		map->stripes[i].dev = device;
		i++;
	}
	ret = insert_existing_cache_extent(&map_tree->cache_tree, &map->ce);
	if (ret == -EEXIST) {
		struct cache_extent *old;
		struct map_lookup *old_map;
		old = find_cache_extent(&map_tree->cache_tree, logical, length);
		old_map = container_of(old, struct map_lookup, ce);
		remove_cache_extent(&map_tree->cache_tree, old);
		kfree(old_map);
		ret = insert_existing_cache_extent(&map_tree->cache_tree,
						   &map->ce);
	}
	BUG_ON(ret);
	return 0;
}

int btrfs_chunk_readonly(struct btrfs_root *root, u64 chunk_offset)
{
	struct cache_extent *ce;
	struct map_lookup *map;
	struct btrfs_mapping_tree *map_tree = &root->fs_info->mapping_tree;
	int readonly = 0;
	int i;

	ce = find_first_cache_extent(&map_tree->cache_tree, chunk_offset);
	BUG_ON(!ce);

	map = container_of(ce, struct map_lookup, ce);
	for (i = 0; i < map->num_stripes; i++) {
		if (!map->stripes[i].dev->writeable) {
			readonly = 1;
			break;
		}
	}

	return readonly;
}

static int read_one_chunk(struct btrfs_root *root, struct btrfs_key *key,
			  struct extent_buffer *leaf,
			  struct btrfs_chunk *chunk)
{
	struct btrfs_mapping_tree *map_tree = &root->fs_info->mapping_tree;
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

	ce = find_first_cache_extent(&map_tree->cache_tree, logical);

	/* already mapped? */
	if (ce && ce->start <= logical && ce->start + ce->size > logical) {
		return 0;
	}

	num_stripes = btrfs_chunk_num_stripes(leaf, chunk);
	map = kmalloc(map_lookup_size(num_stripes), GFP_NOFS);
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
		map->stripes[i].dev = btrfs_find_device(root, devid, uuid,
							NULL);
		if (!map->stripes[i].dev) {
			kfree(map);
			return -EIO;
		}

	}
	ret = insert_existing_cache_extent(&map_tree->cache_tree, &map->ce);
	BUG_ON(ret);

	return 0;
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

static int open_seed_devices(struct btrfs_root *root, u8 *fsid)
{
	struct btrfs_fs_devices *fs_devices;
	int ret;

	fs_devices = root->fs_info->fs_devices->seed;
	while (fs_devices) {
		if (!memcmp(fs_devices->fsid, fsid, BTRFS_UUID_SIZE)) {
			ret = 0;
			goto out;
		}
		fs_devices = fs_devices->seed;
	}

	fs_devices = find_fsid(fsid);
	if (!fs_devices) {
		ret = -ENOENT;
		goto out;
	}

	ret = btrfs_open_devices(fs_devices, O_RDONLY);
	if (ret)
		goto out;

	fs_devices->seed = root->fs_info->fs_devices->seed;
	root->fs_info->fs_devices->seed = fs_devices;
out:
	return ret;
}

static int read_one_dev(struct btrfs_root *root,
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
			   BTRFS_UUID_SIZE);

	if (memcmp(fs_uuid, root->fs_info->fsid, BTRFS_UUID_SIZE)) {
		ret = open_seed_devices(root, fs_uuid);
		if (ret)
			return ret;
	}

	device = btrfs_find_device(root, devid, dev_uuid, fs_uuid);
	if (!device) {
		printk("warning devid %llu not found already\n",
			(unsigned long long)devid);
		device = kmalloc(sizeof(*device), GFP_NOFS);
		if (!device)
			return -ENOMEM;
		device->total_ios = 0;
		list_add(&device->dev_list,
			 &root->fs_info->fs_devices->devices);
	}

	fill_device_from_item(leaf, dev_item, device);
	device->dev_root = root->fs_info->dev_root;
	return ret;
}

int btrfs_read_super_device(struct btrfs_root *root, struct extent_buffer *buf)
{
	struct btrfs_dev_item *dev_item;

	dev_item = (struct btrfs_dev_item *)offsetof(struct btrfs_super_block,
						     dev_item);
	return read_one_dev(root, buf, dev_item);
}

int btrfs_read_sys_array(struct btrfs_root *root)
{
	struct btrfs_super_block *super_copy = &root->fs_info->super_copy;
	struct extent_buffer *sb;
	struct btrfs_disk_key *disk_key;
	struct btrfs_chunk *chunk;
	struct btrfs_key key;
	u32 num_stripes;
	u32 array_size;
	u32 len = 0;
	u8 *ptr;
	unsigned long sb_ptr;
	u32 cur;
	int ret;

	sb = btrfs_find_create_tree_block(root, BTRFS_SUPER_INFO_OFFSET,
					  BTRFS_SUPER_INFO_SIZE);
	if (!sb)
		return -ENOMEM;
	btrfs_set_buffer_uptodate(sb);
	write_extent_buffer(sb, super_copy, 0, BTRFS_SUPER_INFO_SIZE);
	array_size = btrfs_super_sys_array_size(super_copy);

	/*
	 * we do this loop twice, once for the device items and
	 * once for all of the chunks.  This way there are device
	 * structs filled in for every chunk
	 */
	ptr = super_copy->sys_chunk_array;
	sb_ptr = offsetof(struct btrfs_super_block, sys_chunk_array);
	cur = 0;

	while (cur < array_size) {
		disk_key = (struct btrfs_disk_key *)ptr;
		btrfs_disk_key_to_cpu(&key, disk_key);

		len = sizeof(*disk_key);
		ptr += len;
		sb_ptr += len;
		cur += len;

		if (key.type == BTRFS_CHUNK_ITEM_KEY) {
			chunk = (struct btrfs_chunk *)sb_ptr;
			ret = read_one_chunk(root, &key, sb, chunk);
			BUG_ON(ret);
			num_stripes = btrfs_chunk_num_stripes(sb, chunk);
			len = btrfs_chunk_item_size(num_stripes);
		} else {
			BUG();
		}
		ptr += len;
		sb_ptr += len;
		cur += len;
	}
	free_extent_buffer(sb);
	return 0;
}

int btrfs_read_chunk_tree(struct btrfs_root *root)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_key found_key;
	int ret;
	int slot;

	root = root->fs_info->chunk_root;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* first we search for all of the device items, and then we
	 * read in all of the chunk items.  This way we can create chunk
	 * mappings that reference all of the devices that are afound
	 */
	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.offset = 0;
	key.type = 0;
again:
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
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
		if (key.objectid == BTRFS_DEV_ITEMS_OBJECTID) {
			if (found_key.objectid != BTRFS_DEV_ITEMS_OBJECTID)
				break;
			if (found_key.type == BTRFS_DEV_ITEM_KEY) {
				struct btrfs_dev_item *dev_item;
				dev_item = btrfs_item_ptr(leaf, slot,
						  struct btrfs_dev_item);
				ret = read_one_dev(root, leaf, dev_item);
				BUG_ON(ret);
			}
		} else if (found_key.type == BTRFS_CHUNK_ITEM_KEY) {
			struct btrfs_chunk *chunk;
			chunk = btrfs_item_ptr(leaf, slot, struct btrfs_chunk);
			ret = read_one_chunk(root, &found_key, leaf, chunk);
			BUG_ON(ret);
		}
		path->slots[0]++;
	}
	if (key.objectid == BTRFS_DEV_ITEMS_OBJECTID) {
		key.objectid = 0;
		btrfs_release_path(root, path);
		goto again;
	}

	btrfs_free_path(path);
	ret = 0;
error:
	return ret;
}

struct list_head *btrfs_scanned_uuids(void)
{
	return &fs_uuids;
}
