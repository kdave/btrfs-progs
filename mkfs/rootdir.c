/*
 * Copyright (C) 2017 SUSE.  All rights reserved.
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

#include "kerncompat.h"
#include <sys/stat.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <ftw.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "kernel-lib/sizes.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/file-item.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/extent-tree-utils.h"
#include "common/root-tree-utils.h"
#include "common/path-utils.h"
#include "common/rbtree-utils.h"
#include "mkfs/rootdir.h"

static u32 fs_block_size;

/*
 * Size estimate will be done using the following data:
 * 1) Number of inodes
 *    Since we will later shrink the fs, over-estimate is completely fine here
 *    as long as our estimate ensures we can populate the image without ENOSPC.
 *    So we only record how many inodes there are, and account the maximum
 *    space for each inode.
 *
 * 2) Data space for each (regular) inode
 *    To estimate data chunk size.
 *    Don't care if it can fit as an inline extent.
 *    Always round them up to sectorsize.
 */
static u64 ftw_meta_nr_inode;
static u64 ftw_data_size;

/*
 * Represent one inode inside the path.
 *
 * For now, all those inodes are inside fs tree.
 */
struct inode_entry {
	/* The inode number inside btrfs. */
	u64 ino;
	struct btrfs_root *root;
	struct list_head list;
};

/*
 * Record all the hard links we found for a specific file inside
 * rootdir.
 *
 * The search is based on (root, st_dev, st_ino).
 * The reason for @root as a search index is, for hard links separated by
 * subvolume boundaries:
 *
 * rootdir/
 * |- foobar_hardlink1
 * |- foobar_hardlink2
 * |- subv/	<- Will be created as a subvolume
 *    |- foobar_hardlink3.
 *
 * Since all the 3 hard links are inside the same rootdir and the same
 * filesystem, on the host fs they are all hard links to the same inode.
 *
 * But for the btrfs we are building, only hardlink1 and hardlink2 can be
 * created as hardlinks. Since we cannot create hardlink across subvolume.
 * So we need @root as a search index to handle such case.
 */
struct hardlink_entry {
	struct rb_node node;
	/*
	 * The following three members are reported from the stat() of the
	 * host filesystem.
	 *
	 * For st_nlink we cannot trust it unconditionally, as
	 * some hard links may be out of rootdir.
	 * If @found_nlink reached @st_nlink, we know we have created all
	 * the hard links and can remove the entry.
	 */
	dev_t st_dev;
	ino_t st_ino;
	nlink_t st_nlink;

	/* The following two are inside the new btrfs. */
	struct btrfs_root *root;
	u64 btrfs_ino;

	/* How many hard links we have created. */
	nlink_t found_nlink;
};

static struct rb_root hardlink_root = RB_ROOT;

/*
 * The path towards the rootdir.
 *
 * Only directory inodes are stored inside the path.
 */
struct rootdir_path {
	/*
	 * Level 0 means it's uninitialized
	 * Level 1 means it's the rootdir itself.
	 */
	int level;

	struct list_head inode_list;
};

static struct rootdir_path current_path = {
	.level = 0,
};

static struct btrfs_trans_handle *g_trans = NULL;
static struct list_head *g_subvols;
static u64 next_subvol_id = BTRFS_FIRST_FREE_OBJECTID;
static u64 default_subvol_id;

static inline struct inode_entry *rootdir_path_last(struct rootdir_path *path)
{
	UASSERT(!list_empty(&path->inode_list));

	return list_entry(path->inode_list.prev, struct inode_entry, list);
}

static void rootdir_path_pop(struct rootdir_path *path)
{
	struct inode_entry *last;

	UASSERT(path->level > 0);

	last = rootdir_path_last(path);
	list_del_init(&last->list);
	path->level--;
	free(last);
}

static int rootdir_path_push(struct rootdir_path *path, struct btrfs_root *root, u64 ino)
{
	struct inode_entry *new;

	new = malloc(sizeof(*new));
	if (!new)
		return -ENOMEM;
	new->root = root;
	new->ino = ino;
	list_add_tail(&new->list, &path->inode_list);
	path->level++;
	return 0;
}

