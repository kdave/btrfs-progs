/*
 * Copyright (C) 2014 SUSE.  All rights reserved.
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

#ifndef __BTRFS_QGROUP_VERIFY_H__
#define __BTRFS_QGROUP_VERIFY_H__

#include "kerncompat.h"
#include "kernel-shared/ctree.h"

int qgroup_verify_all(struct btrfs_fs_info *info);
void report_qgroups(int all);
int repair_qgroups(struct btrfs_fs_info *info, int *repaired, bool silent);

int print_extent_state(struct btrfs_fs_info *info, u64 subvol);

void free_qgroup_counts(void);

void qgroup_set_item_count_ptr(u64 *item_count_ptr);

#endif
