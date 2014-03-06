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
#include <stdint.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "crc32c.h"
#include "volumes.h"
#include "free-space-cache.h"
#include "math.h"

#define PENDING_EXTENT_INSERT 0
#define PENDING_EXTENT_DELETE 1
#define PENDING_BACKREF_UPDATE 2

struct pending_extent_op {
	int type;
	u64 bytenr;
	u64 num_bytes;
	u64 flags;
	struct btrfs_disk_key key;
	int level;
};

static int alloc_reserved_tree_block(struct btrfs_trans_handle *trans,
				     struct btrfs_root *root,
				     u64 root_objectid, u64 generation,
				     u64 flags, struct btrfs_disk_key *key,
				     int level, struct btrfs_key *ins);
static int __free_extent(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 u64 bytenr, u64 num_bytes, u64 parent,
			 u64 root_objectid, u64 owner_objectid,
			 u64 owner_offset, int refs_to_drop);
static int finish_current_insert(struct btrfs_trans_handle *trans, struct
				 btrfs_root *extent_root);
static int del_pending_extents(struct btrfs_trans_handle *trans, struct
			       btrfs_root *extent_root);
static struct btrfs_block_group_cache *
btrfs_find_block_group(struct btrfs_root *root, struct btrfs_block_group_cache
		       *hint, u64 search_start, int data, int owner);

static int remove_sb_from_cache(struct btrfs_root *root,
				struct btrfs_block_group_cache *cache)
{
	u64 bytenr;
	u64 *logical;
	int stripe_len;
	int i, nr, ret;
	struct extent_io_tree *free_space_cache;

	free_space_cache = &root->fs_info->free_space_cache;
	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = btrfs_rmap_block(&root->fs_info->mapping_tree,
				       cache->key.objectid, bytenr, 0,
				       &logical, &nr, &stripe_len);
		BUG_ON(ret);
		while (nr--) {
			clear_extent_dirty(free_space_cache, logical[nr],
				logical[nr] + stripe_len - 1, GFP_NOFS);
		}
		kfree(logical);
	}
	return 0;
}

static int cache_block_group(struct btrfs_root *root,
			     struct btrfs_block_group_cache *block_group)
{
	struct btrfs_path *path;
	int ret;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct extent_io_tree *free_space_cache;
	int slot;
	u64 last;
	u64 hole_size;

	if (!block_group)
		return 0;

	root = root->fs_info->extent_root;
	free_space_cache = &root->fs_info->free_space_cache;

	if (block_group->cached)
		return 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->reada = 2;
	last = max_t(u64, block_group->key.objectid, BTRFS_SUPER_INFO_OFFSET);
	key.objectid = last;
	key.offset = 0;
	key.type = 0;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto err;

	while(1) {
		leaf = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0)
				goto err;
			if (ret == 0) {
				continue;
			} else {
				break;
			}
		}
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid < block_group->key.objectid) {
			goto next;
		}
		if (key.objectid >= block_group->key.objectid +
		    block_group->key.offset) {
			break;
		}

		if (key.type == BTRFS_EXTENT_ITEM_KEY ||
		    key.type == BTRFS_METADATA_ITEM_KEY) {
			if (key.objectid > last) {
				hole_size = key.objectid - last;
				set_extent_dirty(free_space_cache, last,
						 last + hole_size - 1,
						 GFP_NOFS);
			}
			if (key.type == BTRFS_METADATA_ITEM_KEY)
				last = key.objectid + root->leafsize;
			else
				last = key.objectid + key.offset;
		}
next:
		path->slots[0]++;
	}

	if (block_group->key.objectid +
	    block_group->key.offset > last) {
		hole_size = block_group->key.objectid +
			block_group->key.offset - last;
		set_extent_dirty(free_space_cache, last,
				 last + hole_size - 1, GFP_NOFS);
	}
	remove_sb_from_cache(root, block_group);
	block_group->cached = 1;
err:
	btrfs_free_path(path);
	return 0;
}

struct btrfs_block_group_cache *btrfs_lookup_first_block_group(struct
						       btrfs_fs_info *info,
						       u64 bytenr)
{
	struct extent_io_tree *block_group_cache;
	struct btrfs_block_group_cache *block_group = NULL;
	u64 ptr;
	u64 start;
	u64 end;
	int ret;

	bytenr = max_t(u64, bytenr,
		       BTRFS_SUPER_INFO_OFFSET + BTRFS_SUPER_INFO_SIZE);
	block_group_cache = &info->block_group_cache;
	ret = find_first_extent_bit(block_group_cache,
				    bytenr, &start, &end,
				    BLOCK_GROUP_DATA | BLOCK_GROUP_METADATA |
				    BLOCK_GROUP_SYSTEM);
	if (ret) {
		return NULL;
	}
	ret = get_state_private(block_group_cache, start, &ptr);
	if (ret)
		return NULL;

	block_group = (struct btrfs_block_group_cache *)(unsigned long)ptr;
	return block_group;
}

struct btrfs_block_group_cache *btrfs_lookup_block_group(struct
							 btrfs_fs_info *info,
							 u64 bytenr)
{
	struct extent_io_tree *block_group_cache;
	struct btrfs_block_group_cache *block_group = NULL;
	u64 ptr;
	u64 start;
	u64 end;
	int ret;

	block_group_cache = &info->block_group_cache;
	ret = find_first_extent_bit(block_group_cache,
				    bytenr, &start, &end,
				    BLOCK_GROUP_DATA | BLOCK_GROUP_METADATA |
				    BLOCK_GROUP_SYSTEM);
	if (ret) {
		return NULL;
	}
	ret = get_state_private(block_group_cache, start, &ptr);
	if (ret)
		return NULL;

	block_group = (struct btrfs_block_group_cache *)(unsigned long)ptr;
	if (block_group->key.objectid <= bytenr && bytenr <
	    block_group->key.objectid + block_group->key.offset)
		return block_group;
	return NULL;
}

static int block_group_bits(struct btrfs_block_group_cache *cache, u64 bits)
{
	return (cache->flags & bits) == bits;
}

static int noinline find_search_start(struct btrfs_root *root,
			      struct btrfs_block_group_cache **cache_ret,
			      u64 *start_ret, int num, int data)
{
	int ret;
	struct btrfs_block_group_cache *cache = *cache_ret;
	u64 last = *start_ret;
	u64 start = 0;
	u64 end = 0;
	u64 search_start = *start_ret;
	int wrapped = 0;

	if (!cache)
		goto out;
again:
	ret = cache_block_group(root, cache);
	if (ret)
		goto out;

	last = max(search_start, cache->key.objectid);
	if (cache->ro || !block_group_bits(cache, data))
		goto new_group;

	while(1) {
		ret = find_first_extent_bit(&root->fs_info->free_space_cache,
					    last, &start, &end, EXTENT_DIRTY);
		if (ret) {
			goto new_group;
		}

		start = max(last, start);
		last = end + 1;
		if (last - start < num) {
			continue;
		}
		if (start + num > cache->key.objectid + cache->key.offset) {
			goto new_group;
		}
		*start_ret = start;
		return 0;
	}
out:
	*start_ret = last;
	cache = btrfs_lookup_block_group(root->fs_info, search_start);
	if (!cache) {
		printk("Unable to find block group for %llu\n",
			(unsigned long long)search_start);
		WARN_ON(1);
	}
	return -ENOSPC;

new_group:
	last = cache->key.objectid + cache->key.offset;
wrapped:
	cache = btrfs_lookup_first_block_group(root->fs_info, last);
	if (!cache) {
		if (!wrapped) {
			wrapped = 1;
			last = search_start;
			goto wrapped;
		}
		goto out;
	}
	*cache_ret = cache;
	goto again;
}

static int block_group_state_bits(u64 flags)
{
	int bits = 0;
	if (flags & BTRFS_BLOCK_GROUP_DATA)
		bits |= BLOCK_GROUP_DATA;
	if (flags & BTRFS_BLOCK_GROUP_METADATA)
		bits |= BLOCK_GROUP_METADATA;
	if (flags & BTRFS_BLOCK_GROUP_SYSTEM)
		bits |= BLOCK_GROUP_SYSTEM;
	return bits;
}

static struct btrfs_block_group_cache *
btrfs_find_block_group(struct btrfs_root *root, struct btrfs_block_group_cache
		       *hint, u64 search_start, int data, int owner)
{
	struct btrfs_block_group_cache *cache;
	struct extent_io_tree *block_group_cache;
	struct btrfs_block_group_cache *found_group = NULL;
	struct btrfs_fs_info *info = root->fs_info;
	u64 used;
	u64 last = 0;
	u64 hint_last;
	u64 start;
	u64 end;
	u64 free_check;
	u64 ptr;
	int bit;
	int ret;
	int full_search = 0;
	int factor = 10;

	block_group_cache = &info->block_group_cache;

	if (!owner)
		factor = 10;

	bit = block_group_state_bits(data);

	if (search_start) {
		struct btrfs_block_group_cache *shint;
		shint = btrfs_lookup_block_group(info, search_start);
		if (shint && !shint->ro && block_group_bits(shint, data)) {
			used = btrfs_block_group_used(&shint->item);
			if (used + shint->pinned <
			    div_factor(shint->key.offset, factor)) {
				return shint;
			}
		}
	}
	if (hint && !hint->ro && block_group_bits(hint, data)) {
		used = btrfs_block_group_used(&hint->item);
		if (used + hint->pinned <
		    div_factor(hint->key.offset, factor)) {
			return hint;
		}
		last = hint->key.objectid + hint->key.offset;
		hint_last = last;
	} else {
		if (hint)
			hint_last = max(hint->key.objectid, search_start);
		else
			hint_last = search_start;

		last = hint_last;
	}
again:
	while(1) {
		ret = find_first_extent_bit(block_group_cache, last,
					    &start, &end, bit);
		if (ret)
			break;

		ret = get_state_private(block_group_cache, start, &ptr);
		if (ret)
			break;

		cache = (struct btrfs_block_group_cache *)(unsigned long)ptr;
		last = cache->key.objectid + cache->key.offset;
		used = btrfs_block_group_used(&cache->item);

		if (!cache->ro && block_group_bits(cache, data)) {
			if (full_search)
				free_check = cache->key.offset;
			else
				free_check = div_factor(cache->key.offset,
							factor);

			if (used + cache->pinned < free_check) {
				found_group = cache;
				goto found;
			}
		}
		cond_resched();
	}
	if (!full_search) {
		last = search_start;
		full_search = 1;
		goto again;
	}
found:
	return found_group;
}

/*
 * Back reference rules.  Back refs have three main goals:
 *
 * 1) differentiate between all holders of references to an extent so that
 *    when a reference is dropped we can make sure it was a valid reference
 *    before freeing the extent.
 *
 * 2) Provide enough information to quickly find the holders of an extent
 *    if we notice a given block is corrupted or bad.
 *
 * 3) Make it easy to migrate blocks for FS shrinking or storage pool
 *    maintenance.  This is actually the same as #2, but with a slightly
 *    different use case.
 *
 * There are two kinds of back refs. The implicit back refs is optimized
 * for pointers in non-shared tree blocks. For a given pointer in a block,
 * back refs of this kind provide information about the block's owner tree
 * and the pointer's key. These information allow us to find the block by
 * b-tree searching. The full back refs is for pointers in tree blocks not
 * referenced by their owner trees. The location of tree block is recorded
 * in the back refs. Actually the full back refs is generic, and can be
 * used in all cases the implicit back refs is used. The major shortcoming
 * of the full back refs is its overhead. Every time a tree block gets
 * COWed, we have to update back refs entry for all pointers in it.
 *
 * For a newly allocated tree block, we use implicit back refs for
 * pointers in it. This means most tree related operations only involve
 * implicit back refs. For a tree block created in old transaction, the
 * only way to drop a reference to it is COW it. So we can detect the
 * event that tree block loses its owner tree's reference and do the
 * back refs conversion.
 *
 * When a tree block is COW'd through a tree, there are four cases:
 *
 * The reference count of the block is one and the tree is the block's
 * owner tree. Nothing to do in this case.
 *
 * The reference count of the block is one and the tree is not the
 * block's owner tree. In this case, full back refs is used for pointers
 * in the block. Remove these full back refs, add implicit back refs for
 * every pointers in the new block.
 *
 * The reference count of the block is greater than one and the tree is
 * the block's owner tree. In this case, implicit back refs is used for
 * pointers in the block. Add full back refs for every pointers in the
 * block, increase lower level extents' reference counts. The original
 * implicit back refs are entailed to the new block.
 *
 * The reference count of the block is greater than one and the tree is
 * not the block's owner tree. Add implicit back refs for every pointer in
 * the new block, increase lower level extents' reference count.
 *
 * Back Reference Key composing:
 *
 * The key objectid corresponds to the first byte in the extent,
 * The key type is used to differentiate between types of back refs.
 * There are different meanings of the key offset for different types
 * of back refs.
 *
 * File extents can be referenced by:
 *
 * - multiple snapshots, subvolumes, or different generations in one subvol
 * - different files inside a single subvolume
 * - different offsets inside a file (bookend extents in file.c)
 *
 * The extent ref structure for the implicit back refs has fields for:
 *
 * - Objectid of the subvolume root
 * - objectid of the file holding the reference
 * - original offset in the file
 * - how many bookend extents
 *
 * The key offset for the implicit back refs is hash of the first
 * three fields.
 *
 * The extent ref structure for the full back refs has field for:
 *
 * - number of pointers in the tree leaf
 *
 * The key offset for the implicit back refs is the first byte of
 * the tree leaf
 *
 * When a file extent is allocated, The implicit back refs is used.
 * the fields are filled in:
 *
 *     (root_key.objectid, inode objectid, offset in file, 1)
 *
 * When a file extent is removed file truncation, we find the
 * corresponding implicit back refs and check the following fields:
 *
 *     (btrfs_header_owner(leaf), inode objectid, offset in file)
 *
 * Btree extents can be referenced by:
 *
 * - Different subvolumes
 *
 * Both the implicit back refs and the full back refs for tree blocks
 * only consist of key. The key offset for the implicit back refs is
 * objectid of block's owner tree. The key offset for the full back refs
 * is the first byte of parent block.
 *
 * When implicit back refs is used, information about the lowest key and
 * level of the tree block are required. These information are stored in
 * tree block info structure.
 */

