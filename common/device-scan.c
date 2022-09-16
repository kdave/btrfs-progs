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

#ifdef STATIC_BUILD
#undef HAVE_LIBUDEV
#endif

#include "kerncompat.h"
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <blkid/blkid.h>
#include <uuid/uuid.h>
#ifdef HAVE_LIBUDEV
#include <sys/stat.h>
#include <libudev.h>
#endif
#include "kernel-lib/overflow.h"
#include "kernel-lib/list.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/zoned.h"
#include "common/path-utils.h"
#include "common/device-scan.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/defs.h"
#include "common/open-utils.h"
#include "common/units.h"
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

/* Check if the UUID (as string) appears among devices cached by blkid */
int test_uuid_unique(const char *uuid_str)
{
	int unique = 1;
	blkid_dev_iterate iter = NULL;
	blkid_dev dev = NULL;
	blkid_cache cache = NULL;

	if (blkid_get_cache(&cache, NULL) < 0) {
		error("blkid cache open failed, cannot check uuid uniqueness");
		return 1;
	}
	blkid_probe_all(cache);
	iter = blkid_dev_iterate_begin(cache);
	blkid_dev_set_search(iter, "UUID", uuid_str);

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
	device->fs_info = fs_info;
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

	ret = sbwrite(fd, buf, BTRFS_SUPER_INFO_OFFSET);
	/* Ensure super block was written to the device */
	BUG_ON(ret != BTRFS_SUPER_INFO_SIZE);
	free(buf);
	list_add(&device->dev_list, &fs_info->fs_devices->devices);
	device->fs_devices = fs_info->fs_devices;
	return 0;

out:
	free(device->zone_info);
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
	struct btrfs_super_block disk_super;
	int ret = 0;

	ret = sbread(fd, &disk_super, super_offset);
	if (ret != BTRFS_SUPER_INFO_SIZE)
		goto out;

	ret = 0;
	/*
	 * Accept devices from the same filesystem, allow partially created
	 * structures.
	 */
	if (btrfs_super_magic(&disk_super) != BTRFS_MAGIC &&
			btrfs_super_magic(&disk_super) != BTRFS_MAGIC_TEMPORARY)
		goto out;

	if (!memcmp(disk_super.fsid, root->fs_info->super_copy->fsid,
		    BTRFS_FSID_SIZE))
		ret = 1;
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

#ifdef STATIC_BUILD
static bool is_multipath_path_device(dev_t device)
{
	FILE *file;
	char *line = NULL;
	size_t len = 0;
	ssize_t nread;
	bool ret = false;
	int ret2;
	char path[PATH_MAX];

	ret2 = snprintf(path, sizeof(path), "/run/udev/data/b%u:%u", major(device),
			minor(device));

	if (ret2 < 0)
		return false;

	file = fopen(path, "r");
	if (file == NULL)
		return false;

	while ((nread = getline(&line, &len, file)) != -1) {
		if (strstr(line, "DM_MULTIPATH_DEVICE_PATH=1")) {
			ret = true;
			break;
		}
	}

	if (line)
		free(line);

	fclose(file);

	return ret;
}
#elif defined(HAVE_LIBUDEV)
static bool is_multipath_path_device(dev_t device)
{
	struct udev *udev = NULL;
	struct udev_device *dev = NULL;
	const char *val;
	bool ret = false;

	udev = udev_new();
	if (!udev)
		goto out;

	dev = udev_device_new_from_devnum(udev, 'b', device);
	if (!dev)
		goto out;

	val = udev_device_get_property_value(dev, "DM_MULTIPATH_DEVICE_PATH");
	if (val && atoi(val) > 0)
		ret = true;
out:
	udev_device_unref(dev);
	udev_unref(udev);

	return ret;
}
#else
static bool is_multipath_path_device(dev_t device)
{
	return false;
}
#endif

int btrfs_scan_devices(int verbose)
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
		struct stat dev_stat;

		dev = blkid_verify(cache, dev);
		if (!dev)
			continue;
		/* if we are here its definitely a btrfs disk*/
		strncpy_null(path, blkid_dev_devname(dev));

		if (stat(path, &dev_stat) < 0)
			continue;

		if (is_multipath_path_device(dev_stat.st_rdev))
			continue;

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
		pr_verbose(verbose, "registered: %s\n", path);

		close(fd);
	}
	blkid_dev_iterate_end(iter);
	blkid_put_cache(cache);

	btrfs_scan_done = 1;

	return 0;
}

