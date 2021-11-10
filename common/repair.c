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
