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

#include <time.h>
#include <uuid/uuid.h>
#include "common/root-tree-utils.h"
#include "common/messages.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/uuid-tree.h"

int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid)
{
	int ret;
	struct btrfs_inode_item inode_item;
	time_t now = time(NULL);

	memset(&inode_item, 0, sizeof(inode_item));
	btrfs_set_stack_inode_generation(&inode_item, trans->transid);
	btrfs_set_stack_inode_size(&inode_item, 0);
	btrfs_set_stack_inode_nlink(&inode_item, 1);
	btrfs_set_stack_inode_nbytes(&inode_item, root->fs_info->nodesize);
	btrfs_set_stack_inode_mode(&inode_item, S_IFDIR | 0755);
	btrfs_set_stack_timespec_sec(&inode_item.atime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.atime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.ctime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.ctime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.mtime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.mtime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.otime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.otime, 0);

	if (root->fs_info->tree_root == root)
		btrfs_set_super_root_dir(root->fs_info->super_copy, objectid);

	ret = btrfs_insert_inode(trans, root, objectid, &inode_item);
	if (ret)
		goto error;

	ret = btrfs_insert_inode_ref(trans, root, "..", 2, objectid, objectid, 0);
	if (ret)
		goto error;

	btrfs_set_root_dirid(&root->root_item, objectid);
	ret = 0;
error:
	return ret;
}

/*
 * Create a subvolume and initialize its content with the top inode.
 *
 * The created tree root would have its root_ref as 1.
 * Thus for subvolumes caller needs to properly add ROOT_BACKREF items.
 */
int btrfs_make_subvolume(struct btrfs_trans_handle *trans, u64 objectid,
			 bool readonly)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root;
	struct btrfs_key key = {
		.objectid = objectid,
		.type = BTRFS_ROOT_ITEM_KEY,
	};
	int ret;

	/* FSTREE is different and can not be created by this function. */
	UASSERT(objectid != BTRFS_FS_TREE_OBJECTID);
	UASSERT(is_fstree(objectid) || objectid == BTRFS_DATA_RELOC_TREE_OBJECTID);

	root = btrfs_create_tree(trans, &key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto error;
	}
	/*
	 * Free it for now, and re-read it from disk to setup cache and
	 * tracking.
	 */
	btrfs_free_fs_root(root);
	root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto error;
	}
	ret = btrfs_make_root_dir(trans, root, BTRFS_FIRST_FREE_OBJECTID);
	if (ret < 0)
		goto error;

	btrfs_set_stack_inode_flags(&root->root_item.inode,
				    BTRFS_INODE_ROOT_ITEM_INIT);

	if (readonly)
		btrfs_set_root_flags(&root->root_item, BTRFS_ROOT_SUBVOL_RDONLY);

	ret = btrfs_update_root(trans, fs_info->tree_root, &root->root_key,
				&root->root_item);
	if (ret < 0)
		goto error;
	return 0;
error:
	btrfs_abort_transaction(trans, ret);
	return ret;
}

/*
 * Link subvoume @subvol as @name under directory inode @parent_dir of
 * subvolume @parent_root.
 */
