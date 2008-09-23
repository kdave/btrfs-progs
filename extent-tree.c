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
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "crc32c.h"
#include "volumes.h"

#define BLOCK_GROUP_DATA     EXTENT_WRITEBACK
#define BLOCK_GROUP_METADATA EXTENT_UPTODATE
#define BLOCK_GROUP_SYSTEM   EXTENT_NEW

#define BLOCK_GROUP_DIRTY EXTENT_DIRTY

#define PENDING_EXTENT_INSERT 0
#define PENDING_EXTENT_DELETE 1
#define PENDING_BACKREF_UPDATE 2

struct pending_extent_op {
	int type;
	u64 bytenr;
	u64 num_bytes;
	u64 parent;
	u64 orig_parent;
	u64 generation;
	u64 orig_generation;
	int level;
};

static int finish_current_insert(struct btrfs_trans_handle *trans, struct
				 btrfs_root *extent_root);
static int del_pending_extents(struct btrfs_trans_handle *trans, struct
			       btrfs_root *extent_root);

void maybe_lock_mutex(struct btrfs_root *root)
{
}

void maybe_unlock_mutex(struct btrfs_root *root)
{
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
	u64 last = 0;
	u64 hole_size;
	u64 first_free;
	int found = 0;

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
	first_free = block_group->key.objectid;
	key.objectid = block_group->key.objectid;
	key.offset = 0;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		return ret;
	ret = btrfs_previous_item(root, path, 0, BTRFS_EXTENT_ITEM_KEY);
	if (ret < 0)
		return ret;
	if (ret == 0) {
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid + key.offset > first_free)
			first_free = key.objectid + key.offset;
	}
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

		if (btrfs_key_type(&key) == BTRFS_EXTENT_ITEM_KEY) {
			if (!found) {
				last = first_free;
				found = 1;
			}
			if (key.objectid > last) {
				hole_size = key.objectid - last;
				set_extent_dirty(free_space_cache, last,
						 last + hole_size - 1,
						 GFP_NOFS);
			}
			last = key.objectid + key.offset;
		}
next:
		path->slots[0]++;
	}

	if (!found)
		last = first_free;
	if (block_group->key.objectid +
	    block_group->key.offset > last) {
		hole_size = block_group->key.objectid +
			block_group->key.offset - last;
		set_extent_dirty(free_space_cache, last,
				 last + hole_size - 1, GFP_NOFS);
	}
	block_group->cached = 1;
err:
	btrfs_free_path(path);
	return 0;
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
	u64 last;
	u64 start = 0;
	u64 end = 0;
	u64 cache_miss = 0;
	u64 search_start = *start_ret;
	int wrapped = 0;

	if (!cache) {
		goto out;
	}
again:
	ret = cache_block_group(root, cache);
	if (ret)
		goto out;

	last = max(search_start, cache->key.objectid);
	if (!block_group_bits(cache, data)) {
		goto new_group;
	}

	while(1) {
		ret = find_first_extent_bit(&root->fs_info->free_space_cache,
					    last, &start, &end, EXTENT_DIRTY);
		if (ret) {
			if (!cache_miss)
				cache_miss = last;
			goto new_group;
		}

		start = max(last, start);
		last = end + 1;
		if (last - start < num) {
			if (last == cache->key.objectid + cache->key.offset)
				cache_miss = start;
			continue;
		}
		if (start + num > cache->key.objectid + cache->key.offset)
			goto new_group;
		*start_ret = start;
		return 0;
	}
out:
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
	cache = btrfs_lookup_block_group(root->fs_info, last);
	if (!cache) {
no_cache:
		if (!wrapped) {
			wrapped = 1;
			last = search_start;
			goto wrapped;
		}
		goto out;
	}
	if (cache_miss && !cache->cached) {
		cache_block_group(root, cache);
		last = cache_miss;
		cache = btrfs_lookup_block_group(root->fs_info, last);
	}
	cache = btrfs_find_block_group(root, cache, last, data, 0);
	if (!cache)
		goto no_cache;
	*cache_ret = cache;
	cache_miss = 0;
	goto again;
}

