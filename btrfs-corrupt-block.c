/*
 * Copyright (C) 2009 Oracle.  All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>

#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/transaction.h"
#include "kernel-lib/list.h"
#include "kernel-lib/radix-tree.h"
#include "common/utils.h"
#include "common/help.h"

#define FIELD_BUF_LEN 80

static int debug_corrupt_block(struct extent_buffer *eb,
		struct btrfs_root *root, u64 bytenr, u32 blocksize, u64 copy)
{
	int ret;
	u64 length;
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	int num_copies;
	int mirror_num = 1;

	length = blocksize;
	while (1) {
		ret = btrfs_map_block(root->fs_info, READ, eb->start, &length,
				      &multi, mirror_num, NULL);
		if (ret) {
			error("cannot map block %llu length %llu mirror %d: %d",
					(unsigned long long)eb->start,
					(unsigned long long)length,
					mirror_num, ret);
			return ret;
		}
		device = multi->stripes[0].dev;
		eb->fd = device->fd;
		device->total_ios++;
		eb->dev_bytenr = multi->stripes[0].physical;

		fprintf(stdout,
			"mirror %d logical %llu physical %llu device %s\n",
			mirror_num, (unsigned long long)bytenr,
			(unsigned long long)eb->dev_bytenr, device->name);
		free(multi);

		if (!copy || mirror_num == copy) {
			ret = read_extent_from_disk(eb, 0, eb->len);
			if (ret < 0) {
				errno = -ret;
				error("cannot read eb bytenr %llu: %m",
					(unsigned long long)eb->dev_bytenr);
				return ret;
			}
			printf("corrupting %llu copy %d\n", eb->start,
			       mirror_num);
			memset(eb->data, 0, eb->len);
			ret = write_extent_to_disk(eb);
			if (ret < 0) {
				errno = -ret;
				error("cannot write eb bytenr %llu: %m",
					(unsigned long long)eb->dev_bytenr);
				return ret;
			}
			fsync(eb->fd);
		}

		num_copies = btrfs_num_copies(root->fs_info, eb->start,
					      eb->len);
		if (num_copies == 1)
			break;

		mirror_num++;
		if (mirror_num > num_copies)
			break;
	}

	return 0;
}

static void print_usage(int ret)
{
	printf("usage: btrfs-corrupt-block [options] device\n");
	printf("\t-l   Logical extent to be corrupted\n");
	printf("\t-c   Copy of the extent to be corrupted (usually 1 or 2, default: 0)\n");
	printf("\t-b   Number of bytes to be corrupted\n");
	printf("\t-e   Extent to be corrupted\n");
	printf("\t-E   The whole extent tree to be corrupted\n");
	printf("\t-u   Given chunk item to be corrupted\n");
	printf("\t-U   The whole chunk tree to be corrupted\n");
	printf("\t-i   The inode item to corrupt (must also specify the field to corrupt)\n");
	printf("\t-x   The file extent item to corrupt (must also specify -i for the inode and -f for the field to corrupt)\n");
	printf("\t-m   The metadata block to corrupt (must also specify -f for the field to corrupt)\n");
	printf("\t-K <u64,u8,u64> Corrupt the given key (must also specify -f for the field and optionally -r for the root)\n");
	printf("\t-f   The field in the item to corrupt\n");
	printf("\t-I <u64,u8,u64> Corrupt an item corresponding to the passed key triplet (must also specify the field to corrupt and root for the item)\n");
	printf("\t-D <u64,u8,u64> Corrupt a dir item corresponding to the passed key triplet, must also specify a field\n");
	printf("\t-d <u64,u8,u64> Delete item corresponding to passed key triplet\n");
	printf("\t-r   Operate on this root\n");
	printf("\t-C   Delete a csum for the specified bytenr.  When used with -b it'll delete that many bytes, otherwise it's just sectorsize\n");
	printf("\t--block-group OFFSET  corrupt the given block group\n");
	exit(ret);
}

static void corrupt_keys(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *fs_info,
			 struct extent_buffer *eb)
{
	int slot;
	int bad_slot;
	int nr;
	struct btrfs_disk_key bad_key;;

	nr = btrfs_header_nritems(eb);
	if (nr == 0)
		return;

	slot = rand_range(nr);
	bad_slot = rand_range(nr);

	if (bad_slot == slot)
		return;

	fprintf(stderr,
		"corrupting keys in block %llu slot %d swapping with %d\n",
		(unsigned long long)eb->start, slot, bad_slot);

	if (btrfs_header_level(eb) == 0) {
		btrfs_item_key(eb, &bad_key, bad_slot);
		btrfs_set_item_key(eb, &bad_key, slot);
	} else {
		btrfs_node_key(eb, &bad_key, bad_slot);
		btrfs_set_node_key(eb, &bad_key, slot);
	}
	btrfs_mark_buffer_dirty(eb);
	if (!trans) {
		u16 csum_size = fs_info->csum_size;
		u16 csum_type = fs_info->csum_type;

		csum_tree_block_size(eb, csum_size, 0, csum_type);
		write_extent_to_disk(eb);
	}
}


static int corrupt_keys_in_block(struct btrfs_fs_info *fs_info, u64 bytenr)
{
	struct extent_buffer *eb;

	eb = read_tree_block(fs_info, bytenr, 0);
	if (!extent_buffer_uptodate(eb))
		return -EIO;;

	corrupt_keys(NULL, fs_info, eb);
	free_extent_buffer(eb);
	return 0;
}

static int corrupt_extent(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root, u64 bytenr)
{
	struct btrfs_root *extent_root;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	u32 item_size;
	unsigned long ptr;
	struct btrfs_path *path;
	int ret;
	int slot;
	int should_del = rand_range(3);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = bytenr;
	key.type = (u8)-1;
	key.offset = (u64)-1;

	extent_root = btrfs_extent_root(trans->fs_info, bytenr);
	while(1) {
		ret = btrfs_search_slot(trans, extent_root, &key, path, -1, 1);
		if (ret < 0)
			break;

		if (ret > 0) {
			if (path->slots[0] == 0)
				break;
			path->slots[0]--;
			ret = 0;
		}
		leaf = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid != bytenr)
			break;

		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY &&
		    key.type != BTRFS_TREE_BLOCK_REF_KEY &&
		    key.type != BTRFS_EXTENT_DATA_REF_KEY &&
		    key.type != BTRFS_EXTENT_REF_V0_KEY &&
		    key.type != BTRFS_SHARED_BLOCK_REF_KEY &&
		    key.type != BTRFS_SHARED_DATA_REF_KEY)
			goto next;

		if (should_del) {
			fprintf(stderr,
				"deleting extent record: key %llu %u %llu\n",
				key.objectid, key.type, key.offset);

			if (key.type == BTRFS_EXTENT_ITEM_KEY) {
				/* make sure this extent doesn't get
				 * reused for other purposes */
				btrfs_pin_extent(root->fs_info,
						 key.objectid, key.offset);
			}

			btrfs_del_item(trans, root, path);
		} else {
			fprintf(stderr,
				"corrupting extent record: key %llu %u %llu\n",
				key.objectid, key.type, key.offset);
			ptr = btrfs_item_ptr_offset(leaf, slot);
			item_size = btrfs_item_size_nr(leaf, slot);
			memset_extent_buffer(leaf, 0, ptr, item_size);
			btrfs_mark_buffer_dirty(leaf);
		}
