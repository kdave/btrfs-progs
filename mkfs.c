/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
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

#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <ctype.h>
#include <sys/xattr.h>
#include <limits.h>
#include <linux/limits.h>
#include <blkid/blkid.h>
#include <ftw.h>
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "utils.h"
#include "version.h"

static u64 index_cnt = 2;

#define DEFAULT_MKFS_FEATURES	(BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF \
		| BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA)

#define DEFAULT_MKFS_LEAF_SIZE 16384

struct directory_name_entry {
	char *dir_name;
	char *path;
	ino_t inum;
	struct list_head list;
};

static int make_root_dir(struct btrfs_root *root, int mixed)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key location;
	u64 bytes_used;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	bytes_used = btrfs_super_bytes_used(root->fs_info->super_copy);

	root->fs_info->system_allocs = 1;
	ret = btrfs_make_block_group(trans, root, bytes_used,
				     BTRFS_BLOCK_GROUP_SYSTEM,
				     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     0, BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	BUG_ON(ret);

	if (mixed) {
		ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_METADATA |
					BTRFS_BLOCK_GROUP_DATA);
		if (ret == -ENOSPC) {
			fprintf(stderr,
				"no space to alloc data/metadata chunk\n");
			goto err;
		}
		BUG_ON(ret);
		ret = btrfs_make_block_group(trans, root, 0,
					     BTRFS_BLOCK_GROUP_METADATA |
					     BTRFS_BLOCK_GROUP_DATA,
					     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
					     chunk_start, chunk_size);
		BUG_ON(ret);
		printf("Created a data/metadata chunk of size %llu\n", chunk_size);
	} else {
		ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_METADATA);
		if (ret == -ENOSPC) {
			fprintf(stderr, "no space to alloc metadata chunk\n");
			goto err;
		}
		BUG_ON(ret);
		ret = btrfs_make_block_group(trans, root, 0,
					     BTRFS_BLOCK_GROUP_METADATA,
					     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
					     chunk_start, chunk_size);
		BUG_ON(ret);
	}

	root->fs_info->system_allocs = 0;
	btrfs_commit_transaction(trans, root);
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);

	if (!mixed) {
		ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_DATA);
		if (ret == -ENOSPC) {
			fprintf(stderr, "no space to alloc data chunk\n");
			goto err;
		}
		BUG_ON(ret);
		ret = btrfs_make_block_group(trans, root, 0,
					     BTRFS_BLOCK_GROUP_DATA,
					     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
					     chunk_start, chunk_size);
		BUG_ON(ret);
	}

	ret = btrfs_make_root_dir(trans, root->fs_info->tree_root,
			      BTRFS_ROOT_TREE_DIR_OBJECTID);
	if (ret)
		goto err;
	ret = btrfs_make_root_dir(trans, root, BTRFS_FIRST_FREE_OBJECTID);
	if (ret)
		goto err;
	memcpy(&location, &root->fs_info->fs_root->root_key, sizeof(location));
	location.offset = (u64)-1;
	ret = btrfs_insert_dir_item(trans, root->fs_info->tree_root,
			"default", 7,
			btrfs_super_root_dir(root->fs_info->super_copy),
			&location, BTRFS_FT_DIR, 0);
	if (ret)
		goto err;

	ret = btrfs_insert_inode_ref(trans, root->fs_info->tree_root,
			     "default", 7, location.objectid,
			     BTRFS_ROOT_TREE_DIR_OBJECTID, 0);
	if (ret)
		goto err;

	btrfs_commit_transaction(trans, root);
err:
	return ret;
}

static void __recow_root(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root)
{
	int ret;
	struct extent_buffer *tmp;

	if (trans->transid != btrfs_root_generation(&root->root_item)) {
		extent_buffer_get(root->node);
		ret = __btrfs_cow_block(trans, root, root->node,
					NULL, 0, &tmp, 0, 0);
		BUG_ON(ret);
		free_extent_buffer(tmp);
	}
}

static void recow_roots(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root)
{
	struct btrfs_fs_info *info = root->fs_info;

	__recow_root(trans, info->fs_root);
	__recow_root(trans, info->tree_root);
	__recow_root(trans, info->extent_root);
	__recow_root(trans, info->chunk_root);
	__recow_root(trans, info->dev_root);
	__recow_root(trans, info->csum_root);
}

static int create_one_raid_group(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 type)
{
	u64 chunk_start;
	u64 chunk_size;
	int ret;

	ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
				&chunk_start, &chunk_size, type);
	if (ret == -ENOSPC) {
		fprintf(stderr, "not enough free space\n");
		exit(1);
	}
	BUG_ON(ret);
	ret = btrfs_make_block_group(trans, root->fs_info->extent_root, 0,
				     type, BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     chunk_start, chunk_size);
	BUG_ON(ret);
	return ret;
}

