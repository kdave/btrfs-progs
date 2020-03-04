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
#include <fcntl.h>
#include "kerncompat.h"
#include "kernel-lib/radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"

/* for testing only */
static int next_key(int i, int max_key) {
	return rand() % max_key;
	// return i;
}

int main(int ac, char **av) {
	struct btrfs_key ins;
	struct btrfs_key last = { (u64)-1, 0, 0};
	char *buf;
	int i;
	int num;
	int ret;
	int run_size = 300000;
	int max_key =  100000000;
	int tree_size = 2;
	struct btrfs_path path;
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;

	buf = calloc(1, 512);

	radix_tree_init();

	root = open_ctree(av[1], BTRFS_SUPER_INFO_OFFSET, OPEN_CTREE_WRITES);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	srand(55);
	ins.type = BTRFS_STRING_ITEM_KEY;
	for (i = 0; i < run_size; i++) {
		num = next_key(i, max_key);
		// num = i;
		sprintf(buf, "string-%d", num);
		if (i % 10000 == 0)
			fprintf(stderr, "insert %d:%d\n", num, i);
		ins.objectid = num;
		ins.offset = 0;
		ret = btrfs_insert_item(trans, root, &ins, buf, 512);
		if (!ret)
			tree_size++;
		if (i == run_size - 5) {
			btrfs_commit_transaction(trans, root);
			trans = btrfs_start_transaction(root, 1);
			BUG_ON(IS_ERR(trans));
		}
	}
	btrfs_commit_transaction(trans, root);
	close_ctree(root);
	exit(1);
	root = open_ctree(av[1], BTRFS_SUPER_INFO_OFFSET, OPEN_CTREE_WRITES);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}
	printf("starting search\n");
	srand(55);
	for (i = 0; i < run_size; i++) {
		num = next_key(i, max_key);
		ins.objectid = num;
		btrfs_init_path(&path);
		if (i % 10000 == 0)
			fprintf(stderr, "search %d:%d\n", num, i);
		ret = btrfs_search_slot(NULL, root, &ins, &path, 0, 0);
		if (ret) {
			btrfs_print_tree(root->node, 1, BTRFS_PRINT_TREE_BFS);
			printf("unable to find %d\n", num);
			exit(1);
		}
		btrfs_release_path(&path);
	}
	close_ctree(root);

	root = open_ctree(av[1], BTRFS_SUPER_INFO_OFFSET, OPEN_CTREE_WRITES);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}
	printf("node %p level %d total ptrs %u free spc %lu\n", root->node,
	        btrfs_header_level(root->node),
		btrfs_header_nritems(root->node),
		(unsigned long)BTRFS_NODEPTRS_PER_BLOCK(root->fs_info) -
		btrfs_header_nritems(root->node));
	printf("all searches good, deleting some items\n");
	i = 0;
	srand(55);
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	for (i = 0 ; i < run_size/4; i++) {
		num = next_key(i, max_key);
		ins.objectid = num;
		btrfs_init_path(&path);
		ret = btrfs_search_slot(trans, root, &ins, &path, -1, 1);
		if (!ret) {
			if (i % 10000 == 0)
				fprintf(stderr, "del %d:%d\n", num, i);
			ret = btrfs_del_item(trans, root, &path);
			if (ret != 0)
				BUG();
			tree_size--;
		}
		btrfs_release_path(&path);
	}
	btrfs_commit_transaction(trans, root);
	close_ctree(root);

	root = open_ctree(av[1], BTRFS_SUPER_INFO_OFFSET, OPEN_CTREE_WRITES);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	srand(128);
	for (i = 0; i < run_size; i++) {
		num = next_key(i, max_key);
		sprintf(buf, "string-%d", num);
		ins.objectid = num;
		if (i % 10000 == 0)
			fprintf(stderr, "insert %d:%d\n", num, i);
		ret = btrfs_insert_item(trans, root, &ins, buf, 512);
		if (!ret)
			tree_size++;
	}
	btrfs_commit_transaction(trans, root);
	close_ctree(root);

	root = open_ctree(av[1], BTRFS_SUPER_INFO_OFFSET, OPEN_CTREE_WRITES);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}
	srand(128);
	printf("starting search2\n");
	for (i = 0; i < run_size; i++) {
		num = next_key(i, max_key);
		ins.objectid = num;
		btrfs_init_path(&path);
		if (i % 10000 == 0)
			fprintf(stderr, "search %d:%d\n", num, i);
		ret = btrfs_search_slot(NULL, root, &ins, &path, 0, 0);
		if (ret) {
			btrfs_print_tree(root->node, 1, BTRFS_PRINT_TREE_BFS);
			printf("unable to find %d\n", num);
			exit(1);
		}
		btrfs_release_path(&path);
	}
	printf("starting big long delete run\n");
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	while(root->node && btrfs_header_nritems(root->node) > 0) {
		struct extent_buffer *leaf;
		int slot;
		ins.objectid = (u64)-1;
		btrfs_init_path(&path);
		ret = btrfs_search_slot(trans, root, &ins, &path, -1, 1);
		if (ret == 0)
			BUG();

		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot != btrfs_header_nritems(leaf))
			BUG();
		while(path.slots[0] > 0) {
			path.slots[0] -= 1;
			slot = path.slots[0];
			leaf = path.nodes[0];

			btrfs_item_key_to_cpu(leaf, &last, slot);

			if (tree_size % 10000 == 0)
				printf("big del %d:%d\n", tree_size, i);
			ret = btrfs_del_item(trans, root, &path);
			if (ret != 0) {
				printf("del_item returned %d\n", ret);
				BUG();
			}
			tree_size--;
		}
		btrfs_release_path(&path);
	}
	/*
	printf("previous tree:\n");
	btrfs_print_tree(root, root->commit_root);
	printf("map before commit\n");
	btrfs_print_tree(root->extent_root, root->extent_root->node);
	*/
	btrfs_commit_transaction(trans, root);
	printf("tree size is now %d\n", tree_size);
	printf("root %p commit root %p\n", root->node, root->commit_root);
	btrfs_print_tree(root->node, 1, BTRFS_PRINT_TREE_BFS);
	close_ctree(root);
	return 0;
}