static int hardlink_compare_nodes(const struct rb_node *node1,
				  const struct rb_node *node2)
{
	const struct hardlink_entry *entry1;
	const struct hardlink_entry *entry2;

	entry1 = rb_entry(node1, struct hardlink_entry, node);
	entry2 = rb_entry(node2, struct hardlink_entry, node);
	UASSERT(entry1->root);
	UASSERT(entry2->root);

	if (entry1->st_dev < entry2->st_dev)
		return -1;
	if (entry1->st_dev > entry2->st_dev)
		return 1;
	if (entry1->st_ino < entry2->st_ino)
		return -1;
	if (entry1->st_ino > entry2->st_ino)
		return 1;
	if (entry1->root < entry2->root)
		return -1;
	if (entry1->root > entry2->root)
		return 1;
	return 0;
}

static struct hardlink_entry *find_hard_link(struct btrfs_root *root,
					     const struct stat *st)
{
	struct rb_node *node;
	const struct hardlink_entry tmp = {
		.st_dev = st->st_dev,
		.st_ino = st->st_ino,
		.root = root,
	};

	node = rb_search(&hardlink_root, &tmp,
			(rb_compare_keys)hardlink_compare_nodes, NULL);
	if (node)
		return rb_entry(node, struct hardlink_entry, node);
	return NULL;
}

static int add_hard_link(struct btrfs_root *root, u64 btrfs_ino,
			 const struct stat *st)
{
	struct hardlink_entry *new;
	int ret;

	UASSERT(st->st_nlink > 1);

	new = calloc(1, sizeof(*new));
	if (!new)
		return -ENOMEM;

	new->root = root;
	new->btrfs_ino = btrfs_ino;
	new->found_nlink = 1;
	new->st_dev = st->st_dev;
	new->st_ino = st->st_ino;
	new->st_nlink = st->st_nlink;
	ret = rb_insert(&hardlink_root, &new->node, hardlink_compare_nodes);
	if (ret) {
		free(new);
		return -EEXIST;
	}
	return 0;
}

static void free_one_hardlink(struct rb_node *node)
{
	struct hardlink_entry *entry = rb_entry(node, struct hardlink_entry, node);

	free(entry);
}

static void stat_to_inode_item(struct btrfs_inode_item *dst, const struct stat *st)
{
	/*
	 * Do not touch size for directory inode, the size would be
	 * automatically updated during btrfs_link_inode().
	 */
	if (!S_ISDIR(st->st_mode))
		btrfs_set_stack_inode_size(dst, st->st_size);
	btrfs_set_stack_inode_nbytes(dst, 0);
	btrfs_set_stack_inode_block_group(dst, 0);
	btrfs_set_stack_inode_uid(dst, st->st_uid);
	btrfs_set_stack_inode_gid(dst, st->st_gid);
	btrfs_set_stack_inode_mode(dst, st->st_mode);
	btrfs_set_stack_inode_rdev(dst, 0);
	btrfs_set_stack_inode_flags(dst, 0);
	btrfs_set_stack_timespec_sec(&dst->atime, st->st_atime);
	btrfs_set_stack_timespec_nsec(&dst->atime, 0);
	btrfs_set_stack_timespec_sec(&dst->ctime, st->st_ctime);
	btrfs_set_stack_timespec_nsec(&dst->ctime, 0);
	btrfs_set_stack_timespec_sec(&dst->mtime, st->st_mtime);
	btrfs_set_stack_timespec_nsec(&dst->mtime, 0);
	btrfs_set_stack_timespec_sec(&dst->otime, 0);
	btrfs_set_stack_timespec_nsec(&dst->otime, 0);
}

static int add_xattr_item(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root, u64 objectid,
			  const char *file_name)
{
	int ret;
	int cur_name_len;
	char xattr_list[XATTR_LIST_MAX];
	char *xattr_list_end;
	char *cur_name;
	char cur_value[XATTR_SIZE_MAX];

	ret = llistxattr(file_name, xattr_list, XATTR_LIST_MAX);
	if (ret < 0) {
		if (errno == ENOTSUP)
			return 0;
		error("getting a list of xattr failed for %s: %m", file_name);
		return ret;
	}
	if (ret == 0)
		return ret;

	xattr_list_end = xattr_list + ret;
	cur_name = xattr_list;
	while (cur_name < xattr_list_end) {
		cur_name_len = strlen(cur_name);

		ret = lgetxattr(file_name, cur_name, cur_value, XATTR_SIZE_MAX);
		if (ret < 0) {
			if (errno == ENOTSUP)
				return 0;
			error("getting a xattr value failed for %s attr %s: %m",
				file_name, cur_name);
			return ret;
		}

		ret = btrfs_insert_xattr_item(trans, root, cur_name,
					      cur_name_len, cur_value,
					      ret, objectid);
		if (ret) {
			errno = -ret;
			error("inserting a xattr item failed for %s: %m",
					file_name);
		}

		cur_name += cur_name_len + 1;
	}

	return ret;
}