next:
		btrfs_release_path(path);

		if (key.offset > 0)
			key.offset--;
		if (key.offset == 0)
			break;
	}

	btrfs_free_path(path);
	return 0;
}

static void btrfs_corrupt_extent_leaf(struct btrfs_trans_handle *trans,
				      struct btrfs_root *root,
				      struct extent_buffer *eb)
{
	u32 nr = btrfs_header_nritems(eb);
	u32 victim = rand_range(nr);
	u64 objectid;
	struct btrfs_key key;

	btrfs_item_key_to_cpu(eb, &key, victim);
	objectid = key.objectid;
	corrupt_extent(trans, root, objectid);
}

static void btrfs_corrupt_extent_tree(struct btrfs_trans_handle *trans,
				      struct btrfs_root *root,
				      struct extent_buffer *eb)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	int i;

	if (!eb)
		return;

	if (btrfs_is_leaf(eb)) {
		btrfs_corrupt_extent_leaf(trans, root, eb);
		return;
	}

	if (btrfs_header_level(eb) == 1 && eb != root->node) {
		if (rand_range(5))
			return;
	}

	for (i = 0; i < btrfs_header_nritems(eb); i++) {
		struct extent_buffer *next;

		next = read_tree_block(fs_info, btrfs_node_blockptr(eb, i),
				       btrfs_node_ptr_generation(eb, i));
		if (!extent_buffer_uptodate(next))
			continue;
		btrfs_corrupt_extent_tree(trans, root, next);
		free_extent_buffer(next);
	}
}

enum btrfs_inode_field {
	BTRFS_INODE_FIELD_ISIZE,
	BTRFS_INODE_FIELD_NBYTES,
	BTRFS_INODE_FIELD_NLINK,
	BTRFS_INODE_FIELD_GENERATION,
	BTRFS_INODE_FIELD_TRANSID,
	BTRFS_INODE_FIELD_BLOCK_GROUP,
	BTRFS_INODE_FIELD_MODE,
	BTRFS_INODE_FIELD_UID,
	BTRFS_INODE_FIELD_GID,
	BTRFS_INODE_FIELD_BAD,
};

enum btrfs_file_extent_field {
	BTRFS_FILE_EXTENT_DISK_BYTENR,
	BTRFS_FILE_EXTENT_BAD,
};

enum btrfs_dir_item_field {
	BTRFS_DIR_ITEM_NAME,
	BTRFS_DIR_ITEM_LOCATION_OBJECTID,
	BTRFS_DIR_ITEM_BAD,
};

enum btrfs_metadata_block_field {
	BTRFS_METADATA_BLOCK_GENERATION,
	BTRFS_METADATA_BLOCK_SHIFT_ITEMS,
	BTRFS_METADATA_BLOCK_BAD,
};

enum btrfs_item_field {
	BTRFS_ITEM_OFFSET,
	BTRFS_ITEM_BAD,
};

enum btrfs_key_field {
	BTRFS_KEY_OBJECTID,
	BTRFS_KEY_TYPE,
	BTRFS_KEY_OFFSET,
	BTRFS_KEY_BAD,
};

enum btrfs_block_group_field {
	BTRFS_BLOCK_GROUP_ITEM_USED,
	BTRFS_BLOCK_GROUP_ITEM_FLAGS,
	BTRFS_BLOCK_GROUP_ITEM_CHUNK_OBJECTID,
	BTRFS_BLOCK_GROUP_ITEM_BAD,
};

static enum btrfs_block_group_field convert_block_group_field(char *field)
{
	if (!strncmp(field, "used", FIELD_BUF_LEN))
		return BTRFS_BLOCK_GROUP_ITEM_USED;
	if (!strncmp(field, "flags", FIELD_BUF_LEN))
		return BTRFS_BLOCK_GROUP_ITEM_FLAGS;
	if (!strncmp(field, "chunk_objectid", FIELD_BUF_LEN))
		return BTRFS_BLOCK_GROUP_ITEM_CHUNK_OBJECTID;
	return BTRFS_BLOCK_GROUP_ITEM_BAD;
}

