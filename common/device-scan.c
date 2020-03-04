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

#include "kerncompat.h"
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <linux/limits.h>
#include <blkid/blkid.h>
#include <uuid/uuid.h>
#include "kernel-lib/overflow.h"
#include "common/path-utils.h"
#include "common/device-scan.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/defs.h"
#include "ctree.h"
#include "volumes.h"
#include "disk-io.h"
#include "ioctl.h"

static int btrfs_scan_done = 0;

/*
 * This function checks if the given input parameter is
 * an uuid or a path
 * return <0 : some error in the given input
 * return BTRFS_ARG_UNKNOWN:	unknown input
 * return BTRFS_ARG_UUID:	given input is uuid
 * return BTRFS_ARG_MNTPOINT:	given input is path
 * return BTRFS_ARG_REG:	given input is regular file
 * return BTRFS_ARG_BLKDEV:	given input is block device
 */
int check_arg_type(const char *input)
{
	uuid_t uuid;
	char path[PATH_MAX];

	if (!input)
		return -EINVAL;

	if (realpath(input, path)) {
		if (path_is_block_device(path) == 1)
			return BTRFS_ARG_BLKDEV;

		if (path_is_mount_point(path) == 1)
			return BTRFS_ARG_MNTPOINT;

		if (path_is_reg_file(path))
			return BTRFS_ARG_REG;

		return BTRFS_ARG_UNKNOWN;
	} else {
		return -errno;
	}

	if (strlen(input) == (BTRFS_UUID_UNPARSED_SIZE - 1) &&
		!uuid_parse(input, uuid))
		return BTRFS_ARG_UUID;

	return BTRFS_ARG_UNKNOWN;
}

int test_uuid_unique(char *fs_uuid)
{
	int unique = 1;
	blkid_dev_iterate iter = NULL;
	blkid_dev dev = NULL;
	blkid_cache cache = NULL;

	if (blkid_get_cache(&cache, NULL) < 0) {
		printf("ERROR: lblkid cache get failed\n");
		return 1;
	}
	blkid_probe_all(cache);
	iter = blkid_dev_iterate_begin(cache);
	blkid_dev_set_search(iter, "UUID", fs_uuid);

	while (blkid_dev_next(iter, &dev) == 0) {
		dev = blkid_verify(cache, dev);
		if (dev) {
			unique = 0;
			break;
		}
	}

	blkid_dev_iterate_end(iter);
	blkid_put_cache(cache);

	return unique;
}

int btrfs_add_to_fsid(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, int fd, const char *path,
		      u64 device_total_bytes, u32 io_width, u32 io_align,
		      u32 sectorsize)
{
	struct btrfs_super_block *disk_super;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_super_block *super = fs_info->super_copy;
	struct btrfs_device *device;
	struct btrfs_dev_item *dev_item;
	char *buf = NULL;
	const u64 old_size = btrfs_super_total_bytes(super);
	u64 new_size;
	u64 num_devs;
	int ret;

	device_total_bytes = (device_total_bytes / sectorsize) * sectorsize;

	device = calloc(1, sizeof(*device));
	if (!device) {
		ret = -ENOMEM;
		goto out;
	}
	buf = calloc(1, sectorsize);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	disk_super = (struct btrfs_super_block *)buf;
	dev_item = &disk_super->dev_item;

	uuid_generate(device->uuid);
	device->devid = 0;
	device->type = 0;
	device->io_width = io_width;
	device->io_align = io_align;
	device->sector_size = sectorsize;
	device->fd = fd;
	device->writeable = 1;
	device->total_bytes = device_total_bytes;
	device->bytes_used = 0;
	device->total_ios = 0;
	device->dev_root = fs_info->dev_root;
	device->name = strdup(path);
	if (!device->name) {
		ret = -ENOMEM;
		goto out;
	}

	if (check_add_overflow(old_size, device_total_bytes, &new_size)) {
		error(
		"adding device of %llu (%s) bytes would exceed max file system size",
		      device->total_bytes, pretty_size(device->total_bytes));
		ret = -EOVERFLOW;
		goto out;
	}

	INIT_LIST_HEAD(&device->dev_list);
	ret = btrfs_add_device(trans, fs_info, device);
	if (ret)
		goto out;

	btrfs_set_super_total_bytes(super, new_size);

	num_devs = btrfs_super_num_devices(super) + 1;
	btrfs_set_super_num_devices(super, num_devs);

	memcpy(disk_super, super, sizeof(*disk_super));

	btrfs_set_super_bytenr(disk_super, BTRFS_SUPER_INFO_OFFSET);
	btrfs_set_stack_device_id(dev_item, device->devid);
	btrfs_set_stack_device_type(dev_item, device->type);
	btrfs_set_stack_device_io_align(dev_item, device->io_align);
	btrfs_set_stack_device_io_width(dev_item, device->io_width);
	btrfs_set_stack_device_sector_size(dev_item, device->sector_size);
	btrfs_set_stack_device_total_bytes(dev_item, device->total_bytes);
	btrfs_set_stack_device_bytes_used(dev_item, device->bytes_used);
	memcpy(&dev_item->uuid, device->uuid, BTRFS_UUID_SIZE);

	ret = pwrite(fd, buf, sectorsize, BTRFS_SUPER_INFO_OFFSET);
	BUG_ON(ret != sectorsize);

	free(buf);
	list_add(&device->dev_list, &fs_info->fs_devices->devices);
	device->fs_devices = fs_info->fs_devices;
	return 0;

out:
	free(device);
	free(buf);
	return ret;
}