static int add_symbolic_link(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     struct btrfs_inode_item *inode_item,
			     u64 objectid, const char *path_name)
{
	u64 nbytes;
	int ret;
	char buf[PATH_MAX];

	ret = readlink(path_name, buf, sizeof(buf));
	if (ret <= 0) {
		error("readlink failed for %s: %m", path_name);
		goto fail;
	}
	if (ret >= sizeof(buf)) {
		error("symlink too long for %s", path_name);
		ret = -1;
		goto fail;
	}

	buf[ret] = '\0'; /* readlink does not do it for us */
	nbytes = ret + 1;
	ret = btrfs_insert_inline_extent(trans, root, objectid, 0, buf, nbytes);
	if (ret < 0) {
		errno = -ret;
		error("failed to insert inline extent for %s: %m", path_name);
		goto fail;
	}
	btrfs_set_stack_inode_nbytes(inode_item, nbytes);
fail:
	return ret;
}

static int add_file_items(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  struct btrfs_inode_item *btrfs_inode, u64 objectid,
			  const struct stat *st, const char *path_name)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	int ret = -1;
	ssize_t ret_read;
	u64 bytes_read = 0;
	struct btrfs_key key;
	int blocks;
	u32 sectorsize = fs_info->sectorsize;
	u64 first_block = 0;
	u64 file_pos = 0;
	u64 cur_bytes;
	u64 total_bytes;
	void *buf = NULL;
	int fd;

	if (st->st_size == 0)
		return 0;

	fd = open(path_name, O_RDONLY);
	if (fd == -1) {
		error("cannot open %s: %m", path_name);
		return ret;
	}

	blocks = st->st_size / sectorsize;
	if (st->st_size % sectorsize)
		blocks += 1;

	if (st->st_size <= BTRFS_MAX_INLINE_DATA_SIZE(fs_info) &&
	    st->st_size < sectorsize) {
		char *buffer = malloc(st->st_size);

		if (!buffer) {
			ret = -ENOMEM;
			goto end;
		}

		ret_read = pread(fd, buffer, st->st_size, bytes_read);
		if (ret_read == -1) {
			error("cannot read %s at offset %llu length %llu: %m",
				path_name, bytes_read, (unsigned long long)st->st_size);
			free(buffer);
			goto end;
		}

		ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
						 buffer, st->st_size);
		free(buffer);
		/* Update the inode nbytes for inline extents. */
		btrfs_set_stack_inode_nbytes(btrfs_inode, st->st_size);
		goto end;
	}

	/* round up our st_size to the FS blocksize */
	total_bytes = (u64)blocks * sectorsize;

	buf = malloc(sectorsize);
	if (!buf) {
		ret = -ENOMEM;
		goto end;
	}

again:

	/*
	 * keep our extent size at 1MB max, this makes it easier to work inside
	 * the tiny block groups created during mkfs
	 */
	cur_bytes = min(total_bytes, (u64)SZ_1M);
	ret = btrfs_reserve_extent(trans, root, cur_bytes, 0, 0, (u64)-1,
				   &key, 1);
	if (ret)
		goto end;

	first_block = key.objectid;
	bytes_read = 0;

	while (bytes_read < cur_bytes) {

		memset(buf, 0, sectorsize);

		ret_read = pread(fd, buf, sectorsize, file_pos + bytes_read);
		if (ret_read == -1) {
			error("cannot read %s at offset %llu length %u: %m",
				path_name, file_pos + bytes_read, sectorsize);
			goto end;
		}

		ret = write_data_to_disk(root->fs_info, buf,
					 first_block + bytes_read, sectorsize);
		if (ret) {
			error("failed to write %s", path_name);
			goto end;
		}

		ret = btrfs_csum_file_block(trans, first_block + bytes_read,
					    BTRFS_EXTENT_CSUM_OBJECTID,
					    fs_info->csum_type, buf);
		if (ret)
			goto end;

		bytes_read += sectorsize;
	}

	if (bytes_read) {
		ret = btrfs_record_file_extent(trans, root, objectid,
				btrfs_inode, file_pos, first_block, cur_bytes);
		if (ret)
			goto end;

	}

	file_pos += cur_bytes;
	total_bytes -= cur_bytes;

	if (total_bytes)
		goto again;