#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
static int convert_extent_item_v0(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root,
				  struct btrfs_path *path,
				  u64 owner, u32 extra_size)
{
	struct btrfs_extent_item *item;
	struct btrfs_extent_item_v0 *ei0;
	struct btrfs_extent_ref_v0 *ref0;
	struct btrfs_tree_block_info *bi;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_key found_key;
	u32 new_size = sizeof(*item);
	u64 refs;
	int ret;

	leaf = path->nodes[0];
	BUG_ON(btrfs_item_size_nr(leaf, path->slots[0]) != sizeof(*ei0));

	btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
	ei0 = btrfs_item_ptr(leaf, path->slots[0],
			     struct btrfs_extent_item_v0);
	refs = btrfs_extent_refs_v0(leaf, ei0);

	if (owner == (u64)-1) {
		while (1) {
			if (path->slots[0] >= btrfs_header_nritems(leaf)) {
				ret = btrfs_next_leaf(root, path);
				if (ret < 0)
					return ret;
				BUG_ON(ret > 0);
				leaf = path->nodes[0];
			}
			btrfs_item_key_to_cpu(leaf, &found_key,
					      path->slots[0]);
			BUG_ON(key.objectid != found_key.objectid);
			if (found_key.type != BTRFS_EXTENT_REF_V0_KEY) {
				path->slots[0]++;
				continue;
			}
			ref0 = btrfs_item_ptr(leaf, path->slots[0],
					      struct btrfs_extent_ref_v0);
			owner = btrfs_ref_objectid_v0(leaf, ref0);
			break;
		}
	}
	btrfs_release_path(path);

	if (owner < BTRFS_FIRST_FREE_OBJECTID)
		new_size += sizeof(*bi);

	new_size -= sizeof(*ei0);
	ret = btrfs_search_slot(trans, root, &key, path, new_size, 1);
	if (ret < 0)
		return ret;
	BUG_ON(ret);

	ret = btrfs_extend_item(trans, root, path, new_size);
	BUG_ON(ret);

	leaf = path->nodes[0];
	item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	btrfs_set_extent_refs(leaf, item, refs);
	/* FIXME: get real generation */
	btrfs_set_extent_generation(leaf, item, 0);
	if (owner < BTRFS_FIRST_FREE_OBJECTID) {
		btrfs_set_extent_flags(leaf, item,
				       BTRFS_EXTENT_FLAG_TREE_BLOCK |
				       BTRFS_BLOCK_FLAG_FULL_BACKREF);
		bi = (struct btrfs_tree_block_info *)(item + 1);
		/* FIXME: get first key of the block */
		memset_extent_buffer(leaf, 0, (unsigned long)bi, sizeof(*bi));
		btrfs_set_tree_block_level(leaf, bi, (int)owner);
	} else {
		btrfs_set_extent_flags(leaf, item, BTRFS_EXTENT_FLAG_DATA);
	}
	btrfs_mark_buffer_dirty(leaf);
	return 0;
}
#endif

static u64 hash_extent_data_ref(u64 root_objectid, u64 owner, u64 offset)
{
	u32 high_crc = ~(u32)0;
	u32 low_crc = ~(u32)0;
	__le64 lenum;

	lenum = cpu_to_le64(root_objectid);
	high_crc = btrfs_crc32c(high_crc, &lenum, sizeof(lenum));
	lenum = cpu_to_le64(owner);
	low_crc = btrfs_crc32c(low_crc, &lenum, sizeof(lenum));
	lenum = cpu_to_le64(offset);
	low_crc = btrfs_crc32c(low_crc, &lenum, sizeof(lenum));

	return ((u64)high_crc << 31) ^ (u64)low_crc;
}

static u64 hash_extent_data_ref_item(struct extent_buffer *leaf,
				     struct btrfs_extent_data_ref *ref)
{
	return hash_extent_data_ref(btrfs_extent_data_ref_root(leaf, ref),
				    btrfs_extent_data_ref_objectid(leaf, ref),
				    btrfs_extent_data_ref_offset(leaf, ref));
}

static int match_extent_data_ref(struct extent_buffer *leaf,
				 struct btrfs_extent_data_ref *ref,
				 u64 root_objectid, u64 owner, u64 offset)
{
	if (btrfs_extent_data_ref_root(leaf, ref) != root_objectid ||
	    btrfs_extent_data_ref_objectid(leaf, ref) != owner ||
	    btrfs_extent_data_ref_offset(leaf, ref) != offset)
		return 0;
	return 1;
}

static noinline int lookup_extent_data_ref(struct btrfs_trans_handle *trans,
					   struct btrfs_root *root,
					   struct btrfs_path *path,
					   u64 bytenr, u64 parent,
					   u64 root_objectid,
					   u64 owner, u64 offset)
{
	struct btrfs_key key;
	struct btrfs_extent_data_ref *ref;
	struct extent_buffer *leaf;
	u32 nritems;
	int ret;
	int recow;
	int err = -ENOENT;

	key.objectid = bytenr;
	if (parent) {
		key.type = BTRFS_SHARED_DATA_REF_KEY;
		key.offset = parent;
	} else {
		key.type = BTRFS_EXTENT_DATA_REF_KEY;
		key.offset = hash_extent_data_ref(root_objectid,
						  owner, offset);
	}
again:
	recow = 0;
	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret < 0) {
		err = ret;
		goto fail;
	}

	if (parent) {
		if (!ret)
			return 0;
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
		key.type = BTRFS_EXTENT_REF_V0_KEY;
		btrfs_release_path(path);
		ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret < 0) {
			err = ret;
			goto fail;
		}
		if (!ret)
			return 0;
#endif
		goto fail;
	}

	leaf = path->nodes[0];
	nritems = btrfs_header_nritems(leaf);
	while (1) {
		if (path->slots[0] >= nritems) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0)
				err = ret;
			if (ret)
				goto fail;

			leaf = path->nodes[0];
			nritems = btrfs_header_nritems(leaf);
			recow = 1;
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != bytenr ||
		    key.type != BTRFS_EXTENT_DATA_REF_KEY)
			goto fail;
		
		ref = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_data_ref);

		if (match_extent_data_ref(leaf, ref, root_objectid,
					  owner, offset)) {
			if (recow) {
				btrfs_release_path(path);
				goto again;
			}
			err = 0;
			break;
		}
		path->slots[0]++;
	}
fail:
	return err;
}

static noinline int insert_extent_data_ref(struct btrfs_trans_handle *trans,
					   struct btrfs_root *root,
					   struct btrfs_path *path,
					   u64 bytenr, u64 parent,
					   u64 root_objectid, u64 owner,
					   u64 offset, int refs_to_add)
{
	struct btrfs_key key;
	struct extent_buffer *leaf;
	u32 size;
	u32 num_refs;
	int ret;

	key.objectid = bytenr;
	if (parent) {
		key.type = BTRFS_SHARED_DATA_REF_KEY;
		key.offset = parent;
		size = sizeof(struct btrfs_shared_data_ref);
	} else {
		key.type = BTRFS_EXTENT_DATA_REF_KEY;
		key.offset = hash_extent_data_ref(root_objectid,
						  owner, offset);
		size = sizeof(struct btrfs_extent_data_ref);
	}

	ret = btrfs_insert_empty_item(trans, root, path, &key, size);
	if (ret && ret != -EEXIST)
		goto fail;

	leaf = path->nodes[0];
	if (parent) {
		struct btrfs_shared_data_ref *ref;
		ref = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_shared_data_ref);
		if (ret == 0) {
			btrfs_set_shared_data_ref_count(leaf, ref, refs_to_add);
		} else {
			num_refs = btrfs_shared_data_ref_count(leaf, ref);
			num_refs += refs_to_add;
			btrfs_set_shared_data_ref_count(leaf, ref, num_refs);
		}
	} else {
		struct btrfs_extent_data_ref *ref;
		while (ret == -EEXIST) {
			ref = btrfs_item_ptr(leaf, path->slots[0],
					     struct btrfs_extent_data_ref);
			if (match_extent_data_ref(leaf, ref, root_objectid,
						  owner, offset))
				break;
			btrfs_release_path(path);

			key.offset++;
			ret = btrfs_insert_empty_item(trans, root, path, &key,
						      size);
			if (ret && ret != -EEXIST)
				goto fail;

			leaf = path->nodes[0];
		}
		ref = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_data_ref);
		if (ret == 0) {
			btrfs_set_extent_data_ref_root(leaf, ref,
						       root_objectid);
			btrfs_set_extent_data_ref_objectid(leaf, ref, owner);
			btrfs_set_extent_data_ref_offset(leaf, ref, offset);
			btrfs_set_extent_data_ref_count(leaf, ref, refs_to_add);
		} else {
			num_refs = btrfs_extent_data_ref_count(leaf, ref);
			num_refs += refs_to_add;
			btrfs_set_extent_data_ref_count(leaf, ref, num_refs);
		}
	}
	btrfs_mark_buffer_dirty(leaf);
	ret = 0;
fail:
	btrfs_release_path(path);
	return ret;
}

static noinline int remove_extent_data_ref(struct btrfs_trans_handle *trans,
					   struct btrfs_root *root,
					   struct btrfs_path *path,
					   int refs_to_drop)
{
	struct btrfs_key key;
	struct btrfs_extent_data_ref *ref1 = NULL;
	struct btrfs_shared_data_ref *ref2 = NULL;
	struct extent_buffer *leaf;
	u32 num_refs = 0;
	int ret = 0;

	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);

	if (key.type == BTRFS_EXTENT_DATA_REF_KEY) {
		ref1 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_extent_data_ref);
		num_refs = btrfs_extent_data_ref_count(leaf, ref1);
	} else if (key.type == BTRFS_SHARED_DATA_REF_KEY) {
		ref2 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_shared_data_ref);
		num_refs = btrfs_shared_data_ref_count(leaf, ref2);
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
	} else if (key.type == BTRFS_EXTENT_REF_V0_KEY) {
		struct btrfs_extent_ref_v0 *ref0;
		ref0 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_extent_ref_v0);
		num_refs = btrfs_ref_count_v0(leaf, ref0);
#endif
	} else {
		BUG();
	}

	BUG_ON(num_refs < refs_to_drop);
	num_refs -= refs_to_drop;

	if (num_refs == 0) {
		ret = btrfs_del_item(trans, root, path);
	} else {
		if (key.type == BTRFS_EXTENT_DATA_REF_KEY)
			btrfs_set_extent_data_ref_count(leaf, ref1, num_refs);
		else if (key.type == BTRFS_SHARED_DATA_REF_KEY)
			btrfs_set_shared_data_ref_count(leaf, ref2, num_refs);
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
		else {
			struct btrfs_extent_ref_v0 *ref0;
			ref0 = btrfs_item_ptr(leaf, path->slots[0],
					struct btrfs_extent_ref_v0);
			btrfs_set_ref_count_v0(leaf, ref0, num_refs);
		}
#endif
		btrfs_mark_buffer_dirty(leaf);
	}
	return ret;
}

static noinline u32 extent_data_ref_count(struct btrfs_root *root,
					  struct btrfs_path *path,
					  struct btrfs_extent_inline_ref *iref)
{
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_extent_data_ref *ref1;
	struct btrfs_shared_data_ref *ref2;
	u32 num_refs = 0;

	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
	if (iref) {
		if (btrfs_extent_inline_ref_type(leaf, iref) ==
		    BTRFS_EXTENT_DATA_REF_KEY) {
			ref1 = (struct btrfs_extent_data_ref *)(&iref->offset);
			num_refs = btrfs_extent_data_ref_count(leaf, ref1);
		} else {
			ref2 = (struct btrfs_shared_data_ref *)(iref + 1);
			num_refs = btrfs_shared_data_ref_count(leaf, ref2);
		}
	} else if (key.type == BTRFS_EXTENT_DATA_REF_KEY) {
		ref1 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_extent_data_ref);
		num_refs = btrfs_extent_data_ref_count(leaf, ref1);
	} else if (key.type == BTRFS_SHARED_DATA_REF_KEY) {
		ref2 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_shared_data_ref);
		num_refs = btrfs_shared_data_ref_count(leaf, ref2);
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
	} else if (key.type == BTRFS_EXTENT_REF_V0_KEY) {
		struct btrfs_extent_ref_v0 *ref0;
		ref0 = btrfs_item_ptr(leaf, path->slots[0],
				      struct btrfs_extent_ref_v0);
		num_refs = btrfs_ref_count_v0(leaf, ref0);
#endif
	} else {
		BUG();
	}
	return num_refs;
}