static enum btrfs_inode_field convert_inode_field(char *field)
{
	if (!strncmp(field, "isize", FIELD_BUF_LEN))
		return BTRFS_INODE_FIELD_ISIZE;
	if (!strncmp(field, "nbytes", FIELD_BUF_LEN))
		return BTRFS_INODE_FIELD_NBYTES;
	if (!strncmp(field, "nlink", FIELD_BUF_LEN))
		return BTRFS_INODE_FIELD_NLINK;
	if (!strncmp(field, "generation", FIELD_BUF_LEN))
		return BTRFS_INODE_FIELD_GENERATION;
	if (!strncmp(field, "transid", FIELD_BUF_LEN))
		return BTRFS_INODE_FIELD_TRANSID;
	if (!strncmp(field, "block_group", FIELD_BUF_LEN))
		return BTRFS_INODE_FIELD_BLOCK_GROUP;
	if (!strncmp(field, "mode", FIELD_BUF_LEN))
		return BTRFS_INODE_FIELD_MODE;
	if (!strncmp(field, "uid", FIELD_BUF_LEN))
		return BTRFS_INODE_FIELD_UID;
	if (!strncmp(field, "gid", FIELD_BUF_LEN))
		return BTRFS_INODE_FIELD_GID;
	return BTRFS_INODE_FIELD_BAD;
}

static enum btrfs_file_extent_field convert_file_extent_field(char *field)
{
	if (!strncmp(field, "disk_bytenr", FIELD_BUF_LEN))
		return BTRFS_FILE_EXTENT_DISK_BYTENR;
	return BTRFS_FILE_EXTENT_BAD;
}

static enum btrfs_metadata_block_field
convert_metadata_block_field(char *field)
{
	if (!strncmp(field, "generation", FIELD_BUF_LEN))
		return BTRFS_METADATA_BLOCK_GENERATION;
	if (!strncmp(field, "shift_items", FIELD_BUF_LEN))
		return BTRFS_METADATA_BLOCK_SHIFT_ITEMS;
	return BTRFS_METADATA_BLOCK_BAD;
}

static enum btrfs_key_field convert_key_field(char *field)
{
	if (!strncmp(field, "objectid", FIELD_BUF_LEN))
		return BTRFS_KEY_OBJECTID;
	if (!strncmp(field, "type", FIELD_BUF_LEN))
		return BTRFS_KEY_TYPE;
	if (!strncmp(field, "offset", FIELD_BUF_LEN))
		return BTRFS_KEY_OFFSET;
	return BTRFS_KEY_BAD;
}

static enum btrfs_item_field convert_item_field(char *field)
{
	if (!strncmp(field, "offset", FIELD_BUF_LEN))
		return BTRFS_ITEM_OFFSET;
	return BTRFS_ITEM_BAD;
}

static enum btrfs_dir_item_field convert_dir_item_field(char *field)
{
	if (!strncmp(field, "name", FIELD_BUF_LEN))
		return BTRFS_DIR_ITEM_NAME;
	if (!strncmp(field, "location_objectid", FIELD_BUF_LEN))
		return BTRFS_DIR_ITEM_LOCATION_OBJECTID;
	return BTRFS_DIR_ITEM_BAD;
}

static u64 generate_u64(u64 orig)
{
	u64 ret;
	do {
		ret = rand_u64();
	} while (ret == orig);
	return ret;
}

static u32 generate_u32(u32 orig)
{
	u32 ret;
	do {
		ret = rand_u32();
	} while (ret == orig);
	return ret;
}

static u8 generate_u8(u8 orig)
{
	u8 ret;
	do {
		ret = rand_u8();
	} while (ret == orig);
	return ret;
}

static int corrupt_block_group(struct btrfs_root *root, u64 bg, char *field)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_path *path;
	struct btrfs_block_group_item *bgi;
	struct btrfs_key key;
	enum btrfs_block_group_field corrupt_field;
	u64 orig, bogus;
	int ret = 0;

	root = btrfs_extent_root(root->fs_info, 0);

	corrupt_field = convert_block_group_field(field);
	if (corrupt_field == BTRFS_BLOCK_GROUP_ITEM_BAD) {
		fprintf(stderr, "Invalid field %s\n", field);
		return -EINVAL;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		fprintf(stderr, "Couldn't start transaction %ld\n",
			PTR_ERR(trans));
		return PTR_ERR(trans);
	}

	key.objectid = bg;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret < 0) {
		fprintf(stderr, "Error searching for bg %llu %d\n", bg, ret);
		goto out;
	}

	ret = 0;
	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	if (key.type != BTRFS_BLOCK_GROUP_ITEM_KEY) {
		fprintf(stderr, "Couldn't find the bg %llu\n", bg);
		goto out;
	}

	bgi = btrfs_item_ptr(path->nodes[0], path->slots[0],
			     struct btrfs_block_group_item);
	switch (corrupt_field) {
	case BTRFS_BLOCK_GROUP_ITEM_USED:
		orig = btrfs_block_group_used(path->nodes[0], bgi);
		bogus = generate_u64(orig);
		btrfs_set_block_group_used(path->nodes[0], bgi, bogus);
		break;
	case BTRFS_BLOCK_GROUP_ITEM_CHUNK_OBJECTID:
		orig = btrfs_block_group_chunk_objectid(path->nodes[0], bgi);
		bogus = generate_u64(orig);
		btrfs_set_block_group_chunk_objectid(path->nodes[0], bgi,
						     bogus);
		break;
	case BTRFS_BLOCK_GROUP_ITEM_FLAGS:
		orig = btrfs_block_group_flags(path->nodes[0], bgi);
		bogus = generate_u64(orig);
		btrfs_set_block_group_flags(path->nodes[0], bgi, bogus);
		break;
	default:
		ret = -EINVAL;
		goto out;
	}
	btrfs_mark_buffer_dirty(path->nodes[0]);
out:
	btrfs_commit_transaction(trans, root);
	btrfs_free_path(path);
	return ret;
}

