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
 * License along with this program.
 */

#include "kerncompat.h"
#include "androidcompat.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <linux/limits.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <ftw.h>
#include "ctree.h"
#include "internal.h"
#include "disk-io.h"
#include "messages.h"
#include "transaction.h"
#include "utils.h"
#include "mkfs/rootdir.h"
#include "send-utils.h"

/*
 * This ignores symlinks with unreadable targets and subdirs that can't
 * be read.  It's a best-effort to give a rough estimate of the size of
 * a subdir.  It doesn't guarantee that prepopulating btrfs from this
 * tree won't still run out of space.
 */
static u64 global_total_size;
static u64 fs_block_size;

static u64 index_cnt = 2;

static int add_directory_items(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 objectid,
			       ino_t parent_inum, const char *name,
			       struct stat *st, int *dir_index_cnt)
{
	int ret;
	int name_len;
	struct btrfs_key location;
	u8 filetype = 0;

	name_len = strlen(name);

	location.objectid = objectid;
	location.offset = 0;
	location.type = BTRFS_INODE_ITEM_KEY;

	if (S_ISDIR(st->st_mode))
		filetype = BTRFS_FT_DIR;
	if (S_ISREG(st->st_mode))
		filetype = BTRFS_FT_REG_FILE;
	if (S_ISLNK(st->st_mode))
		filetype = BTRFS_FT_SYMLINK;
	if (S_ISSOCK(st->st_mode))
		filetype = BTRFS_FT_SOCK;
	if (S_ISCHR(st->st_mode))
		filetype = BTRFS_FT_CHRDEV;
	if (S_ISBLK(st->st_mode))
		filetype = BTRFS_FT_BLKDEV;
	if (S_ISFIFO(st->st_mode))
		filetype = BTRFS_FT_FIFO;

	ret = btrfs_insert_dir_item(trans, root, name, name_len,
				    parent_inum, &location,
				    filetype, index_cnt);
	if (ret)
		return ret;
	ret = btrfs_insert_inode_ref(trans, root, name, name_len,
				     objectid, parent_inum, index_cnt);
	*dir_index_cnt = index_cnt;
	index_cnt++;

	return ret;
}

static int fill_inode_item(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct btrfs_inode_item *dst, struct stat *src)
{
	u64 blocks = 0;
	u64 sectorsize = root->fs_info->sectorsize;

	/*
	 * btrfs_inode_item has some reserved fields
	 * and represents on-disk inode entry, so
	 * zero everything to prevent information leak
	 */
	memset(dst, 0, sizeof(*dst));

	btrfs_set_stack_inode_generation(dst, trans->transid);
	btrfs_set_stack_inode_size(dst, src->st_size);
	btrfs_set_stack_inode_nbytes(dst, 0);
	btrfs_set_stack_inode_block_group(dst, 0);
	btrfs_set_stack_inode_nlink(dst, src->st_nlink);
	btrfs_set_stack_inode_uid(dst, src->st_uid);
	btrfs_set_stack_inode_gid(dst, src->st_gid);
	btrfs_set_stack_inode_mode(dst, src->st_mode);
	btrfs_set_stack_inode_rdev(dst, 0);
	btrfs_set_stack_inode_flags(dst, 0);
	btrfs_set_stack_timespec_sec(&dst->atime, src->st_atime);
	btrfs_set_stack_timespec_nsec(&dst->atime, 0);
	btrfs_set_stack_timespec_sec(&dst->ctime, src->st_ctime);
	btrfs_set_stack_timespec_nsec(&dst->ctime, 0);
	btrfs_set_stack_timespec_sec(&dst->mtime, src->st_mtime);
	btrfs_set_stack_timespec_nsec(&dst->mtime, 0);
	btrfs_set_stack_timespec_sec(&dst->otime, 0);
	btrfs_set_stack_timespec_nsec(&dst->otime, 0);

	if (S_ISDIR(src->st_mode)) {
		btrfs_set_stack_inode_size(dst, 0);
		btrfs_set_stack_inode_nlink(dst, 1);
	}
	if (S_ISREG(src->st_mode)) {
		btrfs_set_stack_inode_size(dst, (u64)src->st_size);
		if (src->st_size <= BTRFS_MAX_INLINE_DATA_SIZE(root))
			btrfs_set_stack_inode_nbytes(dst, src->st_size);
		else {
			blocks = src->st_size / sectorsize;
			if (src->st_size % sectorsize)
				blocks += 1;
			blocks *= sectorsize;
			btrfs_set_stack_inode_nbytes(dst, blocks);
		}
	}
	if (S_ISLNK(src->st_mode))
		btrfs_set_stack_inode_nbytes(dst, src->st_size + 1);

	return 0;
}

