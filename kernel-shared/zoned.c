// SPDX-License-Identifier: GPL-2.0

#include <sys/ioctl.h>
#include <linux/fs.h>
#include <unistd.h>

#include "kernel-lib/list.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/zoned.h"
#include "common/utils.h"
#include "common/device-utils.h"
#include "common/messages.h"
#include "mkfs/common.h"

/* Maximum number of zones to report per ioctl(BLKREPORTZONE) call */
#define BTRFS_REPORT_NR_ZONES  		4096

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

#define BTRFS_SB_LOG_FIRST_SHIFT	ilog2(BTRFS_SB_LOG_FIRST_OFFSET)
#define BTRFS_SB_LOG_SECOND_SHIFT	ilog2(BTRFS_SB_LOG_SECOND_OFFSET)

#define EMULATED_ZONE_SIZE		SZ_256M

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

	/* Zoned emulation on regular device */
	if (zoned_model(file) == ZONED_NONE)
		return EMULATED_ZONE_SIZE;

	ret = queue_param(file, "chunk_sectors", chunk, sizeof(chunk));
	if (ret <= 0)
		return 0;

	return strtoull((const char *)chunk, NULL, 10) << SECTOR_SHIFT;
}

u64 max_zone_append_size(const char *file)
{
	char chunk[32];
	int ret;

	ret = queue_param(file, "zone_append_max_bytes", chunk, sizeof(chunk));
	if (ret <= 0)
		return 0;

	return strtoull((const char *)chunk, NULL, 10);
}

#ifdef BTRFS_ZONED
/*
 * Emulate blkdev_report_zones() for a non-zoned device. It slices up the block
 * device into fixed-sized chunks and emulate a conventional zone on each of
 * them.
 */
static int emulate_report_zones(const char *file, int fd, u64 pos,
				struct blk_zone *zones, unsigned int nr_zones)
{
	const sector_t zone_sectors = EMULATED_ZONE_SIZE >> SECTOR_SHIFT;
	struct stat st;
	sector_t bdev_size;
	unsigned int i;
	int ret;

	ret = fstat(fd, &st);
	if (ret < 0) {
		error("unable to stat %s: %m", file);
		return -EIO;
	}

	bdev_size = btrfs_device_size(fd, &st) >> SECTOR_SHIFT;

	pos >>= SECTOR_SHIFT;
	for (i = 0; i < nr_zones; i++) {
		zones[i].start = i * zone_sectors + pos;
		zones[i].len = zone_sectors;
		zones[i].capacity = zone_sectors;
		zones[i].wp = zones[i].start + zone_sectors;
		zones[i].type = BLK_ZONE_TYPE_CONVENTIONAL;
		zones[i].cond = BLK_ZONE_COND_NOT_WP;

		if (zones[i].wp >= bdev_size) {
			i++;
			break;
		}
	}

	return i;
}

static int sb_write_pointer(int fd, struct blk_zone *zones, u64 *wp_ret)
{
	bool empty[BTRFS_NR_SB_LOG_ZONES];
	bool full[BTRFS_NR_SB_LOG_ZONES];
	sector_t sector;

	ASSERT(zones[0].type != BLK_ZONE_TYPE_CONVENTIONAL &&
	       zones[1].type != BLK_ZONE_TYPE_CONVENTIONAL);

	empty[0] = (zones[0].cond == BLK_ZONE_COND_EMPTY);
	empty[1] = (zones[1].cond == BLK_ZONE_COND_EMPTY);
	full[0] = (zones[0].cond == BLK_ZONE_COND_FULL);
	full[1] = (zones[1].cond == BLK_ZONE_COND_FULL);

	/*
	 * Possible states of log buffer zones
	 *
	 *           Empty[0]  In use[0]  Full[0]
	 * Empty[1]         *          x        0
	 * In use[1]        0          x        0
	 * Full[1]          1          1        C
	 *
	 * Log position:
	 *   *: Special case, no superblock is written
	 *   0: Use write pointer of zones[0]
	 *   1: Use write pointer of zones[1]
	 *   C: Compare super blocks from zones[0] and zones[1], use the latest
	 *      one determined by generation
	 *   x: Invalid state
	 */

	if (empty[0] && empty[1]) {
		/* Special case to distinguish no superblock to read */
		*wp_ret = (zones[0].start << SECTOR_SHIFT);
		return -ENOENT;
	} else if (full[0] && full[1]) {
		/* Compare two super blocks */
		u8 buf[BTRFS_NR_SB_LOG_ZONES][BTRFS_SUPER_INFO_SIZE];
		struct btrfs_super_block *super[BTRFS_NR_SB_LOG_ZONES];
		int i;
		int ret;

		for (i = 0; i < BTRFS_NR_SB_LOG_ZONES; i++) {
			u64 bytenr;

			bytenr = ((zones[i].start + zones[i].len)
				   << SECTOR_SHIFT) - BTRFS_SUPER_INFO_SIZE;

			ret = pread64(fd, buf[i], BTRFS_SUPER_INFO_SIZE, bytenr);
			if (ret != BTRFS_SUPER_INFO_SIZE)
				return -EIO;
			super[i] = (struct btrfs_super_block *)&buf[i];
		}

		if (super[0]->generation > super[1]->generation)
			sector = zones[1].start;
		else
			sector = zones[0].start;
	} else if (!full[0] && (empty[1] || full[1])) {
		sector = zones[0].wp;
	} else if (full[0]) {
		sector = zones[1].wp;
	} else {
		return -EUCLEAN;
	}
	*wp_ret = sector << SECTOR_SHIFT;
	return 0;
}

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

