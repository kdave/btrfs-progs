/*
 * Copyright (C) 2014 Fujitsu.  All rights reserved.
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

/*
 * Unlike inode.c in kernel, which can use most of the kernel infrastructure
 * like inode/dentry things, in user-land, we can only use inode number to
 * do directly operation on extent buffer, which may cause extra searching,
 * but should not be a huge problem since progs is less performance sensitive.
 */
#include <sys/stat.h>

#include "ctree.h"
#include "transaction.h"
#include "disk-io.h"
#include "time.h"
#include "common/messages.h"
#include "common/internal.h"

/*
 * Find a free inode index for later btrfs_add_link().
 * Currently just search from the largest dir_index and +1.
 */
static int btrfs_find_free_dir_index(struct btrfs_root *root, u64 dir_ino,
				     u64 *ret_ino)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	u64 ret_val = 2;
	int ret = 0;

	if (!ret_ino)
		return 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = dir_ino;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	ret = 0;
	if (path->slots[0] == 0) {
		ret = btrfs_prev_leaf(root, path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			/*
			 * This shouldn't happen since there must be a leaf
			 * containing the DIR_ITEM.
			 * Can only happen when the previous leaf is corrupted.
			 */
			ret = -EIO;
			goto out;
		}
	} else {
		path->slots[0]--;
	}
	btrfs_item_key_to_cpu(path->nodes[0], &found_key, path->slots[0]);
	if (found_key.objectid != dir_ino ||
	    found_key.type != BTRFS_DIR_INDEX_KEY)
		goto out;
	ret_val = found_key.offset + 1;
out:
	btrfs_free_path(path);
	if (ret == 0)
		*ret_ino = ret_val;
	return ret;
}

/* Check the dir_item/index conflicts before insert */
int check_dir_conflict(struct btrfs_root *root, char *name, int namelen,
		       u64 dir, u64 index)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_inode_item *inode_item;
	struct btrfs_dir_item *dir_item;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* Given dir exists? */
	key.objectid = dir;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	/* Is it a dir? */
	inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_inode_item);
	if (!(btrfs_inode_mode(path->nodes[0], inode_item) & S_IFDIR)) {
		ret = -ENOTDIR;
		goto out;
	}
	btrfs_release_path(path);

	/* Name conflicting? */
	dir_item = btrfs_lookup_dir_item(NULL, root, path, dir, name,
					 namelen, 0);
	if (IS_ERR(dir_item)) {
		ret = PTR_ERR(dir_item);
		goto out;
	}
	if (dir_item) {
		ret = -EEXIST;
		goto out;
	}
	btrfs_release_path(path);

	/* Index conflicting? */
	dir_item = btrfs_lookup_dir_index(NULL, root, path, dir, name,
					  namelen, index, 0);
	if (IS_ERR(dir_item) && PTR_ERR(dir_item) == -ENOENT)
		dir_item = NULL;
	if (IS_ERR(dir_item)) {
		ret = PTR_ERR(dir_item);
		goto out;
	}
	if (dir_item) {
		ret = -EEXIST;
		goto out;
	}

out:
	btrfs_free_path(path);
	return ret;
}

/*
 * Add dir_item/index for 'parent_ino' if add_backref is true, also insert a
 * backref from the ino to parent dir and update the nlink(Kernel version does
 * not do this thing)
 *
 * Currently only supports adding link from an inode to another inode.
 */