int btrfs_register_one_device(const char *fname)
{
	struct btrfs_ioctl_vol_args args;
	int fd;
	int ret;

	fd = open("/dev/btrfs-control", O_RDWR);
	if (fd < 0) {
		warning(
	"failed to open /dev/btrfs-control, skipping device registration: %m");
		return -errno;
	}
	memset(&args, 0, sizeof(args));
	strncpy_null(args.name, fname);
	ret = ioctl(fd, BTRFS_IOC_SCAN_DEV, &args);
	if (ret < 0) {
		error("device scan failed on '%s': %m", fname);
		ret = -errno;
	}
	close(fd);
	return ret;
}

/*
 * Register all devices in the fs_uuid list created in the user
 * space. Ensure btrfs_scan_devices() is called before this func.
 */
int btrfs_register_all_devices(void)
{
	int err = 0;
	int ret = 0;
	struct btrfs_fs_devices *fs_devices;
	struct btrfs_device *device;
	struct list_head *all_uuids;

	all_uuids = btrfs_scanned_uuids();

	list_for_each_entry(fs_devices, all_uuids, list) {
		list_for_each_entry(device, &fs_devices->devices, dev_list) {
			if (*device->name)
				err = btrfs_register_one_device(device->name);

			if (err)
				ret++;
		}
	}

	return ret;
}

int btrfs_device_already_in_root(struct btrfs_root *root, int fd,
				 int super_offset)
{
	struct btrfs_super_block *disk_super;
	char *buf;
	int ret = 0;

	buf = malloc(BTRFS_SUPER_INFO_SIZE);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}
	ret = pread(fd, buf, BTRFS_SUPER_INFO_SIZE, super_offset);
	if (ret != BTRFS_SUPER_INFO_SIZE)
		goto brelse;

	ret = 0;
	disk_super = (struct btrfs_super_block *)buf;
	/*
	 * Accept devices from the same filesystem, allow partially created
	 * structures.
	 */
	if (btrfs_super_magic(disk_super) != BTRFS_MAGIC &&
			btrfs_super_magic(disk_super) != BTRFS_MAGIC_TEMPORARY)
		goto brelse;

	if (!memcmp(disk_super->fsid, root->fs_info->super_copy->fsid,
		    BTRFS_FSID_SIZE))
		ret = 1;
brelse:
	free(buf);
out:
	return ret;
}

int is_seen_fsid(u8 *fsid, struct seen_fsid *seen_fsid_hash[])
{
	u8 hash = fsid[0];
	int slot = hash % SEEN_FSID_HASH_SIZE;
	struct seen_fsid *seen = seen_fsid_hash[slot];

	while (seen) {
		if (memcmp(seen->fsid, fsid, BTRFS_FSID_SIZE) == 0)
			return 1;

		seen = seen->next;
	}

	return 0;
}

int add_seen_fsid(u8 *fsid, struct seen_fsid *seen_fsid_hash[],
		int fd, DIR *dirstream)
{
	u8 hash = fsid[0];
	int slot = hash % SEEN_FSID_HASH_SIZE;
	struct seen_fsid *seen = seen_fsid_hash[slot];
	struct seen_fsid *alloc;

	if (!seen)
		goto insert;

	while (1) {
		if (memcmp(seen->fsid, fsid, BTRFS_FSID_SIZE) == 0)
			return -EEXIST;

		if (!seen->next)
			break;

		seen = seen->next;
	}

insert:
	alloc = malloc(sizeof(*alloc));
	if (!alloc)
		return -ENOMEM;

	alloc->next = NULL;
	memcpy(alloc->fsid, fsid, BTRFS_FSID_SIZE);
	alloc->fd = fd;
	alloc->dirstream = dirstream;

	if (seen)
		seen->next = alloc;
	else
		seen_fsid_hash[slot] = alloc;

	return 0;
}

void free_seen_fsid(struct seen_fsid *seen_fsid_hash[])
{
	int slot;
	struct seen_fsid *seen;
	struct seen_fsid *next;

	for (slot = 0; slot < SEEN_FSID_HASH_SIZE; slot++) {
		seen = seen_fsid_hash[slot];
		while (seen) {
			next = seen->next;
			close_file_or_dir(seen->fd, seen->dirstream);
			free(seen);
			seen = next;
		}
		seen_fsid_hash[slot] = NULL;
	}
}

int btrfs_scan_devices(void)
{
	int fd = -1;
	int ret;
	u64 num_devices;
	struct btrfs_fs_devices *tmp_devices;
	blkid_dev_iterate iter = NULL;
	blkid_dev dev = NULL;
	blkid_cache cache = NULL;
	char path[PATH_MAX];

	if (btrfs_scan_done)
		return 0;

	ret = blkid_get_cache(&cache, NULL);
	if (ret < 0) {
		errno = -ret;
		error("blkid cache get failed: %m");
		return ret;
	}
	blkid_probe_all(cache);
	iter = blkid_dev_iterate_begin(cache);
	blkid_dev_set_search(iter, "TYPE", "btrfs");
	while (blkid_dev_next(iter, &dev) == 0) {
		dev = blkid_verify(cache, dev);
		if (!dev)
			continue;
		/* if we are here its definitely a btrfs disk*/
		strncpy_null(path, blkid_dev_devname(dev));

		fd = open(path, O_RDONLY);
		if (fd < 0) {
			error("cannot open %s: %m", path);
			continue;
		}
		ret = btrfs_scan_one_device(fd, path, &tmp_devices,
				&num_devices, BTRFS_SUPER_INFO_OFFSET,
				SBREAD_DEFAULT);
		if (ret) {
			errno = -ret;
			error("cannot scan %s: %m", path);
			close (fd);
			continue;
		}

		close(fd);
	}
	blkid_dev_iterate_end(iter);
	blkid_put_cache(cache);

	btrfs_scan_done = 1;

	return 0;
}