static int create_raid_groups(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 data_profile,
			      int data_profile_opt, u64 metadata_profile,
			      int mixed)
{
	u64 num_devices = btrfs_super_num_devices(root->fs_info->super_copy);
	int ret;

	if (metadata_profile) {
		u64 meta_flags = BTRFS_BLOCK_GROUP_METADATA;

		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_SYSTEM |
					    metadata_profile);
		BUG_ON(ret);

		if (mixed)
			meta_flags |= BTRFS_BLOCK_GROUP_DATA;

		ret = create_one_raid_group(trans, root, meta_flags |
					    metadata_profile);
		BUG_ON(ret);

	}
	if (!mixed && num_devices > 1 && data_profile) {
		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_DATA |
					    data_profile);
		BUG_ON(ret);
	}
	recow_roots(trans, root);

	return 0;
}

static int create_data_reloc_tree(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root)
{
	struct btrfs_key location;
	struct btrfs_root_item root_item;
	struct extent_buffer *tmp;
	u64 objectid = BTRFS_DATA_RELOC_TREE_OBJECTID;
	int ret;

	ret = btrfs_copy_root(trans, root, root->node, &tmp, objectid);
	BUG_ON(ret);

	memcpy(&root_item, &root->root_item, sizeof(root_item));
	btrfs_set_root_bytenr(&root_item, tmp->start);
	btrfs_set_root_level(&root_item, btrfs_header_level(tmp));
	btrfs_set_root_generation(&root_item, trans->transid);
	free_extent_buffer(tmp);

	location.objectid = objectid;
	location.type = BTRFS_ROOT_ITEM_KEY;
	location.offset = 0;
	ret = btrfs_insert_root(trans, root->fs_info->tree_root,
				&location, &root_item);
	BUG_ON(ret);
	return 0;
}


