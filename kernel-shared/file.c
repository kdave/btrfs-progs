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

#include "kerncompat.h"
#include <sys/stat.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/compression.h"
#include "common/utils.h"

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
		/* Check previous file extent */
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
	 * To keep the search behavior consistent with search_slot(),
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

/*
 * Read out content of one inode.
 *
 * @root:  fs/subvolume root containing the inode
 * @ino:   inode number
 * @start: offset inside the file, aligned to sectorsize
 * @len:   length to read, aligned to sectorisize
 * @dest:  where data will be stored
 *
 * NOTE:
 * 1) compression data is not supported yet
 * 2) @start and @len must be aligned to sectorsize
 * 3) data read out is also aligned to sectorsize, not truncated to inode size
 *
 * Return < 0 for fatal error during read.
 * Otherwise return the number of successfully read data in bytes.
 */
int btrfs_read_file(struct btrfs_root *root, u64 ino, u64 start, int len,
		    char *dest)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_inode_item *ii;
	u64 isize;
	int no_holes = btrfs_fs_incompat(fs_info, NO_HOLES);
	int slot;
	int read = 0;
	int ret;

	if (!IS_ALIGNED(start, fs_info->sectorsize) ||
	    !IS_ALIGNED(len, fs_info->sectorsize)) {
		warning("@start and @len must be aligned to %u for function %s",
			fs_info->sectorsize, __func__);
		return -EINVAL;
	}

	btrfs_init_path(&path);
	key.objectid = ino;
	key.offset = start;
	key.type = BTRFS_EXTENT_DATA_KEY;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;

	if (ret > 0) {
		ret = btrfs_previous_item(root, &path, ino, BTRFS_EXTENT_DATA_KEY);
		if (ret > 0) {
			ret = -ENOENT;
			goto out;
		}
	}

	/*
	 * Reset @dest to all 0, so we don't need to care about holes in
	 * no_hole mode, but focus on reading non-hole part.
	 */
	memset(dest, 0, len);
	while (1) {
		struct btrfs_file_extent_item *fi;
		u64 offset = 0;
		u64 extent_start;
		u64 extent_len;
		u64 read_start;
		u64 read_len;
		u64 disk_bytenr;

		leaf = path.nodes[0];
		slot = path.slots[0];

		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid > ino)
			break;
		if (key.type != BTRFS_EXTENT_DATA_KEY || key.objectid != ino)
			goto next;

		extent_start = key.offset;
		if (extent_start >= start + len)
			break;

		fi = btrfs_item_ptr(leaf, slot, struct btrfs_file_extent_item);
		if (btrfs_file_extent_compression(leaf, fi) !=
		    BTRFS_COMPRESS_NONE) {
			ret = -ENOTTY;
			break;
		}

		/* Inline extent, one inode should only one inline extent */
		if (btrfs_file_extent_type(leaf, fi) ==
		    BTRFS_FILE_EXTENT_INLINE) {
			extent_len = btrfs_file_extent_ram_bytes(leaf, fi);
			if (extent_start + extent_len <= start)
				goto next;
			read_extent_buffer(leaf, dest,
				btrfs_file_extent_inline_start(fi), extent_len);
			read += round_up(extent_len, fs_info->sectorsize);
			break;
		}

		extent_len = btrfs_file_extent_num_bytes(leaf, fi);
		if (extent_start + extent_len <= start)
			goto next;

		read_start = max(start, extent_start);
		read_len = min(start + len, extent_start + extent_len) -
			   read_start;

		/* We have already zeroed @dest, nothing to do */
		if (btrfs_file_extent_type(leaf, fi) ==
		    BTRFS_FILE_EXTENT_PREALLOC ||
		    btrfs_file_extent_disk_num_bytes(leaf, fi) == 0) {
			read += read_len;
			goto next;
		}

		disk_bytenr = btrfs_file_extent_disk_bytenr(leaf, fi) +
			      btrfs_file_extent_offset(leaf, fi);
		while (offset < read_len) {
			u64 read_len_ret = read_len - offset;

			ret = read_data_from_disk(fs_info,
					dest + read_start - start + offset,
					disk_bytenr + offset, &read_len_ret, 0);
			if (ret < 0)
				goto out;
			offset += read_len_ret;
		}
		read += read_len;
next:
		ret = btrfs_next_item(root, &path);
		if (ret > 0) {
			ret = 0;
			break;
		}
	}

	/*
	 * Special trick for no_holes, since for no_holes we don't have good
	 * method to account skipped and tailing holes, we used
	 * min(inode size, len) as return value
	 */
	if (no_holes) {
		btrfs_release_path(&path);
		key.objectid = ino;
		key.offset = 0;
		key.type = BTRFS_INODE_ITEM_KEY;
		ret = btrfs_lookup_inode(NULL, root, &path, &key, 0);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = -ENOENT;
			goto out;
		}
		ii = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_inode_item);
		isize = round_up(btrfs_inode_size(path.nodes[0], ii),
				 fs_info->sectorsize);
		read = min_t(u64, isize - start, len);
	}
out:
	btrfs_release_path(&path);
	if (!ret)
		ret = read;
	return ret;
}
