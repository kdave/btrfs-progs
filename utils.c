/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 * Copyright (C) 2008 Morey Roof.  All rights reserved.
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
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <fcntl.h>
#include <unistd.h>
#include <mntent.h>
#include <ctype.h>
#include <linux/loop.h>
#include <linux/major.h>
#include <linux/kdev_t.h>
#include <limits.h>
#include <blkid/blkid.h>
#include <sys/vfs.h>
#include <sys/statfs.h>
#include <linux/magic.h>
#include <getopt.h>

#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "crc32c.h"
#include "utils.h"
#include "volumes.h"
#include "ioctl.h"
#include "commands.h"
#include "mkfs/common.h"

#ifndef BLKDISCARD
#define BLKDISCARD	_IO(0x12,119)
#endif

static int btrfs_scan_done = 0;

static int rand_seed_initlized = 0;
static unsigned short rand_seed[3];

struct btrfs_config bconf;

/*
 * Discard the given range in one go
 */
static int discard_range(int fd, u64 start, u64 len)
{
	u64 range[2] = { start, len };

	if (ioctl(fd, BLKDISCARD, &range) < 0)
		return errno;
	return 0;
}

/*
 * Discard blocks in the given range in 1G chunks, the process is interruptible
 */
static int discard_blocks(int fd, u64 start, u64 len)
{
	while (len > 0) {
		/* 1G granularity */
		u64 chunk_size = min_t(u64, len, SZ_1G);
		int ret;

		ret = discard_range(fd, start, chunk_size);
		if (ret)
			return ret;
		len -= chunk_size;
		start += chunk_size;
	}

	return 0;
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

u64 btrfs_device_size(int fd, struct stat *st)
{
	u64 size;
	if (S_ISREG(st->st_mode)) {
		return st->st_size;
	}
	if (!S_ISBLK(st->st_mode)) {
		return 0;
	}
	if (ioctl(fd, BLKGETSIZE64, &size) >= 0) {
		return size;
	}
	return 0;
}

static int zero_blocks(int fd, off_t start, size_t len)
{
	char *buf = malloc(len);
	int ret = 0;
	ssize_t written;

	if (!buf)
		return -ENOMEM;
	memset(buf, 0, len);
	written = pwrite(fd, buf, len, start);
	if (written != len)
		ret = -EIO;
	free(buf);
	return ret;
}

#define ZERO_DEV_BYTES SZ_2M

/* don't write outside the device by clamping the region to the device size */
static int zero_dev_clamped(int fd, off_t start, ssize_t len, u64 dev_size)
{
	off_t end = max(start, start + len);

#ifdef __sparc__
	/* and don't overwrite the disk labels on sparc */
	start = max(start, 1024);
	end = max(end, 1024);
#endif

	start = min_t(u64, start, dev_size);
	end = min_t(u64, end, dev_size);

	return zero_blocks(fd, start, end - start);
}

int btrfs_add_to_fsid(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, int fd, const char *path,
		      u64 device_total_bytes, u32 io_width, u32 io_align,
		      u32 sectorsize)
{
	struct btrfs_super_block *disk_super;
	struct btrfs_super_block *super = root->fs_info->super_copy;
	struct btrfs_device *device;
	struct btrfs_dev_item *dev_item;
	char *buf = NULL;
	u64 fs_total_bytes;
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
	device->dev_root = root->fs_info->dev_root;
	device->name = strdup(path);
	if (!device->name) {
		ret = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&device->dev_list);
	ret = btrfs_add_device(trans, root, device);
	if (ret)
		goto out;

	fs_total_bytes = btrfs_super_total_bytes(super) + device_total_bytes;
	btrfs_set_super_total_bytes(super, fs_total_bytes);

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
	list_add(&device->dev_list, &root->fs_info->fs_devices->devices);
	device->fs_devices = root->fs_info->fs_devices;
	return 0;

out:
	free(device);
	free(buf);
	return ret;
}

static int btrfs_wipe_existing_sb(int fd)
{
	const char *off = NULL;
	size_t len = 0;
	loff_t offset;
	char buf[BUFSIZ];
	int ret = 0;
	blkid_probe pr = NULL;

	pr = blkid_new_probe();
	if (!pr)
		return -1;

	if (blkid_probe_set_device(pr, fd, 0, 0)) {
		ret = -1;
		goto out;
	}

	ret = blkid_probe_lookup_value(pr, "SBMAGIC_OFFSET", &off, NULL);
	if (!ret)
		ret = blkid_probe_lookup_value(pr, "SBMAGIC", NULL, &len);

	if (ret || len == 0 || off == NULL) {
		/*
		 * If lookup fails, the probe did not find any values, eg. for
		 * a file image or a loop device. Soft error.
		 */
		ret = 1;
		goto out;
	}

	offset = strtoll(off, NULL, 10);
	if (len > sizeof(buf))
		len = sizeof(buf);

	memset(buf, 0, len);
	ret = pwrite(fd, buf, len, offset);
	if (ret < 0) {
		error("cannot wipe existing superblock: %s", strerror(errno));
		ret = -1;
	} else if (ret != len) {
		error("cannot wipe existing superblock: wrote %d of %zd", ret, len);
		ret = -1;
	}
	fsync(fd);

out:
	blkid_free_probe(pr);
	return ret;
}

int btrfs_prepare_device(int fd, const char *file, u64 *block_count_ret,
		u64 max_block_count, unsigned opflags)
{
	u64 block_count;
	struct stat st;
	int i, ret;

	ret = fstat(fd, &st);
	if (ret < 0) {
		error("unable to stat %s: %s", file, strerror(errno));
		return 1;
	}

	block_count = btrfs_device_size(fd, &st);
	if (block_count == 0) {
		error("unable to determine size of %s", file);
		return 1;
	}
	if (max_block_count)
		block_count = min(block_count, max_block_count);

	if (opflags & PREP_DEVICE_DISCARD) {
		/*
		 * We intentionally ignore errors from the discard ioctl.  It
		 * is not necessary for the mkfs functionality but just an
		 * optimization.
		 */
		if (discard_range(fd, 0, 0) == 0) {
			if (opflags & PREP_DEVICE_VERBOSE)
				printf("Performing full device TRIM %s (%s) ...\n",
						file, pretty_size(block_count));
			discard_blocks(fd, 0, block_count);
		}
	}

	ret = zero_dev_clamped(fd, 0, ZERO_DEV_BYTES, block_count);
	for (i = 0 ; !ret && i < BTRFS_SUPER_MIRROR_MAX; i++)
		ret = zero_dev_clamped(fd, btrfs_sb_offset(i),
				       BTRFS_SUPER_INFO_SIZE, block_count);
	if (!ret && (opflags & PREP_DEVICE_ZERO_END))
		ret = zero_dev_clamped(fd, block_count - ZERO_DEV_BYTES,
				       ZERO_DEV_BYTES, block_count);

	if (ret < 0) {
		error("failed to zero device '%s': %s", file, strerror(-ret));
		return 1;
	}

	ret = btrfs_wipe_existing_sb(fd);
	if (ret < 0) {
		error("cannot wipe superblocks on %s", file);
		return 1;
	}

	*block_count_ret = block_count;
	return 0;
}

int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid)
{
	int ret;
	struct btrfs_inode_item inode_item;
	time_t now = time(NULL);

	memset(&inode_item, 0, sizeof(inode_item));
	btrfs_set_stack_inode_generation(&inode_item, trans->transid);
	btrfs_set_stack_inode_size(&inode_item, 0);
	btrfs_set_stack_inode_nlink(&inode_item, 1);
	btrfs_set_stack_inode_nbytes(&inode_item, root->fs_info->nodesize);
	btrfs_set_stack_inode_mode(&inode_item, S_IFDIR | 0755);
	btrfs_set_stack_timespec_sec(&inode_item.atime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.atime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.ctime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.ctime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.mtime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.mtime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.otime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.otime, 0);

	if (root->fs_info->tree_root == root)
		btrfs_set_super_root_dir(root->fs_info->super_copy, objectid);

	ret = btrfs_insert_inode(trans, root, objectid, &inode_item);
	if (ret)
		goto error;

	ret = btrfs_insert_inode_ref(trans, root, "..", 2, objectid, objectid, 0);
	if (ret)
		goto error;

	btrfs_set_root_dirid(&root->root_item, objectid);
	ret = 0;
error:
	return ret;
}

