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

/*
 * Defines and function declarations for users of the mkfs API, no internal
 * defintions.
 */

#ifndef __BTRFS_MKFS_COMMON_H__
#define __BTRFS_MKFS_COMMON_H__

#include "kerncompat.h"
#include "common-defs.h"

struct btrfs_mkfs_config {
	char *label;
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE];
	char chunk_uuid[BTRFS_UUID_UNPARSED_SIZE];
	u64 blocks[8];
	u64 num_bytes;
	u32 nodesize;
	u32 sectorsize;
	u32 stripesize;
	u64 features;

	/* Super bytenr after make_btrfs */
	u64 super_bytenr;
};

int make_btrfs(int fd, struct btrfs_mkfs_config *cfg);

#endif