static int directory_select(const struct direct *entry)
{
	if (entry->d_name[0] == '.' &&
		(entry->d_name[1] == 0 ||
		 (entry->d_name[1] == '.' && entry->d_name[2] == 0)))
		return 0;
	return 1;
}

static void free_namelist(struct direct **files, int count)
{
	int i;

	if (count < 0)
		return;

	for (i = 0; i < count; ++i)
		free(files[i]);
	free(files);
}

static u64 calculate_dir_inode_size(const char *dirname)
{
	int count, i;
	struct direct **files, *cur_file;
	u64 dir_inode_size = 0;

	count = scandir(dirname, &files, directory_select, NULL);

	for (i = 0; i < count; i++) {
		cur_file = files[i];
		dir_inode_size += strlen(cur_file->d_name);
	}

	free_namelist(files, count);

	dir_inode_size *= 2;
	return dir_inode_size;
}

static int add_inode_items(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct stat *st, const char *name,
			   u64 self_objectid,
			   struct btrfs_inode_item *inode_ret)
{
	int ret;
	struct btrfs_inode_item btrfs_inode;
	u64 objectid;
	u64 inode_size = 0;

	fill_inode_item(trans, root, &btrfs_inode, st);
	objectid = self_objectid;

	if (S_ISDIR(st->st_mode)) {
		inode_size = calculate_dir_inode_size(name);
		btrfs_set_stack_inode_size(&btrfs_inode, inode_size);
	}

	ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);

	*inode_ret = btrfs_inode;
	return ret;
}

static int add_xattr_item(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root, u64 objectid,
			  const char *file_name)
{
	int ret;
	int cur_name_len;
	char xattr_list[XATTR_LIST_MAX];
	char *cur_name;
	char cur_value[XATTR_SIZE_MAX];
	char delimiter = '\0';
	char *next_location = xattr_list;

	ret = llistxattr(file_name, xattr_list, XATTR_LIST_MAX);
	if (ret < 0) {
		if (errno == ENOTSUP)
			return 0;
		error("getting a list of xattr failed for %s: %s", file_name,
				strerror(errno));
		return ret;
	}
	if (ret == 0)
		return ret;

	cur_name = strtok(xattr_list, &delimiter);
	while (cur_name != NULL) {
		cur_name_len = strlen(cur_name);
		next_location += cur_name_len + 1;

		ret = getxattr(file_name, cur_name, cur_value, XATTR_SIZE_MAX);
		if (ret < 0) {
			if (errno == ENOTSUP)
				return 0;
			error("gettig a xattr value failed for %s attr %s: %s",
				file_name, cur_name, strerror(errno));
			return ret;
		}

		ret = btrfs_insert_xattr_item(trans, root, cur_name,
					      cur_name_len, cur_value,
					      ret, objectid);
		if (ret) {
			error("inserting a xattr item failed for %s: %s",
					file_name, strerror(-ret));
		}

		cur_name = strtok(next_location, &delimiter);
	}

	return ret;
}

static int add_symbolic_link(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     u64 objectid, const char *path_name)
{
	int ret;
	char buf[PATH_MAX];

	ret = readlink(path_name, buf, sizeof(buf));
	if (ret <= 0) {
		error("readlink failed for %s: %s", path_name, strerror(errno));
		goto fail;
	}
	if (ret >= sizeof(buf)) {
		error("symlink too long for %s", path_name);
		ret = -1;
		goto fail;
	}

	buf[ret] = '\0'; /* readlink does not do it for us */
	ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
					 buf, ret + 1);
