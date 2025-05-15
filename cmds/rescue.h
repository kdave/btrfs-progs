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
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __BTRFS_RESCUE_H__
#define __BTRFS_RESCUE_H__

enum btrfs_fix_data_checksum_mode {
	BTRFS_FIX_DATA_CSUMS_READONLY,
	BTRFS_FIX_DATA_CSUMS_INTERACTIVE,
	BTRFS_FIX_DATA_CSUMS_UPDATE_CSUM_ITEM,
	BTRFS_FIX_DATA_CSUMS_LAST,
};

int btrfs_recover_superblocks(const char *path, int yes);
int btrfs_recover_chunk_tree(const char *path, int yes);
int btrfs_recover_fix_data_checksum(const char *path,
				    enum btrfs_fix_data_checksum_mode mode,
				    unsigned int mirror);

#endif
