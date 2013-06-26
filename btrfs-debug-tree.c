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
#include <unistd.h>
#include <uuid/uuid.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "version.h"

static int print_usage(void)
{
	fprintf(stderr, "usage: btrfs-debug-tree [-e] [-d] [-r] [-R] [-u]\n");
	fprintf(stderr, "                        [-b block_num ] device\n");
	fprintf(stderr, "\t-e : print detailed extents info\n");
	fprintf(stderr, "\t-d : print info of btrfs device and root tree dirs"
                    " only\n");
	fprintf(stderr, "\t-r : print info of roots only\n");
	fprintf(stderr, "\t-R : print info of roots and root backups\n");
	fprintf(stderr, "\t-u : print info of uuid tree only\n");
	fprintf(stderr, "\t-b block_num : print info of the specified block"
                    " only\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

static void print_extents(struct btrfs_root *root, struct extent_buffer *eb)
{
	int i;
	u32 nr;
	u32 size;

	if (!eb)
		return;

	if (btrfs_is_leaf(eb)) {
		btrfs_print_leaf(root, eb);
		return;
	}

	size = btrfs_level_size(root, btrfs_header_level(eb) - 1);
	nr = btrfs_header_nritems(eb);
	for (i = 0; i < nr; i++) {
		struct extent_buffer *next = read_tree_block(root,
					     btrfs_node_blockptr(eb, i),
					     size,
					     btrfs_node_ptr_generation(eb, i));
		if (btrfs_is_leaf(next) &&
		    btrfs_header_level(eb) != 1)
			BUG();
		if (btrfs_header_level(next) !=
			btrfs_header_level(eb) - 1)
			BUG();
		print_extents(root, next);
		free_extent_buffer(next);
	}
}

static void print_old_roots(struct btrfs_super_block *super)
{
	struct btrfs_root_backup *backup;
	int i;

	for (i = 0; i < BTRFS_NUM_BACKUP_ROOTS; i++) {
		backup = super->super_roots + i;
		printf("btrfs root backup slot %d\n", i);
		printf("\ttree root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_tree_root_gen(backup),
		       (unsigned long long)btrfs_backup_tree_root(backup));

		printf("\t\textent root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_extent_root_gen(backup),
		       (unsigned long long)btrfs_backup_extent_root(backup));

		printf("\t\tchunk root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_chunk_root_gen(backup),
		       (unsigned long long)btrfs_backup_chunk_root(backup));

		printf("\t\tdevice root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_dev_root_gen(backup),
		       (unsigned long long)btrfs_backup_dev_root(backup));

		printf("\t\tcsum root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_csum_root_gen(backup),
		       (unsigned long long)btrfs_backup_csum_root(backup));

		printf("\t\tfs root gen %llu block %llu\n",
		       (unsigned long long)btrfs_backup_fs_root_gen(backup),
		       (unsigned long long)btrfs_backup_fs_root(backup));

		printf("\t\t%llu used %llu total %llu devices\n",
		       (unsigned long long)btrfs_backup_bytes_used(backup),
		       (unsigned long long)btrfs_backup_total_bytes(backup),
		       (unsigned long long)btrfs_backup_num_devices(backup));
	}
}

int main(int ac, char **av)
{
	struct btrfs_root *root;
	struct btrfs_fs_info *info;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root_item ri;
	struct extent_buffer *leaf;
	struct btrfs_disk_key disk_key;
	struct btrfs_key found_key;
	char uuidbuf[37];
	int ret;
	int slot;
	int extent_only = 0;
	int device_only = 0;
	int uuid_tree_only = 0;
	int roots_only = 0;
	int root_backups = 0;
	u64 block_only = 0;
	struct btrfs_root *tree_root_scan;

	radix_tree_init();

	while(1) {
		int c;
		c = getopt(ac, av, "deb:rRu");
		if (c < 0)
			break;
		switch(c) {
			case 'e':
				extent_only = 1;
				break;
			case 'd':
				device_only = 1;
				break;
			case 'r':
				roots_only = 1;
				break;
			case 'u':
				uuid_tree_only = 1;
				break;
			case 'R':
				roots_only = 1;
				root_backups = 1;
				break;
			case 'b':
				block_only = atoll(optarg);
				break;
			default:
				print_usage();
		}
	}
	ac = ac - optind;
	if (ac != 1)
		print_usage();

	info = open_ctree_fs_info(av[optind], 0, 0, 0, 1);
	if (!info) {
		fprintf(stderr, "unable to open %s\n", av[optind]);
		exit(1);
	}
	root = info->fs_root;

	if (block_only) {
		if (!root) {
			fprintf(stderr, "unable to open %s\n", av[optind]);
			exit(1);
		}
		leaf = read_tree_block(root,
				      block_only,
				      root->leafsize, 0);

		if (leaf && btrfs_header_level(leaf) != 0) {
			free_extent_buffer(leaf);
			leaf = NULL;
		}

		if (!leaf) {
			leaf = read_tree_block(root,
					      block_only,
					      root->nodesize, 0);
		}
		if (!leaf) {
			fprintf(stderr, "failed to read %llu\n",
				(unsigned long long)block_only);
			goto close_root;
		}
		btrfs_print_tree(root, leaf, 0);
		goto close_root;
	}

	if (!(extent_only || uuid_tree_only)) {
		if (roots_only) {
			printf("root tree: %llu level %d\n",
			     (unsigned long long)info->tree_root->node->start,
			     btrfs_header_level(info->tree_root->node));
			printf("chunk tree: %llu level %d\n",
			     (unsigned long long)info->chunk_root->node->start,
			     btrfs_header_level(info->chunk_root->node));
		} else {
			if (info->tree_root->node) {
				printf("root tree\n");
				btrfs_print_tree(info->tree_root,
						 info->tree_root->node, 1);
			}

			if (info->chunk_root->node) {
				printf("chunk tree\n");
				btrfs_print_tree(info->chunk_root,
						 info->chunk_root->node, 1);
			}
		}
	}
	tree_root_scan = info->tree_root;

	btrfs_init_path(&path);
again:
	if (!extent_buffer_uptodate(tree_root_scan->node))
		goto no_node;

	key.offset = 0;
	key.objectid = 0;
	btrfs_set_key_type(&key, BTRFS_ROOT_ITEM_KEY);
	ret = btrfs_search_slot(NULL, tree_root_scan, &key, &path, 0, 0);
	BUG_ON(ret < 0);
	while(1) {
		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(tree_root_scan, &path);
			if (ret != 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key(leaf, &disk_key, path.slots[0]);
		btrfs_disk_key_to_cpu(&found_key, &disk_key);
		if (btrfs_key_type(&found_key) == BTRFS_ROOT_ITEM_KEY) {
			unsigned long offset;
			struct extent_buffer *buf;
			int skip = extent_only | device_only | uuid_tree_only;

			offset = btrfs_item_ptr_offset(leaf, slot);
			read_extent_buffer(leaf, &ri, offset, sizeof(ri));
			buf = read_tree_block(tree_root_scan,
					      btrfs_root_bytenr(&ri),
					      btrfs_level_size(tree_root_scan,
							btrfs_root_level(&ri)),
					      0);
			if (!extent_buffer_uptodate(buf))
				goto next;

			switch(found_key.objectid) {
			case BTRFS_ROOT_TREE_OBJECTID:
				if (!skip)
					printf("root");
				break;
			case BTRFS_EXTENT_TREE_OBJECTID:
				if (!device_only && !uuid_tree_only)
					skip = 0;
				if (!skip)
					printf("extent");
				break;
			case BTRFS_CHUNK_TREE_OBJECTID:
				if (!skip) {
					printf("chunk");
				}
				break;
			case BTRFS_DEV_TREE_OBJECTID:
				if (!uuid_tree_only)
					skip = 0;
				if (!skip)
					printf("device");
				break;
			case BTRFS_FS_TREE_OBJECTID:
				if (!skip) {
					printf("fs");
				}
				break;
			case BTRFS_ROOT_TREE_DIR_OBJECTID:
				skip = 0;
				printf("directory");
				break;
			case BTRFS_CSUM_TREE_OBJECTID:
				if (!skip) {
					printf("checksum");
				}
				break;
			case BTRFS_ORPHAN_OBJECTID:
				if (!skip) {
					printf("orphan");
				}
				break;
			case BTRFS_TREE_LOG_OBJECTID:
				if (!skip) {
					printf("log");
				}
				break;
			case BTRFS_TREE_LOG_FIXUP_OBJECTID:
				if (!skip) {
					printf("log fixup");
				}
				break;
			case BTRFS_TREE_RELOC_OBJECTID:
				if (!skip) {
					printf("reloc");
				}
				break;
			case BTRFS_DATA_RELOC_TREE_OBJECTID:
				if (!skip) {
					printf("data reloc");
				}
				break;
			case BTRFS_EXTENT_CSUM_OBJECTID:
				if (!skip) {
					printf("extent checksum");
				}
				break;
			case BTRFS_QUOTA_TREE_OBJECTID:
				if (!skip) {
					printf("quota");
				}
				break;
			case BTRFS_UUID_TREE_OBJECTID:
				if (!extent_only && !device_only)
					skip = 0;
				if (!skip)
					printf("uuid");
				break;
			case BTRFS_MULTIPLE_OBJECTIDS:
				if (!skip) {
					printf("multiple");
				}
				break;
			default:
				if (!skip) {
					printf("file");
				}
			}
			if (extent_only && !skip) {
				print_extents(tree_root_scan, buf);
			} else if (!skip) {
				printf(" tree ");
				btrfs_print_key(&disk_key);
				if (roots_only) {
					printf(" %llu level %d\n",
					       (unsigned long long)buf->start,
					       btrfs_header_level(buf));
				} else {
					printf(" \n");
					btrfs_print_tree(tree_root_scan, buf, 1);
				}
			}
			free_extent_buffer(buf);
		}
next:
		path.slots[0]++;
	}
no_node:
	btrfs_release_path(root, &path);

	if (tree_root_scan == info->tree_root &&
	    info->log_root_tree) {
		tree_root_scan = info->log_root_tree;
		goto again;
	}

	if (extent_only || device_only || uuid_tree_only)
		goto close_root;

	if (root_backups)
		print_old_roots(info->super_copy);

	printf("total bytes %llu\n",
	       (unsigned long long)btrfs_super_total_bytes(info->super_copy));
	printf("bytes used %llu\n",
	       (unsigned long long)btrfs_super_bytes_used(info->super_copy));
	uuidbuf[36] = '\0';
	uuid_unparse(info->super_copy->fsid, uuidbuf);
	printf("uuid %s\n", uuidbuf);
	printf("%s\n", BTRFS_BUILD_VERSION);
close_root:
	return close_ctree(root);
}
