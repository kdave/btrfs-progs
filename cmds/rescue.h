/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 SUSE.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef __BTRFS_RESCUE_H__
#define __BTRFS_RESCUE_H__

int btrfs_recover_superblocks(const char *path, int verbose, int yes);
int btrfs_recover_chunk_tree(const char *path, int verbose, int yes);

#endif
