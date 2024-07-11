#include <errno.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "common/messages.h"
#include "tune/tune.h"

static int remove_quota_tree(struct btrfs_fs_info *fs_info)
{
	int ret;
	struct btrfs_root *quota_root = fs_info->quota_root;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_super_block *sb = fs_info->super_copy;
	int super_flags = btrfs_super_incompat_flags(sb);
	struct btrfs_trans_handle *trans;

	trans = btrfs_start_transaction(quota_root, 0);
	ret = btrfs_clear_tree(trans, quota_root);
	if (ret) {
		btrfs_abort_transaction(trans, ret);
		return ret;
	}

	ret = btrfs_delete_and_free_root(trans, quota_root);
	if (ret) {
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	fs_info->quota_root = NULL;
	super_flags &= ~BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA;
	btrfs_set_super_incompat_flags(sb, super_flags);
	btrfs_commit_transaction(trans, tree_root);
	return 0;
}

/*
 * Given a pointer (ptr) into DATAi (i = slot), and an amount to shift,
 * move all the data to the left (slots >= slot) of that ptr to the right by
 * the shift amount. This overwrites the shift bytes after ptr, effectively
 * removing them from the item data. We must update affected item sizes (only
 * at slot) and offsets (slots >= slot).
 *
 * Leaf view, using '-' to show shift scale:
 * Before:
 * [ITEM0,...,ITEMi,...,ITEMn,-------,DATAn,...,[---DATAi---],...,DATA0]
 * After:
 * [ITEM0,...,ITEMi,...,ITEMn,--------,DATAn,...,[--DATAi---],...,DATA0]
 *
 * Zooming in on DATAi
 * (ptr points at the start of the Ys, and shift is length of the Ys)
 * Before:
 * ...[DATAi+1][XXXXXXXXXXXXYYYYYYYYYYYYYYYYXXXXXXX][DATAi-1]...
 * After:
 * ...................[DATAi+1][XXXXXXXXXXXXXXXXXXX][DATAi-1]...
 * Note that DATAi-1 and smaller are not affected.
 */
static void shift_leaf_data(struct btrfs_trans_handle *trans,
			    struct extent_buffer *leaf, int slot,
			    unsigned long ptr, u32 shift)
{
	u32 nr = btrfs_header_nritems(leaf);
	u32 leaf_data_off = btrfs_item_ptr_offset(leaf, nr - 1);
	u32 len = ptr - leaf_data_off;
	u32 new_size = btrfs_item_size(leaf, slot) - shift;
	for (int i = slot; i < nr; i++) {
		u32 old_item_offset = btrfs_item_offset(leaf, i);
		btrfs_set_item_offset(leaf, i, old_item_offset + shift);
	}
	memmove_extent_buffer(leaf, leaf_data_off + shift, leaf_data_off, len);
	btrfs_set_item_size(leaf, slot, new_size);
	btrfs_set_header_generation(leaf, trans->transid);
	btrfs_mark_buffer_dirty(leaf);
}

/*
 * Iterate over the extent tree and for each EXTENT_DATA item that has an inline
 * ref of type OWNER_REF, shift that leaf to eliminate the owner ref.
 *
 * Note: we use a search_slot per leaf rather than find_next_leaf to get the
 * needed CoW-ing and rebalancing for each leaf and its path up to the root.
 */
static int remove_owner_refs(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *extent_root;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_path path = { 0 };
	int slot;
	int ret;

	extent_root = btrfs_extent_root(fs_info, 0);

	trans = btrfs_start_transaction(extent_root, 0);

	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;

search_slot:
	ret = btrfs_search_slot(trans, extent_root, &key, &path, 1, 1);
	if (ret < 0)
		return ret;
	leaf = path.nodes[0];
	slot = path.slots[0];

	while (1) {
		struct btrfs_key found_key;
		struct btrfs_extent_item *ei;
		struct btrfs_extent_inline_ref *iref;
		u8 type;
		unsigned long ptr;
		unsigned long item_end;

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, &path);
			if (ret < 0) {
				break;
			} else if (ret) {
				ret = 0;
				break;
			}
			leaf = path.nodes[0];
			slot = path.slots[0];
			btrfs_item_key_to_cpu(leaf, &key, slot);
			btrfs_release_path(&path);
			goto search_slot;
		}

		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		if (found_key.type != BTRFS_EXTENT_ITEM_KEY)
			goto next;
		ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
		ptr = (unsigned long)(ei + 1);
		item_end = (unsigned long)ei + btrfs_item_size(leaf, slot);
		/* No inline extent references; accessing type is invalid. */
		if (ptr > item_end)
			goto next;
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		if (type == BTRFS_EXTENT_OWNER_REF_KEY)
			shift_leaf_data(trans, leaf, slot, ptr, sizeof(*iref));
next:
		slot++;
	}
	btrfs_release_path(&path);

	ret = btrfs_commit_transaction(trans, extent_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		return ret;
	}
	return 0;
}

