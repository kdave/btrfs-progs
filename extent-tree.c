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

static int finish_current_insert(struct btrfs_trans_handle *trans, struct
				 btrfs_root *extent_root);
static int run_pending(struct btrfs_trans_handle *trans, struct btrfs_root
		       *extent_root);

static int inc_block_ref(struct btrfs_trans_handle *trans, struct btrfs_root
			 *root, u64 bytenr, u32 blocksize)
{
	struct btrfs_path path;
	int ret;
	struct btrfs_key key;
	struct btrfs_leaf *l;
	struct btrfs_extent_item *item;
	u32 refs;

	btrfs_init_path(&path);
	key.objectid = bytenr;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	key.offset = blocksize;
	ret = btrfs_search_slot(trans, root->fs_info->extent_root, &key, &path,
				0, 1);
	if (ret != 0)
		BUG();
	BUG_ON(ret != 0);
	l = &path.nodes[0]->leaf;
	item = btrfs_item_ptr(l, path.slots[0], struct btrfs_extent_item);
	refs = btrfs_extent_refs(item);
	btrfs_set_extent_refs(item, refs + 1);

	BUG_ON(list_empty(&path.nodes[0]->dirty));
	btrfs_release_path(root->fs_info->extent_root, &path);
	finish_current_insert(trans, root->fs_info->extent_root);
	run_pending(trans, root->fs_info->extent_root);
	return 0;
}

static int lookup_block_ref(struct btrfs_trans_handle *trans, struct btrfs_root
			    *root, u64 bytenr, u32 blocksize, u32 *refs)
{
	struct btrfs_path path;
	int ret;
	struct btrfs_key key;
	struct btrfs_leaf *l;
	struct btrfs_extent_item *item;
	btrfs_init_path(&path);
	key.objectid = bytenr;
	key.offset = blocksize;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	ret = btrfs_search_slot(trans, root->fs_info->extent_root, &key, &path,
				0, 0);
	if (ret != 0)
		BUG();
	l = &path.nodes[0]->leaf;
	item = btrfs_item_ptr(l, path.slots[0], struct btrfs_extent_item);
	*refs = btrfs_extent_refs(item);
	btrfs_release_path(root->fs_info->extent_root, &path);
	return 0;
}

int btrfs_inc_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct btrfs_buffer *buf)
{
	u64 bytenr;
	int i;

	if (!root->ref_cows)
		return 0;

	if (btrfs_is_leaf(&buf->node))
		return 0;

	for (i = 0; i < btrfs_header_nritems(&buf->node.header); i++) {
		bytenr = btrfs_node_blockptr(&buf->node, i);
		inc_block_ref(trans, root, bytenr, root->nodesize);
	}
	return 0;
}

static int write_one_cache_group(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 struct btrfs_block_group_cache *cache)
{
	int ret;
	int pending_ret;
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	struct btrfs_block_group_item *bi;

	ret = btrfs_search_slot(trans, root->fs_info->extent_root,
				&cache->key, path, 0, 1);
	BUG_ON(ret);
	bi = btrfs_item_ptr(&path->nodes[0]->leaf, path->slots[0],
			    struct btrfs_block_group_item);
	memcpy(bi, &cache->item, sizeof(*bi));
	dirty_tree_block(trans, extent_root, path->nodes[0]);
	btrfs_release_path(extent_root, path);
	finish_current_insert(trans, root);
	pending_ret = run_pending(trans, root);
	if (ret)
		return ret;
	if (pending_ret)
		return pending_ret;
	return 0;

}

int btrfs_write_dirty_block_groups(struct btrfs_trans_handle *trans,
				    struct btrfs_root *root)
{
	struct btrfs_block_group_cache *bg;
	struct cache_extent *cache;
	int err = 0;
	int werr = 0;
	struct cache_tree *bg_cache = &root->fs_info->block_group_cache;
	struct btrfs_path path;
	btrfs_init_path(&path);
	u64 start = 0;

	while(1) {
		cache = find_first_cache_extent(bg_cache, start);
		if (!cache)
			break;
		bg = container_of(cache, struct btrfs_block_group_cache,
					cache);
		start = cache->start + cache->size;
		if (bg->dirty) {
			err = write_one_cache_group(trans, root,
						    &path, bg);
			if (err)
				werr = err;
		}
		bg->dirty = 0;
	}
	return werr;
}