fail:
	return ret;
}

static int add_file_items(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  struct btrfs_inode_item *btrfs_inode, u64 objectid,
			  struct stat *st, const char *path_name)
{
	int ret = -1;
	ssize_t ret_read;
	u64 bytes_read = 0;
	struct btrfs_key key;
	int blocks;
	u32 sectorsize = root->fs_info->sectorsize;
	u64 first_block = 0;
	u64 file_pos = 0;
	u64 cur_bytes;
	u64 total_bytes;
	struct extent_buffer *eb = NULL;
	int fd;

	if (st->st_size == 0)
		return 0;

	fd = open(path_name, O_RDONLY);
	if (fd == -1) {
		error("cannot open %s: %s", path_name, strerror(errno));
		return ret;
	}

	blocks = st->st_size / sectorsize;
	if (st->st_size % sectorsize)
		blocks += 1;

	if (st->st_size <= BTRFS_MAX_INLINE_DATA_SIZE(root)) {
		char *buffer = malloc(st->st_size);

		if (!buffer) {
			ret = -ENOMEM;
			goto end;
		}

		ret_read = pread64(fd, buffer, st->st_size, bytes_read);
		if (ret_read == -1) {
			error("cannot read %s at offset %llu length %llu: %s",
				path_name, (unsigned long long)bytes_read,
				(unsigned long long)st->st_size,
				strerror(errno));
			free(buffer);
			goto end;
		}

		ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
						 buffer, st->st_size);
		free(buffer);
		goto end;
	}

	/* round up our st_size to the FS blocksize */
	total_bytes = (u64)blocks * sectorsize;

	/*
	 * do our IO in extent buffers so it can work
	 * against any raid type
	 */
	eb = calloc(1, sizeof(*eb) + sectorsize);
	if (!eb) {
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

		memset(eb->data, 0, sectorsize);

		ret_read = pread64(fd, eb->data, sectorsize, file_pos +
				   bytes_read);
		if (ret_read == -1) {
			error("cannot read %s at offset %llu length %llu: %s",
				path_name,
				(unsigned long long)file_pos + bytes_read,
				(unsigned long long)sectorsize,
				strerror(errno));
			goto end;
		}

		eb->start = first_block + bytes_read;
		eb->len = sectorsize;

		/*
		 * we're doing the csum before we record the extent, but
		 * that's ok
		 */
		ret = btrfs_csum_file_block(trans, root->fs_info->csum_root,
				first_block + bytes_read + sectorsize,
				first_block + bytes_read,
				eb->data, sectorsize);
		if (ret)
			goto end;

		ret = write_and_map_eb(root->fs_info, eb);
		if (ret) {
			error("failed to write %s", path_name);
			goto end;
		}

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
	free(eb);
	close(fd);
	return ret;
}

static int traverse_directory(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, const char *dir_name,
			      struct directory_name_entry *dir_head)
{
	int ret = 0;

	struct btrfs_inode_item cur_inode;
	struct btrfs_inode_item *inode_item;
	int count, i, dir_index_cnt;
	struct direct **files;
	struct stat st;
	struct directory_name_entry *dir_entry, *parent_dir_entry;
	struct direct *cur_file;
	ino_t parent_inum, cur_inum;
	ino_t highest_inum = 0;
	const char *parent_dir_name;
	char real_path[PATH_MAX];
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key root_dir_key;
	u64 root_dir_inode_size = 0;

	/* Add list for source directory */
	dir_entry = malloc(sizeof(struct directory_name_entry));
	if (!dir_entry)
		return -ENOMEM;
	dir_entry->dir_name = dir_name;
	dir_entry->path = realpath(dir_name, real_path);
	if (!dir_entry->path) {
		error("realpath  failed for %s: %s", dir_name, strerror(errno));
		ret = -1;
		goto fail_no_dir;
	}

	parent_inum = highest_inum + BTRFS_FIRST_FREE_OBJECTID;
	dir_entry->inum = parent_inum;
	list_add_tail(&dir_entry->list, &dir_head->list);

