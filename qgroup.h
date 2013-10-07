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

#ifndef _BTRFS_QGROUP_H
#define _BTRFS_QGROUP_H

#include "ioctl.h"
#include "kerncompat.h"

enum btrfs_qgroup_column_enum {
	BTRFS_QGROUP_QGROUPID,
	BTRFS_QGROUP_RFER,
	BTRFS_QGROUP_EXCL,
	BTRFS_QGROUP_PARENT,
	BTRFS_QGROUP_CHILD,
	BTRFS_QGROUP_ALL,
};

int  btrfs_show_qgroups(int fd);
void btrfs_qgroup_setup_print_column(enum btrfs_qgroup_column_enum column);

u64 parse_qgroupid(char *p);
int qgroup_inherit_size(struct btrfs_qgroup_inherit *p);
int qgroup_inherit_add_group(struct btrfs_qgroup_inherit **inherit, char *arg);
int qgroup_inherit_add_copy(struct btrfs_qgroup_inherit **inherit, char *arg,
			    int type);

#endif