static noinline int lookup_tree_block_ref(struct btrfs_trans_handle *trans,
					  struct btrfs_root *root,
					  struct btrfs_path *path,
					  u64 bytenr, u64 parent,
					  u64 root_objectid)
{
	struct btrfs_key key;
	int ret;

	key.objectid = bytenr;
	if (parent) {
		key.type = BTRFS_SHARED_BLOCK_REF_KEY;
		key.offset = parent;
	} else {
		key.type = BTRFS_TREE_BLOCK_REF_KEY;
		key.offset = root_objectid;
	}

	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret > 0)
		ret = -ENOENT;
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
	if (ret == -ENOENT && parent) {
		btrfs_release_path(path);
		key.type = BTRFS_EXTENT_REF_V0_KEY;
		ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret > 0)
			ret = -ENOENT;
	}
#endif
	return ret;
}

static noinline int insert_tree_block_ref(struct btrfs_trans_handle *trans,
					  struct btrfs_root *root,
					  struct btrfs_path *path,
					  u64 bytenr, u64 parent,
					  u64 root_objectid)
{
	struct btrfs_key key;
	int ret;

	key.objectid = bytenr;
	if (parent) {
		key.type = BTRFS_SHARED_BLOCK_REF_KEY;
		key.offset = parent;
	} else {
		key.type = BTRFS_TREE_BLOCK_REF_KEY;
		key.offset = root_objectid;
	}

	ret = btrfs_insert_empty_item(trans, root, path, &key, 0);

	btrfs_release_path(path);
	return ret;
}

static inline int extent_ref_type(u64 parent, u64 owner)
{
	int type;
	if (owner < BTRFS_FIRST_FREE_OBJECTID) {
		if (parent > 0)
			type = BTRFS_SHARED_BLOCK_REF_KEY;
		else
			type = BTRFS_TREE_BLOCK_REF_KEY;
	} else {
		if (parent > 0)
			type = BTRFS_SHARED_DATA_REF_KEY;
		else
			type = BTRFS_EXTENT_DATA_REF_KEY;
	}
	return type;
}

static int find_next_key(struct btrfs_path *path, struct btrfs_key *key)

{
	int level;
	for (level = 0; level < BTRFS_MAX_LEVEL; level++) {
		if (!path->nodes[level])
			break;
		if (path->slots[level] + 1 >=
		    btrfs_header_nritems(path->nodes[level]))
			continue;
		if (level == 0)
			btrfs_item_key_to_cpu(path->nodes[level], key,
					      path->slots[level] + 1);
		else
			btrfs_node_key_to_cpu(path->nodes[level], key,
					      path->slots[level] + 1);
		return 0;
	}
	return 1;
}

static int lookup_inline_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct btrfs_extent_inline_ref **ref_ret,
				 u64 bytenr, u64 num_bytes,
				 u64 parent, u64 root_objectid,
				 u64 owner, u64 offset, int insert)
{
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	u64 flags;
	u32 item_size;
	unsigned long ptr;
	unsigned long end;
	int extra_size;
	int type;
	int want;
	int ret;
	int err = 0;
	int skinny_metadata =
		btrfs_fs_incompat(root->fs_info,
				  BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA);

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = num_bytes;

	want = extent_ref_type(parent, owner);
	if (insert)
		extra_size = btrfs_extent_inline_ref_size(want);
	else
		extra_size = -1;

	if (owner < BTRFS_FIRST_FREE_OBJECTID && skinny_metadata) {
		skinny_metadata = 1;
		key.type = BTRFS_METADATA_ITEM_KEY;
		key.offset = owner;
	} else if (skinny_metadata) {
		skinny_metadata = 0;
	}

again:
	ret = btrfs_search_slot(trans, root, &key, path, extra_size, 1);
	if (ret < 0) {
		err = ret;
		goto out;
	}

	/*
	 * We may be a newly converted file system which still has the old fat
	 * extent entries for metadata, so try and see if we have one of those.
	 */
	if (ret > 0 && skinny_metadata) {
		skinny_metadata = 0;
		if (path->slots[0]) {
			path->slots[0]--;
			btrfs_item_key_to_cpu(path->nodes[0], &key,
					      path->slots[0]);
			if (key.objectid == bytenr &&
			    key.type == BTRFS_EXTENT_ITEM_KEY &&
			    key.offset == num_bytes)
				ret = 0;
		}
		if (ret) {
			key.type = BTRFS_EXTENT_ITEM_KEY;
			key.offset = num_bytes;
			btrfs_release_path(path);
			goto again;
		}
	}

	if (ret) {
		printf("Failed to find [%llu, %u, %llu]\n", key.objectid, key.type, key.offset);
		return -ENOENT;
	}

	BUG_ON(ret);

	leaf = path->nodes[0];
	item_size = btrfs_item_size_nr(leaf, path->slots[0]);
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
	if (item_size < sizeof(*ei)) {
		if (!insert) {
			err = -ENOENT;
			goto out;
		}
		ret = convert_extent_item_v0(trans, root, path, owner,
					     extra_size);
		if (ret < 0) {
			err = ret;
			goto out;
		}
		leaf = path->nodes[0];
		item_size = btrfs_item_size_nr(leaf, path->slots[0]);
	}
#endif
	if (item_size < sizeof(*ei)) {
		printf("Size is %u, needs to be %u, slot %d\n",
		       (unsigned)item_size,
		       (unsigned)sizeof(*ei), path->slots[0]);
		btrfs_print_leaf(root, leaf);
		return -EINVAL;
	}
	BUG_ON(item_size < sizeof(*ei));

	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	flags = btrfs_extent_flags(leaf, ei);

	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + item_size;

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK && !skinny_metadata) {
		ptr += sizeof(struct btrfs_tree_block_info);
		BUG_ON(ptr > end);
	} else if (!(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)) {
		if (!(flags & BTRFS_EXTENT_FLAG_DATA)) {
			return -EIO;
		}
	}

	err = -ENOENT;
	while (1) {
		if (ptr >= end) {
			WARN_ON(ptr > end);
			break;
		}
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		if (want < type)
			break;
		if (want > type) {
			ptr += btrfs_extent_inline_ref_size(type);
			continue;
		}

		if (type == BTRFS_EXTENT_DATA_REF_KEY) {
			struct btrfs_extent_data_ref *dref;
			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			if (match_extent_data_ref(leaf, dref, root_objectid,
						  owner, offset)) {
				err = 0;
				break;
			}
			if (hash_extent_data_ref_item(leaf, dref) <
			    hash_extent_data_ref(root_objectid, owner, offset))
				break;
		} else {
			u64 ref_offset;
			ref_offset = btrfs_extent_inline_ref_offset(leaf, iref);
			if (parent > 0) {
				if (parent == ref_offset) {
					err = 0;
					break;
				}
				if (ref_offset < parent)
					break;
			} else {
				if (root_objectid == ref_offset) {
					err = 0;
					break;
				}
				if (ref_offset < root_objectid)
					break;
			}
		}
		ptr += btrfs_extent_inline_ref_size(type);
	}
	if (err == -ENOENT && insert) {
		if (item_size + extra_size >=
		    BTRFS_MAX_EXTENT_ITEM_SIZE(root)) {
			err = -EAGAIN;
			goto out;
		}
		/*
		 * To add new inline back ref, we have to make sure
		 * there is no corresponding back ref item.
		 * For simplicity, we just do not add new inline back
		 * ref if there is any back ref item.
		 */
		if (find_next_key(path, &key) == 0 && key.objectid == bytenr &&
		    key.type < BTRFS_BLOCK_GROUP_ITEM_KEY) {
			err = -EAGAIN;
			goto out;
		}
	}
	*ref_ret = (struct btrfs_extent_inline_ref *)ptr;
out:
	return err;
}

static int setup_inline_extent_backref(struct btrfs_trans_handle *trans,
				struct btrfs_root *root,
				struct btrfs_path *path,
				struct btrfs_extent_inline_ref *iref,
				u64 parent, u64 root_objectid,
				u64 owner, u64 offset, int refs_to_add)
{
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	unsigned long ptr;
	unsigned long end;
	unsigned long item_offset;
	u64 refs;
	int size;
	int type;
	int ret;

	leaf = path->nodes[0];
	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	item_offset = (unsigned long)iref - (unsigned long)ei;

	type = extent_ref_type(parent, owner);
	size = btrfs_extent_inline_ref_size(type);

	ret = btrfs_extend_item(trans, root, path, size);
	BUG_ON(ret);

	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	refs = btrfs_extent_refs(leaf, ei);
	refs += refs_to_add;
	btrfs_set_extent_refs(leaf, ei, refs);

	ptr = (unsigned long)ei + item_offset;
	end = (unsigned long)ei + btrfs_item_size_nr(leaf, path->slots[0]);
	if (ptr < end - size)
		memmove_extent_buffer(leaf, ptr + size, ptr,
				      end - size - ptr);

	iref = (struct btrfs_extent_inline_ref *)ptr;
	btrfs_set_extent_inline_ref_type(leaf, iref, type);
	if (type == BTRFS_EXTENT_DATA_REF_KEY) {
		struct btrfs_extent_data_ref *dref;
		dref = (struct btrfs_extent_data_ref *)(&iref->offset);
		btrfs_set_extent_data_ref_root(leaf, dref, root_objectid);
		btrfs_set_extent_data_ref_objectid(leaf, dref, owner);
		btrfs_set_extent_data_ref_offset(leaf, dref, offset);
		btrfs_set_extent_data_ref_count(leaf, dref, refs_to_add);
	} else if (type == BTRFS_SHARED_DATA_REF_KEY) {
		struct btrfs_shared_data_ref *sref;
		sref = (struct btrfs_shared_data_ref *)(iref + 1);
		btrfs_set_shared_data_ref_count(leaf, sref, refs_to_add);
		btrfs_set_extent_inline_ref_offset(leaf, iref, parent);
	} else if (type == BTRFS_SHARED_BLOCK_REF_KEY) {
		btrfs_set_extent_inline_ref_offset(leaf, iref, parent);
	} else {
		btrfs_set_extent_inline_ref_offset(leaf, iref, root_objectid);
	}
	btrfs_mark_buffer_dirty(leaf);
	return 0;
}

static int lookup_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct btrfs_extent_inline_ref **ref_ret,
				 u64 bytenr, u64 num_bytes, u64 parent,
				 u64 root_objectid, u64 owner, u64 offset)
{
	int ret;

	ret = lookup_inline_extent_backref(trans, root, path, ref_ret,
					   bytenr, num_bytes, parent,
					   root_objectid, owner, offset, 0);
	if (ret != -ENOENT)
		return ret;

	btrfs_release_path(path);
	*ref_ret = NULL;

	if (owner < BTRFS_FIRST_FREE_OBJECTID) {
		ret = lookup_tree_block_ref(trans, root, path, bytenr, parent,
					    root_objectid);
	} else {
		ret = lookup_extent_data_ref(trans, root, path, bytenr, parent,
					     root_objectid, owner, offset);
	}
	return ret;
}

static int update_inline_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct btrfs_extent_inline_ref *iref,
				 int refs_to_mod)
{
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_data_ref *dref = NULL;
	struct btrfs_shared_data_ref *sref = NULL;
	unsigned long ptr;
	unsigned long end;
	u32 item_size;
	int size;
	int type;
	int ret;
	u64 refs;

	leaf = path->nodes[0];
	ei = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	refs = btrfs_extent_refs(leaf, ei);
	WARN_ON(refs_to_mod < 0 && refs + refs_to_mod <= 0);
	refs += refs_to_mod;
	btrfs_set_extent_refs(leaf, ei, refs);

	type = btrfs_extent_inline_ref_type(leaf, iref);

	if (type == BTRFS_EXTENT_DATA_REF_KEY) {
		dref = (struct btrfs_extent_data_ref *)(&iref->offset);
		refs = btrfs_extent_data_ref_count(leaf, dref);
	} else if (type == BTRFS_SHARED_DATA_REF_KEY) {
		sref = (struct btrfs_shared_data_ref *)(iref + 1);
		refs = btrfs_shared_data_ref_count(leaf, sref);
	} else {
		refs = 1;
		BUG_ON(refs_to_mod != -1);
	}

	BUG_ON(refs_to_mod < 0 && refs < -refs_to_mod);
	refs += refs_to_mod;

	if (refs > 0) {
		if (type == BTRFS_EXTENT_DATA_REF_KEY)
			btrfs_set_extent_data_ref_count(leaf, dref, refs);
		else
			btrfs_set_shared_data_ref_count(leaf, sref, refs);
	} else {
		size =  btrfs_extent_inline_ref_size(type);
		item_size = btrfs_item_size_nr(leaf, path->slots[0]);
		ptr = (unsigned long)iref;
		end = (unsigned long)ei + item_size;
		if (ptr + size < end)
			memmove_extent_buffer(leaf, ptr, ptr + size,
					      end - ptr - size);
		item_size -= size;
		ret = btrfs_truncate_item(trans, root, path, item_size, 1);
		BUG_ON(ret);
	}
	btrfs_mark_buffer_dirty(leaf);
	return 0;
}

static int insert_inline_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 u64 bytenr, u64 num_bytes, u64 parent,
				 u64 root_objectid, u64 owner,
				 u64 offset, int refs_to_add)
{
	struct btrfs_extent_inline_ref *iref;
	int ret;

	ret = lookup_inline_extent_backref(trans, root, path, &iref,
					   bytenr, num_bytes, parent,
					   root_objectid, owner, offset, 1);
	if (ret == 0) {
		BUG_ON(owner < BTRFS_FIRST_FREE_OBJECTID);
		ret = update_inline_extent_backref(trans, root, path, iref,
						   refs_to_add);
	} else if (ret == -ENOENT) {
		ret = setup_inline_extent_backref(trans, root, path, iref,
						  parent, root_objectid,
						  owner, offset, refs_to_add);
	}
	return ret;
}