end:
	free(buf);
	close(fd);
	return ret;
}

static int update_inode_item(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     const struct btrfs_inode_item *inode_item,
			     u64 ino)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key = {
		.objectid = ino,
		.type = BTRFS_INODE_ITEM_KEY,
		.offset = 0,
	};
	u32 item_ptr_off;
	int ret;

	ret = btrfs_lookup_inode(trans, root, &path, &key, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}
	item_ptr_off = btrfs_item_ptr_offset(path.nodes[0], path.slots[0]);
	write_extent_buffer(path.nodes[0], inode_item, item_ptr_off, sizeof(*inode_item));
	btrfs_mark_buffer_dirty(path.nodes[0]);
	btrfs_release_path(&path);
	return 0;
}

static u8 ftype_to_btrfs_type(mode_t ftype)
{
	if (S_ISREG(ftype))
		return BTRFS_FT_REG_FILE;
	if (S_ISDIR(ftype))
		return BTRFS_FT_DIR;
	if (S_ISLNK(ftype))
		return BTRFS_FT_SYMLINK;
	if (S_ISCHR(ftype))
		return BTRFS_FT_CHRDEV;
	if (S_ISBLK(ftype))
		return BTRFS_FT_BLKDEV;
	if (S_ISFIFO(ftype))
		return BTRFS_FT_FIFO;
	if (S_ISSOCK(ftype))
		return BTRFS_FT_SOCK;
	return BTRFS_FT_UNKNOWN;
}

static int ftw_add_subvol(const char *full_path, const struct stat *st,
			  int typeflag, struct FTW *ftwbuf,
			  struct rootdir_subvol *subvol)
{
	int ret;
	struct btrfs_key key;
	struct btrfs_root *new_root;
	struct inode_entry *parent;
	struct btrfs_inode_item inode_item = { 0 };
	u64 subvol_id, ino;

	subvol_id = next_subvol_id++;

	ret = btrfs_make_subvolume(g_trans, subvol_id, subvol->readonly);
	if (ret < 0) {
		errno = -ret;
		error("failed to create subvolume: %m");
		return ret;
	}

	if (subvol->is_default)
		default_subvol_id = subvol_id;

	key.objectid = subvol_id;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	new_root = btrfs_read_fs_root(g_trans->fs_info, &key);
	if (IS_ERR(new_root)) {
		ret = PTR_ERR(new_root);
		errno = -ret;
		error("unable to read fs root id %llu: %m", subvol_id);
		return ret;
	}

	parent = rootdir_path_last(&current_path);

	ret = btrfs_link_subvolume(g_trans, parent->root, parent->ino,
				   path_basename(subvol->full_path),
				   strlen(path_basename(subvol->full_path)),
				   new_root);
	if (ret) {
		errno = -ret;
		error("unable to link subvolume %s: %m", path_basename(subvol->full_path));
		return ret;
	}

	ino = btrfs_root_dirid(&new_root->root_item);

	ret = add_xattr_item(g_trans, new_root, ino, full_path);
	if (ret < 0) {
		errno = -ret;
		error("failed to add xattr item for the top level inode in subvol %llu: %m",
		      subvol_id);
		return ret;
	}
	stat_to_inode_item(&inode_item, st);

	btrfs_set_stack_inode_nlink(&inode_item, 1);
	ret = update_inode_item(g_trans, new_root, &inode_item, ino);
	if (ret < 0) {
		errno = -ret;
		error("failed to update root dir for root %llu: %m", subvol_id);
		return ret;
	}

	ret = rootdir_path_push(&current_path, new_root, ino);
	if (ret < 0) {
		errno = -ret;
		error("failed to allocate new entry for subvolume %llu ('%s'): %m",
		      subvol_id, full_path);
		return ret;
	}

	return 0;
}