static u64 div_factor(u64 num, int factor)
{
	if (factor == 10)
		return num;
	num *= factor;
	num /= 10;
	return num;
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

struct btrfs_block_group_cache *btrfs_find_block_group(struct btrfs_root *root,
						 struct btrfs_block_group_cache
						 *hint, u64 search_start,
						 int data, int owner)
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
		if (shint && block_group_bits(shint, data)) {
			used = btrfs_block_group_used(&shint->item);
			if (used + shint->pinned <
			    div_factor(shint->key.offset, factor)) {
				return shint;
			}
		}
	}
	if (hint && block_group_bits(hint, data)) {
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

		if (block_group_bits(cache, data)) {
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
 * File extents can be referenced by:
 *
 * - multiple snapshots, subvolumes, or different generations in one subvol
 * - different files inside a single subvolume
 * - different offsets inside a file (bookend extents in file.c)
 *
 * The extent ref structure has fields for:
 *
 * - Objectid of the subvolume root
 * - Generation number of the tree holding the reference
 * - objectid of the file holding the reference
 * - offset in the file corresponding to the key holding the reference
 * - number of references holding by parent node (alway 1 for tree blocks)
 *
 * Btree leaf may hold multiple references to a file extent. In most cases,
 * these references are from same file and the corresponding offsets inside
 * the file are close together. So inode objectid and offset in file are
 * just hints, they provide hints about where in the btree the references
 * can be found and when we can stop searching.
 *
 * When a file extent is allocated the fields are filled in:
 *     (root_key.objectid, trans->transid, inode objectid, offset in file, 1)
 *
 * When a leaf is cow'd new references are added for every file extent found
 * in the leaf.  It looks similar to the create case, but trans->transid will
 * be different when the block is cow'd.
 *
 *     (root_key.objectid, trans->transid, inode objectid, offset in file,
 *      number of references in the leaf)
 *
 * Because inode objectid and offset in file are just hints, they are not
 * used when backrefs are deleted. When a file extent is removed either
 * during snapshot deletion or file truncation, we find the corresponding
 * back back reference and check the following fields.
 *
 *     (btrfs_header_owner(leaf), btrfs_header_generation(leaf))
 *
 * Btree extents can be referenced by:
 *
 * - Different subvolumes
 * - Different generations of the same subvolume
 *
 * When a tree block is created, back references are inserted:
 *
 * (root->root_key.objectid, trans->transid, level, 0, 1)
 *
 * When a tree block is cow'd, new back references are added for all the
 * blocks it points to. If the tree block isn't in reference counted root,
 * the old back references are removed. These new back references are of
 * the form (trans->transid will have increased since creation):
 *
 * (root->root_key.objectid, trans->transid, level, 0, 1)
 *
 * When a backref is in deleting, the following fields are checked:
 *
 * if backref was for a tree root:
 *     (btrfs_header_owner(itself), btrfs_header_generation(itself))
 * else
 *     (btrfs_header_owner(parent), btrfs_header_generation(parent))
 *
 * Back Reference Key composing:
 *
 * The key objectid corresponds to the first byte in the extent, the key
 * type is set to BTRFS_EXTENT_REF_KEY, and the key offset is the first
 * byte of parent extent. If a extent is tree root, the key offset is set
 * to the key objectid.
 */

static int noinline lookup_extent_backref(struct btrfs_trans_handle *trans,
					  struct btrfs_root *root,
					  struct btrfs_path *path, u64 bytenr,
					  u64 parent, u64 ref_root,
					  u64 ref_generation, int del)
{
	struct btrfs_key key;
	struct btrfs_extent_ref *ref;
	struct extent_buffer *leaf;
	int ret;

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_REF_KEY;
	key.offset = parent;

	ret = btrfs_search_slot(trans, root, &key, path, del ? -1 : 0, 1);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	leaf = path->nodes[0];
	ref = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_ref);
	if (btrfs_ref_root(leaf, ref) != ref_root ||
	    btrfs_ref_generation(leaf, ref) != ref_generation) {
		ret = -EIO;
		WARN_ON(1);
		goto out;
	}
	ret = 0;
out:
	return ret;
}

static int noinline insert_extent_backref(struct btrfs_trans_handle *trans,
					  struct btrfs_root *root,
					  struct btrfs_path *path,
					  u64 bytenr, u64 parent,
					  u64 ref_root, u64 ref_generation,
					  u64 owner_objectid, u64 owner_offset)
{
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_extent_ref *ref;
	u32 num_refs;
	int ret;

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_REF_KEY;
	key.offset = parent;

	ret = btrfs_insert_empty_item(trans, root, path, &key, sizeof(*ref));
	if (ret == 0) {
		leaf = path->nodes[0];
		ref = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_ref);
		btrfs_set_ref_root(leaf, ref, ref_root);
		btrfs_set_ref_generation(leaf, ref, ref_generation);
		btrfs_set_ref_objectid(leaf, ref, owner_objectid);
		btrfs_set_ref_offset(leaf, ref, owner_offset);
		btrfs_set_ref_num_refs(leaf, ref, 1);
	} else if (ret == -EEXIST) {
		u64 existing_owner;
		BUG_ON(owner_objectid < BTRFS_FIRST_FREE_OBJECTID);
		leaf = path->nodes[0];
		ref = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_ref);
		if (btrfs_ref_root(leaf, ref) != ref_root ||
		    btrfs_ref_generation(leaf, ref) != ref_generation) {
			ret = -EIO;
			WARN_ON(1);
			goto out;
		}

		num_refs = btrfs_ref_num_refs(leaf, ref);
		BUG_ON(num_refs == 0);
		btrfs_set_ref_num_refs(leaf, ref, num_refs + 1);

		existing_owner = btrfs_ref_objectid(leaf, ref);
		if (existing_owner == owner_objectid &&
		    btrfs_ref_offset(leaf, ref) > owner_offset) {
			btrfs_set_ref_offset(leaf, ref, owner_offset);
		} else if (existing_owner != owner_objectid &&
			   existing_owner != BTRFS_MULTIPLE_OBJECTIDS) {
			btrfs_set_ref_objectid(leaf, ref,
					BTRFS_MULTIPLE_OBJECTIDS);
			btrfs_set_ref_offset(leaf, ref, 0);
		}
		ret = 0;
	} else {
		goto out;
	}
	btrfs_mark_buffer_dirty(path->nodes[0]);
out:
	btrfs_release_path(root, path);
	return ret;
}

static int noinline remove_extent_backref(struct btrfs_trans_handle *trans,
					  struct btrfs_root *root,
					  struct btrfs_path *path)
{
	struct extent_buffer *leaf;
	struct btrfs_extent_ref *ref;
	u32 num_refs;
	int ret = 0;

	leaf = path->nodes[0];
	ref = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_extent_ref);
	num_refs = btrfs_ref_num_refs(leaf, ref);
	BUG_ON(num_refs == 0);
	num_refs -= 1;
	if (num_refs == 0) {
		ret = btrfs_del_item(trans, root, path);
	} else {
		btrfs_set_ref_num_refs(leaf, ref, num_refs);
		btrfs_mark_buffer_dirty(leaf);
	}
	btrfs_release_path(root, path);
	return ret;
}

