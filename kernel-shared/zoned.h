/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __BTRFS_ZONED_H__
#define __BTRFS_ZONED_H__

#include "kerncompat.h"
#include <stdbool.h>
#include "kernel-shared/disk-io.h"

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
	u64		        max_zone_append_size;
	u32			nr_zones;
	struct blk_zone		*zones;
};

enum btrfs_zoned_model zoned_model(const char *file);
u64 zone_size(const char *file);
int btrfs_get_zone_info(int fd, const char *file,
			struct btrfs_zoned_device_info **zinfo);
int btrfs_get_dev_zone_info_all_devices(struct btrfs_fs_info *fs_info);
int btrfs_check_zoned_mode(struct btrfs_fs_info *fs_info);

#ifdef BTRFS_ZONED
size_t btrfs_sb_io(int fd, void *buf, off_t offset, int rw);

static inline size_t sbread(int fd, void *buf, off_t offset)
{
	return btrfs_sb_io(fd, buf, offset, READ);
}

static inline size_t sbwrite(int fd, void *buf, off_t offset)
{
	return btrfs_sb_io(fd, buf, offset, WRITE);
}

int btrfs_reset_dev_zone(int fd, struct blk_zone *zone);

#else

#define sbread(fd, buf, offset) \
	pread64(fd, buf, BTRFS_SUPER_INFO_SIZE, offset)
#define sbwrite(fd, buf, offset) \
	pwrite64(fd, buf, BTRFS_SUPER_INFO_SIZE, offset)

static inline int btrfs_reset_dev_zone(int fd, struct blk_zone *zone)
{
	return 0;
}

#endif /* BTRFS_ZONED */

#endif /* __BTRFS_ZONED_H__ */
