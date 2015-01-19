/*
 * Copyright (C) 2015 Fujitsu.  All rights reserved.
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
#include "kerncompat.h"
#include "ctree.h"
#include "utils.h"
#include "find-root.h"
#include "volumes.h"
#include "disk-io.h"
#include "extent-cache.h"

/* Return value is the same as btrfs_find_root_search(). */
static int add_eb_to_result(struct extent_buffer *eb,
			    struct cache_tree *result,
			    u32 leafsize,
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

	/* Get the generation cache or create one */
	cache = search_cache_extent(result, generation);
	if (!cache) {
		gen_cache = malloc(sizeof(*gen_cache));
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
				       start, leafsize);
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
int btrfs_find_root_search(struct btrfs_root *chunk_root,
			   struct btrfs_find_root_filter *filter,
			   struct cache_tree *result,
			   struct cache_extent **match)
{
	struct btrfs_fs_info *fs_info = chunk_root->fs_info;
	struct extent_buffer *eb;
	u64 metadata_offset = 0;
	u64 metadata_size = 0;
	u64 offset = 0;
	u32 leafsize = chunk_root->leafsize;
	int suppress_error = 0;
	int ret = 0;

	suppress_error = fs_info->suppress_error;
	fs_info->suppress_error = 1;
	while (1) {
		ret = btrfs_next_metadata(&fs_info->mapping_tree,
					  &metadata_offset, &metadata_size);
		if (ret) {
			if (ret == -ENOENT)
				ret = 0;
			break;
		}
		for (offset = metadata_offset;
		     offset < metadata_offset + metadata_size;
		     offset += chunk_root->leafsize) {
			eb = read_tree_block(chunk_root, offset, leafsize, 0);
			if (!eb || IS_ERR(eb))
				continue;
			ret = add_eb_to_result(eb, result, leafsize, filter,
					       match);
			free_extent_buffer(eb);
			if (ret)
				goto out;
		}
	}
out:
	fs_info->suppress_error = suppress_error;
	return ret;
}
