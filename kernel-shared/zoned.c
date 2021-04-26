// SPDX-License-Identifier: GPL-2.0

#include <sys/ioctl.h>
#include <linux/fs.h>

#include "kernel-lib/list.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/zoned.h"
#include "common/utils.h"
#include "common/device-utils.h"
#include "common/messages.h"
#include "mkfs/common.h"

/* Maximum number of zones to report per ioctl(BLKREPORTZONE) call */
#define BTRFS_REPORT_NR_ZONES  		4096

static int btrfs_get_dev_zone_info(struct btrfs_device *device);

enum btrfs_zoned_model zoned_model(const char *file)
{
	const char host_aware[] = "host-aware";
	const char host_managed[] = "host-managed";
	struct stat st;
	char model[32];
	int ret;

	ret = stat(file, &st);
	if (ret < 0) {
		error("zoned: unable to stat %s", file);
		return -ENOENT;
	}

	/* Consider a regular file as non-zoned device */
	if (!S_ISBLK(st.st_mode))
		return ZONED_NONE;

	ret = queue_param(file, "zoned", model, sizeof(model));
	if (ret <= 0)
		return ZONED_NONE;

	if (strncmp(model, host_aware, strlen(host_aware)) == 0)
		return ZONED_HOST_AWARE;
	if (strncmp(model, host_managed, strlen(host_managed)) == 0)
		return ZONED_HOST_MANAGED;

	return ZONED_NONE;
}

u64 zone_size(const char *file)
{
	char chunk[32];
	int ret;

	ret = queue_param(file, "chunk_sectors", chunk, sizeof(chunk));
	if (ret <= 0)
		return 0;

	return strtoull((const char *)chunk, NULL, 10) << SECTOR_SHIFT;
}

#ifdef BTRFS_ZONED
static int report_zones(int fd, const char *file,
			struct btrfs_zoned_device_info *zinfo)
{
	u64 device_size;
	u64 zone_bytes = zone_size(file);
	size_t rep_size;
	u64 sector = 0;
	struct blk_zone_report *rep;
	struct blk_zone *zone;
	unsigned int i, n = 0;
	int ret;

	/*
	 * Zones are guaranteed (by kernel) to be a power of 2 number of
	 * sectors. Check this here and make sure that zones are not too small.
	 */
	if (!zone_bytes || !is_power_of_2(zone_bytes)) {
		error("zoned: illegal zone size %llu (not a power of 2)",
		      zone_bytes);
		exit(1);
	}
	/*
	 * The zone size must be large enough to hold the initial system
	 * block group for mkfs time.
	 */
	if (zone_bytes < BTRFS_MKFS_SYSTEM_GROUP_SIZE) {
		error("zoned: illegal zone size %llu (smaller than %d)",
		      zone_bytes, BTRFS_MKFS_SYSTEM_GROUP_SIZE);
		exit(1);
	}

	/*
	 * No need to use btrfs_device_size() here, since it is ensured
	 * that the file is block device.
	 */
	if (ioctl(fd, BLKGETSIZE64, &device_size) < 0) {
		error("zoned: ioctl(BLKGETSIZE64) failed on %s (%m)", file);
		exit(1);
	}

	/* Allocate the zone information array */
	zinfo->zone_size = zone_bytes;
	zinfo->nr_zones = device_size / zone_bytes;
	if (device_size & (zone_bytes - 1))
		zinfo->nr_zones++;
	zinfo->zones = calloc(zinfo->nr_zones, sizeof(struct blk_zone));
	if (!zinfo->zones) {
		error("zoned: no memory for zone information");
		exit(1);
	}

	/* Allocate a zone report */
	rep_size = sizeof(struct blk_zone_report) +
		   sizeof(struct blk_zone) * BTRFS_REPORT_NR_ZONES;
	rep = malloc(rep_size);
	if (!rep) {
		error("zoned: no memory for zones report");
		exit(1);
	}

	/* Get zone information */
	zone = (struct blk_zone *)(rep + 1);
	while (n < zinfo->nr_zones) {
		memset(rep, 0, rep_size);
		rep->sector = sector;
		rep->nr_zones = BTRFS_REPORT_NR_ZONES;

		ret = ioctl(fd, BLKREPORTZONE, rep);
		if (ret != 0) {
			error("zoned: ioctl BLKREPORTZONE failed (%m)");
			exit(1);
		}

		if (!rep->nr_zones)
			break;

		for (i = 0; i < rep->nr_zones; i++) {
			if (n >= zinfo->nr_zones)
				break;
			memcpy(&zinfo->zones[n], &zone[i],
			       sizeof(struct blk_zone));
			n++;
		}

		sector = zone[rep->nr_zones - 1].start +
			 zone[rep->nr_zones - 1].len;
	}

	free(rep);

	return 0;
}

#endif

