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
#include <math.h>
#include "kerncompat.h"
#include "kernel-lib/list.h"
#include "kernel-lib/radix-tree.h"
#include "kernel-lib/rbtree.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/transaction.h"
#include "crypto/crc32c.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/free-space-cache.h"
#include "kernel-shared/free-space-tree.h"
#include "kernel-shared/zoned.h"
#include "common/utils.h"

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

static int __free_extent(struct btrfs_trans_handle *trans,
			 u64 bytenr, u64 num_bytes, u64 parent,
			 u64 root_objectid, u64 owner_objectid,
			 u64 owner_offset, int refs_to_drop);
static struct btrfs_block_group *
btrfs_find_block_group(struct btrfs_root *root, struct btrfs_block_group
		       *hint, u64 search_start, int data, int owner);

static int remove_sb_from_cache(struct btrfs_root *root,
				struct btrfs_block_group *cache)
{
	u64 bytenr;
	u64 *logical;
	int stripe_len;
	int i, nr, ret;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_io_tree *free_space_cache;

	free_space_cache = &fs_info->free_space_cache;
	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = btrfs_rmap_block(fs_info, cache->start, bytenr,
				       &logical, &nr, &stripe_len);
		BUG_ON(ret);
		while (nr--) {
			clear_extent_dirty(free_space_cache, logical[nr],
				logical[nr] + stripe_len - 1);
		}
		kfree(logical);
	}
	return 0;
}

static int cache_block_group(struct btrfs_root *root,
			     struct btrfs_block_group *block_group)
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

	path->reada = READA_FORWARD;
	last = max_t(u64, block_group->start, BTRFS_SUPER_INFO_OFFSET);
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
		if (key.objectid < block_group->start) {
			goto next;
		}
		if (key.objectid >= block_group->start + block_group->length) {
			break;
		}

		if (key.type == BTRFS_EXTENT_ITEM_KEY ||
		    key.type == BTRFS_METADATA_ITEM_KEY) {
			if (key.objectid > last) {
				hole_size = key.objectid - last;
				set_extent_dirty(free_space_cache, last,
						 last + hole_size - 1);
			}
			if (key.type == BTRFS_METADATA_ITEM_KEY)
				last = key.objectid + root->fs_info->nodesize;
			else
				last = key.objectid + key.offset;
		}
next:
		path->slots[0]++;
	}

	if (block_group->start + block_group->length > last) {
		hole_size = block_group->start + block_group->length - last;
		set_extent_dirty(free_space_cache, last, last + hole_size - 1);
	}
	remove_sb_from_cache(root, block_group);
	block_group->cached = 1;
err:
	btrfs_free_path(path);
	return 0;
}

/*
 * This adds the block group to the fs_info rb tree for the block group cache
 */
static int btrfs_add_block_group_cache(struct btrfs_fs_info *info,
				struct btrfs_block_group *block_group)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct btrfs_block_group *cache;

	ASSERT(block_group->length != 0);
	p = &info->block_group_cache_tree.rb_node;

	while (*p) {
		parent = *p;
		cache = rb_entry(parent, struct btrfs_block_group,
				 cache_node);
		if (block_group->start < cache->start)
			p = &(*p)->rb_left;
		else if (block_group->start > cache->start)
			p = &(*p)->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&block_group->cache_node, parent, p);
	rb_insert_color(&block_group->cache_node,
			&info->block_group_cache_tree);

	return 0;
}

/*
 * This will return the block group which contains @bytenr if it exists.
 * If found nothing, the return depends on @next.
 *
 * @next:
 *   if 0, return NULL if there's no block group containing the bytenr.
 *   if 1, return the block group which starts after @bytenr.
 */
static struct btrfs_block_group *block_group_cache_tree_search(
		struct btrfs_fs_info *info, u64 bytenr, int next)
{
	struct btrfs_block_group *cache, *ret = NULL;
	struct rb_node *n;
	u64 end, start;

	n = info->block_group_cache_tree.rb_node;

	while (n) {
		cache = rb_entry(n, struct btrfs_block_group,
				 cache_node);
		end = cache->start + cache->length - 1;
		start = cache->start;

		if (bytenr < start) {
			if (next && (!ret || start < ret->start))
				ret = cache;
			n = n->rb_left;
		} else if (bytenr > start) {
			if (bytenr <= end) {
				ret = cache;
				break;
			}
			n = n->rb_right;
		} else {
			ret = cache;
			break;
		}
	}

	return ret;
}

/*
 * Return the block group that contains @bytenr, otherwise return the next one
 * that starts after @bytenr
 */
struct btrfs_block_group *btrfs_lookup_first_block_group(
		struct btrfs_fs_info *info, u64 bytenr)
{
	return block_group_cache_tree_search(info, bytenr, 1);
}

/*
 * Return the block group that contains the given bytenr
 */
struct btrfs_block_group *btrfs_lookup_block_group(
		struct btrfs_fs_info *info, u64 bytenr)
{
	return block_group_cache_tree_search(info, bytenr, 0);
}

static int block_group_bits(struct btrfs_block_group *cache, u64 bits)
{
	return (cache->flags & bits) == bits;
}

