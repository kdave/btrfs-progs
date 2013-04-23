/*
 * Copyright (C) 2011 Red Hat.  All rights reserved.
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
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"
#include "volumes.h"
#include "utils.h"

static int verbose = 0;
static int no_pretty = 0;

struct root_stats {
	u64 total_nodes;
	u64 total_leaves;
	u64 total_bytes;
	u64 total_inline;
	int total_levels;
};

struct fs_root {
	struct btrfs_key key;
	struct btrfs_key *snaps;
};

static int walk_leaf(struct btrfs_root *root, struct btrfs_path *path,
		     struct root_stats *stat, int find_inline)
{
	struct extent_buffer *b = path->nodes[0];
	struct btrfs_file_extent_item *fi;
	struct btrfs_key found_key;
	int i;

	stat->total_bytes += root->leafsize;
	stat->total_leaves++;

	if (!find_inline)
		return 0;

	for (i = 0; i < btrfs_header_nritems(b); i++) {
		btrfs_item_key_to_cpu(b, &found_key, i);
		if (found_key.type != BTRFS_EXTENT_DATA_KEY)
			continue;

		fi = btrfs_item_ptr(b, i, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(b, fi) == BTRFS_FILE_EXTENT_INLINE)
			stat->total_inline +=
				btrfs_file_extent_inline_item_len(b,
							btrfs_item_nr(b, i));
	}

	return 0;
}

static int walk_nodes(struct btrfs_root *root, struct btrfs_path *path,
		      struct root_stats *stat, int level, int find_inline)
{
	struct extent_buffer *b = path->nodes[level];
	int i;
	int ret = 0;

	stat->total_bytes += root->nodesize;
	stat->total_nodes++;

	for (i = 0; i < btrfs_header_nritems(b); i++) {
		struct extent_buffer *tmp = NULL;

		path->slots[level] = i;
		if ((level - 1) > 0 || find_inline) {
			tmp = read_tree_block(root, btrfs_node_blockptr(b, i),
					      btrfs_level_size(root, level - 1),
					      btrfs_node_ptr_generation(b, i));
			if (!tmp) {
				fprintf(stderr, "Failed to read blocknr %Lu\n",
					btrfs_node_blockptr(b, i));
				continue;
			}
			path->nodes[level - 1] = tmp;
		}
		if (level - 1)
			ret = walk_nodes(root, path, stat, level - 1,
					 find_inline);
		else
			ret = walk_leaf(root, path, stat, find_inline);
		free_extent_buffer(tmp);
		if (ret) {
			fprintf(stderr, "Error walking down path\n");
			break;
		}
	}

	return ret;
}

static int calc_root_size(struct btrfs_root *tree_root, struct btrfs_key *key,
			  int find_inline)
{
	struct btrfs_root *root;
	struct btrfs_path *path;
	struct root_stats stat;
	int level;
	int ret = 0;
	int size_fail = 0;

	root = btrfs_read_fs_root(tree_root->fs_info, key);
	if (!root) {
		fprintf(stderr, "Failed to read root %Lu\n", key->objectid);
		return 1;
	}

	path = btrfs_alloc_path();
	if (!path) {
		fprintf(stderr, "Could not allocate path\n");
		return 1;
	}

	memset(&stat, 0, sizeof(stat));
	level = btrfs_header_level(root->node);
	path->nodes[level] = root->node;
	if (!level) {
		ret = walk_leaf(root, path, &stat, find_inline);
		if (ret)
			goto out;
		goto out_print;
	}

	ret = walk_nodes(root, path, &stat, level, find_inline);
	if (ret)
		goto out;
out_print:
	if (no_pretty || size_fail) {
		printf("\t%Lu total bytes, %Lu inline data bytes, %Lu nodes, "
		       "%Lu leaves, %d levels\n", stat.total_bytes,
		       stat.total_inline, stat.total_nodes, stat.total_leaves,
		       level + 1);
	} else {
		char *total_size;
		char *inline_size;

		total_size = pretty_sizes(stat.total_bytes);
		inline_size = pretty_sizes(stat.total_inline);

		printf("\t%s total size, %s inline data, %Lu nodes, "
		       "%Lu leaves, %d levels\n",
		       total_size, inline_size, stat.total_nodes,
		       stat.total_leaves, level + 1);
		free(total_size);
		free(inline_size);
	}
out:
	btrfs_free_path(path);
	return ret;
}

static void usage()
{
	fprintf(stderr, "Usage: calc-size [-v] [-b] <device>\n");
}

int main(int argc, char **argv)
{
	struct btrfs_key key;
	struct fs_root *roots;
	struct btrfs_root *root;
	size_t fs_roots_size = sizeof(struct fs_root);
	int opt;
	int ret = 0;

	while ((opt = getopt(argc, argv, "vb")) != -1) {
		switch (opt) {
			case 'v':
				verbose++;
				break;
			case 'b':
				no_pretty = 1;
				break;
			default:
				usage();
				exit(1);
		}
	}

	if (optind >= argc) {
		usage();
		exit(1);
	}

	/*
	if ((ret = check_mounted(argv[optind])) < 0) {
		fprintf(stderr, "Could not check mount status: %d\n", ret);
		if (ret == -EACCES)
			fprintf(stderr, "Maybe you need to run as root?\n");
		return ret;
	} else if (ret) {
		fprintf(stderr, "%s is currently mounted.  Aborting.\n",
			argv[optind]);
		return -EBUSY;
	}
	*/

	root = open_ctree(argv[optind], 0, 0);
	if (!root) {
		fprintf(stderr, "Couldn't open ctree\n");
		exit(1);
	}

	roots = malloc(fs_roots_size);
	if (!roots) {
		fprintf(stderr, "No memory\n");
		goto out;
	}

	printf("Calculating size of root tree\n");
	key.objectid = BTRFS_ROOT_TREE_OBJECTID;
	ret = calc_root_size(root, &key, 0);
	if (ret)
		goto out;

	printf("Calculating size of extent tree\n");
	key.objectid = BTRFS_EXTENT_TREE_OBJECTID;
	ret = calc_root_size(root, &key, 0);
	if (ret)
		goto out;

	printf("Calculating size of csum tree\n");
	key.objectid = BTRFS_CSUM_TREE_OBJECTID;
	ret = calc_root_size(root, &key, 0);
	if (ret)
		goto out;

	roots[0].key.objectid = BTRFS_FS_TREE_OBJECTID;
	roots[0].key.offset = (u64)-1;
	printf("Calculatin' size of fs tree\n");
	ret = calc_root_size(root, &roots[0].key, 1);
	if (ret)
		goto out;
out:
	close_ctree(root);
	return ret;
}
