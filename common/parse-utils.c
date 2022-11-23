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
#include <limits.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "libbtrfsutil/btrfsutil.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/compression.h"
#include "common/parse-utils.h"
#include "common/messages.h"
#include "common/utils.h"

int parse_u64(const char *str, u64 *result)
{
	char *endptr;
	u64 val;

	val = strtoull(str, &endptr, 10);
	if (*endptr)
		return 1;

	*result = val;
	return 0;
}

/*
 * Parse range that's missing some part that can be implicit:
 * a..b	- exact range, a can be equal to b
 * a..	- implicitly unbounded maximum (end == (u64)-1)
 * ..b	- implicitly starting at 0
 * a	- invalid; unclear semantics, use parse_u64 instead
 *
 * Returned values are u64, value validation and interpretation should be done
 * by the caller.
 */
int parse_range(const char *range, u64 *start, u64 *end)
{
	char *dots;
	char *endptr;
	const char *rest;
	int skipped = 0;

	dots = strstr(range, "..");
	if (!dots)
		return 1;

	rest = dots + 2;

	if (!*rest) {
		*end = (u64)-1;
		skipped++;
	} else {
		*end = strtoull(rest, &endptr, 10);
		if (*endptr)
			return 1;
	}
	if (dots == range) {
		*start = 0;
		skipped++;
	} else {
		*start = strtoull(range, &endptr, 10);
		if (*endptr != 0 && *endptr != '.')
			return 1;
	}

	if (*start > *end) {
		error("range %llu..%llu doesn't make sense",
			(unsigned long long)*start,
			(unsigned long long)*end);
		return 1;
	}

	if (skipped <= 1)
		return 0;

	return 1;
}

/*
 * Convert 64bit range to 32bit with boundary checks
 */
static int range_to_u32(u64 start, u64 end, u32 *start32, u32 *end32)
{
	if (start > (u32)-1)
		return 1;

	if (end != (u64)-1 && end > (u32)-1)
		return 1;

	*start32 = (u32)start;
	*end32 = (u32)end;

	return 0;
}

int parse_range_u32(const char *range, u32 *start, u32 *end)
{
	u64 tmp_start;
	u64 tmp_end;

	if (parse_range(range, &tmp_start, &tmp_end))
		return 1;

	if (range_to_u32(tmp_start, tmp_end, start, end))
		return 1;

	return 0;
}


/*
 * Parse range and check if start < end
 */
int parse_range_strict(const char *range, u64 *start, u64 *end)
{
	if (parse_range(range, start, end) == 0) {
		if (*start >= *end) {
			error("range %llu..%llu not allowed",
				(unsigned long long)*start,
				(unsigned long long)*end);
			return 1;
		}
		return 0;
	}

	return 1;
}

u64 parse_size_from_string(const char *s)
{
	char c;
	char *endptr;
	u64 mult = 1;
	u64 ret;

	if (!s) {
		error("size value is empty");
		exit(1);
	}
	if (s[0] == '-') {
		error("size value '%s' is less equal than 0", s);
		exit(1);
	}
	ret = strtoull(s, &endptr, 10);
	if (endptr == s) {
		error("size value '%s' is invalid", s);
		exit(1);
	}
	if (endptr[0] && endptr[1]) {
		error("illegal suffix contains character '%c' in wrong position",
			endptr[1]);
		exit(1);
	}
	/*
	 * strtoll returns LLONG_MAX when overflow, if this happens,
	 * need to call strtoull to get the real size
	 */
	if (errno == ERANGE && ret == ULLONG_MAX) {
		error("size value '%s' is too large for u64", s);
		exit(1);
	}
	if (endptr[0]) {
		c = tolower(endptr[0]);
		switch (c) {
		case 'e':
			mult *= 1024;
			/* fallthrough */
		case 'p':
			mult *= 1024;
			/* fallthrough */
		case 't':
			mult *= 1024;
			/* fallthrough */
		case 'g':
			mult *= 1024;
			/* fallthrough */
		case 'm':
			mult *= 1024;
			/* fallthrough */
		case 'k':
			mult *= 1024;
			/* fallthrough */
		case 'b':
			break;
		default:
			error("unknown size descriptor '%c'", c);
			exit(1);
		}
	}
	/* Check whether ret * mult overflow */
	if (fls64(ret) + fls64(mult) - 1 > 64) {
		error("size value '%s' is too large for u64", s);
		exit(1);
	}
	ret *= mult;
	return ret;
}