static int __btrfs_update_extent_ref(struct btrfs_trans_handle *trans,
				     struct btrfs_root *root, u64 bytenr,
				     u64 orig_parent, u64 parent,
				     u64 orig_root, u64 ref_root,
				     u64 orig_generation, u64 ref_generation,
				     u64 owner_objectid, u64 owner_offset)
{
	int ret;
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	struct btrfs_path *path;

	if (root == root->fs_info->extent_root) {
		struct pending_extent_op *extent_op;
		u64 num_bytes;

		BUG_ON(owner_objectid >= BTRFS_MAX_LEVEL);
		num_bytes = btrfs_level_size(root, (int)owner_objectid);
		if (test_range_bit(&root->fs_info->extent_ins, bytenr,
				bytenr + num_bytes - 1, EXTENT_LOCKED, 0)) {
			u64 priv;
			ret = get_state_private(&root->fs_info->extent_ins,
						bytenr, &priv);
			BUG_ON(ret);
			extent_op = (struct pending_extent_op *)
							(unsigned long)priv;
			BUG_ON(extent_op->parent != orig_parent);
			BUG_ON(extent_op->generation != orig_generation);
			extent_op->parent = parent;
			extent_op->generation = ref_generation;
		} else {
			extent_op = kmalloc(sizeof(*extent_op), GFP_NOFS);
			BUG_ON(!extent_op);

			extent_op->type = PENDING_BACKREF_UPDATE;
			extent_op->bytenr = bytenr;
			extent_op->num_bytes = num_bytes;
			extent_op->parent = parent;
			extent_op->orig_parent = orig_parent;
			extent_op->generation = ref_generation;
			extent_op->orig_generation = orig_generation;
			extent_op->level = (int)owner_objectid;

			set_extent_bits(&root->fs_info->extent_ins,
					bytenr, bytenr + num_bytes - 1,
					EXTENT_LOCKED, GFP_NOFS);
			set_state_private(&root->fs_info->extent_ins,
					  bytenr, (unsigned long)extent_op);
		}
		return 0;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	ret = lookup_extent_backref(trans, extent_root, path,
				    bytenr, orig_parent, orig_root,
				    orig_generation, 1);
	if (ret)
		goto out;
	ret = remove_extent_backref(trans, extent_root, path);
	if (ret)
		goto out;
	ret = insert_extent_backref(trans, extent_root, path, bytenr,
				    parent, ref_root, ref_generation,
				    owner_objectid, owner_offset);
	BUG_ON(ret);
	finish_current_insert(trans, extent_root);
	del_pending_extents(trans, extent_root);
out:
	btrfs_free_path(path);
	return ret;
}

int btrfs_update_extent_ref(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root, u64 bytenr,
			    u64 orig_parent, u64 parent,
			    u64 ref_root, u64 ref_generation,
			    u64 owner_objectid, u64 owner_offset)
{
	int ret;
	if (ref_root == BTRFS_TREE_LOG_OBJECTID &&
	    owner_objectid < BTRFS_FIRST_FREE_OBJECTID)
		return 0;
	maybe_lock_mutex(root);
	ret = __btrfs_update_extent_ref(trans, root, bytenr, orig_parent,
					parent, ref_root, ref_root,
					ref_generation, ref_generation,
					owner_objectid, owner_offset);
	maybe_unlock_mutex(root);
	return ret;
}

static int __btrfs_inc_extent_ref(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root, u64 bytenr,
				  u64 orig_parent, u64 parent,
				  u64 orig_root, u64 ref_root,
				  u64 orig_generation, u64 ref_generation,
				  u64 owner_objectid, u64 owner_offset)
{
	struct btrfs_path *path;
	int ret;
	struct btrfs_key key;
	struct extent_buffer *l;
	struct btrfs_extent_item *item;
	u32 refs;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->reada = 1;
	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(trans, root->fs_info->extent_root, &key, path,
				0, 1);
	if (ret < 0)
		return ret;
	BUG_ON(ret == 0 || path->slots[0] == 0);

	path->slots[0]--;
	l = path->nodes[0];

	btrfs_item_key_to_cpu(l, &key, path->slots[0]);
	BUG_ON(key.objectid != bytenr);
	BUG_ON(key.type != BTRFS_EXTENT_ITEM_KEY);

	item = btrfs_item_ptr(l, path->slots[0], struct btrfs_extent_item);
	refs = btrfs_extent_refs(l, item);
	btrfs_set_extent_refs(l, item, refs + 1);
	btrfs_mark_buffer_dirty(path->nodes[0]);

	btrfs_release_path(root->fs_info->extent_root, path);

	path->reada = 1;
	ret = insert_extent_backref(trans, root->fs_info->extent_root,
				    path, bytenr, parent,
				    ref_root, ref_generation,
				    owner_objectid, owner_offset);
	BUG_ON(ret);
	finish_current_insert(trans, root->fs_info->extent_root);
	del_pending_extents(trans, root->fs_info->extent_root);

	btrfs_free_path(path);
	return 0;
}

int btrfs_inc_extent_ref(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 u64 bytenr, u64 num_bytes, u64 parent,
			 u64 ref_root, u64 ref_generation,
			 u64 owner_objectid, u64 owner_offset)
{
	int ret;
	if (ref_root == BTRFS_TREE_LOG_OBJECTID &&
	    owner_objectid < BTRFS_FIRST_FREE_OBJECTID)
		return 0;
	maybe_lock_mutex(root);
	ret = __btrfs_inc_extent_ref(trans, root, bytenr, 0, parent,
				     0, ref_root, 0, ref_generation,
				     owner_objectid, owner_offset);
	maybe_unlock_mutex(root);
	return ret;
}

int btrfs_extent_post_op(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root)
{
	finish_current_insert(trans, root->fs_info->extent_root);
	del_pending_extents(trans, root->fs_info->extent_root);
	return 0;
}

int lookup_extent_ref(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, u64 bytenr,
		      u64 num_bytes, u32 *refs)
{
	struct btrfs_path *path;
	int ret;
	struct btrfs_key key;
	struct extent_buffer *l;
	struct btrfs_extent_item *item;

	WARN_ON(num_bytes < root->sectorsize);
	path = btrfs_alloc_path();
	path->reada = 1;
	key.objectid = bytenr;
	key.offset = num_bytes;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	ret = btrfs_search_slot(trans, root->fs_info->extent_root, &key, path,
				0, 0);
	if (ret < 0)
		goto out;
	if (ret != 0) {
		btrfs_print_leaf(root, path->nodes[0]);
		printk("failed to find block number %Lu\n", bytenr);
		BUG();
	}
	l = path->nodes[0];
	item = btrfs_item_ptr(l, path->slots[0], struct btrfs_extent_item);
	*refs = btrfs_extent_refs(l, item);
out:
	btrfs_free_path(path);
	return 0;
}

int btrfs_inc_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct extent_buffer *orig_buf, struct extent_buffer *buf,
		  u32 *nr_extents)
{
	u64 bytenr;
	u64 ref_root;
	u64 orig_root;
	u64 ref_generation;
	u64 orig_generation;
	u32 nritems;
	u32 nr_file_extents = 0;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	int i;
	int level;
	int ret = 0;
	int faili = 0;
	int (*process_func)(struct btrfs_trans_handle *, struct btrfs_root *,
			    u64, u64, u64, u64, u64, u64, u64, u64, u64);

	ref_root = btrfs_header_owner(buf);
	ref_generation = btrfs_header_generation(buf);
	orig_root = btrfs_header_owner(orig_buf);
	orig_generation = btrfs_header_generation(orig_buf);

	nritems = btrfs_header_nritems(buf);
	level = btrfs_header_level(buf);

	if (root->ref_cows) {
		process_func = __btrfs_inc_extent_ref;
	} else {
		if (level == 0 &&
		    root->root_key.objectid != BTRFS_TREE_LOG_OBJECTID)
			goto out;
		process_func = __btrfs_update_extent_ref;
	}

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

			nr_file_extents++;

			maybe_lock_mutex(root);
			ret = process_func(trans, root, bytenr,
					   orig_buf->start, buf->start,
					   orig_root, ref_root,
					   orig_generation, ref_generation,
					   key.objectid, key.offset);
			maybe_unlock_mutex(root);

			if (ret) {
				faili = i;
				WARN_ON(1);
				goto fail;
			}
		} else {
			bytenr = btrfs_node_blockptr(buf, i);
			maybe_lock_mutex(root);
			ret = process_func(trans, root, bytenr,
					   orig_buf->start, buf->start,
					   orig_root, ref_root,
					   orig_generation, ref_generation,
					   level - 1, 0);
			maybe_unlock_mutex(root);
			if (ret) {
				faili = i;
				WARN_ON(1);
				goto fail;
			}
		}
	}