static int insert_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 u64 bytenr, u64 parent, u64 root_objectid,
				 u64 owner, u64 offset, int refs_to_add)
{
	int ret;

	if (owner >= BTRFS_FIRST_FREE_OBJECTID) {
		ret = insert_extent_data_ref(trans, root, path, bytenr,
					     parent, root_objectid,
					     owner, offset, refs_to_add);
	} else {
		BUG_ON(refs_to_add != 1);
		ret = insert_tree_block_ref(trans, root, path, bytenr,
					    parent, root_objectid);
	}
	return ret;
}

static int remove_extent_backref(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct btrfs_extent_inline_ref *iref,
				 int refs_to_drop, int is_data)
{
	int ret;

	BUG_ON(!is_data && refs_to_drop != 1);
	if (iref) {
		ret = update_inline_extent_backref(trans, root, path, iref,
						   -refs_to_drop);
	} else if (is_data) {
		ret = remove_extent_data_ref(trans, root, path, refs_to_drop);
	} else {
		ret = btrfs_del_item(trans, root, path);
	}
	return ret;
}

int btrfs_inc_extent_ref(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 u64 bytenr, u64 num_bytes, u64 parent,
			 u64 root_objectid, u64 owner, u64 offset)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *item;
	u64 refs;
	int ret;
	int err = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->reada = 1;
	path->leave_spinning = 1;

	ret = insert_inline_extent_backref(trans, root->fs_info->extent_root,
					   path, bytenr, num_bytes, parent,
					   root_objectid, owner, offset, 1);
	if (ret == 0)
		goto out;

	if (ret != -EAGAIN) {
		err = ret;
		goto out;
	}
	
	leaf = path->nodes[0];
	item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_item);
	refs = btrfs_extent_refs(leaf, item);
	btrfs_set_extent_refs(leaf, item, refs + 1);

	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(path);

	path->reada = 1;
	path->leave_spinning = 1;

	/* now insert the actual backref */
	ret = insert_extent_backref(trans, root->fs_info->extent_root,
				    path, bytenr, parent, root_objectid,
				    owner, offset, 1);
	if (ret)
		err = ret;
out:
	btrfs_free_path(path);
	finish_current_insert(trans, root->fs_info->extent_root);
	del_pending_extents(trans, root->fs_info->extent_root);
	BUG_ON(err);
	return err;
}

int btrfs_extent_post_op(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root)
{
	finish_current_insert(trans, root->fs_info->extent_root);
	del_pending_extents(trans, root->fs_info->extent_root);
	return 0;
}

int btrfs_lookup_extent_info(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 bytenr,
			     u64 offset, int metadata, u64 *refs, u64 *flags)
{
	struct btrfs_path *path;
	int ret;
	struct btrfs_key key;
	struct extent_buffer *l;
	struct btrfs_extent_item *item;
	u32 item_size;
	u64 num_refs;
	u64 extent_flags;

	if (metadata &&
	    !btrfs_fs_incompat(root->fs_info,
			       BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA)) {
		offset = root->leafsize;
		metadata = 0;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	path->reada = 1;

	key.objectid = bytenr;
	key.offset = offset;
	if (metadata)
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;

again:
	ret = btrfs_search_slot(trans, root->fs_info->extent_root, &key, path,
				0, 0);
	if (ret < 0)
		goto out;

	/*
	 * Deal with the fact that we may have mixed SKINNY and normal refs.  If
	 * we didn't find what we wanted check and see if we have a normal ref
	 * right next to us, or re-search if we are on the edge of the leaf just
	 * to make sure.
	 */
	if (ret > 0 && metadata) {
		if (path->slots[0]) {
			path->slots[0]--;
			btrfs_item_key_to_cpu(path->nodes[0], &key,
					      path->slots[0]);
			if (key.objectid == bytenr &&
			    key.type == BTRFS_EXTENT_ITEM_KEY &&
			    key.offset == root->leafsize)
				ret = 0;
		}

		if (ret) {
			btrfs_release_path(path);
			key.type = BTRFS_EXTENT_ITEM_KEY;
			key.offset = root->leafsize;
			metadata = 0;
			goto again;
		}
	}

	if (ret != 0) {
		ret = -EIO;
		goto out;
	}

	l = path->nodes[0];
	item_size = btrfs_item_size_nr(l, path->slots[0]);
	if (item_size >= sizeof(*item)) {
		item = btrfs_item_ptr(l, path->slots[0],
				      struct btrfs_extent_item);
		num_refs = btrfs_extent_refs(l, item);
		extent_flags = btrfs_extent_flags(l, item);
	} else {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
			struct btrfs_extent_item_v0 *ei0;
			BUG_ON(item_size != sizeof(*ei0));
			ei0 = btrfs_item_ptr(l, path->slots[0],
					     struct btrfs_extent_item_v0);
			num_refs = btrfs_extent_refs_v0(l, ei0);
			/* FIXME: this isn't correct for data */
			extent_flags = BTRFS_BLOCK_FLAG_FULL_BACKREF;
#else
			BUG();
#endif
	}
	item = btrfs_item_ptr(l, path->slots[0], struct btrfs_extent_item);
	if (refs)
		*refs = num_refs;
	if (flags)
		*flags = extent_flags;
out:
	btrfs_free_path(path);
	return ret;
}

int btrfs_set_block_flags(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  u64 bytenr, int level, u64 flags)
{
	struct btrfs_path *path;
	int ret;
	struct btrfs_key key;
	struct extent_buffer *l;
	struct btrfs_extent_item *item;
	u32 item_size;
	int skinny_metadata =
		btrfs_fs_incompat(root->fs_info,
				  BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	path->reada = 1;

	key.objectid = bytenr;
	if (skinny_metadata) {
		key.offset = level;
		key.type = BTRFS_METADATA_ITEM_KEY;
	} else {
		key.offset = root->leafsize;
		key.type = BTRFS_EXTENT_ITEM_KEY;
	}

again:
	ret = btrfs_search_slot(trans, root->fs_info->extent_root, &key, path,
				0, 0);
	if (ret < 0)
		goto out;

	if (ret > 0 && skinny_metadata) {
		skinny_metadata = 0;
		if (path->slots[0]) {
			path->slots[0]--;
			btrfs_item_key_to_cpu(path->nodes[0], &key,
					      path->slots[0]);
			if (key.objectid == bytenr &&
			    key.offset == root->leafsize &&
			    key.type == BTRFS_EXTENT_ITEM_KEY)
				ret = 0;
		}
		if (ret) {
			btrfs_release_path(path);
			key.offset = root->leafsize;
			key.type = BTRFS_EXTENT_ITEM_KEY;
			goto again;
		}
	}

	if (ret != 0) {
		btrfs_print_leaf(root, path->nodes[0]);
		printk("failed to find block number %Lu\n",
			(unsigned long long)bytenr);
		BUG();
	}
	l = path->nodes[0];
	item_size = btrfs_item_size_nr(l, path->slots[0]);
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
	if (item_size < sizeof(*item)) {
		ret = convert_extent_item_v0(trans, root->fs_info->extent_root,
					     path, (u64)-1, 0);
		if (ret < 0)
			goto out;

		l = path->nodes[0];
		item_size = btrfs_item_size_nr(l, path->slots[0]);
	}
#endif
	BUG_ON(item_size < sizeof(*item));
	item = btrfs_item_ptr(l, path->slots[0], struct btrfs_extent_item);
	flags |= btrfs_extent_flags(l, item);
	btrfs_set_extent_flags(l, item, flags);
out:
	btrfs_free_path(path);
	finish_current_insert(trans, root->fs_info->extent_root);
	del_pending_extents(trans, root->fs_info->extent_root);
	return ret;
}

static int __btrfs_mod_ref(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct extent_buffer *buf,
			   int record_parent, int inc)
{
	u64 bytenr;
	u64 num_bytes;
	u64 parent;
	u64 ref_root;
	u32 nritems;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	int i;
	int level;
	int ret = 0;
	int (*process_func)(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    u64, u64, u64, u64, u64, u64);

	ref_root = btrfs_header_owner(buf);
	nritems = btrfs_header_nritems(buf);
	level = btrfs_header_level(buf);

	if (!root->ref_cows && level == 0)
		return 0;

	if (inc)
		process_func = btrfs_inc_extent_ref;
	else
		process_func = btrfs_free_extent;

	if (record_parent)
		parent = buf->start;
	else
		parent = 0;

	for (i = 0; i < nritems; i++) {
		cond_resched();
		if (level == 0) {
			btrfs_item_key_to_cpu(buf, &key, i);
			if (btrfs_key_type(&key) != BTRFS_EXTENT_DATA_KEY)
				continue;
			fi = btrfs_item_ptr(buf, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(buf, fi) ==
			    BTRFS_FILE_EXTENT_INLINE)
				continue;
			bytenr = btrfs_file_extent_disk_bytenr(buf, fi);
			if (bytenr == 0)
				continue;
			
			num_bytes = btrfs_file_extent_disk_num_bytes(buf, fi);
			key.offset -= btrfs_file_extent_offset(buf, fi);
			ret = process_func(trans, root, bytenr, num_bytes,
					   parent, ref_root, key.objectid,
					   key.offset);
			if (ret) {
				WARN_ON(1);
				goto fail;
			}
		} else {
			bytenr = btrfs_node_blockptr(buf, i);
			num_bytes = btrfs_level_size(root, level - 1);
			ret = process_func(trans, root, bytenr, num_bytes,
					   parent, ref_root, level - 1, 0);
			if (ret) {
				WARN_ON(1);
				goto fail;
			}
		}
	}
	return 0;
fail:
	WARN_ON(1);
	return ret;
}

int btrfs_inc_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct extent_buffer *buf, int record_parent)
{
	return __btrfs_mod_ref(trans, root, buf, record_parent, 1);
}

int btrfs_dec_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct extent_buffer *buf, int record_parent)
{
	return __btrfs_mod_ref(trans, root, buf, record_parent, 0);
}

static int write_one_cache_group(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct btrfs_block_group_cache *cache)
{
	int ret;
	int pending_ret;
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	unsigned long bi;
	struct extent_buffer *leaf;

	ret = btrfs_search_slot(trans, extent_root, &cache->key, path, 0, 1);
	if (ret < 0)
		goto fail;
	BUG_ON(ret);

	leaf = path->nodes[0];
	bi = btrfs_item_ptr_offset(leaf, path->slots[0]);
	write_extent_buffer(leaf, &cache->item, bi, sizeof(cache->item));
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(path);
fail:
	finish_current_insert(trans, extent_root);
	pending_ret = del_pending_extents(trans, extent_root);
	if (ret)
		return ret;
	if (pending_ret)
		return pending_ret;
	return 0;

}

int btrfs_write_dirty_block_groups(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root)
{
	struct extent_io_tree *block_group_cache;
	struct btrfs_block_group_cache *cache;
	int ret;
	struct btrfs_path *path;
	u64 last = 0;
	u64 start;
	u64 end;
	u64 ptr;

	block_group_cache = &root->fs_info->block_group_cache;
	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	while(1) {
		ret = find_first_extent_bit(block_group_cache, last,
					    &start, &end, BLOCK_GROUP_DIRTY);
		if (ret) {
			if (last == 0)
				break;
			last = 0;
			continue;
		}

		last = end + 1;
		ret = get_state_private(block_group_cache, start, &ptr);
		BUG_ON(ret);

		clear_extent_bits(block_group_cache, start, end,
				  BLOCK_GROUP_DIRTY, GFP_NOFS);

		cache = (struct btrfs_block_group_cache *)(unsigned long)ptr;
		ret = write_one_cache_group(trans, root, path, cache);
	}
	btrfs_free_path(path);
	return 0;
}

static struct btrfs_space_info *__find_space_info(struct btrfs_fs_info *info,
						  u64 flags)
{
	struct list_head *head = &info->space_info;
	struct list_head *cur;
	struct btrfs_space_info *found;
	list_for_each(cur, head) {
		found = list_entry(cur, struct btrfs_space_info, list);
		if (found->flags & flags)
			return found;
	}
	return NULL;

}

static int update_space_info(struct btrfs_fs_info *info, u64 flags,
			     u64 total_bytes, u64 bytes_used,
			     struct btrfs_space_info **space_info)
{
	struct btrfs_space_info *found;

	found = __find_space_info(info, flags);
	if (found) {
		found->total_bytes += total_bytes;
		found->bytes_used += bytes_used;
		if (found->total_bytes < found->bytes_used) {
			fprintf(stderr, "warning, bad space info total_bytes "
				"%llu used %llu\n",
			       (unsigned long long)found->total_bytes,
			       (unsigned long long)found->bytes_used);
		}
		*space_info = found;
		return 0;
	}
	found = kmalloc(sizeof(*found), GFP_NOFS);
	if (!found)
		return -ENOMEM;

	list_add(&found->list, &info->space_info);
	found->flags = flags;
	found->total_bytes = total_bytes;
	found->bytes_used = bytes_used;
	found->bytes_pinned = 0;
	found->full = 0;
	*space_info = found;
	return 0;
}