	btrfs_init_path(&path);

	root_dir_key.objectid = btrfs_root_dirid(&root->root_item);
	root_dir_key.offset = 0;
	root_dir_key.type = BTRFS_INODE_ITEM_KEY;
	ret = btrfs_lookup_inode(trans, root, &path, &root_dir_key, 1);
	if (ret) {
		error("failed to lookup root dir: %d", ret);
		goto fail_no_dir;
	}

	leaf = path.nodes[0];
	inode_item = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_inode_item);

	root_dir_inode_size = calculate_dir_inode_size(dir_name);
	btrfs_set_inode_size(leaf, inode_item, root_dir_inode_size);
	btrfs_mark_buffer_dirty(leaf);

	btrfs_release_path(&path);

	do {
		parent_dir_entry = list_entry(dir_head->list.next,
					      struct directory_name_entry,
					      list);
		list_del(&parent_dir_entry->list);

		parent_inum = parent_dir_entry->inum;
		parent_dir_name = parent_dir_entry->dir_name;
		if (chdir(parent_dir_entry->path)) {
			error("chdir failed for %s: %s",
				parent_dir_name, strerror(errno));
			ret = -1;
			goto fail_no_files;
		}

		count = scandir(parent_dir_entry->path, &files,
				directory_select, NULL);
		if (count == -1) {
			error("scandir failed for %s: %s",
				parent_dir_name, strerror(errno));
			ret = -1;
			goto fail;
		}

		for (i = 0; i < count; i++) {
			cur_file = files[i];

			if (lstat(cur_file->d_name, &st) == -1) {
				error("lstat failed for %s: %s",
					cur_file->d_name, strerror(errno));
				ret = -1;
				goto fail;
			}

			cur_inum = st.st_ino;
			ret = add_directory_items(trans, root,
						  cur_inum, parent_inum,
						  cur_file->d_name,
						  &st, &dir_index_cnt);
			if (ret) {
				error("unable to add directory items for %s: %d",
					cur_file->d_name, ret);
				goto fail;
			}

			ret = add_inode_items(trans, root, &st,
					      cur_file->d_name, cur_inum,
					      &cur_inode);
			if (ret == -EEXIST) {
				if (st.st_nlink <= 1) {
					error(
			"item %s already exists but has wrong st_nlink %lu <= 1",
						cur_file->d_name,
						(unsigned long)st.st_nlink);
					goto fail;
				}
				continue;
			}
			if (ret) {
				error("unable to add inode items for %s: %d",
					cur_file->d_name, ret);
				goto fail;
			}

			ret = add_xattr_item(trans, root,
					     cur_inum, cur_file->d_name);
			if (ret) {
				error("unable to add xattr items for %s: %d",
					cur_file->d_name, ret);
				if (ret != -ENOTSUP)
					goto fail;
			}

			if (S_ISDIR(st.st_mode)) {
				char tmp[PATH_MAX];

				dir_entry = malloc(sizeof(*dir_entry));
				if (!dir_entry) {
					ret = -ENOMEM;
					goto fail;
				}
				dir_entry->dir_name = cur_file->d_name;
				if (path_cat_out(tmp, parent_dir_entry->path,
							cur_file->d_name)) {
					error("invalid path: %s/%s",
							parent_dir_entry->path,
							cur_file->d_name);
					ret = -EINVAL;
					goto fail;
				}
				dir_entry->path = strdup(tmp);
				if (!dir_entry->path) {
					error("not enough memory to store path");
					ret = -ENOMEM;
					goto fail;
				}
				dir_entry->inum = cur_inum;
				list_add_tail(&dir_entry->list,
					      &dir_head->list);
			} else if (S_ISREG(st.st_mode)) {
				ret = add_file_items(trans, root, &cur_inode,
						     cur_inum, &st,
						     cur_file->d_name);
				if (ret) {
					error("unable to add file items for %s: %d",
						cur_file->d_name, ret);
					goto fail;
				}
			} else if (S_ISLNK(st.st_mode)) {
				ret = add_symbolic_link(trans, root,
						cur_inum, cur_file->d_name);
				if (ret) {
					error("unable to add symlink for %s: %d",
						cur_file->d_name, ret);
					goto fail;
				}
			}
		}

		free_namelist(files, count);
		free(parent_dir_entry);

		index_cnt = 2;

	} while (!list_empty(&dir_head->list));

