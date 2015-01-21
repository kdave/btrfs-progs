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

#ifndef __BTRFS_REPAIR_H__
#define __BTRFS_REPAIR_H__

struct btrfs_corrupt_block {
	struct cache_extent cache;
	struct btrfs_key key;
	int level;
};

int btrfs_add_corrupt_extent_record(struct btrfs_fs_info *info,
				    struct btrfs_key *first_key,
				    u64 start, u64 len, int level);

#endif