/*
 * checks if a path is a block device node
 * Returns negative errno on failure, otherwise
 * returns 1 for blockdev, 0 for not-blockdev
 */
int is_block_device(const char *path)
{
	struct stat statbuf;

	if (stat(path, &statbuf) < 0)
		return -errno;

	return !!S_ISBLK(statbuf.st_mode);
}

/*
 * check if given path is a mount point
 * return 1 if yes. 0 if no. -1 for error
 */
int is_mount_point(const char *path)
{
	FILE *f;
	struct mntent *mnt;
	int ret = 0;

	f = setmntent("/proc/self/mounts", "r");
	if (f == NULL)
		return -1;

	while ((mnt = getmntent(f)) != NULL) {
		if (strcmp(mnt->mnt_dir, path))
			continue;
		ret = 1;
		break;
	}
	endmntent(f);
	return ret;
}

static int is_reg_file(const char *path)
{
	struct stat statbuf;

	if (stat(path, &statbuf) < 0)
		return -errno;
	return S_ISREG(statbuf.st_mode);
}

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
		if (is_block_device(path) == 1)
			return BTRFS_ARG_BLKDEV;

		if (is_mount_point(path) == 1)
			return BTRFS_ARG_MNTPOINT;

		if (is_reg_file(path))
			return BTRFS_ARG_REG;

		return BTRFS_ARG_UNKNOWN;
	}

	if (strlen(input) == (BTRFS_UUID_UNPARSED_SIZE - 1) &&
		!uuid_parse(input, uuid))
		return BTRFS_ARG_UUID;

	return BTRFS_ARG_UNKNOWN;
}

/*
 * Find the mount point for a mounted device.
 * On success, returns 0 with mountpoint in *mp.
 * On failure, returns -errno (not mounted yields -EINVAL)
 * Is noisy on failures, expects to be given a mounted device.
 */
int get_btrfs_mount(const char *dev, char *mp, size_t mp_size)
{
	int ret;
	int fd = -1;

	ret = is_block_device(dev);
	if (ret <= 0) {
		if (!ret) {
			error("not a block device: %s", dev);
			ret = -EINVAL;
		} else {
			error("cannot check %s: %s", dev, strerror(-ret));
		}
		goto out;
	}

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		error("cannot open %s: %s", dev, strerror(errno));
		goto out;
	}

	ret = check_mounted_where(fd, dev, mp, mp_size, NULL);
	if (!ret) {
		ret = -EINVAL;
	} else { /* mounted, all good */
		ret = 0;
	}
out:
	if (fd != -1)
		close(fd);
	return ret;
}

/*
 * Given a pathname, return a filehandle to:
 * 	the original pathname or,
 * 	if the pathname is a mounted btrfs device, to its mountpoint.
 *
 * On error, return -1, errno should be set.
 */
int open_path_or_dev_mnt(const char *path, DIR **dirstream, int verbose)
{
	char mp[PATH_MAX];
	int ret;

	if (is_block_device(path)) {
		ret = get_btrfs_mount(path, mp, sizeof(mp));
		if (ret < 0) {
			/* not a mounted btrfs dev */
			error_on(verbose, "'%s' is not a mounted btrfs device",
				 path);
			errno = EINVAL;
			return -1;
		}
		ret = open_file_or_dir(mp, dirstream);
		error_on(verbose && ret < 0, "can't access '%s': %s",
			 path, strerror(errno));
	} else {
		ret = btrfs_open_dir(path, dirstream, 1);
	}

	return ret;
}

/*
 * Do the following checks before calling open_file_or_dir():
 * 1: path is in a btrfs filesystem
 * 2: path is a directory
 */
int btrfs_open_dir(const char *path, DIR **dirstream, int verbose)
{
	struct statfs stfs;
	struct stat st;
	int ret;

	if (statfs(path, &stfs) != 0) {
		error_on(verbose, "cannot access '%s': %s", path,
				strerror(errno));
		return -1;
	}

	if (stfs.f_type != BTRFS_SUPER_MAGIC) {
		error_on(verbose, "not a btrfs filesystem: %s", path);
		return -2;
	}

	if (stat(path, &st) != 0) {
		error_on(verbose, "cannot access '%s': %s", path,
				strerror(errno));
		return -1;
	}

	if (!S_ISDIR(st.st_mode)) {
		error_on(verbose, "not a directory: %s", path);
		return -3;
	}

	ret = open_file_or_dir(path, dirstream);
	if (ret < 0) {
		error_on(verbose, "cannot access '%s': %s", path,
				strerror(errno));
	}

	return ret;
}

/* checks if a device is a loop device */
static int is_loop_device (const char* device) {
	struct stat statbuf;

	if(stat(device, &statbuf) < 0)
		return -errno;

	return (S_ISBLK(statbuf.st_mode) &&
		MAJOR(statbuf.st_rdev) == LOOP_MAJOR);
}

/*
 * Takes a loop device path (e.g. /dev/loop0) and returns
 * the associated file (e.g. /images/my_btrfs.img) using
 * loopdev API
 */
static int resolve_loop_device_with_loopdev(const char* loop_dev, char* loop_file)
{
	int fd;
	int ret;
	struct loop_info64 lo64;

	fd = open(loop_dev, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	ret = ioctl(fd, LOOP_GET_STATUS64, &lo64);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}

	memcpy(loop_file, lo64.lo_file_name, sizeof(lo64.lo_file_name));
	loop_file[sizeof(lo64.lo_file_name)] = 0;

out:
	close(fd);

	return ret;
}

/* Takes a loop device path (e.g. /dev/loop0) and returns
 * the associated file (e.g. /images/my_btrfs.img) */
static int resolve_loop_device(const char* loop_dev, char* loop_file,
		int max_len)
{
	int ret;
	FILE *f;
	char fmt[20];
	char p[PATH_MAX];
	char real_loop_dev[PATH_MAX];

	if (!realpath(loop_dev, real_loop_dev))
		return -errno;
	snprintf(p, PATH_MAX, "/sys/block/%s/loop/backing_file", strrchr(real_loop_dev, '/'));
	if (!(f = fopen(p, "r"))) {
		if (errno == ENOENT)
			/*
			 * It's possibly a partitioned loop device, which is
			 * resolvable with loopdev API.
			 */
			return resolve_loop_device_with_loopdev(loop_dev, loop_file);
		return -errno;
	}

	snprintf(fmt, 20, "%%%i[^\n]", max_len-1);
	ret = fscanf(f, fmt, loop_file);
	fclose(f);
	if (ret == EOF)
		return -errno;

	return 0;
}

/*
 * Checks whether a and b are identical or device
 * files associated with the same block device
 */