static int update_block_group(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root,
			      u64 bytenr, u64 num, int alloc)
{
	struct btrfs_block_group_cache *bg;
	struct cache_extent *cache;
	struct btrfs_fs_info *info = root->fs_info;
	u64 total = num;
	u64 old_val;
	u64 byte_in_group;

	while(total) {
		cache = find_first_cache_extent(&info->block_group_cache,
						bytenr);
		if (!cache)
			return -1;
		bg = container_of(cache, struct btrfs_block_group_cache,
					cache);
		bg->dirty = 1;
		byte_in_group = bytenr - bg->key.objectid;
		old_val = btrfs_block_group_used(&bg->item);
		if (total > bg->key.offset - byte_in_group)
			num = bg->key.offset - byte_in_group;
		else
			num = total;
		total -= num;
		bytenr += num;
		if (alloc)
			old_val += num;
		else
			old_val -= num;
		btrfs_set_block_group_used(&bg->item, old_val);
	}
	return 0;
}

int btrfs_finish_extent_commit(struct btrfs_trans_handle *trans, struct
			       btrfs_root *root)
{
	u64 first = 0;
	struct cache_extent *pe;
	struct cache_extent *next;

	pe = find_first_cache_extent(&root->fs_info->pinned_tree, 0);
	if (pe)
		first = pe->start;
	while(pe) {
		next = next_cache_extent(pe);
		remove_cache_extent(&root->fs_info->pinned_tree, pe);
		free_cache_extent(pe);
		pe = next;
	}
	root->fs_info->last_insert.objectid = first;
	root->fs_info->last_insert.offset = 0;
	return 0;
}

static int finish_current_insert(struct btrfs_trans_handle *trans, struct
				 btrfs_root *extent_root)
{
	struct btrfs_key ins;
	struct btrfs_extent_item extent_item;
	int ret;
	struct btrfs_fs_info *info = extent_root->fs_info;
	struct cache_extent *pe;
	struct cache_extent *next;
	struct cache_tree *pending_tree = &info->pending_tree;

	btrfs_set_extent_refs(&extent_item, 1);
	btrfs_set_extent_owner(&extent_item, extent_root->root_key.objectid);
	ins.offset = 1;
	btrfs_set_key_type(&ins, BTRFS_EXTENT_ITEM_KEY);
	pe = find_first_cache_extent(pending_tree, 0);
	while(pe) {
		ins.offset = pe->size;
		ins.objectid = pe->start;

		remove_cache_extent(pending_tree, pe);
		next = next_cache_extent(pe);
		if (!next)
			next = find_first_cache_extent(pending_tree, 0);

		free_cache_extent(pe);
		pe = next;

		ret = btrfs_insert_item(trans, extent_root, &ins, &extent_item,
					sizeof(extent_item));
		if (ret) {
			btrfs_print_tree(extent_root, extent_root->node);
		}
		BUG_ON(ret);
	}
	return 0;
}

/*
 * remove an extent from the root, returns 0 on success
 */
static int __free_extent(struct btrfs_trans_handle *trans, struct btrfs_root
			 *root, u64 bytenr, u64 num_bytes, int pin)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = info->extent_root;
	int ret;
	struct btrfs_extent_item *ei;
	u32 refs;

	key.objectid = bytenr;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	key.offset = num_bytes;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(trans, extent_root, &key, &path, -1, 1);
	if (ret) {
		btrfs_print_tree(extent_root, extent_root->node);
		printf("failed to find %llu\n",
		       (unsigned long long)key.objectid);
		BUG();
	}
	ei = btrfs_item_ptr(&path.nodes[0]->leaf, path.slots[0],
			    struct btrfs_extent_item);
	BUG_ON(ei->refs == 0);
	refs = btrfs_extent_refs(ei) - 1;
	btrfs_set_extent_refs(ei, refs);
	if (refs == 0) {
		u64 super_bytes_used, root_bytes_used;
		if (pin) {
			int err;
			err = insert_cache_extent(&info->pinned_tree,
						    bytenr, num_bytes);
			BUG_ON(err);
		}
		super_bytes_used = btrfs_super_bytes_used(info->disk_super);
		btrfs_set_super_bytes_used(info->disk_super,
					    super_bytes_used - num_bytes);
		root_bytes_used = btrfs_root_bytes_used(&root->root_item);
		btrfs_set_root_bytes_used(&root->root_item,
					  root_bytes_used - num_bytes);

		ret = btrfs_del_item(trans, extent_root, &path);
		if (!pin && extent_root->fs_info->last_insert.objectid >
		    bytenr)
			extent_root->fs_info->last_insert.objectid = bytenr;
		if (ret)
			BUG();
		ret = update_block_group(trans, root, bytenr, num_bytes, 0);
		BUG_ON(ret);
	}
	btrfs_release_path(extent_root, &path);
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
	struct cache_extent *pe;
	struct cache_extent *next;
	struct cache_tree *del_pending = &extent_root->fs_info->del_pending;

	pe = find_first_cache_extent(del_pending, 0);
	while(pe) {
		remove_cache_extent(del_pending, pe);
		ret = __free_extent(trans, extent_root,
				    pe->start, pe->size, 1);
		BUG_ON(ret);
		next = next_cache_extent(pe);
		if (!next)
			next = find_first_cache_extent(del_pending, 0);
		free_cache_extent(pe);
		pe = next;
	}
	return 0;
}

