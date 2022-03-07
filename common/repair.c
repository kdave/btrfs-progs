/*
 * Copyright (C) 2012 Oracle.  All rights reserved.
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

#include "kernel-shared/ctree.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/disk-io.h"
#include "common/extent-cache.h"
#include "common/utils.h"
#include "common/repair.h"

int repair = 0;

int btrfs_add_corrupt_extent_record(struct btrfs_fs_info *info,
				    struct btrfs_key *first_key,
				    u64 start, u64 len, int level)

{
	int ret = 0;
	struct btrfs_corrupt_block *corrupt;

	if (!info->corrupt_blocks)
		return 0;

	corrupt = malloc(sizeof(*corrupt));
	if (!corrupt)
		return -ENOMEM;

	memcpy(&corrupt->key, first_key, sizeof(*first_key));
	corrupt->cache.start = start;
	corrupt->cache.size = len;
	corrupt->level = level;

	ret = insert_cache_extent(info->corrupt_blocks, &corrupt->cache);
	if (ret)
		free(corrupt);
	BUG_ON(ret && ret != -EEXIST);
	return ret;
}

static int traverse_tree_blocks(struct extent_io_tree *tree,
				struct extent_buffer *eb, int tree_root)
{
	struct btrfs_fs_info *fs_info = eb->fs_info;
	struct extent_buffer *tmp;
	struct btrfs_root_item *ri;
	struct btrfs_key key;
	u64 bytenr;
	int level = btrfs_header_level(eb);
	int nritems;
	int ret;
	int i;
	u64 end = eb->start + eb->len;
	bool pin = tree == &fs_info->pinned_extents;

	/*
	 * If we have pinned/excluded this block before, don't do it again.
	 * This can not only avoid forever loop with broken filesystem
	 * but also give us some speedups.
	 */
	if (test_range_bit(tree, eb->start, end - 1, EXTENT_DIRTY, 0))
		return 0;

	if (pin)
		btrfs_pin_extent(fs_info, eb->start, eb->len);
	else
		set_extent_dirty(tree, eb->start, end - 1);

	nritems = btrfs_header_nritems(eb);
	for (i = 0; i < nritems; i++) {
		if (level == 0) {
			bool is_extent_root;
			btrfs_item_key_to_cpu(eb, &key, i);
			if (key.type != BTRFS_ROOT_ITEM_KEY)
				continue;
			is_extent_root =
				key.objectid == BTRFS_EXTENT_TREE_OBJECTID;
			/* If pin, skip the extent root */
			if (pin && is_extent_root)
				continue;
			ri = btrfs_item_ptr(eb, i, struct btrfs_root_item);
			bytenr = btrfs_disk_root_bytenr(eb, ri);

			/*
			 * If at any point we start needing the real root we
			 * will have to build a stump root for the root we are
			 * in, but for now this doesn't actually use the root so
			 * just pass in extent_root.
			 */
			tmp = read_tree_block(fs_info, bytenr, 0);
			if (!extent_buffer_uptodate(tmp)) {
				fprintf(stderr, "Error reading root block\n");
				return -EIO;
			}
			ret = traverse_tree_blocks(tree, tmp, 0);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		} else {
			u64 end;

			bytenr = btrfs_node_blockptr(eb, i);
			end = bytenr + fs_info->nodesize - 1;

			/* If we aren't the tree root don't read the block */
			if (level == 1 && !tree_root) {
				if (pin)
					btrfs_pin_extent(fs_info, bytenr,
							 fs_info->nodesize);
				else
					set_extent_dirty(tree, bytenr, end);
				continue;
			}

			tmp = read_tree_block(fs_info, bytenr, 0);
			if (!extent_buffer_uptodate(tmp)) {
				fprintf(stderr, "Error reading tree block\n");
				return -EIO;
			}
			ret = traverse_tree_blocks(tree, tmp, tree_root);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int btrfs_mark_used_tree_blocks(struct btrfs_fs_info *fs_info,
				struct extent_io_tree *tree)
{
	int ret;

	ret = traverse_tree_blocks(tree, fs_info->chunk_root->node, 0);
	if (!ret)
		ret = traverse_tree_blocks(tree, fs_info->tree_root->node, 1);
	if (!ret && fs_info->block_group_root)
		ret = traverse_tree_blocks(tree,
					   fs_info->block_group_root->node, 0);
	return ret;
}

static int populate_used_from_extent_root(struct btrfs_root *root,
					  struct extent_io_tree *io_tree)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *leaf;
	struct btrfs_path path;
	struct btrfs_key key;
	int slot;
	int ret;

	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

	while(1) {
		u64 start, end;

		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0)
				break;
			if (ret > 0) {
				ret = 0;
				break;
			}
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, slot);
		start = end = key.objectid;
		if (key.type == BTRFS_EXTENT_ITEM_KEY)
			end = start + key.offset - 1;
		else if (key.type == BTRFS_METADATA_ITEM_KEY)
			end = start + fs_info->nodesize - 1;

		if (start != end) {
			if (!IS_ALIGNED(start, fs_info->sectorsize) ||
			    !IS_ALIGNED(end + 1, fs_info->sectorsize)) {
				fprintf(stderr, "unaligned value in the extent tree start %llu end %llu\n",
						start, end + 1);
				ret = -EINVAL;
				break;
			}
			set_extent_dirty(io_tree, start, end);
		}

		path.slots[0]++;
	}
	btrfs_release_path(&path);
	return ret;
}

int btrfs_mark_used_blocks(struct btrfs_fs_info *fs_info,
			   struct extent_io_tree *tree)
{
	struct btrfs_root *root;
	struct rb_node *n;
	int ret;

	root = btrfs_extent_root(fs_info, 0);
	while (1) {
		ret = populate_used_from_extent_root(root, tree);
		if (ret)
			break;
		n = rb_next(&root->rb_node);
		if (!n)
			break;
		root = rb_entry(n, struct btrfs_root, rb_node);
		if (root->root_key.objectid != BTRFS_EXTENT_TREE_OBJECTID)
			break;
	}

	return ret;
}

/*
 * Fixup block accounting. The initial block accounting created by
 * make_block_groups isn't accuracy in this case.
 */
int btrfs_fix_block_accounting(struct btrfs_trans_handle *trans)
{
	struct extent_io_tree used;
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_block_group *cache;
	u64 start, end;
	u64 bytes_used = 0;
	int ret = 0;

	ret = btrfs_run_delayed_refs(trans, -1);
	if (ret)
		return ret;

	extent_io_tree_init(&used);

	ret = btrfs_mark_used_blocks(fs_info, &used);
	if (ret)
		goto out;

	start = 0;
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

	start = 0;
	while (1) {
		ret = find_first_extent_bit(&used, 0, &start, &end,
					    EXTENT_DIRTY);
		if (ret)
			break;

		bytes_used += end - start + 1;
		ret = btrfs_update_block_group(trans, start, end - start + 1,
					       1, 0);
		if (ret)
			goto out;
		clear_extent_dirty(&used, start, end);
	}
	btrfs_set_super_bytes_used(fs_info->super_copy, bytes_used);
	ret = 0;
out:
	extent_io_tree_cleanup(&used);
	return ret;
}