static int is_same_blk_file(const char* a, const char* b)
{
	struct stat st_buf_a, st_buf_b;
	char real_a[PATH_MAX];
	char real_b[PATH_MAX];

	if (!realpath(a, real_a))
		strncpy_null(real_a, a);

	if (!realpath(b, real_b))
		strncpy_null(real_b, b);

	/* Identical path? */
	if (strcmp(real_a, real_b) == 0)
		return 1;

	if (stat(a, &st_buf_a) < 0 || stat(b, &st_buf_b) < 0) {
		if (errno == ENOENT)
			return 0;
		return -errno;
	}

	/* Same blockdevice? */
	if (S_ISBLK(st_buf_a.st_mode) && S_ISBLK(st_buf_b.st_mode) &&
	    st_buf_a.st_rdev == st_buf_b.st_rdev) {
		return 1;
	}

	/* Hardlink? */
	if (st_buf_a.st_dev == st_buf_b.st_dev &&
	    st_buf_a.st_ino == st_buf_b.st_ino) {
		return 1;
	}

	return 0;
}

/* checks if a and b are identical or device
 * files associated with the same block device or
 * if one file is a loop device that uses the other
 * file.
 */
static int is_same_loop_file(const char* a, const char* b)
{
	char res_a[PATH_MAX];
	char res_b[PATH_MAX];
	const char* final_a = NULL;
	const char* final_b = NULL;
	int ret;

	/* Resolve a if it is a loop device */
	if((ret = is_loop_device(a)) < 0) {
		if (ret == -ENOENT)
			return 0;
		return ret;
	} else if (ret) {
		ret = resolve_loop_device(a, res_a, sizeof(res_a));
		if (ret < 0) {
			if (errno != EPERM)
				return ret;
		} else {
			final_a = res_a;
		}
	} else {
		final_a = a;
	}

	/* Resolve b if it is a loop device */
	if ((ret = is_loop_device(b)) < 0) {
		if (ret == -ENOENT)
			return 0;
		return ret;
	} else if (ret) {
		ret = resolve_loop_device(b, res_b, sizeof(res_b));
		if (ret < 0) {
			if (errno != EPERM)
				return ret;
		} else {
			final_b = res_b;
		}
	} else {
		final_b = b;
	}

	return is_same_blk_file(final_a, final_b);
}

/* Checks if a file exists and is a block or regular file*/
static int is_existing_blk_or_reg_file(const char* filename)
{
	struct stat st_buf;

	if(stat(filename, &st_buf) < 0) {
		if(errno == ENOENT)
			return 0;
		else
			return -errno;
	}

	return (S_ISBLK(st_buf.st_mode) || S_ISREG(st_buf.st_mode));
}

/* Checks if a file is used (directly or indirectly via a loop device)
 * by a device in fs_devices
 */
static int blk_file_in_dev_list(struct btrfs_fs_devices* fs_devices,
		const char* file)
{
	int ret;
	struct list_head *head;
	struct list_head *cur;
	struct btrfs_device *device;

	head = &fs_devices->devices;
	list_for_each(cur, head) {
		device = list_entry(cur, struct btrfs_device, dev_list);

		if((ret = is_same_loop_file(device->name, file)))
			return ret;
	}

	return 0;
}

/*
 * Resolve a pathname to a device mapper node to /dev/mapper/<name>
 * Returns NULL on invalid input or malloc failure; Other failures
 * will be handled by the caller using the input pathame.
 */
char *canonicalize_dm_name(const char *ptname)
{
	FILE	*f;
	size_t	sz;
	char	path[PATH_MAX], name[PATH_MAX], *res = NULL;

	if (!ptname || !*ptname)
		return NULL;

	snprintf(path, sizeof(path), "/sys/block/%s/dm/name", ptname);
	if (!(f = fopen(path, "r")))
		return NULL;

	/* read <name>\n from sysfs */
	if (fgets(name, sizeof(name), f) && (sz = strlen(name)) > 1) {
		name[sz - 1] = '\0';
		snprintf(path, sizeof(path), "/dev/mapper/%s", name);

		if (access(path, F_OK) == 0)
			res = strdup(path);
	}
	fclose(f);
	return res;
}

/*
 * Resolve a pathname to a canonical device node, e.g. /dev/sda1 or
 * to a device mapper pathname.
 * Returns NULL on invalid input or malloc failure; Other failures
 * will be handled by the caller using the input pathame.
 */
char *canonicalize_path(const char *path)
{
	char *canonical, *p;

	if (!path || !*path)
		return NULL;

	canonical = realpath(path, NULL);
	if (!canonical)
		return strdup(path);
	p = strrchr(canonical, '/');
	if (p && strncmp(p, "/dm-", 4) == 0 && isdigit(*(p + 4))) {
		char *dm = canonicalize_dm_name(p + 1);

		if (dm) {
			free(canonical);
			return dm;
		}
	}
	return canonical;
}

/*
 * returns 1 if the device was mounted, < 0 on error or 0 if everything
 * is safe to continue.
 */
int check_mounted(const char* file)
{
	int fd;
	int ret;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		error("mount check: cannot open %s: %s", file,
				strerror(errno));
		return -errno;
	}

	ret =  check_mounted_where(fd, file, NULL, 0, NULL);
	close(fd);

	return ret;
}

int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_dev_ret)
{
	int ret;
	u64 total_devs = 1;
	int is_btrfs;
	struct btrfs_fs_devices *fs_devices_mnt = NULL;
	FILE *f;
	struct mntent *mnt;

	/* scan the initial device */
	ret = btrfs_scan_one_device(fd, file, &fs_devices_mnt,
		    &total_devs, BTRFS_SUPER_INFO_OFFSET, SBREAD_DEFAULT);
	is_btrfs = (ret >= 0);

	/* scan other devices */
	if (is_btrfs && total_devs > 1) {
		ret = btrfs_scan_devices();
		if (ret)
			return ret;
	}

	/* iterate over the list of currently mounted filesystems */
	if ((f = setmntent ("/proc/self/mounts", "r")) == NULL)
		return -errno;

	while ((mnt = getmntent (f)) != NULL) {
		if(is_btrfs) {
			if(strcmp(mnt->mnt_type, "btrfs") != 0)
				continue;

			ret = blk_file_in_dev_list(fs_devices_mnt, mnt->mnt_fsname);
		} else {
			/* ignore entries in the mount table that are not
			   associated with a file*/
			if((ret = is_existing_blk_or_reg_file(mnt->mnt_fsname)) < 0)
				goto out_mntloop_err;
			else if(!ret)
				continue;

			ret = is_same_loop_file(file, mnt->mnt_fsname);
		}

		if(ret < 0)
			goto out_mntloop_err;
		else if(ret)
			break;
	}

	/* Did we find an entry in mnt table? */
	if (mnt && size && where) {
		strncpy(where, mnt->mnt_dir, size);
		where[size-1] = 0;
	}
	if (fs_dev_ret)
		*fs_dev_ret = fs_devices_mnt;

	ret = (mnt != NULL);

out_mntloop_err:
	endmntent (f);

	return ret;
}

struct pending_dir {
	struct list_head list;
	char name[PATH_MAX];
};

int btrfs_register_one_device(const char *fname)
{
	struct btrfs_ioctl_vol_args args;
	int fd;
	int ret;

	fd = open("/dev/btrfs-control", O_RDWR);
	if (fd < 0) {
		warning(
	"failed to open /dev/btrfs-control, skipping device registration: %s",
			strerror(errno));
		return -errno;
	}
	memset(&args, 0, sizeof(args));
	strncpy_null(args.name, fname);
	ret = ioctl(fd, BTRFS_IOC_SCAN_DEV, &args);
	if (ret < 0) {
		error("device scan failed on '%s': %s", fname,
				strerror(errno));
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
			btrfs_super_magic(disk_super) != BTRFS_MAGIC_PARTIAL)
		goto brelse;

	if (!memcmp(disk_super->fsid, root->fs_info->super_copy->fsid,
		    BTRFS_FSID_SIZE))
		ret = 1;
brelse:
	free(buf);
out:
	return ret;
}

