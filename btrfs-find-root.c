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
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/transaction.h"
#include "kernel-lib/list.h"
#include "kernel-shared/volumes.h"
#include "common/utils.h"
#include "crypto/crc32c.h"
#include "common/extent-cache.h"
#include "common/help.h"
#include "cmds/commands.h"

/*
 * Find-root will restore the search result in a 2-level trees.
 * Search result is a cache_tree consisted of generation_cache.
 * Each generation cache records the highest level of this generation
 * and all the tree blocks with this generation.
 *
 * <result>
 * cache_tree ----> generation_cache: gen:1 level: 2  eb_tree ----> eb1
 *		|						|-> eb2
 *		|						......
 *		|-> generation_cache: gen:2 level: 3  eb_tree ---> eb3
 *
 * In the above example, generation 1's highest level is 2, but have multiple
 * eb with same generation, so the root of generation 1 must be missing,
 * possibly has already been overwritten.
 * On the other hand, generation 2's highest level is 3 and we find only one
 * eb for it, so it may be the root of generation 2.
 */

struct btrfs_find_root_gen_cache {
	struct cache_extent cache;	/* cache->start is generation */
	u64 highest_level;
	struct cache_tree eb_tree;
};

struct btrfs_find_root_filter {
	u64 objectid;	/* Only search tree with this objectid */
	u64 generation; /* Only record tree block with higher or
			   equal generation */
	u8 level;	/* Only record tree block with higher or
			   equal level */
	u8 match_level;
	u64 match_gen;
	int search_all;
	/*
	 * If set search_all, even the tree block matches match_gen
	 * and match_level and objectid, still continue searching
	 * This *WILL* take *TONS* of extra time.
	 */
};
int btrfs_find_root_search(struct btrfs_fs_info *fs_info,
			   struct btrfs_find_root_filter *filter,
			   struct cache_tree *result,
			   struct cache_extent **match);

static void btrfs_find_root_free(struct cache_tree *result)
{
	struct btrfs_find_root_gen_cache *gen_cache;
	struct cache_extent *cache;

	cache = first_cache_extent(result);
	while (cache) {
		gen_cache = container_of(cache,
				struct btrfs_find_root_gen_cache, cache);
		free_extent_cache_tree(&gen_cache->eb_tree);
		remove_cache_extent(result, cache);
		free(gen_cache);
		cache = first_cache_extent(result);
	}
}

/* Return value is the same as btrfs_find_root_search(). */
static int add_eb_to_result(struct extent_buffer *eb,
			    struct cache_tree *result,
			    u32 nodesize,
			    struct btrfs_find_root_filter *filter,
			    struct cache_extent **match)
{
	u64 generation = btrfs_header_generation(eb);
	u64 level = btrfs_header_level(eb);
	u64 owner = btrfs_header_owner(eb);
	u64 start = eb->start;
	struct cache_extent *cache;
	struct btrfs_find_root_gen_cache *gen_cache = NULL;
	int ret = 0;

	if (owner != filter->objectid || level < filter->level ||
	    generation < filter->generation)
		return ret;

	/*
	 * Get the generation cache or create one
	 *
	 * NOTE: search_cache_extent() may return cache that doesn't cover
	 * the range. So we need an extra check to make sure it's the right one.
	 */
	cache = search_cache_extent(result, generation);
	if (!cache || cache->start != generation) {
		gen_cache = malloc(sizeof(*gen_cache));
		BUG_ON(!gen_cache);
		cache = &gen_cache->cache;
		cache->start = generation;
		cache->size = 1;
		cache->objectid = 0;
		gen_cache->highest_level = 0;
		cache_tree_init(&gen_cache->eb_tree);

		ret = insert_cache_extent(result, cache);
		if (ret < 0)
			return ret;
	}
	gen_cache = container_of(cache, struct btrfs_find_root_gen_cache,
				 cache);

	/* Higher level, clean tree and insert the new one */
	if (level > gen_cache->highest_level) {
		free_extent_cache_tree(&gen_cache->eb_tree);
		gen_cache->highest_level = level;
		/* Fall into the insert routine */
	}

