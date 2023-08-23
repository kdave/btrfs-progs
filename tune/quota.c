#include <errno.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "common/messages.h"
#include "tune/tune.h"

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
