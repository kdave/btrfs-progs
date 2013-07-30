/*
 * Copyright (C) 2009 Oracle.  All rights reserved.
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

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include "kerncompat.h"
#include "ctree.h"
#include "volumes.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"

struct extent_buffer *debug_corrupt_block(struct btrfs_root *root, u64 bytenr,
				     u32 blocksize, int copy)
{
	int ret;
	struct extent_buffer *eb;
	u64 length;
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	int num_copies;
	int mirror_num = 1;

	eb = btrfs_find_create_tree_block(root, bytenr, blocksize);
	if (!eb)
		return NULL;

	length = blocksize;
	while (1) {
		ret = btrfs_map_block(&root->fs_info->mapping_tree, READ,
				      eb->start, &length, &multi,
				      mirror_num, NULL);
		BUG_ON(ret);
		device = multi->stripes[0].dev;
		eb->fd = device->fd;
		device->total_ios++;
		eb->dev_bytenr = multi->stripes[0].physical;

		fprintf(stdout,
			"mirror %d logical %llu physical %llu device %s\n",
			mirror_num, (unsigned long long)bytenr,
			(unsigned long long)eb->dev_bytenr, device->name);
		kfree(multi);

		if (!copy || mirror_num == copy) {
			ret = read_extent_from_disk(eb, 0, eb->len);
			printf("corrupting %llu copy %d\n", eb->start,
			       mirror_num);
			memset(eb->data, 0, eb->len);
			write_extent_to_disk(eb);
			fsync(eb->fd);
		}

		num_copies = btrfs_num_copies(&root->fs_info->mapping_tree,
					      eb->start, eb->len);
		if (num_copies == 1)
			break;

		mirror_num++;
		if (mirror_num > num_copies)
			break;
	}
	return eb;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfs-corrupt-block [options] device\n");
	fprintf(stderr, "\t-l Logical extent to be corrupted\n");
	fprintf(stderr, "\t-c Copy of the extent to be corrupted"
		" (usually 1 or 2, default: 0)\n");
	fprintf(stderr, "\t-b Number of bytes to be corrupted\n");
	fprintf(stderr, "\t-e Extent to be corrupted\n");
	fprintf(stderr, "\t-E The whole extent tree to be corrupted\n");
	fprintf(stderr, "\t-u Given chunk item to be corrupted\n");
	fprintf(stderr, "\t-U The whole chunk tree to be corrupted\n");
	exit(1);
}

static void corrupt_keys(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 struct extent_buffer *eb)
{
	int slot;
	int bad_slot;
	int nr;
	struct btrfs_disk_key bad_key;;

	nr = btrfs_header_nritems(eb);
	if (nr == 0)
		return;

	slot = rand() % nr;
	bad_slot = rand() % nr;

	if (bad_slot == slot)
		return;

	fprintf(stderr,
		"corrupting keys in block %llu slot %d swapping with %d\n",
		(unsigned long long)eb->start, slot, bad_slot);

	if (btrfs_header_level(eb) == 0) {
		btrfs_item_key(eb, &bad_key, bad_slot);
		btrfs_set_item_key(eb, &bad_key, slot);
	} else {
		btrfs_node_key(eb, &bad_key, bad_slot);
		btrfs_set_node_key(eb, &bad_key, slot);
	}
	btrfs_mark_buffer_dirty(eb);
	if (!trans) {
		csum_tree_block(root, eb, 0);
		write_extent_to_disk(eb);
	}
}


static int corrupt_keys_in_block(struct btrfs_root *root, u64 bytenr)
{
	struct extent_buffer *eb;

	eb = read_tree_block(root, bytenr, root->leafsize, 0);
	if (!eb)
		return -EIO;;

	corrupt_keys(NULL, root, eb);
	free_extent_buffer(eb);
	return 0;
}

static int corrupt_extent(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root, u64 bytenr, int copy)
{
	struct btrfs_key key;
	struct extent_buffer *leaf;
	u32 item_size;
	unsigned long ptr;
	struct btrfs_path *path;
	int ret;
	int slot;
	int should_del = rand() % 3;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = bytenr;
	key.type = (u8)-1;
	key.offset = (u64)-1;

	while(1) {
		ret = btrfs_search_slot(trans, root->fs_info->extent_root,
					&key, path, -1, 1);
		if (ret < 0)
			break;

		if (ret > 0) {
			if (path->slots[0] == 0)
				break;
			path->slots[0]--;
			ret = 0;
		}
		leaf = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid != bytenr)
			break;

		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_TREE_BLOCK_REF_KEY &&
		    key.type != BTRFS_EXTENT_DATA_REF_KEY &&
		    key.type != BTRFS_EXTENT_REF_V0_KEY &&
		    key.type != BTRFS_SHARED_BLOCK_REF_KEY &&
		    key.type != BTRFS_SHARED_DATA_REF_KEY)
			goto next;

		if (should_del) {
			fprintf(stderr,
				"deleting extent record: key %llu %u %llu\n",
				key.objectid, key.type, key.offset);

			if (key.type == BTRFS_EXTENT_ITEM_KEY) {
				/* make sure this extent doesn't get
				 * reused for other purposes */
				btrfs_pin_extent(root->fs_info,
						 key.objectid, key.offset);
			}

			btrfs_del_item(trans, root, path);
		} else {
			fprintf(stderr,
				"corrupting extent record: key %llu %u %llu\n",
				key.objectid, key.type, key.offset);
			ptr = btrfs_item_ptr_offset(leaf, slot);
			item_size = btrfs_item_size_nr(leaf, slot);
			memset_extent_buffer(leaf, 0, ptr, item_size);
			btrfs_mark_buffer_dirty(leaf);
		}
