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
#include "hash.h"
#include "transaction.h"

static struct btrfs_dir_item *insert_with_overflow(struct
						   btrfs_trans_handle *trans,
						   struct btrfs_root *root,
						   struct btrfs_path *path,
						   struct btrfs_key *cpu_key,
						   u32 data_size,
						   const char *name,
						   int name_len)
{
	int ret;
	char *ptr;
	struct btrfs_item *item;
	struct btrfs_leaf *leaf;
	ret = btrfs_insert_empty_item(trans, root, path, cpu_key, data_size);
	if (ret == -EEXIST) {
		struct btrfs_dir_item *di;
		di = btrfs_match_dir_item_name(root, path, name, name_len);
		if (di)
			return NULL;
		ret = btrfs_extend_item(trans, root, path, data_size);
	}
	BUG_ON(ret > 0);
	if (ret)
		return NULL;
	leaf = &path->nodes[0]->leaf;
	item = leaf->items + path->slots[0];
	ptr = btrfs_item_ptr(leaf, path->slots[0], char);
	BUG_ON(data_size > btrfs_item_size(item));
	ptr += btrfs_item_size(item) - data_size;
	return (struct btrfs_dir_item *)ptr;
}

int btrfs_insert_dir_item(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, char *name, int name_len, u64 dir,
			  struct btrfs_key *location, u8 type)
{
	int ret = 0;
	struct btrfs_path path;
	struct btrfs_dir_item *dir_item;
	char *name_ptr;
	struct btrfs_key key;
	u32 data_size;

	key.objectid = dir;
	btrfs_set_key_type(&key, BTRFS_DIR_ITEM_KEY);
	if (name_len == 1 && *name == '.')
		key.offset = 1;
	else if (name_len == 2 && name[0] == '.' && name[1] == '.')
		key.offset = 2;
	else
		ret = btrfs_name_hash(name, name_len, &key.offset);
	BUG_ON(ret);
	btrfs_init_path(&path);
	data_size = sizeof(*dir_item) + name_len;
	dir_item = insert_with_overflow(trans, root, &path, &key, data_size,
					name, name_len);
	if (!dir_item) {
		ret = -1;
		goto out;
	}
	btrfs_cpu_key_to_disk(&dir_item->location, location);
	btrfs_set_dir_type(dir_item, type);
	btrfs_set_dir_name_len(dir_item, name_len);
	btrfs_set_dir_data_len(dir_item, 0);
	name_ptr = (char *)(dir_item + 1);
	memcpy(name_ptr, name, name_len);

	/* FIXME, use some real flag for selecting the extra index */
	if (root == root->fs_info->tree_root)
		goto out;

	btrfs_release_path(root, &path);
	btrfs_set_key_type(&key, BTRFS_DIR_INDEX_KEY);
	key.offset = location->objectid;
	dir_item = insert_with_overflow(trans, root, &path, &key, data_size,
					name, name_len);
	if (!dir_item) {
		ret = -1;
		goto out;
	}
	btrfs_cpu_key_to_disk(&dir_item->location, location);
	btrfs_set_dir_type(dir_item, type);
	btrfs_set_dir_name_len(dir_item, name_len);
	btrfs_set_dir_data_len(dir_item, 0);
	name_ptr = (char *)(dir_item + 1);
	memcpy(name_ptr, name, name_len);
out:
	btrfs_release_path(root, &path);
	return ret;
}
struct btrfs_dir_item *btrfs_lookup_dir_item(struct btrfs_trans_handle *trans,
					      struct btrfs_root *root,
					      struct btrfs_path *path, u64 dir,
					      char *name, int name_len, int mod)
{
	int ret;
	struct btrfs_key key;
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;
	struct btrfs_key found_key;
	struct btrfs_leaf *leaf;
	key.objectid = dir;
	btrfs_set_key_type(&key, BTRFS_DIR_ITEM_KEY);
	ret = btrfs_name_hash(name, name_len, &key.offset);
	BUG_ON(ret);
	ret = btrfs_search_slot(trans, root, &key, path, ins_len, cow);
	if (ret < 0)
		return NULL;
	if (ret > 0) {
		if (path->slots[0] == 0)
			return NULL;
		path->slots[0]--;
	}

	leaf = &path->nodes[0]->leaf;
	btrfs_disk_key_to_cpu(&found_key, &leaf->items[path->slots[0]].key);

	if (found_key.objectid != dir ||
	    btrfs_key_type(&found_key) != BTRFS_DIR_ITEM_KEY ||
	    found_key.offset != key.offset)
		return NULL;

	return btrfs_match_dir_item_name(root, path, name, name_len);
}

struct btrfs_dir_item *btrfs_match_dir_item_name(struct btrfs_root *root,
			      struct btrfs_path *path,
			      const char *name, int name_len)
{
	u32 cur = 0;
	u32 this_len;
	u32 total_len;
	char *name_ptr;
	struct btrfs_leaf *leaf;
	struct btrfs_dir_item *dir_item;

	leaf = &path->nodes[0]->leaf;
	dir_item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_dir_item);
	total_len = btrfs_item_size(leaf->items + path->slots[0]);
	while(cur < total_len) {
		this_len = sizeof(*dir_item) + btrfs_dir_name_len(dir_item) +
			   btrfs_dir_data_len(dir_item);
		name_ptr = (char *)(dir_item + 1);

		if (btrfs_dir_name_len(dir_item) == name_len &&
		    memcmp(name, name_ptr, name_len) == 0)
			return dir_item;

		cur += this_len;
		dir_item = (struct btrfs_dir_item *)((char *)dir_item +
						     this_len);
	}
	return NULL;
}

int btrfs_delete_one_dir_name(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root,
			      struct btrfs_path *path,
			      struct btrfs_dir_item *di)
{

	struct btrfs_leaf *leaf;
	u32 sub_item_len;
	u32 item_len;
	int ret = 0;

	leaf = &path->nodes[0]->leaf;
	sub_item_len = sizeof(*di) + btrfs_dir_name_len(di) +
		       btrfs_dir_data_len(di);
	item_len = btrfs_item_size(leaf->items + path->slots[0]);
	if (sub_item_len == item_len) {
		ret = btrfs_del_item(trans, root, path);
	} else {
		char *ptr = (char *)di;
		char *start = btrfs_item_ptr(leaf, path->slots[0], char);
		memmove(ptr, ptr + sub_item_len,
			item_len - (ptr + sub_item_len - start));
		ret = btrfs_truncate_item(trans, root, path,
					  item_len - sub_item_len, 1);
	}
	return 0;
}
