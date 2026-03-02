/*
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

#include "common/messages.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "tune/tune.h"

static int remove_data_reloc_tree(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root;
	int ret;

	struct btrfs_key key = {
		.objectid = BTRFS_DATA_RELOC_TREE_OBJECTID,
		.type = BTRFS_ROOT_ITEM_KEY,
		.offset = 0,
	};

	root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root))
		return PTR_ERR(root);

	ret = btrfs_del_root(trans, fs_info->tree_root, &root->root_key);
	if (ret)
		return ret;

	ret = btrfs_free_tree_block(trans, btrfs_root_id(root), root->node,
				    0, 1);
	if (ret)
		return ret;

	return 0;
}

static int create_remap_tree(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_super_block *sb = fs_info->super_copy;
	struct btrfs_root *root;
	u64 bg_flags, chunk_start, chunk_size;
	int ret;

	struct btrfs_key key = {
		.objectid = BTRFS_REMAP_TREE_OBJECTID,
		.type = BTRFS_ROOT_ITEM_KEY,
		.offset = 0,
	};

	bg_flags = BTRFS_BLOCK_GROUP_METADATA_REMAP;
	bg_flags |= fs_info->avail_metadata_alloc_bits &
			fs_info->metadata_alloc_profile;

	ret = btrfs_alloc_chunk(trans, fs_info, &chunk_start, &chunk_size,
				bg_flags);
	if (ret)
		return ret;

	ret = btrfs_make_block_group(trans, fs_info, 0, bg_flags,
				     chunk_start, chunk_size);
	if (ret)
		return ret;

	root = btrfs_create_tree(trans, &key);
	if (IS_ERR(root))
		return PTR_ERR(root);

	btrfs_set_super_remap_root(sb, root->root_item.bytenr);
	btrfs_set_super_remap_root_generation(sb, root->root_item.generation);
	btrfs_set_super_remap_root_level(sb, root->root_item.level);

	btrfs_free_fs_root(root);

	return 0;
}

static int extend_block_group_items(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	struct extent_buffer *leaf;
	struct btrfs_block_group_item_v2 *bgi;
	int ret;
	u32 size;

	ret = btrfs_search_slot(trans, fs_info->block_group_root, &key, &path,
				-1, 1);
	if (ret < 0)
		return ret;

	leaf = path.nodes[0];

	while (true) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(fs_info->block_group_root, &path);
			if (ret)
				break;

			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);

		size = btrfs_item_size(leaf, path.slots[0]);

		if (size >= sizeof(struct btrfs_block_group_item_v2)) {
			path.slots[0]++;
			continue;
		}

		btrfs_extend_item(&path,
			sizeof(struct btrfs_block_group_item_v2) - size);

		bgi = btrfs_item_ptr(leaf, path.slots[0],
				     struct btrfs_block_group_item_v2);

		btrfs_set_block_group_v2_remap_bytes(leaf, bgi, 0);
		btrfs_set_block_group_v2_identity_remap_count(leaf, bgi, 0);

		path.slots[0]++;
	}

	btrfs_release_path(&path);

	return 0;
}

int convert_to_remap_tree(struct btrfs_fs_info *fs_info)
{
	struct btrfs_super_block *sb = fs_info->super_copy;
	struct btrfs_trans_handle *trans;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int ret;

	key.objectid = BTRFS_BALANCE_OBJECTID;
	key.type = BTRFS_BALANCE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, fs_info->tree_root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

	if (!ret) {
		error("Can't convert filesystem that has balance already in progress");
		btrfs_release_path(&path);
		return -EINVAL;
	}

	btrfs_release_path(&path);

	trans = btrfs_start_transaction(fs_info->tree_root, 0);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	ret = remove_data_reloc_tree(trans);
	if (ret)
		goto fail;

	ret = create_remap_tree(trans);
	if (ret)
		goto fail;

	ret = extend_block_group_items(trans);
	if (ret)
		goto fail;

	btrfs_set_super_incompat_flags(sb, btrfs_super_incompat_flags(sb) |
		BTRFS_FEATURE_INCOMPAT_REMAP_TREE);

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret)
		goto fail;

	pr_verbose(LOG_DEFAULT, "Converted filesystem to remap tree feature\n");

	return ret;

fail:
	btrfs_abort_transaction(trans, ret);
	return ret;
}
