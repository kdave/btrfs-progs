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

#include "kerncompat.h"
#include <errno.h>
#include <stddef.h>
#include "kernel-shared/accessors.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/file-item.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/free-space-tree.h"
#include "common/internal.h"
#include "common/extent-tree-utils.h"
#include "common/messages.h"

/*
 * Search in extent tree to found next meta/data extent. Caller needs to check
 * for no-hole or skinny metadata features.
 */
int btrfs_next_extent_item(struct btrfs_root *root, struct btrfs_path *path,
			   u64 max_objectid)
{
	struct btrfs_key found_key;
	int ret;

	while (1) {
		ret = btrfs_next_item(root, path);
		if (ret)
			return ret;
		btrfs_item_key_to_cpu(path->nodes[0], &found_key, path->slots[0]);
		if (found_key.objectid > max_objectid)
			return 1;
		if (found_key.type == BTRFS_EXTENT_ITEM_KEY ||
		    found_key.type == BTRFS_METADATA_ITEM_KEY)
		return 0;
	}
}

static void __get_extent_size(struct btrfs_root *root, struct btrfs_path *path,
			      u64 *start, u64 *len)
{
	struct btrfs_key key;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	BUG_ON(!(key.type == BTRFS_EXTENT_ITEM_KEY ||
		 key.type == BTRFS_METADATA_ITEM_KEY));
	*start = key.objectid;
	if (key.type == BTRFS_EXTENT_ITEM_KEY)
		*len = key.offset;
	else
		*len = root->fs_info->nodesize;
}

/*
 * Find first overlap extent for range [bytenr, bytenr + len).
 *
 * Return 0 for found and point path to it.
 * Return >0 for not found.
 * Return <0 for err
 */
static int btrfs_search_overlap_extent(struct btrfs_root *root,
				       struct btrfs_path *path, u64 bytenr, u64 len)
{
	struct btrfs_key key;
	u64 cur_start;
	u64 cur_len;
	int ret;

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		return ret;
	if (ret == 0) {
		error_msg(ERROR_MSG_UNEXPECTED, "EXTENT_DATA found at %llu", bytenr);
		return -EUCLEAN;
	}

	ret = btrfs_previous_extent_item(root, path, 0);
	if (ret < 0)
		return ret;
	/* No previous, check next extent. */
	if (ret > 0)
		goto next;
	__get_extent_size(root, path, &cur_start, &cur_len);
	/* Tail overlap. */
	if (cur_start + cur_len > bytenr)
		return 1;

next:
	ret = btrfs_next_extent_item(root, path, bytenr + len);
	if (ret < 0)
		return ret;
	/* No next, prev already checked, no overlap. */
	if (ret > 0)
		return 0;
	__get_extent_size(root, path, &cur_start, &cur_len);
	/* Head overlap.*/
	if (cur_start < bytenr + len)
		return 1;
	return 0;
}

