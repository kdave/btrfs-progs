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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uuid/uuid.h>
#include "common/messages.h"
#include "common/parse-utils.h"
#include "common/reproducible.h"

time_t reproducible_now(void)
{
	const char *sde = getenv("SOURCE_DATE_EPOCH");
	u64 val;

	if (!sde || !*sde)
		return time(NULL);

	if (parse_u64(sde, &val) < 0 || val > (u64)LONG_MAX) {
		error("invalid SOURCE_DATE_EPOCH: '%s' is not a non-negative integer fitting in time_t",
		      sde);
		exit(1);
	}
	return (time_t)val;
}

bool reproducible_is_deterministic(void)
{
	const char *v = getenv("DETERMINISTIC_SEED");

	return v && strcmp(v, "1") == 0;
}

void reproducible_uuid_generate(const u8 fs_uuid[BTRFS_UUID_SIZE],
				enum reproducible_uuid_role role, u64 key,
				u8 out[BTRFS_UUID_SIZE])
{
	u8 name[sizeof(u32) + sizeof(u64)];

	if (!reproducible_is_deterministic()) {
		uuid_generate(out);
		return;
	}

	put_unaligned_le32((u32)role, name);
	put_unaligned_le64(key, name + sizeof(u32));
	uuid_generate_sha1(out, fs_uuid, (const char *)name, sizeof(name));
}