static int run_pending(struct btrfs_trans_handle *trans, struct btrfs_root
		       *extent_root)
{
	del_pending_extents(trans, extent_root);
	return 0;
}


/*
 * remove an extent from the root, returns 0 on success
 */
int btrfs_free_extent(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, u64 bytenr, u64 num_bytes, int pin)
{
	struct btrfs_root *extent_root = root->fs_info->extent_root;
	int pending_ret;
	int ret;

	if (root == extent_root) {
		ret = insert_cache_extent(&root->fs_info->del_pending,
					    bytenr, num_bytes);
		BUG_ON(ret);
		return 0;
	}
	ret = __free_extent(trans, root, bytenr, num_bytes, pin);
	pending_ret = run_pending(trans, root->fs_info->extent_root);
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
static int find_free_extent(struct btrfs_trans_handle *trans, struct btrfs_root
			    *orig_root, u64 total_needed, u64 search_start,
			    u64 search_end, struct btrfs_key *ins)
{
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;
	u64 hole_size = 0;
	int slot = 0;
	u64 last_byte = 0;
	u64 aligned;
	int start_found;
	struct btrfs_leaf *l;
	struct btrfs_root * root = orig_root->fs_info->extent_root;

	if (root->fs_info->last_insert.objectid > search_start)
		search_start = root->fs_info->last_insert.objectid;

	search_start = stripe_align(root, search_start);
	btrfs_set_key_type(ins, BTRFS_EXTENT_ITEM_KEY);

check_failed:
	btrfs_init_path(&path);
	ins->objectid = search_start;
	ins->offset = 0;
	start_found = 0;
	ret = btrfs_search_slot(trans, root, ins, &path, 0, 0);
	if (ret < 0)
		goto error;

	if (path.slots[0] > 0)
		path.slots[0]--;

	while (1) {
		l = &path.nodes[0]->leaf;
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(&l->header)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto error;
			if (!start_found) {
				aligned = stripe_align(root, search_start);
				ins->objectid = aligned;
				ins->offset = (u64)-1 - aligned;
				start_found = 1;
				goto check_pending;
			}
			ins->objectid = stripe_align(root,
						     last_byte > search_start ?
						     last_byte : search_start);
			ins->offset = (u64)-1 - ins->objectid;
			goto check_pending;
		}
		btrfs_disk_key_to_cpu(&key, &l->items[slot].key);
		if (btrfs_key_type(&key) != BTRFS_EXTENT_ITEM_KEY)
			goto next;
		if (key.objectid >= search_start) {
			if (start_found) {
				if (last_byte < search_start)
					last_byte = search_start;
				aligned = stripe_align(root, last_byte);
				hole_size = key.objectid - aligned;
				if (key.objectid > aligned &&
				    hole_size > total_needed) {
					ins->objectid = aligned;
					ins->offset = hole_size;
					goto check_pending;
				}
			}
		}
		start_found = 1;
		last_byte = key.objectid + key.offset;
next:
		path.slots[0]++;
	}
	// FIXME -ENOSPC
check_pending:
	/* we have to make sure we didn't find an extent that has already
	 * been allocated by the map tree or the original allocation
	 */
	btrfs_release_path(root, &path);
	BUG_ON(ins->objectid < search_start);
	if (find_cache_extent(&root->fs_info->pinned_tree,
				ins->objectid, total_needed)) {
		search_start = ins->objectid + total_needed;
		goto check_failed;
	}
	if (find_cache_extent(&root->fs_info->pending_tree,
				ins->objectid, total_needed)) {
		search_start = ins->objectid + total_needed;
		goto check_failed;
	}
	root->fs_info->last_insert.objectid = ins->objectid;
	ins->offset = total_needed;
	return 0;
error:
	btrfs_release_path(root, &path);
	return ret;
}
/*
 * finds a free extent and does all the dirty work required for allocation
 * returns the key for the extent through ins, and a tree buffer for
 * the first block of the extent through buf.
 *
 * returns 0 if everything worked, non-zero otherwise.
 */
