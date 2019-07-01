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
#include <sys/mount.h>
#include <sys/statfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <blkid/blkid.h>
#include "kernel-lib/sizes.h"
#include "disk-io.h"
#include "common/device-utils.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/utils.h"

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
		error("cannot wipe existing superblock: %m");
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
		error("unable to stat %s: %m", file);
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
		errno = -ret;
		error("failed to zero device '%s': %m", file);
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

