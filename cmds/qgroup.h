/*
 * Copyright (C) 2012 STRATO.  All rights reserved.
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

#ifndef __CMDS_QGROUP_H__
#define __CMDS_QGROUP_H__

#include "kerncompat.h"
#include "kernel-shared/uapi/btrfs.h"

struct btrfs_qgroup_info {
	u64 generation;
	u64 referenced;
	u64 referenced_compressed;
	u64 exclusive;
	u64 exclusive_compressed;
};

struct btrfs_qgroup_stats {
	u64 qgroupid;
	struct btrfs_qgroup_info info;
	struct btrfs_qgroup_limit limit;
};

int btrfs_qgroup_query(int fd, u64 qgroupid, struct btrfs_qgroup_stats *stats);

#endif