static int alloc_extent(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 owner,
			u64 num_bytes, u64 search_start,
			u64 search_end, struct btrfs_key *ins)
{
	int ret;
	int pending_ret;
	u64 super_bytes_used, root_bytes_used;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = info->extent_root;
	struct btrfs_extent_item extent_item;

	btrfs_set_extent_refs(&extent_item, 1);
	btrfs_set_extent_owner(&extent_item, owner);

	ret = find_free_extent(trans, root, num_bytes, search_start,
			       search_end, ins);
	if (ret)
		return ret;

	super_bytes_used = btrfs_super_bytes_used(info->disk_super);
	btrfs_set_super_bytes_used(info->disk_super, super_bytes_used +
				    num_bytes);
	root_bytes_used = btrfs_root_bytes_used(&root->root_item);
	btrfs_set_root_bytes_used(&root->root_item, root_bytes_used +
				   num_bytes);
	if (root == extent_root) {
		ret = insert_cache_extent(&root->fs_info->pending_tree,
					    ins->objectid, ins->offset);
		BUG_ON(ret);
		return 0;
	}

	ret = btrfs_insert_item(trans, extent_root, ins, &extent_item,
				sizeof(extent_item));

	finish_current_insert(trans, extent_root);
	pending_ret = run_pending(trans, extent_root);
	if (ret)
		return ret;
	if (pending_ret)
		return pending_ret;
	return 0;
}

/*
 * helper function to allocate a block for a given tree
 * returns the tree buffer or NULL.
 */
struct btrfs_buffer *btrfs_alloc_free_block(struct btrfs_trans_handle *trans,
					    struct btrfs_root *root,
					    u32 blocksize)
{
	struct btrfs_key ins;
	int ret;
	struct btrfs_buffer *buf;

	ret = alloc_extent(trans, root, root->root_key.objectid,
			   blocksize, 0, (unsigned long)-1, &ins);
	if (ret) {
		BUG();
		return NULL;
	}
	ret = update_block_group(trans, root, ins.objectid, ins.offset, 1);
	buf = find_tree_block(root, ins.objectid, blocksize);
	btrfs_set_header_generation(&buf->node.header,
				    root->root_key.offset + 1);
	btrfs_set_header_bytenr(&buf->node.header, buf->bytenr);
	memcpy(buf->node.header.fsid, root->fs_info->disk_super->fsid,
	       sizeof(buf->node.header.fsid));
	dirty_tree_block(trans, root, buf);
	return buf;

}

/*
 * helper function for drop_snapshot, this walks down the tree dropping ref
 * counts as it goes.
 */
