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

#if BTRFS_FLAT_INCLUDES
#include "libbtrfs/kerncompat.h"
#include <stddef.h>
#include "libbtrfs/ctree.h"
#include "kernel-lib/rbtree_types.h"
#else
#include <btrfs/kerncompat.h>
#include <stddef.h>
#include <btrfs/ctree.h>
#include <btrfs/rbtree_types.h>
#endif /* BTRFS_FLAT_INCLUDES */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compatibility code for kernels < 3.12; the UUID tree is not available there
 * and we have to do the slow search. This should be deprecated someday.
 */
#define BTRFS_COMPAT_SEND_NO_UUID_TREE 1

enum subvol_search_type {
	subvol_search_by_root_id,
	subvol_search_by_uuid,
	subvol_search_by_received_uuid,
	subvol_search_by_path,
};

struct subvol_info {
#ifdef BTRFS_COMPAT_SEND_NO_UUID_TREE
	struct rb_node rb_root_id_node;
	struct rb_node rb_local_node;
	struct rb_node rb_received_node;
	struct rb_node rb_path_node;
#endif

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

struct subvol_uuid_search {
	int mnt_fd;
#ifdef BTRFS_COMPAT_SEND_NO_UUID_TREE
	int uuid_tree_existed;

	struct rb_root root_id_subvols;
	struct rb_root local_subvols;
	struct rb_root received_subvols;
	struct rb_root path_subvols;
#endif
};

int subvol_uuid_search_init(int mnt_fd, struct subvol_uuid_search *s);
/*
 * Search for a subvolume by given type (received uuid, root id, path), returns
 * pointer to newly allocated struct subvol_info or NULL in case it's not found
 * or there was another error.
 */
struct subvol_info *subvol_uuid_search(struct subvol_uuid_search *s,
				       u64 root_id, const u8 *uuid, u64 transid,
				       const char *path,
				       enum subvol_search_type type);
int btrfs_subvolid_resolve(int fd, char *path, size_t path_len, u64 subvol_id);

#ifdef __cplusplus
}
#endif

#endif