int btrfs_get_dev_zone_info_all_devices(struct btrfs_fs_info *fs_info)
{
	struct btrfs_fs_devices *fs_devices = fs_info->fs_devices;
	struct btrfs_device *device;
	int ret = 0;

	/* fs_info->zone_size might not set yet. Use the incomapt flag here. */
	if (!btrfs_fs_incompat(fs_info, ZONED))
		return 0;

	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		/* We can skip reading of zone info for missing devices */
		if (device->fd == -1)
			continue;

		ret = btrfs_get_dev_zone_info(device);
		if (ret)
			break;
	}

	return ret;
}

static int btrfs_get_dev_zone_info(struct btrfs_device *device)
{
	struct btrfs_fs_info *fs_info = device->fs_info;

	/*
	 * Cannot use btrfs_is_zoned here, since fs_info::zone_size might not
	 * yet be set.
	 */
	if (!btrfs_fs_incompat(fs_info, ZONED))
		return 0;

	if (device->zone_info)
		return 0;

	return btrfs_get_zone_info(device->fd, device->name, &device->zone_info);
}

int btrfs_get_zone_info(int fd, const char *file,
			struct btrfs_zoned_device_info **zinfo_ret)
{
#ifdef BTRFS_ZONED
	struct btrfs_zoned_device_info *zinfo;
	int ret;
#endif
	enum btrfs_zoned_model model;

	*zinfo_ret = NULL;

	/* Check zone model */
	model = zoned_model(file);
	if (model == ZONED_NONE)
		return 0;

#ifdef BTRFS_ZONED
	zinfo = calloc(1, sizeof(*zinfo));
	if (!zinfo) {
		error("zoned: no memory for zone information");
		exit(1);
	}

	zinfo->model = model;

	/* Get zone information */
	ret = report_zones(fd, file, zinfo);
	if (ret != 0) {
		kfree(zinfo);
		return ret;
	}
	*zinfo_ret = zinfo;
#else
	error("zoned: %s: unsupported host-%s zoned block device", file,
	      model == ZONED_HOST_MANAGED ? "managed" : "aware");
	if (model == ZONED_HOST_MANAGED)
		return -EOPNOTSUPP;

	error("zoned: %s: handling host-aware block device as a regular disk",
	      file);
#endif

	return 0;
}

int btrfs_check_zoned_mode(struct btrfs_fs_info *fs_info)
{
	struct btrfs_fs_devices *fs_devices = fs_info->fs_devices;
	struct btrfs_device *device;
	u64 zoned_devices = 0;
	u64 nr_devices = 0;
	u64 zone_size = 0;
	const bool incompat_zoned = btrfs_fs_incompat(fs_info, ZONED);
	int ret = 0;

	/* Count zoned devices */
	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		enum btrfs_zoned_model model;

		if (device->fd == -1)
			continue;

		model = zoned_model(device->name);
		/*
		 * A Host-Managed zoned device must be used as a zoned device.
		 * A Host-Aware zoned device and a non-zoned devices can be
		 * treated as a zoned device, if ZONED flag is enabled in the
		 * superblock.
		 */
		if (model == ZONED_HOST_MANAGED ||
		    (model == ZONED_HOST_AWARE && incompat_zoned) ||
		    (model == ZONED_NONE && incompat_zoned)) {
			struct btrfs_zoned_device_info *zone_info =
				device->zone_info;

			zoned_devices++;
			if (!zone_size) {
				zone_size = zone_info->zone_size;
			} else if (zone_info->zone_size != zone_size) {
				error(
		"zoned: unequal block device zone sizes: have %llu found %llu",
				      device->zone_info->zone_size,
				      zone_size);
				ret = -EINVAL;
				goto out;
			}
		}
		nr_devices++;
	}

	if (!zoned_devices && !incompat_zoned)
		goto out;

	if (!zoned_devices && incompat_zoned) {
		/* No zoned block device found on ZONED filesystem */
		error("zoned: no zoned devices found on a zoned filesystem");
		ret = -EINVAL;
		goto out;
	}

	if (zoned_devices && !incompat_zoned) {
		error("zoned: mode not enabled but zoned device found");
		ret = -EINVAL;
		goto out;
	}

	if (zoned_devices != nr_devices) {
		error("zoned: cannot mix zoned and regular devices");
		ret = -EINVAL;
		goto out;
	}

	/*
	 * stripe_size is always aligned to BTRFS_STRIPE_LEN in
	 * __btrfs_alloc_chunk(). Since we want stripe_len == zone_size,
	 * check the alignment here.
	 */
	if (!IS_ALIGNED(zone_size, BTRFS_STRIPE_LEN)) {
		error("zoned: zone size %llu not aligned to stripe %u",
		      zone_size, BTRFS_STRIPE_LEN);
		ret = -EINVAL;
		goto out;
	}

	fs_info->zone_size = zone_size;

out:
	return ret;
}