/*
 * Note: this function uses a static per-thread buffer. Do not call this
 * function more than 10 times within one argument list!
 */
const char *pretty_size_mode(u64 size, unsigned mode)
{
	static __thread int ps_index = 0;
	static __thread char ps_array[10][32];
	char *ret;

	ret = ps_array[ps_index];
	ps_index++;
	ps_index %= 10;
	(void)pretty_size_snprintf(size, ret, 32, mode);

	return ret;
}

static const char* unit_suffix_binary[] =
	{ "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
static const char* unit_suffix_decimal[] =
	{ "B", "kB", "MB", "GB", "TB", "PB", "EB"};

int pretty_size_snprintf(u64 size, char *str, size_t str_size, unsigned unit_mode)
{
	int num_divs;
	float fraction;
	u64 base = 0;
	int mult = 0;
	const char** suffix = NULL;
	u64 last_size;
	int negative;

	if (str_size == 0)
		return 0;

	negative = !!(unit_mode & UNITS_NEGATIVE);
	unit_mode &= ~UNITS_NEGATIVE;

	if ((unit_mode & ~UNITS_MODE_MASK) == UNITS_RAW) {
		if (negative)
			snprintf(str, str_size, "%lld", size);
		else
			snprintf(str, str_size, "%llu", size);
		return 0;
	}

	if ((unit_mode & ~UNITS_MODE_MASK) == UNITS_BINARY) {
		base = 1024;
		mult = 1024;
		suffix = unit_suffix_binary;
	} else if ((unit_mode & ~UNITS_MODE_MASK) == UNITS_DECIMAL) {
		base = 1000;
		mult = 1000;
		suffix = unit_suffix_decimal;
	}

	/* Unknown mode */
	if (!base) {
		fprintf(stderr, "INTERNAL ERROR: unknown unit base, mode %d\n",
				unit_mode);
		assert(0);
		return -1;
	}

	num_divs = 0;
	last_size = size;
	switch (unit_mode & UNITS_MODE_MASK) {
	case UNITS_TBYTES: base *= mult; num_divs++;
	case UNITS_GBYTES: base *= mult; num_divs++;
	case UNITS_MBYTES: base *= mult; num_divs++;
	case UNITS_KBYTES: num_divs++;
			   break;
	case UNITS_BYTES:
			   base = 1;
			   num_divs = 0;
			   break;
	default:
		if (negative) {
			s64 ssize = (s64)size;
			s64 last_ssize = ssize;

			while ((ssize < 0 ? -ssize : ssize) >= mult) {
				last_ssize = ssize;
				ssize /= mult;
				num_divs++;
			}
			last_size = (u64)last_ssize;
		} else {
			while (size >= mult) {
				last_size = size;
				size /= mult;
				num_divs++;
			}
		}
		/*
		 * If the value is smaller than base, we didn't do any
		 * division, in that case, base should be 1, not original
		 * base, or the unit will be wrong
		 */
		if (num_divs == 0)
			base = 1;
	}

	if (num_divs >= ARRAY_SIZE(unit_suffix_binary)) {
		str[0] = '\0';
		printf("INTERNAL ERROR: unsupported unit suffix, index %d\n",
				num_divs);
		assert(0);
		return -1;
	}

	if (negative) {
		fraction = (float)(s64)last_size / base;
	} else {
		fraction = (float)last_size / base;
	}

	return snprintf(str, str_size, "%.2f%s", fraction, suffix[num_divs]);
}

/*
 * __strncpy_null - strncpy with null termination
 * @dest:	the target array
 * @src:	the source string
 * @n:		maximum bytes to copy (size of *dest)
 *
 * Like strncpy, but ensures destination is null-terminated.
 *
 * Copies the string pointed to by src, including the terminating null
 * byte ('\0'), to the buffer pointed to by dest, up to a maximum
 * of n bytes.  Then ensure that dest is null-terminated.
 */
char *__strncpy_null(char *dest, const char *src, size_t n)
{
	strncpy(dest, src, n);
	if (n > 0)
		dest[n - 1] = '\0';
	return dest;
}

/*
 * Checks to make sure that the label matches our requirements.
 * Returns:
       0    if everything is safe and usable
      -1    if the label is too long
 */
static int check_label(const char *input)
{
       int len = strlen(input);

       if (len > BTRFS_LABEL_SIZE - 1) {
		error("label %s is too long (max %d)", input,
				BTRFS_LABEL_SIZE - 1);
               return -1;
       }

       return 0;
}

static int set_label_unmounted(const char *dev, const char *label)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	int ret;

	ret = check_mounted(dev);
	if (ret < 0) {
	       error("checking mount status of %s failed: %d", dev, ret);
	       return -1;
	}
	if (ret > 0) {
		error("device %s is mounted, use mount point", dev);
		return -1;
	}

	/* Open the super_block at the default location
	 * and as read-write.
	 */
	root = open_ctree(dev, 0, OPEN_CTREE_WRITES);
	if (!root) /* errors are printed by open_ctree() */
		return -1;

	trans = btrfs_start_transaction(root, 1);
	__strncpy_null(root->fs_info->super_copy->label, label, BTRFS_LABEL_SIZE - 1);

	btrfs_commit_transaction(trans, root);

	/* Now we close it since we are done. */
	close_ctree(root);
	return 0;
}