static void set_avail_alloc_bits(struct btrfs_fs_info *fs_info, u64 flags)
{
	u64 extra_flags = flags & (BTRFS_BLOCK_GROUP_RAID0 |
				   BTRFS_BLOCK_GROUP_RAID1 |
				   BTRFS_BLOCK_GROUP_RAID10 |
				   BTRFS_BLOCK_GROUP_RAID5 |
				   BTRFS_BLOCK_GROUP_RAID6 |
				   BTRFS_BLOCK_GROUP_DUP);
	if (extra_flags) {
		if (flags & BTRFS_BLOCK_GROUP_DATA)
			fs_info->avail_data_alloc_bits |= extra_flags;
		if (flags & BTRFS_BLOCK_GROUP_METADATA)
			fs_info->avail_metadata_alloc_bits |= extra_flags;
		if (flags & BTRFS_BLOCK_GROUP_SYSTEM)
			fs_info->avail_system_alloc_bits |= extra_flags;
	}
}

static int do_chunk_alloc(struct btrfs_trans_handle *trans,
			  struct btrfs_root *extent_root, u64 alloc_bytes,
			  u64 flags)
{
	struct btrfs_space_info *space_info;
	u64 thresh;
	u64 start;
	u64 num_bytes;
	int ret;

	space_info = __find_space_info(extent_root->fs_info, flags);
	if (!space_info) {
		ret = update_space_info(extent_root->fs_info, flags,
					0, 0, &space_info);
		BUG_ON(ret);
	}
	BUG_ON(!space_info);

	if (space_info->full)
		return 0;

	thresh = div_factor(space_info->total_bytes, 7);
	if ((space_info->bytes_used + space_info->bytes_pinned + alloc_bytes) <
	    thresh)
		return 0;

	ret = btrfs_alloc_chunk(trans, extent_root, &start, &num_bytes,
	                        space_info->flags);
	if (ret == -ENOSPC) {
		space_info->full = 1;
		return 0;
	}

	BUG_ON(ret);

	ret = btrfs_make_block_group(trans, extent_root, 0, space_info->flags,
		     BTRFS_FIRST_CHUNK_TREE_OBJECTID, start, num_bytes);
	BUG_ON(ret);
	return 0;
}

static int update_block_group(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root,
			      u64 bytenr, u64 num_bytes, int alloc,
			      int mark_free)
{
	struct btrfs_block_group_cache *cache;
	struct btrfs_fs_info *info = root->fs_info;
	u64 total = num_bytes;
	u64 old_val;
	u64 byte_in_group;
	u64 start;
	u64 end;

	/* block accounting for super block */
	old_val = btrfs_super_bytes_used(info->super_copy);
	if (alloc)
		old_val += num_bytes;
	else
		old_val -= num_bytes;
	btrfs_set_super_bytes_used(info->super_copy, old_val);

	/* block accounting for root item */
	old_val = btrfs_root_used(&root->root_item);
	if (alloc)
		old_val += num_bytes;
	else
		old_val -= num_bytes;
	btrfs_set_root_used(&root->root_item, old_val);

	while(total) {
		cache = btrfs_lookup_block_group(info, bytenr);
		if (!cache) {
			return -1;
		}
		byte_in_group = bytenr - cache->key.objectid;
		WARN_ON(byte_in_group > cache->key.offset);
		start = cache->key.objectid;
		end = start + cache->key.offset - 1;
		set_extent_bits(&info->block_group_cache, start, end,
				BLOCK_GROUP_DIRTY, GFP_NOFS);

		old_val = btrfs_block_group_used(&cache->item);
		num_bytes = min(total, cache->key.offset - byte_in_group);

		if (alloc) {
			old_val += num_bytes;
			cache->space_info->bytes_used += num_bytes;
		} else {
			old_val -= num_bytes;
			cache->space_info->bytes_used -= num_bytes;
			if (mark_free) {
				set_extent_dirty(&info->free_space_cache,
						 bytenr, bytenr + num_bytes - 1,
						 GFP_NOFS);
			}
		}
		btrfs_set_block_group_used(&cache->item, old_val);
		total -= num_bytes;
		bytenr += num_bytes;
	}
	return 0;
}

static int update_pinned_extents(struct btrfs_root *root,
				u64 bytenr, u64 num, int pin)
{
	u64 len;
	struct btrfs_block_group_cache *cache;
	struct btrfs_fs_info *fs_info = root->fs_info;

	if (pin) {
		set_extent_dirty(&fs_info->pinned_extents,
				bytenr, bytenr + num - 1, GFP_NOFS);
	} else {
		clear_extent_dirty(&fs_info->pinned_extents,
				bytenr, bytenr + num - 1, GFP_NOFS);
	}
	while (num > 0) {
		cache = btrfs_lookup_block_group(fs_info, bytenr);
		if (!cache) {
			len = min((u64)root->sectorsize, num);
			goto next;
		}
		WARN_ON(!cache);
		len = min(num, cache->key.offset -
			  (bytenr - cache->key.objectid));
		if (pin) {
			cache->pinned += len;
			cache->space_info->bytes_pinned += len;
			fs_info->total_pinned += len;
		} else {
			cache->pinned -= len;
			cache->space_info->bytes_pinned -= len;
			fs_info->total_pinned -= len;
		}
next:
		bytenr += len;
		num -= len;
	}
	return 0;
}

int btrfs_finish_extent_commit(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root,
			       struct extent_io_tree *unpin)
{
	u64 start;
	u64 end;
	int ret;
	struct extent_io_tree *free_space_cache;
	free_space_cache = &root->fs_info->free_space_cache;

	while(1) {
		ret = find_first_extent_bit(unpin, 0, &start, &end,
					    EXTENT_DIRTY);
		if (ret)
			break;
		update_pinned_extents(root, start, end + 1 - start, 0);
		clear_extent_dirty(unpin, start, end, GFP_NOFS);
		set_extent_dirty(free_space_cache, start, end, GFP_NOFS);
	}
	return 0;
}

static int extent_root_pending_ops(struct btrfs_fs_info *info)
{
	u64 start;
	u64 end;
	int ret;

	ret = find_first_extent_bit(&info->extent_ins, 0, &start,
				    &end, EXTENT_LOCKED);
	if (!ret) {
		ret = find_first_extent_bit(&info->pending_del, 0, &start, &end,
					    EXTENT_LOCKED);
	}
	return ret == 0;

}
static int finish_current_insert(struct btrfs_trans_handle *trans,
				 struct btrfs_root *extent_root)
{
	u64 start;
	u64 end;
	u64 priv;
	struct btrfs_fs_info *info = extent_root->fs_info;
	struct pending_extent_op *extent_op;
	struct btrfs_key key;
	int ret;
	int skinny_metadata =
		btrfs_fs_incompat(extent_root->fs_info,
				  BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA);

	while(1) {
		ret = find_first_extent_bit(&info->extent_ins, 0, &start,
					    &end, EXTENT_LOCKED);
		if (ret)
			break;

		ret = get_state_private(&info->extent_ins, start, &priv);
		BUG_ON(ret);
		extent_op = (struct pending_extent_op *)(unsigned long)priv;

		if (extent_op->type == PENDING_EXTENT_INSERT) {
			key.objectid = start;
			if (skinny_metadata) {
				key.offset = extent_op->level;
				key.type = BTRFS_METADATA_ITEM_KEY;
			} else {
				key.offset = extent_op->num_bytes;
				key.type = BTRFS_EXTENT_ITEM_KEY;
			}
			ret = alloc_reserved_tree_block(trans, extent_root,
						extent_root->root_key.objectid,
						trans->transid,
						extent_op->flags,
						&extent_op->key,
						extent_op->level, &key);
			BUG_ON(ret);
		} else {
			BUG_ON(1);
		}

		clear_extent_bits(&info->extent_ins, start, end, EXTENT_LOCKED,
				  GFP_NOFS);
		kfree(extent_op);
	}
	return 0;
}

static int pin_down_bytes(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  u64 bytenr, u64 num_bytes, int is_data)
{
	int err = 0;
	struct extent_buffer *buf;

	if (is_data)
		goto pinit;

	buf = btrfs_find_tree_block(root, bytenr, num_bytes);
	if (!buf)
		goto pinit;

	/* we can reuse a block if it hasn't been written
	 * and it is from this transaction.  We can't
	 * reuse anything from the tree log root because
	 * it has tiny sub-transactions.
	 */
	if (btrfs_buffer_uptodate(buf, 0)) {
		u64 header_owner = btrfs_header_owner(buf);
		u64 header_transid = btrfs_header_generation(buf);
		if (header_owner != BTRFS_TREE_LOG_OBJECTID &&
		    header_transid == trans->transid &&
		    !btrfs_header_flag(buf, BTRFS_HEADER_FLAG_WRITTEN)) {
			clean_tree_block(NULL, root, buf);
			free_extent_buffer(buf);
			return 1;
		}
	}
	free_extent_buffer(buf);
pinit:
	update_pinned_extents(root, bytenr, num_bytes, 1);

	BUG_ON(err < 0);
	return 0;
}

void btrfs_pin_extent(struct btrfs_fs_info *fs_info,
		       u64 bytenr, u64 num_bytes)
{
	update_pinned_extents(fs_info->extent_root, bytenr, num_bytes, 1);
}

void btrfs_unpin_extent(struct btrfs_fs_info *fs_info,
			u64 bytenr, u64 num_bytes)
{
	update_pinned_extents(fs_info->extent_root, bytenr, num_bytes, 0);
}

/*
 * remove an extent from the root, returns 0 on success
 */
static int __free_extent(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 u64 bytenr, u64 num_bytes, u64 parent,
			 u64 root_objectid, u64 owner_objectid,
			 u64 owner_offset, int refs_to_drop)
{

	struct btrfs_key key;
	struct btrfs_path *path;
	struct btrfs_extent_ops *ops = root->fs_info->extent_ops;
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	int ret;
	int is_data;
	int extent_slot = 0;
	int found_extent = 0;
	int num_to_del = 1;
	u32 item_size;
	u64 refs;
	int skinny_metadata =
		btrfs_fs_incompat(extent_root->fs_info,
				  BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA);

	if (root->fs_info->free_extent_hook) {
		root->fs_info->free_extent_hook(trans, root, bytenr, num_bytes,
						parent, root_objectid, owner_objectid,
						owner_offset, refs_to_drop);

	}
	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->reada = 1;
	path->leave_spinning = 1;

	is_data = owner_objectid >= BTRFS_FIRST_FREE_OBJECTID;
	if (is_data)
		skinny_metadata = 0;
	BUG_ON(!is_data && refs_to_drop != 1);

	ret = lookup_extent_backref(trans, extent_root, path, &iref,
				    bytenr, num_bytes, parent,
				    root_objectid, owner_objectid,
				    owner_offset);
	if (ret == 0) {
		extent_slot = path->slots[0];
		while (extent_slot >= 0) {
			btrfs_item_key_to_cpu(path->nodes[0], &key,
					      extent_slot);
			if (key.objectid != bytenr)
				break;
			if (key.type == BTRFS_EXTENT_ITEM_KEY &&
			    key.offset == num_bytes) {
				found_extent = 1;
				break;
			}
			if (key.type == BTRFS_METADATA_ITEM_KEY &&
			    key.offset == owner_objectid) {
				found_extent = 1;
				break;
			}
			if (path->slots[0] - extent_slot > 5)
				break;
			extent_slot--;
		}
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
		item_size = btrfs_item_size_nr(path->nodes[0], extent_slot);
		if (found_extent && item_size < sizeof(*ei))
			found_extent = 0;
#endif
		if (!found_extent) {
			BUG_ON(iref);
			ret = remove_extent_backref(trans, extent_root, path,
						    NULL, refs_to_drop,
						    is_data);
			BUG_ON(ret);
			btrfs_release_path(path);
			path->leave_spinning = 1;

			key.objectid = bytenr;

			if (skinny_metadata) {
				key.type = BTRFS_METADATA_ITEM_KEY;
				key.offset = owner_objectid;
			} else {
				key.type = BTRFS_EXTENT_ITEM_KEY;
				key.offset = num_bytes;
			}

			ret = btrfs_search_slot(trans, extent_root,
						&key, path, -1, 1);
			if (ret > 0 && skinny_metadata && path->slots[0]) {
				path->slots[0]--;
				btrfs_item_key_to_cpu(path->nodes[0],
						      &key,
						      path->slots[0]);
				if (key.objectid == bytenr &&
				    key.type == BTRFS_EXTENT_ITEM_KEY &&
				    key.offset == num_bytes)
					ret = 0;
			}

			if (ret > 0 && skinny_metadata) {
				skinny_metadata = 0;
				btrfs_release_path(path);
				key.type = BTRFS_EXTENT_ITEM_KEY;
				key.offset = num_bytes;
				ret = btrfs_search_slot(trans, extent_root,
							&key, path, -1, 1);
			}

			if (ret) {
				printk(KERN_ERR "umm, got %d back from search"
				       ", was looking for %llu\n", ret,
				       (unsigned long long)bytenr);
				btrfs_print_leaf(extent_root, path->nodes[0]);
			}
			BUG_ON(ret);
			extent_slot = path->slots[0];
		}
	} else {
		printk(KERN_ERR "btrfs unable to find ref byte nr %llu "
		       "parent %llu root %llu  owner %llu offset %llu\n",
		       (unsigned long long)bytenr,
		       (unsigned long long)parent,
		       (unsigned long long)root_objectid,
		       (unsigned long long)owner_objectid,
		       (unsigned long long)owner_offset);
		ret = -EIO;
		goto fail;
	}

	leaf = path->nodes[0];
	item_size = btrfs_item_size_nr(leaf, extent_slot);
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
	if (item_size < sizeof(*ei)) {
		BUG_ON(found_extent || extent_slot != path->slots[0]);
		ret = convert_extent_item_v0(trans, extent_root, path,
					     owner_objectid, 0);
		BUG_ON(ret < 0);

		btrfs_release_path(path);
		path->leave_spinning = 1;

		key.objectid = bytenr;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = num_bytes;

		ret = btrfs_search_slot(trans, extent_root, &key, path,
					-1, 1);
		if (ret) {
			printk(KERN_ERR "umm, got %d back from search"
			       ", was looking for %llu\n", ret,
			       (unsigned long long)bytenr);
			btrfs_print_leaf(extent_root, path->nodes[0]);
		}
		BUG_ON(ret);
		extent_slot = path->slots[0];
		leaf = path->nodes[0];
		item_size = btrfs_item_size_nr(leaf, extent_slot);
	}
#endif
	BUG_ON(item_size < sizeof(*ei));
	ei = btrfs_item_ptr(leaf, extent_slot,
			    struct btrfs_extent_item);
	if (owner_objectid < BTRFS_FIRST_FREE_OBJECTID &&
	    key.type == BTRFS_EXTENT_ITEM_KEY) {
		struct btrfs_tree_block_info *bi;
		BUG_ON(item_size < sizeof(*ei) + sizeof(*bi));
		bi = (struct btrfs_tree_block_info *)(ei + 1);
		WARN_ON(owner_objectid != btrfs_tree_block_level(leaf, bi));
	}

	refs = btrfs_extent_refs(leaf, ei);
	BUG_ON(refs < refs_to_drop);
	refs -= refs_to_drop;

	if (refs > 0) {
		/*
		 * In the case of inline back ref, reference count will
		 * be updated by remove_extent_backref
		 */
		if (iref) {
			BUG_ON(!found_extent);
		} else {
			btrfs_set_extent_refs(leaf, ei, refs);
			btrfs_mark_buffer_dirty(leaf);
		}
		if (found_extent) {
			ret = remove_extent_backref(trans, extent_root, path,
						    iref, refs_to_drop,
						    is_data);
			BUG_ON(ret);
		}
	} else {
		int mark_free = 0;
		int pin = 1;

		if (found_extent) {
			BUG_ON(is_data && refs_to_drop !=
			       extent_data_ref_count(root, path, iref));
			if (iref) {
				BUG_ON(path->slots[0] != extent_slot);
			} else {
				BUG_ON(path->slots[0] != extent_slot + 1);
				path->slots[0] = extent_slot;
				num_to_del = 2;
			}
		}

		if (ops && ops->free_extent) {
			ret = ops->free_extent(root, bytenr, num_bytes);
			if (ret > 0) {
				pin = 0;
				mark_free = 0;
			}
		}

		if (pin) {
			ret = pin_down_bytes(trans, root, bytenr, num_bytes,
					     is_data);
			if (ret > 0)
				mark_free = 1;
			BUG_ON(ret < 0);
		}

		ret = btrfs_del_items(trans, extent_root, path, path->slots[0],
				      num_to_del);
		BUG_ON(ret);
		btrfs_release_path(path);

		if (is_data) {
			ret = btrfs_del_csums(trans, root, bytenr, num_bytes);
			BUG_ON(ret);
		}

		update_block_group(trans, root, bytenr, num_bytes, 0, mark_free);
	}
fail:
	btrfs_free_path(path);
	finish_current_insert(trans, extent_root);
	return ret;
}

