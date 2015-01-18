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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
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

struct seek {
	u64 distance;
	u64 count;
	struct rb_node n;
};

struct root_stats {
	u64 total_nodes;
	u64 total_leaves;
	u64 total_bytes;
	u64 total_inline;
	u64 total_seeks;
	u64 forward_seeks;
	u64 backward_seeks;
	u64 total_seek_len;
	u64 max_seek_len;
	u64 total_clusters;
	u64 total_cluster_size;
	u64 min_cluster_size;
	u64 max_cluster_size;
	u64 lowest_bytenr;
	u64 highest_bytenr;
	struct rb_root seek_root;
	int total_levels;
};

struct fs_root {
	struct btrfs_key key;
	struct btrfs_key *snaps;
};

static int add_seek(struct rb_root *root, u64 dist)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct seek *seek = NULL;

	while (*p) {
		parent = *p;
		seek = rb_entry(parent, struct seek, n);

		if (dist < seek->distance) {
			p = &(*p)->rb_left;
		} else if (dist > seek->distance) {
			p = &(*p)->rb_right;
		} else {
			seek->count++;
			return 0;
		}
	}

	seek = malloc(sizeof(struct seek));
	if (!seek)
		return -ENOMEM;
	seek->distance = dist;
	seek->count = 1;
	rb_link_node(&seek->n, parent, p);
	rb_insert_color(&seek->n, root);
	return 0;
}

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
							btrfs_item_nr(i));
	}

	return 0;
}

static u64 calc_distance(u64 block1, u64 block2)
{
	if (block1 < block2)
		return block2 - block1;
	return block1 - block2;
}

static int walk_nodes(struct btrfs_root *root, struct btrfs_path *path,
		      struct root_stats *stat, int level, int find_inline)
{
	struct extent_buffer *b = path->nodes[level];
	u64 last_block;
	u64 cluster_size = root->leafsize;
	int i;
	int ret = 0;

	stat->total_bytes += root->nodesize;
	stat->total_nodes++;