int remove_squota(struct btrfs_fs_info *fs_info)
{
	int ret;

	ret = remove_owner_refs(fs_info);
	if (ret)
		return ret;

	return remove_quota_tree(fs_info);
}

static int create_qgroup(struct btrfs_fs_info *fs_info,
			 struct btrfs_trans_handle *trans,
			 u64 qgroupid)
{
	struct btrfs_path path = { 0 };
	struct btrfs_root *quota_root = fs_info->quota_root;
	struct btrfs_key key;
	int ret;

	if (qgroupid >> BTRFS_QGROUP_LEVEL_SHIFT) {
		error("qgroup level other than 0 is not supported yet");
		return -ENOTTY;
	}

	key.objectid = 0;
	key.type = BTRFS_QGROUP_INFO_KEY;
	key.offset = qgroupid;

	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(struct btrfs_qgroup_info_item));
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_LIMIT_KEY;
	key.offset = qgroupid;
	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(struct btrfs_qgroup_limit_item));
	btrfs_release_path(&path);

	printf("created qgroup for %llu\n", qgroupid);
	return ret;
}

static int create_qgroups(struct btrfs_fs_info *fs_info,
			  struct btrfs_trans_handle *trans)
{
	struct btrfs_key key = {
		.objectid = 0,
		.type = BTRFS_ROOT_REF_KEY,
		.offset = 0,
	};
	struct btrfs_path path = { 0 };
	struct extent_buffer *leaf;
	int slot;
	struct btrfs_root *tree_root = fs_info->tree_root;
	int ret;


	ret = create_qgroup(fs_info, trans, BTRFS_FS_TREE_OBJECTID);
	if (ret)
		goto out;

	ret = btrfs_search_slot_for_read(tree_root, &key, &path, 1, 0);
	if (ret)
		goto out;

	while (1) {
		slot = path.slots[0];
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.type == BTRFS_ROOT_REF_KEY) {
			ret = create_qgroup(fs_info, trans, key.offset);
			if (ret)
				goto out;
		}
		ret = btrfs_next_item(tree_root, &path);
		if (ret < 0) {
			error("failed to advance to next item");
			goto out;
		}
		if (ret)
			break;
	}

out:
	btrfs_release_path(&path);
	return ret;
}

int enable_quota(struct btrfs_fs_info *fs_info, bool simple)
{
	struct btrfs_super_block *sb = fs_info->super_copy;
	struct btrfs_trans_handle *trans;
	int super_flags = btrfs_super_incompat_flags(sb);
	struct btrfs_qgroup_status_item *qsi;
	struct btrfs_root *quota_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int flags;
	int ret;

	trans = btrfs_start_transaction(fs_info->tree_root, 2);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	ret = btrfs_create_root(trans, fs_info, BTRFS_QUOTA_TREE_OBJECTID);
	if (ret < 0) {
		error("failed to create quota root: %d (%m)", ret);
		goto fail;
	}
	quota_root = fs_info->quota_root;

	/* Create the qgroup status item */
	key.objectid = 0;
	key.type = BTRFS_QGROUP_STATUS_KEY;
	key.offset = 0;

	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(*qsi));
	if (ret < 0) {
		error("failed to insert qgroup status item: %d (%m)", ret);
		goto fail;
	}

	qsi = btrfs_item_ptr(path.nodes[0], path.slots[0],
			     struct btrfs_qgroup_status_item);
	btrfs_set_qgroup_status_generation(path.nodes[0], qsi, trans->transid);
	btrfs_set_qgroup_status_rescan(path.nodes[0], qsi, 0);
	flags = BTRFS_QGROUP_STATUS_FLAG_ON;
	if (simple) {
		btrfs_set_qgroup_status_enable_gen(path.nodes[0], qsi, trans->transid);
		flags |= BTRFS_QGROUP_STATUS_FLAG_SIMPLE_MODE;
	} else {
		flags |= BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT;
	}

	btrfs_set_qgroup_status_version(path.nodes[0], qsi, 1);
	btrfs_set_qgroup_status_flags(path.nodes[0], qsi, flags);
	btrfs_release_path(&path);

	/* Create the qgroup items */
	ret = create_qgroups(fs_info, trans);
	if (ret < 0) {
		error("failed to create qgroup items for subvols %d (%m)", ret);
		goto fail;
	}

	/* Set squota incompat flag */
	if (simple) {
		super_flags |= BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA;
		btrfs_set_super_incompat_flags(sb, super_flags);
	}

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		return ret;
	}
	return ret;
fail:
	btrfs_abort_transaction(trans, ret);
	return ret;
}