/*
 * find all the blocks marked as pending in the radix tree and remove
 * them from the extent map
 */
static int del_pending_extents(struct btrfs_trans_handle *trans, struct
			       btrfs_root *extent_root)
{
	int ret;
	int err = 0;
	u64 start;
	u64 end;
	u64 priv;
	struct extent_io_tree *pending_del;
	struct extent_io_tree *extent_ins;
	struct pending_extent_op *extent_op;

	extent_ins = &extent_root->fs_info->extent_ins;
	pending_del = &extent_root->fs_info->pending_del;

	while(1) {
		ret = find_first_extent_bit(pending_del, 0, &start, &end,
					    EXTENT_LOCKED);
		if (ret)
			break;

		ret = get_state_private(pending_del, start, &priv);
		BUG_ON(ret);
		extent_op = (struct pending_extent_op *)(unsigned long)priv;

		clear_extent_bits(pending_del, start, end, EXTENT_LOCKED,
				  GFP_NOFS);

		if (!test_range_bit(extent_ins, start, end,
				    EXTENT_LOCKED, 0)) {
			ret = __free_extent(trans, extent_root,
					    start, end + 1 - start, 0,
					    extent_root->root_key.objectid,
					    extent_op->level, 0, 1);
			kfree(extent_op);
		} else {
			kfree(extent_op);
			ret = get_state_private(extent_ins, start, &priv);
			BUG_ON(ret);
			extent_op = (struct pending_extent_op *)
							(unsigned long)priv;

			clear_extent_bits(extent_ins, start, end,
					  EXTENT_LOCKED, GFP_NOFS);

			if (extent_op->type == PENDING_BACKREF_UPDATE)
				BUG_ON(1);

			kfree(extent_op);
		}
		if (ret)
			err = ret;
	}
	return err;
}

/*
 * remove an extent from the root, returns 0 on success
 */

int btrfs_free_extent(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root,
		      u64 bytenr, u64 num_bytes, u64 parent,
		      u64 root_objectid, u64 owner, u64 offset)
{
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	int pending_ret;
	int ret;

	WARN_ON(num_bytes < root->sectorsize);
	if (root == extent_root) {
		struct pending_extent_op *extent_op;

		extent_op = kmalloc(sizeof(*extent_op), GFP_NOFS);
		BUG_ON(!extent_op);

		extent_op->type = PENDING_EXTENT_DELETE;
		extent_op->bytenr = bytenr;
		extent_op->num_bytes = num_bytes;
		extent_op->level = (int)owner;

		set_extent_bits(&root->fs_info->pending_del,
				bytenr, bytenr + num_bytes - 1,
				EXTENT_LOCKED, GFP_NOFS);
		set_state_private(&root->fs_info->pending_del,
				  bytenr, (unsigned long)extent_op);
		return 0;
	}
	ret = __free_extent(trans, root, bytenr, num_bytes, parent,
			    root_objectid, owner, offset, 1);
	pending_ret = del_pending_extents(trans, root->fs_info->extent_root);
	return ret ? ret : pending_ret;
}

static u64 stripe_align(struct btrfs_root *root, u64 val)
{
	u64 mask = ((u64)root->stripesize - 1);
	u64 ret = (val + mask) & ~mask;
	return ret;
}

/*
 * walks the btree of allocated extents and find a hole of a given size.
 * The key ins is changed to record the hole:
 * ins->objectid == block start
 * ins->flags = BTRFS_EXTENT_ITEM_KEY
 * ins->offset == number of blocks
 * Any available blocks before search_start are skipped.
 */
static int noinline find_free_extent(struct btrfs_trans_handle *trans,
				     struct btrfs_root *orig_root,
				     u64 num_bytes, u64 empty_size,
				     u64 search_start, u64 search_end,
				     u64 hint_byte, struct btrfs_key *ins,
				     u64 exclude_start, u64 exclude_nr,
				     int data)
{
	int ret;
	u64 orig_search_start = search_start;
	struct btrfs_root * root = orig_root->fs_info->extent_root;
	struct btrfs_fs_info *info = root->fs_info;
	u64 total_needed = num_bytes;
	struct btrfs_block_group_cache *block_group;
	int full_scan = 0;
	int wrapped = 0;

	WARN_ON(num_bytes < root->sectorsize);
	btrfs_set_key_type(ins, BTRFS_EXTENT_ITEM_KEY);

	search_start = stripe_align(root, search_start);

	if (hint_byte) {
		block_group = btrfs_lookup_first_block_group(info, hint_byte);
		if (!block_group)
			hint_byte = search_start;
		block_group = btrfs_find_block_group(root, block_group,
						     hint_byte, data, 1);
	} else {
		block_group = btrfs_find_block_group(root,
						     trans->block_group,
						     search_start, data, 1);
	}

	total_needed += empty_size;

check_failed:
	search_start = stripe_align(root, search_start);
	if (!block_group) {
		block_group = btrfs_lookup_first_block_group(info,
							     search_start);
		if (!block_group)
			block_group = btrfs_lookup_first_block_group(info,
						       orig_search_start);
	}
	ret = find_search_start(root, &block_group, &search_start,
				total_needed, data);
	if (ret)
		goto new_group;

	ins->objectid = search_start;
	ins->offset = num_bytes;

	if (ins->objectid + num_bytes >
	    block_group->key.objectid + block_group->key.offset) {
		search_start = block_group->key.objectid +
			block_group->key.offset;
		goto new_group;
	}

	if (test_range_bit(&info->extent_ins, ins->objectid,
			   ins->objectid + num_bytes -1, EXTENT_LOCKED, 0)) {
		search_start = ins->objectid + num_bytes;
		goto new_group;
	}

	if (test_range_bit(&info->pinned_extents, ins->objectid,
			   ins->objectid + num_bytes -1, EXTENT_DIRTY, 0)) {
		search_start = ins->objectid + num_bytes;
		goto new_group;
	}

	if (exclude_nr > 0 && (ins->objectid + num_bytes > exclude_start &&
	    ins->objectid < exclude_start + exclude_nr)) {
		search_start = exclude_start + exclude_nr;
		goto new_group;
	}

	if (!(data & BTRFS_BLOCK_GROUP_DATA)) {
		block_group = btrfs_lookup_block_group(info, ins->objectid);
		if (block_group)
			trans->block_group = block_group;
	}
	ins->offset = num_bytes;
	return 0;

new_group:
	block_group = btrfs_lookup_first_block_group(info, search_start);
	if (!block_group) {
		search_start = orig_search_start;
		if (full_scan) {
			ret = -ENOSPC;
			goto error;
		}
		if (wrapped) {
			if (!full_scan)
				total_needed -= empty_size;
			full_scan = 1;
		} else
			wrapped = 1;
	}
	cond_resched();
	block_group = btrfs_find_block_group(root, block_group,
					     search_start, data, 0);
	goto check_failed;

error:
	return ret;
}

int btrfs_reserve_extent(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 u64 num_bytes, u64 empty_size,
			 u64 hint_byte, u64 search_end,
			 struct btrfs_key *ins, int data)
{
	int ret;
	u64 search_start = 0;
	u64 alloc_profile;
	struct btrfs_fs_info *info = root->fs_info;

	if (info->extent_ops) {
		struct btrfs_extent_ops *ops = info->extent_ops;
		ret = ops->alloc_extent(root, num_bytes, hint_byte, ins);
		BUG_ON(ret);
		goto found;
	}

	if (data) {
		alloc_profile = info->avail_data_alloc_bits &
			        info->data_alloc_profile;
		data = BTRFS_BLOCK_GROUP_DATA | alloc_profile;
	} else if ((info->system_allocs > 0 || root == info->chunk_root) &&
		   info->system_allocs >= 0) {
		alloc_profile = info->avail_system_alloc_bits &
			        info->system_alloc_profile;
		data = BTRFS_BLOCK_GROUP_SYSTEM | alloc_profile;
	} else {
		alloc_profile = info->avail_metadata_alloc_bits &
			        info->metadata_alloc_profile;
		data = BTRFS_BLOCK_GROUP_METADATA | alloc_profile;
	}

	if (root->ref_cows) {
		if (!(data & BTRFS_BLOCK_GROUP_METADATA)) {
			ret = do_chunk_alloc(trans, root->fs_info->extent_root,
					     num_bytes,
					     BTRFS_BLOCK_GROUP_METADATA);
			BUG_ON(ret);
		}
		ret = do_chunk_alloc(trans, root->fs_info->extent_root,
				     num_bytes + 2 * 1024 * 1024, data);
		BUG_ON(ret);
	}

	WARN_ON(num_bytes < root->sectorsize);
	ret = find_free_extent(trans, root, num_bytes, empty_size,
			       search_start, search_end, hint_byte, ins,
			       trans->alloc_exclude_start,
			       trans->alloc_exclude_nr, data);
	BUG_ON(ret);
found:
	clear_extent_dirty(&root->fs_info->free_space_cache,
			   ins->objectid, ins->objectid + ins->offset - 1,
			   GFP_NOFS);
	return ret;
}

static int alloc_reserved_tree_block(struct btrfs_trans_handle *trans,
				     struct btrfs_root *root,
				     u64 root_objectid, u64 generation,
				     u64 flags, struct btrfs_disk_key *key,
				     int level, struct btrfs_key *ins)
{
	int ret;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_extent_item *extent_item;
	struct btrfs_tree_block_info *block_info;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	u32 size = sizeof(*extent_item) + sizeof(*iref);
	int skinny_metadata =
		btrfs_fs_incompat(fs_info,
				  BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA);

	if (!skinny_metadata)
		size += sizeof(*block_info);

	path = btrfs_alloc_path();
	BUG_ON(!path);

	path->leave_spinning = 1;
	ret = btrfs_insert_empty_item(trans, fs_info->extent_root, path,
				      ins, size);
	BUG_ON(ret);

	leaf = path->nodes[0];
	extent_item = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_item);
	btrfs_set_extent_refs(leaf, extent_item, 1);
	btrfs_set_extent_generation(leaf, extent_item, generation);
	btrfs_set_extent_flags(leaf, extent_item,
			       flags | BTRFS_EXTENT_FLAG_TREE_BLOCK);

	if (skinny_metadata) {
		iref = (struct btrfs_extent_inline_ref *)(extent_item + 1);
	} else {
		block_info = (struct btrfs_tree_block_info *)(extent_item + 1);
		btrfs_set_tree_block_key(leaf, block_info, key);
		btrfs_set_tree_block_level(leaf, block_info, level);
		iref = (struct btrfs_extent_inline_ref *)(block_info + 1);
	}

	btrfs_set_extent_inline_ref_type(leaf, iref, BTRFS_TREE_BLOCK_REF_KEY);
	btrfs_set_extent_inline_ref_offset(leaf, iref, root_objectid);

	btrfs_mark_buffer_dirty(leaf);
	btrfs_free_path(path);

	ret = update_block_group(trans, root, ins->objectid, root->leafsize,
				 1, 0);
	return ret;
}

