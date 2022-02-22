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
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/transaction.h"
#include "kernel-lib/list.h"
#include "kernel-shared/volumes.h"
#include "common/utils.h"
#include "cmds/commands.h"
#include "common/help.h"
#include "common/open-utils.h"
#include "common/units.h"

static int verbose = 0;
static int no_pretty = 0;

struct seek {
	u64 distance;
	u64 count;
	struct rb_node n;
};

struct root_stats {
	u64 total_nodes;
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
	u64 node_counts[BTRFS_MAX_LEVEL];
	struct rb_root seek_root;
	int total_levels;
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

	stat->total_bytes += root->fs_info->nodesize;
	stat->total_nodes++;
	stat->node_counts[0]++;

	if (!find_inline)
		return 0;

	for (i = 0; i < btrfs_header_nritems(b); i++) {
		btrfs_item_key_to_cpu(b, &found_key, i);
		if (found_key.type != BTRFS_EXTENT_DATA_KEY)
			continue;

		fi = btrfs_item_ptr(b, i, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(b, fi) == BTRFS_FILE_EXTENT_INLINE)
			stat->total_inline +=
				btrfs_file_extent_inline_item_len(b, i);
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
	u32 nodesize = root->fs_info->nodesize;
	u64 last_block;
	u64 cluster_size = nodesize;
	int i;
	int ret = 0;

	stat->total_bytes += nodesize;
	stat->total_nodes++;
	stat->node_counts[level]++;

	last_block = btrfs_header_bytenr(b);
	for (i = 0; i < btrfs_header_nritems(b); i++) {
		struct extent_buffer *tmp = NULL;
		u64 cur_blocknr = btrfs_node_blockptr(b, i);

		path->slots[level] = i;
		if ((level - 1) > 0 || find_inline) {
			tmp = read_tree_block(root->fs_info, cur_blocknr,
					      btrfs_node_ptr_generation(b, i));
			if (!extent_buffer_uptodate(tmp)) {
				error("failed to read blocknr %llu",
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
		if (last_block + nodesize != cur_blocknr) {
			u64 distance = calc_distance(last_block +
						     nodesize,
						     cur_blocknr);
			stat->total_seeks++;
			stat->total_seek_len += distance;
			if (stat->max_seek_len < distance)
				stat->max_seek_len = distance;
			if (add_seek(&stat->seek_root, distance)) {
				error("cannot add new seek at distance %llu",
						(unsigned long long)distance);
				ret = -ENOMEM;
				break;
			}

			if (last_block < cur_blocknr)
				stat->forward_seeks++;
			else
				stat->backward_seeks++;
			if (cluster_size != nodesize) {
				stat->total_cluster_size += cluster_size;
				stat->total_clusters++;
				if (cluster_size < stat->min_cluster_size)
					stat->min_cluster_size = cluster_size;
				if (cluster_size > stat->max_cluster_size)
					stat->max_cluster_size = cluster_size;
			}
			cluster_size = nodesize;
		} else {
			cluster_size += nodesize;
		}
		last_block = cur_blocknr;
		if (cur_blocknr < stat->lowest_bytenr)
			stat->lowest_bytenr = cur_blocknr;
		if (cur_blocknr > stat->highest_bytenr)
			stat->highest_bytenr = cur_blocknr;
		free_extent_buffer(tmp);
		if (ret) {
			error("walking down path failed: %d",  ret);
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
	u64 group_start = 0;
	u64 group_count = 0;
	u64 group_end = 0;
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
			printf("\t\t%*llu - %*llu: %*llu ", digits, group_start,
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

		printf("\t\t%*llu - %*llu: %*llu ", digits, seek->distance,
		       digits, seek->distance, digits, seek->count);
		for (i = 0; i < ticks; i++)
			printf("#");
		printf("\n");
	}
	if (group_count) {
		u64 gticks;

		gticks = group_count / tick_interval;
		printf("\t\t%*llu - %*llu: %*llu ", digits, group_start,
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

static void timeval_subtract(struct timeval *result, struct timeval *x,
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
	struct btrfs_path path;
	struct rb_node *n;
	struct timeval start, end, diff = {0};
	struct root_stats stat;
	int level;
	int ret = 0;
	int size_fail = 0;
	int i;

	root = btrfs_read_fs_root(tree_root->fs_info, key);
	if (IS_ERR(root)) {
		error("failed to read root %llu", key->objectid);
		return 1;
	}

	btrfs_init_path(&path);
	memset(&stat, 0, sizeof(stat));
	level = btrfs_header_level(root->node);
	stat.lowest_bytenr = btrfs_header_bytenr(root->node);
	stat.highest_bytenr = stat.lowest_bytenr;
	stat.min_cluster_size = (u64)-1;
	stat.max_cluster_size = root->fs_info->nodesize;
	path.nodes[level] = root->node;
	if (gettimeofday(&start, NULL)) {
		error("cannot get time: %m");
		goto out;
	}
	if (!level) {
		ret = walk_leaf(root, &path, &stat, find_inline);
		if (ret)
			goto out;
		goto out_print;
	}

	ret = walk_nodes(root, &path, &stat, level, find_inline);
	if (ret)
		goto out;
	if (gettimeofday(&end, NULL)) {
		error("cannot get time: %m");
		goto out;
	}
	timeval_subtract(&diff, &end, &start);
out_print:
	if (stat.min_cluster_size == (u64)-1) {
		stat.min_cluster_size = 0;
		stat.total_clusters = 1;
	}

	if (no_pretty || size_fail) {
		printf("\tTotal size: %llu\n", stat.total_bytes);
		printf("\t\tInline data: %llu\n", stat.total_inline);
		printf("\tTotal seeks: %llu\n", stat.total_seeks);
		printf("\t\tForward seeks: %llu\n", stat.forward_seeks);
		printf("\t\tBackward seeks: %llu\n", stat.backward_seeks);
		printf("\t\tAvg seek len: %llu\n", stat.total_seeks ?
			stat.total_seek_len / stat.total_seeks : 0);
		print_seek_histogram(&stat);
		printf("\tTotal clusters: %llu\n", stat.total_clusters);
		printf("\t\tAvg cluster size: %llu\n", stat.total_cluster_size /
		       stat.total_clusters);
		printf("\t\tMin cluster size: %llu\n", stat.min_cluster_size);
		printf("\t\tMax cluster size: %llu\n", stat.max_cluster_size);
		printf("\tTotal disk spread: %llu\n", stat.highest_bytenr -
		       stat.lowest_bytenr);
		printf("\tTotal read time: %d s %d us\n", (int)diff.tv_sec,
		       (int)diff.tv_usec);
	} else {
		printf("\tTotal size: %s\n", pretty_size(stat.total_bytes));
		printf("\t\tInline data: %s\n", pretty_size(stat.total_inline));
		printf("\tTotal seeks: %llu\n", stat.total_seeks);
		printf("\t\tForward seeks: %llu\n", stat.forward_seeks);
		printf("\t\tBackward seeks: %llu\n", stat.backward_seeks);
		printf("\t\tAvg seek len: %s\n", stat.total_seeks ?
			pretty_size(stat.total_seek_len / stat.total_seeks) :
			pretty_size(0));
		print_seek_histogram(&stat);
		printf("\tTotal clusters: %llu\n", stat.total_clusters);
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
	}
	printf("\tLevels: %d\n", level + 1);
	printf("\tTotal nodes: %llu\n", stat.total_nodes);
	for (i = 0; i < level + 1; i++) {
		printf("\t\tOn level %d: %8llu", i, stat.node_counts[i]);
		if (i > 0) {
			u64 fanout;

			fanout = stat.node_counts[i - 1];
			fanout /= stat.node_counts[i];
			printf("  (avg fanout %llu)", fanout);
		}
		printf("\n");
	}
out:
	while ((n = rb_first(&stat.seek_root)) != NULL) {
		struct seek *seek = rb_entry(n, struct seek, n);
		rb_erase(n, &stat.seek_root);
		free(seek);
	}

	/*
	 * We only use path to save node data in iterating, without holding
	 * eb's ref_cnt in path.  Don't use btrfs_release_path() here, it will
	 * free these eb again, and cause many problems, as negative ref_cnt or
	 * invalid memory access.
	 */
	return ret;
}

static const char * const cmd_inspect_tree_stats_usage[] = {
	"btrfs inspect-internal tree-stats [options] <device>",
	"Print various stats for trees",
	"",
	"-b		raw numbers in bytes",
	NULL
};

static int cmd_inspect_tree_stats(const struct cmd_struct *cmd,
				  int argc, char **argv)
{
	struct btrfs_key key = { .type = BTRFS_ROOT_ITEM_KEY };
	struct btrfs_root *root;
	int opt;
	int ret = 0;

	optind = 0;
	while ((opt = getopt(argc, argv, "vb")) != -1) {
		switch (opt) {
		case 'v':
			verbose++;
			break;
		case 'b':
			no_pretty = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	ret = check_mounted(argv[optind]);
	if (ret < 0) {
		errno = -ret;
		warning("unable to check mount status of: %m");
	} else if (ret) {
		warning("%s already mounted, results may be inaccurate",
				argv[optind]);
	}

	root = open_ctree(argv[optind], 0, 0);
	if (!root) {
		error("cannot open ctree");
		exit(1);
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

	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.offset = (u64)-1;
	printf("Calculating size of fs tree\n");
	ret = calc_root_size(root, &key, 1);
	if (ret)
		goto out;
out:
	close_ctree(root);
	return ret;
}
DEFINE_SIMPLE_COMMAND(inspect_tree_stats, "tree-stats");
