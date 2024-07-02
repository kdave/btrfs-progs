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
