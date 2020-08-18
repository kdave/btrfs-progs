/*
 * Copyright (C) 2012 Oracle.  All rights reserved.
 *
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

#include "kernel-shared/ctree.h"
#include "common/extent-cache.h"
#include "common/utils.h"
#include "repair.h"

int repair = 0;

int btrfs_add_corrupt_extent_record(struct btrfs_fs_info *info,
				    struct btrfs_key *first_key,
				    u64 start, u64 len, int level)

{
	int ret = 0;
	struct btrfs_corrupt_block *corrupt;

	if (!info->corrupt_blocks)
		return 0;

	corrupt = malloc(sizeof(*corrupt));
	if (!corrupt)
		return -ENOMEM;

	memcpy(&corrupt->key, first_key, sizeof(*first_key));
	corrupt->cache.start = start;
	corrupt->cache.size = len;
	corrupt->level = level;

	ret = insert_cache_extent(info->corrupt_blocks, &corrupt->cache);
	if (ret)
		free(corrupt);
	BUG_ON(ret && ret != -EEXIST);
	return ret;
}

