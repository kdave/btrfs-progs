/*
 * Copyright (C) 2016 Fujitsu.  All rights reserved.
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

#ifndef __BTRFS_SEND_DUMP_H__
#define __BTRFS_SEND_DUMP_H__

#include <linux/limits.h>

struct btrfs_dump_send_args {
	char full_subvol_path[PATH_MAX];
	char root_path[PATH_MAX];
};

extern struct btrfs_send_ops btrfs_print_send_ops;

#endif
