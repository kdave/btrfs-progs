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
#ifndef SEND_UTILS_H_
#define SEND_UTILS_H_

#if BTRFS_FLAT_INCLUDES
#include "ctree.h"
#include "rbtree.h"
#else
#include <btrfs/ctree.h>
#include <btrfs/rbtree.h>
#endif /* BTRFS_FLAT_INCLUDES */

#ifdef __cplusplus
extern "C" {
#endif

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

struct subvol_uuid_search {
	int mnt_fd;
};

int subvol_uuid_search_init(int mnt_fd, struct subvol_uuid_search *s);
void subvol_uuid_search_finit(struct subvol_uuid_search *s);
struct subvol_info *subvol_uuid_search(struct subvol_uuid_search *s,
				       u64 root_id, const u8 *uuid, u64 transid,
				       const char *path,
				       enum subvol_search_type type);
void subvol_uuid_search_add(struct subvol_uuid_search *s,
			    struct subvol_info *si);

int btrfs_subvolid_resolve(int fd, char *path, size_t path_len, u64 subvol_id);

char *path_cat(const char *p1, const char *p2);
char *path_cat3(const char *p1, const char *p2, const char *p3);

#ifdef __cplusplus
}
#endif

#endif /* SEND_UTILS_H_ */
