/*
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

#ifndef __BTRFS_UUID_TREE_H__
#define __BTRFS_UUID_TREE_H__

#include "kerncompat.h"

struct btrfs_key;
struct btrfs_trans_handle;

/* uuid-tree.c, interface for mounted mounted filesystem */
int btrfs_lookup_uuid_subvol_item(int fd, const u8 *uuid, u64 *subvol_id);
int btrfs_lookup_uuid_received_subvol_item(int fd, const u8 *uuid,
					   u64 *subvol_id);

/* uuid-tree.c, interface for unmounte filesystem */
int btrfs_uuid_tree_remove(struct btrfs_trans_handle *trans, u8 *uuid, u8 type,
			   u64 subid);
void btrfs_uuid_to_key(const u8 *uuid, u8 type, struct btrfs_key *key);
int btrfs_uuid_tree_add(struct btrfs_trans_handle *trans, const u8 *uuid, u8 type,
			u64 subid_cpu);

#endif