out:
	return !!ret;
fail:
	free_namelist(files, count);
fail_no_files:
	free(parent_dir_entry);
	goto out;
fail_no_dir:
	free(dir_entry);
	goto out;
}

int btrfs_mkfs_fill_dir(const char *source_dir, struct btrfs_root *root,
			bool verbose)
{
	int ret;
	struct btrfs_trans_handle *trans;
	struct stat root_st;
	struct directory_name_entry dir_head;
	struct directory_name_entry *dir_entry = NULL;

	ret = lstat(source_dir, &root_st);
	if (ret) {
		error("unable to lstat %s: %s", source_dir, strerror(errno));
		ret = -errno;
		goto out;
	}

	INIT_LIST_HEAD(&dir_head.list);

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	ret = traverse_directory(trans, root, source_dir, &dir_head);
	if (ret) {
		error("unable to traverse directory %s: %d", source_dir, ret);
		goto fail;
	}
	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		error("transaction commit failed: %d", ret);
		goto out;
	}

	if (verbose)
		printf("Making image is completed.\n");
	return 0;
fail:
	/*
	 * Since we don't have btrfs_abort_transaction() yet, uncommitted trans
	 * will trigger a BUG_ON().
	 *
	 * However before mkfs is fully finished, the magic number is invalid,
	 * so even we commit transaction here, the fs still can't be mounted.
	 *
	 * To do a graceful error out, here we commit transaction as a
	 * workaround.
	 * Since we have already hit some problem, the return value doesn't
	 * matter now.
	 */
	btrfs_commit_transaction(trans, root);
	while (!list_empty(&dir_head.list)) {
		dir_entry = list_entry(dir_head.list.next,
				       struct directory_name_entry, list);
		list_del(&dir_entry->list);
		free(dir_entry);
	}
out:
	return ret;
}

static int ftw_add_entry_size(const char *fpath, const struct stat *st,
			      int type)
{
	if (type == FTW_F || type == FTW_D)
		global_total_size += round_up(st->st_size, fs_block_size);

	return 0;
}

u64 btrfs_mkfs_size_dir(const char *dir_name, u64 sectorsize,
			u64 *num_of_meta_chunks_ret, u64 *size_of_data_ret)
{
	u64 dir_size = 0;
	u64 total_size = 0;
	int ret;
	u64 default_chunk_size = SZ_8M;
	u64 allocated_meta_size = SZ_8M;
	u64 allocated_total_size = 20 * SZ_1M;	/* 20MB */
	u64 num_of_meta_chunks = 0;
	u64 num_of_data_chunks = 0;
	u64 num_of_allocated_meta_chunks =
			allocated_meta_size / default_chunk_size;

	global_total_size = 0;
	fs_block_size = sectorsize;
	ret = ftw(dir_name, ftw_add_entry_size, 10);
	dir_size = global_total_size;
	if (ret < 0) {
		error("ftw subdir walk of %s failed: %s", dir_name,
			strerror(errno));
		exit(1);
	}

	num_of_data_chunks = (dir_size + default_chunk_size - 1) /
		default_chunk_size;

	num_of_meta_chunks = (dir_size / 2) / default_chunk_size;
	if (((dir_size / 2) % default_chunk_size) != 0)
		num_of_meta_chunks++;
	if (num_of_meta_chunks <= num_of_allocated_meta_chunks)
		num_of_meta_chunks = 0;
	else
		num_of_meta_chunks -= num_of_allocated_meta_chunks;

	total_size = allocated_total_size +
		     (num_of_data_chunks * default_chunk_size) +
		     (num_of_meta_chunks * default_chunk_size);

	*num_of_meta_chunks_ret = num_of_meta_chunks;
	*size_of_data_ret = num_of_data_chunks * default_chunk_size;
	return total_size;
}