static int noinline find_search_start(struct btrfs_root *root,
			      struct btrfs_block_group **cache_ret,
			      u64 *start_ret, int num, int data)
{
	int ret;
	struct btrfs_block_group *cache = *cache_ret;
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

	last = max(search_start, cache->start);
	if (cache->ro || !block_group_bits(cache, data))
		goto new_group;

	if (btrfs_is_zoned(root->fs_info)) {
		if (cache->length - cache->alloc_offset < num)
			goto new_group;
		*start_ret = cache->start + cache->alloc_offset;
		cache->alloc_offset += num;
		return 0;
	}

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
		if (start + num > cache->start + cache->length) {
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
		return -ENOENT;
	}
	return -ENOSPC;

new_group:
	last = cache->start + cache->length;
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

static struct btrfs_block_group *
btrfs_find_block_group(struct btrfs_root *root, struct btrfs_block_group
		       *hint, u64 search_start, int data, int owner)
{
	struct btrfs_block_group *cache;
	struct btrfs_block_group *found_group = NULL;
	struct btrfs_fs_info *info = root->fs_info;
	u64 used;
	u64 last = 0;
	u64 hint_last;
	u64 free_check;
	int full_search = 0;
	int factor = 10;

	if (!owner)
		factor = 10;

	if (search_start) {
		struct btrfs_block_group *shint;
		shint = btrfs_lookup_block_group(info, search_start);
		if (shint && !shint->ro && block_group_bits(shint, data)) {
			used = shint->used;
			if (used + shint->pinned <
			    div_factor(shint->length, factor)) {
				return shint;
			}
		}
	}
	if (hint && !hint->ro && block_group_bits(hint, data)) {
		used = hint->used;
		if (used + hint->pinned <
		    div_factor(hint->length, factor)) {
			return hint;
		}
		last = hint->start + hint->length;
		hint_last = last;
	} else {
		if (hint)
			hint_last = max(hint->start , search_start);
		else
			hint_last = search_start;

		last = hint_last;
	}
again:
	while(1) {
		cache = btrfs_lookup_first_block_group(info, last);
		if (!cache)
			break;

		last = cache->start + cache->length;
		used = cache->used;

		if (!cache->ro && block_group_bits(cache, data)) {
			if (full_search)
				free_check = cache->length;
			else
				free_check = div_factor(cache->length, factor);

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

u64 hash_extent_data_ref(u64 root_objectid, u64 owner, u64 offset)
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
		btrfs_mark_buffer_dirty(leaf);
	}
	return ret;
}

static noinline u32 extent_data_ref_count(struct btrfs_path *path,
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
		btrfs_fs_incompat(root->fs_info, SKINNY_METADATA);

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = num_bytes;

	want = extent_ref_type(parent, owner);
	if (insert)
		extra_size = btrfs_extent_inline_ref_size(want);
	else
		extra_size = -1;

	if (owner < BTRFS_FIRST_FREE_OBJECTID && skinny_metadata) {
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
	if (item_size < sizeof(*ei)) {
		printf("Size is %u, needs to be %u, slot %d\n",
		       (unsigned)item_size,
		       (unsigned)sizeof(*ei), path->slots[0]);
		btrfs_print_leaf(leaf, BTRFS_PRINT_TREE_DEFAULT);
		return -EINVAL;
	}

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

static int setup_inline_extent_backref(struct btrfs_root *root,
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

	ret = btrfs_extend_item(root, path, size);
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
		ret = btrfs_truncate_item(root, path, item_size, 1);
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
		ret = setup_inline_extent_backref(root, path, iref,
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

	/* now insert the actual backref */
	ret = insert_extent_backref(trans, root->fs_info->extent_root,
				    path, bytenr, parent, root_objectid,
				    owner, offset, 1);
	if (ret)
		err = ret;
out:
	btrfs_free_path(path);
	BUG_ON(err);
	return err;
}

int btrfs_lookup_extent_info(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info, u64 bytenr,
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

	if (metadata && !btrfs_fs_incompat(fs_info, SKINNY_METADATA)) {
		offset = fs_info->nodesize;
		metadata = 0;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = bytenr;
	key.offset = offset;
	if (metadata)
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;

again:
	ret = btrfs_search_slot(trans, fs_info->extent_root, &key, path, 0, 0);
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
			    key.offset == fs_info->nodesize)
				ret = 0;
		}

		if (ret) {
			btrfs_release_path(path);
			key.type = BTRFS_EXTENT_ITEM_KEY;
			key.offset = fs_info->nodesize;
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
			BUG();
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

int btrfs_set_block_flags(struct btrfs_trans_handle *trans, u64 bytenr,
			  int level, u64 flags)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_path *path;
	int ret;
	struct btrfs_key key;
	struct extent_buffer *l;
	struct btrfs_extent_item *item;
	u32 item_size;
	int skinny_metadata = btrfs_fs_incompat(fs_info, SKINNY_METADATA);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = bytenr;
	if (skinny_metadata) {
		key.offset = level;
		key.type = BTRFS_METADATA_ITEM_KEY;
	} else {
		key.offset = fs_info->nodesize;
		key.type = BTRFS_EXTENT_ITEM_KEY;
	}

again:
	ret = btrfs_search_slot(trans, fs_info->extent_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;

	if (ret > 0 && skinny_metadata) {
		skinny_metadata = 0;
		if (path->slots[0]) {
			path->slots[0]--;
			btrfs_item_key_to_cpu(path->nodes[0], &key,
					      path->slots[0]);
			if (key.objectid == bytenr &&
			    key.offset == fs_info->nodesize &&
			    key.type == BTRFS_EXTENT_ITEM_KEY)
				ret = 0;
		}
		if (ret) {
			btrfs_release_path(path);
			key.offset = fs_info->nodesize;
			key.type = BTRFS_EXTENT_ITEM_KEY;
			goto again;
		}
	}

	if (ret != 0) {
		btrfs_print_leaf(path->nodes[0], BTRFS_PRINT_TREE_DEFAULT);
		printk("failed to find block number %llu\n",
			(unsigned long long)bytenr);
		BUG();
	}
	l = path->nodes[0];
	item_size = btrfs_item_size_nr(l, path->slots[0]);
	if (item_size < sizeof(*item)) {
		error(
"unsupported or corrupted extent item, item size=%u expect minimal size=%zu",
			item_size, sizeof(*item));
		ret = -EUCLEAN;
		goto out;
	}
	item = btrfs_item_ptr(l, path->slots[0], struct btrfs_extent_item);
	flags |= btrfs_extent_flags(l, item);
	btrfs_set_extent_flags(l, item, flags);
out:
	btrfs_free_path(path);
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
			if (key.type != BTRFS_EXTENT_DATA_KEY)
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
			num_bytes = root->fs_info->nodesize;
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

static int update_block_group_item(struct btrfs_trans_handle *trans,
				   struct btrfs_path *path,
				   struct btrfs_block_group *cache)
{
	int ret;
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root = fs_info->extent_root;
	unsigned long bi;
	struct btrfs_block_group_item bgi;
	struct extent_buffer *leaf;
	struct btrfs_key key;

	key.objectid = cache->start;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = cache->length;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto fail;

	leaf = path->nodes[0];
	bi = btrfs_item_ptr_offset(leaf, path->slots[0]);
	btrfs_set_stack_block_group_used(&bgi, cache->used);
	btrfs_set_stack_block_group_flags(&bgi, cache->flags);
	btrfs_set_stack_block_group_chunk_objectid(&bgi,
			BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	write_extent_buffer(leaf, &bgi, bi, sizeof(bgi));
	btrfs_mark_buffer_dirty(leaf);
fail:
	btrfs_release_path(path);
	return ret;

}

int btrfs_write_dirty_block_groups(struct btrfs_trans_handle *trans)
{
	struct btrfs_block_group *cache;
	struct btrfs_path *path;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	while (!list_empty(&trans->dirty_bgs)) {
		cache = list_first_entry(&trans->dirty_bgs,
				 struct btrfs_block_group, dirty_list);
		list_del_init(&cache->dirty_list);
		ret = update_block_group_item(trans, path, cache);
		if (ret)
			break;
	}
	btrfs_free_path(path);
	return ret;
}

static struct btrfs_space_info *__find_space_info(struct btrfs_fs_info *info,
						  u64 flags)
{
	struct btrfs_space_info *found;

	flags &= BTRFS_BLOCK_GROUP_TYPE_MASK;

	list_for_each_entry(found, &info->space_info, list) {
		if (found->flags & flags)
			return found;
	}
	return NULL;

}

static int free_space_info(struct btrfs_fs_info *fs_info, u64 flags,
                          u64 total_bytes, u64 bytes_used,
                          struct btrfs_space_info **space_info)
{
	struct btrfs_space_info *found;

	/* only support free block group which is empty */
	if (bytes_used)
		return -ENOTEMPTY;

	found = __find_space_info(fs_info, flags);
	if (!found)
		return -ENOENT;
	if (found->total_bytes < total_bytes) {
		fprintf(stderr,
			"WARNING: bad space info to free %llu only have %llu\n",
			total_bytes, found->total_bytes);
		return -EINVAL;
	}
	found->total_bytes -= total_bytes;
	if (space_info)
		*space_info = found;
	return 0;
}

int update_space_info(struct btrfs_fs_info *info, u64 flags,
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
	found->flags = flags & BTRFS_BLOCK_GROUP_TYPE_MASK;
	found->total_bytes = total_bytes;
	found->bytes_used = bytes_used;
	found->bytes_pinned = 0;
	found->bytes_reserved = 0;
	found->full = 0;
	*space_info = found;
	return 0;
}


static void set_avail_alloc_bits(struct btrfs_fs_info *fs_info, u64 flags)
{
	u64 extra_flags = flags & (BTRFS_BLOCK_GROUP_RAID0 |
				   BTRFS_BLOCK_GROUP_RAID1_MASK |
				   BTRFS_BLOCK_GROUP_RAID10 |
				   BTRFS_BLOCK_GROUP_RAID56_MASK |
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
			  struct btrfs_fs_info *fs_info, u64 alloc_bytes,
			  u64 flags)
{
	struct btrfs_space_info *space_info;
	u64 thresh;
	u64 start;
	u64 num_bytes;
	int ret;

	space_info = __find_space_info(fs_info, flags);
	if (!space_info) {
		ret = update_space_info(fs_info, flags, 0, 0, &space_info);
		BUG_ON(ret);
	}
	BUG_ON(!space_info);

	if (space_info->full)
		return 0;

	thresh = div_factor(space_info->total_bytes, 7);
	if ((space_info->bytes_used + space_info->bytes_pinned +
	     space_info->bytes_reserved + alloc_bytes) < thresh)
		return 0;

	/*
	 * Avoid allocating given chunk type
	 */
	if (fs_info->avoid_meta_chunk_alloc &&
	    (flags & BTRFS_BLOCK_GROUP_METADATA))
		return 0;
	if (fs_info->avoid_sys_chunk_alloc &&
	    (flags & BTRFS_BLOCK_GROUP_SYSTEM))
		return 0;

	/*
	 * We're going to allocate new chunk, during the process, we will
	 * allocate new tree blocks, which can trigger new chunk allocation
	 * again. Avoid the recursion.
	 */
	if (trans->allocating_chunk)
		return 0;
	trans->allocating_chunk = 1;

	/*
	 * The space_info only has block group type (data/meta/sys), doesn't
	 * have the proper profile.
	 * While we still want to handle mixed block groups properly.
	 * So here add the extra bits for mixed profile.
	 */
	flags |= space_info->flags;
	ret = btrfs_alloc_chunk(trans, fs_info, &start, &num_bytes, flags);
	if (ret == -ENOSPC) {
		space_info->full = 1;
		trans->allocating_chunk = 0;
		return 0;
	}

	BUG_ON(ret);

	ret = btrfs_make_block_group(trans, fs_info, 0, flags, start,
				     num_bytes);
	BUG_ON(ret);
	trans->allocating_chunk = 0;
	return 0;
}

static int update_block_group(struct btrfs_trans_handle *trans, u64 bytenr,
			      u64 num_bytes, int alloc, int mark_free)
{
	struct btrfs_fs_info *info = trans->fs_info;
	struct btrfs_block_group *cache;
	u64 total = num_bytes;
	u64 old_val;
	u64 byte_in_group;

	/* block accounting for super block */
	old_val = btrfs_super_bytes_used(info->super_copy);
	if (alloc)
		old_val += num_bytes;
	else
		old_val -= num_bytes;
	btrfs_set_super_bytes_used(info->super_copy, old_val);

	while(total) {
		cache = btrfs_lookup_block_group(info, bytenr);
		if (!cache) {
			return -1;
		}
		byte_in_group = bytenr - cache->start;
		WARN_ON(byte_in_group > cache->length);
		if (list_empty(&cache->dirty_list))
			list_add_tail(&cache->dirty_list, &trans->dirty_bgs);
		old_val = cache->used;
		num_bytes = min(total, cache->length- byte_in_group);

		if (alloc) {
			old_val += num_bytes;
			cache->space_info->bytes_used += num_bytes;
		} else {
			old_val -= num_bytes;
			cache->space_info->bytes_used -= num_bytes;
			if (mark_free) {
				set_extent_dirty(&info->free_space_cache,
						bytenr, bytenr + num_bytes - 1);
			}
		}
		cache->used = old_val;
		total -= num_bytes;
		bytenr += num_bytes;
	}
	return 0;
}

static int update_pinned_extents(struct btrfs_fs_info *fs_info,
				u64 bytenr, u64 num, int pin)
{
	u64 len;
	struct btrfs_block_group *cache;

	if (pin) {
		set_extent_dirty(&fs_info->pinned_extents,
				bytenr, bytenr + num - 1);
	} else {
		clear_extent_dirty(&fs_info->pinned_extents,
				bytenr, bytenr + num - 1);
	}
	while (num > 0) {
		cache = btrfs_lookup_block_group(fs_info, bytenr);
		if (!cache) {
			len = min((u64)fs_info->sectorsize, num);
			goto next;
		}
		WARN_ON(!cache);
		len = min(num, cache->length - (bytenr - cache->start));
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

void btrfs_finish_extent_commit(struct btrfs_trans_handle *trans)
{
	u64 start;
	u64 end;
	int ret;
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct extent_io_tree *free_space_cache = &fs_info->free_space_cache;
	struct extent_io_tree *pinned_extents = &fs_info->pinned_extents;

	while(1) {
		ret = find_first_extent_bit(pinned_extents, 0, &start, &end,
					    EXTENT_DIRTY);
		if (ret)
			break;
		update_pinned_extents(trans->fs_info, start, end + 1 - start,
				      0);
		clear_extent_dirty(pinned_extents, start, end);
		set_extent_dirty(free_space_cache, start, end);
	}
}

static int pin_down_bytes(struct btrfs_trans_handle *trans, u64 bytenr,
			  u64 num_bytes, int is_data)
{
	int err = 0;
	struct extent_buffer *buf;

	if (is_data)
		goto pinit;

	buf = btrfs_find_tree_block(trans->fs_info, bytenr, num_bytes);
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
			clean_tree_block(buf);
			free_extent_buffer(buf);
			return 1;
		}
	}
	free_extent_buffer(buf);
pinit:
	update_pinned_extents(trans->fs_info, bytenr, num_bytes, 1);

	BUG_ON(err < 0);
	return 0;
}

void btrfs_pin_extent(struct btrfs_fs_info *fs_info,
		       u64 bytenr, u64 num_bytes)
{
	update_pinned_extents(fs_info, bytenr, num_bytes, 1);
}

void btrfs_unpin_extent(struct btrfs_fs_info *fs_info,
			u64 bytenr, u64 num_bytes)
{
	update_pinned_extents(fs_info, bytenr, num_bytes, 0);
}

/*
 * remove an extent from the root, returns 0 on success
 */
static int __free_extent(struct btrfs_trans_handle *trans,
			 u64 bytenr, u64 num_bytes, u64 parent,
			 u64 root_objectid, u64 owner_objectid,
			 u64 owner_offset, int refs_to_drop)
{

	struct btrfs_key key;
	struct btrfs_path *path;
	struct btrfs_root *extent_root = trans->fs_info->extent_root;
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
		btrfs_fs_incompat(extent_root->fs_info, SKINNY_METADATA);

	if (trans->fs_info->free_extent_hook) {
		trans->fs_info->free_extent_hook(bytenr, num_bytes,
						parent, root_objectid, owner_objectid,
						owner_offset, refs_to_drop);

	}
	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

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
		if (!found_extent) {
			BUG_ON(iref);
			ret = remove_extent_backref(trans, extent_root, path,
						    NULL, refs_to_drop,
						    is_data);
			BUG_ON(ret);
			btrfs_release_path(path);

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
				btrfs_print_leaf(path->nodes[0], BTRFS_PRINT_TREE_DEFAULT);
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
		printf("path->slots[0]: %d path->nodes[0]:\n", path->slots[0]);
		btrfs_print_leaf(path->nodes[0], BTRFS_PRINT_TREE_DEFAULT);
		ret = -EIO;
		goto fail;
	}

	leaf = path->nodes[0];
	item_size = btrfs_item_size_nr(leaf, extent_slot);
	if (item_size < sizeof(*ei)) {
		error(
"unsupported or corrupted extent item, item size=%u expect minimal size=%zu",
			item_size, sizeof(*ei));
		ret = -EUCLEAN;
		goto fail;
	}
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

		if (found_extent) {
			BUG_ON(is_data && refs_to_drop !=
			       extent_data_ref_count(path, iref));
			if (iref) {
				BUG_ON(path->slots[0] != extent_slot);
			} else {
				BUG_ON(path->slots[0] != extent_slot + 1);
				path->slots[0] = extent_slot;
				num_to_del = 2;
			}
		}

		ret = pin_down_bytes(trans, bytenr, num_bytes,
				     is_data);
		if (ret > 0)
			mark_free = 1;
		BUG_ON(ret < 0);

		ret = btrfs_del_items(trans, extent_root, path, path->slots[0],
				      num_to_del);
		BUG_ON(ret);
		btrfs_release_path(path);

		if (is_data) {
			ret = btrfs_del_csums(trans, bytenr, num_bytes);
			BUG_ON(ret);
		}

		ret = add_to_free_space_tree(trans, bytenr, num_bytes);
		if (ret) {
			goto fail;
		}

		update_block_group(trans, bytenr, num_bytes, 0, mark_free);
	}
fail:
	btrfs_free_path(path);
	return ret;
}

int btrfs_free_tree_block(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  struct extent_buffer *buf,
			  u64 parent, int last_ref)
{
	return btrfs_free_extent(trans, root, buf->start, buf->len, parent,
				 root->root_key.objectid,
				 btrfs_header_level(buf), 0);
}

/*
 * remove an extent from the root, returns 0 on success
 */

int btrfs_free_extent(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root,
		      u64 bytenr, u64 num_bytes, u64 parent,
		      u64 root_objectid, u64 owner, u64 offset)
{
	int ret;

	WARN_ON(num_bytes < root->fs_info->sectorsize);
	/*
	 * tree log blocks never actually go into the extent allocation
	 * tree, just update pinning info and exit early.
	 */
	if (root_objectid == BTRFS_TREE_LOG_OBJECTID) {
		printf("PINNING EXTENTS IN LOG TREE\n");
		WARN_ON(owner >= BTRFS_FIRST_FREE_OBJECTID);
		btrfs_pin_extent(trans->fs_info, bytenr, num_bytes);
		ret = 0;
	} else if (owner < BTRFS_FIRST_FREE_OBJECTID) {
		BUG_ON(offset);
		ret = btrfs_add_delayed_tree_ref(trans->fs_info, trans,
						 bytenr, num_bytes, parent,
						 root_objectid, (int)owner,
						 BTRFS_DROP_DELAYED_REF,
						 NULL, NULL, NULL);
	} else {
		ret = __free_extent(trans, bytenr, num_bytes, parent,
				    root_objectid, owner, offset, 1);
	}
	return ret;
}

static u64 stripe_align(struct btrfs_root *root, u64 val)
{
	return round_up(val, (u64)root->fs_info->stripesize);
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
	struct btrfs_block_group *block_group;
	int full_scan = 0;
	int wrapped = 0;

	WARN_ON(num_bytes < info->sectorsize);
	ins->type = BTRFS_EXTENT_ITEM_KEY;

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
	    block_group->start + block_group->length) {
		search_start = block_group->start + block_group->length;
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

	if (info->excluded_extents &&
	    test_range_bit(info->excluded_extents, ins->objectid,
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
		if (check_crossing_stripes(info, ins->objectid, num_bytes)) {
			struct btrfs_block_group *bg_cache;
			u64 bg_offset;

			bg_cache = btrfs_lookup_block_group(info, ins->objectid);
			if (!bg_cache)
				goto no_bg_cache;
			bg_offset = ins->objectid - bg_cache->start;

			search_start = round_up(
				bg_offset + num_bytes, BTRFS_STRIPE_LEN) +
				bg_cache->start;
			goto new_group;
		}
no_bg_cache:
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
			 struct btrfs_key *ins, bool is_data)
{
	int ret;
	u64 search_start = 0;
	u64 alloc_profile;
	u64 profile;
	struct btrfs_fs_info *info = root->fs_info;

	if (is_data) {
		alloc_profile = info->avail_data_alloc_bits &
			        info->data_alloc_profile;
		profile = BTRFS_BLOCK_GROUP_DATA | alloc_profile;
	} else if (info->system_allocs == 1 || root == info->chunk_root) {
		alloc_profile = info->avail_system_alloc_bits &
			        info->system_alloc_profile;
		profile = BTRFS_BLOCK_GROUP_SYSTEM | alloc_profile;
	} else {
		alloc_profile = info->avail_metadata_alloc_bits &
			        info->metadata_alloc_profile;
		profile = BTRFS_BLOCK_GROUP_METADATA | alloc_profile;
	}

	/*
	 * Also preallocate metadata for csum tree and fs trees (root->ref_cows
	 * already set), as they can consume a lot of metadata space.
	 * Pre-allocate to avoid unexpected ENOSPC.
	 */
	if (root->ref_cows ||
	    root->root_key.objectid == BTRFS_CSUM_TREE_OBJECTID) {
		if (!(profile & BTRFS_BLOCK_GROUP_METADATA)) {
			ret = do_chunk_alloc(trans, info,
					     num_bytes,
					     BTRFS_BLOCK_GROUP_METADATA);
			BUG_ON(ret);
		}
		ret = do_chunk_alloc(trans, info,
				     num_bytes + SZ_2M, profile);
		BUG_ON(ret);
	}

	WARN_ON(num_bytes < info->sectorsize);
	ret = find_free_extent(trans, root, num_bytes, empty_size,
			       search_start, search_end, hint_byte, ins,
			       trans->alloc_exclude_start,
			       trans->alloc_exclude_nr, profile);
	if (ret < 0)
		return ret;
	clear_extent_dirty(&info->free_space_cache,
			   ins->objectid, ins->objectid + ins->offset - 1);
	return ret;
}

static int alloc_reserved_tree_block(struct btrfs_trans_handle *trans,
				      struct btrfs_delayed_ref_node *node,
				      struct btrfs_delayed_extent_op *extent_op)
{

	struct btrfs_delayed_tree_ref *ref = btrfs_delayed_node_to_tree_ref(node);
	bool skinny_metadata = btrfs_fs_incompat(trans->fs_info, SKINNY_METADATA);
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_extent_item *extent_item;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_space_info *sinfo;
	struct extent_buffer *leaf;
	struct btrfs_path *path;
	struct btrfs_key ins;
	u32 size = sizeof(*extent_item) + sizeof(*iref);
	u64 start, end;
	int ret;

	sinfo = __find_space_info(fs_info, BTRFS_BLOCK_GROUP_METADATA);
	ASSERT(sinfo);

	ins.objectid = node->bytenr;
	if (skinny_metadata) {
		ins.offset = ref->level;
		ins.type = BTRFS_METADATA_ITEM_KEY;
	} else {
		ins.offset = node->num_bytes;
		ins.type = BTRFS_EXTENT_ITEM_KEY;

		size += sizeof(struct btrfs_tree_block_info);
	}

	if (ref->root == BTRFS_EXTENT_TREE_OBJECTID) {
		ret = find_first_extent_bit(&trans->fs_info->extent_ins,
					    node->bytenr, &start, &end,
					    EXTENT_LOCKED);
		ASSERT(!ret);
		ASSERT(start == node->bytenr);
		ASSERT(end == node->bytenr + node->num_bytes - 1);
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_insert_empty_item(trans, fs_info->extent_root, path,
				      &ins, size);
	if (ret)
		return ret;

	leaf = path->nodes[0];
	extent_item = btrfs_item_ptr(leaf, path->slots[0],
				     struct btrfs_extent_item);
	btrfs_set_extent_refs(leaf, extent_item, 1);
	btrfs_set_extent_generation(leaf, extent_item, trans->transid);
	btrfs_set_extent_flags(leaf, extent_item,
			       extent_op->flags_to_set |
			       BTRFS_EXTENT_FLAG_TREE_BLOCK);

	if (skinny_metadata) {
		iref = (struct btrfs_extent_inline_ref *)(extent_item + 1);
	} else {
		struct btrfs_tree_block_info *block_info;
		block_info = (struct btrfs_tree_block_info *)(extent_item + 1);
		btrfs_set_tree_block_key(leaf, block_info, &extent_op->key);
		btrfs_set_tree_block_level(leaf, block_info, ref->level);
		iref = (struct btrfs_extent_inline_ref *)(block_info + 1);
	}

	btrfs_set_extent_inline_ref_type(leaf, iref, BTRFS_TREE_BLOCK_REF_KEY);
	btrfs_set_extent_inline_ref_offset(leaf, iref, ref->root);

	btrfs_mark_buffer_dirty(leaf);
	btrfs_free_path(path);

	ret = remove_from_free_space_tree(trans, ins.objectid, fs_info->nodesize);
	if (ret)
		return ret;

	ret = update_block_group(trans, ins.objectid, fs_info->nodesize, 1, 0);
	if (sinfo) {
		if (fs_info->nodesize > sinfo->bytes_reserved) {
			WARN_ON(1);
			sinfo->bytes_reserved = 0;
		} else {
			sinfo->bytes_reserved -= fs_info->nodesize;
		}
	}

	if (ref->root == BTRFS_EXTENT_TREE_OBJECTID) {
		clear_extent_bits(&trans->fs_info->extent_ins, start, end,
				  EXTENT_LOCKED);
	}

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
	u64 extent_size;
	struct btrfs_delayed_extent_op *extent_op;
	struct btrfs_space_info *sinfo;
	struct btrfs_fs_info *fs_info = root->fs_info;
	bool skinny_metadata = btrfs_fs_incompat(root->fs_info,
						 SKINNY_METADATA);

	extent_op = btrfs_alloc_delayed_extent_op();
	if (!extent_op)
		return -ENOMEM;

	sinfo = __find_space_info(fs_info, BTRFS_BLOCK_GROUP_METADATA);
	if (!sinfo) {
		error("Corrupted fs, no valid METADATA block group found");
		return -EUCLEAN;
	}
	ret = btrfs_reserve_extent(trans, root, num_bytes, empty_size,
				   hint_byte, search_end, ins, 0);
	if (ret < 0)
		return ret;

	if (key)
		memcpy(&extent_op->key, key, sizeof(extent_op->key));
	else
		memset(&extent_op->key, 0, sizeof(extent_op->key));
	extent_op->flags_to_set = flags;
	extent_op->update_key = skinny_metadata ? false : true;
	extent_op->update_flags = true;
	extent_op->is_data = false;
	extent_op->level = level;

	extent_size = ins->offset;

	if (btrfs_fs_incompat(root->fs_info, SKINNY_METADATA)) {
		ins->offset = level;
		ins->type = BTRFS_METADATA_ITEM_KEY;
	}

	/* Ensure this reserved extent is not found by the allocator */
	if (root_objectid == BTRFS_EXTENT_TREE_OBJECTID) {
		ret = set_extent_bits(&trans->fs_info->extent_ins,
				      ins->objectid,
				      ins->objectid + extent_size - 1,
				      EXTENT_LOCKED);

		BUG_ON(ret);
	}

	sinfo->bytes_reserved += extent_size;
	ret = btrfs_add_delayed_tree_ref(root->fs_info, trans, ins->objectid,
					 extent_size, 0, root_objectid,
					 level, BTRFS_ADD_DELAYED_EXTENT,
					 extent_op, NULL, NULL);
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

	buf = btrfs_find_create_tree_block(root->fs_info, ins.objectid);
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

int btrfs_free_block_groups(struct btrfs_fs_info *info)
{
	struct btrfs_space_info *sinfo;
	struct btrfs_block_group *cache, *next;
	u64 start;
	u64 end;
	int ret;

	rbtree_postorder_for_each_entry_safe(cache, next,
			     &info->block_group_cache_tree, cache_node) {
		if (!list_empty(&cache->dirty_list))
			list_del_init(&cache->dirty_list);
		RB_CLEAR_NODE(&cache->cache_node);
		if (cache->free_space_ctl) {
			btrfs_remove_free_space_cache(cache);
			kfree(cache->free_space_ctl);
		}
		kfree(cache);
	}

	while(1) {
		ret = find_first_extent_bit(&info->free_space_cache, 0,
					    &start, &end, EXTENT_DIRTY);
		if (ret)
			break;
		clear_extent_dirty(&info->free_space_cache, start, end);
	}

	while (!list_empty(&info->space_info)) {
		sinfo = list_entry(info->space_info.next,
				   struct btrfs_space_info, list);
		list_del_init(&sinfo->list);
		if (sinfo->bytes_reserved)
			warning(
		"reserved space leaked, flag=0x%llx bytes_reserved=%llu",
				sinfo->flags, sinfo->bytes_reserved);
		kfree(sinfo);
	}
	return 0;
}

/*
 * Find a block group which starts >= @key->objectid in extent tree.
 *
 * Return 0 for found
 * Return >0 for not found
 * Return <0 for error
 */
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
	ret = 1;
error:
	return ret;
}

static int read_block_group_item(struct btrfs_block_group *cache,
				 struct btrfs_path *path,
				 const struct btrfs_key *key)
{
	struct extent_buffer *leaf = path->nodes[0];
	struct btrfs_block_group_item bgi;
	int slot = path->slots[0];

	ASSERT(key->type == BTRFS_BLOCK_GROUP_ITEM_KEY);

	cache->start = key->objectid;
	cache->length = key->offset;

	read_extent_buffer(leaf, &bgi, btrfs_item_ptr_offset(leaf, slot),
			   sizeof(bgi));
	cache->used = btrfs_stack_block_group_used(&bgi);
	cache->flags = btrfs_stack_block_group_flags(&bgi);

	return 0;
}

/*
 * Read out one BLOCK_GROUP_ITEM and insert it into block group cache.
 *
 * Return 0 if nothing wrong (either insert the bg cache or skip 0 sized bg)
 * Return <0 for error.
 */
static int read_one_block_group(struct btrfs_fs_info *fs_info,
				 struct btrfs_path *path)
{
	struct extent_buffer *leaf = path->nodes[0];
	struct btrfs_space_info *space_info;
	struct btrfs_block_group *cache;
	struct btrfs_key key;
	int slot = path->slots[0];
	int ret;

	btrfs_item_key_to_cpu(leaf, &key, slot);
	ASSERT(key.type == BTRFS_BLOCK_GROUP_ITEM_KEY);

	/*
	 * Skip 0 sized block group, don't insert them into block group cache
	 * tree, as its length is 0, it won't get freed at close_ctree() time.
	 */
	if (key.offset == 0)
		return 0;

	cache = kzalloc(sizeof(*cache), GFP_NOFS);
	if (!cache)
		return -ENOMEM;
	ret = read_block_group_item(cache, path, &key);
	if (ret < 0) {
		free(cache);
		return ret;
	}
	INIT_LIST_HEAD(&cache->dirty_list);

	set_avail_alloc_bits(fs_info, cache->flags);
	ret = btrfs_chunk_readonly(fs_info, cache->start);
	if (ret < 0) {
		free(cache);
		return ret;
	}
	if (ret)
		cache->ro = 1;
	exclude_super_stripes(fs_info, cache);

	ret = update_space_info(fs_info, cache->flags, cache->length,
				cache->used, &space_info);
	if (ret < 0) {
		free(cache);
		return ret;
	}
	cache->space_info = space_info;

	ret = btrfs_load_block_group_zone_info(fs_info, cache);
	if (ret)
		return ret;

	btrfs_add_block_group_cache(fs_info, cache);
	return 0;
}

int btrfs_read_block_groups(struct btrfs_fs_info *fs_info)
{
	struct btrfs_path path;
	struct btrfs_root *root;
	int ret;
	struct btrfs_key key;

	root = fs_info->extent_root;
	key.objectid = 0;
	key.offset = 0;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	btrfs_init_path(&path);

	while(1) {
		ret = find_first_block_group(root, &path, &key);
		if (ret > 0) {
			ret = 0;
			goto error;
		}
		if (ret != 0) {
			goto error;
		}
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

		ret = read_one_block_group(fs_info, &path);
		if (ret < 0 && ret != -ENOENT)
			goto error;

		if (key.offset == 0)
			key.objectid++;
		else
			key.objectid = key.objectid + key.offset;
		key.offset = 0;
		btrfs_release_path(&path);
	}
	ret = 0;
error:
	btrfs_release_path(&path);
	return ret;
}

struct btrfs_block_group *
btrfs_add_block_group(struct btrfs_fs_info *fs_info, u64 bytes_used, u64 type,
		      u64 chunk_offset, u64 size)
{
	int ret;
	struct btrfs_block_group *cache;

	cache = kzalloc(sizeof(*cache), GFP_NOFS);
	BUG_ON(!cache);
	cache->start = chunk_offset;
	cache->length = size;

	ret = btrfs_load_block_group_zone_info(fs_info, cache);
	BUG_ON(ret);

	cache->used = bytes_used;
	cache->flags = type;
	INIT_LIST_HEAD(&cache->dirty_list);

	exclude_super_stripes(fs_info, cache);
	ret = update_space_info(fs_info, cache->flags, size, bytes_used,
				&cache->space_info);
	BUG_ON(ret);

	ret = btrfs_add_block_group_cache(fs_info, cache);
	BUG_ON(ret);
	set_avail_alloc_bits(fs_info, type);

	return cache;
}

int btrfs_make_block_group(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *fs_info, u64 bytes_used,
			   u64 type, u64 chunk_offset, u64 size)
{
	int ret;
	struct btrfs_root *extent_root = fs_info->extent_root;
	struct btrfs_block_group *cache;
	struct btrfs_block_group_item bgi;
	struct btrfs_key key;

	cache = btrfs_add_block_group(fs_info, bytes_used, type, chunk_offset,
				      size);
	btrfs_set_stack_block_group_used(&bgi, cache->used);
	btrfs_set_stack_block_group_flags(&bgi, cache->flags);
	btrfs_set_stack_block_group_chunk_objectid(&bgi,
			BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	key.objectid = cache->start;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = cache->length;
	ret = btrfs_insert_item(trans, extent_root, &key, &bgi, sizeof(bgi));
	BUG_ON(ret);

	add_block_group_free_space(trans, cache);

	return 0;
}

static int insert_block_group_item(struct btrfs_trans_handle *trans,
				   struct btrfs_block_group *block_group)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group_item bgi;
	struct btrfs_root *root;
	struct btrfs_key key;

	btrfs_set_stack_block_group_used(&bgi, block_group->used);
	btrfs_set_stack_block_group_chunk_objectid(&bgi,
				BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_stack_block_group_flags(&bgi, block_group->flags);
	key.objectid = block_group->start;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = block_group->length;

	root = fs_info->extent_root;
	return btrfs_insert_item(trans, root, &key, &bgi, sizeof(bgi));
}

/*
 * This is for converter use only.
 *
 * In that case, we don't know where are free blocks located.
 * Therefore all block group cache entries must be setup properly
 * before doing any block allocation.
 */
int btrfs_make_block_groups(struct btrfs_trans_handle *trans,
			    struct btrfs_fs_info *fs_info)
{
	u64 total_bytes;
	u64 cur_start;
	u64 group_type;
	u64 group_size;
	u64 group_align;
	u64 total_data = 0;
	u64 total_metadata = 0;
	int ret;
	struct btrfs_block_group *cache;

	total_bytes = btrfs_super_total_bytes(fs_info->super_copy);
	group_align = 64 * fs_info->sectorsize;

	cur_start = 0;
	while (cur_start < total_bytes) {
		group_size = total_bytes / 12;
		group_size = min_t(u64, group_size, total_bytes - cur_start);
		if (cur_start == 0) {
			group_type = BTRFS_BLOCK_GROUP_SYSTEM;
			group_size /= 4;
			group_size &= ~(group_align - 1);
			group_size = max_t(u64, group_size, SZ_8M);
			group_size = min_t(u64, group_size, SZ_32M);
		} else {
			group_size &= ~(group_align - 1);
			if (total_data >= total_metadata * 2) {
				group_type = BTRFS_BLOCK_GROUP_METADATA;
				group_size = min_t(u64, group_size, SZ_1G);
				total_metadata += group_size;
			} else {
				group_type = BTRFS_BLOCK_GROUP_DATA;
				group_size = min_t(u64, group_size,
						   5ULL * SZ_1G);
				total_data += group_size;
			}
			if ((total_bytes - cur_start) * 4 < group_size * 5)
				group_size = total_bytes - cur_start;
		}

		cache = kzalloc(sizeof(*cache), GFP_NOFS);
		BUG_ON(!cache);

		cache->start = cur_start;
		cache->length = group_size;
		cache->used = 0;
		cache->flags = group_type;
		INIT_LIST_HEAD(&cache->dirty_list);

		ret = update_space_info(fs_info, group_type, group_size,
					0, &cache->space_info);
		BUG_ON(ret);
		set_avail_alloc_bits(fs_info, group_type);
		btrfs_add_block_group_cache(fs_info, cache);
		cur_start += group_size;
	}
	/* then insert all the items */
	cur_start = 0;
	while(cur_start < total_bytes) {
		cache = btrfs_lookup_block_group(fs_info, cur_start);
		BUG_ON(!cache);

		ret = insert_block_group_item(trans, cache);
		BUG_ON(ret);

		cur_start = cache->start + cache->length;
	}
	return 0;
}

int btrfs_update_block_group(struct btrfs_trans_handle *trans,
			     u64 bytenr, u64 num_bytes, int alloc,
			     int mark_free)
{
	return update_block_group(trans, bytenr, num_bytes, alloc, mark_free);
}

/*
 * Just remove a block group item in extent tree
 * Caller should ensure the block group is empty and all space is pinned.
 * Or new tree block/data may be allocated into it.
 */
static int remove_block_group_item(struct btrfs_trans_handle *trans,
				   struct btrfs_path *path,
				   struct btrfs_block_group *block_group)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_key key;
	struct btrfs_root *root = fs_info->extent_root;
	int ret = 0;

	key.objectid = block_group->start;
	key.offset = block_group->length;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;

	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		return ret;

	return btrfs_del_item(trans, root, path);
}

static int free_dev_extent_item(struct btrfs_trans_handle *trans,
				struct btrfs_fs_info *fs_info,
				u64 devid, u64 dev_offset)
{
	struct btrfs_root *root = fs_info->dev_root;
	struct btrfs_path *path;
	struct btrfs_key key;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = dev_offset;

	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	ret = btrfs_del_item(trans, root, path);
out:
	btrfs_free_path(path);
	return ret;
}

static int free_chunk_dev_extent_items(struct btrfs_trans_handle *trans,
				       struct btrfs_fs_info *fs_info,
				       u64 chunk_offset)
{
	struct btrfs_chunk *chunk = NULL;
	struct btrfs_root *root= fs_info->chunk_root;
	struct btrfs_path *path;
	struct btrfs_key key;
	u16 num_stripes;
	int i;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = chunk_offset;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}
	chunk = btrfs_item_ptr(path->nodes[0], path->slots[0],
			       struct btrfs_chunk);
	num_stripes = btrfs_chunk_num_stripes(path->nodes[0], chunk);
	for (i = 0; i < num_stripes; i++) {
		u64 devid = btrfs_stripe_devid_nr(path->nodes[0], chunk, i);
		u64 offset = btrfs_stripe_offset_nr(path->nodes[0], chunk, i);
		u64 length = btrfs_stripe_length(fs_info, path->nodes[0], chunk);

		ret = btrfs_reset_chunk_zones(fs_info, devid, offset, length);
		if (ret < 0)
			goto out;

		ret = free_dev_extent_item(trans, fs_info,
			btrfs_stripe_devid_nr(path->nodes[0], chunk, i),
			btrfs_stripe_offset_nr(path->nodes[0], chunk, i));
		if (ret < 0)
			goto out;
	}
out:
	btrfs_free_path(path);
	return ret;
}

static int free_system_chunk_item(struct btrfs_super_block *super,
				  struct btrfs_key *key)
{
	struct btrfs_disk_key *disk_key;
	struct btrfs_key cpu_key;
	u32 array_size = btrfs_super_sys_array_size(super);
	char *ptr = (char *)super->sys_chunk_array;
	int cur = 0;
	int ret = -ENOENT;

	while (cur < btrfs_super_sys_array_size(super)) {
		struct btrfs_chunk *chunk;
		u32 num_stripes;
		u32 chunk_len;

		disk_key = (struct btrfs_disk_key *)(ptr + cur);
		btrfs_disk_key_to_cpu(&cpu_key, disk_key);
		if (cpu_key.type != BTRFS_CHUNK_ITEM_KEY) {
			/* just in case */
			ret = -EIO;
			goto out;
		}

		chunk = (struct btrfs_chunk *)(ptr + cur + sizeof(*disk_key));
		num_stripes = btrfs_stack_chunk_num_stripes(chunk);
		chunk_len = btrfs_chunk_item_size(num_stripes) +
			    sizeof(*disk_key);

		if (key->objectid == cpu_key.objectid &&
		    key->offset == cpu_key.offset &&
		    key->type == cpu_key.type) {
			memmove(ptr + cur, ptr + cur + chunk_len,
				array_size - cur - chunk_len);
			array_size -= chunk_len;
			btrfs_set_super_sys_array_size(super, array_size);
			ret = 0;
			goto out;
		}

		cur += chunk_len;
	}
out:
	return ret;
}

static int free_chunk_item(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *fs_info,
			   u64 bytenr)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_root *root = fs_info->chunk_root;
	struct btrfs_chunk *chunk;
	u64 chunk_type;
	int ret;

	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.offset = bytenr;
	key.type = BTRFS_CHUNK_ITEM_KEY;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}
	if (ret < 0)
		goto out;
	chunk = btrfs_item_ptr(path->nodes[0], path->slots[0],
			       struct btrfs_chunk);
	chunk_type = btrfs_chunk_type(path->nodes[0], chunk);

	ret = btrfs_del_item(trans, root, path);
	if (ret < 0)
		goto out;

	if (chunk_type & BTRFS_BLOCK_GROUP_SYSTEM)
		ret = free_system_chunk_item(fs_info->super_copy, &key);
out:
	btrfs_free_path(path);
	return ret;
}

static u64 get_dev_extent_len(struct map_lookup *map)
{
	int div;

	switch (map->type & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
	case 0: /* Single */
	case BTRFS_BLOCK_GROUP_DUP:
	case BTRFS_BLOCK_GROUP_RAID1:
	case BTRFS_BLOCK_GROUP_RAID1C3:
	case BTRFS_BLOCK_GROUP_RAID1C4:
		div = 1;
		break;
	case BTRFS_BLOCK_GROUP_RAID5:
		div = (map->num_stripes - 1);
		break;
	case BTRFS_BLOCK_GROUP_RAID6:
		div = (map->num_stripes - 2);
		break;
	case BTRFS_BLOCK_GROUP_RAID10:
		div = (map->num_stripes / map->sub_stripes);
		break;
	default:
		/* normally, read chunk security hook should handled it */
		BUG_ON(1);
	}
	return map->ce.size / div;
}

/* free block group/chunk related caches */
static int free_block_group_cache(struct btrfs_trans_handle *trans,
				  struct btrfs_fs_info *fs_info,
				  u64 bytenr, u64 len)
{
	struct btrfs_block_group *cache;
	struct cache_extent *ce;
	struct map_lookup *map;
	int ret;
	int i;
	u64 flags;

	/* Free block group cache first */
	cache = btrfs_lookup_block_group(fs_info, bytenr);
	if (!cache)
		return -ENOENT;
	flags = cache->flags;
	if (cache->free_space_ctl) {
		btrfs_remove_free_space_cache(cache);
		kfree(cache->free_space_ctl);
	}
	if (!list_empty(&cache->dirty_list))
		list_del(&cache->dirty_list);
	rb_erase(&cache->cache_node, &fs_info->block_group_cache_tree);
	ret = free_space_info(fs_info, flags, len, 0, NULL);
	if (ret < 0)
		goto out;
	kfree(cache);

	/* Then free mapping info and dev usage info */
	ce = search_cache_extent(&fs_info->mapping_tree.cache_tree, bytenr);
	if (!ce || ce->start != bytenr) {
		ret = -ENOENT;
		goto out;
	}
	map = container_of(ce, struct map_lookup, ce);
	for (i = 0; i < map->num_stripes; i++) {
		struct btrfs_device *device;

		device = map->stripes[i].dev;
		device->bytes_used -= get_dev_extent_len(map);
		ret = btrfs_update_device(trans, device);
		if (ret < 0)
			goto out;
	}
	remove_cache_extent(&fs_info->mapping_tree.cache_tree, ce);
	free(map);
out:
	return ret;
}

int btrfs_remove_block_group(struct btrfs_trans_handle *trans,
			     u64 bytenr, u64 len)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group *block_group;
	struct btrfs_path path;
	int ret = 0;

	block_group = btrfs_lookup_block_group(fs_info, bytenr);
	if (!block_group || block_group->start != bytenr ||
	    block_group->length != len)
		return -ENOENT;
	/* Double check the block group to ensure it's empty */
	if (block_group->used) {
		fprintf(stderr,
			"WARNING: block group [%llu,%llu) is not empty\n",
			bytenr, bytenr + len);
		return -EUCLEAN;
	}

	/*
	 * Now pin all space in the block group, to prevent further transaction
	 * allocate space from it.
	 * Every operation needs a transaction must be in the range.
	 */
	btrfs_pin_extent(fs_info, bytenr, len);

	btrfs_init_path(&path);
	/* delete block group item and chunk item */
	ret = remove_block_group_item(trans, &path, block_group);
	btrfs_release_path(&path);
	if (ret < 0) {
		fprintf(stderr,
			"failed to free block group item for [%llu,%llu)\n",
			bytenr, bytenr + len);
		btrfs_unpin_extent(fs_info, bytenr, len);
		return ret;
	}

	ret = free_chunk_dev_extent_items(trans, fs_info, bytenr);
	if (ret < 0) {
		fprintf(stderr,
			"failed to dev extents belongs to [%llu,%llu)\n",
			bytenr, bytenr + len);
		btrfs_unpin_extent(fs_info, bytenr, len);
		return ret;
	}
	ret = free_chunk_item(trans, fs_info, bytenr);
	if (ret < 0) {
		fprintf(stderr,
			"failed to free chunk for [%llu,%llu)\n",
			bytenr, bytenr + len);
		btrfs_unpin_extent(fs_info, bytenr, len);
		return ret;
	}

	/* Now release the block_group_cache */
	ret = free_block_group_cache(trans, fs_info, bytenr, len);
	btrfs_unpin_extent(fs_info, bytenr, len);

	return ret;
}

/*
 * Fixup block accounting. The initial block accounting created by
 * make_block_groups isn't accuracy in this case.
 */
int btrfs_fix_block_accounting(struct btrfs_trans_handle *trans)
{
	int ret = 0;
	int slot;
	u64 start = 0;
	u64 bytes_used = 0;
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_block_group *cache;
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root = fs_info->extent_root;

	ret = btrfs_run_delayed_refs(trans, -1);
	if (ret)
		return ret;

	while(1) {
		cache = btrfs_lookup_first_block_group(fs_info, start);
		if (!cache)
			break;

		start = cache->start + cache->length;
		cache->used = 0;
		cache->space_info->bytes_used = 0;
		if (list_empty(&cache->dirty_list))
			list_add_tail(&cache->dirty_list, &trans->dirty_bgs);
	}

	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
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
			ret = btrfs_update_block_group(trans,
				  key.objectid, key.offset, 1, 0);
			BUG_ON(ret);
		} else if (key.type == BTRFS_METADATA_ITEM_KEY) {
			bytes_used += fs_info->nodesize;
			ret = btrfs_update_block_group(trans,
				  key.objectid, fs_info->nodesize, 1, 0);
			if (ret)
				goto out;
		}
		path.slots[0]++;
	}
	btrfs_set_super_bytes_used(root->fs_info->super_copy, bytes_used);
	ret = 0;
out:
	btrfs_release_path(&path);
	return ret;
}

static void __get_extent_size(struct btrfs_root *root, struct btrfs_path *path,
			      u64 *start, u64 *len)
{
	struct btrfs_key key;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	BUG_ON(!(key.type == BTRFS_EXTENT_ITEM_KEY ||
		 key.type == BTRFS_METADATA_ITEM_KEY));
	*start = key.objectid;
	if (key.type == BTRFS_EXTENT_ITEM_KEY)
		*len = key.offset;
	else
		*len = root->fs_info->nodesize;
}

/*
 * Find first overlap extent for range [bytenr, bytenr + len)
 * Return 0 for found and point path to it.
 * Return >0 for not found.
 * Return <0 for err
 */
static int btrfs_search_overlap_extent(struct btrfs_root *root,
				struct btrfs_path *path, u64 bytenr, u64 len)
{
	struct btrfs_key key;
	u64 cur_start;
	u64 cur_len;
	int ret;

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		return ret;
	BUG_ON(ret == 0);

	ret = btrfs_previous_extent_item(root, path, 0);
	if (ret < 0)
		return ret;
	/* no previous, check next extent */
	if (ret > 0)
		goto next;
	__get_extent_size(root, path, &cur_start, &cur_len);
	/* Tail overlap */
	if (cur_start + cur_len > bytenr)
		return 1;

next:
	ret = btrfs_next_extent_item(root, path, bytenr + len);
	if (ret < 0)
		return ret;
	/* No next, prev already checked, no overlap */
	if (ret > 0)
		return 0;
	__get_extent_size(root, path, &cur_start, &cur_len);
	/* head overlap*/
	if (cur_start < bytenr + len)
		return 1;
	return 0;
}

static int __btrfs_record_file_extent(struct btrfs_trans_handle *trans,
				      struct btrfs_root *root, u64 objectid,
				      struct btrfs_inode_item *inode,
				      u64 file_pos, u64 disk_bytenr,
				      u64 *ret_num_bytes)
{
	int ret;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key ins_key;
	struct btrfs_path *path;
	struct btrfs_extent_item *ei;
	u64 nbytes;
	u64 extent_num_bytes;
	u64 extent_bytenr;
	u64 extent_offset;
	u64 num_bytes = *ret_num_bytes;

	/*
	 * All supported file system should not use its 0 extent.
	 * As it's for hole
	 *
	 * And hole extent has no size limit, no need to loop.
	 */
	if (disk_bytenr == 0) {
		ret = btrfs_insert_file_extent(trans, root, objectid,
						file_pos, disk_bytenr,
						num_bytes, num_bytes);
		return ret;
	}
	num_bytes = min_t(u64, num_bytes, BTRFS_MAX_EXTENT_SIZE);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* First to check extent overlap */
	ret = btrfs_search_overlap_extent(extent_root, path, disk_bytenr,
					  num_bytes);
	if (ret < 0)
		goto fail;
	if (ret > 0) {
		/* Found overlap */
		u64 cur_start;
		u64 cur_len;

		__get_extent_size(extent_root, path, &cur_start, &cur_len);
		/*
		 * For convert case, this extent should be a subset of
		 * existing one.
		 */
		BUG_ON(disk_bytenr < cur_start);

		extent_bytenr = cur_start;
		extent_num_bytes = cur_len;
		extent_offset = disk_bytenr - extent_bytenr;
	} else {
		/* No overlap, create new extent */
		btrfs_release_path(path);
		ins_key.objectid = disk_bytenr;
		ins_key.offset = num_bytes;
		ins_key.type = BTRFS_EXTENT_ITEM_KEY;

		ret = btrfs_insert_empty_item(trans, extent_root, path,
					      &ins_key, sizeof(*ei));
		if (ret == 0) {
			leaf = path->nodes[0];
			ei = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_extent_item);

			btrfs_set_extent_refs(leaf, ei, 0);
			btrfs_set_extent_generation(leaf, ei, 0);
			btrfs_set_extent_flags(leaf, ei,
					       BTRFS_EXTENT_FLAG_DATA);
			btrfs_mark_buffer_dirty(leaf);

			ret = btrfs_update_block_group(trans, disk_bytenr,
						       num_bytes, 1, 0);
			if (ret)
				goto fail;
		} else if (ret != -EEXIST) {
			goto fail;
		}
		btrfs_run_delayed_refs(trans, -1);
		extent_bytenr = disk_bytenr;
		extent_num_bytes = num_bytes;
		extent_offset = 0;
	}
	btrfs_release_path(path);
	ins_key.objectid = objectid;
	ins_key.offset = file_pos;
	ins_key.type = BTRFS_EXTENT_DATA_KEY;
	ret = btrfs_insert_empty_item(trans, root, path, &ins_key,
				      sizeof(*fi));
	if (ret)
		goto fail;
	leaf = path->nodes[0];
	fi = btrfs_item_ptr(leaf, path->slots[0],
			    struct btrfs_file_extent_item);
	btrfs_set_file_extent_generation(leaf, fi, trans->transid);
	btrfs_set_file_extent_type(leaf, fi, BTRFS_FILE_EXTENT_REG);
	btrfs_set_file_extent_disk_bytenr(leaf, fi, extent_bytenr);
	btrfs_set_file_extent_disk_num_bytes(leaf, fi, extent_num_bytes);
	btrfs_set_file_extent_offset(leaf, fi, extent_offset);
	btrfs_set_file_extent_num_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_ram_bytes(leaf, fi, extent_num_bytes);
	btrfs_set_file_extent_compression(leaf, fi, 0);
	btrfs_set_file_extent_encryption(leaf, fi, 0);
	btrfs_set_file_extent_other_encoding(leaf, fi, 0);
	btrfs_mark_buffer_dirty(leaf);

	nbytes = btrfs_stack_inode_nbytes(inode) + num_bytes;
	btrfs_set_stack_inode_nbytes(inode, nbytes);
	btrfs_release_path(path);

	ret = btrfs_inc_extent_ref(trans, root, extent_bytenr, extent_num_bytes,
				   0, root->root_key.objectid, objectid,
				   file_pos - extent_offset);
	if (ret)
		goto fail;
	ret = 0;
	*ret_num_bytes = min(extent_num_bytes - extent_offset, num_bytes);
fail:
	btrfs_free_path(path);
	return ret;
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
	u64 cur_disk_bytenr = disk_bytenr;
	u64 cur_file_pos = file_pos;
	u64 cur_num_bytes = num_bytes;
	int ret = 0;

	while (num_bytes > 0) {
		ret = __btrfs_record_file_extent(trans, root, objectid,
						 inode, cur_file_pos,
						 cur_disk_bytenr,
						 &cur_num_bytes);
		if (ret < 0)
			break;
		cur_disk_bytenr += cur_num_bytes;
		cur_file_pos += cur_num_bytes;
		num_bytes -= cur_num_bytes;
	}
	return ret;
}


static int add_excluded_extent(struct btrfs_fs_info *fs_info,
			       u64 start, u64 num_bytes)
{
	u64 end = start + num_bytes - 1;
	set_extent_bits(&fs_info->pinned_extents,
			start, end, EXTENT_UPTODATE);
	return 0;
}

void free_excluded_extents(struct btrfs_fs_info *fs_info,
			   struct btrfs_block_group *cache)
{
	u64 start, end;

	start = cache->start;
	end = start + cache->length - 1;

	clear_extent_bits(&fs_info->pinned_extents,
			  start, end, EXTENT_UPTODATE);
}

int exclude_super_stripes(struct btrfs_fs_info *fs_info,
			  struct btrfs_block_group *cache)
{
	u64 bytenr;
	u64 *logical;
	int stripe_len;
	int i, nr, ret;

	if (cache->start < BTRFS_SUPER_INFO_OFFSET) {
		stripe_len = BTRFS_SUPER_INFO_OFFSET - cache->start;
		cache->bytes_super += stripe_len;
		ret = add_excluded_extent(fs_info, cache->start, stripe_len);
		if (ret)
			return ret;
	}

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = btrfs_rmap_block(fs_info, cache->start, bytenr,
				       &logical, &nr, &stripe_len);
		if (ret)
			return ret;

		while (nr--) {
			u64 start, len;

			if (logical[nr] >= cache->start + cache->length)
				continue;

			if (logical[nr] + stripe_len <= cache->start)
				continue;

			start = logical[nr];
			if (start < cache->start) {
				start = cache->start;
				len = (logical[nr] + stripe_len) - start;
			} else {
				len = min_t(u64, stripe_len, cache->start +
					    cache->length - start);
			}

			cache->bytes_super += len;
			ret = add_excluded_extent(fs_info, start, len);
			if (ret) {
				kfree(logical);
				return ret;
			}
		}

		kfree(logical);
	}
	return 0;
}

u64 add_new_free_space(struct btrfs_block_group *block_group,
		       struct btrfs_fs_info *info, u64 start, u64 end)
{
	u64 extent_start, extent_end, size, total_added = 0;
	int ret;

	while (start < end) {
		ret = find_first_extent_bit(&info->pinned_extents, start,
					    &extent_start, &extent_end,
					    EXTENT_DIRTY | EXTENT_UPTODATE);
		if (ret)
			break;

		if (extent_start <= start) {
			start = extent_end + 1;
		} else if (extent_start > start && extent_start < end) {
			size = extent_start - start;
			total_added += size;
			ret = btrfs_add_free_space(block_group->free_space_ctl,
						   start, size);
			BUG_ON(ret); /* -ENOMEM or logic error */
			start = extent_end + 1;
		} else {
			break;
		}
	}

	if (start < end) {
		size = end - start;
		total_added += size;
		ret = btrfs_add_free_space(block_group->free_space_ctl, start,
					   size);
		BUG_ON(ret); /* -ENOMEM or logic error */
	}

	return total_added;
}

static void cleanup_extent_op(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info,
			     struct btrfs_delayed_ref_head *head)
{
	struct btrfs_delayed_extent_op *extent_op = head->extent_op;

	if (!extent_op)
		return;
	head->extent_op = NULL;
	btrfs_free_delayed_extent_op(extent_op);
}

static void unselect_delayed_ref_head(struct btrfs_delayed_ref_root *delayed_refs,
				      struct btrfs_delayed_ref_head *head)
{
	head->processing = 0;
	delayed_refs->num_heads_ready++;
}

int cleanup_ref_head(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info,
		     struct btrfs_delayed_ref_head *head)
{
	struct btrfs_delayed_ref_root *delayed_refs;

	delayed_refs = &trans->delayed_refs;

	cleanup_extent_op(trans, fs_info, head);

	/*
	 * Need to drop our head ref lock and re-acquire the delayed ref lock
	 * and then re-check to make sure nobody got added.
	 */
	if (!RB_EMPTY_ROOT(&head->ref_tree) || head->extent_op)
		return 1;

	delayed_refs->num_heads--;
	rb_erase(&head->href_node, &delayed_refs->href_root);
	RB_CLEAR_NODE(&head->href_node);

	if (head->must_insert_reserved) {
		btrfs_pin_extent(fs_info, head->bytenr, head->num_bytes);
		if (!head->is_data) {
			struct btrfs_space_info *sinfo;

			sinfo = __find_space_info(trans->fs_info,
					BTRFS_BLOCK_GROUP_METADATA);
			ASSERT(sinfo);
			sinfo->bytes_reserved -= head->num_bytes;
		}
	}

	btrfs_put_delayed_ref_head(head);
	return 0;
}

static inline struct btrfs_delayed_ref_node *
select_delayed_ref(struct btrfs_delayed_ref_head *head)
{
	struct btrfs_delayed_ref_node *ref;

	if (RB_EMPTY_ROOT(&head->ref_tree))
		return NULL;
	/*
	 * Select a delayed ref of type BTRFS_ADD_DELAYED_REF first.
	 * This is to prevent a ref count from going down to zero, which deletes
	 * the extent item from the extent tree, when there still are references
	 * to add, which would fail because they would not find the extent item.
	 */
	if (!list_empty(&head->ref_add_list))
		return list_first_entry(&head->ref_add_list,
					struct btrfs_delayed_ref_node,
					add_list);
	ref = rb_entry(rb_first(&head->ref_tree),
		       struct btrfs_delayed_ref_node, ref_node);
	ASSERT(list_empty(&ref->add_list));
	return ref;
}


static int run_delayed_tree_ref(struct btrfs_trans_handle *trans,
				struct btrfs_fs_info *fs_info,
				struct btrfs_delayed_ref_node *node,
				struct btrfs_delayed_extent_op *extent_op,
				int insert_reserved)
{
	int ret = 0;
	struct btrfs_delayed_tree_ref *ref;
	u64 parent = 0;
	u64 ref_root = 0;

	ref = btrfs_delayed_node_to_tree_ref(node);

	if (node->type == BTRFS_SHARED_BLOCK_REF_KEY)
			parent = ref->parent;
	ref_root = ref->root;

	if (node->ref_mod != 1) {
		printf("btree block(%llu) has %d references rather than 1: action %u ref_root %llu parent %llu",
			node->bytenr, node->ref_mod, node->action, ref_root,
			parent);
		return -EIO;
	}
	if (node->action == BTRFS_ADD_DELAYED_REF && insert_reserved) {
		BUG_ON(!extent_op || !extent_op->update_flags);
		ret = alloc_reserved_tree_block(trans, node, extent_op);
	} else if (node->action == BTRFS_DROP_DELAYED_REF) {
		struct btrfs_delayed_tree_ref *ref = btrfs_delayed_node_to_tree_ref(node);
		ret =  __free_extent(trans, node->bytenr, node->num_bytes,
			     ref->parent, ref->root, ref->level, 0, 1);
	} else {
		BUG();
	}

	return ret;
}

/* helper function to actually process a single delayed ref entry */
static int run_one_delayed_ref(struct btrfs_trans_handle *trans,
			       struct btrfs_fs_info *fs_info,
			       struct btrfs_delayed_ref_node *node,
			       struct btrfs_delayed_extent_op *extent_op,
			       int insert_reserved)
{
	int ret = 0;

	if (node->type == BTRFS_TREE_BLOCK_REF_KEY ||
		node->type == BTRFS_SHARED_BLOCK_REF_KEY) {
		ret = run_delayed_tree_ref(trans, fs_info, node, extent_op,
					   insert_reserved);
	} else
		BUG();
	return ret;
}

int btrfs_run_delayed_refs(struct btrfs_trans_handle *trans, unsigned long nr)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_delayed_ref_root *delayed_refs;
	struct btrfs_delayed_ref_node *ref;
	struct btrfs_delayed_ref_head *locked_ref = NULL;
	struct btrfs_delayed_extent_op *extent_op;
	int ret;
	int must_insert_reserved = 0;

	delayed_refs = &trans->delayed_refs;
	while (1) {
		if (!locked_ref) {
			locked_ref = btrfs_select_ref_head(trans);
			if (!locked_ref)
				break;
		}
		/*
		 * We need to try and merge add/drops of the same ref since we
		 * can run into issues with relocate dropping the implicit ref
		 * and then it being added back again before the drop can
		 * finish.	If we merged anything we need to re-loop so we can
		 * get a good ref.
		 * Or we can get node references of the same type that weren't
		 * merged when created due to bumps in the tree mod seq, and
		 * we need to merge them to prevent adding an inline extent
		 * backref before dropping it (triggering a BUG_ON at
		 * insert_inline_extent_backref()).
		 */
		btrfs_merge_delayed_refs(trans, delayed_refs, locked_ref);
		ref = select_delayed_ref(locked_ref);
		/*
		 * We're done processing refs in this ref_head, clean everything
		 * up and move on to the next ref_head.
		 */
		if (!ref) {
			ret = cleanup_ref_head(trans, fs_info, locked_ref);
			if (ret > 0 ) {
				/* We dropped our lock, we need to loop. */
				ret = 0;
				continue;
			}
			locked_ref = NULL;
			continue;
		}

		ref->in_tree = 0;
		rb_erase(&ref->ref_node, &locked_ref->ref_tree);
		RB_CLEAR_NODE(&ref->ref_node);
		if (!list_empty(&ref->add_list))
				list_del(&ref->add_list);
		/*
		 * When we play the delayed ref, also correct the ref_mod on
		 * head
		 */
		switch (ref->action) {
		case BTRFS_ADD_DELAYED_REF:
		case BTRFS_ADD_DELAYED_EXTENT:
			locked_ref->ref_mod -= ref->ref_mod;
			break;
		case BTRFS_DROP_DELAYED_REF:
			locked_ref->ref_mod += ref->ref_mod;
			break;
		default:
			WARN_ON(1);
		}

		/*
		 * Record the must-insert_reserved flag before we drop the spin
		 * lock.
		 */
		must_insert_reserved = locked_ref->must_insert_reserved;
		locked_ref->must_insert_reserved = 0;

		extent_op = locked_ref->extent_op;
		locked_ref->extent_op = NULL;

		ret = run_one_delayed_ref(trans, fs_info, ref, extent_op,
					  must_insert_reserved);

		btrfs_free_delayed_extent_op(extent_op);
		/*
		 * If we are re-initing extent tree in this transaction
		 * failure in freeing old roots are expected (because we don't
		 * have the old extent tree, hence backref resolution will
		 * return -EIO).
		 */
		if (ret && (!trans->reinit_extent_tree ||
		     ref->action != BTRFS_DROP_DELAYED_REF)) {
			unselect_delayed_ref_head(delayed_refs, locked_ref);
			btrfs_put_delayed_ref(ref);
			return ret;
		}

		btrfs_put_delayed_ref(ref);
	}

	return 0;
}