int btrfs_add_link(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		   u64 ino, u64 parent_ino, char *name, int namelen,
		   u8 type, u64 *index, int add_backref, int ignore_existed)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_inode_item *inode_item;
	u32 nlink;
	u64 inode_size;
	u64 ret_index = 0;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	if (index && *index) {
		ret_index = *index;
	} else {
		ret = btrfs_find_free_dir_index(root, parent_ino, &ret_index);
		if (ret < 0)
			goto out;
	}

	ret = check_dir_conflict(root, name, namelen, parent_ino, ret_index);
	if (ret < 0 && !(ignore_existed && ret == -EEXIST))
		goto out;

	/* Add inode ref */
	if (add_backref) {
		ret = btrfs_insert_inode_ref(trans, root, name, namelen,
					     ino, parent_ino, ret_index);
		if (ret < 0 && !(ignore_existed && ret == -EEXIST))
			goto out;

		/* do not update nlinks if existed */
		if (!ret) {
			/* Update nlinks for the inode */
			key.objectid = ino;
			key.type = BTRFS_INODE_ITEM_KEY;
			key.offset = 0;
			ret = btrfs_search_slot(trans, root, &key, path, 1, 1);
			if (ret) {
				if (ret > 0)
					ret = -ENOENT;
				goto out;
			}
			inode_item = btrfs_item_ptr(path->nodes[0],
				    path->slots[0], struct btrfs_inode_item);
			nlink = btrfs_inode_nlink(path->nodes[0], inode_item);
			nlink++;
			btrfs_set_inode_nlink(path->nodes[0], inode_item,
					      nlink);
			btrfs_mark_buffer_dirty(path->nodes[0]);
			btrfs_release_path(path);
		}
	}

	/* Add dir_item and dir_index */
	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_insert_dir_item(trans, root, name, namelen, parent_ino,
				    &key, type, ret_index);
	if (ret < 0)
		goto out;

	/* Update inode size of the parent inode */
	key.objectid = parent_ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, root, &key, path, 1, 1);
	if (ret)
		goto out;
	inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_inode_item);
	inode_size = btrfs_inode_size(path->nodes[0], inode_item);
	inode_size += namelen * 2;
	btrfs_set_inode_size(path->nodes[0], inode_item, inode_size);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	btrfs_release_path(path);

out:
	btrfs_free_path(path);
	if (ret == 0 && index)
		*index = ret_index;
	return ret;
}

int btrfs_add_orphan_item(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root, struct btrfs_path *path,
			  u64 ino)
{
	struct btrfs_key key;

	key.objectid = BTRFS_ORPHAN_OBJECTID;
	key.type = BTRFS_ORPHAN_ITEM_KEY;
	key.offset = ino;

	return btrfs_insert_empty_item(trans, root, path, &key, 0);
}

/*
 * Unlink an inode, which will remove its backref and corresponding dir_index/
 * dir_item if any of them exists.
 *
 * If an inode's nlink is reduced to 0 and 'add_orphan' is true, it will be
 * added to orphan inode and waiting to be deleted by next kernel mount.
 */
