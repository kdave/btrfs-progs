/* SPDX-License-Identifier: GPL-2.0 */
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

#ifndef __BTRFS_ZONED_H__
#define __BTRFS_ZONED_H__

#include "kerncompat.h"
#include <stdbool.h>
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"

#ifdef BTRFS_ZONED
#include <linux/blkzoned.h>
#else

struct blk_zone {
	int dummy;
};

#endif /* BTRFS_ZONED */

/* Number of superblock log zones */
#define BTRFS_NR_SB_LOG_ZONES		2

/*
 * Location of the first zone of superblock logging zone pairs.
 *
 * - primary superblock:    0B (zone 0)
 * - first copy:          512G (zone starting at that offset)
 * - second copy:           4T (zone starting at that offset)
 */
#define BTRFS_SB_LOG_PRIMARY_OFFSET	(0ULL)
#define BTRFS_SB_LOG_FIRST_OFFSET	(512ULL * SZ_1G)
#define BTRFS_SB_LOG_SECOND_OFFSET	(4096ULL * SZ_1G)

#define BTRFS_SB_LOG_FIRST_SHIFT	const_ilog2(BTRFS_SB_LOG_FIRST_OFFSET)
#define BTRFS_SB_LOG_SECOND_SHIFT	const_ilog2(BTRFS_SB_LOG_SECOND_OFFSET)

/*
 * Zoned block device models
 */
enum btrfs_zoned_model {
	ZONED_NONE,
	ZONED_HOST_AWARE,
	ZONED_HOST_MANAGED,
};

/*
 * Zone information for a zoned block device.
 */
struct btrfs_zoned_device_info {
	enum btrfs_zoned_model	model;
	u64			zone_size;
	u32			nr_zones;
	struct blk_zone		*zones;
	bool			emulated;
};

enum btrfs_zoned_model zoned_model(const char *file);
u64 zone_size(const char *file);
int btrfs_get_zone_info(int fd, const char *file,
			struct btrfs_zoned_device_info **zinfo);
int btrfs_get_dev_zone_info_all_devices(struct btrfs_fs_info *fs_info);
int btrfs_check_zoned_mode(struct btrfs_fs_info *fs_info);

#ifdef BTRFS_ZONED
size_t btrfs_sb_io(int fd, void *buf, off_t offset, int rw);

/*
 * Read BTRFS_SUPER_INFO_SIZE bytes from fd to buf
 *
 * @fd		fd of the device to be read from
 * @buf:	buffer contains a super block
 * @offset:	offset of the superblock
 *
 * Return count of bytes successfully read.
 */
static inline size_t sbread(int fd, void *buf, off_t offset)
{
	return btrfs_sb_io(fd, buf, offset, READ);
}

/*
 * Write BTRFS_SUPER_INFO_SIZE bytes from buf to fd
 *
 * @fd		fd of the device to be written to
 * @buf:	buffer contains a super block
 * @offset:	offset of the superblock
 *
 * Return count of bytes successfully written.
 */
static inline size_t sbwrite(int fd, void *buf, off_t offset)
{
	return btrfs_sb_io(fd, buf, offset, WRITE);
}

static inline bool zone_is_sequential(struct btrfs_zoned_device_info *zinfo,
				      u64 bytenr)
{
	unsigned int zno;

	if (!zinfo || zinfo->model == ZONED_NONE)
		return false;

	zno = bytenr / zinfo->zone_size;
	return zinfo->zones[zno].type == BLK_ZONE_TYPE_SEQWRITE_REQ;
}

static inline bool btrfs_dev_is_empty_zone(struct btrfs_device *device, u64 pos)
{
	struct btrfs_zoned_device_info *zinfo = device->zone_info;
	unsigned int zno;

	if (!zone_is_sequential(zinfo, pos))
		return true;

	zno = pos / zinfo->zone_size;
	return zinfo->zones[zno].cond == BLK_ZONE_COND_EMPTY;
}

bool zoned_profile_supported(u64 map_type);
int btrfs_reset_dev_zone(int fd, struct blk_zone *zone);
u64 btrfs_find_allocatable_zones(struct btrfs_device *device, u64 hole_start,
				 u64 hole_end, u64 num_bytes);
int btrfs_load_block_group_zone_info(struct btrfs_fs_info *fs_info,
				     struct btrfs_block_group *cache);
bool btrfs_redirty_extent_buffer_for_zoned(struct btrfs_fs_info *fs_info,
					   u64 start, u64 end);
int btrfs_reset_chunk_zones(struct btrfs_fs_info *fs_info, u64 devid,
			    u64 offset, u64 length);
int btrfs_reset_all_zones(int fd, struct btrfs_zoned_device_info *zinfo);
int zero_zone_blocks(int fd, struct btrfs_zoned_device_info *zinfo, off_t start,
		     size_t len);
int btrfs_wipe_temporary_sb(struct btrfs_fs_devices *fs_devices);

#else

#define sbread(fd, buf, offset) \
	pread64(fd, buf, BTRFS_SUPER_INFO_SIZE, offset)
#define sbwrite(fd, buf, offset) \
	pwrite64(fd, buf, BTRFS_SUPER_INFO_SIZE, offset)

static inline int btrfs_reset_dev_zone(int fd, struct blk_zone *zone)
{
	return 0;
}

static inline bool zone_is_sequential(struct btrfs_zoned_device_info *zinfo,
				      u64 bytenr)
{
	return false;
}

static inline u64 btrfs_find_allocatable_zones(struct btrfs_device *device,
					       u64 hole_start, u64 hole_end,
					       u64 num_bytes)
{
	return hole_start;
}

static inline bool btrfs_dev_is_empty_zone(struct btrfs_device *device, u64 pos)
{
	return true;
}

static inline int btrfs_load_block_group_zone_info(
	struct btrfs_fs_info *fs_info, struct btrfs_block_group *cache)
{
	return 0;
}

static inline bool btrfs_redirty_extent_buffer_for_zoned(
	struct btrfs_fs_info *fs_info, u64 start, u64 end)
{
	return false;
}

static inline int btrfs_reset_chunk_zones(struct btrfs_fs_info *fs_info,
					  u64 devid, u64 offset, u64 length)
{
	return 0;
}

static inline int btrfs_reset_all_zones(int fd,
					struct btrfs_zoned_device_info *zinfo)
{
	return -EOPNOTSUPP;
}

static inline int zero_zone_blocks(int fd,
				   struct btrfs_zoned_device_info *zinfo,
				   off_t start, size_t len)
{
	return -EOPNOTSUPP;
}

static inline int btrfs_wipe_temporary_sb(struct btrfs_fs_devices *fs_devices)
{
	return 0;
}

static inline bool zoned_profile_supported(u64 map_type)
{
	return false;
}

#endif /* BTRFS_ZONED */

/*
 * Get the first zone number of the superblock mirror
 */
static inline u32 sb_zone_number(int shift, int mirror)
{
	u64 zone = 0;

	ASSERT(0 <= mirror && mirror < BTRFS_SUPER_MIRROR_MAX);
	switch (mirror) {
	case 0: zone = 0; break;
	case 1: zone = 1ULL << (BTRFS_SB_LOG_FIRST_SHIFT - shift); break;
	case 2: zone = 1ULL << (BTRFS_SB_LOG_SECOND_SHIFT - shift); break;
	}

	ASSERT(zone <= U32_MAX);

	return (u32)zone;
}

static inline bool btrfs_dev_is_sequential(struct btrfs_device *device, u64 pos)
{
	return zone_is_sequential(device->zone_info, pos);
}

#endif /* __BTRFS_ZONED_H__ */
