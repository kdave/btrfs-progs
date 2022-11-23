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
#include "ioctl.h"

struct btrfs_qgroup_info {
	u64 generation;
	u64 rfer;
	u64 rfer_cmpr;
	u64 excl;
	u64 excl_cmpr;
};

struct btrfs_qgroup_stats {
	u64 qgroupid;
	struct btrfs_qgroup_info info;
	struct btrfs_qgroup_limit limit;
};

int btrfs_qgroup_inherit_size(struct btrfs_qgroup_inherit *p);
int btrfs_qgroup_inherit_add_group(struct btrfs_qgroup_inherit **inherit, char *arg);
int btrfs_qgroup_inherit_add_copy(struct btrfs_qgroup_inherit **inherit, char *arg,
			    int type);
int btrfs_qgroup_query(int fd, u64 qgroupid, struct btrfs_qgroup_stats *stats);

#endif
