// SPDX-License-Identifier: GPL-2.0
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

#include "kerncompat.h"
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "kernel-lib/list.h"
#include "kernel-lib/bitmap.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/zoned.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "common/utils.h"
#include "common/device-utils.h"
#include "common/extent-cache.h"
#include "common/internal.h"
#include "common/string-utils.h"
#include "common/messages.h"
#include "mkfs/common.h"

/* Maximum number of zones to report per ioctl(BLKREPORTZONE) call */
#define BTRFS_REPORT_NR_ZONES  		4096
/* Invalid allocation pointer value for missing devices */
#define WP_MISSING_DEV			((u64)-1)
/* Pseudo write pointer value for conventional zone */
#define WP_CONVENTIONAL			((u64)-2)

#define DEFAULT_EMULATED_ZONE_SIZE		SZ_256M

static u64 emulated_zone_size = DEFAULT_EMULATED_ZONE_SIZE;

/*
 * Minimum / maximum supported zone size. Currently, SMR disks have a zone size
 * of 256MiB, and we are expecting ZNS drives to be in the 1-4GiB range.  We do
 * not expect the zone size to become larger than 8GiB or smaller than 4MiB in
 * the near future.
 */
#define BTRFS_MAX_ZONE_SIZE		(8ULL * SZ_1G)
#define BTRFS_MIN_ZONE_SIZE		(SZ_4M)

/*
 * Minimum of active zones we need:
 *
 * - BTRFS_SUPER_MIRROR_MAX zones for superblock mirrors
 * - 3 zones to ensure at least one zone per SYSTEM, META and DATA block group
 * - 1 zone for tree-log dedicated block group
 * - 1 zone for relocation
 */
#define BTRFS_MIN_ACTIVE_ZONES		(BTRFS_SUPER_MIRROR_MAX + 5)

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

	ret = device_get_queue_param(file, "zoned", model, sizeof(model));
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
	if (zoned_model(file) == ZONED_NONE) {
		const char *tmp;
		u64 size = DEFAULT_EMULATED_ZONE_SIZE;

		tmp = bconf_param_value("zone-size");
		if (tmp) {
			size = arg_strtou64_with_suffix(tmp);
			if (!is_power_of_2(size) || size < BTRFS_MIN_ZONE_SIZE ||
			    size > BTRFS_MAX_ZONE_SIZE) {
				error("invalid emulated zone size %llu", size);
				exit(1);
			}
		}
		emulated_zone_size = size;
		return emulated_zone_size;
	}

	ret = device_get_queue_param(file, "chunk_sectors", chunk, sizeof(chunk));
	if (ret <= 0)
		return 0;

	return strtoull((const char *)chunk, NULL, 10) << SECTOR_SHIFT;
}

static u64 max_zone_append_size(const char *file)
{
	char chunk[32];
	int ret;

	ret = device_get_queue_param(file, "zone_append_max_bytes", chunk,
				     sizeof(chunk));
	if (ret <= 0)
		return 0;

	return strtoull((const char *)chunk, NULL, 10);
}

