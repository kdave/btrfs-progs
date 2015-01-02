/*
 * Copyright (C) 2014 Fujitsu.  All rights reserved.
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

#include <sys/stat.h>
#include "ctree.h"
#include "transaction.h"
#include "kerncompat.h"

/*
 * Get the first file extent that covers (part of) the given range
 * Unlike kernel using extent_map to handle hole even no-hole is enabled,
 * progs don't have such infrastructure, so caller should do extra care
 * for no-hole.
 *
 * return 0 for found, and path points to the file extent.
 * return >0 for not found, and path points to the insert position.
 * return <0 for error.
 */
int btrfs_get_extent(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root,
		     struct btrfs_path *path,
		     u64 ino, u64 offset, u64 len, int ins_len)
{
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_file_extent_item *fi_item;
	u64 end = 0;
	int ret = 0;
	int not_found = 1;

	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = offset;

	ret = btrfs_search_slot(trans, root, &key, path, ins_len,
				ins_len ? 1 : 0);
	if (ret <= 0)
		goto out;
	if (ret > 0) {
		/* Check preivous file extent */
		ret = btrfs_previous_item(root, path, ino,
					  BTRFS_EXTENT_DATA_KEY);
		if (ret < 0)
			goto out;
		if (ret > 0)
			goto check_next;
	}
	btrfs_item_key_to_cpu(path->nodes[0], &found_key, path->slots[0]);
	if (found_key.objectid != ino ||
	    found_key.type != BTRFS_EXTENT_DATA_KEY)
		goto check_next;

	fi_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				 struct btrfs_file_extent_item);
	end = found_key.offset +
	      btrfs_file_extent_ram_bytes(path->nodes[0], fi_item);
	/*
	 * existing file extent
	 * |--------|	  |----|
	 *      |-------|
	 *      offset + len
	 * OR
	 * |---------------|
	 *	|-------|
	 */
	if (end > offset) {
		not_found = 0;
		goto out;
	}
check_next:
	ret = btrfs_next_item(root, path);
	if (ret)
		goto out;

	btrfs_item_key_to_cpu(path->nodes[0], &found_key, path->slots[0]);
	if (found_key.objectid != ino ||
	    found_key.type != BTRFS_EXTENT_DATA_KEY) {
		ret = 1;
		goto out;
	}
	if (found_key.offset < offset + len)
		/*
		 * existing file extent
		 * |---|	|------|
		 *	|-------|
		 *	offset + len
		 */
		not_found = 0;
	else
		/*
		 * existing file extent
		 * |----|		|----|
		 *		|----|
		 *		offset + len
		 */
		not_found = 1;

	/*
	 * To keep the search hehavior consistent with search_slot(),
	 * we need to go back to the prev leaf's nritem slot if
	 * we are at the first slot of the leaf.
	 */
	if (path->slots[0] == 0) {
		ret = btrfs_prev_leaf(root, path);
		/* Not possible */
		if (ret)
			goto out;
		path->slots[0] = btrfs_header_nritems(path->nodes[0]);
	}

out:
	if (ret == 0)
		ret = not_found;
	return ret;
}

/*
 * Punch hole ranged [offset,len) for the file given by ino and root.
 *
 * Unlink kernel punch_hole, which will not zero/free existing extent,
 * instead it will return -EEXIST if there is any extents in the hole
 * range.
 */
int btrfs_punch_hole(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root,
		     u64 ino, u64 offset, u64 len)
{
	struct btrfs_path *path;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_get_extent(NULL, root, path, ino, offset, len, 0);
	if (ret < 0)
		goto out;
	if (ret == 0) {
		ret = -EEXIST;
		goto out;
	}

	ret = btrfs_insert_file_extent(trans, root, ino, offset, 0, 0, len);
out:
	btrfs_free_path(path);
	return ret;
}