static int ftw_add_inode(const char *full_path, const struct stat *st,
			 int typeflag, struct FTW *ftwbuf)
{
	struct btrfs_fs_info *fs_info = g_trans->fs_info;
	struct btrfs_root *root;
	struct btrfs_inode_item inode_item = { 0 };
	struct inode_entry *parent;
	struct rootdir_subvol *rds;
	const bool have_hard_links = (!S_ISDIR(st->st_mode) && st->st_nlink > 1);
	u64 ino;
	int ret;

	/* The rootdir itself. */
	if (unlikely(ftwbuf->level == 0)) {
		u64 root_ino;

		root = fs_info->fs_root;
		root_ino = btrfs_root_dirid(&root->root_item);

		UASSERT(S_ISDIR(st->st_mode));
		UASSERT(current_path.level == 0);

		ret = add_xattr_item(g_trans, root, root_ino, full_path);
		if (ret < 0) {
			errno = -ret;
			error("failed to add xattr item for the top level inode: %m");
			return ret;
		}
		stat_to_inode_item(&inode_item, st);
		/*
		 * Rootdir inode exists without any parent, thus needs to set
		 * its nlink to 1 manually.
		 */
		btrfs_set_stack_inode_nlink(&inode_item, 1);
		ret = update_inode_item(g_trans, root, &inode_item, root_ino);
		if (ret < 0) {
			errno = -ret;
			error("failed to update root dir for root %llu: %m",
			      root->root_key.objectid);
			return ret;
		}

		/* Push (and initialize) the rootdir directory into the stack. */
		ret = rootdir_path_push(&current_path, root, btrfs_root_dirid(&root->root_item));
		if (ret < 0) {
			errno = -ret;
			error_msg(ERROR_MSG_MEMORY, "push path for rootdir: %m");
			return ret;
		}
		return ret;
	}

	/*
	 * The rootdir_path structure works like this, with the layout:
	 *
	 * rootdir/
	 * |- file1
	 * |- dir1
	 * |  |- file2
	 * |- file3
	 *
	 * nftw() would results the following sequence:
	 *
	 * - "rootdir"			level=0 empty stack (level 0).
	 *   The initial push. Our rootpath stack has nothing.
	 *   So we push the ino of rootdir (btrfs ino 256) into the stack.
	 *
	 * - "rootdir/dir1"		level=1 stack=256 (level 1)
	 *   nftw() is pre-order traversal, and it always visit
	 *   directory first.
	 *   We find it's a directory, knowing we will visit the
	 *   child inodes of it.
	 *   So we push the inode (btrfs ino 257) into the stack.
	 *
	 * - "rootdir/dir1/file2"	level=2 stack=256,257 (level 2)
	 *   This is a regular file, we do not need to change our stack.
	 *
	 * - "rootdir/file1"		level=1 stack=256,257 (level 2)
	 *   Level changed, we enter the upper level directory.
	 *   Pop the stack to match the parent inode.
	 *
	 * - "rootdir/file3"		level=1 stack=256 (level 1)
	 *
	 * So if our stack level > current ftw level, it means we
	 * have changed to a (one or more levels) upper directory,
	 * thus we need to pop the path until we reach the correct
	 * parent.
	 */
	while (current_path.level > ftwbuf->level)
		rootdir_path_pop(&current_path);

	if (S_ISDIR(st->st_mode)) {
		list_for_each_entry(rds, g_subvols, list) {
			if (!strcmp(full_path, rds->full_path)) {
				ret = ftw_add_subvol(full_path, st, typeflag,
						     ftwbuf, rds);

				free(rds->dir);
				free(rds->full_path);

				list_del(&rds->list);
				free(rds);

				return ret;
			}
		}
	}

	parent = rootdir_path_last(&current_path);
	root = parent->root;

	/* Check if there is already a hard link record for this. */
	if (have_hard_links) {
		struct hardlink_entry *found;

		found = find_hard_link(root, st);
		/*
		 * Can only add the hard link if it doesn't cross subvolume
		 * boundary.
		 */
		if (found && found->root == root) {
			ret = btrfs_add_link(g_trans, root, found->btrfs_ino,
					     parent->ino, full_path + ftwbuf->base,
					     strlen(full_path) - ftwbuf->base,
					     ftype_to_btrfs_type(st->st_mode),
					     NULL, 1, 0);
			if (ret < 0) {
				errno = -ret;
				error(
			"failed to add link for hard link ('%s'): %m", full_path);
				return ret;
			}
			found->found_nlink++;
			/* We found all hard links for it. Can remove the entry. */
			if (found->found_nlink >= found->st_nlink) {
				rb_erase(&found->node, &hardlink_root);
				free(found);
			}
			return 0;
		}
	}

	ret = btrfs_find_free_objectid(g_trans, root,
				       BTRFS_FIRST_FREE_OBJECTID, &ino);
	if (ret < 0) {
		errno = -ret;
		error("failed to find free objectid for file %s: %m", full_path);
		return ret;
	}
	stat_to_inode_item(&inode_item, st);

	ret = btrfs_insert_inode(g_trans, root, ino, &inode_item);
	if (ret < 0) {
		errno = -ret;
		error("failed to insert inode item %llu for '%s': %m", ino, full_path);
		return ret;
	}

	ret = btrfs_add_link(g_trans, root, ino, parent->ino,
			     full_path + ftwbuf->base,
			     strlen(full_path) - ftwbuf->base,
			     ftype_to_btrfs_type(st->st_mode),
			     NULL, 1, 0);
	if (ret < 0) {
		errno = -ret;
		error("failed to add link for inode %llu ('%s'): %m", ino, full_path);
		return ret;
	}

	/* Record this new hard link. */
	if (have_hard_links) {
		ret = add_hard_link(root, ino, st);
		if (ret < 0) {
			errno = -ret;
			error("failed to add hard link record for '%s': %m",
			       full_path);
			return ret;
		}
		ret = 0;
	}

	/*
	 * btrfs_add_link() has increased the nlink to 1 in the metadata.
	 * Also update the value in case we need to update the inode item
	 * later.
	 */
	btrfs_set_stack_inode_nlink(&inode_item, 1);

	ret = add_xattr_item(g_trans, root, ino, full_path);
	if (ret < 0) {
		errno = -ret;
		error("failed to add xattrs for inode %llu ('%s'): %m", ino, full_path);
		return ret;
	}
	if (S_ISDIR(st->st_mode)) {
		ret = rootdir_path_push(&current_path, root, ino);
		if (ret < 0) {
			errno = -ret;
			error("failed to allocate new entry for inode %llu ('%s'): %m",
				ino, full_path);
			return ret;
		}
	} else if (S_ISREG(st->st_mode)) {
		ret = add_file_items(g_trans, root, &inode_item, ino, st, full_path);
		if (ret < 0) {
			errno = -ret;
			error("failed to add file extents for inode %llu ('%s'): %m",
				ino, full_path);
			return ret;
		}
		ret = update_inode_item(g_trans, root, &inode_item, ino);
		if (ret < 0) {
			errno = -ret;
			error("failed to update inode item for inode %llu ('%s'): %m",
				ino, full_path);
			return ret;
		}
	} else if (S_ISLNK(st->st_mode)) {
		ret = add_symbolic_link(g_trans, root, &inode_item, ino, full_path);
		if (ret < 0) {
			errno = -ret;
			error("failed to insert link for inode %llu ('%s'): %m",
				ino, full_path);
			return ret;
		}
		ret = update_inode_item(g_trans, root, &inode_item, ino);
		if (ret < 0) {
			errno = -ret;
			error("failed to update inode item for inode %llu ('%s'): %m",
				ino, full_path);
			return ret;
		}
	}
	return 0;
};