static int walk_down_tree(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, struct btrfs_path *path, int *level)
{
	struct btrfs_buffer *next;
	struct btrfs_buffer *cur;
	u64 bytenr;
	int ret;
	u32 refs;

	ret = lookup_block_ref(trans, root, path->nodes[*level]->bytenr,
			       btrfs_level_size(root, *level), &refs);
	BUG_ON(ret);
	if (refs > 1)
		goto out;
	/*
	 * walk down to the last node level and free all the leaves
	 */
	while(*level > 0) {
		u32 size = btrfs_level_size(root, *level - 1);

		cur = path->nodes[*level];
		if (path->slots[*level] >=
		    btrfs_header_nritems(&cur->node.header))
			break;
		bytenr = btrfs_node_blockptr(&cur->node, path->slots[*level]);
		ret = lookup_block_ref(trans, root, bytenr, size, &refs);
		if (refs != 1 || *level == 1) {
			path->slots[*level]++;
			ret = btrfs_free_extent(trans, root, bytenr, size, 1);
			BUG_ON(ret);
			continue;
		}
		BUG_ON(ret);
		next = read_tree_block(root, bytenr, size);
		if (path->nodes[*level-1])
			btrfs_block_release(root, path->nodes[*level-1]);
		path->nodes[*level-1] = next;
		*level = btrfs_header_level(&next->node.header);
		path->slots[*level] = 0;
	}
out:
	ret = btrfs_free_extent(trans, root, path->nodes[*level]->bytenr,
				btrfs_level_size(root, *level), 1);
	btrfs_block_release(root, path->nodes[*level]);
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
static int walk_up_tree(struct btrfs_trans_handle *trans, struct btrfs_root
			*root, struct btrfs_path *path, int *level)
{
	int i;
	int slot;
	int ret;
	for(i = *level; i < BTRFS_MAX_LEVEL - 1 && path->nodes[i]; i++) {
		slot = path->slots[i];
		if (slot <
		    btrfs_header_nritems(&path->nodes[i]->node.header)- 1) {
			path->slots[i]++;
			*level = i;
			return 0;
		} else {
			ret = btrfs_free_extent(trans, root,
					path->nodes[*level]->bytenr,
					btrfs_level_size(root, *level), 1);
			btrfs_block_release(root, path->nodes[*level]);
			path->nodes[*level] = NULL;
			*level = i + 1;
			BUG_ON(ret);
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
			*root, struct btrfs_buffer *snap)
{
	int ret = 0;
	int wret;
	int level;
	struct btrfs_path path;
	int i;
	int orig_level;

	btrfs_init_path(&path);

	level = btrfs_header_level(&snap->node.header);
	orig_level = level;
	path.nodes[level] = snap;
	path.slots[level] = 0;
	while(1) {
		wret = walk_down_tree(trans, root, &path, &level);
		if (wret > 0)
			break;
		if (wret < 0)
			ret = wret;

		wret = walk_up_tree(trans, root, &path, &level);
		if (wret > 0)
			break;
		if (wret < 0)
			ret = wret;
	}
	for (i = 0; i <= orig_level; i++) {
		if (path.nodes[i]) {
			btrfs_block_release(root, path.nodes[i]);
		}
	}
	return ret;
}

int btrfs_free_block_groups(struct btrfs_fs_info *info)
{
	struct btrfs_block_group_cache *bg;
	struct cache_extent *cache;

	while(1) {
		cache = find_first_cache_extent(&info->block_group_cache, 0);
		if (!cache)
			break;
		bg = container_of(cache, struct btrfs_block_group_cache,
					cache);
		remove_cache_extent(&info->block_group_cache, cache);
		free(bg);
	}
	return 0;
}

int btrfs_read_block_groups(struct btrfs_root *root)
{
	struct btrfs_path path;
	int ret;
	int err = 0;
	struct btrfs_block_group_item *bi;
	struct btrfs_block_group_cache *bg;
	struct cache_tree *bg_cache;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_leaf *leaf;
	u64 group_size = BTRFS_BLOCK_GROUP_SIZE;

	root = root->fs_info->extent_root;
	bg_cache = &root->fs_info->block_group_cache;
	key.objectid = 0;
	key.offset = group_size;
	btrfs_set_key_type(&key, BTRFS_BLOCK_GROUP_ITEM_KEY);
	btrfs_init_path(&path);

	while(1) {
		ret = btrfs_search_slot(NULL, root->fs_info->extent_root,
					&key, &path, 0, 0);
		if (ret != 0) {
			err = ret;
			break;
		}
		leaf = &path.nodes[0]->leaf;
		btrfs_disk_key_to_cpu(&found_key,
				      &leaf->items[path.slots[0]].key);
		bg = malloc(sizeof(*bg));
		if (!bg) {
			err = -1;
			break;
		}
		bi = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_block_group_item);
		memcpy(&bg->item, bi, sizeof(*bi));
		memcpy(&bg->key, &found_key, sizeof(found_key));
		key.objectid = found_key.objectid + found_key.offset;
		btrfs_release_path(root, &path);
		bg->cache.start = found_key.objectid;
		bg->cache.size = found_key.offset;
		bg->dirty = 0;
		ret = insert_existing_cache_extent(bg_cache, &bg->cache);
		BUG_ON(ret);
		if (key.objectid >=
		    btrfs_super_total_bytes(root->fs_info->disk_super))
			break;
	}
	btrfs_release_path(root, &path);
	return 0;
}

int btrfs_insert_block_group(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     struct btrfs_key *key,
			     struct btrfs_block_group_item *bi)
{
	int ret;
	int pending_ret;

	root = root->fs_info->extent_root;
	ret = btrfs_insert_item(trans, root, key, bi, sizeof(*bi));
	finish_current_insert(trans, root);
	pending_ret = run_pending(trans, root);
	if (ret)
		return ret;
	if (pending_ret)
		return pending_ret;
	return ret;
}
