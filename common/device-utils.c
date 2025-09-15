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

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/limits.h>
#ifdef BTRFS_ZONED
#include <linux/blkzoned.h>
#endif
#include <linux/fs.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <blkid/blkid.h>
#include "kernel-lib/sizes.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/zoned.h"
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "common/device-utils.h"
#include "common/sysfs-utils.h"
#include "common/path-utils.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/units.h"

#ifndef BLKDISCARD
#define BLKDISCARD	_IO(0x12,119)
#endif

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
int device_discard_blocks(int fd, u64 start, u64 len)
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

static void prepare_discard_device(const char *filename, int fd, u64 byte_count, unsigned opflags)
{
	u64 cur = 0;

	while (cur < byte_count) {
		/* 1G granularity */
		u64 chunk_size = (cur == 0) ? SZ_1M : min_t(u64, byte_count - cur, SZ_1G);
		int ret;

		ret = discard_range(fd, cur, chunk_size);
		if (ret)
			return;
		/*
		 * The first range discarded successfully, meaning the device supports
		 * discard.
		 */
		if (opflags & PREP_DEVICE_VERBOSE && cur == 0)
			printf("Performing full device TRIM %s (%s) ...\n",
			       filename, pretty_size(byte_count));
		cur += chunk_size;
	}
}

/*
 * Write zeros to the given range [start, start + len)
 */
int device_zero_blocks(int fd, off_t start, size_t len, bool direct)
{
	char *buf = malloc(len);
	int ret = 0;
	ssize_t written;

	if (!buf)
		return -ENOMEM;
	memset(buf, 0, len);
	written = btrfs_pwrite(fd, buf, len, start, direct);
	if (written != len) {
		error_msg(ERROR_MSG_WRITE, "zeroing range from %llu: %m",
			  (unsigned long long)start);
		ret = -EIO;
	}
	free(buf);
	return ret;
}

#define ZERO_DEV_BYTES SZ_2M

/*
 * Zero blocks in the range from start but not after the given device size.
 * (On SPARC the disk labels are preserved too.)
 */
static int zero_dev_clamped(int fd, struct btrfs_zoned_device_info *zinfo,
			    off_t start, ssize_t len, u64 dev_size)
{
	off_t end = max(start, start + len);

#ifdef __sparc__
	/* and don't overwrite the disk labels on sparc */
	start = max(start, 1024);
	end = max(end, 1024);
#endif

	start = min_t(u64, start, dev_size);
	end = min_t(u64, end, dev_size);

	if (zinfo && zinfo->model == ZONED_HOST_MANAGED)
		return zero_zone_blocks(fd, zinfo, start, end - start);

	return device_zero_blocks(fd, start, end - start, false);
}

/*
 * Find all magic signatures known to blkid and remove them
 */
static int btrfs_wipe_existing_sb(int fd, struct btrfs_zoned_device_info *zinfo)
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

	if (!zone_is_sequential(zinfo, offset)) {
		const bool direct = zinfo && zinfo->model == ZONED_HOST_MANAGED;

		memset(buf, 0, len);
		ret = btrfs_pwrite(fd, buf, len, offset, direct);
		if (ret < 0) {
			error("cannot wipe existing superblock: %m");
			ret = -1;
		} else if (ret != len) {
			error("cannot wipe existing superblock: wrote %d of %zd",
			      ret, len);
			ret = -1;
		}
	} else {
		struct blk_zone *zone = &zinfo->zones[offset / zinfo->zone_size];

		ret = btrfs_reset_dev_zone(fd, zone);
		if (ret < 0) {
			error(
		"zoned: failed to wipe zones containing superblock: %m");
			ret = -1;
		}
	}
	fsync(fd);

out:
	blkid_free_probe(pr);
	return ret;
}

/*
 * Prepare a device before it's added to the filesystem. Optionally:
 * - remove old superblocks
 * - discard
 * - reset zones
 * - delete end of the device
 */