static int __btrfs_record_file_extent(struct btrfs_trans_handle *trans,
				      struct btrfs_root *root, u64 objectid,
				      struct btrfs_inode_item *inode,
				      u64 file_pos, u64 disk_bytenr,
				      u64 *ret_num_bytes)
{
	int ret;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = btrfs_extent_root(info, disk_bytenr);
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key ins_key;
	struct btrfs_path *path;
	struct btrfs_extent_item *ei;
	u64 nbytes;
	u64 extent_num_bytes;
	u64 extent_bytenr;
	u64 extent_offset;
	u64 num_bytes = *ret_num_bytes;

	/*
	 * @objectid should be an inode number, thus it must not be smaller
	 * than BTRFS_FIRST_FREE_OBJECTID.
	 */
	UASSERT(objectid >= BTRFS_FIRST_FREE_OBJECTID);

	/*
	 * All supported file system should not use its 0 extent.  As it's for
	 * hole.  And hole extent has no size limit, no need to loop.
	 */
	if (disk_bytenr == 0) {
		ret = btrfs_insert_file_extent(trans, root, objectid,
					       file_pos, disk_bytenr,
					       num_bytes, num_bytes);
		return ret;
	}
	num_bytes = min_t(u64, num_bytes, BTRFS_MAX_EXTENT_SIZE);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* First to check extent overlap. */
	ret = btrfs_search_overlap_extent(extent_root, path, disk_bytenr, num_bytes);
	if (ret < 0)
		goto fail;
	if (ret > 0) {
		/* Found overlap. */
		u64 cur_start;
		u64 cur_len;

		__get_extent_size(extent_root, path, &cur_start, &cur_len);
		/* For convert case, this extent should be a subset of existing one. */
		if (disk_bytenr < cur_start) {
			error_msg(ERROR_MSG_UNEXPECTED,
				  "invalid range disk_bytenr < cur_start: %llu < %llu",
				  disk_bytenr, cur_start);
			ret = -EUCLEAN;
			goto fail;
		}

		extent_bytenr = cur_start;
		extent_num_bytes = cur_len;
		extent_offset = disk_bytenr - extent_bytenr;
	} else {
		/* No overlap, create new extent. */
		btrfs_release_path(path);
		ins_key.objectid = disk_bytenr;
		ins_key.type = BTRFS_EXTENT_ITEM_KEY;
		ins_key.offset = num_bytes;

		ret = btrfs_insert_empty_item(trans, extent_root, path,
					      &ins_key, sizeof(*ei));
		if (ret == 0) {
			leaf = path->nodes[0];
			ei = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_extent_item);

			btrfs_set_extent_refs(leaf, ei, 0);
			btrfs_set_extent_generation(leaf, ei, trans->transid);
			btrfs_set_extent_flags(leaf, ei,
					       BTRFS_EXTENT_FLAG_DATA);
			btrfs_mark_buffer_dirty(leaf);

			ret = btrfs_update_block_group(trans, disk_bytenr,
						       num_bytes, 1, 0);
			if (ret)
				goto fail;
		} else if (ret != -EEXIST) {
			goto fail;
		}

		ret = remove_from_free_space_tree(trans, disk_bytenr, num_bytes);
		if (ret)
			goto fail;

		btrfs_run_delayed_refs(trans, -1);
		extent_bytenr = disk_bytenr;
		extent_num_bytes = num_bytes;
		extent_offset = 0;
	}
	btrfs_release_path(path);
	ins_key.objectid = objectid;
	ins_key.offset = file_pos;
	ins_key.type = BTRFS_EXTENT_DATA_KEY;
	ret = btrfs_insert_empty_item(trans, root, path, &ins_key, sizeof(*fi));
	if (ret)
		goto fail;
	leaf = path->nodes[0];
	fi = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_file_extent_item);
	btrfs_set_file_extent_generation(leaf, fi, trans->transid);
	btrfs_set_file_extent_type(leaf, fi, BTRFS_FILE_EXTENT_REG);
	btrfs_set_file_extent_disk_bytenr(leaf, fi, extent_bytenr);
	btrfs_set_file_extent_disk_num_bytes(leaf, fi, extent_num_bytes);
	btrfs_set_file_extent_offset(leaf, fi, extent_offset);
	btrfs_set_file_extent_num_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_ram_bytes(leaf, fi, extent_num_bytes);
	btrfs_set_file_extent_compression(leaf, fi, 0);
	btrfs_set_file_extent_encryption(leaf, fi, 0);
	btrfs_set_file_extent_other_encoding(leaf, fi, 0);
	btrfs_mark_buffer_dirty(leaf);

	nbytes = btrfs_stack_inode_nbytes(inode) + num_bytes;
	btrfs_set_stack_inode_nbytes(inode, nbytes);
	btrfs_release_path(path);

	ret = btrfs_inc_extent_ref(trans, extent_bytenr, extent_num_bytes,
				   0, root->root_key.objectid, objectid,
				   file_pos - extent_offset);
	if (ret)
		goto fail;
	ret = 0;
	*ret_num_bytes = min(extent_num_bytes - extent_offset, num_bytes);
fail:
	btrfs_free_path(path);
	return ret;
}

/*
 * Record a file extent. Do all the required works, such as inserting file
 * extent item, inserting extent item and backref item into extent tree and
 * updating block accounting.
 */
int btrfs_record_file_extent(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 objectid,
			     struct btrfs_inode_item *inode,
			     u64 file_pos, u64 disk_bytenr,
			     u64 num_bytes)
{
	u64 cur_disk_bytenr = disk_bytenr;
	u64 cur_file_pos = file_pos;
	u64 cur_num_bytes = num_bytes;
	int ret = 0;

	while (num_bytes > 0) {
		ret = __btrfs_record_file_extent(trans, root, objectid,
						 inode, cur_file_pos,
						 cur_disk_bytenr,
						 &cur_num_bytes);
		if (ret < 0)
			break;
		cur_disk_bytenr += cur_num_bytes;
		cur_file_pos += cur_num_bytes;
		num_bytes -= cur_num_bytes;
	}
	return ret;
}
