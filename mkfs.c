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

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <linux/fs.h>
#include <ctype.h>
#include <attr/xattr.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "utils.h"
#include "version.h"

static u64 index_cnt = 2;

struct directory_name_entry {
	char *dir_name;
	char *path;
	ino_t inum;
	struct list_head list;
};

static u64 parse_size(char *s)
{
	int len = strlen(s);
	char c;
	u64 mult = 1;
	u64 ret;

	s = strdup(s);

	if (len && !isdigit(s[len - 1])) {
		c = tolower(s[len - 1]);
		switch (c) {
		case 'g':
			mult *= 1024;
		case 'm':
			mult *= 1024;
		case 'k':
			mult *= 1024;
		case 'b':
			break;
		default:
			fprintf(stderr, "Unknown size descriptor %c\n", c);
			exit(1);
		}
		s[len - 1] = '\0';
	}
	ret = atol(s) * mult;
	free(s);
	return ret;
}

static int make_root_dir(struct btrfs_root *root, int mixed)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key location;
	u64 bytes_used;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	bytes_used = btrfs_super_bytes_used(&root->fs_info->super_copy);

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
			btrfs_super_root_dir(&root->fs_info->super_copy),
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

static int recow_roots(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root)
{
	int ret;
	struct extent_buffer *tmp;
	struct btrfs_fs_info *info = root->fs_info;

	ret = __btrfs_cow_block(trans, info->fs_root, info->fs_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	ret = __btrfs_cow_block(trans, info->tree_root, info->tree_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	ret = __btrfs_cow_block(trans, info->extent_root,
				info->extent_root->node, NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	ret = __btrfs_cow_block(trans, info->chunk_root, info->chunk_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);


	ret = __btrfs_cow_block(trans, info->dev_root, info->dev_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	ret = __btrfs_cow_block(trans, info->csum_root, info->csum_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	return 0;
}

static int create_one_raid_group(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 type)
{
	u64 chunk_start;
	u64 chunk_size;
	int ret;

	ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
				&chunk_start, &chunk_size, type);
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
			      int metadata_profile_opt, int mixed)
{
	u64 num_devices = btrfs_super_num_devices(&root->fs_info->super_copy);
	u64 allowed;
	int ret;

	/*
	 * Set default profiles according to number of added devices.
	 * For mixed groups defaults are single/single.
	 */
	if (!metadata_profile_opt && !mixed) {
		metadata_profile = (num_devices > 1) ?
			BTRFS_BLOCK_GROUP_RAID1 : BTRFS_BLOCK_GROUP_DUP;
	}
	if (!data_profile_opt && !mixed) {
		data_profile = (num_devices > 1) ?
			BTRFS_BLOCK_GROUP_RAID0 : 0; /* raid0 or single */
	}

	if (num_devices == 1)
		allowed = BTRFS_BLOCK_GROUP_DUP;
	else if (num_devices >= 4) {
		allowed = BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 |
			BTRFS_BLOCK_GROUP_RAID10;
	} else
		allowed = BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1;

	if (metadata_profile & ~allowed) {
		fprintf(stderr,	"unable to create FS with metadata "
			"profile %llu (have %llu devices)\n", metadata_profile,
			num_devices);
		exit(1);
	}
	if (data_profile & ~allowed) {
		fprintf(stderr, "unable to create FS with data "
			"profile %llu (have %llu devices)\n", data_profile,
			num_devices);
		exit(1);
	}

	/* allow dup'ed data chunks only in mixed mode */
	if (!mixed && (data_profile & BTRFS_BLOCK_GROUP_DUP)) {
		fprintf(stderr, "dup for data is allowed only in mixed mode\n");
		exit(1);
	}

	if (allowed & metadata_profile) {
		u64 meta_flags = BTRFS_BLOCK_GROUP_METADATA;

		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_SYSTEM |
					    (allowed & metadata_profile));
		BUG_ON(ret);

		if (mixed)
			meta_flags |= BTRFS_BLOCK_GROUP_DATA;

		ret = create_one_raid_group(trans, root, meta_flags |
					    (allowed & metadata_profile));
		BUG_ON(ret);

		ret = recow_roots(trans, root);
		BUG_ON(ret);
	}
	if (!mixed && num_devices > 1 && (allowed & data_profile)) {
		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_DATA |
					    (allowed & data_profile));
		BUG_ON(ret);
	}
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

static void print_usage(void)
{
	fprintf(stderr, "usage: mkfs.btrfs [options] dev [ dev ... ]\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "\t -A --alloc-start the offset to start the FS\n");
	fprintf(stderr, "\t -b --byte-count total number of bytes in the FS\n");
	fprintf(stderr, "\t -d --data data profile, raid0, raid1, raid10, dup or single\n");
	fprintf(stderr, "\t -l --leafsize size of btree leaves\n");
	fprintf(stderr, "\t -L --label set a label\n");
	fprintf(stderr, "\t -m --metadata metadata profile, values like data profile\n");
	fprintf(stderr, "\t -M --mixed mix metadata and data together\n");
	fprintf(stderr, "\t -n --nodesize size of btree nodes\n");
	fprintf(stderr, "\t -s --sectorsize min block allocation\n");
	fprintf(stderr, "\t -r --rootdir the source directory\n");
	fprintf(stderr, "\t -K --nodiscard do not perform whole device TRIM\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

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
	int i;
	int len = strlen(input);

	if (len >= BTRFS_LABEL_SIZE) {
		fprintf(stderr, "Label %s is too long (max %d)\n", input,
			BTRFS_LABEL_SIZE - 1);
		exit(1);
	}
	for (i = 0; i < len; i++) {
		if (input[i] == '/' || input[i] == '\\') {
			fprintf(stderr, "invalid label %s\n", input);
			exit(1);
		}
	}
	return strdup(input);
}

static struct option long_options[] = {
	{ "alloc-start", 1, NULL, 'A'},
	{ "byte-count", 1, NULL, 'b' },
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
	{ 0, 0, 0, 0}
};

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
	int name_len;

	name_len = strlen(name);
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
	if (ret)
		goto fail;

	ret = btrfs_insert_inode_ref(trans, root, name, name_len,
				     objectid, parent_inum, dir_index_cnt);
	if (ret)
		goto fail;

	*inode_ret = btrfs_inode;
fail:
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
static int custom_alloc_extent(struct btrfs_root *root, u64 num_bytes,
			       u64 hint_byte, struct btrfs_key *ins)
{
	u64 start;
	u64 end;
	u64 last = hint_byte;
	int ret;
	int wrapped = 0;
	struct btrfs_block_group_cache *cache;

	while (1) {
		ret = find_first_extent_bit(&root->fs_info->free_space_cache,
					    last, &start, &end, EXTENT_DIRTY);
		if (ret) {
			if (wrapped++ == 0) {
				last = 0;
				continue;
			} else {
				goto fail;
			}
		}

		start = max(last, start);
		last = end + 1;
		if (last - start < num_bytes)
			continue;

		last = start + num_bytes;
		if (test_range_bit(&root->fs_info->pinned_extents,
				   start, last - 1, EXTENT_DIRTY, 0))
			continue;

		cache = btrfs_lookup_block_group(root->fs_info, start);
		BUG_ON(!cache);
		if (cache->flags & BTRFS_BLOCK_GROUP_SYSTEM ||
		    last > cache->key.objectid + cache->key.offset) {
			last = cache->key.objectid + cache->key.offset;
			continue;
		}

		if (cache->flags & (BTRFS_BLOCK_GROUP_SYSTEM |
			    BTRFS_BLOCK_GROUP_METADATA)) {
			last = cache->key.objectid + cache->key.offset;
			continue;
		}

		clear_extent_dirty(&root->fs_info->free_space_cache,
				   start, start + num_bytes - 1, 0);

		ins->objectid = start;
		ins->offset = num_bytes;
		ins->type = BTRFS_EXTENT_ITEM_KEY;
		return 0;
	}
fail:
	fprintf(stderr, "not enough free space\n");
	return -ENOSPC;
}

static int record_file_extent(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *inode,
			      u64 file_pos, u64 disk_bytenr,
			      u64 num_bytes)
{
	int ret;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key ins_key;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;

	btrfs_init_path(&path);

	ins_key.objectid = objectid;
	ins_key.offset = 0;
	btrfs_set_key_type(&ins_key, BTRFS_EXTENT_DATA_KEY);
	ret = btrfs_insert_empty_item(trans, root, &path, &ins_key,
				      sizeof(*fi));
	if (ret)
		goto fail;
	leaf = path.nodes[0];
	fi = btrfs_item_ptr(leaf, path.slots[0],
			    struct btrfs_file_extent_item);
	btrfs_set_file_extent_generation(leaf, fi, trans->transid);
	btrfs_set_file_extent_type(leaf, fi, BTRFS_FILE_EXTENT_REG);
	btrfs_set_file_extent_disk_bytenr(leaf, fi, disk_bytenr);
	btrfs_set_file_extent_disk_num_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_offset(leaf, fi, 0);
	btrfs_set_file_extent_num_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_ram_bytes(leaf, fi, num_bytes);
	btrfs_set_file_extent_compression(leaf, fi, 0);
	btrfs_set_file_extent_encryption(leaf, fi, 0);
	btrfs_set_file_extent_other_encoding(leaf, fi, 0);
	btrfs_mark_buffer_dirty(leaf);

	btrfs_release_path(root, &path);

	ins_key.objectid = disk_bytenr;
	ins_key.offset = num_bytes;
	ins_key.type = BTRFS_EXTENT_ITEM_KEY;

	ret = btrfs_insert_empty_item(trans, extent_root, &path,
				&ins_key, sizeof(*ei));
	if (ret == 0) {
		leaf = path.nodes[0];
		ei = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_extent_item);

		btrfs_set_extent_refs(leaf, ei, 0);
		btrfs_set_extent_generation(leaf, ei, trans->transid);
		btrfs_set_extent_flags(leaf, ei, BTRFS_EXTENT_FLAG_DATA);

		btrfs_mark_buffer_dirty(leaf);
		ret = btrfs_update_block_group(trans, root, disk_bytenr,
					       num_bytes, 1, 0);
		if (ret)
			goto fail;
	} else if (ret != -EEXIST) {
		goto fail;
	}

	ret = btrfs_inc_extent_ref(trans, root, disk_bytenr, num_bytes, 0,
				   root->root_key.objectid,
				   objectid, 0);
fail:
	btrfs_release_path(root, &path);
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
	char *buffer = NULL;
	struct btrfs_key key;
	int blocks;
	u32 sectorsize = root->sectorsize;
	u64 first_block = 0;
	u64 num_blocks = 0;
	int fd;

	fd = open(path_name, O_RDONLY);
	if (fd == -1) {
		fprintf(stderr, "%s open failed\n", path_name);
		goto end;
	}

	blocks = st->st_size / sectorsize;
	if (st->st_size % sectorsize)
		blocks += 1;

	if (st->st_size <= BTRFS_MAX_INLINE_DATA_SIZE(root)) {
		buffer = malloc(st->st_size);
		ret_read = pread64(fd, buffer, st->st_size, bytes_read);
		if (ret_read == -1) {
			fprintf(stderr, "%s read failed\n", path_name);
			goto end;
		}

		ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
						 buffer, st->st_size);
		goto end;
	}

	ret = custom_alloc_extent(root, blocks * sectorsize, 0, &key);
	if (ret)
		goto end;

	first_block = key.objectid;
	bytes_read = 0;
	buffer = malloc(sectorsize);

	do {
		memset(buffer, 0, sectorsize);
		ret_read = pread64(fd, buffer, sectorsize, bytes_read);
		if (ret_read == -1) {
			fprintf(stderr, "%s read failed\n", path_name);
			goto end;
		}

		ret = pwrite64(out_fd, buffer, sectorsize,
			       first_block + bytes_read);
		if (ret != sectorsize) {
			fprintf(stderr, "output file write failed\n");
			goto end;
		}

		/* checksum for file data */
		ret = btrfs_csum_file_block(trans, root->fs_info->csum_root,
				first_block + (blocks * sectorsize),
				first_block + bytes_read,
				buffer, sectorsize);
		if (ret) {
			fprintf(stderr, "%s checksum failed\n", path_name);
			goto end;
		}

		bytes_read += ret_read;
		num_blocks++;
	} while (ret_read == sectorsize);

	if (num_blocks > 0) {
		ret = record_file_extent(trans, root, objectid, btrfs_inode,
					 first_block, first_block,
					 blocks * sectorsize);
		if (ret)
			goto end;
	}

end:
	if (buffer)
		free(buffer);
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
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key root_dir_key;
	u64 root_dir_inode_size = 0;

	/* Add list for source directory */
	dir_entry = malloc(sizeof(struct directory_name_entry));
	dir_entry->dir_name = dir_name;
	dir_entry->path = strdup(dir_name);

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
		return -1;
	}

	leaf = path.nodes[0];
	inode_item = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_inode_item);

	root_dir_inode_size = calculate_dir_inode_size(dir_name);
	btrfs_set_inode_size(leaf, inode_item, root_dir_inode_size);
	btrfs_mark_buffer_dirty(leaf);

	btrfs_release_path(root, &path);

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
			goto fail_no_files;
		}

		count = scandir(parent_dir_entry->path, &files,
				directory_select, NULL);
		if (count == -1)
		{
			fprintf(stderr, "scandir for %s failed: %s\n",
				parent_dir_name, strerror (errno));
			goto fail;
		}

		for (i = 0; i < count; i++) {
			cur_file = files[i];

			if (lstat(cur_file->d_name, &st) == -1) {
				fprintf(stderr, "lstat failed for file %s\n",
					cur_file->d_name);
				goto fail;
			}

			cur_inum = ++highest_inum + BTRFS_FIRST_FREE_OBJECTID;
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
		free(parent_dir_entry->path);
		free(parent_dir_entry);

		index_cnt = 2;

	} while (!list_empty(&dir_head->list));

	return 0;
fail:
	free_namelist(files, count);
fail_no_files:
	free(parent_dir_entry->path);
	free(parent_dir_entry);
	return -1;
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

	ret = lstat(source_dir, &root_st);
	if (ret) {
		fprintf(stderr, "unable to lstat the %s\n", source_dir);
		goto fail;
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
	fprintf(stderr, "Making image is aborted.\n");
	return -1;
}

static u64 size_sourcedir(char *dir_name, u64 sectorsize,
			  u64 *num_of_meta_chunks_ret, u64 *size_of_data_ret)
{
	u64 dir_size = 0;
	u64 total_size = 0;
	int ret;
	char command[1024];
	char path[512];
	char *file_name = "temp_file";
	FILE *file;
	u64 default_chunk_size = 8 * 1024 * 1024;	/* 8MB */
	u64 allocated_meta_size = 8 * 1024 * 1024;	/* 8MB */
	u64 allocated_total_size = 20 * 1024 * 1024;	/* 20MB */
	u64 num_of_meta_chunks = 0;
	u64 num_of_allocated_meta_chunks =
			allocated_meta_size / default_chunk_size;

	ret = sprintf(command, "du -B 4096 -s ");
	if (ret < 0) {
		fprintf(stderr, "error executing sprintf for du command\n");
		return -1;
	}
	strcat(command, dir_name);
	strcat(command, " > ");
	strcat(command, file_name);
	ret = system(command);

	file = fopen(file_name, "r");
	ret = fscanf(file, "%lld %s\n", &dir_size, path);
	fclose(file);
	remove(file_name);

	dir_size *= sectorsize;
	*size_of_data_ret = dir_size;

	num_of_meta_chunks = (dir_size / 2) / default_chunk_size;
	if (((dir_size / 2) % default_chunk_size) != 0)
		num_of_meta_chunks++;
	if (num_of_meta_chunks <= num_of_allocated_meta_chunks)
		num_of_meta_chunks = 0;
	else
		num_of_meta_chunks -= num_of_allocated_meta_chunks;

	total_size = allocated_total_size + dir_size +
		     (num_of_meta_chunks * default_chunk_size);

	*num_of_meta_chunks_ret = num_of_meta_chunks;

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
	u32 leafsize = getpagesize();
	u32 sectorsize = 4096;
	u32 nodesize = leafsize;
	u32 stripesize = 4096;
	int zero_end = 1;
	int option_index = 0;
	int fd;
	int ret;
	int i;
	int mixed = 0;
	int data_profile_opt = 0;
	int metadata_profile_opt = 0;
	int nodiscard = 0;

	char *source_dir = NULL;
	int source_dir_set = 0;
	u64 num_of_meta_chunks = 0;
	u64 size_of_data = 0;
	u64 source_dir_size = 0;
	char *pretty_buf;

	while(1) {
		int c;
		c = getopt_long(ac, av, "A:b:l:n:s:m:d:L:r:VMK", long_options,
				&option_index);
		if (c < 0)
			break;
		switch(c) {
			case 'A':
				alloc_start = parse_size(optarg);
				break;
			case 'd':
				data_profile = parse_profile(optarg);
				data_profile_opt = 1;
				break;
			case 'l':
			case 'n':
				nodesize = parse_size(optarg);
				leafsize = parse_size(optarg);
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
			case 's':
				sectorsize = parse_size(optarg);
				break;
			case 'b':
				block_count = parse_size(optarg);
				if (block_count <= 1024*1024*1024) {
					printf("SMALL VOLUME: forcing mixed "
					       "metadata/data groups\n");
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
			case 'K':
				nodiscard=1;
				break;
			default:
				print_usage();
		}
	}
	sectorsize = max(sectorsize, (u32)getpagesize());
	if (check_leaf_or_node_size(leafsize, sectorsize))
		exit(1);
	if (check_leaf_or_node_size(nodesize, sectorsize))
		exit(1);
	ac = ac - optind;
	if (ac == 0)
		print_usage();

	printf("\nWARNING! - %s IS EXPERIMENTAL\n", BTRFS_BUILD_VERSION);
	printf("WARNING! - see http://btrfs.wiki.kernel.org before using\n\n");

	if (source_dir == 0) {
		file = av[optind++];
		ret = check_mounted(file);
		if (ret < 0) {
			fprintf(stderr, "error checking %s mount status\n", file);
			exit(1);
		}
		if (ret == 1) {
			fprintf(stderr, "%s is mounted\n", file);
			exit(1);
		}
		ac--;
		fd = open(file, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "unable to open %s\n", file);
			exit(1);
		}
		first_file = file;
		ret = btrfs_prepare_device(fd, file, zero_end, &dev_block_count,
					   block_count, &mixed, nodiscard);
		if (block_count && block_count > dev_block_count) {
			fprintf(stderr, "%s is smaller than requested size\n", file);
			exit(1);
		}
	} else {
		ac = 0;
		file = av[optind++];
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
	}
	if (mixed) {
		if (metadata_profile != data_profile) {
			fprintf(stderr, "With mixed block groups data and metadata "
				"profiles must be the same\n");
			exit(1);
		}
	}

	blocks[0] = BTRFS_SUPER_INFO_OFFSET;
	for (i = 1; i < 7; i++) {
		blocks[i] = BTRFS_SUPER_INFO_OFFSET + 1024 * 1024 +
			leafsize * i;
	}

	ret = make_btrfs(fd, file, label, blocks, dev_block_count,
			 nodesize, leafsize,
			 sectorsize, stripesize);
	if (ret) {
		fprintf(stderr, "error during mkfs %d\n", ret);
		exit(1);
	}

	root = open_ctree(file, 0, O_RDWR);
	if (!root) {
		fprintf(stderr, "ctree init failed\n");
		exit(1);
	}
	root->fs_info->alloc_start = alloc_start;

	ret = make_root_dir(root, mixed);
	if (ret) {
		fprintf(stderr, "failed to setup the root directory\n");
		exit(1);
	}

	trans = btrfs_start_transaction(root, 1);

	if (ac == 0)
		goto raid_groups;

	btrfs_register_one_device(file);

	zero_end = 1;
	while(ac-- > 0) {
		int old_mixed = mixed;

		file = av[optind++];
		ret = check_mounted(file);
		if (ret < 0) {
			fprintf(stderr, "error checking %s mount status\n",
				file);
			exit(1);
		}
		if (ret == 1) {
			fprintf(stderr, "%s is mounted\n", file);
			exit(1);
		}
		fd = open(file, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "unable to open %s\n", file);
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
					   block_count, &mixed, nodiscard);
		mixed = old_mixed;
		BUG_ON(ret);

		ret = btrfs_add_to_fsid(trans, root, fd, file, dev_block_count,
					sectorsize, sectorsize, sectorsize);
		BUG_ON(ret);
		btrfs_register_one_device(file);
	}

raid_groups:
	if (!source_dir_set) {
		ret = create_raid_groups(trans, root, data_profile,
				 data_profile_opt, metadata_profile,
				 metadata_profile_opt, mixed);
		BUG_ON(ret);
	}

	ret = create_data_reloc_tree(trans, root);
	BUG_ON(ret);

	if (mixed) {
		struct btrfs_super_block *super = &root->fs_info->super_copy;
		u64 flags = btrfs_super_incompat_flags(super);

		flags |= BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS;
		btrfs_set_super_incompat_flags(super, flags);
	}

	printf("fs created label %s on %s\n\tnodesize %u leafsize %u "
	    "sectorsize %u size %s\n",
	    label, first_file, nodesize, leafsize, sectorsize,
	    pretty_buf = pretty_sizes(btrfs_super_total_bytes(&root->fs_info->super_copy)));
	free(pretty_buf);

	printf("%s\n", BTRFS_BUILD_VERSION);
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

