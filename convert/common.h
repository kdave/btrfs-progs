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
 * definitions.
 */

#ifndef __BTRFS_CONVERT_COMMON_H__
#define __BTRFS_CONVERT_COMMON_H__

#include "kerncompat.h"
#include "common/extent-cache.h"

struct btrfs_mkfs_config;

#define SOURCE_FS_UUID_SIZE	(16)

struct btrfs_convert_context {
	u32 blocksize;
	u64 first_data_block;
	u64 block_count;
	u64 inodes_count;
	u64 free_inodes_count;
	u64 total_bytes;
	u64 free_bytes_initial;
	char *label;
	u8 fs_uuid[SOURCE_FS_UUID_SIZE];
	const struct btrfs_convert_operations *convert_ops;

	/* The accurate used space of old filesystem */
	struct cache_tree used_space;

	/* Batched ranges which must be covered by data chunks */
	struct cache_tree data_chunks;

	/* Free space which is not covered by data_chunks */
	struct cache_tree free_space;

	/*
	 * Free space reserved for ENOSPC report, it's just a copy free_space.
	 * But after initial calculation, free_space_initial is no longer
	 * updated, so we have a good idea on how much free space we really
	 * have for btrfs.
	 */
	struct cache_tree free_space_initial;
	void *fs_data;
};

int make_convert_btrfs(int fd, struct btrfs_mkfs_config *cfg,
			      struct btrfs_convert_context *cctx);

/*
 * Represents a simple contiguous range.
 *
 * For multiple or non-contiguous ranges, use extent_cache_tree from
 * extent-cache.c
 */
struct simple_range {
	u64 start;
	u64 len;
};

/*
 * Simple range functions
 */

/* Get range end (exclusive) */
static inline u64 range_end(const struct simple_range *range)
{
	return (range->start + range->len);
}

#endif