int btrfs_prepare_device(int fd, const char *file, u64 *byte_count_ret,
			 u64 max_byte_count, unsigned opflags)
{
	struct btrfs_zoned_device_info *zinfo = NULL;
	u64 byte_count;
	struct stat st;
	int i, ret;

	ret = fstat(fd, &st);
	if (ret < 0) {
		error("unable to stat %s: %m", file);
		return 1;
	}

	ret = device_get_partition_size_fd_stat(fd, &st, &byte_count);
	if (ret < 0) {
		errno = -ret;
		error("unable to determine size of %s: %m", file);
		return 1;
	}
	if (max_byte_count)
		byte_count = min(byte_count, max_byte_count);

	if (opflags & PREP_DEVICE_ZONED) {
		ret = btrfs_get_zone_info(fd, file, &zinfo);
		if (ret < 0 || !zinfo) {
			error("zoned: unable to load zone information of %s",
			      file);
			return 1;
		}

		if (!zinfo->emulated) {
			if (opflags & PREP_DEVICE_VERBOSE)
				printf("Resetting device zones %s (%llu zones) ...\n",
				       file, byte_count / zinfo->zone_size);
			/*
			 * We cannot ignore zone reset errors for a zoned block
			 * device as this could result in the inability to write
			 * to non-empty sequential zones of the device.
			 */
			ret = btrfs_reset_zones(fd, zinfo, byte_count);
			if (ret) {
				if (ret == EBUSY) {
					error("zoned: device '%s' contains an active zone outside of fs range", file);
					error("zoned: btrfs needs full control of active zones");
				} else {
					error("zoned: failed to reset device '%s' zones: %m", file);
				}
				goto err;
			}
		}
	}

	ret = zero_dev_clamped(fd, zinfo, 0, ZERO_DEV_BYTES, byte_count);
	for (i = 0 ; !ret && i < BTRFS_SUPER_MIRROR_MAX; i++)
		ret = zero_dev_clamped(fd, zinfo, btrfs_sb_offset(i),
				       BTRFS_SUPER_INFO_SIZE, byte_count);
	if (!ret && (opflags & PREP_DEVICE_ZERO_END))
		ret = zero_dev_clamped(fd, zinfo, byte_count - ZERO_DEV_BYTES,
				       ZERO_DEV_BYTES, byte_count);

	if (ret < 0) {
		errno = -ret;
		error("failed to zero device '%s': %m", file);
		goto err;
	}

	if (!(opflags & PREP_DEVICE_ZONED) && (opflags & PREP_DEVICE_DISCARD))
		prepare_discard_device(file, fd, byte_count, opflags);

	ret = btrfs_wipe_existing_sb(fd, zinfo);
	if (ret < 0) {
		error("cannot wipe superblocks on %s", file);
		goto err;
	}

	free(zinfo);
	*byte_count_ret = byte_count;
	return 0;

err:
	free(zinfo);
	return 1;
}

int device_get_partition_size_fd_stat(int fd, const struct stat *st, u64 *size_ret)
{
	if (S_ISREG(st->st_mode)) {
		*size_ret = st->st_size;
		return 0;
	}
	if (!S_ISBLK(st->st_mode))
		return -EINVAL;
	if (ioctl(fd, BLKGETSIZE64, size_ret) < 0)
		return -errno;
	return 0;
}

static int device_get_partition_size_sysfs(const char *dev, u64 *size_ret)
{
	int ret;
	char path[PATH_MAX] = {};
	char sysfs[PATH_MAX] = {};
	char sizebuf[128] = {};
	const char *name = NULL;
	int sysfd;
	unsigned long long size = 0;

	name = realpath(dev, path);
	if (!name)
		return -errno;
	name = path_basename(path);

	ret = path_cat3_out(sysfs, "/sys/class/block", name, "size");
	if (ret < 0)
		return ret;
	sysfd = open(sysfs, O_RDONLY);
	if (sysfd < 0)
		return -errno;
	ret = sysfs_read_file(sysfd, sizebuf, sizeof(sizebuf));
	close(sysfd);
	if (ret < 0)
		return ret;
	errno = 0;
	size = strtoull(sizebuf, NULL, 10);
	if (size == ULLONG_MAX && errno == ERANGE)
		return -ERANGE;
	/* Extra overflow check. */
	if (size > ULLONG_MAX >> SECTOR_SHIFT)
		return -ERANGE;
	*size_ret = size << SECTOR_SHIFT;
	return 0;
}