static void print_usage(void) __attribute__((noreturn));
static void print_usage(void)
{
	fprintf(stderr, "usage: mkfs.btrfs [options] dev [ dev ... ]\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "\t -A --alloc-start the offset to start the FS\n");
	fprintf(stderr, "\t -b --byte-count total number of bytes in the FS\n");
	fprintf(stderr, "\t -d --data data profile, raid0, raid1, raid5, raid6, raid10, dup or single\n");
	fprintf(stderr, "\t -f --force force overwrite of existing filesystem\n");
	fprintf(stderr, "\t -l --leafsize size of btree leaves\n");
	fprintf(stderr, "\t -L --label set a label\n");
	fprintf(stderr, "\t -m --metadata metadata profile, values like data profile\n");
	fprintf(stderr, "\t -M --mixed mix metadata and data together\n");
	fprintf(stderr, "\t -n --nodesize size of btree nodes\n");
	fprintf(stderr, "\t -s --sectorsize min block allocation (may not mountable by current kernel)\n");
	fprintf(stderr, "\t -r --rootdir the source directory\n");
	fprintf(stderr, "\t -K --nodiscard do not perform whole device TRIM\n");
	fprintf(stderr, "\t -O --features comma separated list of filesystem features\n");
	fprintf(stderr, "\t -U --uuid specify the filesystem UUID\n");
	fprintf(stderr, "\t -V --version print the mkfs.btrfs version and exit\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

static void print_version(void) __attribute__((noreturn));
static void print_version(void)
{
	fprintf(stderr, "mkfs.btrfs, part of %s\n", BTRFS_BUILD_VERSION);
	exit(0);
}

static u64 parse_profile(char *s)
{
	if (strcmp(s, "raid0") == 0) {
		return BTRFS_BLOCK_GROUP_RAID0;
	} else if (strcmp(s, "raid1") == 0) {
		return BTRFS_BLOCK_GROUP_RAID1;
	} else if (strcmp(s, "raid5") == 0) {
		return BTRFS_BLOCK_GROUP_RAID5;
	} else if (strcmp(s, "raid6") == 0) {
		return BTRFS_BLOCK_GROUP_RAID6;
	} else if (strcmp(s, "raid10") == 0) {
		return BTRFS_BLOCK_GROUP_RAID10;
	} else if (strcmp(s, "dup") == 0) {
		return BTRFS_BLOCK_GROUP_DUP;
	} else if (strcmp(s, "single") == 0) {
		return 0;
	} else {
		fprintf(stderr, "Unknown profile %s\n", s);
		print_usage();
	}
	/* not reached */
	return 0;
}

static char *parse_label(char *input)
{
	int len = strlen(input);

	if (len >= BTRFS_LABEL_SIZE) {
		fprintf(stderr, "Label %s is too long (max %d)\n", input,
			BTRFS_LABEL_SIZE - 1);
		exit(1);
	}
	return strdup(input);
}

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
	btrfs_set_key_type(&location, BTRFS_INODE_ITEM_KEY);

	if (S_ISDIR(st->st_mode))
		filetype = BTRFS_FT_DIR;
	if (S_ISREG(st->st_mode))
		filetype = BTRFS_FT_REG_FILE;
	if (S_ISLNK(st->st_mode))
		filetype = BTRFS_FT_SYMLINK;

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
	u64 sectorsize = root->sectorsize;

	/*
	 * btrfs_inode_item has some reserved fields
	 * and represents on-disk inode entry, so
	 * zero everything to prevent information leak
	 */
	memset(dst, 0, sizeof (*dst));

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
	if ((strncmp(entry->d_name, ".", entry->d_reclen) == 0) ||
		(strncmp(entry->d_name, "..", entry->d_reclen) == 0))
		return 0;
	else
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

static u64 calculate_dir_inode_size(char *dirname)
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
			   struct stat *st, char *name,
			   u64 self_objectid, ino_t parent_inum,
			   int dir_index_cnt, struct btrfs_inode_item *inode_ret)
{
	int ret;
	struct btrfs_key inode_key;
	struct btrfs_inode_item btrfs_inode;
	u64 objectid;
	u64 inode_size = 0;

	fill_inode_item(trans, root, &btrfs_inode, st);
	objectid = self_objectid;

	if (S_ISDIR(st->st_mode)) {
		inode_size = calculate_dir_inode_size(name);
		btrfs_set_stack_inode_size(&btrfs_inode, inode_size);
	}

	inode_key.objectid = objectid;
	inode_key.offset = 0;
	btrfs_set_key_type(&inode_key, BTRFS_INODE_ITEM_KEY);

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
		if(errno == ENOTSUP)
			return 0;
		fprintf(stderr, "get a list of xattr failed for %s\n",
			file_name);
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
			if(errno == ENOTSUP)
				return 0;
			fprintf(stderr, "get a xattr value failed for %s attr %s\n",
				file_name, cur_name);
			return ret;
		}

		ret = btrfs_insert_xattr_item(trans, root, cur_name,
					      cur_name_len, cur_value,
					      ret, objectid);
		if (ret) {
			fprintf(stderr, "insert a xattr item failed for %s\n",
				file_name);
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
	u64 sectorsize = root->sectorsize;
	char *buf = malloc(sectorsize);

	ret = readlink(path_name, buf, sectorsize);
	if (ret <= 0) {
		fprintf(stderr, "readlink failed for %s\n", path_name);
		goto fail;
	}
	if (ret >= sectorsize) {
		fprintf(stderr, "symlink too long for %s", path_name);
		ret = -1;
		goto fail;
	}

	buf[ret] = '\0'; /* readlink does not do it for us */
	ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
					 buf, ret + 1);
fail:
	free(buf);
	return ret;
}

static int add_file_items(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  struct btrfs_inode_item *btrfs_inode, u64 objectid,
			  ino_t parent_inum, struct stat *st,
			  const char *path_name, int out_fd)
{
	int ret = -1;
	ssize_t ret_read;
	u64 bytes_read = 0;
	struct btrfs_key key;
	int blocks;
	u32 sectorsize = root->sectorsize;
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
		fprintf(stderr, "%s open failed\n", path_name);
		return ret;
	}

	blocks = st->st_size / sectorsize;
	if (st->st_size % sectorsize)
		blocks += 1;

	if (st->st_size <= BTRFS_MAX_INLINE_DATA_SIZE(root)) {
		char *buffer = malloc(st->st_size);
		ret_read = pread64(fd, buffer, st->st_size, bytes_read);
		if (ret_read == -1) {
			fprintf(stderr, "%s read failed\n", path_name);
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
	eb = malloc(sizeof(*eb) + sectorsize);
	if (!eb) {
		ret = -ENOMEM;
		goto end;
	}
	memset(eb, 0, sizeof(*eb) + sectorsize);

again:

	/*
	 * keep our extent size at 1MB max, this makes it easier to work inside
	 * the tiny block groups created during mkfs
	 */
	cur_bytes = min(total_bytes, 1024ULL * 1024);
	ret = btrfs_reserve_extent(trans, root, cur_bytes, 0, 0, (u64)-1,
				   &key, 1);
	if (ret)
		goto end;

	first_block = key.objectid;
	bytes_read = 0;

	while (bytes_read < cur_bytes) {

		memset(eb->data, 0, sectorsize);

		ret_read = pread64(fd, eb->data, sectorsize, file_pos + bytes_read);
		if (ret_read == -1) {
			fprintf(stderr, "%s read failed\n", path_name);
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

		ret = write_and_map_eb(trans, root, eb);
		if (ret) {
			fprintf(stderr, "output file write failed\n");
			goto end;
		}

		bytes_read += sectorsize;
	}

	if (bytes_read) {
		ret = btrfs_record_file_extent(trans, root, objectid, btrfs_inode,
					       file_pos, first_block, cur_bytes);
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

static char *make_path(char *dir, char *name)
{
	char *path;

	path = malloc(strlen(dir) + strlen(name) + 2);
	if (!path)
		return NULL;
	strcpy(path, dir);
	if (dir[strlen(dir) - 1] != '/')
		strcat(path, "/");
	strcat(path, name);
	return path;
}

static int traverse_directory(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, char *dir_name,
			      struct directory_name_entry *dir_head, int out_fd)
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
	char *parent_dir_name;
	char real_path[PATH_MAX];
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key root_dir_key;
	u64 root_dir_inode_size = 0;

	/* Add list for source directory */
	dir_entry = malloc(sizeof(struct directory_name_entry));
	dir_entry->dir_name = dir_name;
	dir_entry->path = realpath(dir_name, real_path);
	if (!dir_entry->path) {
		fprintf(stderr, "get directory real path error\n");
		ret = -1;
		goto fail_no_dir;
	}

	parent_inum = highest_inum + BTRFS_FIRST_FREE_OBJECTID;
	dir_entry->inum = parent_inum;
	list_add_tail(&dir_entry->list, &dir_head->list);

	btrfs_init_path(&path);

	root_dir_key.objectid = btrfs_root_dirid(&root->root_item);
	root_dir_key.offset = 0;
	btrfs_set_key_type(&root_dir_key, BTRFS_INODE_ITEM_KEY);
	ret = btrfs_lookup_inode(trans, root, &path, &root_dir_key, 1);
	if (ret) {
		fprintf(stderr, "root dir lookup error\n");
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
			fprintf(stderr, "chdir error for %s\n",
				parent_dir_name);
			ret = -1;
			goto fail_no_files;
		}

		count = scandir(parent_dir_entry->path, &files,
				directory_select, NULL);
		if (count == -1)
		{
			fprintf(stderr, "scandir for %s failed: %s\n",
				parent_dir_name, strerror (errno));
			ret = -1;
			goto fail;
		}

		for (i = 0; i < count; i++) {
			cur_file = files[i];

			if (lstat(cur_file->d_name, &st) == -1) {
				fprintf(stderr, "lstat failed for file %s\n",
					cur_file->d_name);
				ret = -1;
				goto fail;
			}

			cur_inum = st.st_ino;
			ret = add_directory_items(trans, root,
						  cur_inum, parent_inum,
						  cur_file->d_name,
						  &st, &dir_index_cnt);
			if (ret) {
				fprintf(stderr, "add_directory_items failed\n");
				goto fail;
			}

			ret = add_inode_items(trans, root, &st,
					      cur_file->d_name, cur_inum,
					      parent_inum, dir_index_cnt,
					      &cur_inode);
			if (ret == -EEXIST) {
				BUG_ON(st.st_nlink <= 1);
				continue;
			}
			if (ret) {
				fprintf(stderr, "add_inode_items failed\n");
				goto fail;
			}

			ret = add_xattr_item(trans, root,
					     cur_inum, cur_file->d_name);
			if (ret) {
				fprintf(stderr, "add_xattr_item failed\n");
				if(ret != -ENOTSUP)
					goto fail;
			}

			if (S_ISDIR(st.st_mode)) {
				dir_entry = malloc(sizeof(struct directory_name_entry));
				dir_entry->dir_name = cur_file->d_name;
				dir_entry->path = make_path(parent_dir_entry->path,
							    cur_file->d_name);
				dir_entry->inum = cur_inum;
				list_add_tail(&dir_entry->list,	&dir_head->list);
			} else if (S_ISREG(st.st_mode)) {
				ret = add_file_items(trans, root, &cur_inode,
						     cur_inum, parent_inum, &st,
						     cur_file->d_name, out_fd);
				if (ret) {
					fprintf(stderr, "add_file_items failed\n");
					goto fail;
				}
			} else if (S_ISLNK(st.st_mode)) {
				ret = add_symbolic_link(trans, root,
						        cur_inum, cur_file->d_name);
				if (ret) {
					fprintf(stderr, "add_symbolic_link failed\n");
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

static int open_target(char *output_name)
{
	int output_fd;
	output_fd = open(output_name, O_CREAT | O_RDWR | O_TRUNC,
		         S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);

	return output_fd;
}

static int create_chunks(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root, u64 num_of_meta_chunks,
			 u64 size_of_data)
{
	u64 chunk_start;
	u64 chunk_size;
	u64 meta_type = BTRFS_BLOCK_GROUP_METADATA;
	u64 data_type = BTRFS_BLOCK_GROUP_DATA;
	u64 minimum_data_chunk_size = 8 * 1024 * 1024;
	u64 i;
	int ret;

	for (i = 0; i < num_of_meta_chunks; i++) {
		ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
					&chunk_start, &chunk_size, meta_type);
		BUG_ON(ret);
		ret = btrfs_make_block_group(trans, root->fs_info->extent_root, 0,
					     meta_type, BTRFS_FIRST_CHUNK_TREE_OBJECTID,
					     chunk_start, chunk_size);
		BUG_ON(ret);
		set_extent_dirty(&root->fs_info->free_space_cache,
				 chunk_start, chunk_start + chunk_size - 1, 0);
	}

	if (size_of_data < minimum_data_chunk_size)
		size_of_data = minimum_data_chunk_size;

	ret = btrfs_alloc_data_chunk(trans, root->fs_info->extent_root,
				     &chunk_start, size_of_data, data_type);
	BUG_ON(ret);
	ret = btrfs_make_block_group(trans, root->fs_info->extent_root, 0,
				     data_type, BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     chunk_start, size_of_data);
	BUG_ON(ret);
	set_extent_dirty(&root->fs_info->free_space_cache,
			 chunk_start, chunk_start + size_of_data - 1, 0);
	return ret;
}

static int make_image(char *source_dir, struct btrfs_root *root, int out_fd)
{
	int ret;
	struct btrfs_trans_handle *trans;

	struct stat root_st;

	struct directory_name_entry dir_head;

	struct directory_name_entry *dir_entry = NULL;

	ret = lstat(source_dir, &root_st);
	if (ret) {
		fprintf(stderr, "unable to lstat the %s\n", source_dir);
		goto out;
	}

	INIT_LIST_HEAD(&dir_head.list);

	trans = btrfs_start_transaction(root, 1);
	ret = traverse_directory(trans, root, source_dir, &dir_head, out_fd);
	if (ret) {
		fprintf(stderr, "unable to traverse_directory\n");
		goto fail;
	}
	btrfs_commit_transaction(trans, root);

	printf("Making image is completed.\n");
	return 0;
fail:
	while (!list_empty(&dir_head.list)) {
		dir_entry = list_entry(dir_head.list.next,
				       struct directory_name_entry, list);
		list_del(&dir_entry->list);
		free(dir_entry);
	}
out:
	fprintf(stderr, "Making image is aborted.\n");
	return -1;
}

/*
 * This ignores symlinks with unreadable targets and subdirs that can't
 * be read.  It's a best-effort to give a rough estimate of the size of
 * a subdir.  It doesn't guarantee that prepopulating btrfs from this
 * tree won't still run out of space. 
 *
 * The rounding up to 4096 is questionable.  Previous code used du -B 4096.
 */
static u64 global_total_size;
static int ftw_add_entry_size(const char *fpath, const struct stat *st,
			      int type)
{
	if (type == FTW_F || type == FTW_D)
		global_total_size += round_up(st->st_size, 4096);

	return 0;
}

static u64 size_sourcedir(char *dir_name, u64 sectorsize,
			  u64 *num_of_meta_chunks_ret, u64 *size_of_data_ret)
{
	u64 dir_size = 0;
	u64 total_size = 0;
	int ret;
	u64 default_chunk_size = 8 * 1024 * 1024;	/* 8MB */
	u64 allocated_meta_size = 8 * 1024 * 1024;	/* 8MB */
	u64 allocated_total_size = 20 * 1024 * 1024;	/* 20MB */
	u64 num_of_meta_chunks = 0;
	u64 num_of_data_chunks = 0;
	u64 num_of_allocated_meta_chunks =
			allocated_meta_size / default_chunk_size;

	global_total_size = 0;
	ret = ftw(dir_name, ftw_add_entry_size, 10);
	dir_size = global_total_size;
	if (ret < 0) {
		fprintf(stderr, "ftw subdir walk of '%s' failed: %s\n",
			dir_name, strerror(errno));
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

static int zero_output_file(int out_fd, u64 size, u32 sectorsize)
{
	int len = sectorsize;
	int loop_num = size / sectorsize;
	u64 location = 0;
	char *buf = malloc(len);
	int ret = 0, i;
	ssize_t written;

	if (!buf)
		return -ENOMEM;
	memset(buf, 0, len);
	for (i = 0; i < loop_num; i++) {
		written = pwrite64(out_fd, buf, len, location);
		if (written != len)
			ret = -EIO;
		location += sectorsize;
	}
	free(buf);
	return ret;
}

static int check_leaf_or_node_size(u32 size, u32 sectorsize)
{
	if (size < sectorsize) {
		fprintf(stderr,
			"Illegal leafsize (or nodesize) %u (smaller than %u)\n",
			size, sectorsize);
		return -1;
	} else if (size > BTRFS_MAX_METADATA_BLOCKSIZE) {
		fprintf(stderr,
			"Illegal leafsize (or nodesize) %u (larger than %u)\n",
			size, BTRFS_MAX_METADATA_BLOCKSIZE);
		return -1;
	} else if (size & (sectorsize - 1)) {
		fprintf(stderr,
			"Illegal leafsize (or nodesize) %u (not align to %u)\n",
			size, sectorsize);
		return -1;
	}
	return 0;
}

static int is_ssd(const char *file)
{
	blkid_probe probe;
	char wholedisk[32];
	char sysfs_path[PATH_MAX];
	dev_t devno;
	int fd;
	char rotational;
	int ret;

	probe = blkid_new_probe_from_filename(file);
	if (!probe)
		return 0;

	/* Device number of this disk (possibly a partition) */
	devno = blkid_probe_get_devno(probe);
	if (!devno) {
		blkid_free_probe(probe);
		return 0;
	}

	/* Get whole disk name (not full path) for this devno */
	ret = blkid_devno_to_wholedisk(devno,
			wholedisk, sizeof(wholedisk), NULL);
	if (ret) {
		blkid_free_probe(probe);
		return 0;
	}

	snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/queue/rotational",
		 wholedisk);

	blkid_free_probe(probe);

	fd = open(sysfs_path, O_RDONLY);
	if (fd < 0) {
		return 0;
	}

	if (read(fd, &rotational, sizeof(char)) < sizeof(char)) {
		close(fd);
		return 0;
	}
	close(fd);

	return !atoi((const char *)&rotational);
}

#define BTRFS_FEATURE_LIST_ALL		(1ULL << 63)

static const struct btrfs_fs_feature {
	const char *name;
	u64 flag;
	const char *desc;
} mkfs_features[] = {
	{ "mixed-bg", BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS,
		"mixed data and metadata block groups" },
	{ "extref", BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF,
		"increased hardlink limit per file to 65536" },
	{ "raid56", BTRFS_FEATURE_INCOMPAT_RAID56,
		"raid56 extended format" },
	{ "skinny-metadata", BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA,
		"reduced-size metadata extent refs" },
	{ "no-holes", BTRFS_FEATURE_INCOMPAT_NO_HOLES,
		"no explicit hole extents for files" },
	/* Keep this one last */
	{ "list-all", BTRFS_FEATURE_LIST_ALL, NULL }
};

static void list_all_fs_features(void)
{
	int i;

	fprintf(stderr, "Filesystem features available at mkfs time:\n");
	for (i = 0; i < ARRAY_SIZE(mkfs_features) - 1; i++) {
		char *is_default = "";

		if (mkfs_features[i].flag & DEFAULT_MKFS_FEATURES)
			is_default = ", default";
		fprintf(stderr, "%-20s- %s (0x%llx%s)\n",
				mkfs_features[i].name,
				mkfs_features[i].desc,
				mkfs_features[i].flag,
				is_default);
	}
}

static int parse_one_fs_feature(const char *name, u64 *flags)
{
	int i;
	int found = 0;

	for (i = 0; i < ARRAY_SIZE(mkfs_features); i++) {
		if (name[0] == '^' &&
			!strcmp(mkfs_features[i].name, name + 1)) {
			*flags &= ~ mkfs_features[i].flag;
			found = 1;
		} else if (!strcmp(mkfs_features[i].name, name)) {
			*flags |= mkfs_features[i].flag;
			found = 1;
		}
	}

	return !found;
}

static void process_fs_features(u64 flags)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mkfs_features); i++) {
		if (flags & mkfs_features[i].flag) {
			printf("Turning ON incompat feature '%s': %s\n",
				mkfs_features[i].name,
				mkfs_features[i].desc);
		}
	}
}


/*
 * Return NULL if all features were parsed fine, otherwise return the name of
 * the first unparsed.
 */
static char* parse_fs_features(char *namelist, u64 *flags)
{
	char *this_char;
	char *save_ptr = NULL; /* Satisfy static checkers */

	for (this_char = strtok_r(namelist, ",", &save_ptr);
	     this_char != NULL;
	     this_char = strtok_r(NULL, ",", &save_ptr)) {
		if (parse_one_fs_feature(this_char, flags))
			return this_char;
	}

	return NULL;
}

int main(int ac, char **av)
{
	char *file;
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;
	char *label = NULL;
	char *first_file;
	u64 block_count = 0;
	u64 dev_block_count = 0;
	u64 blocks[7];
	u64 alloc_start = 0;
	u64 metadata_profile = 0;
	u64 data_profile = 0;
	u32 leafsize = max_t(u32, sysconf(_SC_PAGESIZE), DEFAULT_MKFS_LEAF_SIZE);
	u32 sectorsize = 4096;
	u32 nodesize = leafsize;
	u32 stripesize = 4096;
	int zero_end = 1;
	int fd;
	int ret;
	int i;
	int mixed = 0;
	int leaf_forced = 0;
	int data_profile_opt = 0;
	int metadata_profile_opt = 0;
	int discard = 1;
	int ssd = 0;
	int force_overwrite = 0;

	char *source_dir = NULL;
	int source_dir_set = 0;
	u64 num_of_meta_chunks = 0;
	u64 size_of_data = 0;
	u64 source_dir_size = 0;
	int dev_cnt = 0;
	int saved_optind;
	char estr[100];
	char *fs_uuid = NULL;
	u64 features = DEFAULT_MKFS_FEATURES;

	while(1) {
		int c;
		int option_index = 0;
		static const struct option long_options[] = {
			{ "alloc-start", 1, NULL, 'A'},
			{ "byte-count", 1, NULL, 'b' },
			{ "force", 0, NULL, 'f' },
			{ "leafsize", 1, NULL, 'l' },
			{ "label", 1, NULL, 'L'},
			{ "metadata", 1, NULL, 'm' },
			{ "mixed", 0, NULL, 'M' },
			{ "nodesize", 1, NULL, 'n' },
			{ "sectorsize", 1, NULL, 's' },
			{ "data", 1, NULL, 'd' },
			{ "version", 0, NULL, 'V' },
			{ "rootdir", 1, NULL, 'r' },
			{ "nodiscard", 0, NULL, 'K' },
			{ "features", 1, NULL, 'O' },
			{ "uuid", required_argument, NULL, 'U' },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(ac, av, "A:b:fl:n:s:m:d:L:O:r:U:VMK",
				long_options, &option_index);
		if (c < 0)
			break;
		switch(c) {
			case 'A':
				alloc_start = parse_size(optarg);
				break;
			case 'f':
				force_overwrite = 1;
				break;
			case 'd':
				data_profile = parse_profile(optarg);
				data_profile_opt = 1;
				break;
			case 'l':
			case 'n':
				nodesize = parse_size(optarg);
				leafsize = parse_size(optarg);
				leaf_forced = 1;
				break;
			case 'L':
				label = parse_label(optarg);
				break;
			case 'm':
				metadata_profile = parse_profile(optarg);
				metadata_profile_opt = 1;
				break;
			case 'M':
				mixed = 1;
				break;
			case 'O': {
				char *orig = strdup(optarg);
				char *tmp = orig;

				tmp = parse_fs_features(tmp, &features);
				if (tmp) {
					fprintf(stderr,
						"Unrecognized filesystem feature '%s'\n",
							tmp);
					free(orig);
					exit(1);
				}
				free(orig);
				if (features & BTRFS_FEATURE_LIST_ALL) {
					list_all_fs_features();
					exit(0);
				}
				break;
				}
			case 's':
				sectorsize = parse_size(optarg);
				break;
			case 'b':
				block_count = parse_size(optarg);
				if (block_count <= BTRFS_MKFS_SMALL_VOLUME_SIZE) {
					fprintf(stdout,
				"SMALL VOLUME: forcing mixed metadata/data groups\n");
					mixed = 1;
				}
				zero_end = 0;
				break;
			case 'V':
				print_version();
				break;
			case 'r':
				source_dir = optarg;
				source_dir_set = 1;
				break;
			case 'U':
				fs_uuid = optarg;
				break;
			case 'K':
				discard = 0;
				break;
			default:
				print_usage();
		}
	}
	sectorsize = max(sectorsize, (u32)sysconf(_SC_PAGESIZE));
	if (check_leaf_or_node_size(leafsize, sectorsize))
		exit(1);
	if (check_leaf_or_node_size(nodesize, sectorsize))
		exit(1);
	saved_optind = optind;
	dev_cnt = ac - optind;
	if (dev_cnt == 0)
		print_usage();

	if (source_dir_set && dev_cnt > 1) {
		fprintf(stderr,
			"The -r option is limited to a single device\n");
		exit(1);
	}

	if (fs_uuid) {
		uuid_t dummy_uuid;

		if (uuid_parse(fs_uuid, dummy_uuid) != 0) {
			fprintf(stderr, "could not parse UUID: %s\n", fs_uuid);
			exit(1);
		}
		if (!test_uuid_unique(fs_uuid)) {
			fprintf(stderr, "non-unique UUID: %s\n", fs_uuid);
			exit(1);
		}
	}
	
	while (dev_cnt-- > 0) {
		file = av[optind++];
		if (is_block_device(file))
			if (test_dev_for_mkfs(file, force_overwrite, estr)) {
				fprintf(stderr, "Error: %s", estr);
				exit(1);
			}
	}

	optind = saved_optind;
	dev_cnt = ac - optind;

	file = av[optind++];
	ssd = is_ssd(file);

	if (is_vol_small(file)) {
		printf("SMALL VOLUME: forcing mixed metadata/data groups\n");
		mixed = 1;
	}

	/*
	* Set default profiles according to number of added devices.
	* For mixed groups defaults are single/single.
	*/
	if (!mixed) {
		if (!metadata_profile_opt) {
			if (dev_cnt == 1 && ssd)
				printf("Detected a SSD, turning off metadata "
				"duplication.  Mkfs with -m dup if you want to "
				"force metadata duplication.\n");

			metadata_profile = (dev_cnt > 1) ?
					BTRFS_BLOCK_GROUP_RAID1 : (ssd) ?
					0: BTRFS_BLOCK_GROUP_DUP;
		}
		if (!data_profile_opt) {
			data_profile = (dev_cnt > 1) ?
				BTRFS_BLOCK_GROUP_RAID0 : 0; /* raid0 or single */
		}
	} else {
		u32 best_leafsize = max_t(u32, sysconf(_SC_PAGESIZE), sectorsize);

		if (metadata_profile_opt || data_profile_opt) {
			if (metadata_profile != data_profile) {
				fprintf(stderr,
	"ERROR: With mixed block groups data and metadata profiles must be the same\n");
				exit(1);
			}
		}

		if (!leaf_forced) {
			leafsize = best_leafsize;
			nodesize = best_leafsize;
			if (check_leaf_or_node_size(leafsize, sectorsize))
				exit(1);
		}
		if (leafsize != sectorsize) {
			fprintf(stderr, "Error: mixed metadata/data block groups "
				"require metadata blocksizes equal to the sectorsize\n");
			exit(1);
		}
	}

	/* Check device/block_count after the leafsize is determined */
	if (block_count && block_count < btrfs_min_dev_size(leafsize)) {
		fprintf(stderr,
			"Size '%llu' is too small to make a usable filesystem\n",
			block_count);
		fprintf(stderr,
			"Minimum size for btrfs filesystem is %llu\n",
			btrfs_min_dev_size(leafsize));
		exit(1);
	}
	for (i = saved_optind; i < saved_optind + dev_cnt; i++) {
		char *path;

		path = av[i];
		ret = test_minimum_size(path, leafsize);
		if (ret < 0) {
			fprintf(stderr, "Failed to check size for '%s': %s\n",
				path, strerror(-ret));
			exit (1);
		}
		if (ret > 0) {
			fprintf(stderr,
				"'%s' is too small to make a usable filesystem\n",
				path);
			fprintf(stderr,
				"Minimum size for each btrfs device is %llu.\n",
				btrfs_min_dev_size(leafsize));
			exit(1);
		}
	}
	ret = test_num_disk_vs_raid(metadata_profile, data_profile,
			dev_cnt, mixed, estr);
	if (ret) {
		fprintf(stderr, "Error: %s\n", estr);
		exit(1);
	}

	/* if we are here that means all devs are good to btrfsify */
	printf("%s\n", BTRFS_BUILD_VERSION);
	printf("See http://btrfs.wiki.kernel.org for more information.\n\n");

	dev_cnt--;

	if (!source_dir_set) {
		/*
		 * open without O_EXCL so that the problem should not
		 * occur by the following processing.
		 * (btrfs_register_one_device() fails if O_EXCL is on)
		 */
		fd = open(file, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "unable to open %s: %s\n", file,
				strerror(errno));
			exit(1);
		}
		first_file = file;
		ret = btrfs_prepare_device(fd, file, zero_end, &dev_block_count,
					   block_count, &mixed, discard);
		if (ret) {
			close(fd);
			exit(1);
		}
		if (block_count && block_count > dev_block_count) {
			fprintf(stderr, "%s is smaller than requested size\n", file);
			exit(1);
		}
	} else {
		fd = open_target(file);
		if (fd < 0) {
			fprintf(stderr, "unable to open the %s\n", file);
			exit(1);
		}

		first_file = file;
		source_dir_size = size_sourcedir(source_dir, sectorsize,
					     &num_of_meta_chunks, &size_of_data);
		if(block_count < source_dir_size)
			block_count = source_dir_size;
		ret = zero_output_file(fd, block_count, sectorsize);
		if (ret) {
			fprintf(stderr, "unable to zero the output file\n");
			exit(1);
		}
		/* our "device" is the new image file */
		dev_block_count = block_count;
	}

	/* To create the first block group and chunk 0 in make_btrfs */
	if (dev_block_count < BTRFS_MKFS_SYSTEM_GROUP_SIZE) {
		fprintf(stderr, "device is too small to make filesystem\n");
		exit(1);
	}

	blocks[0] = BTRFS_SUPER_INFO_OFFSET;
	for (i = 1; i < 7; i++) {
		blocks[i] = BTRFS_SUPER_INFO_OFFSET + 1024 * 1024 +
			leafsize * i;
	}

	/*
	 * FS features that can be set by other means than -O
	 * just set the bit here
	 */
	if (mixed)
		features |= BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS;

	if ((data_profile | metadata_profile) &
	    (BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)) {
		features |= BTRFS_FEATURE_INCOMPAT_RAID56;
	}

	process_fs_features(features);

	ret = make_btrfs(fd, file, label, fs_uuid, blocks, dev_block_count,
			 nodesize, leafsize,
			 sectorsize, stripesize, features);
	if (ret) {
		fprintf(stderr, "error during mkfs: %s\n", strerror(-ret));
		exit(1);
	}

	root = open_ctree(file, 0, OPEN_CTREE_WRITES);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		close(fd);
		exit(1);
	}
	root->fs_info->alloc_start = alloc_start;

	ret = make_root_dir(root, mixed);
	if (ret) {
		fprintf(stderr, "failed to setup the root directory\n");
		exit(1);
	}

	trans = btrfs_start_transaction(root, 1);

	btrfs_register_one_device(file);

	if (dev_cnt == 0)
		goto raid_groups;

	while (dev_cnt-- > 0) {
		int old_mixed = mixed;

		file = av[optind++];

		/*
		 * open without O_EXCL so that the problem should not
		 * occur by the following processing.
		 * (btrfs_register_one_device() fails if O_EXCL is on)
		 */
		fd = open(file, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "unable to open %s: %s\n", file,
				strerror(errno));
			exit(1);
		}
		ret = btrfs_device_already_in_root(root, fd,
						   BTRFS_SUPER_INFO_OFFSET);
		if (ret) {
			fprintf(stderr, "skipping duplicate device %s in FS\n",
				file);
			close(fd);
			continue;
		}
		ret = btrfs_prepare_device(fd, file, zero_end, &dev_block_count,
					   block_count, &mixed, discard);
		if (ret) {
			close(fd);
			exit(1);
		}
		mixed = old_mixed;

		ret = btrfs_add_to_fsid(trans, root, fd, file, dev_block_count,
					sectorsize, sectorsize, sectorsize);
		BUG_ON(ret);
		btrfs_register_one_device(file);
	}

raid_groups:
	if (!source_dir_set) {
		ret = create_raid_groups(trans, root, data_profile,
				 data_profile_opt, metadata_profile,
				 mixed);
		BUG_ON(ret);
	}

	ret = create_data_reloc_tree(trans, root);
	BUG_ON(ret);

	printf("fs created label %s on %s\n\tnodesize %u leafsize %u "
	    "sectorsize %u size %s\n",
	    label, first_file, nodesize, leafsize, sectorsize,
	    pretty_size(btrfs_super_total_bytes(root->fs_info->super_copy)));

	btrfs_commit_transaction(trans, root);

	if (source_dir_set) {
		trans = btrfs_start_transaction(root, 1);
		ret = create_chunks(trans, root,
				    num_of_meta_chunks, size_of_data);
		BUG_ON(ret);
		btrfs_commit_transaction(trans, root);

		ret = make_image(source_dir, root, fd);
		BUG_ON(ret);
	}

	ret = close_ctree(root);
	BUG_ON(ret);
	free(label);
	return 0;
}