out:
	if (nr_extents) {
		if (level == 0)
			*nr_extents = nr_file_extents;
		else
			*nr_extents = nritems;
	}
	return 0;
fail:
	WARN_ON(1);
#if 0
	for (i =0; i < faili; i++) {
		if (level == 0) {
			u64 disk_bytenr;
			btrfs_item_key_to_cpu(buf, &key, i);
			if (btrfs_key_type(&key) != BTRFS_EXTENT_DATA_KEY)
				continue;
			fi = btrfs_item_ptr(buf, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(buf, fi) ==
			    BTRFS_FILE_EXTENT_INLINE)
				continue;
			disk_bytenr = btrfs_file_extent_disk_bytenr(buf, fi);
			if (disk_bytenr == 0)
				continue;
			err = btrfs_free_extent(trans, root, disk_bytenr,
				    btrfs_file_extent_disk_num_bytes(buf,
								      fi), 0);
			BUG_ON(err);
		} else {
			bytenr = btrfs_node_blockptr(buf, i);
			err = btrfs_free_extent(trans, root, bytenr,
					btrfs_level_size(root, level - 1), 0);
			BUG_ON(err);
		}
	}
#endif
	return ret;
}

int btrfs_update_ref(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root, struct extent_buffer *orig_buf,
		     struct extent_buffer *buf, int start_slot, int nr)

{
	u64 bytenr;
	u64 ref_root;
	u64 orig_root;
	u64 ref_generation;
	u64 orig_generation;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	int i;
	int ret;
	int slot;
	int level;

	BUG_ON(start_slot < 0);
	BUG_ON(start_slot + nr > btrfs_header_nritems(buf));

	ref_root = btrfs_header_owner(buf);
	ref_generation = btrfs_header_generation(buf);
	orig_root = btrfs_header_owner(orig_buf);
	orig_generation = btrfs_header_generation(orig_buf);
	level = btrfs_header_level(buf);

	if (!root->ref_cows) {
		if (level == 0 &&
		    root->root_key.objectid != BTRFS_TREE_LOG_OBJECTID)
			return 0;
	}

	for (i = 0, slot = start_slot; i < nr; i++, slot++) {
		cond_resched();
		if (level == 0) {
			btrfs_item_key_to_cpu(buf, &key, slot);
			if (btrfs_key_type(&key) != BTRFS_EXTENT_DATA_KEY)
				continue;
			fi = btrfs_item_ptr(buf, slot,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(buf, fi) ==
			    BTRFS_FILE_EXTENT_INLINE)
				continue;
			bytenr = btrfs_file_extent_disk_bytenr(buf, fi);
			if (bytenr == 0)
				continue;

			maybe_lock_mutex(root);
			ret = __btrfs_update_extent_ref(trans, root, bytenr,
					    orig_buf->start, buf->start,
					    orig_root, ref_root,
					    orig_generation, ref_generation,
					    key.objectid, key.offset);
			maybe_unlock_mutex(root);
			if (ret)
				goto fail;
		} else {
			bytenr = btrfs_node_blockptr(buf, slot);
			maybe_lock_mutex(root);
			ret = __btrfs_update_extent_ref(trans, root, bytenr,
					    orig_buf->start, buf->start,
					    orig_root, ref_root,
					    orig_generation, ref_generation,
					    level - 1, 0);
			maybe_unlock_mutex(root);
			if (ret)
				goto fail;
		}
	}
	return 0;
fail:
	WARN_ON(1);
	return -1;
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
	btrfs_release_path(extent_root, path);
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
	int err = 0;
	int werr = 0;
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
		if (ret)
			break;