	/* Same level, insert it into the eb_tree */
	if (level == gen_cache->highest_level) {
		ret = add_cache_extent(&gen_cache->eb_tree,
				       start, nodesize);
		if (ret < 0 && ret != -EEXIST)
			return ret;
		ret = 0;
	}
	if (generation == filter->match_gen &&
	    level == filter->match_level &&
	    !filter->search_all) {
		ret = 1;
		if (match)
			*match = search_cache_extent(&gen_cache->eb_tree,
						     start);
	}
	return ret;
}

/*
 * Return 0 if iterating all the metadata extents.
 * Return 1 if found root with given gen/level and set *match to it.
 * Return <0 if error happens
 */
int btrfs_find_root_search(struct btrfs_fs_info *fs_info,
			   struct btrfs_find_root_filter *filter,
			   struct cache_tree *result,
			   struct cache_extent **match)
{
	struct extent_buffer *eb;
	u64 chunk_offset = 0;
	u64 chunk_size = 0;
	u64 offset = 0;
	u32 nodesize = btrfs_super_nodesize(fs_info->super_copy);
	int suppress_errors = 0;
	int ret = 0;

	suppress_errors = fs_info->suppress_check_block_errors;
	fs_info->suppress_check_block_errors = 1;
	while (1) {
		if (filter->objectid != BTRFS_CHUNK_TREE_OBJECTID)
			ret = btrfs_next_bg_metadata(fs_info,
						  &chunk_offset,
						  &chunk_size);
		else
			ret = btrfs_next_bg_system(fs_info,
						&chunk_offset,
						&chunk_size);
		if (ret) {
			if (ret == -ENOENT)
				ret = 0;
			break;
		}
		for (offset = chunk_offset;
		     offset < chunk_offset + chunk_size;
		     offset += nodesize) {
			eb = read_tree_block(fs_info, offset, 0);
			if (!eb || IS_ERR(eb))
				continue;
			ret = add_eb_to_result(eb, result, nodesize, filter,
					       match);
			free_extent_buffer(eb);
			if (ret)
				goto out;
		}
	}
out:
	fs_info->suppress_check_block_errors = suppress_errors;
	return ret;
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

static const char * btrfs_find_root_usage[] = {
	"btrfs-find-usage [options] <device>",
	"Attempt to find tree roots on the device",
	"",
	"  -a              search through all metadata even if the root has been found",
	"  -o OBJECTID     filter by the tree's object id",
	"  -l LEVEL        filter by tree level, (default: 0)",
	"  -g GENERATION   filter by tree generation",
};

static const struct cmd_struct btrfs_find_root_cmd = {
	"btrfs-find-root", NULL, btrfs_find_root_usage, NULL, 0,
};

int main(int argc, char **argv)
{
	struct btrfs_fs_info *fs_info;
	struct btrfs_find_root_filter filter = {0};
	struct cache_tree result;
	struct cache_extent *found;
	struct open_ctree_flags ocf = { 0 };
	int ret;

	/* Default to search root tree */
	filter.objectid = BTRFS_ROOT_TREE_OBJECTID;
	filter.match_gen = (u64)-1;
	filter.match_level = (u8)-1;
	opterr = 0;
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
			usage_command(&btrfs_find_root_cmd, 0, 0);
			return 0;
		default:
			usage_unknown_option(&btrfs_find_root_cmd, argv);
		}
	}

	set_argv0(argv);
	if (check_argc_min(argc - optind, 1))
		return 1;

	ocf.filename = argv[optind];
	ocf.flags = OPEN_CTREE_CHUNK_ROOT_ONLY | OPEN_CTREE_IGNORE_CHUNK_TREE_ERROR;
	fs_info = open_ctree_fs_info(&ocf);
	if (!fs_info) {
		error("open ctree failed");
		return 1;
	}
	cache_tree_init(&result);

	get_root_gen_and_level(filter.objectid, fs_info,
			       &filter.match_gen, &filter.match_level);
	ret = btrfs_find_root_search(fs_info, &filter, &result, &found);
	if (ret < 0) {
		errno = -ret;
		fprintf(stderr, "Fail to search the tree root: %m\n");
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
