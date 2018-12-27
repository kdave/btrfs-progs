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
#include "ctree.h"
#include "transaction.h"
#include "disk-io.h"
#include "print-tree.h"
#include "utils.h"


static void btrfs_uuid_to_key(const u8 *uuid, u64 *key_objectid,
			      u64 *key_offset)
{
	*key_objectid = get_unaligned_le64(uuid);
	*key_offset = get_unaligned_le64(uuid + sizeof(u64));
}

/*
 * Search uuid tree of a *MOUNTED* btrfs (online)
 *
 * return -ENOENT for !found, < 0 for errors, or 0 if an item was found
 */
static int btrfs_uuid_tree_lookup_any(int fd, const u8 *uuid, u8 type,
				      u64 *subid)
{
	int ret;
	u64 key_objectid = 0;
	u64 key_offset;
	struct btrfs_ioctl_search_args search_arg;
	struct btrfs_ioctl_search_header *search_header;
	u32 item_size;
	__le64 lesubid;

	btrfs_uuid_to_key(uuid, &key_objectid, &key_offset);

	memset(&search_arg, 0, sizeof(search_arg));
	search_arg.key.tree_id = BTRFS_UUID_TREE_OBJECTID;
	search_arg.key.min_objectid = key_objectid;
	search_arg.key.max_objectid = key_objectid;
	search_arg.key.min_type = type;
	search_arg.key.max_type = type;
	search_arg.key.min_offset = key_offset;
	search_arg.key.max_offset = key_offset;
	search_arg.key.max_transid = (u64)-1;
	search_arg.key.nr_items = 1;
	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search_arg);
	if (ret < 0) {
		fprintf(stderr,
			"ioctl(BTRFS_IOC_TREE_SEARCH, uuid, key %016llx, UUID_KEY, %016llx) ret=%d, error: %m\n",
			(unsigned long long)key_objectid,
			(unsigned long long)key_offset, ret);
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

/*
 * Search uuid tree of an *UNMOUNTED* btrfs (offline)
 *
 * return -ENOENT for !found, < 0 for errors, or 0 if an item was found
 */
static int btrfs_uuid_tree_lookup(struct btrfs_root *uuid_root, u8 *uuid,
				  u8 type, u64 subid)
{
	int ret;
	struct btrfs_path *path = NULL;
	struct extent_buffer *eb;
	int slot;
	u32 item_size;
	unsigned long offset;
	struct btrfs_key key;

	if (!uuid_root) {
		ret = -ENOENT;
		goto out;
	}

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	btrfs_uuid_to_key(uuid, &key.objectid, &key.offset);
	key.type = type;
	ret = btrfs_search_slot(NULL, uuid_root, &key, path, 0, 0);
	if (ret < 0) {
		goto out;
	} else if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	eb = path->nodes[0];
	slot = path->slots[0];
	item_size = btrfs_item_size_nr(eb, slot);
	offset = btrfs_item_ptr_offset(eb, slot);
	ret = -ENOENT;

	if (!IS_ALIGNED(item_size, sizeof(u64))) {
		warning("uuid item with illegal size %lu!",
			(unsigned long)item_size);
		goto out;
	}
	while (item_size) {
		__le64 data;

		read_extent_buffer(eb, &data, offset, sizeof(data));
		if (le64_to_cpu(data) == subid) {
			ret = 0;
			break;
		}
		offset += sizeof(data);
		item_size -= sizeof(data);
	}

out:
	btrfs_free_path(path);
	return ret;
}

int btrfs_uuid_tree_add(struct btrfs_trans_handle *trans, u8 *uuid, u8 type,
			u64 subid_cpu)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *uuid_root = fs_info->uuid_root;
	int ret;
	struct btrfs_path *path = NULL;
	struct btrfs_key key;
	struct extent_buffer *eb;
	int slot;
	unsigned long offset;
	__le64 subid_le;

	if (!uuid_root) {
		warning("%s: uuid root is not initialized", __func__);
		return -EINVAL;
	}

	ret = btrfs_uuid_tree_lookup(uuid_root, uuid, type, subid_cpu);
	if (ret != -ENOENT)
		return ret;

	key.type = type;
	btrfs_uuid_to_key(uuid, &key.objectid, &key.offset);

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	ret = btrfs_insert_empty_item(trans, uuid_root, path, &key,
				      sizeof(subid_le));
	if (ret >= 0) {
		/* Add an item for the type for the first time */
		eb = path->nodes[0];
		slot = path->slots[0];
		offset = btrfs_item_ptr_offset(eb, slot);
	} else if (ret == -EEXIST) {
		/*
		 * An item with that type already exists.
		 * Extend the item and store the new subid at the end.
		 */
		btrfs_extend_item(uuid_root, path, sizeof(subid_le));
		eb = path->nodes[0];
		slot = path->slots[0];
		offset = btrfs_item_ptr_offset(eb, slot);
		offset += btrfs_item_size_nr(eb, slot) - sizeof(subid_le);
	} else if (ret < 0) {
		warning("insert uuid item failed %d (0x%016llx, 0x%016llx) type %u!",
			ret, (unsigned long long)key.objectid,
			(unsigned long long)key.offset, type);
		goto out;
	}

	ret = 0;
	subid_le = cpu_to_le64(subid_cpu);
	write_extent_buffer(eb, &subid_le, offset, sizeof(subid_le));
	btrfs_mark_buffer_dirty(eb);

out:
	btrfs_free_path(path);
	return ret;
}