static int set_default_subvolume(struct btrfs_trans_handle *trans)
{
	struct btrfs_path path = { 0 };
	struct btrfs_dir_item *di;
	struct btrfs_key location;
	struct extent_buffer *leaf;
	struct btrfs_disk_key disk_key;
	u64 features;

	di = btrfs_lookup_dir_item(trans, trans->fs_info->tree_root, &path,
				   btrfs_super_root_dir(trans->fs_info->super_copy),
				   "default", 7, 1);
	if (IS_ERR_OR_NULL(di)) {
		btrfs_release_path(&path);

		if (di)
			return PTR_ERR(di);
		else
			return -ENOENT;
	}

	leaf = path.nodes[0];

	location.objectid = default_subvol_id;
	location.type = BTRFS_ROOT_ITEM_KEY;
	location.offset = 0;

	btrfs_cpu_key_to_disk(&disk_key, &location);
	btrfs_set_dir_item_key(leaf, di, &disk_key);

	btrfs_mark_buffer_dirty(leaf);

	btrfs_release_path(&path);

	features = btrfs_super_incompat_flags(trans->fs_info->super_copy);
	features |= BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL;
	btrfs_set_super_incompat_flags(trans->fs_info->super_copy, features);

	return 0;
}

int btrfs_mkfs_fill_dir(struct btrfs_trans_handle *trans, const char *source_dir,
			struct btrfs_root *root, struct list_head *subvols)
{
	int ret;
	struct stat root_st;

