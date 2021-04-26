/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __BTRFS_ZONED_H__
#define __BTRFS_ZONED_H__

#include "kerncompat.h"
#include <stdbool.h>

#ifdef BTRFS_ZONED
#include <linux/blkzoned.h>
#else

struct blk_zone {
	int dummy;
};

#endif /* BTRFS_ZONED */

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
};

enum btrfs_zoned_model zoned_model(const char *file);
u64 zone_size(const char *file);
int btrfs_get_zone_info(int fd, const char *file,
			struct btrfs_zoned_device_info **zinfo);
int btrfs_get_dev_zone_info_all_devices(struct btrfs_fs_info *fs_info);

#endif /* __BTRFS_ZONED_H__ */