static int alloc_tree_block(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root, u64 num_bytes,
			    u64 root_objectid, u64 generation,
			    u64 flags, struct btrfs_disk_key *key,
			    int level, u64 empty_size, u64 hint_byte,
			    u64 search_end, struct btrfs_key *ins)
{
	int ret;
	ret = btrfs_reserve_extent(trans, root, num_bytes, empty_size,
				   hint_byte, search_end, ins, 0);
	BUG_ON(ret);

	if (root_objectid == BTRFS_EXTENT_TREE_OBJECTID) {
		struct pending_extent_op *extent_op;

		extent_op = kmalloc(sizeof(*extent_op), GFP_NOFS);
		BUG_ON(!extent_op);

		extent_op->type = PENDING_EXTENT_INSERT;
		extent_op->bytenr = ins->objectid;
		extent_op->num_bytes = ins->offset;
		extent_op->level = level;
		extent_op->flags = flags;
		memcpy(&extent_op->key, key, sizeof(*key));

		set_extent_bits(&root->fs_info->extent_ins, ins->objectid,
				ins->objectid + ins->offset - 1,
				EXTENT_LOCKED, GFP_NOFS);
		set_state_private(&root->fs_info->extent_ins,
				  ins->objectid, (unsigned long)extent_op);
	} else {
		if (btrfs_fs_incompat(root->fs_info,
				BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA)) {
			ins->offset = level;
			ins->type = BTRFS_METADATA_ITEM_KEY;
		}
		ret = alloc_reserved_tree_block(trans, root, root_objectid,
						generation, flags,
						key, level, ins);
		finish_current_insert(trans, root->fs_info->extent_root);
		del_pending_extents(trans, root->fs_info->extent_root);
	}
	return ret;
}

/*
 * helper function to allocate a block for a given tree
 * returns the tree buffer or NULL.
 */
struct extent_buffer *btrfs_alloc_free_block(struct btrfs_trans_handle *trans,
					struct btrfs_root *root,
					u32 blocksize, u64 root_objectid,
					struct btrfs_disk_key *key, int level,
					u64 hint, u64 empty_size)
{
	struct btrfs_key ins;
	int ret;
	struct extent_buffer *buf;

	ret = alloc_tree_block(trans, root, blocksize, root_objectid,
			       trans->transid, 0, key, level,
			       empty_size, hint, (u64)-1, &ins);
	if (ret) {
		BUG_ON(ret > 0);
		return ERR_PTR(ret);
	}

	buf = btrfs_find_create_tree_block(root, ins.objectid, blocksize);
	if (!buf) {
		btrfs_free_extent(trans, root, ins.objectid, ins.offset,
				  0, root->root_key.objectid, level, 0);
		BUG_ON(1);
		return ERR_PTR(-ENOMEM);
	}
	btrfs_set_buffer_uptodate(buf);
	trans->blocks_used++;

	return buf;
}

#if 0

static int noinline drop_leaf_ref(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root,
				  struct extent_buffer *leaf)
{
	u64 leaf_owner;
	u64 leaf_generation;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	int i;
	int nritems;
	int ret;

	BUG_ON(!btrfs_is_leaf(leaf));
	nritems = btrfs_header_nritems(leaf);
	leaf_owner = btrfs_header_owner(leaf);
	leaf_generation = btrfs_header_generation(leaf);

	for (i = 0; i < nritems; i++) {
		u64 disk_bytenr;

		btrfs_item_key_to_cpu(leaf, &key, i);
		if (btrfs_key_type(&key) != BTRFS_EXTENT_DATA_KEY)
			continue;
		fi = btrfs_item_ptr(leaf, i, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(leaf, fi) ==
		    BTRFS_FILE_EXTENT_INLINE)
			continue;
		/*
		 * FIXME make sure to insert a trans record that
		 * repeats the snapshot del on crash
		 */
		disk_bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
		if (disk_bytenr == 0)
			continue;
		ret = btrfs_free_extent(trans, root, disk_bytenr,
				btrfs_file_extent_disk_num_bytes(leaf, fi),
				leaf->start, leaf_owner, leaf_generation,
				key.objectid, 0);
		BUG_ON(ret);
	}
	return 0;
}

static void noinline reada_walk_down(struct btrfs_root *root,
				     struct extent_buffer *node,
				     int slot)
{
	u64 bytenr;
	u64 last = 0;
	u32 nritems;
	u32 refs;
	u32 blocksize;
	int ret;
	int i;
	int level;
	int skipped = 0;

	nritems = btrfs_header_nritems(node);
	level = btrfs_header_level(node);
	if (level)
		return;

	for (i = slot; i < nritems && skipped < 32; i++) {
		bytenr = btrfs_node_blockptr(node, i);
		if (last && ((bytenr > last && bytenr - last > 32 * 1024) ||
			     (last > bytenr && last - bytenr > 32 * 1024))) {
			skipped++;
			continue;
		}
		blocksize = btrfs_level_size(root, level - 1);
		if (i != slot) {
			ret = btrfs_lookup_extent_ref(NULL, root, bytenr,
						      blocksize, &refs);
			BUG_ON(ret);
			if (refs != 1) {
				skipped++;
				continue;
			}
		}
		mutex_unlock(&root->fs_info->fs_mutex);
		ret = readahead_tree_block(root, bytenr, blocksize,
					   btrfs_node_ptr_generation(node, i));
		last = bytenr + blocksize;
		cond_resched();
		mutex_lock(&root->fs_info->fs_mutex);
		if (ret)
			break;
	}
}

/*
 * helper function for drop_snapshot, this walks down the tree dropping ref
 * counts as it goes.
 */
static int noinline walk_down_tree(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct btrfs_path *path, int *level)
{
	u64 root_owner;
	u64 root_gen;
	u64 bytenr;
	u64 ptr_gen;
	struct extent_buffer *next;
	struct extent_buffer *cur;
	struct extent_buffer *parent;
	u32 blocksize;
	int ret;
	u32 refs;

	WARN_ON(*level < 0);
	WARN_ON(*level >= BTRFS_MAX_LEVEL);
	ret = btrfs_lookup_extent_ref(trans, root,
				      path->nodes[*level]->start,
				      path->nodes[*level]->len, &refs);
	BUG_ON(ret);
	if (refs > 1)
		goto out;

	/*
	 * walk down to the last node level and free all the leaves
	 */
	while(*level >= 0) {
		WARN_ON(*level < 0);
		WARN_ON(*level >= BTRFS_MAX_LEVEL);
		cur = path->nodes[*level];

		if (btrfs_header_level(cur) != *level)
			WARN_ON(1);

		if (path->slots[*level] >=
		    btrfs_header_nritems(cur))
			break;
		if (*level == 0) {
			ret = drop_leaf_ref(trans, root, cur);
			BUG_ON(ret);
			break;
		}
		bytenr = btrfs_node_blockptr(cur, path->slots[*level]);
		ptr_gen = btrfs_node_ptr_generation(cur, path->slots[*level]);
		blocksize = btrfs_level_size(root, *level - 1);
		ret = btrfs_lookup_extent_ref(trans, root, bytenr, blocksize,
					      &refs);
		BUG_ON(ret);
		if (refs != 1) {
			parent = path->nodes[*level];
			root_owner = btrfs_header_owner(parent);
			root_gen = btrfs_header_generation(parent);
			path->slots[*level]++;
			ret = btrfs_free_extent(trans, root, bytenr, blocksize,
						parent->start, root_owner,
						root_gen, *level - 1, 1);
			BUG_ON(ret);
			continue;
		}
		next = btrfs_find_tree_block(root, bytenr, blocksize);
		if (!next || !btrfs_buffer_uptodate(next, ptr_gen)) {
			free_extent_buffer(next);
			reada_walk_down(root, cur, path->slots[*level]);
			mutex_unlock(&root->fs_info->fs_mutex);
			next = read_tree_block(root, bytenr, blocksize,
					       ptr_gen);
			mutex_lock(&root->fs_info->fs_mutex);
		}
		WARN_ON(*level <= 0);
		if (path->nodes[*level-1])
			free_extent_buffer(path->nodes[*level-1]);
		path->nodes[*level-1] = next;
		*level = btrfs_header_level(next);
		path->slots[*level] = 0;
	}
out:
	WARN_ON(*level < 0);
	WARN_ON(*level >= BTRFS_MAX_LEVEL);

	if (path->nodes[*level] == root->node) {
		root_owner = root->root_key.objectid;
		parent = path->nodes[*level];
	} else {
		parent = path->nodes[*level + 1];
		root_owner = btrfs_header_owner(parent);
	}

	root_gen = btrfs_header_generation(parent);
	ret = btrfs_free_extent(trans, root, path->nodes[*level]->start,
				path->nodes[*level]->len, parent->start,
				root_owner, root_gen, *level, 1);
	free_extent_buffer(path->nodes[*level]);
	path->nodes[*level] = NULL;
	*level += 1;
	BUG_ON(ret);
	return 0;
}

/*
 * helper for dropping snapshots.  This walks back up the tree in the path
 * to find the first node higher up where we haven't yet gone through
 * all the slots
 */
static int noinline walk_up_tree(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path, int *level)
{
	u64 root_owner;
	u64 root_gen;
	struct btrfs_root_item *root_item = &root->root_item;
	int i;
	int slot;
	int ret;

	for(i = *level; i < BTRFS_MAX_LEVEL - 1 && path->nodes[i]; i++) {
		slot = path->slots[i];
		if (slot < btrfs_header_nritems(path->nodes[i]) - 1) {
			struct extent_buffer *node;
			struct btrfs_disk_key disk_key;
			node = path->nodes[i];
			path->slots[i]++;
			*level = i;
			WARN_ON(*level == 0);
			btrfs_node_key(node, &disk_key, path->slots[i]);
			memcpy(&root_item->drop_progress,
			       &disk_key, sizeof(disk_key));
			root_item->drop_level = i;
			return 0;
		} else {
			struct extent_buffer *parent;
			if (path->nodes[*level] == root->node)
				parent = path->nodes[*level];
			else
				parent = path->nodes[*level + 1];

			root_owner = btrfs_header_owner(parent);
			root_gen = btrfs_header_generation(parent);
			ret = btrfs_free_extent(trans, root,
						path->nodes[*level]->start,
						path->nodes[*level]->len,
						parent->start, root_owner,
						root_gen, *level, 1);
			BUG_ON(ret);
			free_extent_buffer(path->nodes[*level]);
			path->nodes[*level] = NULL;
			*level = i + 1;
		}
	}
	return 1;
}

#endif

int btrfs_free_block_groups(struct btrfs_fs_info *info)
{
	struct btrfs_space_info *sinfo;
	struct btrfs_block_group_cache *cache;
	u64 start;
	u64 end;
	u64 ptr;
	int ret;

	while(1) {
		ret = find_first_extent_bit(&info->block_group_cache, 0,
					    &start, &end, (unsigned int)-1);
		if (ret)
			break;
		ret = get_state_private(&info->block_group_cache, start, &ptr);
		if (!ret) {
			cache = (struct btrfs_block_group_cache *)
					(uintptr_t)ptr;
			if (cache->free_space_ctl) {
				btrfs_remove_free_space_cache(cache);
				kfree(cache->free_space_ctl);
			}
			kfree(cache);
		}
		clear_extent_bits(&info->block_group_cache, start,
				  end, (unsigned int)-1, GFP_NOFS);
	}
	while(1) {
		ret = find_first_extent_bit(&info->free_space_cache, 0,
					    &start, &end, EXTENT_DIRTY);
		if (ret)
			break;
		clear_extent_dirty(&info->free_space_cache, start,
				   end, GFP_NOFS);
	}

	while (!list_empty(&info->space_info)) {
		sinfo = list_entry(info->space_info.next,
				   struct btrfs_space_info, list);
		list_del_init(&sinfo->list);
		kfree(sinfo);
	}
	return 0;
}

static int find_first_block_group(struct btrfs_root *root,
		struct btrfs_path *path, struct btrfs_key *key)
{
	int ret;
	struct btrfs_key found_key;
	struct extent_buffer *leaf;
	int slot;

	ret = btrfs_search_slot(NULL, root, key, path, 0, 0);
	if (ret < 0)
		return ret;
	while(1) {
		slot = path->slots[0];
		leaf = path->nodes[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto error;
			break;
		}
		btrfs_item_key_to_cpu(leaf, &found_key, slot);

		if (found_key.objectid >= key->objectid &&
		    found_key.type == BTRFS_BLOCK_GROUP_ITEM_KEY)
			return 0;
		path->slots[0]++;
	}
	ret = -ENOENT;
error:
	return ret;
}

