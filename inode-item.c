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

int btrfs_insert_inode_ref(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   const char *name, int name_len,
			   u64 inode_objectid, u64 ref_objectid)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_inode_ref *ref;
	char *ptr;
	int ret;
	int ins_len = name_len + sizeof(*ref);

	key.objectid = inode_objectid;
	key.offset = ref_objectid;
	btrfs_set_key_type(&key, BTRFS_INODE_REF_KEY);

	btrfs_init_path(&path);
	ret = btrfs_insert_empty_item(trans, root, &path, &key,
				      ins_len);
	if (ret == -EEXIST) {
#if 0
		u32 old_size;

		if (find_name_in_backref(path, name, name_len, &ref))
			goto out;

		old_size = btrfs_item_size_nr(path->nodes[0], path->slots[0]);
		ret = btrfs_extend_item(trans, root, path, ins_len);
		BUG_ON(ret);
		ref = btrfs_item_ptr(path->nodes[0], path->slots[0],
				     struct btrfs_inode_ref);
		ref = (struct btrfs_inode_ref *)((unsigned long)ref + old_size);
		btrfs_set_inode_ref_name_len(path->nodes[0], ref, name_len);
		ptr = (unsigned long)(ref + 1);
		ret = 0;
#endif
		goto out;
	} else if (ret < 0) {
		goto out;
	} else {
		ref = btrfs_item_ptr(&path.nodes[0]->leaf, path.slots[0],
				     struct btrfs_inode_ref);
		btrfs_set_inode_ref_name_len(ref, name_len);
		ptr = (char *)(ref + 1);
	}
	memcpy(ptr, name, name_len);
	dirty_tree_block(trans, root, path.nodes[0]);

out:
	btrfs_release_path(root, &path);
	return ret;
}

int btrfs_insert_inode(struct btrfs_trans_handle *trans, struct btrfs_root
		       *root, u64 objectid, struct btrfs_inode_item
		       *inode_item)
{
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;
	key.objectid = objectid;
	btrfs_set_key_type(&key, BTRFS_INODE_ITEM_KEY);
	key.offset = 0;

	btrfs_init_path(&path);
	ret = btrfs_insert_item(trans, root, &key, inode_item,
				sizeof(*inode_item));
	btrfs_release_path(root, &path);
	return ret;
}

int btrfs_lookup_inode(struct btrfs_trans_handle *trans, struct btrfs_root
		       *root, struct btrfs_path *path, u64 objectid, int mod)
{
	struct btrfs_key key;
	int ins_len = mod < 0 ? -1 : 0;
	int cow = mod != 0;

	key.objectid = objectid;
	btrfs_set_key_type(&key, BTRFS_INODE_ITEM_KEY);
	key.offset = 0;
	return btrfs_search_slot(trans, root, &key, path, ins_len, cow);
}