next:
		btrfs_release_path(NULL, path);

		if (key.offset > 0)
			key.offset--;
		if (key.offset == 0)
			break;
	}

	btrfs_free_path(path);
	return 0;
}

static void btrfs_corrupt_extent_leaf(struct btrfs_trans_handle *trans,
				      struct btrfs_root *root,
				      struct extent_buffer *eb)
{
	u32 nr = btrfs_header_nritems(eb);
	u32 victim = rand() % nr;
	u64 objectid;
	struct btrfs_key key;

	btrfs_item_key_to_cpu(eb, &key, victim);
	objectid = key.objectid;
	corrupt_extent(trans, root, objectid, 1);
}

static void btrfs_corrupt_extent_tree(struct btrfs_trans_handle *trans,
				      struct btrfs_root *root,
				      struct extent_buffer *eb)
{
	int i;
	u32 nr;

	if (!eb)
		return;

	nr = btrfs_header_nritems(eb);
	if (btrfs_is_leaf(eb)) {
		btrfs_corrupt_extent_leaf(trans, root, eb);
		return;
	}

	if (btrfs_header_level(eb) == 1 && eb != root->node) {
		if (rand() % 5)
			return;
	}

	for (i = 0; i < nr; i++) {
		struct extent_buffer *next;

		next = read_tree_block(root, btrfs_node_blockptr(eb, i),
				       root->leafsize,
				       btrfs_node_ptr_generation(eb, i));
		if (!next)
			continue;
		btrfs_corrupt_extent_tree(trans, root, next);
		free_extent_buffer(next);
	}
}

static struct option long_options[] = {
	/* { "byte-count", 1, NULL, 'b' }, */
	{ "logical", 1, NULL, 'l' },
	{ "copy", 1, NULL, 'c' },
	{ "bytes", 1, NULL, 'b' },
	{ "extent-record", 0, NULL, 'e' },
	{ "extent-tree", 0, NULL, 'E' },
	{ "keys", 0, NULL, 'k' },
	{ "chunk-record", 0, NULL, 'u' },
	{ "chunk-tree", 0, NULL, 'U' },
	{ 0, 0, 0, 0}
};

/* corrupt item using NO cow.
 * Because chunk recover will recover based on whole partition scaning,
 * If using COW, chunk recover will use the old item to recover,
 * which is still OK but we want to check the ability to rebuild chunk
 * not only restore the old ones */
