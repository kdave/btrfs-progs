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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>
#include <getopt.h>

#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "volumes.h"
#include "utils.h"
#include "crc32c.h"
#include "extent-cache.h"
#include "find-root.h"

static void usage(void)
{
	fprintf(stderr, "Usage: find-roots [-a] [-o search_objectid] "
		"[ -g search_generation ] [ -l search_level ] <device>\n");
}

/*
 * Get reliable generation and level for given root.
 *
 * We have two sources of gen/level: superblock and tree root.
 * superblock include the following level:
 *   Root, chunk, log
 * and the following generations:
 *   Root, chunk, uuid
 * Other gen/leven can only be read from its btrfs_tree_root if possible.
 *
 * Currently we only believe things from superblock.
 */
static void get_root_gen_and_level(u64 objectid, struct btrfs_fs_info *fs_info,
				   u64 *ret_gen, u8 *ret_level)
{
	struct btrfs_super_block *super = fs_info->super_copy;
	u64 gen = (u64)-1;
	u8 level = (u8)-1;

	switch (objectid) {
	case BTRFS_ROOT_TREE_OBJECTID:
		level = btrfs_super_root_level(super);
		gen = btrfs_super_generation(super);
		break;
	case BTRFS_CHUNK_TREE_OBJECTID:
		level = btrfs_super_chunk_root_level(super);
		gen = btrfs_super_chunk_root_generation(super);
		break;
	case BTRFS_TREE_LOG_OBJECTID:
		level = btrfs_super_log_root_level(super);
		gen = btrfs_super_log_root_transid(super);
		break;
	case BTRFS_UUID_TREE_OBJECTID:
		gen = btrfs_super_uuid_tree_generation(super);
		break;
	}
	if (gen != (u64)-1) {
		printf("Superblock thinks the generation is %llu\n", gen);
		if (ret_gen)
			*ret_gen = gen;
	} else {
		printf("Superblock doesn't contain generation info for root %llu\n",
		       objectid);
	}
	if (level != (u8)-1) {
		printf("Superblock thinks the level is %u\n", level);
		if (ret_level)
			*ret_level = level;
	} else {
		printf("Superblock doesn't contain the level info for root %llu\n",
		       objectid);
	}
}

static void print_one_result(struct cache_extent *tree_block,
			     u8 level, u64 generation,
			     struct btrfs_find_root_filter *filter)
{
	int unsure = 0;

	if (filter->match_gen == (u64)-1 || filter->match_level == (u8)-1)
		unsure = 1;
	printf("Well block %llu(gen: %llu level: %u) seems good, ",
	       tree_block->start, generation, level);
	if (unsure)
		printf("but we are unsure about the correct generation/level\n");
	else if (level == filter->match_level &&
		 generation == filter->match_gen)
		printf("and it matches superblock\n");
	else
		printf("but generation/level doesn't match, want gen: %llu level: %u\n",
		       filter->match_gen, filter->match_level);
}

static void print_find_root_result(struct cache_tree *result,
				   struct btrfs_find_root_filter *filter)
{
	struct btrfs_find_root_gen_cache *gen_cache;
	struct cache_extent *cache;
	struct cache_extent *tree_block;
	u64 generation = 0;
	u8 level = 0;

	for (cache = last_cache_extent(result);
	     cache; cache = prev_cache_extent(cache)) {
		gen_cache = container_of(cache,
				struct btrfs_find_root_gen_cache, cache);
		level = gen_cache->highest_level;
		generation = cache->start;
		/* For exact found one, skip it as it's output before */
		if (level == filter->match_level &&
		    generation == filter->match_gen &&
		    !filter->search_all)
			continue;
		for (tree_block = last_cache_extent(&gen_cache->eb_tree);
		     tree_block; tree_block = prev_cache_extent(tree_block))
			print_one_result(tree_block, level, generation, filter);
	}
}

int main(int argc, char **argv)
{
	struct btrfs_fs_info *fs_info;
	struct btrfs_find_root_filter filter = {0};
	struct cache_tree result;
	struct cache_extent *found;
	int ret;

	/* Default to search root tree */
	filter.objectid = BTRFS_ROOT_TREE_OBJECTID;
	filter.match_gen = (u64)-1;
	filter.match_level = (u8)-1;
	while (1) {
		static const struct option long_options[] = {
			{ "help", no_argument, NULL, GETOPT_VAL_HELP},
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "al:o:g:", long_options, NULL);

		if (c < 0)
			break;

		switch (c) {
		case 'a':
			filter.search_all = 1;
			break;
		case 'o':
			filter.objectid = arg_strtou64(optarg);
			break;
		case 'g':
			filter.generation = arg_strtou64(optarg);
			break;
		case 'l':
			filter.level = arg_strtou64(optarg);
			break;
		case GETOPT_VAL_HELP:
		default:
			usage();
			exit(c != GETOPT_VAL_HELP);
		}
	}

	set_argv0(argv);
	if (check_argc_min(argc - optind, 1)) {
		usage();
		exit(1);
	}

	fs_info = open_ctree_fs_info(argv[optind], 0, 0, 0,
			OPEN_CTREE_CHUNK_ROOT_ONLY |
			OPEN_CTREE_IGNORE_CHUNK_TREE_ERROR);
	if (!fs_info) {
		error("open ctree failed");
		exit(1);
	}
	cache_tree_init(&result);

	get_root_gen_and_level(filter.objectid, fs_info,
			       &filter.match_gen, &filter.match_level);
	ret = btrfs_find_root_search(fs_info, &filter, &result, &found);
	if (ret < 0) {
		fprintf(stderr, "Fail to search the tree root: %s\n",
			strerror(-ret));
		goto out;
	}
	if (ret > 0) {
		printf("Found tree root at %llu gen %llu level %u\n",
		       found->start, filter.match_gen, filter.match_level);
		ret = 0;
	}
	print_find_root_result(&result, &filter);
out:
	btrfs_find_root_free(&result);
	close_ctree_fs_info(fs_info);
	btrfs_close_all_devices();
	return ret;
}