		last = end + 1;
		ret = get_state_private(block_group_cache, start, &ptr);
		if (ret)
			break;
		cache = (struct btrfs_block_group_cache *)(unsigned long)ptr;
		err = write_one_cache_group(trans, root,
					    path, cache);
		/*
		 * if we fail to write the cache group, we want
		 * to keep it marked dirty in hopes that a later
		 * write will work
		 */
		if (err) {
			werr = err;
			continue;
		}
		clear_extent_bits(block_group_cache, start, end,
				  BLOCK_GROUP_DIRTY, GFP_NOFS);
	}
	btrfs_free_path(path);
	return werr;
}

static struct btrfs_space_info *__find_space_info(struct btrfs_fs_info *info,
						  u64 flags)
{
	struct list_head *head = &info->space_info;
	struct list_head *cur;
	struct btrfs_space_info *found;
	list_for_each(cur, head) {
		found = list_entry(cur, struct btrfs_space_info, list);
		if (found->flags == flags)
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
		WARN_ON(found->total_bytes < found->bytes_used);
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

	ret = btrfs_alloc_chunk(trans, extent_root, &start, &num_bytes, flags);
	if (ret == -ENOSPC) {
		space_info->full = 1;
		return 0;
	}

	BUG_ON(ret);

	ret = btrfs_make_block_group(trans, extent_root, 0, flags,
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
		bytenr += len;
		num -= len;
	}
	return 0;
}

int btrfs_copy_pinned(struct btrfs_root *root, struct extent_io_tree *copy)
{
	u64 last = 0;
	u64 start;
	u64 end;
	struct extent_io_tree *pinned_extents = &root->fs_info->pinned_extents;
	int ret;

	while(1) {
		ret = find_first_extent_bit(pinned_extents, last,
					    &start, &end, EXTENT_DIRTY);
		if (ret)
			break;
		set_extent_dirty(copy, start, end, GFP_NOFS);
		last = end + 1;
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

static int finish_current_insert(struct btrfs_trans_handle *trans,
				 struct btrfs_root *extent_root)
{
	u64 start;
	u64 end;
	u64 priv;
	struct btrfs_fs_info *info = extent_root->fs_info;
	struct btrfs_path *path;
	struct btrfs_extent_ref *ref;
	struct pending_extent_op *extent_op;
	struct btrfs_key key;
	struct btrfs_extent_item extent_item;
	int ret;
	int err = 0;

	btrfs_set_stack_extent_refs(&extent_item, 1);
	path = btrfs_alloc_path();

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
			key.offset = end + 1 - start;
			key.type = BTRFS_EXTENT_ITEM_KEY;
			err = btrfs_insert_item(trans, extent_root, &key,
					&extent_item, sizeof(extent_item));
			BUG_ON(err);

			clear_extent_bits(&info->extent_ins, start, end,
					  EXTENT_LOCKED, GFP_NOFS);

			err = insert_extent_backref(trans, extent_root, path,
						start, extent_op->parent,
						extent_root->root_key.objectid,
						extent_op->generation,
						extent_op->level, 0);
			BUG_ON(err);
		} else if (extent_op->type == PENDING_BACKREF_UPDATE) {
			err = lookup_extent_backref(trans, extent_root, path,
						start, extent_op->orig_parent,
						extent_root->root_key.objectid,
						extent_op->orig_generation, 0);
			BUG_ON(err);

			clear_extent_bits(&info->extent_ins, start, end,
					  EXTENT_LOCKED, GFP_NOFS);

			key.objectid = start;
			key.offset = extent_op->parent;
			key.type = BTRFS_EXTENT_REF_KEY;
			err = btrfs_set_item_key_safe(trans, extent_root, path,
						      &key);
			BUG_ON(err);
			ref = btrfs_item_ptr(path->nodes[0], path->slots[0],
					     struct btrfs_extent_ref);
			btrfs_set_ref_generation(path->nodes[0], ref,
						 extent_op->generation);
			btrfs_mark_buffer_dirty(path->nodes[0]);
			btrfs_release_path(extent_root, path);
		} else {
			BUG_ON(1);
		}
		kfree(extent_op);
	}
	btrfs_free_path(path);
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
		    header_owner != BTRFS_TREE_RELOC_OBJECTID &&
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

/*
 * remove an extent from the root, returns 0 on success
 */
static int __free_extent(struct btrfs_trans_handle *trans, struct btrfs_root
			 *root, u64 bytenr, u64 num_bytes, u64 parent,
			 u64 root_objectid, u64 ref_generation,
			 u64 owner_objectid, u64 owner_offset, int pin,
			 int mark_free)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_extent_ops *ops = info->extent_ops;
	struct btrfs_root *extent_root = info->extent_root;
	struct extent_buffer *leaf;
	int ret;
	int extent_slot = 0;
	int found_extent = 0;
	int num_to_del = 1;
	struct btrfs_extent_item *ei;
	u32 refs;

	key.objectid = bytenr;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	key.offset = num_bytes;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = lookup_extent_backref(trans, extent_root, path, bytenr, parent,
				    root_objectid, ref_generation, 1);
	if (ret == 0) {
		struct btrfs_key found_key;
		extent_slot = path->slots[0];
		while(extent_slot > 0) {
			extent_slot--;
			btrfs_item_key_to_cpu(path->nodes[0], &found_key,
					      extent_slot);
			if (found_key.objectid != bytenr)
				break;
			if (found_key.type == BTRFS_EXTENT_ITEM_KEY &&
			    found_key.offset == num_bytes) {
				found_extent = 1;
				break;
			}
			if (path->slots[0] - extent_slot > 5)
				break;
		}
		if (!found_extent) {
			ret = remove_extent_backref(trans, extent_root, path);
			BUG_ON(ret);
			btrfs_release_path(extent_root, path);
			ret = btrfs_search_slot(trans, extent_root,
						&key, path, -1, 1);
			BUG_ON(ret);
			extent_slot = path->slots[0];
		}
	} else {
		btrfs_print_leaf(extent_root, path->nodes[0]);
		printk("Unable to find ref byte nr %llu root %llu "
		       " gen %llu owner %llu offset %llu\n",
		       (unsigned long long)bytenr,
		       (unsigned long long)root_objectid,
		       (unsigned long long)ref_generation,
		       (unsigned long long)owner_objectid,
		       (unsigned long long)owner_offset);
		BUG_ON(1);
	}

	leaf = path->nodes[0];
	ei = btrfs_item_ptr(leaf, extent_slot,
			    struct btrfs_extent_item);
	refs = btrfs_extent_refs(leaf, ei);
	BUG_ON(refs == 0);
	refs -= 1;
	btrfs_set_extent_refs(leaf, ei, refs);

	btrfs_mark_buffer_dirty(leaf);

	if (refs == 0 && found_extent && path->slots[0] == extent_slot + 1) {
		struct btrfs_extent_ref *ref;
		ref = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_ref);
		BUG_ON(btrfs_ref_num_refs(leaf, ref) != 1);
		/* if the back ref and the extent are next to each other
		 * they get deleted below in one shot
		 */
		path->slots[0] = extent_slot;
		num_to_del = 2;
	} else if (found_extent) {
		/* otherwise delete the extent back ref */
		ret = remove_extent_backref(trans, extent_root, path);
		BUG_ON(ret);
		/* if refs are 0, we need to setup the path for deletion */
		if (refs == 0) {
			btrfs_release_path(extent_root, path);
			ret = btrfs_search_slot(trans, extent_root, &key, path,
						-1, 1);
			if (ret < 0)
				return ret;
			BUG_ON(ret);
		}
	}

	if (refs == 0) {
		u64 super_used;
		u64 root_used;

		if (pin) {
			ret = pin_down_bytes(trans, root, bytenr, num_bytes, 0);
			if (ret > 0)
				mark_free = 1;
			BUG_ON(ret < 0);
		}

		/* block accounting for super block */
		super_used = btrfs_super_bytes_used(&info->super_copy);
		btrfs_set_super_bytes_used(&info->super_copy,
					   super_used - num_bytes);

		/* block accounting for root item */
		root_used = btrfs_root_used(&root->root_item);
		btrfs_set_root_used(&root->root_item,
					   root_used - num_bytes);
		ret = btrfs_del_items(trans, extent_root, path, path->slots[0],
				      num_to_del);
		if (ret)
			return ret;

		if (ops && ops->free_extent)
			ops->free_extent(root, bytenr, num_bytes);

		ret = update_block_group(trans, root, bytenr, num_bytes, 0,
					 mark_free);
		BUG_ON(ret);
	}
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
	int mark_free = 0;
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

		ret = pin_down_bytes(trans, extent_root, start,
				     end + 1 - start, 0);
		mark_free = ret > 0;
		if (!test_range_bit(extent_ins, start, end,
				    EXTENT_LOCKED, 0)) {
free_extent:
			ret = __free_extent(trans, extent_root,
					    start, end + 1 - start,
					    extent_op->orig_parent,
					    extent_root->root_key.objectid,
					    extent_op->orig_generation,
					    extent_op->level, 0, 0, mark_free);
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
				goto free_extent;

			ret = update_block_group(trans, extent_root, start,
						end + 1 - start, 0, mark_free);
			BUG_ON(ret);
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
int btrfs_free_extent(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, u64 bytenr, u64 num_bytes, u64 parent,
		      u64 root_objectid, u64 ref_generation,
		      u64 owner_objectid, u64 owner_offset, int pin)
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
		extent_op->parent = parent;
		extent_op->orig_parent = parent;
		extent_op->generation = ref_generation;
		extent_op->orig_generation = ref_generation;
		extent_op->level = (int)owner_objectid;

		set_extent_bits(&root->fs_info->pending_del,
				bytenr, bytenr + num_bytes - 1,
				EXTENT_LOCKED, GFP_NOFS);
		set_state_private(&root->fs_info->pending_del,
				  bytenr, (unsigned long)extent_op);
		return 0;
	}
	ret = __free_extent(trans, root, bytenr, num_bytes, parent,
			    root_objectid, ref_generation,
			    owner_objectid, owner_offset, pin, pin == 0);
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

	if (search_end == (u64)-1)
		search_end = btrfs_super_total_bytes(&info->super_copy);

	if (hint_byte) {
		block_group = btrfs_lookup_block_group(info, hint_byte);
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
	if (!block_group) {
		block_group = btrfs_lookup_block_group(info, search_start);
		if (!block_group)
			block_group = btrfs_lookup_block_group(info,
						       orig_search_start);
	}
	ret = find_search_start(root, &block_group, &search_start,
				total_needed, data);
	if (ret)
		goto error;

	search_start = stripe_align(root, search_start);
	ins->objectid = search_start;
	ins->offset = num_bytes;

	if (ins->objectid + num_bytes >= search_end)
		goto enospc;

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
	if (search_start + num_bytes >= search_end) {
enospc:
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
	block_group = btrfs_lookup_block_group(info, search_start);
	cond_resched();
	block_group = btrfs_find_block_group(root, block_group,
					     search_start, data, 0);
	goto check_failed;

error:
	return ret;
}
/*
 * finds a free extent and does all the dirty work required for allocation
 * returns the key for the extent through ins, and a tree buffer for
 * the first block of the extent through buf.
 *
 * returns 0 if everything worked, non-zero otherwise.
 */
int btrfs_alloc_extent(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root,
		       u64 num_bytes, u64 parent,
		       u64 root_objectid, u64 ref_generation,
		       u64 owner, u64 owner_offset,
		       u64 empty_size, u64 hint_byte,
		       u64 search_end, struct btrfs_key *ins, int data)
{
	int ret;
	int pending_ret;
	u64 super_used, root_used;
	u64 search_start = 0;
	u64 alloc_profile;
	u32 sizes[2];
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = info->extent_root;
	struct btrfs_path *path;
	struct btrfs_extent_item *extent_item;
	struct btrfs_extent_ref *ref;
	struct btrfs_key keys[2];

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
	if (ret)
		return ret;

	if (parent == 0)
		parent = ins->objectid;

	/* block accounting for super block */
	super_used = btrfs_super_bytes_used(&info->super_copy);
	btrfs_set_super_bytes_used(&info->super_copy, super_used + num_bytes);

	/* block accounting for root item */
	root_used = btrfs_root_used(&root->root_item);
	btrfs_set_root_used(&root->root_item, root_used + num_bytes);

	clear_extent_dirty(&root->fs_info->free_space_cache,
			   ins->objectid, ins->objectid + ins->offset - 1,
			   GFP_NOFS);

	if (root == extent_root) {
		struct pending_extent_op *extent_op;

		extent_op = kmalloc(sizeof(*extent_op), GFP_NOFS);
		BUG_ON(!extent_op);

		extent_op->type = PENDING_EXTENT_INSERT;
		extent_op->bytenr = ins->objectid;
		extent_op->num_bytes = ins->offset;
		extent_op->parent = parent;
		extent_op->orig_parent = 0;
		extent_op->generation = ref_generation;
		extent_op->orig_generation = 0;
		extent_op->level = (int)owner;

		set_extent_bits(&root->fs_info->extent_ins, ins->objectid,
				ins->objectid + ins->offset - 1,
				EXTENT_LOCKED, GFP_NOFS);
		set_state_private(&root->fs_info->extent_ins,
				  ins->objectid, (unsigned long)extent_op);
		goto update_block;
	}

	WARN_ON(trans->alloc_exclude_nr);
	trans->alloc_exclude_start = ins->objectid;
	trans->alloc_exclude_nr = ins->offset;

	memcpy(&keys[0], ins, sizeof(*ins));
	keys[1].objectid = ins->objectid;
	keys[1].type = BTRFS_EXTENT_REF_KEY;
	keys[1].offset = parent;
	sizes[0] = sizeof(*extent_item);
	sizes[1] = sizeof(*ref);

	path = btrfs_alloc_path();
	BUG_ON(!path);

	ret = btrfs_insert_empty_items(trans, extent_root, path, keys,
				       sizes, 2);

	BUG_ON(ret);
	extent_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				     struct btrfs_extent_item);
	btrfs_set_extent_refs(path->nodes[0], extent_item, 1);
	ref = btrfs_item_ptr(path->nodes[0], path->slots[0] + 1,
			     struct btrfs_extent_ref);

	btrfs_set_ref_root(path->nodes[0], ref, root_objectid);
	btrfs_set_ref_generation(path->nodes[0], ref, ref_generation);
	btrfs_set_ref_objectid(path->nodes[0], ref, owner);
	btrfs_set_ref_offset(path->nodes[0], ref, owner_offset);
	btrfs_set_ref_num_refs(path->nodes[0], ref, 1);

	btrfs_mark_buffer_dirty(path->nodes[0]);

	trans->alloc_exclude_start = 0;
	trans->alloc_exclude_nr = 0;
	btrfs_free_path(path);
	finish_current_insert(trans, extent_root);
	pending_ret = del_pending_extents(trans, extent_root);

	if (ret) {
		return ret;
	}
	if (pending_ret) {
		return pending_ret;
	}

update_block:
	ret = update_block_group(trans, root, ins->objectid, ins->offset, 1, 0);
	if (ret) {
		printk("update block group failed for %llu %llu\n",
		       (unsigned long long)ins->objectid,
		       (unsigned long long)ins->offset);
		BUG();
	}
	return 0;
}

/*
 * helper function to allocate a block for a given tree
 * returns the tree buffer or NULL.
 */
struct extent_buffer *btrfs_alloc_free_block(struct btrfs_trans_handle *trans,
					     struct btrfs_root *root,
					     u32 blocksize, u64 parent,
					     u64 root_objectid,
					     u64 ref_generation,
					     int level,
					     u64 hint,
					     u64 empty_size)
{
	struct btrfs_key ins;
	int ret;
	struct extent_buffer *buf;

	ret = btrfs_alloc_extent(trans, root, blocksize, parent,
				 root_objectid, ref_generation,
				 level, 0, empty_size, hint,
				 (u64)-1, &ins, 0);
	if (ret) {
		BUG_ON(ret > 0);
		return ERR_PTR(ret);
	}
	buf = btrfs_find_create_tree_block(root, ins.objectid, blocksize);
	if (!buf) {
		if (parent == 0)
			parent = ins.objectid;
		btrfs_free_extent(trans, root, ins.objectid, blocksize,
				  parent, root->root_key.objectid,
				  ref_generation, 0, 0, 0);
		BUG_ON(1);
		return ERR_PTR(-ENOMEM);
	}
	btrfs_set_buffer_uptodate(buf);
	trans->blocks_used++;
	return buf;
}

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
				key.objectid, key.offset, 0);
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
			ret = lookup_extent_ref(NULL, root, bytenr,
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
	ret = lookup_extent_ref(trans, root,
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
		ret = lookup_extent_ref(trans, root, bytenr, blocksize, &refs);
		BUG_ON(ret);
		if (refs != 1) {
			parent = path->nodes[*level];
			root_owner = btrfs_header_owner(parent);
			root_gen = btrfs_header_generation(parent);
			path->slots[*level]++;
			ret = btrfs_free_extent(trans, root, bytenr, blocksize,
						parent->start, root_owner,
						root_gen, 0, 0, 1);
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

			/* we dropped the lock, check one more time */
			ret = lookup_extent_ref(trans, root, bytenr,
						blocksize, &refs);
			BUG_ON(ret);
			if (refs != 1) {
				parent = path->nodes[*level];
				root_owner = btrfs_header_owner(parent);
				root_gen = btrfs_header_generation(parent);

				path->slots[*level]++;
				free_extent_buffer(next);
				ret = btrfs_free_extent(trans, root, bytenr,
						blocksize, parent->start,
						root_owner, root_gen, 0, 0, 1);
				BUG_ON(ret);
				continue;
			}
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
				root_owner, root_gen, 0, 0, 1);
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
						root_gen, 0, 0, 1);
			BUG_ON(ret);
			free_extent_buffer(path->nodes[*level]);
			path->nodes[*level] = NULL;
			*level = i + 1;
		}
	}
	return 1;
}