enum btrfs_csum_type parse_csum_type(const char *s)
{
	if (strcasecmp(s, "crc32c") == 0) {
		return BTRFS_CSUM_TYPE_CRC32;
	} else if (strcasecmp(s, "xxhash64") == 0 ||
		   strcasecmp(s, "xxhash") == 0) {
		return BTRFS_CSUM_TYPE_XXHASH;
	} else if (strcasecmp(s, "sha256") == 0) {
		return BTRFS_CSUM_TYPE_SHA256;
	} else if (strcasecmp(s, "blake2b") == 0 ||
		   strcasecmp(s, "blake2") == 0) {
		return BTRFS_CSUM_TYPE_BLAKE2;
	} else {
		error("unknown csum type %s", s);
		exit(1);
	}
	/* not reached */
	return 0;
}

/*
 * Parse name of the supported compression algorithm, without level, case
 * insensitive
 */
int parse_compress_type(const char *type)
{
	if (strcasecmp(type, "zlib") == 0)
		return BTRFS_COMPRESS_ZLIB;
	else if (strcasecmp(type, "lzo") == 0)
		return BTRFS_COMPRESS_LZO;
	else if (strcasecmp(type, "zstd") == 0)
		return BTRFS_COMPRESS_ZSTD;
	else
		return -EINVAL;
}

/*
 * Find last set bit in a 64-bit word. Returns 0 if value is 0 or the position
 * of the last set bit if value is nonzero. The last (most significant) bit is
 * at position 64.
 */
int fls64(u64 x)
{
	int i;

	for (i = 0; i < 64; i++)
		if (x << i & (1ULL << 63))
			return 64 - i;
	return 64 - i;
}

/*
 * Parse string description of block group profile and set that bit in @flags.
 * Return 1 if the profile is not valid, otherwise 0.
 *
 * String matched against btrfs_raid_array, case insensitive.
 */
int parse_bg_profile(const char *profile, u64 *flags)
{
	int i;

	for (i = 0; i < BTRFS_NR_RAID_TYPES; i++) {
		if (strcasecmp(btrfs_raid_array[i].upper_name, profile) == 0) {
			*flags |= btrfs_raid_array[i].bg_flag;
			return 0;
		}
	}
	return 1;
}

/*
 * Parse qgroupid of format LEVEL/ID, level and id are numerical, nothing must
 * follow after the last character of ID.
 */
int parse_qgroupid(const char *str, u64 *qgroupid)
{
	char *end = NULL;
	u64 level;
	u64 id;

	level = strtoull(str, &end, 10);
	if (str == end)
		return -EINVAL;
	if (end[0] != '/')
		return -EINVAL;
	str = end + 1;
	end = NULL;
	id = strtoull(str, &end, 10);
	if (str == end)
		return -EINVAL;
	if (end[0])
		return -EINVAL;
	if (id >= (1ULL << BTRFS_QGROUP_LEVEL_SHIFT))
		return -ERANGE;
	if (level >= (1ULL << (64 - BTRFS_QGROUP_LEVEL_SHIFT)))
		return -ERANGE;

	*qgroupid = (level << BTRFS_QGROUP_LEVEL_SHIFT) | id;
	return 0;
}

u64 parse_qgroupid_or_path(const char *p)
{
	enum btrfs_util_error err;
	u64 id;
	u64 qgroupid;
	int fd;
	int ret = 0;

	if (p[0] == '/')
		goto path;

	ret = parse_qgroupid(p, &qgroupid);
	if (ret < 0)
		goto err;

	return qgroupid;

path:
	/* Path format like subv at 'my_subvol' is the fallback case */
	err = btrfs_util_is_subvolume(p);
	if (err)
		goto err;
	fd = open(p, O_RDONLY);
	if (fd < 0)
		goto err;
	ret = lookup_path_rootid(fd, &id);
	if (ret) {
		errno = -ret;
		error("failed to lookup root id: %m");
	}
	close(fd);
	if (ret < 0)
		goto err;
	return id;

err:
	error("invalid qgroupid or subvolume path: %s", p);
	exit(-1);
}

