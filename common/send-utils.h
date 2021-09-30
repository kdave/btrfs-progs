/*
 * Copyright (C) 2012 Alexander Block.  All rights reserved.
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

#ifndef __BTRFS_SEND_UTILS_H__
#define __BTRFS_SEND_UTILS_H__

#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "kernel-lib/rbtree.h"

enum subvol_search_type {
	subvol_search_by_root_id,
	subvol_search_by_uuid,
	subvol_search_by_received_uuid,
	subvol_search_by_path,
};

struct subvol_info {
	u64 root_id;
	u8 uuid[BTRFS_UUID_SIZE];
	u8 parent_uuid[BTRFS_UUID_SIZE];
	u8 received_uuid[BTRFS_UUID_SIZE];
	u64 ctransid;
	u64 otransid;
	u64 stransid;
	u64 rtransid;

	char *path;
};

/*
 * Search for a subvolume by given type (received uuid, root id, path), returns
 * pointer to newly allocated struct subvol_info or NULL in case it's not found
 * or there was another error.
 */
struct subvol_info *subvol_uuid_search(int mnt_fd,
				       u64 root_id, const u8 *uuid, u64 transid,
				       const char *path,
				       enum subvol_search_type type);

int btrfs_subvolid_resolve(int fd, char *path, size_t path_len, u64 subvol_id);

#endif