int corrupt_item_nocow(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root, struct btrfs_path *path,
		       int del)
{
	int ret = 0;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	unsigned long ptr;
	int slot;
	u32 item_size;

	leaf = path->nodes[0];
	slot = path->slots[0];
	/* Not deleting the first item of a leaf to keep leaf structure */
	if (slot == 0)
		del = 0;
	/* Only accept valid eb */
	BUG_ON(!leaf->data || slot >= btrfs_header_nritems(leaf));
	btrfs_item_key_to_cpu(leaf, &key, slot);
	if (del) {
		fprintf(stdout, "Deleting key and data [%llu, %u, %llu].\n",
			key.objectid, key.type, key.offset);
		btrfs_del_item(trans, root, path);
	} else {
		fprintf(stdout, "Corrupting key and data [%llu, %u, %llu].\n",
			key.objectid, key.type, key.offset);
		ptr = btrfs_item_ptr_offset(leaf, slot);
		item_size = btrfs_item_size_nr(leaf, slot);
		memset_extent_buffer(leaf, 0, ptr, item_size);
		btrfs_mark_buffer_dirty(leaf);
	}
	return ret;
}
int corrupt_chunk_tree(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root)
{
	int ret;
	int del;
	int slot;
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct extent_buffer *leaf;

	path = btrfs_alloc_path();
	key.objectid = (u64)-1;
	key.offset = (u64)-1;
	key.type = (u8)-1;

	/* Here, cow and ins_len must equals 0 for the following reasons:
	 * 1) chunk recover is based on disk scanning, so COW should be
	 *    disabled in case the original chunk being scanned and
	 *    recovered using the old chunk.
	 * 2) if cow = 0, ins_len must also be set to 0, or BUG_ON will be
	 *    triggered.
	 */
	ret = btrfs_search_slot(trans, root, &key, path, 0, 0);
	BUG_ON(ret == 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching tree\n");
		goto free_out;
	}
	/* corrupt/del dev_item first */
	while (!btrfs_previous_item(root, path, 0, BTRFS_DEV_ITEM_KEY)) {
		slot = path->slots[0];
		leaf = path->nodes[0];
		del = rand() % 3;
		/* Never delete the first item to keep the leaf structure */
		if (path->slots[0] == 0)
			del = 0;
		ret = corrupt_item_nocow(trans, root, path, del);
		if (ret)
			goto free_out;
	}
	btrfs_free_path(path);

	/* Here, cow and ins_len must equals 0 for the following reasons:
	 * 1) chunk recover is based on disk scanning, so COW should be
	 *    disabled in case the original chunk being scanned and
	 *    recovered using the old chunk.
	 * 2) if cow = 0, ins_len must also be set to 0, or BUG_ON will be
	 *    triggered.
	 */
	path = btrfs_alloc_path();
	ret = btrfs_search_slot(trans, root, &key, path, 0, 0);
	BUG_ON(ret == 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching tree\n");
		goto free_out;
	}
	/* corrupt/del chunk then*/
	while (!btrfs_previous_item(root, path, 0, BTRFS_CHUNK_ITEM_KEY)) {
		slot = path->slots[0];
		leaf = path->nodes[0];
		del = rand() % 3;
		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		ret = corrupt_item_nocow(trans, root, path, del);
		if (ret)
			goto free_out;
	}
free_out:
	btrfs_free_path(path);
	return ret;
}
int find_chunk_offset(struct btrfs_root *root,
		      struct btrfs_path *path, u64 offset)
{
	struct btrfs_key key;
	int ret;

	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = offset;

	/* Here, cow and ins_len must equals 0 for following reasons:
	 * 1) chunk recover is based on disk scanning, so COW should
	 *    be disabled in case the original chunk being scanned
	 *    and recovered using the old chunk.
	 * 2) if cow = 0, ins_len must also be set to 0, or BUG_ON
	 *    will be triggered.
	 */
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret > 0) {
		fprintf(stderr, "Can't find chunk with given offset %llu\n",
			offset);
		goto out;
	}
	if (ret < 0) {
		fprintf(stderr, "Error searching chunk");
		goto out;
	}
