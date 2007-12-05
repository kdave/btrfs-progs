/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
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
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "crc32c.h"

#define MAX_CSUM_ITEMS(r) ((((BTRFS_LEAF_DATA_SIZE(r) - \
			       sizeof(struct btrfs_item) * 2) / \
			       BTRFS_CRC32_SIZE) - 1))
int btrfs_create_file(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, u64 dirid, u64 *objectid)
{
	return 0;
}

int btrfs_insert_file_extent(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root,
			       u64 objectid, u64 pos,
			       u64 offset, u64 disk_num_bytes,
			       u64 num_bytes)
{
	int ret = 0;
	struct btrfs_file_extent_item *item;
	struct btrfs_key file_key;
	struct btrfs_path path;
	struct btrfs_leaf *leaf;


	btrfs_init_path(&path);
	file_key.objectid = objectid;
	file_key.offset = pos;
	btrfs_set_key_type(&file_key, BTRFS_EXTENT_DATA_KEY);

	ret = btrfs_insert_empty_item(trans, root, &path, &file_key,
				      sizeof(*item));
	if (ret < 0)
		goto out;
	BUG_ON(ret);
	leaf = &path.nodes[0]->leaf;
	item = btrfs_item_ptr(leaf, path.slots[0],
			      struct btrfs_file_extent_item);
	btrfs_set_file_extent_disk_bytenr(item, offset);
	btrfs_set_file_extent_disk_num_bytes(item, disk_num_bytes);
	btrfs_set_file_extent_offset(item, 0);
	btrfs_set_file_extent_num_bytes(item, num_bytes);
	btrfs_set_file_extent_generation(item, trans->transid);
	btrfs_set_file_extent_type(item, BTRFS_FILE_EXTENT_REG);
out:
	btrfs_release_path(root, &path);
	return ret;
}

int btrfs_insert_inline_file_extent(struct btrfs_trans_handle *trans,
				    struct btrfs_root *root, u64 objectid,
				    u64 offset, char *buffer, size_t size)
{
	int ret;
	char *ptr;
	u32 datasize;
	struct btrfs_key key;
	struct btrfs_path path;
	struct btrfs_leaf *leaf;
	struct btrfs_file_extent_item *ei;

	btrfs_init_path(&path);
	key.objectid = objectid;
	key.offset = offset;
	btrfs_set_key_type(&key, BTRFS_EXTENT_DATA_KEY);

	datasize = btrfs_file_extent_calc_inline_size(size);
	ret = btrfs_insert_empty_item(trans, root, &path, &key,
				      datasize);
	BUG_ON(ret);
	leaf = &path.nodes[0]->leaf;
	ei = btrfs_item_ptr(leaf, path.slots[0],
			    struct btrfs_file_extent_item);
	btrfs_set_file_extent_generation(ei, trans->transid);
	btrfs_set_file_extent_type(ei, BTRFS_FILE_EXTENT_INLINE);
	ptr = btrfs_file_extent_inline_start(ei);
	memcpy(ptr, buffer, size);
	btrfs_release_path(root, &path);
	return 0;
}

int btrfs_lookup_csum(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root,
		      struct btrfs_path *path,
		      u64 objectid, u64 offset, int cow,
		      struct btrfs_csum_item **item_ret)
{
	int ret;
	int slot;
	struct btrfs_key file_key;
	struct btrfs_key found_key;
	struct btrfs_csum_item *item;
	struct btrfs_leaf *leaf;
	u64 csum_offset = 0;
	int csums_in_item;

	file_key.objectid = objectid;
	file_key.offset = offset;
	btrfs_set_key_type(&file_key, BTRFS_CSUM_ITEM_KEY);
	ret = btrfs_search_slot(trans, root, &file_key, path, 0, cow);
	if (ret < 0)
		goto fail;
	leaf = &path->nodes[0]->leaf;
	if (ret > 0) {
		if (path->slots[0] == 0)
			goto fail;
		path->slots[0]--;

		slot = path->slots[0];
		btrfs_disk_key_to_cpu(&found_key, &leaf->items[slot].key);
		if (btrfs_key_type(&found_key) != BTRFS_CSUM_ITEM_KEY ||
		    found_key.objectid != objectid) {
			goto fail;
		}
		csum_offset = (offset - found_key.offset) / root->sectorsize;
		csums_in_item = btrfs_item_size(&leaf->items[slot]);
		csums_in_item /= BTRFS_CRC32_SIZE;

		if (csum_offset >= csums_in_item) {
			ret = -EFBIG;
			goto fail;
		}
		ret = 0;
	}
	item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_csum_item);
	item = (struct btrfs_csum_item *)((unsigned char *)item +
					  csum_offset * BTRFS_CRC32_SIZE);
	*item_ret = item;
fail:
	if (ret > 0)
		ret = -ENOENT;
	return ret;
}