static int set_label_mounted(const char *mount_path, const char *labelp)
{
	int fd;
	char label[BTRFS_LABEL_SIZE];

	fd = open(mount_path, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		error("unable to access %s: %s", mount_path, strerror(errno));
		return -1;
	}

	memset(label, 0, sizeof(label));
	__strncpy_null(label, labelp, BTRFS_LABEL_SIZE - 1);
	if (ioctl(fd, BTRFS_IOC_SET_FSLABEL, label) < 0) {
		error("unable to set label of %s: %s", mount_path,
				strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

int get_label_unmounted(const char *dev, char *label)
{
	struct btrfs_root *root;
	int ret;

	ret = check_mounted(dev);
	if (ret < 0) {
	       error("checking mount status of %s failed: %d", dev, ret);
	       return -1;
	}

	/* Open the super_block at the default location
	 * and as read-only.
	 */
	root = open_ctree(dev, 0, 0);
	if(!root)
		return -1;

	__strncpy_null(label, root->fs_info->super_copy->label,
			BTRFS_LABEL_SIZE - 1);

	/* Now we close it since we are done. */
	close_ctree(root);
	return 0;
}

/*
 * If a partition is mounted, try to get the filesystem label via its
 * mounted path rather than device.  Return the corresponding error
 * the user specified the device path.
 */
int get_label_mounted(const char *mount_path, char *labelp)
{
	char label[BTRFS_LABEL_SIZE];
	int fd;
	int ret;

	fd = open(mount_path, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		error("unable to access %s: %s", mount_path, strerror(errno));
		return -1;
	}

	memset(label, '\0', sizeof(label));
	ret = ioctl(fd, BTRFS_IOC_GET_FSLABEL, label);
	if (ret < 0) {
		if (errno != ENOTTY)
			error("unable to get label of %s: %s", mount_path,
					strerror(errno));
		ret = -errno;
		close(fd);
		return ret;
	}

	__strncpy_null(labelp, label, BTRFS_LABEL_SIZE - 1);
	close(fd);
	return 0;
}

int get_label(const char *btrfs_dev, char *label)
{
	int ret;

	ret = is_existing_blk_or_reg_file(btrfs_dev);
	if (!ret)
		ret = get_label_mounted(btrfs_dev, label);
	else if (ret > 0)
		ret = get_label_unmounted(btrfs_dev, label);

	return ret;
}

int set_label(const char *btrfs_dev, const char *label)
{
	int ret;

	if (check_label(label))
		return -1;

	ret = is_existing_blk_or_reg_file(btrfs_dev);
	if (!ret)
		ret = set_label_mounted(btrfs_dev, label);
	else if (ret > 0)
		ret = set_label_unmounted(btrfs_dev, label);

	return ret;
}

/*
 * A not-so-good version fls64. No fascinating optimization since
 * no one except parse_size use it
 */
static int fls64(u64 x)
{
	int i;

	for (i = 0; i <64; i++)
		if (x << i & (1ULL << 63))
			return 64 - i;
	return 64 - i;
}

u64 parse_size(char *s)
{
	char c;
	char *endptr;
	u64 mult = 1;
	u64 ret;

	if (!s) {
		error("size value is empty");
		exit(1);
	}
	if (s[0] == '-') {
		error("size value '%s' is less equal than 0", s);
		exit(1);
	}
	ret = strtoull(s, &endptr, 10);
	if (endptr == s) {
		error("size value '%s' is invalid", s);
		exit(1);
	}
	if (endptr[0] && endptr[1]) {
		error("illegal suffix contains character '%c' in wrong position",
			endptr[1]);
		exit(1);
	}
	/*
	 * strtoll returns LLONG_MAX when overflow, if this happens,
	 * need to call strtoull to get the real size
	 */
	if (errno == ERANGE && ret == ULLONG_MAX) {
		error("size value '%s' is too large for u64", s);
		exit(1);
	}
	if (endptr[0]) {
		c = tolower(endptr[0]);
		switch (c) {
		case 'e':
			mult *= 1024;
			/* fallthrough */
		case 'p':
			mult *= 1024;
			/* fallthrough */
		case 't':
			mult *= 1024;
			/* fallthrough */
		case 'g':
			mult *= 1024;
			/* fallthrough */
		case 'm':
			mult *= 1024;
			/* fallthrough */
		case 'k':
			mult *= 1024;
			/* fallthrough */
		case 'b':
			break;
		default:
			error("unknown size descriptor '%c'", c);
			exit(1);
		}
	}
	/* Check whether ret * mult overflow */
	if (fls64(ret) + fls64(mult) - 1 > 64) {
		error("size value '%s' is too large for u64", s);
		exit(1);
	}
	ret *= mult;
	return ret;
}

u64 parse_qgroupid(const char *p)
{
	char *s = strchr(p, '/');
	const char *ptr_src_end = p + strlen(p);
	char *ptr_parse_end = NULL;
	u64 level;
	u64 id;
	int fd;
	int ret = 0;

	if (p[0] == '/')
		goto path;

	/* Numeric format like '0/257' is the primary case */
	if (!s) {
		id = strtoull(p, &ptr_parse_end, 10);
		if (ptr_parse_end != ptr_src_end)
			goto path;
		return id;
	}
	level = strtoull(p, &ptr_parse_end, 10);
	if (ptr_parse_end != s)
		goto path;

	id = strtoull(s + 1, &ptr_parse_end, 10);
	if (ptr_parse_end != ptr_src_end)
		goto  path;

	return (level << BTRFS_QGROUP_LEVEL_SHIFT) | id;

path:
	/* Path format like subv at 'my_subvol' is the fallback case */
	ret = test_issubvolume(p);
	if (ret < 0 || !ret)
		goto err;
	fd = open(p, O_RDONLY);
	if (fd < 0)
		goto err;
	ret = lookup_path_rootid(fd, &id);
	if (ret)
		error("failed to lookup root id: %s", strerror(-ret));
	close(fd);
	if (ret < 0)
		goto err;
	return id;

err:
	error("invalid qgroupid or subvolume path: %s", p);
	exit(-1);
}

int open_file_or_dir3(const char *fname, DIR **dirstream, int open_flags)
{
	int ret;
	struct stat st;
	int fd;

	ret = stat(fname, &st);
	if (ret < 0) {
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		*dirstream = opendir(fname);
		if (!*dirstream)
			return -1;
		fd = dirfd(*dirstream);
	} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
		fd = open(fname, open_flags);
	} else {
		/*
		 * we set this on purpose, in case the caller output
		 * strerror(errno) as success
		 */
		errno = EINVAL;
		return -1;
	}
	if (fd < 0) {
		fd = -1;
		if (*dirstream) {
			closedir(*dirstream);
			*dirstream = NULL;
		}
	}
	return fd;
}

int open_file_or_dir(const char *fname, DIR **dirstream)
{
	return open_file_or_dir3(fname, dirstream, O_RDWR);
}

void close_file_or_dir(int fd, DIR *dirstream)
{
	if (dirstream)
		closedir(dirstream);
	else if (fd >= 0)
		close(fd);
}

int get_device_info(int fd, u64 devid,
		struct btrfs_ioctl_dev_info_args *di_args)
{
	int ret;

	di_args->devid = devid;
	memset(&di_args->uuid, '\0', sizeof(di_args->uuid));

	ret = ioctl(fd, BTRFS_IOC_DEV_INFO, di_args);
	return ret < 0 ? -errno : 0;
}

static u64 find_max_device_id(struct btrfs_ioctl_search_args *search_args,
			      int nr_items)
{
	struct btrfs_dev_item *dev_item;
	char *buf = search_args->buf;

	buf += (nr_items - 1) * (sizeof(struct btrfs_ioctl_search_header)
				       + sizeof(struct btrfs_dev_item));
	buf += sizeof(struct btrfs_ioctl_search_header);

	dev_item = (struct btrfs_dev_item *)buf;

	return btrfs_stack_device_id(dev_item);
}

static int search_chunk_tree_for_fs_info(int fd,
				struct btrfs_ioctl_fs_info_args *fi_args)
{
	int ret;
	int max_items;
	u64 start_devid = 1;
	struct btrfs_ioctl_search_args search_args;
	struct btrfs_ioctl_search_key *search_key = &search_args.key;

	fi_args->num_devices = 0;

	max_items = BTRFS_SEARCH_ARGS_BUFSIZE
	       / (sizeof(struct btrfs_ioctl_search_header)
			       + sizeof(struct btrfs_dev_item));

	search_key->tree_id = BTRFS_CHUNK_TREE_OBJECTID;
	search_key->min_objectid = BTRFS_DEV_ITEMS_OBJECTID;
	search_key->max_objectid = BTRFS_DEV_ITEMS_OBJECTID;
	search_key->min_type = BTRFS_DEV_ITEM_KEY;
	search_key->max_type = BTRFS_DEV_ITEM_KEY;
	search_key->min_transid = 0;
	search_key->max_transid = (u64)-1;
	search_key->nr_items = max_items;
	search_key->max_offset = (u64)-1;

again:
	search_key->min_offset = start_devid;

	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search_args);
	if (ret < 0)
		return -errno;

	fi_args->num_devices += (u64)search_key->nr_items;

	if (search_key->nr_items == max_items) {
		start_devid = find_max_device_id(&search_args,
					search_key->nr_items) + 1;
		goto again;
	}

	/* get the lastest max_id to stay consistent with the num_devices */
	if (search_key->nr_items == 0)
		/*
		 * last tree_search returns an empty buf, use the devid of
		 * the last dev_item of the previous tree_search
		 */
		fi_args->max_id = start_devid - 1;
	else
		fi_args->max_id = find_max_device_id(&search_args,
						search_key->nr_items);

	return 0;
}

/*
 * For a given path, fill in the ioctl fs_ and info_ args.
 * If the path is a btrfs mountpoint, fill info for all devices.
 * If the path is a btrfs device, fill in only that device.
 *
 * The path provided must be either on a mounted btrfs fs,
 * or be a mounted btrfs device.
 *
 * Returns 0 on success, or a negative errno.
 */
int get_fs_info(const char *path, struct btrfs_ioctl_fs_info_args *fi_args,
		struct btrfs_ioctl_dev_info_args **di_ret)
{
	int fd = -1;
	int ret = 0;
	int ndevs = 0;
	u64 last_devid = 0;
	int replacing = 0;
	struct btrfs_fs_devices *fs_devices_mnt = NULL;
	struct btrfs_ioctl_dev_info_args *di_args;
	struct btrfs_ioctl_dev_info_args tmp;
	char mp[PATH_MAX];
	DIR *dirstream = NULL;

	memset(fi_args, 0, sizeof(*fi_args));

	if (is_block_device(path) == 1) {
		struct btrfs_super_block *disk_super;
		char buf[BTRFS_SUPER_INFO_SIZE];

		/* Ensure it's mounted, then set path to the mountpoint */
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			ret = -errno;
			error("cannot open %s: %s", path, strerror(errno));
			goto out;
		}
		ret = check_mounted_where(fd, path, mp, sizeof(mp),
					  &fs_devices_mnt);
		if (!ret) {
			ret = -EINVAL;
			goto out;
		}
		if (ret < 0)
			goto out;
		path = mp;
		/* Only fill in this one device */
		fi_args->num_devices = 1;

		disk_super = (struct btrfs_super_block *)buf;
		ret = btrfs_read_dev_super(fd, disk_super,
					   BTRFS_SUPER_INFO_OFFSET, 0);
		if (ret < 0) {
			ret = -EIO;
			goto out;
		}
		last_devid = btrfs_stack_device_id(&disk_super->dev_item);
		fi_args->max_id = last_devid;

		memcpy(fi_args->fsid, fs_devices_mnt->fsid, BTRFS_FSID_SIZE);
		close(fd);
	}

	/* at this point path must not be for a block device */
	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	/* fill in fi_args if not just a single device */
	if (fi_args->num_devices != 1) {
		ret = ioctl(fd, BTRFS_IOC_FS_INFO, fi_args);
		if (ret < 0) {
			ret = -errno;
			goto out;
		}

		/*
		 * The fs_args->num_devices does not include seed devices
		 */
		ret = search_chunk_tree_for_fs_info(fd, fi_args);
		if (ret)
			goto out;

		/*
		 * search_chunk_tree_for_fs_info() will lacks the devid 0
		 * so manual probe for it here.
		 */
		ret = get_device_info(fd, 0, &tmp);
		if (!ret) {
			fi_args->num_devices++;
			ndevs++;
			replacing = 1;
			if (last_devid == 0)
				last_devid++;
		}
	}

	if (!fi_args->num_devices)
		goto out;

	di_args = *di_ret = malloc((fi_args->num_devices) * sizeof(*di_args));
	if (!di_args) {
		ret = -errno;
		goto out;
	}

	if (replacing)
		memcpy(di_args, &tmp, sizeof(tmp));
	for (; last_devid <= fi_args->max_id; last_devid++) {
		ret = get_device_info(fd, last_devid, &di_args[ndevs]);
		if (ret == -ENODEV)
			continue;
		if (ret)
			goto out;
		ndevs++;
	}

	/*
	* only when the only dev we wanted to find is not there then
	* let any error be returned
	*/
	if (fi_args->num_devices != 1) {
		BUG_ON(ndevs == 0);
		ret = 0;
	}

out:
	close_file_or_dir(fd, dirstream);
	return ret;
}