	ret = lstat(source_dir, &root_st);
	if (ret) {
		error("unable to lstat %s: %m", source_dir);
		return -errno;
	}

	g_trans = trans;
	g_subvols = subvols;
	INIT_LIST_HEAD(&current_path.inode_list);

	ret = nftw(source_dir, ftw_add_inode, 32, FTW_PHYS);
	if (ret) {
		error("unable to traverse directory %s: %d", source_dir, ret);
		return ret;
	}

	while (current_path.level > 0)
		rootdir_path_pop(&current_path);

	if (default_subvol_id != 0) {
		ret = set_default_subvolume(trans);
		if (ret < 0) {
			error("error setting default subvolume: %d", ret);
			return ret;
		}
	}

	rb_free_nodes(&hardlink_root, free_one_hardlink);
	return 0;
}

static int ftw_add_entry_size(const char *fpath, const struct stat *st,
			      int type, struct FTW *ftwbuf)
{
	/*
	 * Failed to read the directory, mostly due to EPERM.  Abort ASAP, so
	 * we don't need to populate the fs.
	 */
	if (type == FTW_DNR || type == FTW_NS)
		return -EPERM;

	if (S_ISREG(st->st_mode))
		ftw_data_size += round_up(st->st_size, fs_block_size);
	ftw_meta_nr_inode++;

	return 0;
}

u64 btrfs_mkfs_size_dir(const char *dir_name, u32 sectorsize, u64 min_dev_size,
			u64 meta_profile, u64 data_profile)
{
	u64 total_size = 0;
	int ret;

	u64 meta_size = 0;		/* Based on @ftw_meta_nr_inode */
	u64 meta_chunk_size = 0;	/* Based on @meta_size */
	u64 data_chunk_size = 0;	/* Based on @ftw_data_size */

	u64 meta_threshold = SZ_8M;
	u64 data_threshold = SZ_8M;

	float data_multiplier = 1;
	float meta_multiplier = 1;

	fs_block_size = sectorsize;
	ftw_data_size = 0;
	ftw_meta_nr_inode = 0;

	/*
	 * Symbolic link is not followed when creating files, so no need to
	 * follow them here.
	 */
	ret = nftw(dir_name, ftw_add_entry_size, 10, FTW_PHYS);
	if (ret < 0) {
		error("ftw subdir walk of %s failed: %m", dir_name);
		exit(1);
	}


	/*
	 * Maximum metadata usage for every inode, which will be PATH_MAX
	 * for the following items:
	 * 1) DIR_ITEM
	 * 2) DIR_INDEX
	 * 3) INODE_REF
	 *
	 * Plus possible inline extent size, which is sectorsize.
	 *
	 * And finally, allow metadata usage to increase with data size.
	 * Follow the old kernel 8:1 data:meta ratio.
	 * This is especially important for --rootdir, as the file extent size
	 * upper limit is 1M, instead of 128M in kernel.
	 * This can bump meta usage easily.
	 */
	meta_size = ftw_meta_nr_inode * (PATH_MAX * 3 + sectorsize) +
		    ftw_data_size / 8;

	/* Minimal chunk size from btrfs_alloc_chunk(). */
	if (meta_profile & BTRFS_BLOCK_GROUP_DUP) {
		meta_threshold = SZ_32M;
		meta_multiplier = 2;
	}
	if (data_profile & BTRFS_BLOCK_GROUP_DUP) {
		data_threshold = SZ_64M;
		data_multiplier = 2;
	}

	/*
	 * Only when the usage is larger than the minimal chunk size (threshold)
	 * we need to allocate new chunk, or the initial chunk in the image is
	 * large enough.
	 */
	if (meta_size > meta_threshold)
		meta_chunk_size = (round_up(meta_size, meta_threshold) -
				   meta_threshold) * meta_multiplier;
	if (ftw_data_size > data_threshold)
		data_chunk_size = (round_up(ftw_data_size, data_threshold) -
				   data_threshold) * data_multiplier;

	total_size = data_chunk_size + meta_chunk_size + min_dev_size;
	return total_size;
}

/*
 * Get the end position of the last device extent for given @devid;
 * @size_ret is exclusive (means it should be aligned to sectorsize)
 */