int btrfs_unlink(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		 u64 ino, u64 parent_ino, u64 index, const char *name,
		 int namelen, int add_orphan)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_inode_item *inode_item;
	struct btrfs_inode_ref *inode_ref;
	struct btrfs_dir_item *dir_item;
	u64 inode_size;
	u32 nlinks;
	int del_inode_ref = 0;
	int del_dir_item = 0;
	int del_dir_index = 0;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* check the ref and backref exists */
	inode_ref = btrfs_lookup_inode_ref(trans, root, path, name, namelen,
					   ino, parent_ino, 0);
	if (IS_ERR(inode_ref)) {
		ret = PTR_ERR(inode_ref);
		goto out;
	}
	if (inode_ref)
		del_inode_ref = 1;
	btrfs_release_path(path);

	dir_item = btrfs_lookup_dir_item(NULL, root, path, parent_ino,
					 name, namelen, 0);
	if (IS_ERR(dir_item)) {
		ret = PTR_ERR(dir_item);
		goto out;
	}
	if (dir_item)
		del_dir_item = 1;
	btrfs_release_path(path);

	dir_item = btrfs_lookup_dir_index(NULL, root, path, parent_ino,
					  name, namelen, index, 0);
	/*
	 * Since lookup_dir_index() will return -ENOENT when not found,
	 * we need to do extra check.
	 */
	if (IS_ERR(dir_item) && PTR_ERR(dir_item) == -ENOENT)
		dir_item = NULL;
	if (IS_ERR(dir_item)) {
		ret = PTR_ERR(dir_item);
		goto out;
	}
	if (dir_item)
		del_dir_index = 1;
	btrfs_release_path(path);

	if (!del_inode_ref && !del_dir_item && !del_dir_index) {
		/* All not found, shouldn't happen */
		ret = -ENOENT;
		goto out;
	}

	if (del_inode_ref) {
		/* Only decrease nlink when deleting inode_ref */
		key.objectid = ino;
		key.type = BTRFS_INODE_ITEM_KEY;
		key.offset = 0;
		ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret) {
			if (ret > 0)
				ret = -ENOENT;
			goto out;
		}
		inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
					    struct btrfs_inode_item);
		nlinks = btrfs_inode_nlink(path->nodes[0], inode_item);
		if (nlinks > 0)
			nlinks--;
		btrfs_set_inode_nlink(path->nodes[0], inode_item, nlinks);
		btrfs_mark_buffer_dirty(path->nodes[0]);
		btrfs_release_path(path);

		/* For nlinks == 0, add it to orphan list if needed */
		if (nlinks == 0 && add_orphan) {
			ret = btrfs_add_orphan_item(trans, root, path, ino);
			if (ret < 0)
				goto out;
			btrfs_mark_buffer_dirty(path->nodes[0]);
			btrfs_release_path(path);
		}

		ret = btrfs_del_inode_ref(trans, root, name, namelen, ino,
					  parent_ino, &index);
		if (ret < 0)
			goto out;
	}

	if (del_dir_index) {
		dir_item = btrfs_lookup_dir_index(trans, root, path,
						  parent_ino, name, namelen,
						  index, -1);
		if (IS_ERR(dir_item)) {
			ret = PTR_ERR(dir_item);
			goto out;
		}
		if (!dir_item) {
			ret = -ENOENT;
			goto out;
		}
		ret = btrfs_delete_one_dir_name(trans, root, path, dir_item);
		if (ret)
			goto out;
		btrfs_release_path(path);

		/* Update inode size of the parent inode */
		key.objectid = parent_ino;
		key.type = BTRFS_INODE_ITEM_KEY;
		key.offset = 0;
		ret = btrfs_search_slot(trans, root, &key, path, 1, 1);
		if (ret)
			goto out;
		inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
					    struct btrfs_inode_item);
		inode_size = btrfs_inode_size(path->nodes[0], inode_item);
		if (inode_size >= namelen)
			inode_size -= namelen;
		btrfs_set_inode_size(path->nodes[0], inode_item, inode_size);
		btrfs_mark_buffer_dirty(path->nodes[0]);
		btrfs_release_path(path);
	}

	if (del_dir_item) {
		dir_item = btrfs_lookup_dir_item(trans, root, path, parent_ino,
						 name, namelen, -1);
		if (IS_ERR(dir_item)) {
			ret = PTR_ERR(dir_item);
			goto out;
		}
		if (!dir_item) {
			ret = -ENOENT;
			goto out;
		}
		ret = btrfs_delete_one_dir_name(trans, root, path, dir_item);
		if (ret < 0)
			goto out;
		btrfs_release_path(path);

		/* Update inode size of the parent inode */
		key.objectid = parent_ino;
		key.type = BTRFS_INODE_ITEM_KEY;
		key.offset = 0;
		ret = btrfs_search_slot(trans, root, &key, path, 1, 1);
		if (ret)
			goto out;
		inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
					    struct btrfs_inode_item);
		inode_size = btrfs_inode_size(path->nodes[0], inode_item);
		if (inode_size >= namelen)
			inode_size -= namelen;
		btrfs_set_inode_size(path->nodes[0], inode_item, inode_size);
		btrfs_mark_buffer_dirty(path->nodes[0]);
		btrfs_release_path(path);
	}

out:
	btrfs_free_path(path);
	return ret;
}

/* Fill inode item with 'mode'. Uid/gid to root/root */
static void fill_inode_item(struct btrfs_trans_handle *trans,
			    struct btrfs_inode_item *inode_item,
			    u32 mode, u32 nlink)
{
	time_t now = time(NULL);

	btrfs_set_stack_inode_generation(inode_item, trans->transid);
	btrfs_set_stack_inode_uid(inode_item, 0);
	btrfs_set_stack_inode_gid(inode_item, 0);
	btrfs_set_stack_inode_size(inode_item, 0);
	btrfs_set_stack_inode_mode(inode_item, mode);
	btrfs_set_stack_inode_nlink(inode_item, nlink);
	btrfs_set_stack_timespec_sec(&inode_item->atime, now);
	btrfs_set_stack_timespec_nsec(&inode_item->atime, 0);
	btrfs_set_stack_timespec_sec(&inode_item->mtime, now);
	btrfs_set_stack_timespec_nsec(&inode_item->mtime, 0);
	btrfs_set_stack_timespec_sec(&inode_item->ctime, now);
	btrfs_set_stack_timespec_nsec(&inode_item->ctime, 0);
}