out:
	return ret;

}
int main(int ac, char **av)
{
	struct cache_tree root_cache;
	struct btrfs_root *root;
	struct extent_buffer *eb;
	char *dev;
	/* chunk offset can be 0,so change to (u64)-1 */
	u64 logical = (u64)-1;
	int ret = 0;
	int option_index = 0;
	int copy = 0;
	u64 bytes = 4096;
	int extent_rec = 0;
	int extent_tree = 0;
	int corrupt_block_keys = 0;
	int chunk_rec = 0;
	int chunk_tree = 0;

	srand(128);

	while(1) {
		int c;
		c = getopt_long(ac, av, "l:c:b:eEkuU", long_options,
				&option_index);
		if (c < 0)
			break;
		switch(c) {
			case 'l':
				logical = atoll(optarg);
				break;
			case 'c':
				copy = atoi(optarg);
				if (copy == 0) {
					fprintf(stderr,
						"invalid copy number\n");
					print_usage();
				}
				break;
			case 'b':
				bytes = atoll(optarg);
				if (bytes == 0) {
					fprintf(stderr,
						"invalid byte count\n");
					print_usage();
				}
				break;
			case 'e':
				extent_rec = 1;
				break;
			case 'E':
				extent_tree = 1;
				break;
			case 'k':
				corrupt_block_keys = 1;
				break;
			case 'u':
				chunk_rec = 1;
				break;
			case 'U':
				chunk_tree = 1;
				break;
			default:
				print_usage();
		}
	}
	ac = ac - optind;
	if (ac == 0)
		print_usage();
	if (logical == (u64)-1 && !(extent_tree || chunk_tree))
		print_usage();
	if (copy < 0)
		print_usage();

	dev = av[optind];

	radix_tree_init();
	cache_tree_init(&root_cache);

	root = open_ctree(dev, 0, 1);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}
	if (extent_rec) {
		struct btrfs_trans_handle *trans;
		trans = btrfs_start_transaction(root, 1);
		ret = corrupt_extent (trans, root, logical, 0);
		btrfs_commit_transaction(trans, root);
		goto out_close;
	}
	if (extent_tree) {
		struct btrfs_trans_handle *trans;
		trans = btrfs_start_transaction(root, 1);
		btrfs_corrupt_extent_tree(trans, root->fs_info->extent_root,
					  root->fs_info->extent_root->node);
		btrfs_commit_transaction(trans, root);
		goto out_close;
	}
	if (chunk_rec) {
		struct btrfs_trans_handle *trans;
		struct btrfs_path *path;
		int del;

		del = rand() % 3;
		path = btrfs_alloc_path();

		if (find_chunk_offset(root->fs_info->chunk_root, path,
				      logical) != 0) {
			btrfs_free_path(path);
			goto out_close;
		}
		trans = btrfs_start_transaction(root, 1);
		ret = corrupt_item_nocow(trans, root->fs_info->chunk_root,
					 path, del);
		if (ret < 0)
			fprintf(stderr, "Failed to corrupt chunk record\n");
		btrfs_commit_transaction(trans, root);
		goto out_close;
	}
	if (chunk_tree) {
		struct btrfs_trans_handle *trans;
		trans = btrfs_start_transaction(root, 1);
		ret = corrupt_chunk_tree(trans, root->fs_info->chunk_root);
		if (ret < 0)
			fprintf(stderr, "Failed to corrupt chunk tree\n");
		btrfs_commit_transaction(trans, root);
		goto out_close;
	}

	if (bytes == 0)
		bytes = root->sectorsize;

	bytes = (bytes + root->sectorsize - 1) / root->sectorsize;
	bytes *= root->sectorsize;

	while (bytes > 0) {
		if (corrupt_block_keys) {
			corrupt_keys_in_block(root, logical);
		} else {
			eb = debug_corrupt_block(root, logical,
						 root->sectorsize, copy);
			free_extent_buffer(eb);
		}
		logical += root->sectorsize;
		bytes -= root->sectorsize;
	}
	return ret;
out_close:
	close_ctree(root);
	return ret;
}