int btrfs_reset_dev_zone(int fd, struct blk_zone *zone)
{
	struct blk_zone_range range;

	/* Nothing to do if it is already empty */
	if (zone->type == BLK_ZONE_TYPE_CONVENTIONAL ||
	    zone->cond == BLK_ZONE_COND_EMPTY)
		return 0;

	range.sector = zone->start;
	range.nr_sectors = zone->len;

	if (ioctl(fd, BLKRESETZONE, &range) < 0)
		return -errno;

	zone->cond = BLK_ZONE_COND_EMPTY;
	zone->wp = zone->start;

	return 0;
}

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
	zinfo->max_zone_append_size = max_zone_append_size(file);
	zinfo->nr_zones = device_size / zone_bytes;
	if (device_size & (zone_bytes - 1))
		zinfo->nr_zones++;

	if (zoned_model(file) != ZONED_NONE &&
	    zinfo->max_zone_append_size == 0) {
		error(
		"zoned: device %s does not support ZONE_APPEND command", file);
		exit(1);
	}

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

		if (zinfo->model != ZONED_NONE) {
			ret = ioctl(fd, BLKREPORTZONE, rep);
			if (ret != 0) {
				error("zoned: ioctl BLKREPORTZONE failed (%m)");
				exit(1);
			}
		} else {
			ret = emulate_report_zones(file, fd,
						   sector << SECTOR_SHIFT,
						   zone, BTRFS_REPORT_NR_ZONES);
			if (ret < 0) {
				error("zoned: failed to emulate BLKREPORTZONE");
				exit(1);
			}
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

static int sb_log_location(int fd, struct blk_zone *zones, int rw, u64 *bytenr_ret)
{
	u64 wp;
	int ret;

	/* Use the head of the zones if either zone is conventional */
	if (zones[0].type == BLK_ZONE_TYPE_CONVENTIONAL) {
		*bytenr_ret = zones[0].start << SECTOR_SHIFT;
		return 0;
	} else if (zones[1].type == BLK_ZONE_TYPE_CONVENTIONAL) {
		*bytenr_ret = zones[1].start << SECTOR_SHIFT;
		return 0;
	}

	ret = sb_write_pointer(fd, zones, &wp);
	if (ret != -ENOENT && ret < 0)
		return ret;

	if (rw == WRITE) {
		struct blk_zone *reset = NULL;

		if (wp == zones[0].start << SECTOR_SHIFT)
			reset = &zones[0];
		else if (wp == zones[1].start << SECTOR_SHIFT)
			reset = &zones[1];

		if (reset && reset->cond != BLK_ZONE_COND_EMPTY) {
			ASSERT(reset->cond == BLK_ZONE_COND_FULL);

			ret = btrfs_reset_dev_zone(fd, reset);
			if (ret)
				return ret;
		}
	} else if (ret != -ENOENT) {
		/* For READ, we want the previous one */
		if (wp == zones[0].start << SECTOR_SHIFT)
			wp = (zones[1].start + zones[1].len) << SECTOR_SHIFT;
		wp -= BTRFS_SUPER_INFO_SIZE;
	}

	*bytenr_ret = wp;
	return 0;
}

size_t btrfs_sb_io(int fd, void *buf, off_t offset, int rw)
{
	size_t count = BTRFS_SUPER_INFO_SIZE;
	struct stat stat_buf;
	struct blk_zone_report *rep;
	struct blk_zone *zones;
	const u64 sb_size_sector = (BTRFS_SUPER_INFO_SIZE >> SECTOR_SHIFT);
	u64 mapped = U64_MAX;
	u32 zone_num;
	unsigned int zone_size_sector;
	size_t rep_size;
	int mirror = -1;
	int i;
	int ret;
	size_t ret_sz;

	ASSERT(rw == READ || rw == WRITE);

	if (fstat(fd, &stat_buf) == -1) {
		error("fstat failed (%s)", strerror(errno));
		exit(1);
	}

	/* Do not call ioctl(BLKGETZONESZ) on a regular file. */
	if ((stat_buf.st_mode & S_IFMT) == S_IFBLK) {
		ret = ioctl(fd, BLKGETZONESZ, &zone_size_sector);
		if (ret) {
			error("zoned: ioctl BLKGETZONESZ failed (%m)");
			exit(1);
		}
	} else {
		zone_size_sector = 0;
	}

	/* We can call pread/pwrite if 'fd' is non-zoned device/file */
	if (zone_size_sector == 0) {
		if (rw == READ)
			return pread64(fd, buf, count, offset);
		return pwrite64(fd, buf, count, offset);
	}

	ASSERT(IS_ALIGNED(zone_size_sector, sb_size_sector));

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		if (offset == btrfs_sb_offset(i)) {
			mirror = i;
			break;
		}
	}
	ASSERT(mirror != -1);

	zone_num = sb_zone_number(ilog2(zone_size_sector) + SECTOR_SHIFT,
				  mirror);

	rep_size = sizeof(struct blk_zone_report) + sizeof(struct blk_zone) * 2;
	rep = calloc(1, rep_size);
	if (!rep) {
		error("zoned: no memory for zones report");
		exit(1);
	}

	rep->sector = zone_num * (sector_t)zone_size_sector;
	rep->nr_zones = 2;

	ret = ioctl(fd, BLKREPORTZONE, rep);
	if (ret) {
		error("zoned: ioctl BLKREPORTZONE failed (%m)");
		exit(1);
	}
	if (rep->nr_zones != 2) {
		if (errno == ENOENT || errno == 0)
			return (rw == WRITE ? count : 0);
		error("zoned: failed to read zone info of %u and %u: %m",
		      zone_num, zone_num + 1);
		free(rep);
		return 0;
	}

	zones = (struct blk_zone *)(rep + 1);

	ret = sb_log_location(fd, zones, rw, &mapped);
	/*
	 * Special case: no superblock found in the zones. This case happens
	 * when initializing a file-system.
	 */
	if (rw == READ && ret == -ENOENT) {
		memset(buf, 0, count);
		return count;
	}
	if (ret)
		return ret;

	if (rw == READ)
		ret_sz = pread64(fd, buf, count, mapped);
	else
		ret_sz = pwrite64(fd, buf, count, mapped);

	if (ret_sz != count)
		return ret_sz;

	/* Call fsync() to force the write order */
	if (rw == WRITE && fsync(fd)) {
		error("failed to synchronize superblock: %s", strerror(errno));
		exit(1);
	}

	return ret_sz;
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
	u64 max_zone_append_size = 0;
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
			if (!max_zone_append_size ||
			    (zone_info->max_zone_append_size &&
			     zone_info->max_zone_append_size < max_zone_append_size))
				max_zone_append_size =
					zone_info->max_zone_append_size;
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

	if (btrfs_fs_incompat(fs_info, MIXED_GROUPS)) {
		error("zoned: mixed block groups not supported");
		ret = -EINVAL;
		goto out;
	}

	fs_info->zone_size = zone_size;
	fs_info->max_zone_append_size = max_zone_append_size;

out:
	return ret;
}