	last_block = btrfs_header_bytenr(b);
	for (i = 0; i < btrfs_header_nritems(b); i++) {
		struct extent_buffer *tmp = NULL;
		u64 cur_blocknr = btrfs_node_blockptr(b, i);

		path->slots[level] = i;
		if ((level - 1) > 0 || find_inline) {
			tmp = read_tree_block(root, cur_blocknr,
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
		if (last_block + root->leafsize != cur_blocknr) {
			u64 distance = calc_distance(last_block +
						     root->leafsize,
						     cur_blocknr);
			stat->total_seeks++;
			stat->total_seek_len += distance;
			if (stat->max_seek_len < distance)
				stat->max_seek_len = distance;
			if (add_seek(&stat->seek_root, distance)) {
				fprintf(stderr, "Error adding new seek\n");
				ret = -ENOMEM;
				break;
			}

			if (last_block < cur_blocknr)
				stat->forward_seeks++;
			else
				stat->backward_seeks++;
			if (cluster_size != root->leafsize) {
				stat->total_cluster_size += cluster_size;
				stat->total_clusters++;
				if (cluster_size < stat->min_cluster_size)
					stat->min_cluster_size = cluster_size;
				if (cluster_size > stat->max_cluster_size)
					stat->max_cluster_size = cluster_size;
			}
			cluster_size = root->leafsize;
		} else {
			cluster_size += root->leafsize;
		}
		last_block = cur_blocknr;
		if (cur_blocknr < stat->lowest_bytenr)
			stat->lowest_bytenr = cur_blocknr;
		if (cur_blocknr > stat->highest_bytenr)
			stat->highest_bytenr = cur_blocknr;
		free_extent_buffer(tmp);
		if (ret) {
			fprintf(stderr, "Error walking down path\n");
			break;
		}
	}

	return ret;
}

static void print_seek_histogram(struct root_stats *stat)
{
	struct rb_node *n = rb_first(&stat->seek_root);
	struct seek *seek;
	u64 tick_interval;
	u64 group_start;
	u64 group_count = 0;
	u64 group_end;
	u64 i;
	u64 max_seek = stat->max_seek_len;
	int digits = 1;

	if (stat->total_seeks < 20)
		return;

	while ((max_seek /= 10))
		digits++;

	/* Make a tick count as 5% of the total seeks */
	tick_interval = stat->total_seeks / 20;
	printf("\tSeek histogram\n");
	for (; n; n = rb_next(n)) {
		u64 ticks, gticks = 0;

		seek = rb_entry(n, struct seek, n);
		ticks = seek->count / tick_interval;
		if (group_count)
			gticks = group_count / tick_interval;

		if (ticks <= 2 && gticks <= 2) {
			if (group_count == 0)
				group_start = seek->distance;
			group_end = seek->distance;
			group_count += seek->count;
			continue;
		}

		if (group_count) {

			gticks = group_count / tick_interval;
			printf("\t\t%*Lu - %*Lu: %*Lu ", digits, group_start,
			       digits, group_end, digits, group_count);
			if (gticks) {
				for (i = 0; i < gticks; i++)
					printf("#");
				printf("\n");
			} else {
				printf("|\n");
			}
			group_count = 0;
		}

		if (ticks <= 2)
			continue;

		printf("\t\t%*Lu - %*Lu: %*Lu ", digits, seek->distance,
		       digits, seek->distance, digits, seek->count);
		for (i = 0; i < ticks; i++)
			printf("#");
		printf("\n");
	}
	if (group_count) {
		u64 gticks;

		gticks = group_count / tick_interval;
		printf("\t\t%*Lu - %*Lu: %*Lu ", digits, group_start,
		       digits, group_end, digits, group_count);
		if (gticks) {
			for (i = 0; i < gticks; i++)
				printf("#");
			printf("\n");
		} else {
			printf("|\n");
		}
		group_count = 0;
	}
}

static void timeval_subtract(struct timeval *result,struct timeval *x,
			     struct timeval *y)
{
	if (x->tv_usec < y->tv_usec) {
		int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
		y->tv_usec -= 1000000 * nsec;
		y->tv_sec += nsec;
	}

	if (x->tv_usec - y->tv_usec > 1000000) {
		int nsec = (x->tv_usec - y->tv_usec) / 1000000;
		y->tv_usec += 1000000 * nsec;
		y->tv_sec -= nsec;
	}

	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_usec = x->tv_usec - y->tv_usec;
}

static int calc_root_size(struct btrfs_root *tree_root, struct btrfs_key *key,
			  int find_inline)
{
	struct btrfs_root *root;
	struct btrfs_path *path;
	struct rb_node *n;
	struct timeval start, end, diff = {0};
	struct root_stats stat;
	int level;
	int ret = 0;
	int size_fail = 0;

	root = btrfs_read_fs_root(tree_root->fs_info, key);
	if (IS_ERR(root)) {
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
	stat.lowest_bytenr = btrfs_header_bytenr(root->node);
	stat.highest_bytenr = stat.lowest_bytenr;
	stat.min_cluster_size = (u64)-1;
	stat.max_cluster_size = root->leafsize;
	path->nodes[level] = root->node;
	if (gettimeofday(&start, NULL)) {
		fprintf(stderr, "Error getting time: %d\n", errno);
		goto out;
	}
	if (!level) {
		ret = walk_leaf(root, path, &stat, find_inline);
		if (ret)
			goto out;
		goto out_print;
	}

	ret = walk_nodes(root, path, &stat, level, find_inline);
	if (ret)
		goto out;
	if (gettimeofday(&end, NULL)) {
		fprintf(stderr, "Error getting time: %d\n", errno);
		goto out;
	}
	timeval_subtract(&diff, &end, &start);
out_print:
	if (stat.min_cluster_size == (u64)-1) {
		stat.min_cluster_size = 0;
		stat.total_clusters = 1;
	}

	if (no_pretty || size_fail) {
		printf("\tTotal size: %Lu\n", stat.total_bytes);
		printf("\t\tInline data: %Lu\n", stat.total_inline);
		printf("\tTotal seeks: %Lu\n", stat.total_seeks);
		printf("\t\tForward seeks: %Lu\n", stat.forward_seeks);
		printf("\t\tBackward seeks: %Lu\n", stat.backward_seeks);
		printf("\t\tAvg seek len: %Lu\n", stat.total_seek_len /
		       stat.total_seeks);
		print_seek_histogram(&stat);
		printf("\tTotal clusters: %Lu\n", stat.total_clusters);
		printf("\t\tAvg cluster size: %Lu\n", stat.total_cluster_size /
		       stat.total_clusters);
		printf("\t\tMin cluster size: %Lu\n", stat.min_cluster_size);
		printf("\t\tMax cluster size: %Lu\n", stat.max_cluster_size);
		printf("\tTotal disk spread: %Lu\n", stat.highest_bytenr -
		       stat.lowest_bytenr);
		printf("\tTotal read time: %d s %d us\n", (int)diff.tv_sec,
		       (int)diff.tv_usec);
		printf("\tLevels: %d\n", level + 1);
	} else {
		printf("\tTotal size: %s\n", pretty_size(stat.total_bytes));
		printf("\t\tInline data: %s\n", pretty_size(stat.total_inline));
		printf("\tTotal seeks: %Lu\n", stat.total_seeks);
		printf("\t\tForward seeks: %Lu\n", stat.forward_seeks);
		printf("\t\tBackward seeks: %Lu\n", stat.backward_seeks);
		printf("\t\tAvg seek len: %s\n", stat.total_seeks ?
			pretty_size(stat.total_seek_len / stat.total_seeks) :
			pretty_size(0));
		print_seek_histogram(&stat);
		printf("\tTotal clusters: %Lu\n", stat.total_clusters);
		printf("\t\tAvg cluster size: %s\n",
				pretty_size((stat.total_cluster_size /
						stat.total_clusters)));
		printf("\t\tMin cluster size: %s\n",
				pretty_size(stat.min_cluster_size));
		printf("\t\tMax cluster size: %s\n",
				pretty_size(stat.max_cluster_size));
		printf("\tTotal disk spread: %s\n",
				pretty_size(stat.highest_bytenr -
					stat.lowest_bytenr));
		printf("\tTotal read time: %d s %d us\n", (int)diff.tv_sec,
		       (int)diff.tv_usec);
		printf("\tLevels: %d\n", level + 1);
	}
out:
	while ((n = rb_first(&stat.seek_root)) != NULL) {
		struct seek *seek = rb_entry(n, struct seek, n);
		rb_erase(n, &stat.seek_root);
		free(seek);
	}

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

	set_argv0(argv);
	argc = argc - optind;
	if (check_argc_min(argc, 1)) {
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
	free(roots);
	return ret;
}
