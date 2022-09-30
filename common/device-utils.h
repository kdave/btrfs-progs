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

#ifndef __DEVICE_UTILS_H__
#define __DEVICE_UTILS_H__

#include "kerncompat.h"
#include <sys/stat.h>
#include <stdbool.h>
#include <unistd.h>

/*
 * Options for btrfs_prepare_device
 */
#define	PREP_DEVICE_ZERO_END	(1U << 0)
#define	PREP_DEVICE_DISCARD	(1U << 1)
#define	PREP_DEVICE_VERBOSE	(1U << 2)
#define	PREP_DEVICE_ZONED	(1U << 3)

/* Placeholder to denote no results for the zone_unusable sysfs value */
#define DEVICE_ZONE_UNUSABLE_UNKNOWN		((u64)-1)

/*
 * Generic block device helpers
 */
int device_discard_blocks(int fd, u64 start, u64 len);
int device_zero_blocks(int fd, off_t start, size_t len, const bool direct);
u64 device_get_partition_size(const char *dev);
u64 device_get_partition_size_fd(int fd);
u64 device_get_partition_size_fd_stat(int fd, const struct stat *st);
int device_get_queue_param(const char *file, const char *param, char *buf, size_t len);
u64 device_get_zone_unusable(int fd, u64 flags);
u64 device_get_zone_size(int fd, const char *name);
int device_get_rotational(const char *file);
/*
 * Updates to devices with btrfs-specific changs
 */
int btrfs_prepare_device(int fd, const char *file, u64 *block_count_ret,
		u64 max_block_count, unsigned opflags);
ssize_t btrfs_direct_pio(int rw, int fd, void *buf, size_t count, off_t offset);

#ifdef BTRFS_ZONED
static inline ssize_t btrfs_pwrite(int fd, void *buf, size_t count,
				   off_t offset, bool direct)
{
	if (!direct)
		return pwrite(fd, buf, count, offset);

	return btrfs_direct_pio(WRITE, fd, buf, count, offset);
}
static inline ssize_t btrfs_pread(int fd, void *buf, size_t count, off_t offset,
				  bool direct)
{
	if (!direct)
		return pread(fd, buf, count, offset);

	return btrfs_direct_pio(READ, fd, buf, count, offset);
}
#else
#define btrfs_pwrite(fd, buf, count, offset, direct) \
	({ (void)(direct); pwrite(fd, buf, count, offset); })
#define btrfs_pread(fd, buf, count, offset, direct) \
	({ (void)(direct); pread(fd, buf, count, offset); })
#endif

#endif