/*
 * drop the reference count on the tree rooted at 'snap'.  This traverses
 * the tree freeing any blocks that have a ref count of zero after being
 * decremented.
 */
int btrfs_drop_snapshot(struct btrfs_trans_handle *trans, struct btrfs_root
			*root)
{
	int ret = 0;
	int wret;
	int level;
	struct btrfs_path *path;
	int i;
	int orig_level;
	struct btrfs_root_item *root_item = &root->root_item;

	path = btrfs_alloc_path();
	BUG_ON(!path);

	level = btrfs_header_level(root->node);
	orig_level = level;
	if (btrfs_disk_key_objectid(&root_item->drop_progress) == 0) {
		path->nodes[level] = root->node;
		extent_buffer_get(root->node);
		path->slots[level] = 0;
	} else {
		struct btrfs_key key;
		struct btrfs_disk_key found_key;
		struct extent_buffer *node;

		btrfs_disk_key_to_cpu(&key, &root_item->drop_progress);
		level = root_item->drop_level;
		path->lowest_level = level;
		wret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
		if (wret < 0) {
			ret = wret;
			goto out;
		}
		node = path->nodes[level];
		btrfs_node_key(node, &found_key, path->slots[level]);
		WARN_ON(memcmp(&found_key, &root_item->drop_progress,
			       sizeof(found_key)));
	}
	while(1) {
		wret = walk_down_tree(trans, root, path, &level);
		if (wret < 0)
			ret = wret;
		if (wret != 0)
			break;

		wret = walk_up_tree(trans, root, path, &level);
		if (wret < 0)
			ret = wret;
		if (wret != 0)
			break;
		/*
		ret = -EAGAIN;
		break;
		*/
	}
	for (i = 0; i <= orig_level; i++) {
		if (path->nodes[i]) {
			free_extent_buffer(path->nodes[i]);
			path->nodes[i] = NULL;
		}
	}
out:
	btrfs_free_path(path);
	return ret;
}