static int corrupt_key(struct btrfs_root *root, struct btrfs_key *key,
		       char *field)
{
	enum btrfs_key_field corrupt_field = convert_key_field(field);
	struct btrfs_path *path;
	struct btrfs_trans_handle *trans;
	int ret;

	if (corrupt_field == BTRFS_KEY_BAD) {
		fprintf(stderr, "Invalid field %s\n", field);
		return -EINVAL;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		return PTR_ERR(trans);
	}

	ret = btrfs_search_slot(trans, root, key, path, 0, 1);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		fprintf(stderr, "Couldn't find the key to corrupt\n");
		ret = -ENOENT;
		goto out;
	}

	switch (corrupt_field) {
	case BTRFS_KEY_OBJECTID:
		key->objectid = generate_u64(key->objectid);
		break;
	case BTRFS_KEY_TYPE:
		key->type = generate_u8(key->type);
		break;
	case BTRFS_KEY_OFFSET:
		key->offset = generate_u64(key->objectid);
		break;
	default:
		fprintf(stderr, "Invalid field %s, %d\n", field,
			corrupt_field);
		ret = -EINVAL;
		goto out;
	}

	btrfs_set_item_key_unsafe(root, path, key);
out:
	btrfs_free_path(path);
	btrfs_commit_transaction(trans, root);
	return ret;
}

static int corrupt_dir_item(struct btrfs_root *root, struct btrfs_key *key,
			    char *field)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_dir_item *di;
	struct btrfs_path *path;
	char name[PATH_MAX];
	struct btrfs_key location;
	struct btrfs_disk_key disk_key;
	unsigned long name_ptr;
	enum btrfs_dir_item_field corrupt_field =
		convert_dir_item_field(field);
	u64 bogus;
	u16 name_len;
	int ret;

	if (corrupt_field == BTRFS_DIR_ITEM_BAD) {
		fprintf(stderr, "Invalid field %s\n", field);
		return -EINVAL;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		return PTR_ERR(trans);
	}

	ret = btrfs_search_slot(trans, root, key, path, 0, 1);
	if (ret) {
		if (ret > 0)
			ret = -ENOENT;
		fprintf(stderr, "Error searching for dir item %d\n", ret);
		goto out;
	}

	di = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_dir_item);

	switch (corrupt_field) {
	case BTRFS_DIR_ITEM_NAME:
		name_len = btrfs_dir_name_len(path->nodes[0], di);
		name_ptr = (unsigned long)(di + 1);
		read_extent_buffer(path->nodes[0], name, name_ptr, name_len);
		name[0]++;
		write_extent_buffer(path->nodes[0], name, name_ptr, name_len);
		btrfs_mark_buffer_dirty(path->nodes[0]);
		goto out;
	case BTRFS_DIR_ITEM_LOCATION_OBJECTID:
		btrfs_dir_item_key_to_cpu(path->nodes[0], di, &location);
		bogus = generate_u64(location.objectid);
		location.objectid = bogus;
		btrfs_cpu_key_to_disk(&disk_key, &location);
		btrfs_set_dir_item_key(path->nodes[0], di, &disk_key);
		btrfs_mark_buffer_dirty(path->nodes[0]);
		goto out;
	default:
		ret = -EINVAL;
		goto out;
	}
out:
	btrfs_commit_transaction(trans, root);
	btrfs_free_path(path);
	return ret;
}

static int corrupt_inode(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root, u64 inode, char *field)
{
	struct btrfs_inode_item *ei;
	struct btrfs_path *path;
	struct btrfs_key key;
	enum btrfs_inode_field corrupt_field = convert_inode_field(field);
	u64 bogus;
	u64 orig;
	int ret;

	if (corrupt_field == BTRFS_INODE_FIELD_BAD) {
		fprintf(stderr, "Invalid field %s\n", field);
		return -EINVAL;
	}

	key.objectid = inode;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = (u64)-1;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret < 0)
		goto out;
	if (ret) {
		if (!path->slots[0]) {
			fprintf(stderr, "Couldn't find inode %llu\n", inode);
			ret = -ENOENT;
			goto out;
		}
		path->slots[0]--;
		ret = 0;
	}

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	if (key.objectid != inode) {
		fprintf(stderr, "Couldn't find inode %llu\n", inode);
		ret = -ENOENT;
		goto out;
	}

	ei = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	switch (corrupt_field) {
	case BTRFS_INODE_FIELD_ISIZE:
		orig = btrfs_inode_size(path->nodes[0], ei);
		bogus = generate_u64(orig);
		btrfs_set_inode_size(path->nodes[0], ei, bogus);
		break;
	case BTRFS_INODE_FIELD_NBYTES:
		orig = btrfs_inode_nbytes(path->nodes[0], ei);
		bogus = generate_u64(orig);
		btrfs_set_inode_nbytes(path->nodes[0], ei, bogus);
		break;
	case BTRFS_INODE_FIELD_NLINK:
		orig = btrfs_inode_nlink(path->nodes[0], ei);
		bogus = generate_u32(orig);
		btrfs_set_inode_nlink(path->nodes[0], ei, bogus);
		break;
	case BTRFS_INODE_FIELD_GENERATION:
		orig = btrfs_inode_generation(path->nodes[0], ei);
		bogus = generate_u64(orig);
		btrfs_set_inode_generation(path->nodes[0], ei, bogus);
		break;
	case BTRFS_INODE_FIELD_TRANSID:
		orig = btrfs_inode_transid(path->nodes[0], ei);
		bogus = generate_u64(orig);
		btrfs_set_inode_transid(path->nodes[0], ei, bogus);
		break;
	case BTRFS_INODE_FIELD_BLOCK_GROUP:
		orig = btrfs_inode_block_group(path->nodes[0], ei);
		bogus = generate_u64(orig);
		btrfs_set_inode_block_group(path->nodes[0], ei, bogus);
		break;
	case BTRFS_INODE_FIELD_MODE:
		orig = btrfs_inode_mode(path->nodes[0], ei);
		bogus = generate_u32(orig);
		btrfs_set_inode_mode(path->nodes[0], ei, bogus);
		break;
	case BTRFS_INODE_FIELD_UID:
		orig = btrfs_inode_uid(path->nodes[0], ei);
		bogus = generate_u32(orig);
		btrfs_set_inode_uid(path->nodes[0], ei, bogus);
		break;
	case BTRFS_INODE_FIELD_GID:
		orig = btrfs_inode_gid(path->nodes[0], ei);
		bogus = generate_u32(orig);
		btrfs_set_inode_gid(path->nodes[0], ei, bogus);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	btrfs_mark_buffer_dirty(path->nodes[0]);
out:
	btrfs_free_path(path);
	return ret;
}

