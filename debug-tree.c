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
#include <uuid/uuid.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"

int main(int ac, char **av) {
	struct btrfs_root *root;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root_item ri;
	struct extent_buffer *leaf;
	struct btrfs_key found_key;
	char uuidbuf[37];
	int ret;
	int slot;

	if (ac != 2) {
		fprintf(stderr, "usage: %s device\n", av[0]);
		exit(1);
	}
	radix_tree_init();
	root = open_ctree(av[1], 0);
	if (!root) {
		fprintf(stderr, "unable to open %s\n", av[1]);
		exit(1);
	}
	printf("root tree\n");
	btrfs_print_tree(root->fs_info->tree_root,
			 root->fs_info->tree_root->node);
	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	btrfs_set_key_type(&key, BTRFS_ROOT_ITEM_KEY);
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root,
					&key, &path, 0, 0);
	BUG_ON(ret < 0);
	while(1) {
		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret != 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (btrfs_key_type(&found_key) == BTRFS_ROOT_ITEM_KEY) {
			unsigned long offset;
			struct extent_buffer *buf;
			offset = btrfs_item_ptr_offset(leaf, slot);
			read_extent_buffer(leaf, &ri, offset, sizeof(ri));
			buf = read_tree_block(root->fs_info->tree_root,
					      btrfs_root_bytenr(&ri),
					      root->leafsize);
			switch(found_key.objectid) {
			case BTRFS_ROOT_TREE_OBJECTID:
				printf("root ");
				break;
			case BTRFS_EXTENT_TREE_OBJECTID:
				printf("extent tree ");
				break;
			}
			printf("tree %llu %u %llu\n",
			       (unsigned long long)found_key.objectid,
			       found_key.type,
			       (unsigned long long)found_key.offset);
			btrfs_print_tree(root, buf);
		}
		path.slots[0]++;
	}
	btrfs_release_path(root, &path);
	printf("total bytes %llu\n",
	       (unsigned long long)btrfs_super_total_bytes(&root->fs_info->super_copy));
	printf("bytes used %llu\n",
	       (unsigned long long)btrfs_super_bytes_used(&root->fs_info->super_copy));
	uuidbuf[36] = '\0';
	uuid_unparse(root->fs_info->super_copy.fsid, uuidbuf);
	printf("uuid %s\n", uuidbuf);
	return 0;
}