int btrfs_link_subvolume(struct btrfs_trans_handle *trans,
			 struct btrfs_root *parent_root,
			 u64 parent_dir, const char *name,
			 int namelen, struct btrfs_root *subvol)
{
	struct btrfs_root *tree_root = trans->fs_info->tree_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	struct btrfs_inode_item *ii;
	u64 index;
	u64 isize;
	u32 imode;
	int ret;

	UASSERT(namelen && namelen <= BTRFS_NAME_LEN);

	/* Make sure @parent_dir is a directory. */
	key.objectid = parent_dir;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, parent_root, &key, &path, 0, 0);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}
	ii = btrfs_item_ptr(path.nodes[0], path.slots[0], struct btrfs_inode_item);
	imode = btrfs_inode_mode(path.nodes[0], ii);
	btrfs_release_path(&path);

	if (!S_ISDIR(imode)) {
		ret = -EUCLEAN;
		error("%s: inode %llu of subvolume %llu is not a directory",
		      __func__, parent_dir, parent_root->root_key.objectid);
		return ret;
	}

	ret = btrfs_find_free_dir_index(parent_root, parent_dir, &index);
	if (ret < 0)
		return ret;

	/* Filename conflicts check. */
	ret = btrfs_check_dir_conflict(parent_root, name, namelen, parent_dir,
				       index);
	if (ret < 0)
		return ret;

	/*
	 * Now everything is fine, add the link.
	 * From now on, every error would lead to transaction abort.
	 *
	 * Add the dir_item/index first.
	 */
	ret = btrfs_insert_dir_item(trans, parent_root, name, namelen,
				    parent_dir, &subvol->root_key,
				    BTRFS_FT_DIR, index);
	if (ret < 0)
		goto abort;

	/* Update inode size of the parent inode */
	key.objectid = parent_dir;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, parent_root, &key, &path, 1, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0) {
		btrfs_release_path(&path);
		goto abort;
	}
	ii = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_inode_item);
	isize = btrfs_inode_size(path.nodes[0], ii);
	isize += namelen * 2;
	btrfs_set_inode_size(path.nodes[0], ii, isize);
	btrfs_mark_buffer_dirty(path.nodes[0]);
	btrfs_release_path(&path);

	/* Add the root backref. */
	ret = btrfs_add_root_ref(trans, tree_root, subvol->root_key.objectid,
				 BTRFS_ROOT_BACKREF_KEY,
				 parent_root->root_key.objectid, parent_dir,
				 index, name, namelen);
	if (ret < 0)
		goto abort;

	/* Then forward ref*/
	ret = btrfs_add_root_ref(trans, tree_root,
				 parent_root->root_key.objectid,
				 BTRFS_ROOT_REF_KEY, subvol->root_key.objectid,
				 parent_dir, index, name, namelen);
	if (ret < 0)
		goto abort;
	/* For now, all root should have its refs == 1 already.
	 * So no need to update the root refs. */
	UASSERT(btrfs_root_refs(&subvol->root_item) == 1);
	return 0;
abort:
	btrfs_abort_transaction(trans, ret);
	return ret;
}