static unsigned int max_active_zone_count(const char *file)
{
	char buf[32];
	int ret;

	ret = device_get_queue_param(file, "max_active_zones", buf, sizeof(buf));
	if (ret <= 0)
		return 0;

	return strtoul((const char *)buf, NULL, 10);
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
	const sector_t zone_sectors = emulated_zone_size >> SECTOR_SHIFT;
	struct stat st;
	u64 bdev_size;
	sector_t bdev_nr_sectors;
	unsigned int i;
	int ret;

	ret = fstat(fd, &st);
	if (ret < 0) {
		error("unable to stat %s: %m", file);
		return -EIO;
	}

	ret = device_get_partition_size_fd_stat(fd, &st, &bdev_size);
	if (ret < 0) {
		errno = -ret;
		error("failed to get device size for %s: %m", file);
		return ret;
	}
	bdev_nr_sectors = bdev_size >> SECTOR_SHIFT;

	pos >>= SECTOR_SHIFT;
	for (i = 0; i < nr_zones; i++) {
		zones[i].start = i * zone_sectors + pos;
		zones[i].len = zone_sectors;
		zones[i].capacity = zone_sectors;
		zones[i].wp = zones[i].start + zone_sectors;
		zones[i].type = BLK_ZONE_TYPE_CONVENTIONAL;
		zones[i].cond = BLK_ZONE_COND_NOT_WP;

		if (zones[i].wp >= bdev_nr_sectors) {
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

			ret = pread(fd, buf[i], BTRFS_SUPER_INFO_SIZE, bytenr);
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
	struct stat st;
	struct blk_zone_report *rep;
	struct blk_zone *zone;
	unsigned int i, nreported = 0, nactive = 0;
	unsigned int max_active_zones;
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

	ret = fstat(fd, &st);
	if (ret < 0) {
		error("error when reading zone info on %s: %m", file);
		return -EIO;
	}

	ret = device_get_partition_size_fd_stat(fd, &st, &device_size);
	if (ret < 0) {
		errno = -ret;
		error("zoned: failed to read size of %s: %m", file);
		exit(1);
	}

	/* Allocate the zone information array */
	zinfo->zone_size = zone_bytes;
	zinfo->nr_zones = device_size / zone_bytes;

	if (zinfo->zone_size > BTRFS_MAX_ZONE_SIZE) {
		error("zoned: zone size %llu larger than supported maximum %llu",
		      zinfo->zone_size, BTRFS_MAX_ZONE_SIZE);
		exit(1);
	} else if (zinfo->zone_size < BTRFS_MIN_ZONE_SIZE) {
		error("zoned: zone size %llu smaller than supported minimum %u",
		      zinfo->zone_size, BTRFS_MIN_ZONE_SIZE);
		exit(1);
	}

	if (device_size & (zone_bytes - 1))
		zinfo->nr_zones++;

	if (zoned_model(file) != ZONED_NONE && max_zone_append_size(file) == 0) {
		error(
		"zoned: device %s does not support ZONE_APPEND command", file);
		exit(1);
	}

	zinfo->zones = calloc(zinfo->nr_zones, sizeof(struct blk_zone));
	if (!zinfo->zones) {
		error_mem("zone information");
		exit(1);
	}

	zinfo->active_zones = bitmap_zalloc(zinfo->nr_zones);
	if (!zinfo->active_zones) {
		error_mem("active zone bitmap");
		exit(1);
	}

	max_active_zones = max_active_zone_count(file);
	if (max_active_zones && max_active_zones < BTRFS_MIN_ACTIVE_ZONES) {
		error("zoned: %s: max active zones %u is too small, need at least %u active zones",
		      file, max_active_zones, BTRFS_MIN_ACTIVE_ZONES);
		exit(1);
	}
	zinfo->max_active_zones = max_active_zones;

	/* Allocate a zone report */
	rep_size = sizeof(struct blk_zone_report) +
		   sizeof(struct blk_zone) * BTRFS_REPORT_NR_ZONES;
	rep = kmalloc(rep_size, GFP_KERNEL);
	if (!rep) {
		error_mem("zone report");
		exit(1);
	}

	/* Get zone information */
	zone = (struct blk_zone *)(rep + 1);
	while (nreported < zinfo->nr_zones) {
		memset(rep, 0, rep_size);
		rep->sector = sector;
		rep->nr_zones = BTRFS_REPORT_NR_ZONES;

		if (zinfo->model != ZONED_NONE) {
			ret = ioctl(fd, BLKREPORTZONE, rep);
			if (ret != 0) {
				error("zoned: ioctl BLKREPORTZONE failed (%m)");
				exit(1);
			}
			zinfo->emulated = false;
		} else {
			ret = emulate_report_zones(file, fd,
						   sector << SECTOR_SHIFT,
						   zone, BTRFS_REPORT_NR_ZONES);
			if (ret < 0) {
				error("zoned: failed to emulate BLKREPORTZONE");
				exit(1);
			}
			zinfo->emulated = true;
		}

		if (!rep->nr_zones)
			break;

		for (i = 0; i < rep->nr_zones; i++) {
			if (nreported >= zinfo->nr_zones)
				break;
			memcpy(&zinfo->zones[nreported], &zone[i],
			       sizeof(struct blk_zone));
			switch (zone[i].cond) {
			case BLK_ZONE_COND_EMPTY:
				break;
			case BLK_ZONE_COND_IMP_OPEN:
			case BLK_ZONE_COND_EXP_OPEN:
			case BLK_ZONE_COND_CLOSED:
				set_bit(nreported, zinfo->active_zones);
				nactive++;
				break;
			}
			nreported++;
		}

		sector = zone[rep->nr_zones - 1].start +
			 zone[rep->nr_zones - 1].len;
	}

	if (max_active_zones) {
		if (nactive > max_active_zones) {
			error("zoned: %u active zones on %s exceeds max_active_zones %u",
			      nactive, file, max_active_zones);
			exit(1);
		}
		zinfo->active_zones_left = max_active_zones - nactive;
	}

	kfree(rep);

	return 0;
}

/*
 * Discard blocks in the zones of a zoned block device. Process this with zone
 * size granularity so that blocks in conventional zones are discarded using
 * discard_range and blocks in sequential zones are reset though a zone reset.
 *
 * We need to ensure that zones outside of the fs are not active, so that the fs
 * can use all the active zones. Return EBUSY if there is an active zone.
 */
int btrfs_reset_zones(int fd, struct btrfs_zoned_device_info *zinfo, u64 byte_count)
{
	unsigned int i;
	int ret = 0;

	ASSERT(zinfo);
	ASSERT(IS_ALIGNED(byte_count, zinfo->zone_size));

	/* Zone size granularity */
	for (i = 0; i < zinfo->nr_zones; i++) {
		if (byte_count == 0)
			break;

		if (zinfo->zones[i].type == BLK_ZONE_TYPE_CONVENTIONAL) {
			ret = device_discard_blocks(fd,
					     zinfo->zones[i].start << SECTOR_SHIFT,
					     zinfo->zone_size);
			if (ret == EOPNOTSUPP)
				ret = 0;
		} else if (zinfo->zones[i].cond != BLK_ZONE_COND_EMPTY) {
			ret = btrfs_reset_dev_zone(fd, &zinfo->zones[i]);
		} else {
			ret = 0;
		}

		if (ret)
			return ret;

		byte_count -= zinfo->zone_size;
	}
	for (; i < zinfo->nr_zones; i++) {
		const enum blk_zone_cond cond = zinfo->zones[i].cond;

		if (zinfo->zones[i].type == BLK_ZONE_TYPE_CONVENTIONAL)
			continue;
		if (cond == BLK_ZONE_COND_IMP_OPEN ||
		    cond == BLK_ZONE_COND_EXP_OPEN ||
		    cond == BLK_ZONE_COND_CLOSED)
			return EBUSY;
	}

	return fsync(fd);
}

int zero_zone_blocks(int fd, struct btrfs_zoned_device_info *zinfo, off_t start,
		     size_t len)
{
	size_t zone_len = zinfo->zone_size;
	off_t ofst = start;
	size_t count;
	int ret;

	/* Make sure that device_zero_blocks does not write sequential zones */
	while (len > 0) {
		/* Limit device_zero_blocks to a single zone */
		count = min_t(size_t, len, zone_len);
		if (count > zone_len - (ofst & (zone_len - 1)))
			count = zone_len - (ofst & (zone_len - 1));

		if (!zone_is_sequential(zinfo, ofst)) {
			ret = device_zero_blocks(fd, ofst, count, true);
			if (ret != 0)
				return ret;
		}

		len -= count;
		ofst += count;
	}

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

static u32 sb_bytenr_to_sb_zone(u64 bytenr, int zone_size_shift)
{
	int mirror = -1;

	for (int i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		if (bytenr == btrfs_sb_offset(i)) {
			mirror = i;
			break;
		}
	}
	ASSERT(mirror != -1);

	return sb_zone_number(zone_size_shift, mirror);
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
	u32 zone_size_sector;
	size_t rep_size;
	int ret;
	size_t ret_sz;

	ASSERT(rw == READ || rw == WRITE);

	if (fstat(fd, &stat_buf) == -1) {
		error("fstat failed: %m");
		exit(1);
	}

	/* Do not call ioctl(BLKGETZONESZ) on a regular file. */
	if ((stat_buf.st_mode & S_IFMT) == S_IFBLK) {
		ret = ioctl(fd, BLKGETZONESZ, &zone_size_sector);
		if (ret < 0) {
			if (errno == ENOTTY || errno == EINVAL) {
				/*
				 * No kernel support, assuming non-zoned device.
				 *
				 * Note: older kernels before 5.11 could return
				 * EINVAL in case the ioctl is not available,
				 * which is wrong.
				 */
				zone_size_sector = 0;
			} else {
				error("zoned: ioctl BLKGETZONESZ failed: %m");
				exit(1);
			}
		}
	} else {
		zone_size_sector = 0;
	}

	/* We can call pread/pwrite if 'fd' is non-zoned device/file */
	if (zone_size_sector == 0) {
		if (rw == READ)
			return pread(fd, buf, count, offset);
		return pwrite(fd, buf, count, offset);
	}

	ASSERT(IS_ALIGNED(zone_size_sector, sb_size_sector));

	zone_num = sb_bytenr_to_sb_zone(offset, ilog2(zone_size_sector) + SECTOR_SHIFT);

	rep_size = sizeof(struct blk_zone_report) + sizeof(struct blk_zone) * 2;
	rep = calloc(1, rep_size);
	if (!rep) {
		error_mem("zone report");
		exit(1);
	}

	rep->sector = zone_num * (sector_t)zone_size_sector;
	rep->nr_zones = 2;

	ret = ioctl(fd, BLKREPORTZONE, rep);
	if (ret) {
		if (errno == ENOTTY || errno == EINVAL) {
			/*
			 * Note: older kernels before 5.11 could return EINVAL
			 * in case the ioctl is not available, which is wrong.
			 */
			error("zoned: BLKREPORTZONE failed but BLKGETZONESZ works: %m");
			exit(1);
		}
		error("zoned: ioctl BLKREPORTZONE failed: %m");
		exit(1);
	}
	if (rep->nr_zones != 2) {
		if (errno == ENOENT || errno == 0)
			return (rw == WRITE ? count : 0);
		error("zoned: failed to read zone info of %u and %u: %m",
		      zone_num, zone_num + 1);
		kfree(rep);
		return 0;
	}

	zones = (struct blk_zone *)(rep + 1);

	ret = sb_log_location(fd, zones, rw, &mapped);
	kfree(rep);
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
		ret_sz = btrfs_pread(fd, buf, count, mapped, true);
	else
		ret_sz = btrfs_pwrite(fd, buf, count, mapped, true);

	if (ret_sz != count)
		return ret_sz;

	/* Call fsync() to force the write order */
	if (rw == WRITE && fsync(fd)) {
		error("failed to synchronize superblock: %m");
		exit(1);
	}

	return ret_sz;
}

/**
 * btrfs_find_allocatable_zones - find allocatable zones within a given region
 *
 * @device:	the device to allocate a region on
 * @hole_start: the position of the hole to allocate the region
 * @num_bytes:	size of wanted region
 * @hole_end:	the end of the hole
 * @return:	position of allocatable zones
 *
 * Allocatable region should not contain any superblock locations.
 */
u64 btrfs_find_allocatable_zones(struct btrfs_device *device, u64 hole_start,
				 u64 hole_end, u64 num_bytes)
{
	struct btrfs_zoned_device_info *zinfo = device->zone_info;
	int shift = ilog2(zinfo->zone_size);
	u64 nzones = num_bytes >> shift;
	u64 pos = hole_start;
	u64 begin, end;
	bool is_sequential;
	bool have_sb;
	int i;

	ASSERT(IS_ALIGNED(hole_start, zinfo->zone_size));
	ASSERT(IS_ALIGNED(num_bytes, zinfo->zone_size));

	while (pos < hole_end) {
		begin = pos >> shift;
		end = begin + nzones;

		if (end > zinfo->nr_zones)
			return hole_end;

		/*
		 * The zones must be all sequential (and empty), or
		 * conventional
		 */
		is_sequential = btrfs_dev_is_sequential(device, pos);
		for (i = 0; i < end - begin; i++) {
			u64 zone_offset = pos + ((u64)i << shift);

			if ((is_sequential &&
			     !btrfs_dev_is_empty_zone(device, zone_offset)) ||
			    (is_sequential !=
			     btrfs_dev_is_sequential(device, zone_offset))) {
				pos += zinfo->zone_size;
				continue;
			}
		}

		have_sb = false;
		for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
			u32 sb_zone;
			u64 sb_pos;

			sb_zone = sb_zone_number(shift, i);
			if (!(end <= sb_zone ||
			      sb_zone + BTRFS_NR_SB_LOG_ZONES <= begin)) {
				have_sb = true;
				pos = ((u64)sb_zone + BTRFS_NR_SB_LOG_ZONES) << shift;
				break;
			}

			/* We also need to exclude regular superblock positions */
			sb_pos = btrfs_sb_offset(i);
			if (!(pos + num_bytes <= sb_pos ||
			      sb_pos + BTRFS_SUPER_INFO_SIZE <= pos)) {
				have_sb = true;
				pos = ALIGN(sb_pos + BTRFS_SUPER_INFO_SIZE,
					    zinfo->zone_size);
				break;
			}
		}
		if (!have_sb)
			break;
	}

	return pos;
}

/*
 * Calculate an allocation pointer from the extent allocation information
 * for a block group consisting of conventional zones. It is pointed to the
 * end of the highest addressed extent in the block group as an allocation
 * offset.
 */
static int calculate_alloc_pointer(struct btrfs_fs_info *fs_info,
				   struct btrfs_block_group *cache,
				   u64 *offset_ret)
{
	struct btrfs_root *root = btrfs_extent_root(fs_info, cache->start);
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	int ret;
	u64 length;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = cache->start + cache->length;
	key.type = 0;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	/* There should be no exact match (ie. an extent) at this address */
	if (!ret)
		ret = -EUCLEAN;
	if (ret < 0)
		goto out;

	ret = btrfs_previous_extent_item(root, path, cache->start);
	if (ret) {
		if (ret == 1) {
			ret = 0;
			*offset_ret = 0;
		}
		goto out;
	}

	btrfs_item_key_to_cpu(path->nodes[0], &found_key, path->slots[0]);

	if (found_key.type == BTRFS_EXTENT_ITEM_KEY)
		length = found_key.offset;
	else
		length = fs_info->nodesize;

	if (!(found_key.objectid >= cache->start &&
	       found_key.objectid + length <= cache->start + cache->zone_capacity)) {
		ret = -EUCLEAN;
		goto out;
	}
	*offset_ret = found_key.objectid + length - cache->start;
	ret = 0;

out:
	btrfs_free_path(path);
	return ret;
}

bool zoned_profile_supported(u64 map_type, bool rst)
{
	bool data = (map_type & BTRFS_BLOCK_GROUP_DATA);
	u64 flags = (map_type & BTRFS_BLOCK_GROUP_PROFILE_MASK);

	/* SINGLE */
	if (flags == 0)
		return true;

	if (data) {
		if ((flags & BTRFS_BLOCK_GROUP_DUP) && rst)
			return true;
		/* Data RAID1 needs a raid-stripe-tree. */
		if ((flags & BTRFS_BLOCK_GROUP_RAID1_MASK) && rst)
			return true;
		/* Data RAID0 needs a raid-stripe-tree. */
		if ((flags & BTRFS_BLOCK_GROUP_RAID0) && rst)
			return true;
		/* Data RAID10 needs a raid-stripe-tree. */
		if ((flags & BTRFS_BLOCK_GROUP_RAID10) && rst)
			return true;
	} else {
		/* We can support DUP on metadata/system. */
		if (flags & BTRFS_BLOCK_GROUP_DUP)
			return true;
		/* We can support RAID1 on metadata/system. */
		if (flags & BTRFS_BLOCK_GROUP_RAID1_MASK)
			return true;
		/* We can support RAID0 on metadata/system. */
		if (flags & BTRFS_BLOCK_GROUP_RAID0)
			return true;
		/* We can support RAID10 on metadata/system. */
		if (flags & BTRFS_BLOCK_GROUP_RAID10)
			return true;
	}

	/* All other profiles are not supported yet */
	return false;
}

struct zone_info {
	u64 physical;
	u64 capacity;
	u64 alloc_offset;
};

static int btrfs_load_zone_info(struct btrfs_fs_info *fs_info, int zone_idx,
				struct zone_info *info, unsigned long *active,
				struct map_lookup *map)
{
	struct btrfs_device *device;
	struct blk_zone zone;

	info->physical = map->stripes[zone_idx].physical;

	device = map->stripes[zone_idx].dev;

	if (device->fd == -1) {
		info->alloc_offset = WP_MISSING_DEV;
		return 0;
	}

	/* Consider a zone as active if we can allow any number of active zones. */
	if (!device->zone_info->max_active_zones)
		set_bit(zone_idx, active);

	if (!btrfs_dev_is_sequential(device, info->physical)) {
		info->alloc_offset = WP_CONVENTIONAL;
		info->capacity = device->zone_info->zone_size;
		return 0;
	}

	/*
	 * The group is mapped to a sequential zone. Get the zone write
	 * pointer to determine the allocation offset within the zone.
	 */
	WARN_ON(!IS_ALIGNED(info->physical, fs_info->zone_size));
	zone = device->zone_info->zones[info->physical / fs_info->zone_size];

	if (zone.type == BLK_ZONE_TYPE_CONVENTIONAL) {
		error("zoned: unexpected conventional zone %llu on device %s (devid %llu)",
		      zone.start << SECTOR_SHIFT, device->name,
		      device->devid);
		return -EIO;
	}

	info->capacity = (zone.capacity << SECTOR_SHIFT);

	switch (zone.cond) {
	case BLK_ZONE_COND_OFFLINE:
	case BLK_ZONE_COND_READONLY:
		error(
	"zoned: offline/readonly zone %llu on device %s (devid %llu)",
		      info->physical / fs_info->zone_size, device->name,
		      device->devid);
		info->alloc_offset = WP_MISSING_DEV;
		break;
	case BLK_ZONE_COND_EMPTY:
		info->alloc_offset = 0;
		break;
	case BLK_ZONE_COND_FULL:
		info->alloc_offset = fs_info->zone_size;
		break;
	default:
		/* Partially used zone */
		info->alloc_offset = ((zone.wp - zone.start) << SECTOR_SHIFT);
		set_bit(zone_idx, active);
		break;
	}

	return 0;
}

static int btrfs_load_block_group_single(struct btrfs_fs_info *fs_info,
					 struct btrfs_block_group *bg,
					 struct zone_info *info,
					 unsigned long *active)
{
	if (info->alloc_offset == WP_MISSING_DEV) {
		btrfs_err(fs_info,
			"zoned: cannot recover write pointer for zone %llu",
			info->physical);
		return -EIO;
	}

	bg->alloc_offset = info->alloc_offset;
	bg->zone_capacity = info->capacity;
	if (test_bit(0, active))
		bg->zone_is_active = 1;
	return 0;
}

static int btrfs_load_block_group_dup(struct btrfs_fs_info *fs_info,
				      struct btrfs_block_group *bg,
				      struct map_lookup *map,
				      struct zone_info *zone_info,
				      unsigned long *active, u64 last_alloc)
{
	if ((map->type & BTRFS_BLOCK_GROUP_DATA) && !fs_info->stripe_root) {
		btrfs_err(fs_info, "zoned: data DUP profile needs raid-stripe-tree");
		return -EINVAL;
	}

	bg->zone_capacity = min_not_zero(zone_info[0].capacity, zone_info[1].capacity);

	if (zone_info[0].alloc_offset == WP_MISSING_DEV) {
		btrfs_err(fs_info,
			  "zoned: cannot recover write pointer for zone %llu",
			  zone_info[0].physical);
		return -EIO;
	}
	if (zone_info[1].alloc_offset == WP_MISSING_DEV) {
		btrfs_err(fs_info,
			  "zoned: cannot recover write pointer for zone %llu",
			  zone_info[1].physical);
		return -EIO;
	}

	if (zone_info[0].alloc_offset == WP_CONVENTIONAL)
		zone_info[0].alloc_offset = last_alloc;
	if (zone_info[1].alloc_offset == WP_CONVENTIONAL)
		zone_info[1].alloc_offset = last_alloc;

	if (zone_info[0].alloc_offset != zone_info[1].alloc_offset) {
		btrfs_err(fs_info,
			  "zoned: write pointer offset mismatch of zones in DUP profile");
		return -EIO;
	}

	if (test_bit(0, active) != test_bit(1, active)) {
		return -EIO;
	} else if (test_bit(0, active)) {
		bg->zone_is_active = 1;
	}

	bg->alloc_offset = zone_info[0].alloc_offset;
	return 0;
}

static int btrfs_load_block_group_raid1(struct btrfs_fs_info *fs_info,
					struct btrfs_block_group *bg,
					struct map_lookup *map,
					struct zone_info *zone_info,
					unsigned long *active, u64 last_alloc)
{
	int i;

	if ((map->type & BTRFS_BLOCK_GROUP_DATA) && !fs_info->stripe_root) {
		btrfs_err(fs_info, "zoned: data %s needs raid-stripe-tree",
			  btrfs_bg_type_to_raid_name(map->type));
		return -EINVAL;
	}

	/* In case a device is missing we have a cap of 0, so don't use it. */
	bg->zone_capacity = min_not_zero(zone_info[0].capacity, zone_info[1].capacity);

	for (i = 0; i < map->num_stripes; i++) {
		if (zone_info[i].alloc_offset == WP_MISSING_DEV)
			continue;
		if (zone_info[i].alloc_offset == WP_CONVENTIONAL)
			zone_info[i].alloc_offset = last_alloc;

		if (zone_info[0].alloc_offset != zone_info[i].alloc_offset) {
			btrfs_err(fs_info,
			"zoned: write pointer offset mismatch of zones in %s profile",
				  btrfs_bg_type_to_raid_name(map->type));
			return -EIO;
		}
		if (test_bit(0, active) != test_bit(i, active)) {
			return -EIO;
		} else {
			if (test_bit(0, active))
				bg->zone_is_active = 1;
		}
	}

	if (zone_info[0].alloc_offset != WP_MISSING_DEV)
		bg->alloc_offset = zone_info[0].alloc_offset;
	else
		bg->alloc_offset = zone_info[i - 1].alloc_offset;

	return 0;
}

static int btrfs_load_block_group_raid0(struct btrfs_fs_info *fs_info,
					struct btrfs_block_group *bg,
					struct map_lookup *map,
					struct zone_info *zone_info,
					unsigned long *active, u64 last_alloc)
{
	if ((map->type & BTRFS_BLOCK_GROUP_DATA) && !fs_info->stripe_root) {
		btrfs_err(fs_info, "zoned: data %s needs raid-stripe-tree",
			  btrfs_bg_type_to_raid_name(map->type));
		return -EINVAL;
	}

	for (int i = 0; i < map->num_stripes; i++) {
		if (zone_info[i].alloc_offset == WP_MISSING_DEV)
			continue;
		if (zone_info[i].alloc_offset == WP_CONVENTIONAL) {
			u64 stripe_nr, full_stripe_nr;
			u64 stripe_offset;
			int stripe_index;

			stripe_nr = last_alloc / map->stripe_len;
			stripe_offset = stripe_nr * map->stripe_len;
			full_stripe_nr = stripe_nr / map->num_stripes;
			stripe_index = stripe_nr % map->num_stripes;

			zone_info[i].alloc_offset = full_stripe_nr * map->stripe_len;
			if (stripe_index > i)
				zone_info[i].alloc_offset += map->stripe_len;
			else if (stripe_index == i)
				zone_info[i].alloc_offset += (last_alloc - stripe_offset);
		}

		if (test_bit(0, active) != test_bit(i, active)) {
			return -EIO;
		} else {
			if (test_bit(0, active))
				bg->zone_is_active = 1;
		}
		bg->zone_capacity += zone_info[i].capacity;
		bg->alloc_offset += zone_info[i].alloc_offset;
	}

	return 0;
}

static int btrfs_load_block_group_raid10(struct btrfs_fs_info *fs_info,
					 struct btrfs_block_group *bg,
					 struct map_lookup *map,
					 struct zone_info *zone_info,
					 unsigned long *active, u64 last_alloc)
{
	if ((map->type & BTRFS_BLOCK_GROUP_DATA) && !fs_info->stripe_root) {
		btrfs_err(fs_info, "zoned: data %s needs raid-stripe-tree",
			  btrfs_bg_type_to_raid_name(map->type));
		return -EINVAL;
	}

	for (int i = 0; i < map->num_stripes; i++) {
		if (zone_info[i].alloc_offset == WP_MISSING_DEV)
			continue;
		if (zone_info[i].alloc_offset == WP_CONVENTIONAL) {
			u64 stripe_nr, full_stripe_nr;
			u64 stripe_offset;
			int stripe_index;

			stripe_nr = last_alloc / map->stripe_len;
			stripe_offset = stripe_nr * map->stripe_len;
			full_stripe_nr = stripe_nr / (map->num_stripes / map->sub_stripes);
			stripe_index = stripe_nr % (map->num_stripes / map->sub_stripes);

			zone_info[i].alloc_offset = full_stripe_nr * map->stripe_len;
			if (stripe_index > (i / map->sub_stripes))
				zone_info[i].alloc_offset += map->stripe_len;
			else if (stripe_index == (i / map->sub_stripes))
				zone_info[i].alloc_offset += (last_alloc - stripe_offset);
		}

		if (test_bit(0, active) != test_bit(i, active)) {
			return -EIO;
		} else {
			if (test_bit(0, active))
				bg->zone_is_active = 1;
		}

		if ((i % map->sub_stripes) == 0) {
			bg->zone_capacity += zone_info[i].capacity;
			bg->alloc_offset += zone_info[i].alloc_offset;
		}
	}

	return 0;
}

int btrfs_load_block_group_zone_info(struct btrfs_fs_info *fs_info,
				     struct btrfs_block_group *cache)
{
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	struct cache_extent *ce;
	struct map_lookup *map;
	u64 logical = cache->start;
	u64 length = cache->length;
	struct zone_info *zone_info = NULL;
	unsigned long *active = NULL;
	int ret = 0;
	int i;
	u64 last_alloc = 0;
	u32 num_conventional = 0;
	u64 profile;

	if (!btrfs_is_zoned(fs_info))
		return 0;

	/* Sanity check */
	if (logical == BTRFS_BLOCK_RESERVED_1M_FOR_SUPER) {
		if (length + SZ_1M != fs_info->zone_size) {
			error("zoned: unaligned initial system block group");
			return -EIO;
		}
	} else if (!IS_ALIGNED(length, fs_info->zone_size)) {
		error("zoned: unaligned block group at %llu + %llu", logical,
		      length);
		return -EIO;
	}

	/* Get the chunk mapping */
	ce = search_cache_extent(&map_tree->cache_tree, logical);
	if (!ce) {
		error("zoned: failed to find block group at %llu", logical);
		return -ENOENT;
	}
	map = container_of(ce, struct map_lookup, ce);

	zone_info = calloc(map->num_stripes, sizeof(*zone_info));
	if (!zone_info) {
		error_mem("zone info");
		return -ENOMEM;
	}

	active = bitmap_zalloc(map->num_stripes);
	if (!active) {
		free(zone_info);
		error_mem("active bitmap");
		return -ENOMEM;
	}

	for (i = 0; i < map->num_stripes; i++) {
		ret = btrfs_load_zone_info(fs_info, i, &zone_info[i], active, map);
		if (ret)
			goto out;

		if (zone_info[i].alloc_offset == WP_CONVENTIONAL)
			num_conventional++;
	}

	if (num_conventional > 0) {
		/* Zone capacity is always zone size in emulation */
		cache->zone_capacity = cache->length;
		ret = calculate_alloc_pointer(fs_info, cache, &last_alloc);
		if (ret || map->num_stripes == num_conventional) {
			if (!ret)
				cache->alloc_offset = last_alloc;
			else
				error(
		"zoned: failed to determine allocation offset of block group %llu",
					  cache->start);
			goto out;
		}
	}

	if (!zoned_profile_supported(map->type, !!fs_info->stripe_root)) {
		error("zoned: profile %s not yet supported",
		      btrfs_group_profile_str(map->type));
		ret = -EINVAL;
		goto out;
	}

	profile = map->type & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	switch (profile) {
	case 0: /* single */
		ret = btrfs_load_block_group_single(fs_info, cache, &zone_info[0], active);
		break;
	case BTRFS_BLOCK_GROUP_DUP:
		ret = btrfs_load_block_group_dup(fs_info, cache, map, zone_info, active, last_alloc);
		break;
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID1C3:
	case BTRFS_BLOCK_GROUP_RAID1C4:
		ret = btrfs_load_block_group_raid1(fs_info, cache, map, zone_info, active, last_alloc);
		break;
	case BTRFS_BLOCK_GROUP_RAID0:
		ret = btrfs_load_block_group_raid0(fs_info, cache, map, zone_info, active, last_alloc);
		break;
	case BTRFS_BLOCK_GROUP_RAID10:
		ret = btrfs_load_block_group_raid10(fs_info, cache, map, zone_info, active, last_alloc);
		break;
	case BTRFS_BLOCK_GROUP_RAID5:
	case BTRFS_BLOCK_GROUP_RAID6:
	default:
		error("zoned: profile %s not yet supported",
		      btrfs_bg_type_to_raid_name(map->type));
		ret = -EINVAL;
		goto out;
	}

out:
	/* An extent is allocated after the write pointer */
	if (!ret && num_conventional && last_alloc > cache->alloc_offset) {
		error(
		"zoned: got wrong write pointer in block group %llu: %llu > %llu",
		      logical, last_alloc, cache->alloc_offset);
		ret = -EIO;
	}

	if (!ret)
		cache->write_offset = cache->alloc_offset;

	kfree(zone_info);
	return ret;
}

bool btrfs_redirty_extent_buffer_for_zoned(struct btrfs_fs_info *fs_info,
					   u64 start, u64 end)
{
	u64 next;
	struct btrfs_block_group *cache;
	struct extent_buffer *eb;

	if (!btrfs_is_zoned(fs_info))
		return false;

	cache = btrfs_lookup_first_block_group(fs_info, start);
	BUG_ON(!cache);

	if (cache->start + cache->write_offset < start) {
		next = cache->start + cache->write_offset;
		BUG_ON(next + fs_info->nodesize > start);
		eb = btrfs_find_create_tree_block(fs_info, next);
		btrfs_mark_buffer_dirty(eb);
		free_extent_buffer(eb);
		return true;
	}

	cache->write_offset += (end + 1 - start);

	return false;
}

int btrfs_reset_chunk_zones(struct btrfs_fs_info *fs_info, u64 devid,
			    u64 offset, u64 length)
{
	struct btrfs_device *device;

	list_for_each_entry(device, &fs_info->fs_devices->devices, dev_list) {
		struct btrfs_zoned_device_info *zinfo;
		struct blk_zone *reset;

		if (device->devid != devid)
			continue;

		zinfo = device->zone_info;
		if (!zone_is_sequential(zinfo, offset))
			continue;

		reset = &zinfo->zones[offset / zinfo->zone_size];
		if (btrfs_reset_dev_zone(device->fd, reset)) {
			error("zoned: failed to reset zone %llu: %m",
			      offset / zinfo->zone_size);
			return -EIO;
		}
	}

	return 0;
}

int btrfs_wipe_temporary_sb(struct btrfs_fs_devices *fs_devices)
{
	struct list_head *head = &fs_devices->devices;
	struct btrfs_device *dev;
	int ret = 0;

	list_for_each_entry(dev, head, dev_list) {
		struct btrfs_zoned_device_info *zinfo = dev->zone_info;

		if (!zinfo)
			continue;

		ret = btrfs_reset_dev_zone(dev->fd, &zinfo->zones[0]);
		if (ret)
			break;
	}

	return ret;
}

bool btrfs_sb_zone_exists(struct btrfs_device *device, u64 bytenr)
{
	u32 zone_num = sb_bytenr_to_sb_zone(bytenr,
					    ilog2(device->zone_info->zone_size));

	return zone_num + 1 <= device->zone_info->nr_zones - 1;
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
	int ret;

	/*
	 * Cannot use btrfs_is_zoned here, since fs_info::zone_size might not
	 * yet be set.
	 */
	if (!btrfs_fs_incompat(fs_info, ZONED))
		return 0;

	if (device->zone_info)
		return 0;

	ret = btrfs_get_zone_info(device->fd, device->name, &device->zone_info);
	if (ret)
		return ret;

	if (device->zone_info->max_active_zones)
		fs_info->active_zone_tracking = 1;

	return 0;
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
		error_mem("zone information");
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

	if (btrfs_fs_incompat(fs_info, MIXED_GROUPS)) {
		error("zoned: mixed block groups not supported");
		ret = -EINVAL;
		goto out;
	}

	fs_info->zone_size = zone_size;
	fs_info->fs_devices->chunk_alloc_policy = BTRFS_CHUNK_ALLOC_ZONED;

out:
	return ret;
}
