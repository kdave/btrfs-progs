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


static void btrfs_uuid_to_key(const u8 *uuid, u64 *key_objectid,
			      u64 *key_offset)
{
	*key_objectid = get_unaligned_le64(uuid);
	*key_offset = get_unaligned_le64(uuid + sizeof(u64));
}


/* return -ENOENT for !found, < 0 for errors, or 0 if an item was found */
static int btrfs_uuid_tree_lookup_any(int fd, const u8 *uuid, u8 type,
				      u64 *subid)
{
	int ret;
	u64 key_objectid = 0;
	u64 key_offset;
	struct btrfs_ioctl_search_args search_arg;
	struct btrfs_ioctl_search_header *search_header;
	u32 item_size;

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
			"ioctl(BTRFS_IOC_TREE_SEARCH, uuid, key %016llx, UUID_KEY, %016llx) ret=%d, error: %s\n",
			(unsigned long long)key_objectid,
			(unsigned long long)key_offset, ret, strerror(errno));
		ret = -ENOENT;
		goto out;
	}

	if (search_arg.key.nr_items < 1) {
		ret = -ENOENT;
		goto out;
	}
	search_header = (struct btrfs_ioctl_search_header *)(search_arg.buf);
	item_size = search_header->len;
	if ((item_size & (sizeof(u64) - 1)) || item_size == 0) {
		printf("btrfs: uuid item with illegal size %lu!\n",
		       (unsigned long)item_size);
		ret = -ENOENT;
		goto out;
	} else {
		ret = 0;
	}

	/* return first stored id */
	memcpy(subid, search_header + 1, sizeof(*subid));
	*subid = le64_to_cpu(*subid);

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
