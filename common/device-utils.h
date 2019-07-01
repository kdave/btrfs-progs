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
#include "sys/stat.h"

#define	PREP_DEVICE_ZERO_END	(1U << 0)
#define	PREP_DEVICE_DISCARD	(1U << 1)
#define	PREP_DEVICE_VERBOSE	(1U << 2)

u64 get_partition_size(const char *dev);
u64 disk_size(const char *path);
u64 btrfs_device_size(int fd, struct stat *st);
int btrfs_prepare_device(int fd, const char *file, u64 *block_count_ret,
		u64 max_block_count, unsigned opflags);

#endif