int btrfs_read_block_groups(struct btrfs_root *root)
{
	struct btrfs_path *path;
	int ret;
	int bit;
	struct btrfs_block_group_cache *cache;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_space_info *space_info;
	struct extent_io_tree *block_group_cache;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct extent_buffer *leaf;

	block_group_cache = &info->block_group_cache;

	root = info->extent_root;
	key.objectid = 0;
	key.offset = 0;
	btrfs_set_key_type(&key, BTRFS_BLOCK_GROUP_ITEM_KEY);
	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	while(1) {
		ret = find_first_block_group(root, path, &key);
		if (ret > 0) {
			ret = 0;
			goto error;
		}
		if (ret != 0) {
			goto error;
		}
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
		cache = kzalloc(sizeof(*cache), GFP_NOFS);
		if (!cache) {
			ret = -ENOMEM;
			break;
		}

		read_extent_buffer(leaf, &cache->item,
				   btrfs_item_ptr_offset(leaf, path->slots[0]),
				   sizeof(cache->item));
		memcpy(&cache->key, &found_key, sizeof(found_key));
		cache->cached = 0;
		cache->pinned = 0;
		key.objectid = found_key.objectid + found_key.offset;
		btrfs_release_path(path);
		cache->flags = btrfs_block_group_flags(&cache->item);
		bit = 0;
		if (cache->flags & BTRFS_BLOCK_GROUP_DATA) {
			bit = BLOCK_GROUP_DATA;
		} else if (cache->flags & BTRFS_BLOCK_GROUP_SYSTEM) {
			bit = BLOCK_GROUP_SYSTEM;
		} else if (cache->flags & BTRFS_BLOCK_GROUP_METADATA) {
			bit = BLOCK_GROUP_METADATA;
		}
		set_avail_alloc_bits(info, cache->flags);
		if (btrfs_chunk_readonly(root, cache->key.objectid))
			cache->ro = 1;

		ret = update_space_info(info, cache->flags, found_key.offset,
					btrfs_block_group_used(&cache->item),
					&space_info);
		BUG_ON(ret);
		cache->space_info = space_info;

		/* use EXTENT_LOCKED to prevent merging */
		set_extent_bits(block_group_cache, found_key.objectid,
				found_key.objectid + found_key.offset - 1,
				bit | EXTENT_LOCKED, GFP_NOFS);
		set_state_private(block_group_cache, found_key.objectid,
				  (unsigned long)cache);
	}
	ret = 0;
error:
	btrfs_free_path(path);
	return ret;
}

struct btrfs_block_group_cache *
btrfs_add_block_group(struct btrfs_fs_info *fs_info, u64 bytes_used, u64 type,
		      u64 chunk_objectid, u64 chunk_offset, u64 size)
{
	int ret;
	int bit = 0;
	struct btrfs_block_group_cache *cache;
	struct extent_io_tree *block_group_cache;

	block_group_cache = &fs_info->block_group_cache;

	cache = kzalloc(sizeof(*cache), GFP_NOFS);
	BUG_ON(!cache);
	cache->key.objectid = chunk_offset;
	cache->key.offset = size;

	btrfs_set_key_type(&cache->key, BTRFS_BLOCK_GROUP_ITEM_KEY);
	btrfs_set_block_group_used(&cache->item, bytes_used);
	btrfs_set_block_group_chunk_objectid(&cache->item, chunk_objectid);
	cache->flags = type;
	btrfs_set_block_group_flags(&cache->item, type);

	ret = update_space_info(fs_info, cache->flags, size, bytes_used,
				&cache->space_info);
	BUG_ON(ret);

	bit = block_group_state_bits(type);
	ret = set_extent_bits(block_group_cache, chunk_offset,
			      chunk_offset + size - 1,
			      bit | EXTENT_LOCKED, GFP_NOFS);
	BUG_ON(ret);

	ret = set_state_private(block_group_cache, chunk_offset,
				(unsigned long)cache);
	BUG_ON(ret);
	set_avail_alloc_bits(fs_info, type);

	return cache;
}

int btrfs_make_block_group(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root, u64 bytes_used,
			   u64 type, u64 chunk_objectid, u64 chunk_offset,
			   u64 size)
{
	int ret;
	struct btrfs_root *extent_root;
	struct btrfs_block_group_cache *cache;

	cache = btrfs_add_block_group(root->fs_info, bytes_used, type,
				      chunk_objectid, chunk_offset, size);
	extent_root = root->fs_info->extent_root;
	ret = btrfs_insert_item(trans, extent_root, &cache->key, &cache->item,
				sizeof(cache->item));
	BUG_ON(ret);

	ret = finish_current_insert(trans, extent_root);
	BUG_ON(ret);
	ret = del_pending_extents(trans, extent_root);
	BUG_ON(ret);

	return 0;
}

/*
 * This is for converter use only.
 *
 * In that case, we don't know where are free blocks located.
 * Therefore all block group cache entries must be setup properly
 * before doing any block allocation.
 */
int btrfs_make_block_groups(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root)
{
	u64 total_bytes;
	u64 cur_start;
	u64 group_type;
	u64 group_size;
	u64 group_align;
	u64 total_data = 0;
	u64 total_metadata = 0;
	u64 chunk_objectid;
	int ret;
	int bit;
	struct btrfs_root *extent_root;
	struct btrfs_block_group_cache *cache;
	struct extent_io_tree *block_group_cache;

	extent_root = root->fs_info->extent_root;
	block_group_cache = &root->fs_info->block_group_cache;
	chunk_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	total_bytes = btrfs_super_total_bytes(root->fs_info->super_copy);
	group_align = 64 * root->sectorsize;

	cur_start = 0;
	while (cur_start < total_bytes) {
		group_size = total_bytes / 12;
		group_size = min_t(u64, group_size, total_bytes - cur_start);
		if (cur_start == 0) {
			bit = BLOCK_GROUP_SYSTEM;
			group_type = BTRFS_BLOCK_GROUP_SYSTEM;
			group_size /= 4;
			group_size &= ~(group_align - 1);
			group_size = max_t(u64, group_size, 8 * 1024 * 1024);
			group_size = min_t(u64, group_size, 32 * 1024 * 1024);
		} else {
			group_size &= ~(group_align - 1);
			if (total_data >= total_metadata * 2) {
				group_type = BTRFS_BLOCK_GROUP_METADATA;
				group_size = min_t(u64, group_size,
						   1ULL * 1024 * 1024 * 1024);
				total_metadata += group_size;
			} else {
				group_type = BTRFS_BLOCK_GROUP_DATA;
				group_size = min_t(u64, group_size,
						   5ULL * 1024 * 1024 * 1024);
				total_data += group_size;
			}
			if ((total_bytes - cur_start) * 4 < group_size * 5)
				group_size = total_bytes - cur_start;
		}

		cache = kzalloc(sizeof(*cache), GFP_NOFS);
		BUG_ON(!cache);

		cache->key.objectid = cur_start;
		cache->key.offset = group_size;
		btrfs_set_key_type(&cache->key, BTRFS_BLOCK_GROUP_ITEM_KEY);

		btrfs_set_block_group_used(&cache->item, 0);
		btrfs_set_block_group_chunk_objectid(&cache->item,
						     chunk_objectid);
		btrfs_set_block_group_flags(&cache->item, group_type);

		cache->flags = group_type;

		ret = update_space_info(root->fs_info, group_type, group_size,
					0, &cache->space_info);
		BUG_ON(ret);
		set_avail_alloc_bits(extent_root->fs_info, group_type);

		set_extent_bits(block_group_cache, cur_start,
				cur_start + group_size - 1,
				bit | EXTENT_LOCKED, GFP_NOFS);
		set_state_private(block_group_cache, cur_start,
				  (unsigned long)cache);
		cur_start += group_size;
	}
	/* then insert all the items */
	cur_start = 0;
	while(cur_start < total_bytes) {
		cache = btrfs_lookup_block_group(root->fs_info, cur_start);
		BUG_ON(!cache);

		ret = btrfs_insert_item(trans, extent_root, &cache->key, &cache->item,
					sizeof(cache->item));
		BUG_ON(ret);

		finish_current_insert(trans, extent_root);
		ret = del_pending_extents(trans, extent_root);
		BUG_ON(ret);

		cur_start = cache->key.objectid + cache->key.offset;
	}
	return 0;
}

int btrfs_update_block_group(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     u64 bytenr, u64 num_bytes, int alloc,
			     int mark_free)
{
	return update_block_group(trans, root, bytenr, num_bytes,
				  alloc, mark_free);
}

/*
 * Fixup block accounting. The initial block accounting created by
 * make_block_groups isn't accuracy in this case.
 */
int btrfs_fix_block_accounting(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root)
{
	int ret;
	int slot;
	u64 start = 0;
	u64 bytes_used = 0;
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_block_group_cache *cache;
	struct btrfs_fs_info *fs_info = root->fs_info;

	root = root->fs_info->extent_root;

	while(extent_root_pending_ops(fs_info)) {
		ret = finish_current_insert(trans, root);
		if (ret)
			return ret;
		ret = del_pending_extents(trans, root);
		if (ret)
			return ret;
	}

	while(1) {
		cache = btrfs_lookup_first_block_group(fs_info, start);
		if (!cache)
			break;
		start = cache->key.objectid + cache->key.offset;
		btrfs_set_block_group_used(&cache->item, 0);
		cache->space_info->bytes_used = 0;
		set_extent_bits(&root->fs_info->block_group_cache,
				cache->key.objectid,
				cache->key.objectid + cache->key.offset -1,
				BLOCK_GROUP_DIRTY, GFP_NOFS);
	}

	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	ret = btrfs_search_slot(trans, root->fs_info->extent_root,
				&key, &path, 0, 0);
	if (ret < 0)
		return ret;
	while(1) {
		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0)
				return ret;
			if (ret > 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.type == BTRFS_EXTENT_ITEM_KEY) {
			bytes_used += key.offset;
			ret = btrfs_update_block_group(trans, root,
				  key.objectid, key.offset, 1, 0);
			BUG_ON(ret);
		} else if (key.type == BTRFS_METADATA_ITEM_KEY) {
			bytes_used += root->leafsize;
			ret = btrfs_update_block_group(trans, root,
				  key.objectid, root->leafsize, 1, 0);
			BUG_ON(ret);
		}
		path.slots[0]++;
	}
	btrfs_set_super_bytes_used(root->fs_info->super_copy, bytes_used);
	btrfs_release_path(&path);
	return 0;
}

/*
 * Record a file extent. Do all the required works, such as inserting
 * file extent item, inserting extent item and backref item into extent
 * tree and updating block accounting.
 */
int btrfs_record_file_extent(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *inode,
			      u64 file_pos, u64 disk_bytenr,
			      u64 num_bytes)
{
	int ret;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key ins_key;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	u64 nbytes;

	if (disk_bytenr == 0) {
		ret = btrfs_insert_file_extent(trans, root, objectid,
						file_pos, disk_bytenr,
						num_bytes, num_bytes);
		return ret;
	}

	btrfs_init_path(&path);

	ins_key.objectid = objectid;
	ins_key.offset = file_pos;
	btrfs_set_key_type(&ins_key, BTRFS_EXTENT_DATA_KEY);
	ret = btrfs_insert_empty_item(trans, root, &path, &ins_key,
				      sizeof(*fi));
	if (ret)
		goto fail;
	leaf = path.nodes[0];
	fi = btrfs_item_ptr(leaf, path.slots[0],
			    struct btrfs_file_extent_item);
	btrfs_set_file_extent_generation(leaf, fi, trans->transid);
	btrfs_set_file_extent_type(leaf, fi, BTRFS_FILE_EXTENT_REG);
	btrfs_set_file_extent_disk_bytenr(leaf, fi, disk_bytenr);
	btrfs_set_file_extent_disk_num_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_offset(leaf, fi, 0);
	btrfs_set_file_extent_num_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_ram_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_compression(leaf, fi, 0);
	btrfs_set_file_extent_encryption(leaf, fi, 0);
	btrfs_set_file_extent_other_encoding(leaf, fi, 0);
	btrfs_mark_buffer_dirty(leaf);

	nbytes = btrfs_stack_inode_nbytes(inode) + num_bytes;
	btrfs_set_stack_inode_nbytes(inode, nbytes);

	btrfs_release_path(&path);

	ins_key.objectid = disk_bytenr;
	ins_key.offset = num_bytes;
	ins_key.type = BTRFS_EXTENT_ITEM_KEY;

	ret = btrfs_insert_empty_item(trans, extent_root, &path,
				      &ins_key, sizeof(*ei));
	if (ret == 0) {
		leaf = path.nodes[0];
		ei = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_extent_item);

		btrfs_set_extent_refs(leaf, ei, 0);
		btrfs_set_extent_generation(leaf, ei, 0);
		btrfs_set_extent_flags(leaf, ei, BTRFS_EXTENT_FLAG_DATA);

		btrfs_mark_buffer_dirty(leaf);

		ret = btrfs_update_block_group(trans, root, disk_bytenr,
					       num_bytes, 1, 0);
		if (ret)
			goto fail;
	} else if (ret != -EEXIST) {
		goto fail;
	}
	btrfs_extent_post_op(trans, extent_root);

	ret = btrfs_inc_extent_ref(trans, root, disk_bytenr, num_bytes, 0,
				   root->root_key.objectid,
				   objectid, file_pos);
	if (ret)
		goto fail;
	ret = 0;
fail:
	btrfs_release_path(&path);
	return ret;
}
