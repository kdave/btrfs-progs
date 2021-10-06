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

#include <sys/ioctl.h>
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
/* Invalid allocation pointer value for missing devices */
#define WP_MISSING_DEV			((u64)-1)
/* Pseudo write pointer value for conventional zone */
#define WP_CONVENTIONAL			((u64)-2)

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
	if (zoned_model(file) == ZONED_NONE)
		return EMULATED_ZONE_SIZE;

	ret = device_get_queue_param(file, "chunk_sectors", chunk, sizeof(chunk));
	if (ret <= 0)
		return 0;

	return strtoull((const char *)chunk, NULL, 10) << SECTOR_SHIFT;
}

u64 max_zone_append_size(const char *file)
{
	char chunk[32];
	int ret;

	ret = device_get_queue_param(file, "zone_append_max_bytes", chunk,
				     sizeof(chunk));
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
	struct stat st;
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

	ret = fstat(fd, &st);
	if (ret < 0) {
		error("error when reading zone info on %s: %m", file);
		return -EIO;
	}

	device_size = btrfs_device_size(fd, &st);
	if (device_size == 0) {
		error("zoned: failed to read size of %s: %m", file);
		exit(1);
	}

	/* Allocate the zone information array */
	zinfo->zone_size = zone_bytes;
	zinfo->nr_zones = device_size / zone_bytes;
	if (device_size & (zone_bytes - 1))
		zinfo->nr_zones++;

	if (zoned_model(file) != ZONED_NONE && max_zone_append_size(file) == 0) {
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

/*
 * Discard blocks in the zones of a zoned block device. Process this with zone
 * size granularity so that blocks in conventional zones are discarded using
 * discard_range and blocks in sequential zones are reset though a zone reset.
 */
int btrfs_reset_all_zones(int fd, struct btrfs_zoned_device_info *zinfo)
{
	unsigned int i;
	int ret = 0;

	ASSERT(zinfo);

	/* Zone size granularity */
	for (i = 0; i < zinfo->nr_zones; i++) {
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
		free(rep);
		return 0;
	}

	zones = (struct blk_zone *)(rep + 1);

	ret = sb_log_location(fd, zones, rw, &mapped);
	free(rep);
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
		error("failed to synchronize superblock: %s", strerror(errno));
		exit(1);
	}

	return ret_sz;
}

/*
 * Check if spcecifeid region is suitable for allocation
 *
 * @device:	the device to allocate a region
 * @pos:	the position of the region
 * @num_bytes:	the size of the region
 *
 * In non-ZONED device, anywhere is suitable for allocation. In ZONED
 * device, check if:
 * 1) the region is not on non-empty sequential zones,
 * 2) all zones in the region have the same zone type,
 * 3) it does not contain super block location
 */
bool btrfs_check_allocatable_zones(struct btrfs_device *device, u64 pos,
				   u64 num_bytes)
{
	struct btrfs_zoned_device_info *zinfo = device->zone_info;
	u64 nzones, begin, end;
	u64 sb_pos;
	bool is_sequential;
	int shift;
	int i;

	if (!zinfo || zinfo->model == ZONED_NONE)
		return true;

	nzones = num_bytes / zinfo->zone_size;
	begin = pos / zinfo->zone_size;
	end = begin + nzones;

	ASSERT(IS_ALIGNED(pos, zinfo->zone_size));
	ASSERT(IS_ALIGNED(num_bytes, zinfo->zone_size));

	if (end > zinfo->nr_zones)
		return false;

	shift = ilog2(zinfo->zone_size);
	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		sb_pos = sb_zone_number(shift, i);
		if (!(end < sb_pos || sb_pos + 1 < begin))
			return false;
	}

	is_sequential = btrfs_dev_is_sequential(device, pos);

	while (num_bytes) {
		if (is_sequential && !btrfs_dev_is_empty_zone(device, pos))
			return false;
		if (is_sequential != btrfs_dev_is_sequential(device, pos))
			return false;

		pos += zinfo->zone_size;
		num_bytes -= zinfo->zone_size;
	}

	return true;
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
	struct btrfs_root *root = fs_info->extent_root;
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
	       found_key.objectid + length <= cache->start + cache->length)) {
		ret = -EUCLEAN;
		goto out;
	}
	*offset_ret = found_key.objectid + length - cache->start;
	ret = 0;

out:
	btrfs_free_path(path);
	return ret;
}

static bool profile_supported(u64 flags)
{
	flags &= BTRFS_BLOCK_GROUP_PROFILE_MASK;

	/* SINGLE */
	if (flags == 0)
		return true;
	/* non-single profiles are not supported yet */
	return false;
}

int btrfs_load_block_group_zone_info(struct btrfs_fs_info *fs_info,
				     struct btrfs_block_group *cache)
{
	struct btrfs_device *device;
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	struct cache_extent *ce;
	struct map_lookup *map;
	u64 logical = cache->start;
	u64 length = cache->length;
	u64 physical = 0;
	int ret = 0;
	int i;
	u64 *alloc_offsets = NULL;
	u64 last_alloc = 0;
	u32 num_sequential = 0, num_conventional = 0;

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

	alloc_offsets = calloc(map->num_stripes, sizeof(*alloc_offsets));
	if (!alloc_offsets) {
		error("zoned: failed to allocate alloc_offsets");
		return -ENOMEM;
	}

	for (i = 0; i < map->num_stripes; i++) {
		bool is_sequential;
		struct blk_zone zone;

		device = map->stripes[i].dev;
		physical = map->stripes[i].physical;

		if (device->fd == -1) {
			alloc_offsets[i] = WP_MISSING_DEV;
			continue;
		}

		is_sequential = btrfs_dev_is_sequential(device, physical);
		if (is_sequential)
			num_sequential++;
		else
			num_conventional++;

		if (!is_sequential) {
			alloc_offsets[i] = WP_CONVENTIONAL;
			continue;
		}

		/*
		 * The group is mapped to a sequential zone. Get the zone write
		 * pointer to determine the allocation offset within the zone.
		 */
		WARN_ON(!IS_ALIGNED(physical, fs_info->zone_size));
		zone = device->zone_info->zones[physical / fs_info->zone_size];

		switch (zone.cond) {
		case BLK_ZONE_COND_OFFLINE:
		case BLK_ZONE_COND_READONLY:
			error(
		"zoned: offline/readonly zone %llu on device %s (devid %llu)",
			      physical / fs_info->zone_size, device->name,
			      device->devid);
			alloc_offsets[i] = WP_MISSING_DEV;
			break;
		case BLK_ZONE_COND_EMPTY:
			alloc_offsets[i] = 0;
			break;
		case BLK_ZONE_COND_FULL:
			alloc_offsets[i] = fs_info->zone_size;
			break;
		default:
			/* Partially used zone */
			alloc_offsets[i] =
					((zone.wp - zone.start) << SECTOR_SHIFT);
			break;
		}
	}

	if (num_conventional > 0) {
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

	if (!profile_supported(map->type)) {
		error("zoned: profile %s not yet supported",
		      btrfs_group_profile_str(map->type));
		ret = -EINVAL;
		goto out;
	}
	cache->alloc_offset = alloc_offsets[0];

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

	free(alloc_offsets);
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