static int corrupt_file_extent(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 inode, u64 extent,
			       char *field)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_path *path;
	struct btrfs_key key;
	enum btrfs_file_extent_field corrupt_field;
	u64 bogus;
	u64 orig;
	int ret = 0;

	corrupt_field = convert_file_extent_field(field);
	if (corrupt_field == BTRFS_FILE_EXTENT_BAD) {
		fprintf(stderr, "Invalid field %s\n", field);
		return -EINVAL;
	}

	key.objectid = inode;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = extent;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret < 0)
		goto out;
	if (ret) {
		fprintf(stderr, "Couldn't find extent %llu for inode %llu\n",
			extent, inode);
		ret = -ENOENT;
		goto out;
	}

	fi = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_file_extent_item);
	switch (corrupt_field) {
	case BTRFS_FILE_EXTENT_DISK_BYTENR:
		orig = btrfs_file_extent_disk_bytenr(path->nodes[0], fi);
		bogus = generate_u64(orig);
		btrfs_set_file_extent_disk_bytenr(path->nodes[0], fi, bogus);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	btrfs_mark_buffer_dirty(path->nodes[0]);
out:
	btrfs_free_path(path);
	return ret;
}

static void shift_items(struct btrfs_root *root, struct extent_buffer *eb)
{
	int nritems = btrfs_header_nritems(eb);
	int shift_space = btrfs_leaf_free_space(eb) / 2;
	int slot = nritems / 2;
	int i = 0;
	unsigned int data_end = btrfs_item_offset_nr(eb, nritems - 1);

	/* Shift the item data up to and including slot back by shift space */
	memmove_extent_buffer(eb, btrfs_leaf_data(eb) + data_end - shift_space,
			      btrfs_leaf_data(eb) + data_end,
			      btrfs_item_offset_nr(eb, slot - 1) - data_end);

	/* Now update the item pointers. */
	for (i = nritems - 1; i >= slot; i--) {
		u32 offset = btrfs_item_offset_nr(eb, i);
		offset -= shift_space;
		btrfs_set_item_offset(eb, btrfs_item_nr(i), offset);
	}
}

static int corrupt_metadata_block(struct btrfs_fs_info *fs_info, u64 block,
				  char *field)
{
	struct extent_buffer *eb;
	enum btrfs_metadata_block_field corrupt_field;
	int ret;

	corrupt_field = convert_metadata_block_field(field);
	if (corrupt_field == BTRFS_METADATA_BLOCK_BAD) {
		fprintf(stderr, "Invalid field %s\n", field);
		return -EINVAL;
	}

	eb = read_tree_block(fs_info, block, 0);
	if (!extent_buffer_uptodate(eb)) {
		fprintf(stderr, "Couldn't read in tree block %s\n", field);
		return -EINVAL;
	}

	ret = 0;
	switch (corrupt_field) {
	case BTRFS_METADATA_BLOCK_GENERATION:
		{
		u64 orig = btrfs_header_generation(eb);
		u64 bogus = generate_u64(orig);

		btrfs_set_header_generation(eb, bogus);
		ret = write_and_map_eb(fs_info, eb);
		free_extent_buffer(eb);
		if (ret < 0) {
			errno = -ret;
			fprintf(stderr,
				"failed to write extent buffer at %llu: %m",
				eb->start);
			return ret;
		}
		break;
		}
	case BTRFS_METADATA_BLOCK_SHIFT_ITEMS:
		{
		struct btrfs_trans_handle *trans;
		struct btrfs_root *root;
		struct btrfs_path *path;
		struct btrfs_key key, root_key;
		u64 root_objectid;
		u8 level;

		root_objectid = btrfs_header_owner(eb);
		level = btrfs_header_level(eb);
		if (level)
			btrfs_node_key_to_cpu(eb, &key, 0);
		else
			btrfs_item_key_to_cpu(eb, &key, 0);
		free_extent_buffer(eb);

		root_key.objectid = root_objectid;
		root_key.type = BTRFS_ROOT_ITEM_KEY;
		root_key.offset = (u64)-1;

		root = btrfs_read_fs_root(fs_info, &root_key);
		if (IS_ERR(root)) {
			fprintf(stderr, "Couldn't find owner root %llu\n",
				key.objectid);
			return PTR_ERR(root);
		}

		path = btrfs_alloc_path();
		if (!path)
			return -ENOMEM;

		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			btrfs_free_path(path);
			fprintf(stderr, "Couldn't start transaction %ld\n",
				PTR_ERR(trans));
			return PTR_ERR(trans);
		}

		path->lowest_level = level;
		ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
		if (ret < 0) {
			fprintf(stderr, "Error searching to node %d\n", ret);
			btrfs_free_path(path);
			btrfs_abort_transaction(trans, ret);
			return ret;
		}
		eb = path->nodes[level];
		shift_items(root, path->nodes[level]);
		btrfs_mark_buffer_dirty(path->nodes[level]);
		btrfs_commit_transaction(trans, root);
		break;
		}
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int corrupt_btrfs_item(struct btrfs_root *root, struct btrfs_key *key,
			      char *field)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_path *path;
	enum btrfs_item_field corrupt_field;
	u32 orig, bogus;
	int ret;

	corrupt_field = convert_item_field(field);
	if (corrupt_field == BTRFS_ITEM_BAD) {
		fprintf(stderr, "Invalid field %s\n", field);
		return -EINVAL;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		fprintf(stderr, "Couldn't start transaction %ld\n",
			PTR_ERR(trans));
		return PTR_ERR(trans);
	}

	ret = btrfs_search_slot(trans, root, key, path, 0, 1);
	if (ret != 0) {
		fprintf(stderr, "Error searching to node %d\n", ret);
		goto out;
	}

	ret = 0;
	switch (corrupt_field) {
	case BTRFS_ITEM_OFFSET:
		orig = btrfs_item_offset_nr(path->nodes[0], path->slots[0]);
		bogus = generate_u32(orig);
		btrfs_set_item_offset(path->nodes[0],
				      btrfs_item_nr(path->slots[0]), bogus);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	btrfs_mark_buffer_dirty(path->nodes[0]);
out:
	btrfs_commit_transaction(trans, root);
	btrfs_free_path(path);
	return ret;
}

static int delete_item(struct btrfs_root *root, struct btrfs_key *key)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_path *path;
	int ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		btrfs_free_path(path);
		fprintf(stderr, "Couldn't start transaction %ld\n",
			PTR_ERR(trans));
		return PTR_ERR(trans);
	}

	ret = btrfs_search_slot(trans, root, key, path, -1, 1);
	if (ret) {
		if (ret > 0)
			ret = -ENOENT;
		fprintf(stderr, "Error searching to node %d\n", ret);
		goto out;
	}
	ret = btrfs_del_item(trans, root, path);
	btrfs_mark_buffer_dirty(path->nodes[0]);
