/*
 * Copyright (C) 2015 Facebook.  All rights reserved.
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

#include "ctree.h"
#include "disk-io.h"
#include "free-space-cache.h"
#include "free-space-tree.h"
#include "transaction.h"

static struct btrfs_free_space_info *
search_free_space_info(struct btrfs_trans_handle *trans,
		       struct btrfs_fs_info *fs_info,
		       struct btrfs_block_group_cache *block_group,
		       struct btrfs_path *path, int cow)
{
	struct btrfs_root *root = fs_info->free_space_root;
	struct btrfs_key key;
	int ret;

	key.objectid = block_group->key.objectid;
	key.type = BTRFS_FREE_SPACE_INFO_KEY;
	key.offset = block_group->key.offset;

	ret = btrfs_search_slot(trans, root, &key, path, 0, cow);
	if (ret < 0)
		return ERR_PTR(ret);
	if (ret != 0)
		return ERR_PTR(-ENOENT);

	return btrfs_item_ptr(path->nodes[0], path->slots[0],
			      struct btrfs_free_space_info);
}

static int free_space_test_bit(struct btrfs_block_group_cache *block_group,
			       struct btrfs_path *path, u64 offset,
			       u64 sectorsize)
{
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 found_start, found_end;
	unsigned long ptr, i;

	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
	ASSERT(key.type == BTRFS_FREE_SPACE_BITMAP_KEY);

	found_start = key.objectid;
	found_end = key.objectid + key.offset;
	ASSERT(offset >= found_start && offset < found_end);

	ptr = btrfs_item_ptr_offset(leaf, path->slots[0]);
	i = (offset - found_start) / sectorsize;
	return !!extent_buffer_test_bit(leaf, ptr, i);
}

static int clear_free_space_tree(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	int nr;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = 0;
	key.type = 0;
	key.offset = 0;

	while (1) {
		ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret < 0)
			goto out;

		nr = btrfs_header_nritems(path->nodes[0]);
		if (!nr)
			break;

		path->slots[0] = 0;
		ret = btrfs_del_items(trans, root, path, 0, nr);
		if (ret)
			goto out;

		btrfs_release_path(path);
	}

	ret = 0;
out:
	btrfs_free_path(path);
	return ret;
}

int btrfs_clear_free_space_tree(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *free_space_root = fs_info->free_space_root;
	int ret;
	u64 features;

	trans = btrfs_start_transaction(tree_root, 0);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	features = btrfs_super_compat_ro_flags(fs_info->super_copy);
	features &= ~(BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID |
		      BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE);
	btrfs_set_super_compat_ro_flags(fs_info->super_copy, features);
	fs_info->free_space_root = NULL;

	ret = clear_free_space_tree(trans, free_space_root);
	if (ret)
		goto abort;

	ret = btrfs_del_root(trans, tree_root, &free_space_root->root_key);
	if (ret)
		goto abort;

	list_del(&free_space_root->dirty_list);

	ret = clean_tree_block(trans, tree_root, free_space_root->node);
	if (ret)
		goto abort;
	ret = btrfs_free_tree_block(trans, free_space_root,
				    free_space_root->node, 0, 1);
	if (ret)
		goto abort;

	free_extent_buffer(free_space_root->node);
	free_extent_buffer(free_space_root->commit_root);
	kfree(free_space_root);

	ret = btrfs_commit_transaction(trans, tree_root);

abort:
	return ret;
}

static int load_free_space_bitmaps(struct btrfs_fs_info *fs_info,
				   struct btrfs_block_group_cache *block_group,
				   struct btrfs_path *path,
				   u32 expected_extent_count,
				   int *errors)
{
	struct btrfs_root *root = fs_info->free_space_root;
	struct btrfs_key key;
	int prev_bit = 0, bit;
	u64 extent_start = 0;
	u64 start, end, offset;
	u32 extent_count = 0;
	int ret;

	start = block_group->key.objectid;
	end = block_group->key.objectid + block_group->key.offset;

	while (1) {
		ret = btrfs_next_item(root, path);
		if (ret < 0)
			goto out;
		if (ret)
			break;

		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

		if (key.type == BTRFS_FREE_SPACE_INFO_KEY)
			break;

		if (key.type != BTRFS_FREE_SPACE_BITMAP_KEY) {
			fprintf(stderr, "unexpected key of type %u\n", key.type);
			(*errors)++;
			break;
		}
		if (key.objectid >= end) {
			fprintf(stderr,
	"free space bitmap starts at %llu, beyond end of block group %llu-%llu\n",
				key.objectid, start, end);
			(*errors)++;
			break;
		}
		if (key.objectid + key.offset > end) {
			fprintf(stderr,
	"free space bitmap ends at %llu, beyond end of block group %llu-%llu\n",
				key.objectid, start, end);
			(*errors)++;
			break;
		}

		offset = key.objectid;
		while (offset < key.objectid + key.offset) {
			bit = free_space_test_bit(block_group, path, offset,
						  root->sectorsize);
			if (prev_bit == 0 && bit == 1) {
				extent_start = offset;
			} else if (prev_bit == 1 && bit == 0) {
				add_new_free_space(block_group, fs_info, extent_start, offset);
				extent_count++;
			}
			prev_bit = bit;
			offset += root->sectorsize;
		}
	}

	if (prev_bit == 1) {
		add_new_free_space(block_group, fs_info, extent_start, end);
		extent_count++;
	}

	if (extent_count != expected_extent_count) {
		fprintf(stderr, "free space info recorded %u extents, counted %u\n",
			expected_extent_count, extent_count);
		(*errors)++;
	}

	ret = 0;
out:
	return ret;
}

static int load_free_space_extents(struct btrfs_fs_info *fs_info,
				   struct btrfs_block_group_cache *block_group,
				   struct btrfs_path *path,
				   u32 expected_extent_count,
				   int *errors)
{
	struct btrfs_root *root = fs_info->free_space_root;
	struct btrfs_key key, prev_key;
	int have_prev = 0;
	u64 start, end;
	u32 extent_count = 0;
	int ret;

	start = block_group->key.objectid;
	end = block_group->key.objectid + block_group->key.offset;

	while (1) {
		ret = btrfs_next_item(root, path);
		if (ret < 0)
			goto out;
		if (ret)
			break;

		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

		if (key.type == BTRFS_FREE_SPACE_INFO_KEY)
			break;

		if (key.type != BTRFS_FREE_SPACE_EXTENT_KEY) {
			fprintf(stderr, "unexpected key of type %u\n", key.type);
			(*errors)++;
			break;
		}
		if (key.objectid >= end) {
			fprintf(stderr,
	"free space extent starts at %llu, beyond end of block group %llu-%llu\n",
				key.objectid, start, end);
			(*errors)++;
			break;
		}
		if (key.objectid + key.offset > end) {
			fprintf(stderr,
	"free space extent ends at %llu, beyond end of block group %llu-%llu\n",
				key.objectid, start, end);
			(*errors)++;
			break;
		}

		if (have_prev) {
			u64 cur_start = key.objectid;
			u64 cur_end = cur_start + key.offset;
			u64 prev_start = prev_key.objectid;
			u64 prev_end = prev_start + prev_key.offset;

			if (cur_start < prev_end) {
				fprintf(stderr,
	"free space extent %llu-%llu overlaps with previous %llu-%llu\n",
					cur_start, cur_end,
					prev_start, prev_end);
				(*errors)++;
			} else if (cur_start == prev_end) {
				fprintf(stderr,
	"free space extent %llu-%llu is unmerged with previous %llu-%llu\n",
					cur_start, cur_end,
					prev_start, prev_end);
				(*errors)++;
			}
		}

		add_new_free_space(block_group, fs_info, key.objectid, key.objectid + key.offset);
		extent_count++;

		prev_key = key;
		have_prev = 1;
	}

	if (extent_count != expected_extent_count) {
		fprintf(stderr, "free space info recorded %u extents, counted %u\n",
			expected_extent_count, extent_count);
		(*errors)++;
	}

	ret = 0;
out:
	return ret;
}

int load_free_space_tree(struct btrfs_fs_info *fs_info,
			 struct btrfs_block_group_cache *block_group)
{
	struct btrfs_free_space_info *info;
	struct btrfs_path *path;
	u32 extent_count, flags;
	int errors = 0;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	path->reada = 1;

	info = search_free_space_info(NULL, fs_info, block_group, path, 0);
	if (IS_ERR(info)) {
		ret = PTR_ERR(info);
		goto out;
	}
	extent_count = btrfs_free_space_extent_count(path->nodes[0], info);
	flags = btrfs_free_space_flags(path->nodes[0], info);

	if (flags & BTRFS_FREE_SPACE_USING_BITMAPS) {
		ret = load_free_space_bitmaps(fs_info, block_group, path,
					      extent_count, &errors);
	} else {
		ret = load_free_space_extents(fs_info, block_group, path,
					      extent_count, &errors);
	}
	if (ret)
		goto out;

	ret = 0;
out:
	btrfs_free_path(path);
	return ret ? ret : errors;
}
