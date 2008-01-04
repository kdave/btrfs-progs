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

#define _XOPEN_SOURCE 600
#define __USE_XOPEN2K
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <fcntl.h>
#include <unistd.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "crc32c.h"
#include "utils.h"

static u64 reference_root_table[4] = {
	[0] =	0,
	[1] =	BTRFS_ROOT_TREE_OBJECTID,
	[2] =	BTRFS_EXTENT_TREE_OBJECTID,
	[3] =	BTRFS_FS_TREE_OBJECTID,
};

int make_btrfs(int fd, u64 blocks[4], u64 num_bytes, u32 nodesize,
	       u32 leafsize, u32 sectorsize, u32 stripesize)
{
	struct btrfs_super_block super;
	struct extent_buffer *buf;
	struct btrfs_root_item root_item;
	struct btrfs_disk_key disk_key;
	struct btrfs_extent_ref *extent_ref;
	struct btrfs_extent_item *extent_item;
	struct btrfs_inode_item *inode_item;
	int i;
	int ret;
	u32 itemoff;
	u32 nritems = 0;
	u64 hash;
	u64 first_free;
	u64 ref_gen;
	u64 ref_root;

	first_free = BTRFS_SUPER_INFO_OFFSET + sectorsize * 2 - 1;
	first_free &= ~((u64)sectorsize - 1);

	num_bytes = (num_bytes / sectorsize) * sectorsize;
	uuid_generate(super.fsid);
	btrfs_set_super_bytenr(&super, blocks[0]);
	strcpy((char *)(&super.magic), BTRFS_MAGIC);
	btrfs_set_super_generation(&super, 1);
	btrfs_set_super_root(&super, blocks[1]);
	btrfs_set_super_total_bytes(&super, num_bytes);
	btrfs_set_super_bytes_used(&super, first_free + 3 * leafsize);
	btrfs_set_super_root_dir(&super, 0);
	btrfs_set_super_sectorsize(&super, sectorsize);
	btrfs_set_super_leafsize(&super, leafsize);
	btrfs_set_super_nodesize(&super, nodesize);
	btrfs_set_super_stripesize(&super, stripesize);
	btrfs_set_super_root_level(&super, 0);

	buf = malloc(sizeof(*buf) + max(sectorsize, leafsize));

	BUG_ON(sizeof(super) > sectorsize);
	memset(buf->data, 0, sectorsize);
	memcpy(buf->data, &super, sizeof(super));
	ret = pwrite(fd, buf->data, sectorsize, blocks[0]);
	BUG_ON(ret != sectorsize);

	/* create the tree of root objects */
	memset(buf->data, 0, leafsize);
	btrfs_set_header_bytenr(buf, blocks[1]);
	btrfs_set_header_nritems(buf, 2);
	btrfs_set_header_generation(buf, 1);
	btrfs_set_header_owner(buf, BTRFS_ROOT_TREE_OBJECTID);
	write_extent_buffer(buf, super.fsid, (unsigned long)
			    btrfs_header_fsid(buf), BTRFS_FSID_SIZE);

	/* create the items for the root tree */
	memset(&root_item, 0, sizeof(root_item));
	inode_item = &root_item.inode;
	btrfs_set_stack_inode_generation(inode_item, 1);
	btrfs_set_stack_inode_size(inode_item, 3);
	btrfs_set_stack_inode_nlink(inode_item, 1);
	btrfs_set_stack_inode_nblocks(inode_item, 1);
	btrfs_set_stack_inode_mode(inode_item, S_IFDIR | 0755);
	btrfs_set_root_refs(&root_item, 1);
	btrfs_set_root_used(&root_item, leafsize);

	memset(&disk_key, 0, sizeof(disk_key));
	btrfs_set_disk_key_type(&disk_key, BTRFS_ROOT_ITEM_KEY);
	btrfs_set_disk_key_offset(&disk_key, 0);

	itemoff = __BTRFS_LEAF_DATA_SIZE(leafsize) - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, blocks[2]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, 0);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, 0), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf, 0), sizeof(root_item));
	write_extent_buffer(buf, &root_item, btrfs_item_ptr_offset(buf, 0),
			    sizeof(root_item));

	itemoff = itemoff - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, blocks[3]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_FS_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, 1);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, 1), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf, 1), sizeof(root_item));
	write_extent_buffer(buf, &root_item, btrfs_item_ptr_offset(buf, 1),
			    sizeof(root_item));
	ret = pwrite(fd, buf->data, leafsize, blocks[1]);
	BUG_ON(ret != leafsize);

	/* create the items for the extent tree */
	itemoff = __BTRFS_LEAF_DATA_SIZE(leafsize) -
		  sizeof(struct btrfs_extent_item);
	btrfs_set_disk_key_objectid(&disk_key, 0);
	btrfs_set_disk_key_offset(&disk_key, first_free);
	btrfs_set_disk_key_type(&disk_key, BTRFS_EXTENT_ITEM_KEY);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf,  nritems),
			    sizeof(struct btrfs_extent_item));
	extent_item = btrfs_item_ptr(buf, nritems, struct btrfs_extent_item);
	btrfs_set_extent_refs(buf, extent_item, 1);
	nritems++;

	for (i = 0; i < 4; i++) {
		if (blocks[i] < first_free) {
			BUG_ON(i > 0);
			continue;
		}
		/* create extent item */
		itemoff = itemoff - sizeof(struct btrfs_extent_item);
		btrfs_set_disk_key_objectid(&disk_key, blocks[i]);
		btrfs_set_disk_key_offset(&disk_key, leafsize);
		btrfs_set_disk_key_type(&disk_key, BTRFS_EXTENT_ITEM_KEY);
		btrfs_set_item_key(buf, &disk_key, nritems);
		btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems),
				      itemoff);
		btrfs_set_item_size(buf, btrfs_item_nr(buf, nritems),
				    sizeof(struct btrfs_extent_item));
		extent_item = btrfs_item_ptr(buf, nritems,
					     struct btrfs_extent_item);
		btrfs_set_extent_refs(buf, extent_item, 1);
		nritems++;

		/* create extent ref */
		ref_root = reference_root_table[i];
		if (ref_root == BTRFS_FS_TREE_OBJECTID)
			ref_gen = 1;
		else
			ref_gen = 0;

		hash = btrfs_hash_extent_ref(ref_root, ref_gen, 0, 0);
		itemoff = itemoff - sizeof(struct btrfs_extent_ref);
		btrfs_set_disk_key_objectid(&disk_key, blocks[i]);
		btrfs_set_disk_key_offset(&disk_key, hash);
		btrfs_set_disk_key_type(&disk_key, BTRFS_EXTENT_REF_KEY);
		btrfs_set_item_key(buf, &disk_key, nritems);
		btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems),
				      itemoff);
		btrfs_set_item_size(buf, btrfs_item_nr(buf, nritems),
				    sizeof(struct btrfs_extent_ref));
		extent_ref = btrfs_item_ptr(buf, nritems,
					     struct btrfs_extent_ref);
		btrfs_set_ref_root(buf, extent_ref, ref_root);
		btrfs_set_ref_generation(buf, extent_ref, ref_gen);
		btrfs_set_ref_objectid(buf, extent_ref, 0);
		btrfs_set_ref_offset(buf, extent_ref, 0);
		nritems++;
	}
	btrfs_set_header_bytenr(buf, blocks[2]);
	btrfs_set_header_owner(buf, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	ret = pwrite(fd, buf->data, leafsize, blocks[2]);
	BUG_ON(ret != leafsize);

	/* finally create the FS root */
	btrfs_set_header_bytenr(buf, blocks[3]);
	btrfs_set_header_owner(buf, BTRFS_FS_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, 0);
	ret = pwrite(fd, buf->data, leafsize, blocks[3]);
	BUG_ON(ret != leafsize);

	free(buf);
	return 0;
}

int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid)
{
	int ret;
	struct btrfs_inode_item inode_item;

	memset(&inode_item, 0, sizeof(inode_item));
	btrfs_set_stack_inode_generation(&inode_item, trans->transid);
	btrfs_set_stack_inode_size(&inode_item, 0);
	btrfs_set_stack_inode_nlink(&inode_item, 1);
	btrfs_set_stack_inode_nblocks(&inode_item, 1);
	btrfs_set_stack_inode_mode(&inode_item, S_IFDIR | 0555);

	if (root->fs_info->tree_root == root)
		btrfs_set_super_root_dir(&root->fs_info->super_copy, objectid);

	ret = btrfs_insert_inode(trans, root, objectid, &inode_item);
	if (ret)
		goto error;

	ret = btrfs_insert_inode_ref(trans, root, "..", 2, objectid, objectid);
	if (ret)
		goto error;

	btrfs_set_root_dirid(&root->root_item, objectid);
	ret = 0;
error:
	return ret;
}