static int get_device_extent_end(struct btrfs_fs_info *fs_info,
				 u64 devid, u64 *size_ret)
{
	struct btrfs_root *dev_root = fs_info->dev_root;
	struct btrfs_key key;
	struct btrfs_path path = { 0 };
	struct btrfs_dev_extent *de;
	int ret;

	key.objectid = devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, dev_root, &key, &path, 0, 0);
	if (ret == 0) {
		error("DEV_EXTENT for devid %llu not found", devid);
		ret = -EUCLEAN;
		goto out;
	}

	ret = btrfs_previous_item(dev_root, &path, devid, BTRFS_DEV_EXTENT_KEY);
	if (ret < 0)
		goto out;

	/* No dev_extent at all, not really possible for rootdir case */
	if (ret > 0) {
		*size_ret = 0;
		ret = -EUCLEAN;
		goto out;
	}

	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	de = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_dev_extent);
	*size_ret = key.offset + btrfs_dev_extent_length(path.nodes[0], de);
out:
	btrfs_release_path(&path);

	return ret;
}

/*
 * Set device size to @new_size.
 *
 * Only used for --rootdir option.
 * We will need to reset the following values:
 * 1) dev item in chunk tree
 * 2) super->dev_item
 * 3) super->total_bytes
 */
static int set_device_size(struct btrfs_fs_info *fs_info,
			   struct btrfs_device *device, u64 new_size)
{
	struct btrfs_root *chunk_root = fs_info->chunk_root;
	struct btrfs_trans_handle *trans;
	struct btrfs_dev_item *di;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int ret;

	/*
	 * Update in-memory device->total_bytes, so that at trans commit time,
	 * super->dev_item will also get updated
	 */
	device->total_bytes = new_size;

	/* Update device item in chunk tree */
	trans = btrfs_start_transaction(chunk_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}
	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = device->devid;

	ret = btrfs_search_slot(trans, chunk_root, &key, &path, 0, 1);
	if (ret < 0)
		goto err;
	if (ret > 0)
		ret = -ENOENT;
	di = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_dev_item);
	btrfs_set_device_total_bytes(path.nodes[0], di, new_size);
	btrfs_mark_buffer_dirty(path.nodes[0]);

	/*
	 * Update super->total_bytes, since it's only used for --rootdir,
	 * there is only one device, just use the @new_size.
	 */
	btrfs_set_super_total_bytes(fs_info->super_copy, new_size);

	/*
	 * Commit transaction to reflect the updated super->total_bytes and
	 * super->dev_item
	 */
	ret = btrfs_commit_transaction(trans, chunk_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
	}
	btrfs_release_path(&path);
	return ret;

err:
	btrfs_release_path(&path);
	/*
	 * Committing the transaction here won't cause problems since the fs
	 * still has an invalid magic number, and something wrong already
	 * happened, we don't care the return value anyway.
	 */
	btrfs_commit_transaction(trans, chunk_root);
	return ret;
}

int btrfs_mkfs_shrink_fs(struct btrfs_fs_info *fs_info, u64 *new_size_ret,
			 bool shrink_file_size)
{
	u64 new_size;
	struct btrfs_device *device;
	struct list_head *cur;
	struct stat file_stat;
	int nr_devs = 0;
	int ret;

	list_for_each(cur, &fs_info->fs_devices->devices)
		nr_devs++;

	if (nr_devs > 1) {
		error("cannot shrink fs with more than 1 device");
		return -ENOTTY;
	}

	ret = get_device_extent_end(fs_info, 1, &new_size);
	if (ret < 0) {
		errno = -ret;
		error("failed to get minimal device size: %d (%m)", ret);
		return ret;
	}

	if (!IS_ALIGNED(new_size, fs_info->sectorsize)) {
		error("shrunk filesystem size %llu not aligned to %u",
				new_size, fs_info->sectorsize);
		return -EUCLEAN;
	}

	device = list_entry(fs_info->fs_devices->devices.next,
			   struct btrfs_device, dev_list);
	ret = set_device_size(fs_info, device, new_size);
	if (ret < 0)
		return ret;
	if (new_size_ret)
		*new_size_ret = new_size;

	if (shrink_file_size) {
		ret = fstat(device->fd, &file_stat);
		if (ret < 0) {
			error("failed to stat devid %llu: %m", device->devid);
			return ret;
		}
		if (!S_ISREG(file_stat.st_mode))
			return ret;
		ret = ftruncate(device->fd, new_size);
		if (ret < 0) {
			error("failed to truncate device file of devid %llu: %m",
				device->devid);
			return ret;
		}
	}
	return ret;
}