int btrfs_free_block_groups(struct btrfs_fs_info *info)
{
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
		if (!ret)
			kfree((void *)(unsigned long)ptr);
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
	return 0;
}

int find_first_block_group(struct btrfs_root *root, struct btrfs_path *path,
			   struct btrfs_key *key)
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
		cache = kmalloc(sizeof(*cache), GFP_NOFS);
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
		btrfs_release_path(root, path);
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

		if (key.objectid >=
		    btrfs_super_total_bytes(&info->super_copy))
			break;
	}
	ret = 0;
error:
	btrfs_free_path(path);
	return ret;
}

int btrfs_make_block_group(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root, u64 bytes_used,
			   u64 type, u64 chunk_objectid, u64 chunk_offset,
			   u64 size)
{
	int ret;
	int bit = 0;
	struct btrfs_root *extent_root;
	struct btrfs_block_group_cache *cache;
	struct extent_io_tree *block_group_cache;

	extent_root = root->fs_info->extent_root;
	block_group_cache = &root->fs_info->block_group_cache;

	cache = kzalloc(sizeof(*cache), GFP_NOFS);
	BUG_ON(!cache);
	cache->key.objectid = chunk_offset;
	cache->key.offset = size;

	btrfs_set_key_type(&cache->key, BTRFS_BLOCK_GROUP_ITEM_KEY);
	btrfs_set_block_group_used(&cache->item, bytes_used);
	btrfs_set_block_group_chunk_objectid(&cache->item, chunk_objectid);
	cache->flags = type;
	btrfs_set_block_group_flags(&cache->item, type);

	ret = update_space_info(root->fs_info, cache->flags, size, bytes_used,
				&cache->space_info);
	BUG_ON(ret);

	bit = block_group_state_bits(type);
	set_extent_bits(block_group_cache, chunk_offset,
			chunk_offset + size - 1,
			bit | EXTENT_LOCKED, GFP_NOFS);

	set_state_private(block_group_cache, chunk_offset,
			  (unsigned long)cache);
	ret = btrfs_insert_item(trans, extent_root, &cache->key, &cache->item,
				sizeof(cache->item));
	BUG_ON(ret);

	finish_current_insert(trans, extent_root);
	ret = del_pending_extents(trans, extent_root);
	BUG_ON(ret);
	set_avail_alloc_bits(extent_root->fs_info, type);
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
	total_bytes = btrfs_super_total_bytes(&root->fs_info->super_copy);
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
			group_size = max_t(u64, group_size, 32 * 1024 * 1024);
			group_size = min_t(u64, group_size, 128 * 1024 * 1024);
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
