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

	/*
	 * We've already made sure there's not a balance in progress,
	 * double-check that we're not going to mess things up because the
	 * tree has multiple leaves.
	 */
	assert(btrfs_header_level(root->node) == 0);

	rb_erase(&root->rb_node, &fs_info->fs_root_tree);

	ret = btrfs_delete_and_free_root(trans, root);
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

	kfree(fs_info->remap_root);
	fs_info->remap_root = root;

	add_root_to_dirty_list(root);

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

	while (true) {
		ret = btrfs_search_slot(trans, fs_info->block_group_root,
					&key, &path, -1, 1);
		if (ret < 0)
			return ret;

		leaf = path.nodes[0];

		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			btrfs_release_path(&path);
			break;
		}

		while (path.slots[0] < btrfs_header_nritems(leaf)) {
			btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);

			assert(key.type == BTRFS_BLOCK_GROUP_ITEM_KEY);

			size = btrfs_item_size(leaf, path.slots[0]);

			assert(size == sizeof(struct btrfs_block_group_item));

			btrfs_extend_item(&path,
				sizeof(struct btrfs_block_group_item_v2) - size);

			leaf = path.nodes[0];

			bgi = btrfs_item_ptr(leaf, path.slots[0],
					     struct btrfs_block_group_item_v2);

			btrfs_set_block_group_v2_remap_bytes(leaf, bgi, 0);
			btrfs_set_block_group_v2_identity_remap_count(leaf, bgi, 0);

			path.slots[0]++;
		}

		key.objectid++;
		key.offset = 0;

		btrfs_release_path(&path);
	}

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