static int group_profile_devs_min(u64 flag)
{
	switch (flag & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
	case 0: /* single */
	case BTRFS_BLOCK_GROUP_DUP:
		return 1;
	case BTRFS_BLOCK_GROUP_RAID0:
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID5:
		return 2;
	case BTRFS_BLOCK_GROUP_RAID6:
		return 3;
	case BTRFS_BLOCK_GROUP_RAID10:
		return 4;
	default:
		return -1;
	}
}

int test_num_disk_vs_raid(u64 metadata_profile, u64 data_profile,
	u64 dev_cnt, int mixed, int ssd)
{
	u64 allowed = 0;
	u64 profile = metadata_profile | data_profile;

	switch (dev_cnt) {
	default:
	case 4:
		allowed |= BTRFS_BLOCK_GROUP_RAID10;
	case 3:
		allowed |= BTRFS_BLOCK_GROUP_RAID6;
	case 2:
		allowed |= BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 |
			BTRFS_BLOCK_GROUP_RAID5;
	case 1:
		allowed |= BTRFS_BLOCK_GROUP_DUP;
	}

	if (dev_cnt > 1 && profile & BTRFS_BLOCK_GROUP_DUP) {
		warning("DUP is not recommended on filesystem with multiple devices");
	}
	if (metadata_profile & ~allowed) {
		fprintf(stderr,
			"ERROR: unable to create FS with metadata profile %s "
			"(have %llu devices but %d devices are required)\n",
			btrfs_group_profile_str(metadata_profile), dev_cnt,
			group_profile_devs_min(metadata_profile));
		return 1;
	}
	if (data_profile & ~allowed) {
		fprintf(stderr,
			"ERROR: unable to create FS with data profile %s "
			"(have %llu devices but %d devices are required)\n",
			btrfs_group_profile_str(data_profile), dev_cnt,
			group_profile_devs_min(data_profile));
		return 1;
	}

	if (dev_cnt == 3 && profile & BTRFS_BLOCK_GROUP_RAID6) {
		warning("RAID6 is not recommended on filesystem with 3 devices only");
	}
	if (dev_cnt == 2 && profile & BTRFS_BLOCK_GROUP_RAID5) {
		warning("RAID5 is not recommended on filesystem with 2 devices only");
	}
	warning_on(!mixed && (data_profile & BTRFS_BLOCK_GROUP_DUP) && ssd,
		   "DUP may not actually lead to 2 copies on the device, see manual page");

	return 0;
}