int device_get_partition_size(const char *dev, u64 *size_ret)
{
	u64 result;
	int fd = open(dev, O_RDONLY);

	if (fd < 0)
		return device_get_partition_size_sysfs(dev, size_ret);

	if (ioctl(fd, BLKGETSIZE64, &result) < 0) {
		close(fd);
		return -errno;
	}
	close(fd);
	*size_ret = result;
	return 0;
}

/*
 * Get a device request queue parameter from sysfs.
 */
int device_get_queue_param(const char *file, const char *param, char *buf, size_t len)
{
	blkid_probe probe;
	char wholedisk[PATH_MAX];
	char sysfs_path[PATH_MAX];
	dev_t devno;
	int fd;
	int ret;

	probe = blkid_new_probe_from_filename(file);
	if (!probe)
		return 0;

	/* Device number of this disk (possibly a partition) */
	devno = blkid_probe_get_devno(probe);
	if (!devno) {
		blkid_free_probe(probe);
		return 0;
	}

	/* Get whole disk name (not full path) for this devno */
	ret = blkid_devno_to_wholedisk(devno, wholedisk, sizeof(wholedisk), NULL);
	if (ret) {
		blkid_free_probe(probe);
		return 0;
	}

	snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/queue/%s",
		 wholedisk, param);

	blkid_free_probe(probe);

	fd = open(sysfs_path, O_RDONLY);
	if (fd < 0)
		return 0;

	len = read(fd, buf, len);
	close(fd);

	return len;
}

/*
 * Read value of zone_unusable from sysfs for given block group type in flags
 */
u64 device_get_zone_unusable(int fd, u64 flags)
{
	int ret;
	u64 unusable;

	/* Don't report it for a regular fs */
	ret = sysfs_open_fsid_file(fd, "features/zoned");
	if (ret < 0)
		return DEVICE_ZONE_UNUSABLE_UNKNOWN;
	close(ret);
	ret = -1;

	if ((flags & BTRFS_BLOCK_GROUP_DATA) == BTRFS_BLOCK_GROUP_DATA)
		ret = sysfs_read_fsid_file_u64(fd, "allocation/data/bytes_zone_unusable", &unusable);
	else if ((flags & BTRFS_BLOCK_GROUP_METADATA) == BTRFS_BLOCK_GROUP_METADATA)
		ret = sysfs_read_fsid_file_u64(fd, "allocation/metadata/bytes_zone_unusable", &unusable);
	else if ((flags & BTRFS_BLOCK_GROUP_SYSTEM) == BTRFS_BLOCK_GROUP_SYSTEM)
		ret = sysfs_read_fsid_file_u64(fd, "allocation/system/bytes_zone_unusable", &unusable);

	if (ret < 0)
		return DEVICE_ZONE_UNUSABLE_UNKNOWN;

	return unusable;
}

/*
 * Read information about zone size of the given device (short @name) from a
 * given filesystem fd
 */
u64 device_get_zone_size(int fd, const char *name)
{
	DIR *dir;
	struct dirent *de;
	int sysfs_fd;
	u64 ret = 0;

	sysfs_fd = sysfs_open_fsid_dir(fd, "devices");
	if (sysfs_fd < 0)
		return 0;

	dir = fdopendir(sysfs_fd);
	if (!dir) {
		ret = 0;
		goto out;
	}
	while (1) {
		int queue_fd;
		char queue[PATH_MAX];
		char buf[128] = {0};

		de = readdir(dir);
		if (!de) {
			ret = 0;
			break;
		}

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		if (strcmp(name, de->d_name) != 0)
			continue;

		path_cat3_out(queue, "devices", de->d_name, "queue/chunk_sectors");
		/* /sys/fs/btrfs/FSID/devices/NAME/queue/chunk_sectors */
		queue_fd = sysfs_open_fsid_file(fd, queue);
		if (queue_fd < 0) {
			queue_fd = -1;
			ret = 0;
			break;
		}
		sysfs_read_file(queue_fd, buf, sizeof(buf));
		ret = atoll(buf);
		close(queue_fd);
		break;
	}
	closedir(dir);

out:
	close(sysfs_fd);
	return ret;
}