/*
 * Unlike kernel btrfs_new_inode(), we only create the INODE_ITEM, without
 * its backref.
 * The backref is added by btrfs_add_link().
 */
int btrfs_new_inode(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		u64 ino, u32 mode)
{
	struct btrfs_inode_item inode_item = {0};
	int ret = 0;

	fill_inode_item(trans, &inode_item, mode, 0);
	ret = btrfs_insert_inode(trans, root, ino, &inode_item);
	return ret;
}

/*
 * Change inode flags to given value
 */
int btrfs_change_inode_flags(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 ino, u64 flags)
{
	struct btrfs_inode_item *item;
	struct btrfs_path *path;
	struct btrfs_key key;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}
	if (ret < 0)
		goto out;

	item = btrfs_item_ptr(path->nodes[0], path->slots[0],
			      struct btrfs_inode_item);
	btrfs_set_inode_flags(path->nodes[0], item, flags);
	btrfs_mark_buffer_dirty(path->nodes[0]);
out:
	btrfs_free_path(path);
	return ret;
}

/*
 * Make a dir under the parent inode 'parent_ino' with 'name'
 * and 'mode', The owner will be root/root.
 */
int btrfs_mkdir(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		char *name, int namelen, u64 parent_ino, u64 *ino, int mode)
{
	struct btrfs_dir_item *dir_item;
	struct btrfs_path *path;
	u64 ret_ino = 0;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	if (ino && *ino)
		ret_ino = *ino;

	dir_item = btrfs_lookup_dir_item(NULL, root, path, parent_ino,
					 name, namelen, 0);
	if (IS_ERR(dir_item)) {
		ret = PTR_ERR(dir_item);
		goto out;
	}

	if (dir_item) {
		struct btrfs_key found_key;

		/*
		 * Already have conflicting name, check if it is a dir.
		 * Either way, no need to continue.
		 */
		btrfs_dir_item_key_to_cpu(path->nodes[0], dir_item, &found_key);
		ret_ino = found_key.objectid;
		if (btrfs_dir_type(path->nodes[0], dir_item) != BTRFS_FT_DIR)
			ret = -EEXIST;
		goto out;
	}

	if (!ret_ino)
		/*
		 * This is *UNSAFE* if some leaf is corrupted,
		 * only used as a fallback method. Caller should either
		 * ensure the fs is OK or pass ino with unused inode number.
		 */
		ret = btrfs_find_free_objectid(NULL, root, parent_ino,
					       &ret_ino);
	if (ret)
		goto out;
	ret = btrfs_new_inode(trans, root, ret_ino, mode | S_IFDIR);
	if (ret)
		goto out;
	ret = btrfs_add_link(trans, root, ret_ino, parent_ino, name, namelen,
			     BTRFS_FT_DIR, NULL, 1, 0);
	if (ret)
		goto out;
out:
	btrfs_free_path(path);
	if (ret == 0 && ino)
		*ino = ret_ino;
	return ret;
}

