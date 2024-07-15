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
#include "common/root-tree-utils.h"
#include "common/messages.h"
#include "kernel-shared/disk-io.h"

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
int btrfs_make_subvolume(struct btrfs_trans_handle *trans, u64 objectid)
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