int device_get_rotational(const char *file)
{
	char rotational;
	int ret;

	ret = device_get_queue_param(file, "rotational", &rotational, 1);
	if (ret < 1)
		return 0;

	return (rotational == '0');
}

int device_get_info(int fd, u64 devid, struct btrfs_ioctl_dev_info_args *di_args)
{
	int ret;

	di_args->devid = devid;
	memset(&di_args->uuid, 0, sizeof(di_args->uuid));
	ret = ioctl(fd, BTRFS_IOC_DEV_INFO, di_args);

	return ret < 0 ? -errno : 0;
}

ssize_t btrfs_direct_pread(int fd, void *buf, size_t count, off_t offset)
{
	int alignment;
	size_t iosize;
	void *bounce_buf = NULL;
	struct stat stat_buf;
	unsigned long req;
	int ret;

	if (fstat(fd, &stat_buf) == -1) {
		error("fstat failed: %m");
		return -errno;
	}

	if ((stat_buf.st_mode & S_IFMT) == S_IFBLK)
		req = BLKSSZGET;
	else
		req = FIGETBSZ;

	if (ioctl(fd, req, &alignment)) {
		error("failed to get block size: %m");
		return -errno;
	}

	if (IS_ALIGNED((size_t)buf, alignment) && IS_ALIGNED(count, alignment))
		return pread(fd, buf, count, offset);

	iosize = round_up(count, alignment);

	ret = posix_memalign(&bounce_buf, alignment, iosize);
	if (ret) {
		error_mem("bounce buffer");
		errno = ret;
		return -ret;
	}

	ret = pread(fd, bounce_buf, iosize, offset);
	if (ret >= count)
		ret = count;
	memcpy(buf, bounce_buf, count);

	free(bounce_buf);
	return ret;
}

ssize_t btrfs_direct_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	int alignment;
	size_t iosize;
	void *bounce_buf = NULL;
	struct stat stat_buf;
	unsigned long req;
	int ret;

	if (fstat(fd, &stat_buf) == -1) {
		error("fstat failed: %m");
		return -errno;
	}

	if ((stat_buf.st_mode & S_IFMT) == S_IFBLK)
		req = BLKSSZGET;
	else
		req = FIGETBSZ;

	if (ioctl(fd, req, &alignment)) {
		error("failed to get block size: %m");
		return -errno;
	}

	if (IS_ALIGNED((size_t)buf, alignment) && IS_ALIGNED(count, alignment))
		return pwrite(fd, buf, count, offset);

	/* Cannot do anything if the write size is not aligned */
	if (!IS_ALIGNED(count, alignment)) {
		error("%zu is not aligned to %d", count, alignment);
		return -EINVAL;
	}

	iosize = round_up(count, alignment);

	ret = posix_memalign(&bounce_buf, alignment, iosize);
	if (ret) {
		error_mem("bounce buffer");
		errno = ret;
		return -ret;
	}

	UASSERT(iosize == count);
	memcpy(bounce_buf, buf, count);
	ret = pwrite(fd, bounce_buf, iosize, offset);

	free(bounce_buf);
	return ret;
}

/* Sort devices by devid, ascending */
int cmp_device_id(void *priv, struct list_head *a, struct list_head *b)
{
	const struct btrfs_device *da = list_entry(a, struct btrfs_device,
			dev_list);
	const struct btrfs_device *db = list_entry(b, struct btrfs_device,
			dev_list);

	return da->devid < db->devid ? -1 :
		da->devid > db->devid ? 1 : 0;
}