struct btrfs_root *btrfs_mksubvol(struct btrfs_root *root,
				  const char *base, u64 root_objectid,
				  bool convert)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *new_root = NULL;
	struct btrfs_path path;
	struct btrfs_inode_item *inode_item;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 dirid = btrfs_root_dirid(&root->root_item);
	u64 index = 2;
	char buf[BTRFS_NAME_LEN + 1]; /* for snprintf null */
	int len;
	int i;
	int ret;

	len = strlen(base);
	if (len == 0 || len > BTRFS_NAME_LEN)
		return NULL;

	btrfs_init_path(&path);
	key.objectid = dirid;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret <= 0) {
		error("search for DIR_INDEX dirid %llu failed: %d",
				(unsigned long long)dirid, ret);
		goto fail;
	}

	if (path.slots[0] > 0) {
		path.slots[0]--;
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.objectid == dirid && key.type == BTRFS_DIR_INDEX_KEY)
			index = key.offset + 1;
	}
	btrfs_release_path(&path);

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		error("unable to start transaction");
		goto fail;
	}

	key.objectid = dirid;
	key.offset = 0;
	key.type =  BTRFS_INODE_ITEM_KEY;

	ret = btrfs_lookup_inode(trans, root, &path, &key, 1);
	if (ret) {
		error("search for INODE_ITEM %llu failed: %d",
				(unsigned long long)dirid, ret);
		goto fail;
	}
	leaf = path.nodes[0];
	inode_item = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_inode_item);

	key.objectid = root_objectid;
	key.offset = (u64)-1;
	key.type = BTRFS_ROOT_ITEM_KEY;

	memcpy(buf, base, len);
	if (convert) {
		for (i = 0; i < 1024; i++) {
			ret = btrfs_insert_dir_item(trans, root, buf, len,
					dirid, &key, BTRFS_FT_DIR, index);
			if (ret != -EEXIST)
				break;
			len = snprintf(buf, ARRAY_SIZE(buf), "%s%d", base, i);
			if (len < 1 || len > BTRFS_NAME_LEN) {
				ret = -EINVAL;
				break;
			}
		}
	} else {
		ret = btrfs_insert_dir_item(trans, root, buf, len, dirid, &key,
					    BTRFS_FT_DIR, index);
	}
	if (ret)
		goto fail;

	btrfs_set_inode_size(leaf, inode_item, len * 2 +
			     btrfs_inode_size(leaf, inode_item));
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(&path);

	/* add the backref first */
	ret = btrfs_add_root_ref(trans, tree_root, root_objectid,
				 BTRFS_ROOT_BACKREF_KEY,
				 root->root_key.objectid,
				 dirid, index, buf, len);
	if (ret) {
		error("unable to add root backref for %llu: %d",
				root->root_key.objectid, ret);
		goto fail;
	}

	/* now add the forward ref */
	ret = btrfs_add_root_ref(trans, tree_root, root->root_key.objectid,
				 BTRFS_ROOT_REF_KEY, root_objectid,
				 dirid, index, buf, len);
	if (ret) {
		error("unable to add root ref for %llu: %d",
				root->root_key.objectid, ret);
		goto fail;
	}

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		error("transaction commit failed: %d", ret);
		goto fail;
	}

	new_root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(new_root)) {
		error("unable to fs read root: %lu", PTR_ERR(new_root));
		new_root = NULL;
	}
fail:
	btrfs_init_path(&path);
	return new_root;
}

/*
 * Walk the tree of allocated inodes and find a hole.
 */
int btrfs_find_free_objectid(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     u64 dirid, u64 *objectid)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	int ret;
	int slot = 0;
	u64 last_ino = 0;
	int start_found;
	struct extent_buffer *l;
	struct btrfs_key search_key;
	u64 search_start = dirid;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	search_start = root->last_inode_alloc;
	search_start = max((unsigned long long)search_start,
				BTRFS_FIRST_FREE_OBJECTID);
	search_key.objectid = search_start;
	search_key.offset = 0;
	search_key.type = 0;

	btrfs_init_path(path);
	start_found = 0;
	ret = btrfs_search_slot(trans, root, &search_key, path, 0, 0);
	if (ret < 0)
		goto error;

	if (path->slots[0] > 0)
		path->slots[0]--;

	while (1) {
		l = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(l)) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto error;
			if (!start_found) {
				*objectid = search_start;
				start_found = 1;
				goto found;
			}
			*objectid = last_ino > search_start ?
				last_ino : search_start;
			goto found;
		}
		btrfs_item_key_to_cpu(l, &key, slot);
		if (key.objectid >= search_start) {
			if (start_found) {
				if (last_ino < search_start)
					last_ino = search_start;
				if (key.objectid > last_ino) {
					*objectid = last_ino;
					goto found;
				}
			}
		}
		start_found = 1;
		last_ino = key.objectid + 1;
		path->slots[0]++;
	}
	// FIXME -ENOSPC
found:
	root->last_inode_alloc = *objectid;
	btrfs_free_path(path);
	BUG_ON(*objectid < search_start);
	return 0;
error:
	btrfs_free_path(path);
	return ret;
}
