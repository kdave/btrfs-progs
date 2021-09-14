/*
 * Copyright (C) STRATO AG 2013.  All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <uuid/uuid.h>
#include <sys/ioctl.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "common/utils.h"

void btrfs_uuid_to_key(const u8 *uuid, struct btrfs_key *key)
{
	u64 tmp;

	tmp = get_unaligned_le64(uuid);
	put_unaligned_64(tmp, &key->objectid);
	tmp = get_unaligned_le64(uuid + sizeof(u64));
	put_unaligned_64(tmp, &key->offset);
}

/*
 * Search uuid tree - mounted
 *
 * return -ENOENT for !found, < 0 for errors, or 0 if an item was found
 */
static int btrfs_uuid_tree_lookup_any(int fd, const u8 *uuid, u8 type,
				      u64 *subid)
{
	int ret;
	struct btrfs_ioctl_search_args search_arg;
	struct btrfs_ioctl_search_header *search_header;
	u32 item_size;
	__le64 lesubid;
	struct btrfs_key key;

	key.type = type;
	btrfs_uuid_to_key(uuid, &key);

	memset(&search_arg, 0, sizeof(search_arg));
	search_arg.key.tree_id = BTRFS_UUID_TREE_OBJECTID;
	search_arg.key.min_objectid = key.objectid;
	search_arg.key.max_objectid = key.objectid;
	search_arg.key.min_type = type;
	search_arg.key.max_type = type;
	search_arg.key.min_offset = key.offset;
	search_arg.key.max_offset = key.offset;
	search_arg.key.max_transid = (u64)-1;
	search_arg.key.nr_items = 1;
	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search_arg);
	if (ret < 0) {
		fprintf(stderr,
			"ioctl(BTRFS_IOC_TREE_SEARCH, uuid, key %016llx, UUID_KEY, %016llx) ret=%d, error: %m\n",
			(unsigned long long)key.objectid,
			(unsigned long long)key.offset, ret);
		ret = -ENOENT;
		goto out;
	}

	if (search_arg.key.nr_items < 1) {
		ret = -ENOENT;
		goto out;
	}
	search_header = (struct btrfs_ioctl_search_header *)(search_arg.buf);
	item_size = btrfs_search_header_len(search_header);
	if ((item_size & (sizeof(u64) - 1)) || item_size == 0) {
		printf("btrfs: uuid item with illegal size %lu!\n",
		       (unsigned long)item_size);
		ret = -ENOENT;
		goto out;
	} else {
		ret = 0;
	}

	/* return first stored id */
	memcpy(&lesubid, search_header + 1, sizeof(lesubid));
	*subid = le64_to_cpu(lesubid);

out:
	return ret;
}

int btrfs_lookup_uuid_subvol_item(int fd, const u8 *uuid, u64 *subvol_id)
{
	return btrfs_uuid_tree_lookup_any(fd, uuid, BTRFS_UUID_KEY_SUBVOL,
					  subvol_id);
}

int btrfs_lookup_uuid_received_subvol_item(int fd, const u8 *uuid,
					   u64 *subvol_id)
{
	return btrfs_uuid_tree_lookup_any(fd, uuid,
					  BTRFS_UUID_KEY_RECEIVED_SUBVOL,
					  subvol_id);
}

int btrfs_uuid_tree_remove(struct btrfs_trans_handle *trans, u8 *uuid, u8 type,
		u64 subid)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *uuid_root = fs_info->uuid_root;
	int ret;
	struct btrfs_path *path = NULL;
	struct btrfs_key key;
	struct extent_buffer *eb;
	int slot;
	unsigned long offset;
	u32 item_size;
	unsigned long move_dst;
	unsigned long move_src;
	unsigned long move_len;

	if (!uuid_root) {
		ret = -EINVAL;
		goto out;
	}

	btrfs_uuid_to_key(uuid, &key);
	key.type = type;

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	ret = btrfs_search_slot(trans, uuid_root, &key, path, -1, 1);
	if (ret < 0) {
		warning("error %d while searching for uuid item!", ret);
		goto out;
	}
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	eb = path->nodes[0];
	slot = path->slots[0];
	offset = btrfs_item_ptr_offset(eb, slot);
	item_size = btrfs_item_size_nr(eb, slot);
	if (!IS_ALIGNED(item_size, sizeof(u64))) {
		warning("uuid item with illegal size %u!", item_size);
		ret = -ENOENT;
		goto out;
	}
	while (item_size) {
		__le64 read_subid;

		read_extent_buffer(eb, &read_subid, offset, sizeof(read_subid));
		if (le64_to_cpu(read_subid) == subid)
			break;
		offset += sizeof(read_subid);
		item_size -= sizeof(read_subid);
	}

	if (!item_size) {
		ret = -ENOENT;
		goto out;
	}

	item_size = btrfs_item_size_nr(eb, slot);
	if (item_size == sizeof(subid)) {
		ret = btrfs_del_item(trans, uuid_root, path);
		goto out;
	}

	move_dst = offset;
	move_src = offset + sizeof(subid);
	move_len = item_size - (move_src - btrfs_item_ptr_offset(eb, slot));
	memmove_extent_buffer(eb, move_dst, move_src, move_len);
	btrfs_truncate_item(path, item_size - sizeof(subid), 1);

out:
	btrfs_free_path(path);
	return ret;
}
