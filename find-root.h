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

#ifndef __BTRFS_FIND_ROOT_H__
#define __BTRFS_FIND_ROOT_H__

#include "kerncompat.h"

#include "ctree.h"
#include "kernel-lib/list.h"
#include "extent-cache.h"

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
static inline void btrfs_find_root_free(struct cache_tree *result)
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
#endif