out:
	btrfs_commit_transaction(trans, root);
	btrfs_free_path(path);
	return ret;
}

static int delete_csum(struct btrfs_root *root, u64 bytenr, u64 bytes)
{
	struct btrfs_trans_handle *trans;
	int ret;

	root = btrfs_csum_root(root->fs_info, bytenr);
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		fprintf(stderr, "Couldn't start transaction %ld\n",
			PTR_ERR(trans));
		return PTR_ERR(trans);
	}

	ret = btrfs_del_csums(trans, bytenr, bytes);
	if (ret)
		fprintf(stderr, "Error deleting csums %d\n", ret);
	btrfs_commit_transaction(trans, root);
	return ret;
}

/* corrupt item using NO cow.
 * Because chunk recover will recover based on whole partition scanning,
 * If using COW, chunk recover will use the old item to recover,
 * which is still OK but we want to check the ability to rebuild chunk
 * not only restore the old ones */
static int corrupt_item_nocow(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root, struct btrfs_path *path,
		       int del)
{
	int ret = 0;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	unsigned long ptr;
	int slot;
	u32 item_size;

	leaf = path->nodes[0];
	slot = path->slots[0];
	/* Not deleting the first item of a leaf to keep leaf structure */
	if (slot == 0)
		del = 0;
	/* Only accept valid eb */
	if (slot >= btrfs_header_nritems(leaf)) {
		error("invalid eb: no data or slot out of range: %d >= %d",
				slot, btrfs_header_nritems(leaf));
		return -EINVAL;
	}
	btrfs_item_key_to_cpu(leaf, &key, slot);
	if (del) {
		fprintf(stdout, "Deleting key and data [%llu, %u, %llu].\n",
			key.objectid, key.type, key.offset);
		btrfs_del_item(trans, root, path);
	} else {
		fprintf(stdout, "Corrupting key and data [%llu, %u, %llu].\n",
			key.objectid, key.type, key.offset);
		ptr = btrfs_item_ptr_offset(leaf, slot);
		item_size = btrfs_item_size_nr(leaf, slot);
		memset_extent_buffer(leaf, 0, ptr, item_size);
		btrfs_mark_buffer_dirty(leaf);
	}
	return ret;
}
static int corrupt_chunk_tree(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root)
{
	int ret;
	int del;
	int slot;
	struct btrfs_path *path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct extent_buffer *leaf;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = (u64)-1;
	key.offset = (u64)-1;
	key.type = (u8)-1;

	/* Here, cow and ins_len must equals 0 for the following reasons:
	 * 1) chunk recover is based on disk scanning, so COW should be
	 *    disabled in case the original chunk being scanned and
	 *    recovered using the old chunk.
	 * 2) if cow = 0, ins_len must also be set to 0, or BUG_ON will be
	 *    triggered.
	 */
	ret = btrfs_search_slot(trans, root, &key, path, 0, 0);
	BUG_ON(ret == 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching tree\n");
		goto free_out;
	}
	/* corrupt/del dev_item first */
	while (!btrfs_previous_item(root, path, 0, BTRFS_DEV_ITEM_KEY)) {
		slot = path->slots[0];
		leaf = path->nodes[0];
		del = rand_range(3);
		/* Never delete the first item to keep the leaf structure */
		if (path->slots[0] == 0)
			del = 0;
		ret = corrupt_item_nocow(trans, root, path, del);
		if (ret)
			goto free_out;
	}
	btrfs_release_path(path);

	/* Here, cow and ins_len must equals 0 for the following reasons:
	 * 1) chunk recover is based on disk scanning, so COW should be
	 *    disabled in case the original chunk being scanned and
	 *    recovered using the old chunk.
	 * 2) if cow = 0, ins_len must also be set to 0, or BUG_ON will be
	 *    triggered.
	 */
	ret = btrfs_search_slot(trans, root, &key, path, 0, 0);
	BUG_ON(ret == 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching tree\n");
		goto free_out;
	}
	/* corrupt/del chunk then*/
	while (!btrfs_previous_item(root, path, 0, BTRFS_CHUNK_ITEM_KEY)) {
		slot = path->slots[0];
		leaf = path->nodes[0];
		del = rand_range(3);
		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		ret = corrupt_item_nocow(trans, root, path, del);
		if (ret)
			goto free_out;
	}
free_out:
	btrfs_free_path(path);
	return ret;
}
static int find_chunk_offset(struct btrfs_root *root,
		      struct btrfs_path *path, u64 offset)
{
	struct btrfs_key key;
	int ret;

	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = offset;