int group_profile_max_safe_loss(u64 flags)
{
	switch (flags & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
	case 0: /* single */
	case BTRFS_BLOCK_GROUP_DUP:
	case BTRFS_BLOCK_GROUP_RAID0:
		return 0;
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID5:
	case BTRFS_BLOCK_GROUP_RAID10:
		return 1;
	case BTRFS_BLOCK_GROUP_RAID6:
		return 2;
	default:
		return -1;
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

	if (blkid_get_cache(&cache, NULL) < 0) {
		error("blkid cache get failed");
		return 1;
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
			error("cannot open %s: %s", path, strerror(errno));
			continue;
		}
		ret = btrfs_scan_one_device(fd, path, &tmp_devices,
				&num_devices, BTRFS_SUPER_INFO_OFFSET,
				SBREAD_DEFAULT);
		if (ret) {
			error("cannot scan %s: %s", path, strerror(-ret));
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

/*
 * This reads a line from the stdin and only returns non-zero if the
 * first whitespace delimited token is a case insensitive match with yes
 * or y.
 */
int ask_user(const char *question)
{
	char buf[30] = {0,};
	char *saveptr = NULL;
	char *answer;

	printf("%s [y/N]: ", question);

	return fgets(buf, sizeof(buf) - 1, stdin) &&
	       (answer = strtok_r(buf, " \t\n\r", &saveptr)) &&
	       (!strcasecmp(answer, "yes") || !strcasecmp(answer, "y"));
}

/*
 * return 0 if a btrfs mount point is found
 * return 1 if a mount point is found but not btrfs
 * return <0 if something goes wrong
 */
int find_mount_root(const char *path, char **mount_root)
{
	FILE *mnttab;
	int fd;
	struct mntent *ent;
	int len;
	int ret;
	int not_btrfs = 1;
	int longest_matchlen = 0;
	char *longest_match = NULL;

	fd = open(path, O_RDONLY | O_NOATIME);
	if (fd < 0)
		return -errno;
	close(fd);

	mnttab = setmntent("/proc/self/mounts", "r");
	if (!mnttab)
		return -errno;

	while ((ent = getmntent(mnttab))) {
		len = strlen(ent->mnt_dir);
		if (strncmp(ent->mnt_dir, path, len) == 0) {
			/* match found and use the latest match */
			if (longest_matchlen <= len) {
				free(longest_match);
				longest_matchlen = len;
				longest_match = strdup(ent->mnt_dir);
				not_btrfs = strcmp(ent->mnt_type, "btrfs");
			}
		}
	}
	endmntent(mnttab);

	if (!longest_match)
		return -ENOENT;
	if (not_btrfs) {
		free(longest_match);
		return 1;
	}

	ret = 0;
	*mount_root = realpath(longest_match, NULL);
	if (!*mount_root)
		ret = -errno;

	free(longest_match);
	return ret;
}

/*
 * Test if path is a directory
 * Returns:
 *   0 - path exists but it is not a directory
 *   1 - path exists and it is a directory
 * < 0 - error
 */
int test_isdir(const char *path)
{
	struct stat st;
	int ret;

	ret = stat(path, &st);
	if (ret < 0)
		return -errno;

	return !!S_ISDIR(st.st_mode);
}

void units_set_mode(unsigned *units, unsigned mode)
{
	unsigned base = *units & UNITS_MODE_MASK;

	*units = base | mode;
}

void units_set_base(unsigned *units, unsigned base)
{
	unsigned mode = *units & ~UNITS_MODE_MASK;

	*units = base | mode;
}

int find_next_key(struct btrfs_path *path, struct btrfs_key *key)
{
	int level;

	for (level = 0; level < BTRFS_MAX_LEVEL; level++) {
		if (!path->nodes[level])
			break;
		if (path->slots[level] + 1 >=
		    btrfs_header_nritems(path->nodes[level]))
			continue;
		if (level == 0)
			btrfs_item_key_to_cpu(path->nodes[level], key,
					      path->slots[level] + 1);
		else
			btrfs_node_key_to_cpu(path->nodes[level], key,
					      path->slots[level] + 1);
		return 0;
	}
	return 1;
}

const char* btrfs_group_type_str(u64 flag)
{
	u64 mask = BTRFS_BLOCK_GROUP_TYPE_MASK |
		BTRFS_SPACE_INFO_GLOBAL_RSV;

	switch (flag & mask) {
	case BTRFS_BLOCK_GROUP_DATA:
		return "Data";
	case BTRFS_BLOCK_GROUP_SYSTEM:
		return "System";
	case BTRFS_BLOCK_GROUP_METADATA:
		return "Metadata";
	case BTRFS_BLOCK_GROUP_DATA|BTRFS_BLOCK_GROUP_METADATA:
		return "Data+Metadata";
	case BTRFS_SPACE_INFO_GLOBAL_RSV:
		return "GlobalReserve";
	default:
		return "unknown";
	}
}

const char* btrfs_group_profile_str(u64 flag)
{
	switch (flag & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
	case 0:
		return "single";
	case BTRFS_BLOCK_GROUP_RAID0:
		return "RAID0";
	case BTRFS_BLOCK_GROUP_RAID1:
		return "RAID1";
	case BTRFS_BLOCK_GROUP_RAID5:
		return "RAID5";
	case BTRFS_BLOCK_GROUP_RAID6:
		return "RAID6";
	case BTRFS_BLOCK_GROUP_DUP:
		return "DUP";
	case BTRFS_BLOCK_GROUP_RAID10:
		return "RAID10";
	default:
		return "unknown";
	}
}

u64 disk_size(const char *path)
{
	struct statfs sfs;

	if (statfs(path, &sfs) < 0)
		return 0;
	else
		return sfs.f_bsize * sfs.f_blocks;
}

u64 get_partition_size(const char *dev)
{
	u64 result;
	int fd = open(dev, O_RDONLY);

	if (fd < 0)
		return 0;
	if (ioctl(fd, BLKGETSIZE64, &result) < 0) {
		close(fd);
		return 0;
	}
	close(fd);

	return result;
}

/*
 * Check if the BTRFS_IOC_TREE_SEARCH_V2 ioctl is supported on a given
 * filesystem, opened at fd
 */
int btrfs_tree_search2_ioctl_supported(int fd)
{
	struct btrfs_ioctl_search_args_v2 *args2;
	struct btrfs_ioctl_search_key *sk;
	int args2_size = 1024;
	char args2_buf[args2_size];
	int ret;

	args2 = (struct btrfs_ioctl_search_args_v2 *)args2_buf;
	sk = &(args2->key);

	/*
	 * Search for the extent tree item in the root tree.
	 */
	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;
	sk->min_objectid = BTRFS_EXTENT_TREE_OBJECTID;
	sk->max_objectid = BTRFS_EXTENT_TREE_OBJECTID;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_type = BTRFS_ROOT_ITEM_KEY;
	sk->min_offset = 0;
	sk->max_offset = (u64)-1;
	sk->min_transid = 0;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;
	args2->buf_size = args2_size - sizeof(struct btrfs_ioctl_search_args_v2);
	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, args2);
	if (ret == -EOPNOTSUPP)
		return 0;
	else if (ret == 0)
		return 1;
	return ret;
}

int btrfs_check_nodesize(u32 nodesize, u32 sectorsize, u64 features)
{
	if (nodesize < sectorsize) {
		error("illegal nodesize %u (smaller than %u)",
				nodesize, sectorsize);
		return -1;
	} else if (nodesize > BTRFS_MAX_METADATA_BLOCKSIZE) {
		error("illegal nodesize %u (larger than %u)",
			nodesize, BTRFS_MAX_METADATA_BLOCKSIZE);
		return -1;
	} else if (nodesize & (sectorsize - 1)) {
		error("illegal nodesize %u (not aligned to %u)",
			nodesize, sectorsize);
		return -1;
	} else if (features & BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS &&
		   nodesize != sectorsize) {
		error("illegal nodesize %u (not equal to %u for mixed block group)",
			nodesize, sectorsize);
		return -1;
	}
	return 0;
}

/*
 * Copy a path argument from SRC to DEST and check the SRC length if it's at
 * most PATH_MAX and fits into DEST. DESTLEN is supposed to be exact size of
 * the buffer.
 * The destination buffer is zero terminated.
 * Return < 0 for error, 0 otherwise.
 */
int arg_copy_path(char *dest, const char *src, int destlen)
{
	size_t len = strlen(src);

	if (len >= PATH_MAX || len >= destlen)
		return -ENAMETOOLONG;

	__strncpy_null(dest, src, destlen);

	return 0;
}

unsigned int get_unit_mode_from_arg(int *argc, char *argv[], int df_mode)
{
	unsigned int unit_mode = UNITS_DEFAULT;
	int arg_i;
	int arg_end;

	for (arg_i = 0; arg_i < *argc; arg_i++) {
		if (!strcmp(argv[arg_i], "--"))
			break;

		if (!strcmp(argv[arg_i], "--raw")) {
			unit_mode = UNITS_RAW;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--human-readable")) {
			unit_mode = UNITS_HUMAN_BINARY;
			argv[arg_i] = NULL;
			continue;
		}

		if (!strcmp(argv[arg_i], "--iec")) {
			units_set_mode(&unit_mode, UNITS_BINARY);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--si")) {
			units_set_mode(&unit_mode, UNITS_DECIMAL);
			argv[arg_i] = NULL;
			continue;
		}

		if (!strcmp(argv[arg_i], "--kbytes")) {
			units_set_base(&unit_mode, UNITS_KBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--mbytes")) {
			units_set_base(&unit_mode, UNITS_MBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--gbytes")) {
			units_set_base(&unit_mode, UNITS_GBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "--tbytes")) {
			units_set_base(&unit_mode, UNITS_TBYTES);
			argv[arg_i] = NULL;
			continue;
		}

		if (!df_mode)
			continue;

		if (!strcmp(argv[arg_i], "-b")) {
			unit_mode = UNITS_RAW;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-h")) {
			unit_mode = UNITS_HUMAN_BINARY;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-H")) {
			unit_mode = UNITS_HUMAN_DECIMAL;
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-k")) {
			units_set_base(&unit_mode, UNITS_KBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-m")) {
			units_set_base(&unit_mode, UNITS_MBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-g")) {
			units_set_base(&unit_mode, UNITS_GBYTES);
			argv[arg_i] = NULL;
			continue;
		}
		if (!strcmp(argv[arg_i], "-t")) {
			units_set_base(&unit_mode, UNITS_TBYTES);
			argv[arg_i] = NULL;
			continue;
		}
	}

	for (arg_i = 0, arg_end = 0; arg_i < *argc; arg_i++) {
		if (!argv[arg_i])
			continue;
		argv[arg_end] = argv[arg_i];
		arg_end++;
	}

	*argc = arg_end;

	return unit_mode;
}

u64 div_factor(u64 num, int factor)
{
	if (factor == 10)
		return num;
	num *= factor;
	num /= 10;
	return num;
}
/*
 * Get the length of the string converted from a u64 number.
 *
 * Result is equal to log10(num) + 1, but without the use of math library.
 */
int count_digits(u64 num)
{
	int ret = 0;

	if (num == 0)
		return 1;
	while (num > 0) {
		ret++;
		num /= 10;
	}
	return ret;
}

int string_is_numerical(const char *str)
{
	if (!str)
		return 0;
	if (!(*str >= '0' && *str <= '9'))
		return 0;
	while (*str >= '0' && *str <= '9')
		str++;
	if (*str != '\0')
		return 0;
	return 1;
}

int prefixcmp(const char *str, const char *prefix)
{
	for (; ; str++, prefix++)
		if (!*prefix)
			return 0;
		else if (*str != *prefix)
			return (unsigned char)*prefix - (unsigned char)*str;
}

/* Subvolume helper functions */
/*
 * test if name is a correct subvolume name
 * this function return
 * 0-> name is not a correct subvolume name
 * 1-> name is a correct subvolume name
 */
int test_issubvolname(const char *name)
{
	return name[0] != '\0' && !strchr(name, '/') &&
		strcmp(name, ".") && strcmp(name, "..");
}

/*
 * Test if path is a subvolume
 * Returns:
 *   0 - path exists but it is not a subvolume
 *   1 - path exists and it is  a subvolume
 * < 0 - error
 */
int test_issubvolume(const char *path)
{
	struct stat	st;
	struct statfs stfs;
	int		res;

	res = stat(path, &st);
	if (res < 0)
		return -errno;

	if (st.st_ino != BTRFS_FIRST_FREE_OBJECTID || !S_ISDIR(st.st_mode))
		return 0;

	res = statfs(path, &stfs);
	if (res < 0)
		return -errno;

	return (int)stfs.f_type == BTRFS_SUPER_MAGIC;
}

const char *subvol_strip_mountpoint(const char *mnt, const char *full_path)
{
	int len = strlen(mnt);
	if (!len)
		return full_path;

	if (mnt[len - 1] != '/')
		len += 1;

	return full_path + len;
}

/*
 * Returns
 * <0: Std error
 * 0: All fine
 * 1: Error; and error info printed to the terminal. Fixme.
 * 2: If the fullpath is root tree instead of subvol tree
 */
int get_subvol_info(const char *fullpath, struct root_info *get_ri)
{
	u64 sv_id;
	int ret = 1;
	int fd = -1;
	int mntfd = -1;
	char *mnt = NULL;
	const char *svpath = NULL;
	DIR *dirstream1 = NULL;
	DIR *dirstream2 = NULL;

	ret = test_issubvolume(fullpath);
	if (ret < 0)
		return ret;
	if (!ret) {
		error("not a subvolume: %s", fullpath);
		return 1;
	}

	ret = find_mount_root(fullpath, &mnt);
	if (ret < 0)
		return ret;
	if (ret > 0) {
		error("%s doesn't belong to btrfs mount point", fullpath);
		return 1;
	}
	ret = 1;
	svpath = subvol_strip_mountpoint(mnt, fullpath);

	fd = btrfs_open_dir(fullpath, &dirstream1, 1);
	if (fd < 0)
		goto out;

	ret = btrfs_list_get_path_rootid(fd, &sv_id);
	if (ret)
		goto out;

	mntfd = btrfs_open_dir(mnt, &dirstream2, 1);
	if (mntfd < 0)
		goto out;

	memset(get_ri, 0, sizeof(*get_ri));
	get_ri->root_id = sv_id;

	if (sv_id == BTRFS_FS_TREE_OBJECTID)
		ret = btrfs_get_toplevel_subvol(mntfd, get_ri);
	else
		ret = btrfs_get_subvol(mntfd, get_ri);
	if (ret)
		error("can't find '%s': %d", svpath, ret);

out:
	close_file_or_dir(mntfd, dirstream2);
	close_file_or_dir(fd, dirstream1);
	free(mnt);

	return ret;
}

/* Set the seed manually */
void init_rand_seed(u64 seed)
{
	int i;

	/* only use the last 48 bits */
	for (i = 0; i < 3; i++) {
		rand_seed[i] = (unsigned short)(seed ^ (unsigned short)(-1));
		seed >>= 16;
	}
	rand_seed_initlized = 1;
}

static void __init_seed(void)
{
	struct timeval tv;
	int ret;
	int fd;

	if(rand_seed_initlized)
		return;
	/* Use urandom as primary seed source. */
	fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		ret = read(fd, rand_seed, sizeof(rand_seed));
		close(fd);
		if (ret < sizeof(rand_seed))
			goto fallback;
	} else {
fallback:
		/* Use time and pid as fallback seed */
		warning("failed to read /dev/urandom, use time and pid as random seed");
		gettimeofday(&tv, 0);
		rand_seed[0] = getpid() ^ (tv.tv_sec & 0xFFFF);
		rand_seed[1] = getppid() ^ (tv.tv_usec & 0xFFFF);
		rand_seed[2] = (tv.tv_sec ^ tv.tv_usec) >> 16;
	}
	rand_seed_initlized = 1;
}

u32 rand_u32(void)
{
	__init_seed();
	/*
	 * Don't use nrand48, its range is [0,2^31) The highest bit will alwasy
	 * be 0.  Use jrand48 to include the highest bit.
	 */
	return (u32)jrand48(rand_seed);
}

/* Return random number in range [0, upper) */
unsigned int rand_range(unsigned int upper)
{
	__init_seed();
	/*
	 * Use the full 48bits to mod, which would be more uniformly
	 * distributed
	 */
	return (unsigned int)(jrand48(rand_seed) % upper);
}

int rand_int(void)
{
	return (int)(rand_u32());
}

u64 rand_u64(void)
{
	u64 ret = 0;

	ret += rand_u32();
	ret <<= 32;
	ret += rand_u32();
	return ret;
}

u16 rand_u16(void)
{
	return (u16)(rand_u32());
}

u8 rand_u8(void)
{
	return (u8)(rand_u32());
}

void btrfs_config_init(void)
{
}