int btrfs_csum_file_block(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  struct btrfs_inode_item *inode,
			  u64 objectid, u64 offset,
			  char *data, size_t len)
{
	int ret;
	int slot;
	struct btrfs_key file_key;
	struct btrfs_key found_key;
	u64 next_offset = (u64)-1;
	int found_next = 0;
	struct btrfs_path path;
	struct btrfs_csum_item *item;
	struct btrfs_leaf *leaf = NULL;
	u64 csum_offset;
	u32 csum_result = ~(u32)0;
	u32 nritems;
	u32 ins_size;

	btrfs_init_path(&path);

	file_key.objectid = objectid;
	file_key.offset = offset;
	btrfs_set_key_type(&file_key, BTRFS_CSUM_ITEM_KEY);

	ret = btrfs_lookup_csum(trans, root, &path, objectid,
				offset, 1, &item);
	if (!ret) {
		leaf = &path.nodes[0]->leaf;
		goto found;
	}
	if (ret != -EFBIG && ret != -ENOENT)
		goto fail;
	leaf = &path.nodes[0]->leaf;
	if (ret == -EFBIG) {
		u32 item_size;
		slot = path.slots[0];
		/* we found one, but it isn't big enough yet */
		item_size = btrfs_item_size(&leaf->items[slot]);
		if ((item_size / BTRFS_CRC32_SIZE) >= MAX_CSUM_ITEMS(root)) {
			/* already at max size, make a new one */
			goto insert;
		}
	} else {
		slot = path.slots[0] + 1;
		/* we didn't find a csum item, insert one */
		nritems = btrfs_header_nritems(&leaf->header);
		if (path.slots[0] >= nritems - 1) {
			ret = btrfs_next_leaf(root, &path);
			if (ret == 1)
				found_next = 1;
			if (ret != 0)
				goto insert;
			slot = 0;
		}
		btrfs_disk_key_to_cpu(&found_key, &leaf->items[slot].key);
		if (found_key.objectid != objectid ||
		    found_key.type != BTRFS_CSUM_ITEM_KEY) {
			found_next = 1;
			goto insert;
		}
		next_offset = found_key.offset;
		found_next = 1;
		goto insert;
	}

	/*
	 * at this point, we know the tree has an item, but it isn't big
	 * enough yet to put our csum in.  Grow it
	 */
	btrfs_release_path(root, &path);
	ret = btrfs_search_slot(trans, root, &file_key, &path,
				BTRFS_CRC32_SIZE, 1);
	if (ret < 0)
		goto fail;
	BUG_ON(ret == 0);
	if (path.slots[0] == 0) {
		goto insert;
	}
	path.slots[0]--;
	slot = path.slots[0];
	leaf = &path.nodes[0]->leaf;
	btrfs_disk_key_to_cpu(&found_key, &leaf->items[slot].key);
	csum_offset = (offset - found_key.offset) / root->sectorsize;
	if (btrfs_key_type(&found_key) != BTRFS_CSUM_ITEM_KEY ||
	    found_key.objectid != objectid ||
	    csum_offset >= MAX_CSUM_ITEMS(root)) {
		goto insert;
	}
	if (csum_offset >= btrfs_item_size(&leaf->items[slot]) /
	    BTRFS_CRC32_SIZE) {
		u32 diff = (csum_offset + 1) * BTRFS_CRC32_SIZE;
		diff = diff - btrfs_item_size(&leaf->items[slot]);
		if (diff != BTRFS_CRC32_SIZE)
			goto insert;
		ret = btrfs_extend_item(trans, root, &path, diff);
		BUG_ON(ret);
		goto csum;
	}

insert:
	btrfs_release_path(root, &path);
	csum_offset = 0;
	if (found_next) {
		u64 tmp;
		if (next_offset > btrfs_inode_size(inode))
			next_offset = btrfs_inode_size(inode);
		tmp = next_offset - offset + root->sectorsize - 1;
		tmp /= root->sectorsize;
		if (tmp > MAX_CSUM_ITEMS(root))
			tmp =  MAX_CSUM_ITEMS(root);
		ins_size = BTRFS_CRC32_SIZE * tmp;
	} else {
		ins_size = BTRFS_CRC32_SIZE;
	}
	ret = btrfs_insert_empty_item(trans, root, &path, &file_key,
				      ins_size);
	if (ret < 0)
		goto fail;
	BUG_ON(ret != 0);
csum:
	slot = path.slots[0];
	leaf = &path.nodes[0]->leaf;
	item = btrfs_item_ptr(leaf, slot, struct btrfs_csum_item);
	item = (struct btrfs_csum_item *)((unsigned char *)item +
					  csum_offset * BTRFS_CRC32_SIZE);
found:
	csum_result = crc32c(csum_result, data, len);
	csum_result = ~cpu_to_le32(csum_result);
	memcpy(item, &csum_result, BTRFS_CRC32_SIZE);
	ret = 0;
fail:
	btrfs_release_path(root, &path);
	return ret;
}