	/* Here, cow and ins_len must equals 0 for following reasons:
	 * 1) chunk recover is based on disk scanning, so COW should
	 *    be disabled in case the original chunk being scanned
	 *    and recovered using the old chunk.
	 * 2) if cow = 0, ins_len must also be set to 0, or BUG_ON
	 *    will be triggered.
	 */
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret > 0) {
		fprintf(stderr, "Can't find chunk with given offset %llu\n",
			offset);
		goto out;
	}
	if (ret < 0) {
		fprintf(stderr, "Error searching chunk\n");
		goto out;
	}
out:
	return ret;

}

static void parse_key(u64 *objectid, u8 *type, u64 *offset)
{

	int ret = sscanf(optarg, "%llu,%hhu,%llu", objectid, type, offset);
	if (ret != 3) {
	        fprintf(stderr, "error parsing key '%s': %d\n", optarg, errno);
	        print_usage(1);
	}
}

static struct btrfs_root *open_root(struct btrfs_fs_info *fs_info,
				    u64 root_objectid)
{

	struct btrfs_key root_key;
	struct btrfs_root *root;

	root_key.objectid = root_objectid;
	root_key.type = BTRFS_ROOT_ITEM_KEY;
	root_key.offset = (u64)-1;

	root = btrfs_read_fs_root(fs_info, &root_key);
	if (IS_ERR(root)) {
		fprintf(stderr, "couldn't find root %llu\n", root_objectid);
		print_usage(1);
	}

	return root;
}
int main(int argc, char **argv)
{
	struct cache_tree root_cache;
	struct btrfs_key key;
	struct btrfs_root *root, *target_root;
	char *dev;
	/* chunk offset can be 0,so change to (u64)-1 */
	u64 logical = (u64)-1;
	int ret = 0;
	u64 copy = 0;
	u64 bytes = 4096;
	int extent_rec = 0;
	int extent_tree = 0;
	int corrupt_block_keys = 0;
	int chunk_rec = 0;
	int chunk_tree = 0;
	int corrupt_item = 0;
	int corrupt_di = 0;
	int delete = 0;
	int should_corrupt_key = 0;
	u64 metadata_block = 0;
	u64 inode = 0;
	u64 file_extent = (u64)-1;
	u64 root_objectid = 0;
	u64 csum_bytenr = 0;
	u64 block_group = 0;
	char field[FIELD_BUF_LEN];

	field[0] = '\0';
	memset(&key, 0, sizeof(key));

	while(1) {
		int c;
		enum { GETOPT_VAL_BLOCK_GROUP = 256 };
		static const struct option long_options[] = {
			/* { "byte-count", 1, NULL, 'b' }, */
			{ "logical", required_argument, NULL, 'l' },
			{ "copy", required_argument, NULL, 'c' },
			{ "bytes", required_argument, NULL, 'b' },
			{ "extent-record", no_argument, NULL, 'e' },
			{ "extent-tree", no_argument, NULL, 'E' },
			{ "keys", no_argument, NULL, 'k' },
			{ "chunk-record", no_argument, NULL, 'u' },
			{ "chunk-tree", no_argument, NULL, 'U' },
			{ "inode", required_argument, NULL, 'i'},
			{ "file-extent", required_argument, NULL, 'x'},
			{ "metadata-block", required_argument, NULL, 'm'},
			{ "field", required_argument, NULL, 'f'},
			{ "key", required_argument, NULL, 'K'},
			{ "item", no_argument, NULL, 'I'},
			{ "dir-item", no_argument, NULL, 'D'},
			{ "delete", no_argument, NULL, 'd'},
			{ "root", no_argument, NULL, 'r'},
			{ "csum", required_argument, NULL, 'C'},
			{ "block-group", required_argument, NULL, GETOPT_VAL_BLOCK_GROUP},
			{ "help", no_argument, NULL, GETOPT_VAL_HELP},
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "l:c:b:eEkuUi:f:x:m:K:I:D:d:r:C:",
				long_options, NULL);
		if (c < 0)
			break;
		switch(c) {
			case 'l':
				logical = arg_strtou64(optarg);
				break;
			case 'c':
				copy = arg_strtou64(optarg);
				break;
			case 'b':
				bytes = arg_strtou64(optarg);
				break;
			case 'e':
				extent_rec = 1;
				break;
			case 'E':
				extent_tree = 1;
				break;
			case 'k':
				corrupt_block_keys = 1;
				break;
			case 'u':
				chunk_rec = 1;
				break;
			case 'U':
				chunk_tree = 1;
				break;
			case 'i':
				inode = arg_strtou64(optarg);
				break;
			case 'f':
				strncpy(field, optarg, FIELD_BUF_LEN);
				break;
			case 'x':
				file_extent = arg_strtou64(optarg);
				break;
			case 'm':
				metadata_block = arg_strtou64(optarg);
				break;
			case 'K':
				should_corrupt_key = 1;
				parse_key(&key.objectid, &key.type, &key.offset);
				break;
			case 'D':
				corrupt_di = 1;
				parse_key(&key.objectid, &key.type, &key.offset);
				break;
			case 'I':
				corrupt_item = 1;
				parse_key(&key.objectid, &key.type, &key.offset);
				break;
			case 'd':
				delete = 1;
				parse_key(&key.objectid, &key.type, &key.offset);
				break;
			case 'r':
				root_objectid = arg_strtou64(optarg);
				break;
			case 'C':
				csum_bytenr = arg_strtou64(optarg);
				break;
			case GETOPT_VAL_BLOCK_GROUP:
				block_group = arg_strtou64(optarg);
				break;
			case GETOPT_VAL_HELP:
			default:
				print_usage(c != GETOPT_VAL_HELP);
		}
	}
	set_argv0(argv);
	if (check_argc_min(argc - optind, 1))
		return 1;
	dev = argv[optind];

	radix_tree_init();
	cache_tree_init(&root_cache);

	root = open_ctree(dev, 0, OPEN_CTREE_WRITES);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}
	target_root = root;
	if (root_objectid)
		target_root = open_root(root->fs_info, root_objectid);

	if (extent_rec) {
		struct btrfs_trans_handle *trans;

		if (logical == (u64)-1)
			print_usage(1);
		trans = btrfs_start_transaction(root, 1);
		BUG_ON(IS_ERR(trans));
		ret = corrupt_extent(trans, root, logical);
		btrfs_commit_transaction(trans, root);
		goto out_close;
	}
	if (extent_tree) {
		struct btrfs_trans_handle *trans;
		struct btrfs_root *extent_root;

		extent_root = btrfs_extent_root(root->fs_info, 0);
		trans = btrfs_start_transaction(root, 1);
		BUG_ON(IS_ERR(trans));
		btrfs_corrupt_extent_tree(trans, extent_root,
					  extent_root->node);
		btrfs_commit_transaction(trans, root);
		goto out_close;
	}
	if (chunk_rec) {
		struct btrfs_trans_handle *trans;
		struct btrfs_path *path;
		int del;

		if (logical == (u64)-1)
			print_usage(1);
		del = rand_range(3);
		path = btrfs_alloc_path();
		if (!path) {
			fprintf(stderr, "path allocation failed\n");
			goto out_close;
		}

		if (find_chunk_offset(root->fs_info->chunk_root, path,
				      logical) != 0) {
			btrfs_free_path(path);
			goto out_close;
		}
		trans = btrfs_start_transaction(root, 1);
		BUG_ON(IS_ERR(trans));
		ret = corrupt_item_nocow(trans, root->fs_info->chunk_root,
					 path, del);
		if (ret < 0)
			fprintf(stderr, "Failed to corrupt chunk record\n");
		btrfs_commit_transaction(trans, root);
		goto out_close;
	}
	if (chunk_tree) {
		struct btrfs_trans_handle *trans;

		trans = btrfs_start_transaction(root, 1);
		BUG_ON(IS_ERR(trans));
		ret = corrupt_chunk_tree(trans, root->fs_info->chunk_root);
		if (ret < 0)
			fprintf(stderr, "Failed to corrupt chunk tree\n");
		btrfs_commit_transaction(trans, root);
		goto out_close;
	}
	if (inode) {
		struct btrfs_trans_handle *trans;

		if (*field == 0)
			print_usage(1);

		trans = btrfs_start_transaction(root, 1);
		BUG_ON(IS_ERR(trans));
		if (file_extent == (u64)-1) {
			printf("corrupting inode\n");
			ret = corrupt_inode(trans, root, inode, field);
		} else {
			printf("corrupting file extent\n");
			ret = corrupt_file_extent(trans, root, inode,
						  file_extent, field);
		}
		btrfs_commit_transaction(trans, root);
		goto out_close;
	}
	if (metadata_block) {
		if (*field == 0)
			print_usage(1);
		ret = corrupt_metadata_block(root->fs_info, metadata_block,
					     field);
		goto out_close;
	}
	if (corrupt_di) {
		if (!key.objectid || *field == 0)
			print_usage(1);
		ret = corrupt_dir_item(target_root, &key, field);
		goto out_close;
	}
	if (csum_bytenr) {
		ret = delete_csum(root, csum_bytenr, bytes);
		goto out_close;
	}
	if (corrupt_item) {
		if (!key.objectid)
			print_usage(1);
		if (!root_objectid)
			print_usage(1);

		ret = corrupt_btrfs_item(target_root, &key, field);
		goto out_close;
	}
	if (delete) {
		if (!key.objectid)
			print_usage(1);

		ret = delete_item(target_root, &key);
		goto out_close;
	}
	if (should_corrupt_key) {
		if (*field == 0)
			print_usage(1);

		ret = corrupt_key(target_root, &key, field);
		goto out_close;
	}
	if (block_group) {
		if (*field == 0)
			print_usage(1);
		ret = corrupt_block_group(root, block_group, field);
		goto out_close;
	}
	/*
	 * If we made it here and we have extent set then we didn't specify
	 * inode and we're screwed.
	 */
	if (file_extent != (u64)-1)
		print_usage(1);

	if (logical == (u64)-1)
		print_usage(1);

	if (bytes == 0)
		bytes = root->fs_info->sectorsize;

	bytes = round_up(bytes, root->fs_info->sectorsize);

	while (bytes > 0) {
		if (corrupt_block_keys) {
			corrupt_keys_in_block(root->fs_info, logical);
		} else {
			struct extent_buffer *eb;

			eb = btrfs_find_create_tree_block(root->fs_info,
					logical);
			if (!eb) {
				error(
		"not enough memory to allocate extent buffer for bytenr %llu",
					(unsigned long long)logical);
				ret = 1;
				goto out_close;
			}

			debug_corrupt_block(eb, root, logical,
					    root->fs_info->sectorsize, copy);
			free_extent_buffer(eb);
		}
		logical += root->fs_info->sectorsize;
		bytes -= root->fs_info->sectorsize;
	}
	return ret;
out_close:
	close_ctree(root);
	return ret;
}