static int remove_all_tree_items(struct btrfs_root *root)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	int ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS,
			  "remove all items for tree %lld: %m",
			  root->root_key.objectid);
		return ret;
	}
	while (true) {
		int nr_items;

		ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
		if (ret < 0) {
			errno = -ret;
			error("failed to locate the first key of root %lld: %m",
				root->root_key.objectid);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		if (ret == 0) {
			ret = -EUCLEAN;
			errno = -ret;
			error("unexpected all zero key found in root %lld",
				root->root_key.objectid);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		nr_items = btrfs_header_nritems(path.nodes[0]);
		/* The tree is empty. */
		if (nr_items == 0) {
			btrfs_release_path(&path);
			break;
		}
		ret = btrfs_del_items(trans, root, &path, 0, nr_items);
		btrfs_release_path(&path);
		if (ret < 0) {
			errno = -ret;
			error("failed to empty the first leaf of root %lld: %m",
				root->root_key.objectid);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
	}
	ret = btrfs_commit_transaction(trans, root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS,
			  "removal all items for tree %lld: %m",
			  root->root_key.objectid);
	}
	return ret;
}

static int rescan_subvol_uuid(struct btrfs_trans_handle *trans,
			      struct btrfs_key *subvol_key)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *subvol;
	int ret;

	UASSERT(is_fstree(subvol_key->objectid));

	/*
	 * Read out the subvolume root and updates root::root_item.
	 * This is to avoid de-sync between in-memory and on-disk root_items.
	 */
	subvol = btrfs_read_fs_root(fs_info, subvol_key);
	if (IS_ERR(subvol)) {
		ret = PTR_ERR(subvol);
		error("failed to read subvolume %llu: %m",
			subvol_key->objectid);
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	/* The uuid is not set, regenerate one. */
	if (uuid_is_null(subvol->root_item.uuid)) {
		uuid_generate(subvol->root_item.uuid);
		ret = btrfs_update_root(trans, fs_info->tree_root, &subvol->root_key,
					&subvol->root_item);
		if (ret < 0) {
			error("failed to update subvolume %llu: %m",
			      subvol_key->objectid);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
	}
	ret = btrfs_uuid_tree_add(trans, subvol->root_item.uuid,
				  BTRFS_UUID_KEY_SUBVOL,
				  subvol->root_key.objectid);
	if (ret < 0) {
		errno = -ret;
		error("failed to add uuid for subvolume %llu: %m",
		      subvol_key->objectid);
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	if (!uuid_is_null(subvol->root_item.received_uuid)) {
		ret = btrfs_uuid_tree_add(trans, subvol->root_item.uuid,
					  BTRFS_UUID_KEY_RECEIVED_SUBVOL,
					  subvol->root_key.objectid);
		if (ret < 0) {
			errno = -ret;
			error("failed to add received_uuid for subvol %llu: %m",
			      subvol->root_key.objectid);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
	}
	return 0;
}

static int rescan_uuid_tree(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *uuid_root = fs_info->uuid_root;
	struct btrfs_trans_handle *trans;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	int ret;

	UASSERT(uuid_root);
	trans = btrfs_start_transaction(uuid_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "rescan uuid tree: %m");
		return ret;
	}
	key.objectid = BTRFS_LAST_FREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	/* Iterate through all subvolumes except fs tree. */
	while (true) {
		struct btrfs_key found_key;
		struct extent_buffer *leaf;
		int slot;

		/* No more subvolume. */
		if (key.objectid < BTRFS_FIRST_FREE_OBJECTID) {
			ret = 0;
			break;
		}
		ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
		if (ret < 0) {
			errno = -ret;
			error_msg(ERROR_MSG_READ, "iterate subvolumes: %m");
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		if (ret > 0) {
			ret = btrfs_previous_item(tree_root, &path,
						  BTRFS_FIRST_FREE_OBJECTID,
						  BTRFS_ROOT_ITEM_KEY);
			if (ret < 0) {
				errno = -ret;
				btrfs_release_path(&path);
				error_msg(ERROR_MSG_READ, "iterate subvolumes: %m");
				btrfs_abort_transaction(trans, ret);
				return ret;
			}
			/* No more subvolume. */
			if (ret > 0) {
				ret = 0;
				btrfs_release_path(&path);
				break;
			}
		}
		leaf = path.nodes[0];
		slot = path.slots[0];
		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		btrfs_release_path(&path);
		key.objectid = found_key.objectid - 1;

		ret = rescan_subvol_uuid(trans, &found_key);
		if (ret < 0) {
			errno = -ret;
			error("failed to rescan the uuid of subvolume %llu: %m",
			      found_key.objectid);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
	}

	/* Update fs tree uuid. */
	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;
	ret = rescan_subvol_uuid(trans, &key);
	if (ret < 0) {
		errno = -ret;
		error("failed to rescan the uuid of subvolume %llu: %m",
		      key.objectid);
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	ret = btrfs_commit_transaction(trans, uuid_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "rescan uuid tree: %m");
	}
	return ret;
}

/*
 * Rebuild the whole uuid tree.
 *
 * If no uuid tree is present, create a new one.
 * If there is an existing uuid tree, all items will be deleted first.
 *
 * For all existing subvolumes (except fs tree), any uninitialized uuid
 * (all zero) will be generated using a random uuid, and inserted into the new
 * tree.
 * And if a subvolume has its UUID initialized, it will not be touched and
 * added to the new uuid tree.
 */
int btrfs_rebuild_uuid_tree(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *uuid_root;
	struct btrfs_key key;
	int ret;

	if (!fs_info->uuid_root) {
		struct btrfs_trans_handle *trans;

		trans = btrfs_start_transaction(fs_info->tree_root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			errno = -ret;
			error_msg(ERROR_MSG_START_TRANS, "create uuid tree: %m");
			return ret;
		}
		key.objectid = BTRFS_UUID_TREE_OBJECTID;
		key.type = BTRFS_ROOT_ITEM_KEY;
		key.offset = 0;
		uuid_root = btrfs_create_tree(trans, &key);
		if (IS_ERR(uuid_root)) {
			ret = PTR_ERR(uuid_root);
			errno = -ret;
			error("failed to create uuid root: %m");
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		add_root_to_dirty_list(uuid_root);
		fs_info->uuid_root = uuid_root;
		ret = btrfs_commit_transaction(trans, fs_info->tree_root);
		if (ret < 0) {
			errno = -ret;
			error_msg(ERROR_MSG_COMMIT_TRANS, "create uuid tree: %m");
			return ret;
		}
	} else {
		ret = remove_all_tree_items(fs_info->uuid_root);
		if (ret < 0) {
			errno = -ret;
			error("failed to clear the existing uuid tree: %m");
			return ret;
		}
	}
	UASSERT(fs_info->uuid_root);
	ret = rescan_uuid_tree(fs_info);
	if (ret < 0) {
		errno = -ret;
		error("failed to rescan the uuid tree: %m");
		return ret;
	}
	return 0;
}
