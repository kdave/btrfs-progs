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

#define _XOPEN_SOURCE 600
#define _GNU_SOURCE 1

#include "kerncompat.h"

#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/acl.h>
#include <fcntl.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <linux/fs.h>

#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "crc32c.h"
#include "utils.h"
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_ext_attr.h>

#define INO_OFFSET (BTRFS_FIRST_FREE_OBJECTID - EXT2_ROOT_INO)
#define STRIPE_LEN (64 * 1024)
#define EXT2_IMAGE_SUBVOL_OBJECTID BTRFS_FIRST_FREE_OBJECTID

/*
 * Open Ext2fs in readonly mode, read block allocation bitmap and
 * inode bitmap into memory.
 */
static int open_ext2fs(const char *name, ext2_filsys *ret_fs)
{
	errcode_t ret;
	ext2_filsys ext2_fs;
	ext2_ino_t ino;
	ret = ext2fs_open(name, 0, 0, 0, unix_io_manager, &ext2_fs);
	if (ret) {
		fprintf(stderr, "ext2fs_open: %s\n", error_message(ret));
		goto fail;
	}
	ret = ext2fs_read_inode_bitmap(ext2_fs);
	if (ret) {
		fprintf(stderr, "ext2fs_read_inode_bitmap: %s\n",
			error_message(ret));
		goto fail;
	}
	ret = ext2fs_read_block_bitmap(ext2_fs);
	if (ret) {
		fprintf(stderr, "ext2fs_read_block_bitmap: %s\n",
			error_message(ret));
		goto fail;
	}
	/*
	 * search each block group for a free inode. this set up
	 * uninit block/inode bitmaps appropriately.
	 */
	ino = 1;
	while (ino <= ext2_fs->super->s_inodes_count) {
		ext2_ino_t foo;
		ext2fs_new_inode(ext2_fs, ino, 0, NULL, &foo);
		ino += EXT2_INODES_PER_GROUP(ext2_fs->super);
	}

	*ret_fs = ext2_fs;
	return 0;
fail:
	return -1;
}

static int close_ext2fs(ext2_filsys fs)
{
	ext2fs_close(fs);
	return 0;
}

static int ext2_alloc_block(ext2_filsys fs, u64 goal, u64 *block_ret)
{
	blk_t block;

	if (!ext2fs_new_block(fs, goal, NULL, &block)) {
		ext2fs_fast_mark_block_bitmap(fs->block_map, block);
		*block_ret = block;
		return 0;
	}
	return -ENOSPC;
}

static int ext2_free_block(ext2_filsys fs, u64 block)
{
	BUG_ON(block != (blk_t)block);
	ext2fs_fast_unmark_block_bitmap(fs->block_map, block);
	return 0;
}

static int cache_free_extents(struct btrfs_root *root, ext2_filsys ext2_fs)

{
	int i, ret = 0;
	blk_t block;
	u64 bytenr;
	u64 blocksize = ext2_fs->blocksize;

	block = ext2_fs->super->s_first_data_block;
	for (; block < ext2_fs->super->s_blocks_count; block++) {
		if (ext2fs_fast_test_block_bitmap(ext2_fs->block_map, block))
			continue;
		bytenr = block * blocksize;
		ret = set_extent_dirty(&root->fs_info->free_space_cache,
				       bytenr, bytenr + blocksize - 1, 0);
		BUG_ON(ret);
	}

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		bytenr &= ~((u64)STRIPE_LEN - 1);
		if (bytenr >= blocksize * ext2_fs->super->s_blocks_count)
			break;
		clear_extent_dirty(&root->fs_info->free_space_cache, bytenr,
				   bytenr + STRIPE_LEN - 1, 0);
	}

	clear_extent_dirty(&root->fs_info->free_space_cache,
			   0, BTRFS_SUPER_INFO_OFFSET - 1, 0);

	return 0;
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

	while(1) {
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

static int intersect_with_sb(u64 bytenr, u64 num_bytes)
{
	int i;
	u64 offset;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		offset = btrfs_sb_offset(i);
		offset &= ~((u64)STRIPE_LEN - 1);

		if (bytenr < offset + STRIPE_LEN &&
		    bytenr + num_bytes > offset)
			return 1;
	}
	return 0;
}

static int custom_free_extent(struct btrfs_root *root, u64 bytenr,
			      u64 num_bytes)
{
	return intersect_with_sb(bytenr, num_bytes);
}

struct btrfs_extent_ops extent_ops = {
	.alloc_extent = custom_alloc_extent,
	.free_extent = custom_free_extent,
};

struct dir_iterate_data {
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	struct btrfs_inode_item *inode;
	u64 objectid;
	u64 index_cnt;
	u64 parent;
	int errcode;
};

static u8 filetype_conversion_table[EXT2_FT_MAX] = {
	[EXT2_FT_UNKNOWN]	= BTRFS_FT_UNKNOWN,
	[EXT2_FT_REG_FILE]	= BTRFS_FT_REG_FILE,
	[EXT2_FT_DIR]		= BTRFS_FT_DIR,
	[EXT2_FT_CHRDEV]	= BTRFS_FT_CHRDEV,
	[EXT2_FT_BLKDEV]	= BTRFS_FT_BLKDEV,
	[EXT2_FT_FIFO]		= BTRFS_FT_FIFO,
	[EXT2_FT_SOCK]		= BTRFS_FT_SOCK,
	[EXT2_FT_SYMLINK]	= BTRFS_FT_SYMLINK,
};

static int dir_iterate_proc(ext2_ino_t dir, int entry,
			    struct ext2_dir_entry *dirent,
			    int offset, int blocksize,
			    char *buf,void *priv_data)
{
	int ret;
	int file_type;
	u64 objectid;
        u64 inode_size;
	char dotdot[] = "..";
	struct btrfs_key location;
	struct dir_iterate_data *idata = (struct dir_iterate_data *)priv_data;
	int name_len;

	name_len = dirent->name_len & 0xFF;

	objectid = dirent->inode + INO_OFFSET;
	if (!strncmp(dirent->name, dotdot, name_len)) {
		if (name_len == 2) {
			BUG_ON(idata->parent != 0);
			idata->parent = objectid;
		}
		return 0;
	}
	if (dirent->inode < EXT2_GOOD_OLD_FIRST_INO)
		return 0;

	location.objectid = objectid;
	location.offset = 0;
	btrfs_set_key_type(&location, BTRFS_INODE_ITEM_KEY);

	file_type = dirent->name_len >> 8;
	BUG_ON(file_type > EXT2_FT_SYMLINK);
	ret = btrfs_insert_dir_item(idata->trans, idata->root,
				    dirent->name, name_len,
				    idata->objectid, &location,
				    filetype_conversion_table[file_type],
				    idata->index_cnt);
	if (ret)
		goto fail;
	ret = btrfs_insert_inode_ref(idata->trans, idata->root,
				     dirent->name, name_len,
				     objectid, idata->objectid,
				     idata->index_cnt);
	if (ret)
		goto fail;
	idata->index_cnt++;
	inode_size = btrfs_stack_inode_size(idata->inode) +
		     name_len * 2;
	btrfs_set_stack_inode_size(idata->inode, inode_size);
	return 0;
fail:
	idata->errcode = ret;
	return BLOCK_ABORT;
}

static int create_dir_entries(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *btrfs_inode,
			      ext2_filsys ext2_fs, ext2_ino_t ext2_ino)
{
	int ret;
	errcode_t err;
	struct dir_iterate_data data = {
		.trans		= trans,
		.root		= root,
		.inode		= btrfs_inode,
		.objectid	= objectid,
		.index_cnt	= 2,
		.parent		= 0,
		.errcode	= 0,
	};

	err = ext2fs_dir_iterate2(ext2_fs, ext2_ino, 0, NULL,
				  dir_iterate_proc, &data);
	if (err)
		goto error;
	ret = data.errcode;
	if (ret == 0 && data.parent == objectid) {
		ret = btrfs_insert_inode_ref(trans, root, "..", 2,
					     objectid, objectid, 0);
	}
	return ret;
error:
	fprintf(stderr, "ext2fs_dir_iterate2: %s\n", error_message(err));
	return -1;
}

static int read_disk_extent(struct btrfs_root *root, u64 bytenr,
		            u32 num_bytes, char *buffer)
{
	int ret;
	struct btrfs_fs_devices *fs_devs = root->fs_info->fs_devices;

	ret = pread(fs_devs->latest_bdev, buffer, num_bytes, bytenr);
	if (ret != num_bytes)
		goto fail;
	ret = 0;
fail:
	if (ret > 0)
		ret = -1;
	return ret;
}
/*
 * Record a file extent. Do all the required works, such as inserting
 * file extent item, inserting extent item and backref item into extent
 * tree and updating block accounting.
 */
static int record_file_extent(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *inode,
			      u64 file_pos, u64 disk_bytenr,
			      u64 num_bytes, int checksum)
{
	int ret;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key ins_key;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	u32 blocksize = root->sectorsize;
	u64 nbytes;

	if (disk_bytenr == 0) {
		ret = btrfs_insert_file_extent(trans, root, objectid,
						file_pos, disk_bytenr,
						num_bytes, num_bytes);
		return ret;
	}

	btrfs_init_path(&path);

	if (checksum) {
		u64 offset;
		char *buffer;

		ret = -ENOMEM;
		buffer = malloc(blocksize);
		if (!buffer)
			goto fail;
		for (offset = 0; offset < num_bytes; offset += blocksize) {
			ret = read_disk_extent(root, disk_bytenr + offset,
						blocksize, buffer);
			if (ret)
				break;
			ret = btrfs_csum_file_block(trans,
						    root->fs_info->csum_root,
						    disk_bytenr + num_bytes,
						    disk_bytenr + offset,
						    buffer, blocksize);
			if (ret)
				break;
		}
		free(buffer);
		if (ret)
			goto fail;
	}

	ins_key.objectid = objectid;
	ins_key.offset = file_pos;
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

	nbytes = btrfs_stack_inode_nbytes(inode) + num_bytes;
	btrfs_set_stack_inode_nbytes(inode, nbytes);

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
		btrfs_set_extent_generation(leaf, ei, 0);
		btrfs_set_extent_flags(leaf, ei, BTRFS_EXTENT_FLAG_DATA);

		btrfs_mark_buffer_dirty(leaf);

		ret = btrfs_update_block_group(trans, root, disk_bytenr,
					       num_bytes, 1, 0);
		if (ret)
			goto fail;
	} else if (ret != -EEXIST) {
		goto fail;
	}
	btrfs_extent_post_op(trans, extent_root);

	ret = btrfs_inc_extent_ref(trans, root, disk_bytenr, num_bytes, 0,
				   root->root_key.objectid,
				   objectid, file_pos);
	if (ret)
		goto fail;
	ret = 0;
fail:
	btrfs_release_path(root, &path);
	return ret;
}

static int record_file_blocks(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *inode,
			      u64 file_block, u64 disk_block,
			      u64 num_blocks, int checksum)
{
	u64 file_pos = file_block * root->sectorsize;
	u64 disk_bytenr = disk_block * root->sectorsize;
	u64 num_bytes = num_blocks * root->sectorsize;
	return record_file_extent(trans, root, objectid, inode, file_pos,
				  disk_bytenr, num_bytes, checksum);
}

struct blk_iterate_data {
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	struct btrfs_inode_item *inode;
	u64 objectid;
	u64 first_block;
	u64 disk_block;
	u64 num_blocks;
	u64 boundary;
	int checksum;
	int errcode;
};

static int block_iterate_proc(ext2_filsys ext2_fs,
			      u64 disk_block, u64 file_block,
		              struct blk_iterate_data *idata)
{
	int ret;
	int sb_region;
	int do_barrier;
	struct btrfs_root *root = idata->root;
	struct btrfs_trans_handle *trans = idata->trans;
	struct btrfs_block_group_cache *cache;
	u64 bytenr = disk_block * root->sectorsize;

	sb_region = intersect_with_sb(bytenr, root->sectorsize);
	do_barrier = sb_region || disk_block >= idata->boundary;
	if ((idata->num_blocks > 0 && do_barrier) ||
	    (file_block > idata->first_block + idata->num_blocks) ||
	    (disk_block != idata->disk_block + idata->num_blocks)) {
		if (idata->num_blocks > 0) {
			ret = record_file_blocks(trans, root, idata->objectid,
					idata->inode, idata->first_block,
					idata->disk_block, idata->num_blocks,
					idata->checksum);
			if (ret)
				goto fail;
			idata->first_block += idata->num_blocks;
			idata->num_blocks = 0;
		}
		if (file_block > idata->first_block) {
			ret = record_file_blocks(trans, root, idata->objectid,
					idata->inode, idata->first_block,
					0, file_block - idata->first_block,
					idata->checksum);
			if (ret)
				goto fail;
		}

		if (sb_region) {
			bytenr += STRIPE_LEN - 1;
			bytenr &= ~((u64)STRIPE_LEN - 1);
		} else {
			cache = btrfs_lookup_block_group(root->fs_info, bytenr);
			BUG_ON(!cache);
			bytenr = cache->key.objectid + cache->key.offset;
		}

		idata->first_block = file_block;
		idata->disk_block = disk_block;
		idata->boundary = bytenr / root->sectorsize;
	}
	idata->num_blocks++;
	return 0;
fail:
	idata->errcode = ret;
	return BLOCK_ABORT;
}

static int __block_iterate_proc(ext2_filsys fs, blk_t *blocknr,
			        e2_blkcnt_t blockcnt, blk_t ref_block,
			        int ref_offset, void *priv_data)
{
	struct blk_iterate_data *idata;
	idata = (struct blk_iterate_data *)priv_data;
	return block_iterate_proc(fs, *blocknr, blockcnt, idata);
}

/*
 * traverse file's data blocks, record these data blocks as file extents.
 */
static int create_file_extents(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 objectid,
			       struct btrfs_inode_item *btrfs_inode,
			       ext2_filsys ext2_fs, ext2_ino_t ext2_ino,
			       int datacsum, int packing)
{
	int ret;
	char *buffer = NULL;
	errcode_t err;
	u32 last_block;
	u32 sectorsize = root->sectorsize;
	u64 inode_size = btrfs_stack_inode_size(btrfs_inode);
	struct blk_iterate_data data = {
		.trans		= trans,
		.root		= root,
		.inode		= btrfs_inode,
		.objectid	= objectid,
		.first_block	= 0,
		.disk_block	= 0,
		.num_blocks	= 0,
		.boundary	= (u64)-1,
		.checksum	= datacsum,
		.errcode	= 0,
	};
	err = ext2fs_block_iterate2(ext2_fs, ext2_ino, BLOCK_FLAG_DATA_ONLY,
				    NULL, __block_iterate_proc, &data);
	if (err)
		goto error;
	ret = data.errcode;
	if (ret)
		goto fail;
	if (packing && data.first_block == 0 && data.num_blocks > 0 &&
	    inode_size <= BTRFS_MAX_INLINE_DATA_SIZE(root)) {
		u64 num_bytes = data.num_blocks * sectorsize;
		u64 disk_bytenr = data.disk_block * sectorsize;
		u64 nbytes;

		buffer = malloc(num_bytes);
		if (!buffer)
			return -ENOMEM;
		ret = read_disk_extent(root, disk_bytenr, num_bytes, buffer);
		if (ret)
			goto fail;
		if (num_bytes > inode_size)
			num_bytes = inode_size;
		ret = btrfs_insert_inline_extent(trans, root, objectid,
						 0, buffer, num_bytes);
		if (ret)
			goto fail;
		nbytes = btrfs_stack_inode_nbytes(btrfs_inode) + num_bytes;
		btrfs_set_stack_inode_nbytes(btrfs_inode, nbytes);
	} else if (data.num_blocks > 0) {
		ret = record_file_blocks(trans, root, objectid, btrfs_inode,
					 data.first_block, data.disk_block,
					 data.num_blocks, data.checksum);
		if (ret)
			goto fail;
	}
	data.first_block += data.num_blocks;
	last_block = (inode_size + sectorsize - 1) / sectorsize;
	if (last_block > data.first_block) {
		ret = record_file_blocks(trans, root, objectid, btrfs_inode,
					 data.first_block, 0, last_block -
					 data.first_block, data.checksum);
	}
fail:
	if (buffer)
		free(buffer);
	return ret;
error:
	fprintf(stderr, "ext2fs_block_iterate2: %s\n", error_message(err));
	return -1;
}

static int create_symbol_link(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *btrfs_inode,
			      ext2_filsys ext2_fs, ext2_ino_t ext2_ino,
			      struct ext2_inode *ext2_inode)
{
	int ret;
	char *pathname;
	u64 inode_size = btrfs_stack_inode_size(btrfs_inode);
	if (ext2fs_inode_data_blocks(ext2_fs, ext2_inode)) {
		btrfs_set_stack_inode_size(btrfs_inode, inode_size + 1);
		ret = create_file_extents(trans, root, objectid, btrfs_inode,
					  ext2_fs, ext2_ino, 1, 1);
		btrfs_set_stack_inode_size(btrfs_inode, inode_size);
		return ret;
	}

	pathname = (char *)&(ext2_inode->i_block[0]);
	BUG_ON(pathname[inode_size] != 0);
	ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
					 pathname, inode_size + 1);
	btrfs_set_stack_inode_nbytes(btrfs_inode, inode_size + 1);
	return ret;
}

/*
 * Following xattr/acl related codes are based on codes in
 * fs/ext3/xattr.c and fs/ext3/acl.c
 */
#define EXT2_XATTR_BHDR(ptr) ((struct ext2_ext_attr_header *)(ptr))
#define EXT2_XATTR_BFIRST(ptr) \
	((struct ext2_ext_attr_entry *)(EXT2_XATTR_BHDR(ptr) + 1))
#define EXT2_XATTR_IHDR(inode) \
	((struct ext2_ext_attr_header *) ((void *)(inode) + \
		EXT2_GOOD_OLD_INODE_SIZE + (inode)->i_extra_isize))
#define EXT2_XATTR_IFIRST(inode) \
	((struct ext2_ext_attr_entry *) ((void *)EXT2_XATTR_IHDR(inode) + \
		sizeof(EXT2_XATTR_IHDR(inode)->h_magic)))

static int ext2_xattr_check_names(struct ext2_ext_attr_entry *entry,
				  const void *end)
{
	struct ext2_ext_attr_entry *next;

	while (!EXT2_EXT_IS_LAST_ENTRY(entry)) {
		next = EXT2_EXT_ATTR_NEXT(entry);
		if ((void *)next >= end)
			return -EIO;
		entry = next;
	}
	return 0;
}

static int ext2_xattr_check_block(const char *buf, size_t size)
{
	int error;
	struct ext2_ext_attr_header *header = EXT2_XATTR_BHDR(buf);

	if (header->h_magic != EXT2_EXT_ATTR_MAGIC ||
	    header->h_blocks != 1)
		return -EIO;
	error = ext2_xattr_check_names(EXT2_XATTR_BFIRST(buf), buf + size);
	return error;
}

static int ext2_xattr_check_entry(struct ext2_ext_attr_entry *entry,
				  size_t size)
{
	size_t value_size = entry->e_value_size;

	if (entry->e_value_block != 0 || value_size > size ||
	    entry->e_value_offs + value_size > size)
		return -EIO;
	return 0;
}

#define EXT2_ACL_VERSION	0x0001

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
	__le32		e_id;
} ext2_acl_entry;

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
} ext2_acl_entry_short;

typedef struct {
	__le32		a_version;
} ext2_acl_header;

static inline int ext2_acl_count(size_t size)
{
	ssize_t s;
	size -= sizeof(ext2_acl_header);
	s = size - 4 * sizeof(ext2_acl_entry_short);
	if (s < 0) {
		if (size % sizeof(ext2_acl_entry_short))
			return -1;
		return size / sizeof(ext2_acl_entry_short);
	} else {
		if (s % sizeof(ext2_acl_entry))
			return -1;
		return s / sizeof(ext2_acl_entry) + 4;
	}
}

#define ACL_EA_VERSION		0x0002

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
	__le32		e_id;
} acl_ea_entry;

typedef struct {
	__le32		a_version;
	acl_ea_entry	a_entries[0];
} acl_ea_header;

static inline size_t acl_ea_size(int count)
{
	return sizeof(acl_ea_header) + count * sizeof(acl_ea_entry);
}

static int ext2_acl_to_xattr(void *dst, const void *src,
			     size_t dst_size, size_t src_size)
{
	int i, count;
	const void *end = src + src_size;
	acl_ea_header *ext_acl = (acl_ea_header *)dst;
	acl_ea_entry *dst_entry = ext_acl->a_entries;
	ext2_acl_entry *src_entry;

	if (src_size < sizeof(ext2_acl_header))
		goto fail;
	if (((ext2_acl_header *)src)->a_version !=
	    cpu_to_le32(EXT2_ACL_VERSION))
		goto fail;
	src += sizeof(ext2_acl_header);
	count = ext2_acl_count(src_size);
	if (count <= 0)
		goto fail;

	BUG_ON(dst_size < acl_ea_size(count));
	ext_acl->a_version = cpu_to_le32(ACL_EA_VERSION);
	for (i = 0; i < count; i++, dst_entry++) {
		src_entry = (ext2_acl_entry *)src;
		if (src + sizeof(ext2_acl_entry_short) > end)
			goto fail;
		dst_entry->e_tag = src_entry->e_tag;
		dst_entry->e_perm = src_entry->e_perm;
		switch (le16_to_cpu(src_entry->e_tag)) {
		case ACL_USER_OBJ:
		case ACL_GROUP_OBJ:
		case ACL_MASK:
		case ACL_OTHER:
			src += sizeof(ext2_acl_entry_short);
			dst_entry->e_id = cpu_to_le32(ACL_UNDEFINED_ID);
			break;
		case ACL_USER:
		case ACL_GROUP:
			src += sizeof(ext2_acl_entry);
			if (src > end)
				goto fail;
			dst_entry->e_id = src_entry->e_id;
			break;
		default:
			goto fail;
		}
	}
	if (src != end)
		goto fail;
	return 0;
fail:
	return -EINVAL;
}

static char *xattr_prefix_table[] = {
	[1] =	"user.",
	[2] =	"system.posix_acl_access",
	[3] =	"system.posix_acl_default",
	[4] =	"trusted.",
	[6] =	"security.",
};

static int copy_single_xattr(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 objectid,
			     struct ext2_ext_attr_entry *entry,
			     const void *data, u32 datalen)
{
	int ret = 0;
	int name_len;
	int name_index;
	void *databuf = NULL;
	char namebuf[XATTR_NAME_MAX + 1];

	name_index = entry->e_name_index;
	if (name_index >= ARRAY_SIZE(xattr_prefix_table) ||
	    xattr_prefix_table[name_index] == NULL)
		return -EOPNOTSUPP;
	name_len = strlen(xattr_prefix_table[name_index]) +
		   entry->e_name_len;
	if (name_len >= sizeof(namebuf))
		return -ERANGE;

	if (name_index == 2 || name_index == 3) {
		size_t bufsize = acl_ea_size(ext2_acl_count(datalen));
		databuf = malloc(bufsize);
		if (!databuf)
		       return -ENOMEM;
		ret = ext2_acl_to_xattr(databuf, data, bufsize, datalen);
		if (ret)
			goto out;
		data = databuf;
		datalen = bufsize;
	}
	strncpy(namebuf, xattr_prefix_table[name_index], XATTR_NAME_MAX);
	strncat(namebuf, EXT2_EXT_ATTR_NAME(entry), entry->e_name_len);
	if (name_len + datalen > BTRFS_LEAF_DATA_SIZE(root) -
	    sizeof(struct btrfs_item) - sizeof(struct btrfs_dir_item)) {
		fprintf(stderr, "skip large xattr on inode %Lu name %.*s\n",
			objectid - INO_OFFSET, name_len, namebuf);
		goto out;
	}
	ret = btrfs_insert_xattr_item(trans, root, namebuf, name_len,
				      data, datalen, objectid);
out:
	if (databuf)
		free(databuf);
	return ret;
}

static int copy_extended_attrs(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 objectid,
			       struct btrfs_inode_item *btrfs_inode,
			       ext2_filsys ext2_fs, ext2_ino_t ext2_ino)
{
	int ret = 0;
	int inline_ea = 0;
	errcode_t err;
	u32 datalen;
	u32 block_size = ext2_fs->blocksize;
	u32 inode_size = EXT2_INODE_SIZE(ext2_fs->super);
	struct ext2_inode_large *ext2_inode;
	struct ext2_ext_attr_entry *entry;
	void *data;
	char *buffer = NULL;
	char inode_buf[EXT2_GOOD_OLD_INODE_SIZE];

	if (inode_size <= EXT2_GOOD_OLD_INODE_SIZE) {
		ext2_inode = (struct ext2_inode_large *)inode_buf;
	} else {
		ext2_inode = (struct ext2_inode_large *)malloc(inode_size);
		if (!ext2_inode)
		       return -ENOMEM;
	}
	err = ext2fs_read_inode_full(ext2_fs, ext2_ino, (void *)ext2_inode,
				     inode_size);
	if (err) {
		fprintf(stderr, "ext2fs_read_inode_full: %s\n",
			error_message(err));
		ret = -1;
		goto out;
	}

	if (ext2_ino > ext2_fs->super->s_first_ino &&
	    inode_size > EXT2_GOOD_OLD_INODE_SIZE) {
		if (EXT2_GOOD_OLD_INODE_SIZE +
		    ext2_inode->i_extra_isize > inode_size) {
			ret = -EIO;
			goto out;
		}
		if (ext2_inode->i_extra_isize != 0 &&
		    EXT2_XATTR_IHDR(ext2_inode)->h_magic ==
		    EXT2_EXT_ATTR_MAGIC) {
			inline_ea = 1;
		}
	}
	if (inline_ea) {
		int total;
		void *end = (void *)ext2_inode + inode_size;
		entry = EXT2_XATTR_IFIRST(ext2_inode);
		total = end - (void *)entry;
		ret = ext2_xattr_check_names(entry, end);
		if (ret)
			goto out;
		while (!EXT2_EXT_IS_LAST_ENTRY(entry)) {
			ret = ext2_xattr_check_entry(entry, total);
			if (ret)
				goto out;
			data = (void *)EXT2_XATTR_IFIRST(ext2_inode) +
				entry->e_value_offs;
			datalen = entry->e_value_size;
			ret = copy_single_xattr(trans, root, objectid,
						entry, data, datalen);
			if (ret)
				goto out;
			entry = EXT2_EXT_ATTR_NEXT(entry);
		}
	}

	if (ext2_inode->i_file_acl == 0)
		goto out;

	buffer = malloc(block_size);
	if (!buffer) {
		ret = -ENOMEM;
		goto out;
	}
	err = ext2fs_read_ext_attr(ext2_fs, ext2_inode->i_file_acl, buffer);
	if (err) {
		fprintf(stderr, "ext2fs_read_ext_attr: %s\n",
			error_message(err));
		ret = -1;
		goto out;
	}
	ret = ext2_xattr_check_block(buffer, block_size);
	if (ret)
		goto out;

	entry = EXT2_XATTR_BFIRST(buffer);
	while (!EXT2_EXT_IS_LAST_ENTRY(entry)) {
		ret = ext2_xattr_check_entry(entry, block_size);
		if (ret)
			goto out;
		data = buffer + entry->e_value_offs;
		datalen = entry->e_value_size;
		ret = copy_single_xattr(trans, root, objectid,
					entry, data, datalen);
		if (ret)
			goto out;
		entry = EXT2_EXT_ATTR_NEXT(entry);
	}
out:
	if (buffer != NULL)
		free(buffer);
	if ((void *)ext2_inode != inode_buf)
		free(ext2_inode);
	return ret;
}
#define MINORBITS	20
#define MKDEV(ma, mi)	(((ma) << MINORBITS) | (mi))

static inline dev_t old_decode_dev(u16 val)
{
	return MKDEV((val >> 8) & 255, val & 255);
}

static inline dev_t new_decode_dev(u32 dev)
{
	unsigned major = (dev & 0xfff00) >> 8;
	unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);
	return MKDEV(major, minor);
}

static int copy_inode_item(struct btrfs_inode_item *dst,
			   struct ext2_inode *src, u32 blocksize)
{
	btrfs_set_stack_inode_generation(dst, 1);
	btrfs_set_stack_inode_size(dst, src->i_size);
	btrfs_set_stack_inode_nbytes(dst, 0);
	btrfs_set_stack_inode_block_group(dst, 0);
	btrfs_set_stack_inode_nlink(dst, src->i_links_count);
	btrfs_set_stack_inode_uid(dst, src->i_uid | (src->i_uid_high << 16));
	btrfs_set_stack_inode_gid(dst, src->i_gid | (src->i_gid_high << 16));
	btrfs_set_stack_inode_mode(dst, src->i_mode);
	btrfs_set_stack_inode_rdev(dst, 0);
	btrfs_set_stack_inode_flags(dst, 0);
	btrfs_set_stack_timespec_sec(&dst->atime, src->i_atime);
	btrfs_set_stack_timespec_nsec(&dst->atime, 0);
	btrfs_set_stack_timespec_sec(&dst->ctime, src->i_ctime);
	btrfs_set_stack_timespec_nsec(&dst->ctime, 0);
	btrfs_set_stack_timespec_sec(&dst->mtime, src->i_mtime);
	btrfs_set_stack_timespec_nsec(&dst->mtime, 0);
	btrfs_set_stack_timespec_sec(&dst->otime, 0);
	btrfs_set_stack_timespec_nsec(&dst->otime, 0);

	if (S_ISDIR(src->i_mode)) {
		btrfs_set_stack_inode_size(dst, 0);
		btrfs_set_stack_inode_nlink(dst, 1);
	}
	if (S_ISREG(src->i_mode)) {
		btrfs_set_stack_inode_size(dst, (u64)src->i_size_high << 32 |
					   (u64)src->i_size);
	}
	if (!S_ISREG(src->i_mode) && !S_ISDIR(src->i_mode) &&
	    !S_ISLNK(src->i_mode)) {
		if (src->i_block[0]) {
			btrfs_set_stack_inode_rdev(dst,
				old_decode_dev(src->i_block[0]));
		} else {
			btrfs_set_stack_inode_rdev(dst,
				new_decode_dev(src->i_block[1]));
		}
	}
	return 0;
}

/*
 * copy a single inode. do all the required works, such as cloning
 * inode item, creating file extents and creating directory entries.
 */
static int copy_single_inode(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 objectid,
			     ext2_filsys ext2_fs, ext2_ino_t ext2_ino,
			     struct ext2_inode *ext2_inode,
			     int datacsum, int packing, int noxattr)
{
	int ret;
	struct btrfs_key inode_key;
	struct btrfs_inode_item btrfs_inode;

	if (ext2_inode->i_links_count == 0)
		return 0;

	copy_inode_item(&btrfs_inode, ext2_inode, ext2_fs->blocksize);
	if (!datacsum && S_ISREG(ext2_inode->i_mode)) {
		u32 flags = btrfs_stack_inode_flags(&btrfs_inode) |
			    BTRFS_INODE_NODATASUM;
		btrfs_set_stack_inode_flags(&btrfs_inode, flags);
	}

	switch (ext2_inode->i_mode & S_IFMT) {
	case S_IFREG:
		ret = create_file_extents(trans, root, objectid, &btrfs_inode,
					ext2_fs, ext2_ino, datacsum, packing);
		break;
	case S_IFDIR:
		ret = create_dir_entries(trans, root, objectid, &btrfs_inode,
					 ext2_fs, ext2_ino);
		break;
	case S_IFLNK:
		ret = create_symbol_link(trans, root, objectid, &btrfs_inode,
					 ext2_fs, ext2_ino, ext2_inode);
		break;
	default:
		ret = 0;
		break;
	}
	if (ret)
		return ret;

	if (!noxattr) {
		ret = copy_extended_attrs(trans, root, objectid, &btrfs_inode,
					  ext2_fs, ext2_ino);
		if (ret)
			return ret;
	}
	inode_key.objectid = objectid;
	inode_key.offset = 0;
	btrfs_set_key_type(&inode_key, BTRFS_INODE_ITEM_KEY);
	ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);
	return ret;
}

static int copy_disk_extent(struct btrfs_root *root, u64 dst_bytenr,
		            u64 src_bytenr, u32 num_bytes)
{
	int ret;
	char *buffer;
	struct btrfs_fs_devices *fs_devs = root->fs_info->fs_devices;

	buffer = malloc(num_bytes);
	if (!buffer)
		return -ENOMEM;
	ret = pread(fs_devs->latest_bdev, buffer, num_bytes, src_bytenr);
	if (ret != num_bytes)
		goto fail;
	ret = pwrite(fs_devs->latest_bdev, buffer, num_bytes, dst_bytenr);
	if (ret != num_bytes)
		goto fail;
	ret = 0;
fail:
	free(buffer);
	if (ret > 0)
		ret = -1;
	return ret;
}
/*
 * scan ext2's inode bitmap and copy all used inodes.
 */
static int copy_inodes(struct btrfs_root *root, ext2_filsys ext2_fs,
		       int datacsum, int packing, int noxattr)
{
	int ret;
	errcode_t err;
	ext2_inode_scan ext2_scan;
	struct ext2_inode ext2_inode;
	ext2_ino_t ext2_ino;
	u64 objectid;
	struct btrfs_trans_handle *trans;

	trans = btrfs_start_transaction(root, 1);
	if (!trans)
		return -ENOMEM;
	err = ext2fs_open_inode_scan(ext2_fs, 0, &ext2_scan);
	if (err) {
		fprintf(stderr, "ext2fs_open_inode_scan: %s\n", error_message(err));
		return -1;
	}
	while (!(err = ext2fs_get_next_inode(ext2_scan, &ext2_ino,
					     &ext2_inode))) {
		/* no more inodes */
		if (ext2_ino == 0)
			break;
		/* skip special inode in ext2fs */
		if (ext2_ino < EXT2_GOOD_OLD_FIRST_INO &&
		    ext2_ino != EXT2_ROOT_INO)
			continue;
		objectid = ext2_ino + INO_OFFSET;
		ret = copy_single_inode(trans, root,
					objectid, ext2_fs, ext2_ino,
					&ext2_inode, datacsum, packing,
					noxattr);
		if (ret)
			return ret;
		if (trans->blocks_used >= 4096) {
			ret = btrfs_commit_transaction(trans, root);
			BUG_ON(ret);
			trans = btrfs_start_transaction(root, 1);
			BUG_ON(!trans);
		}
	}
	if (err) {
		fprintf(stderr, "ext2fs_get_next_inode: %s\n", error_message(err));
		return -1;
	}
	ret = btrfs_commit_transaction(trans, root);
	BUG_ON(ret);

	return ret;
}

/*
 * Construct a range of ext2fs image file.
 * scan block allocation bitmap, find all blocks used by the ext2fs
 * in this range and create file extents that point to these blocks.
 *
 * Note: Before calling the function, no file extent points to blocks
 * 	 in this range
 */
static int create_image_file_range(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root, u64 objectid,
				   struct btrfs_inode_item *inode,
				   u64 start_byte, u64 end_byte,
				   ext2_filsys ext2_fs)
{
	u32 blocksize = ext2_fs->blocksize;
	u32 block = start_byte / blocksize;
	u32 last_block = (end_byte + blocksize - 1) / blocksize;
	int ret = 0;
	struct blk_iterate_data data = {
		.trans		= trans,
		.root		= root,
		.inode		= inode,
		.objectid	= objectid,
		.first_block	= block,
		.disk_block	= 0,
		.num_blocks	= 0,
		.boundary	= (u64)-1,
		.checksum 	= 0,
		.errcode	= 0,
	};
	for (; start_byte < end_byte; block++, start_byte += blocksize) {
		if (!ext2fs_fast_test_block_bitmap(ext2_fs->block_map, block))
			continue;
		ret = block_iterate_proc(NULL, block, block, &data);
		if (ret & BLOCK_ABORT) {
			ret = data.errcode;
			goto fail;
		}
	}
	if (data.num_blocks > 0) {
		ret = record_file_blocks(trans, root, objectid, inode,
					 data.first_block, data.disk_block,
					 data.num_blocks, 0);
		if (ret)
			goto fail;
		data.first_block += data.num_blocks;
	}
	if (last_block > data.first_block) {
		ret = record_file_blocks(trans, root, objectid, inode,
					 data.first_block, 0, last_block -
					 data.first_block, 0);
		if (ret)
			goto fail;
	}
fail:
	return ret;
}
/*
 * Create the ext2fs image file.
 */
static int create_ext2_image(struct btrfs_root *root, ext2_filsys ext2_fs,
			     const char *name)
{
	int ret;
	struct btrfs_key key;
	struct btrfs_key location;
	struct btrfs_path path;
	struct btrfs_inode_item btrfs_inode;
	struct btrfs_inode_item *inode_item;
	struct extent_buffer *leaf;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_root *extent_root = fs_info->extent_root;
	struct btrfs_trans_handle *trans;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	u64 bytenr;
	u64 num_bytes;
	u64 objectid;
	u64 last_byte;
	u64 first_free;
	u64 total_bytes;
	u32 sectorsize = root->sectorsize;

	total_bytes = btrfs_super_total_bytes(fs_info->super_copy);
	first_free =  BTRFS_SUPER_INFO_OFFSET + sectorsize * 2 - 1;
	first_free &= ~((u64)sectorsize - 1);

	memset(&btrfs_inode, 0, sizeof(btrfs_inode));
	btrfs_set_stack_inode_generation(&btrfs_inode, 1);
	btrfs_set_stack_inode_size(&btrfs_inode, total_bytes);
	btrfs_set_stack_inode_nlink(&btrfs_inode, 1);
	btrfs_set_stack_inode_nbytes(&btrfs_inode, 0);
	btrfs_set_stack_inode_mode(&btrfs_inode, S_IFREG | 0400);
	btrfs_set_stack_inode_flags(&btrfs_inode, BTRFS_INODE_NODATASUM |
				    BTRFS_INODE_READONLY);
	btrfs_init_path(&path);
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);

	objectid = btrfs_root_dirid(&root->root_item);
	ret = btrfs_find_free_objectid(trans, root, objectid, &objectid);
	if (ret)
		goto fail;

	/*
	 * copy blocks covered by extent #0 to new positions. extent #0 is
	 * special, we can't rely on relocate_extents_range to relocate it.
	 */
	for (last_byte = 0; last_byte < first_free; last_byte += sectorsize) {
		ret = custom_alloc_extent(root, sectorsize, 0, &key);
		if (ret)
			goto fail;
		ret = copy_disk_extent(root, key.objectid, last_byte,
				       sectorsize);
		if (ret)
			goto fail;
		ret = record_file_extent(trans, root, objectid,
					 &btrfs_inode, last_byte,
					 key.objectid, sectorsize, 0);
		if (ret)
			goto fail;
	}

	while(1) {
		key.objectid = last_byte;
		key.offset = 0;
		btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
		ret = btrfs_search_slot(trans, fs_info->extent_root,
					&key, &path, 0, 0);
		if (ret < 0)
			goto fail;
next:
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, &path);
			if (ret < 0)
				goto fail;
			if (ret > 0)
				break;
			leaf = path.nodes[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (last_byte > key.objectid ||
		    key.type != BTRFS_EXTENT_ITEM_KEY) {
			path.slots[0]++;
			goto next;
		}

		bytenr = key.objectid;
		num_bytes = key.offset;
		ei = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_extent_item);
		if (!(btrfs_extent_flags(leaf, ei) & BTRFS_EXTENT_FLAG_DATA)) {
			path.slots[0]++;
			goto next;
		}

		BUG_ON(btrfs_item_size_nr(leaf, path.slots[0]) != sizeof(*ei) +
		       btrfs_extent_inline_ref_size(BTRFS_EXTENT_DATA_REF_KEY));

		iref = (struct btrfs_extent_inline_ref *)(ei + 1);
		key.type = btrfs_extent_inline_ref_type(leaf, iref);
		BUG_ON(key.type != BTRFS_EXTENT_DATA_REF_KEY);
		dref = (struct btrfs_extent_data_ref *)(&iref->offset);
		if (btrfs_extent_data_ref_root(leaf, dref) !=
		    BTRFS_FS_TREE_OBJECTID) {
			path.slots[0]++;
			goto next;
		}

		if (bytenr > last_byte) {
			ret = create_image_file_range(trans, root, objectid,
						      &btrfs_inode, last_byte,
						      bytenr, ext2_fs);
			if (ret)
				goto fail;
		}
		ret = record_file_extent(trans, root, objectid, &btrfs_inode,
					 bytenr, bytenr, num_bytes, 0);
		if (ret)
			goto fail;
		last_byte = bytenr + num_bytes;
		btrfs_release_path(extent_root, &path);

		if (trans->blocks_used >= 4096) {
			ret = btrfs_commit_transaction(trans, root);
			BUG_ON(ret);
			trans = btrfs_start_transaction(root, 1);
			BUG_ON(!trans);
		}
	}
	btrfs_release_path(root, &path);
	if (total_bytes > last_byte) {
		ret = create_image_file_range(trans, root, objectid,
					      &btrfs_inode, last_byte,
					      total_bytes, ext2_fs);
		if (ret)
			goto fail;
	}

	ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);
	if (ret)
		goto fail;

	location.objectid = objectid;
	location.offset = 0;
	btrfs_set_key_type(&location, BTRFS_INODE_ITEM_KEY);
	ret = btrfs_insert_dir_item(trans, root, name, strlen(name),
				    btrfs_root_dirid(&root->root_item),
				    &location, EXT2_FT_REG_FILE, objectid);
	if (ret)
		goto fail;
	ret = btrfs_insert_inode_ref(trans, root, name, strlen(name),
				     objectid,
				     btrfs_root_dirid(&root->root_item),
				     objectid);
	if (ret)
		goto fail;
	location.objectid = btrfs_root_dirid(&root->root_item);
	location.offset = 0;
	btrfs_set_key_type(&location, BTRFS_INODE_ITEM_KEY);
	ret = btrfs_lookup_inode(trans, root, &path, &location, 1);
	if (ret)
		goto fail;
	leaf = path.nodes[0];
	inode_item = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_inode_item);
	btrfs_set_inode_size(leaf, inode_item, strlen(name) * 2 +
			     btrfs_inode_size(leaf, inode_item));
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(root, &path);
	ret = btrfs_commit_transaction(trans, root);
	BUG_ON(ret);
fail:
	btrfs_release_path(root, &path);
	return ret;
}

struct btrfs_root *link_subvol(struct btrfs_root *root, const char *base,
			       u64 root_objectid)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *new_root = NULL;
	struct btrfs_path *path;
	struct btrfs_inode_item *inode_item;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 dirid = btrfs_root_dirid(&root->root_item);
	u64 index = 2;
	char buf[64];
	int i;
	int ret;

	path = btrfs_alloc_path();
	BUG_ON(!path);

	key.objectid = dirid;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	BUG_ON(ret <= 0);

	if (path->slots[0] > 0) {
		path->slots[0]--;
		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		if (key.objectid == dirid && key.type == BTRFS_DIR_INDEX_KEY)
			index = key.offset + 1;
	}
	btrfs_release_path(root, path);

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);

	key.objectid = dirid;
	key.offset = 0;
	key.type =  BTRFS_INODE_ITEM_KEY;

	ret = btrfs_lookup_inode(trans, root, path, &key, 1);
	BUG_ON(ret);
	leaf = path->nodes[0];
	inode_item = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_inode_item);

	key.objectid = root_objectid;
	key.offset = (u64)-1;
	key.type = BTRFS_ROOT_ITEM_KEY;

	strcpy(buf, base);
	for (i = 0; i < 1024; i++) {
		ret = btrfs_insert_dir_item(trans, root, buf, strlen(buf),
					    dirid, &key, BTRFS_FT_DIR, index);
		if (ret != -EEXIST)
			break;
		sprintf(buf, "%s%d", base, i);
	}
	if (ret)
		goto fail;

	btrfs_set_inode_size(leaf, inode_item, strlen(buf) * 2 +
			     btrfs_inode_size(leaf, inode_item));
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(root, path);

	/* add the backref first */
	ret = btrfs_add_root_ref(trans, tree_root, root_objectid,
				 BTRFS_ROOT_BACKREF_KEY,
				 root->root_key.objectid,
				 dirid, index, buf, strlen(buf));
	BUG_ON(ret);

	/* now add the forward ref */
	ret = btrfs_add_root_ref(trans, tree_root, root->root_key.objectid,
				 BTRFS_ROOT_REF_KEY, root_objectid,
				 dirid, index, buf, strlen(buf));

	ret = btrfs_commit_transaction(trans, root);
	BUG_ON(ret);

	new_root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(new_root))
		new_root = NULL;
fail:
	btrfs_free_path(path);
	return new_root;
}

static int create_chunk_mapping(struct btrfs_trans_handle *trans,
				struct btrfs_root *root)
{
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *chunk_root = info->chunk_root;
	struct btrfs_root *extent_root = info->extent_root;
	struct btrfs_device *device;
	struct btrfs_block_group_cache *cache;
	struct btrfs_dev_extent *extent;
	struct extent_buffer *leaf;
	struct btrfs_chunk chunk;
	struct btrfs_key key;
	struct btrfs_path path;
	u64 cur_start;
	u64 total_bytes;
	u64 chunk_objectid;
	int ret;

	btrfs_init_path(&path);

	total_bytes = btrfs_super_total_bytes(root->fs_info->super_copy);
	chunk_objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;

	BUG_ON(list_empty(&info->fs_devices->devices));
	device = list_entry(info->fs_devices->devices.next,
			    struct btrfs_device, dev_list);
	BUG_ON(device->devid != info->fs_devices->latest_devid);

	/* delete device extent created by make_btrfs */
	key.objectid = device->devid;
	key.offset = 0;
	key.type = BTRFS_DEV_EXTENT_KEY;
	ret = btrfs_search_slot(trans, device->dev_root, &key, &path, -1, 1);
	if (ret < 0)
		goto err;

	BUG_ON(ret > 0);
	ret = btrfs_del_item(trans, device->dev_root, &path);
	if (ret)
		goto err;
	btrfs_release_path(device->dev_root, &path);

	/* delete chunk item created by make_btrfs */
	key.objectid = chunk_objectid;
	key.offset = 0;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	ret = btrfs_search_slot(trans, chunk_root, &key, &path, -1, 1);
	if (ret < 0)
		goto err;

	BUG_ON(ret > 0);
	ret = btrfs_del_item(trans, chunk_root, &path);
	if (ret)
		goto err;
	btrfs_release_path(chunk_root, &path);

	/* for each block group, create device extent and chunk item */
	cur_start = 0;
	while (cur_start < total_bytes) {
		cache = btrfs_lookup_block_group(root->fs_info, cur_start);
		BUG_ON(!cache);

		/* insert device extent */
		key.objectid = device->devid;
		key.offset = cache->key.objectid;
		key.type = BTRFS_DEV_EXTENT_KEY;
		ret = btrfs_insert_empty_item(trans, device->dev_root, &path,
					      &key, sizeof(*extent));
		if (ret)
			goto err;

		leaf = path.nodes[0];
		extent = btrfs_item_ptr(leaf, path.slots[0],
					struct btrfs_dev_extent);

		btrfs_set_dev_extent_chunk_tree(leaf, extent,
						chunk_root->root_key.objectid);
		btrfs_set_dev_extent_chunk_objectid(leaf, extent,
						    chunk_objectid);
		btrfs_set_dev_extent_chunk_offset(leaf, extent,
						  cache->key.objectid);
		btrfs_set_dev_extent_length(leaf, extent, cache->key.offset);
		write_extent_buffer(leaf, root->fs_info->chunk_tree_uuid,
		    (unsigned long)btrfs_dev_extent_chunk_tree_uuid(extent),
		    BTRFS_UUID_SIZE);
		btrfs_mark_buffer_dirty(leaf);
		btrfs_release_path(device->dev_root, &path);

		/* insert chunk item */
		btrfs_set_stack_chunk_length(&chunk, cache->key.offset);
		btrfs_set_stack_chunk_owner(&chunk,
					    extent_root->root_key.objectid);
		btrfs_set_stack_chunk_stripe_len(&chunk, STRIPE_LEN);
		btrfs_set_stack_chunk_type(&chunk, cache->flags);
		btrfs_set_stack_chunk_io_align(&chunk, device->io_align);
		btrfs_set_stack_chunk_io_width(&chunk, device->io_width);
		btrfs_set_stack_chunk_sector_size(&chunk, device->sector_size);
		btrfs_set_stack_chunk_num_stripes(&chunk, 1);
		btrfs_set_stack_chunk_sub_stripes(&chunk, 0);
		btrfs_set_stack_stripe_devid(&chunk.stripe, device->devid);
		btrfs_set_stack_stripe_offset(&chunk.stripe,
					      cache->key.objectid);
		memcpy(&chunk.stripe.dev_uuid, device->uuid, BTRFS_UUID_SIZE);

		key.objectid = chunk_objectid;
		key.offset = cache->key.objectid;
		key.type = BTRFS_CHUNK_ITEM_KEY;

		ret = btrfs_insert_item(trans, chunk_root, &key, &chunk,
					btrfs_chunk_item_size(1));
		if (ret)
			goto err;

		cur_start = cache->key.objectid + cache->key.offset;
	}

	device->bytes_used = total_bytes;
	ret = btrfs_update_device(trans, device);
err:
	btrfs_release_path(device->dev_root, &path);
	return ret;
}

static int create_subvol(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root, u64 root_objectid)
{
	struct extent_buffer *tmp;
	struct btrfs_root *new_root;
	struct btrfs_key key;
	struct btrfs_root_item root_item;
	int ret;

	ret = btrfs_copy_root(trans, root, root->node, &tmp,
			      root_objectid);
	BUG_ON(ret);

	memcpy(&root_item, &root->root_item, sizeof(root_item));
	btrfs_set_root_bytenr(&root_item, tmp->start);
	btrfs_set_root_level(&root_item, btrfs_header_level(tmp));
	btrfs_set_root_generation(&root_item, trans->transid);
	free_extent_buffer(tmp);

	key.objectid = root_objectid;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = trans->transid;
	ret = btrfs_insert_root(trans, root->fs_info->tree_root,
				&key, &root_item);

	key.offset = (u64)-1;
	new_root = btrfs_read_fs_root(root->fs_info, &key);
	BUG_ON(!new_root || IS_ERR(new_root));

	ret = btrfs_make_root_dir(trans, new_root, BTRFS_FIRST_FREE_OBJECTID);
	BUG_ON(ret);

	return 0;
}

static int init_btrfs(struct btrfs_root *root)
{
	int ret;
	struct btrfs_key location;
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *tmp;

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);
	ret = btrfs_make_block_groups(trans, root);
	if (ret)
		goto err;
	ret = btrfs_fix_block_accounting(trans, root);
	if (ret)
		goto err;
	ret = create_chunk_mapping(trans, root);
	if (ret)
		goto err;
	ret = btrfs_make_root_dir(trans, fs_info->tree_root,
				  BTRFS_ROOT_TREE_DIR_OBJECTID);
	if (ret)
		goto err;
	memcpy(&location, &root->root_key, sizeof(location));
	location.offset = (u64)-1;
	ret = btrfs_insert_dir_item(trans, fs_info->tree_root, "default", 7,
				btrfs_super_root_dir(fs_info->super_copy),
				&location, BTRFS_FT_DIR, 0);
	if (ret)
		goto err;
	ret = btrfs_insert_inode_ref(trans, fs_info->tree_root, "default", 7,
				location.objectid,
				btrfs_super_root_dir(fs_info->super_copy), 0);
	if (ret)
		goto err;
	btrfs_set_root_dirid(&fs_info->fs_root->root_item,
			     BTRFS_FIRST_FREE_OBJECTID);

	/* subvol for ext2 image file */
	ret = create_subvol(trans, root, EXT2_IMAGE_SUBVOL_OBJECTID);
	BUG_ON(ret);
	/* subvol for data relocation */
	ret = create_subvol(trans, root, BTRFS_DATA_RELOC_TREE_OBJECTID);
	BUG_ON(ret);

	ret = __btrfs_cow_block(trans, fs_info->csum_root,
				fs_info->csum_root->node, NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	ret = btrfs_commit_transaction(trans, root);
	BUG_ON(ret);
err:
	return ret;
}

/*
 * Migrate super block to it's default position and zero 0 ~ 16k
 */
static int migrate_super_block(int fd, u64 old_bytenr, u32 sectorsize)
{
	int ret;
	struct extent_buffer *buf;
	struct btrfs_super_block *super;
	u32 len;
	u32 bytenr;

	BUG_ON(sectorsize < sizeof(*super));
	buf = malloc(sizeof(*buf) + sectorsize);
	if (!buf)
		return -ENOMEM;

	buf->len = sectorsize;
	ret = pread(fd, buf->data, sectorsize, old_bytenr);
	if (ret != sectorsize)
		goto fail;

	super = (struct btrfs_super_block *)buf->data;
	BUG_ON(btrfs_super_bytenr(super) != old_bytenr);
	btrfs_set_super_bytenr(super, BTRFS_SUPER_INFO_OFFSET);

	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, sectorsize, BTRFS_SUPER_INFO_OFFSET);
	if (ret != sectorsize)
		goto fail;

	ret = fsync(fd);
	if (ret)
		goto fail;

	memset(buf->data, 0, sectorsize);
	for (bytenr = 0; bytenr < BTRFS_SUPER_INFO_OFFSET; ) {
		len = BTRFS_SUPER_INFO_OFFSET - bytenr;
		if (len > sectorsize)
			len = sectorsize;
		ret = pwrite(fd, buf->data, len, bytenr);
		if (ret != len) {
			fprintf(stderr, "unable to zero fill device\n");
			break;
		}
		bytenr += len;
	}
	ret = 0;
	fsync(fd);
fail:
	free(buf);
	if (ret > 0)
		ret = -1;
	return ret;
}

static int prepare_system_chunk_sb(struct btrfs_super_block *super)
{
	struct btrfs_chunk *chunk;
	struct btrfs_disk_key *key;
	u32 sectorsize = btrfs_super_sectorsize(super);

	key = (struct btrfs_disk_key *)(super->sys_chunk_array);
	chunk = (struct btrfs_chunk *)(super->sys_chunk_array +
				       sizeof(struct btrfs_disk_key));

	btrfs_set_disk_key_objectid(key, BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_disk_key_type(key, BTRFS_CHUNK_ITEM_KEY);
	btrfs_set_disk_key_offset(key, 0);

	btrfs_set_stack_chunk_length(chunk, btrfs_super_total_bytes(super));
	btrfs_set_stack_chunk_owner(chunk, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_stack_chunk_stripe_len(chunk, 64 * 1024);
	btrfs_set_stack_chunk_type(chunk, BTRFS_BLOCK_GROUP_SYSTEM);
	btrfs_set_stack_chunk_io_align(chunk, sectorsize);
	btrfs_set_stack_chunk_io_width(chunk, sectorsize);
	btrfs_set_stack_chunk_sector_size(chunk, sectorsize);
	btrfs_set_stack_chunk_num_stripes(chunk, 1);
	btrfs_set_stack_chunk_sub_stripes(chunk, 0);
	chunk->stripe.devid = super->dev_item.devid;
	btrfs_set_stack_stripe_offset(&chunk->stripe, 0);
	memcpy(chunk->stripe.dev_uuid, super->dev_item.uuid, BTRFS_UUID_SIZE);
	btrfs_set_super_sys_array_size(super, sizeof(*key) + sizeof(*chunk));
	return 0;
}

static int prepare_system_chunk(int fd, u64 sb_bytenr, u32 sectorsize)
{
	int ret;
	struct extent_buffer *buf;
	struct btrfs_super_block *super;

	BUG_ON(sectorsize < sizeof(*super));
	buf = malloc(sizeof(*buf) + sectorsize);
	if (!buf)
		return -ENOMEM;

	buf->len = sectorsize;
	ret = pread(fd, buf->data, sectorsize, sb_bytenr);
	if (ret != sectorsize)
		goto fail;

	super = (struct btrfs_super_block *)buf->data;
	BUG_ON(btrfs_super_bytenr(super) != sb_bytenr);
	BUG_ON(btrfs_super_num_devices(super) != 1);

	ret = prepare_system_chunk_sb(super);
	if (ret)
		goto fail;

	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, sectorsize, sb_bytenr);
	if (ret != sectorsize)
		goto fail;

	ret = 0;
fail:
	free(buf);
	if (ret > 0)
		ret = -1;
	return ret;
}

static int relocate_one_reference(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root,
				  u64 extent_start, u64 extent_size,
				  struct btrfs_key *extent_key,
				  struct extent_io_tree *reloc_tree)
{
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key key;
	struct btrfs_path path;
	struct btrfs_inode_item inode;
	struct blk_iterate_data data;
	u64 bytenr;
	u64 num_bytes;
	u64 cur_offset;
	u64 new_pos;
	u64 nbytes;
	u64 sector_end;
	u32 sectorsize = root->sectorsize;
	unsigned long ptr;
	int datacsum;
	int fd;
	int ret;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(trans, root, extent_key, &path, -1, 1);
	if (ret)
		goto fail;

	leaf = path.nodes[0];
	fi = btrfs_item_ptr(leaf, path.slots[0],
			    struct btrfs_file_extent_item);
	BUG_ON(btrfs_file_extent_offset(leaf, fi) > 0);
	if (extent_start != btrfs_file_extent_disk_bytenr(leaf, fi) ||
	    extent_size != btrfs_file_extent_disk_num_bytes(leaf, fi)) {
		ret = 1;
		goto fail;
	}

	bytenr = extent_start + btrfs_file_extent_offset(leaf, fi);
	num_bytes = btrfs_file_extent_num_bytes(leaf, fi);

	ret = btrfs_del_item(trans, root, &path);
	if (ret)
		goto fail;

	ret = btrfs_free_extent(trans, root, extent_start, extent_size, 0,
				root->root_key.objectid,
				extent_key->objectid, extent_key->offset);
	if (ret)
		goto fail;

	btrfs_release_path(root, &path);

	key.objectid = extent_key->objectid;
	key.offset = 0;
	key.type =  BTRFS_INODE_ITEM_KEY;
	ret = btrfs_lookup_inode(trans, root, &path, &key, 0);
	if (ret)
		goto fail;

	leaf = path.nodes[0];
	ptr = btrfs_item_ptr_offset(leaf, path.slots[0]);
	read_extent_buffer(leaf, &inode, ptr, sizeof(inode));
	btrfs_release_path(root, &path);

	BUG_ON(num_bytes & (sectorsize - 1));
	nbytes = btrfs_stack_inode_nbytes(&inode) - num_bytes;
	btrfs_set_stack_inode_nbytes(&inode, nbytes);
	datacsum = !(btrfs_stack_inode_flags(&inode) & BTRFS_INODE_NODATASUM);

	data = (struct blk_iterate_data) {
		.trans		= trans,
		.root		= root,
		.inode		= &inode,
		.objectid	= extent_key->objectid,
		.first_block	= extent_key->offset / sectorsize,
		.disk_block	= 0,
		.num_blocks	= 0,
		.boundary	= (u64)-1,
		.checksum	= datacsum,
		.errcode	= 0,
	};

	cur_offset = extent_key->offset;
	while (num_bytes > 0) {
		sector_end = bytenr + sectorsize - 1;
		if (test_range_bit(reloc_tree, bytenr, sector_end,
				   EXTENT_LOCKED, 1)) {
			ret = get_state_private(reloc_tree, bytenr, &new_pos);
			BUG_ON(ret);
		} else {
			ret = custom_alloc_extent(root, sectorsize, 0, &key);
			if (ret)
				goto fail;
			new_pos = key.objectid;

			if (cur_offset == extent_key->offset) {
				fd = root->fs_info->fs_devices->latest_bdev;
				readahead(fd, bytenr, num_bytes);
			}
			ret = copy_disk_extent(root, new_pos, bytenr,
					       sectorsize);
			if (ret)
				goto fail;
			ret = set_extent_bits(reloc_tree, bytenr, sector_end,
					      EXTENT_LOCKED, GFP_NOFS);
			BUG_ON(ret);
			ret = set_state_private(reloc_tree, bytenr, new_pos);
			BUG_ON(ret);
		}

		ret = block_iterate_proc(NULL, new_pos / sectorsize,
					 cur_offset / sectorsize, &data);
		if (ret & BLOCK_ABORT) {
			ret = data.errcode;
			goto fail;
		}

		cur_offset += sectorsize;
		bytenr += sectorsize;
		num_bytes -= sectorsize;
	}

	if (data.num_blocks > 0) {
		ret = record_file_blocks(trans, root,
					 extent_key->objectid, &inode,
					 data.first_block, data.disk_block,
					 data.num_blocks, datacsum);
		if (ret)
			goto fail;
	}

	key.objectid = extent_key->objectid;
	key.offset = 0;
	key.type =  BTRFS_INODE_ITEM_KEY;
	ret = btrfs_lookup_inode(trans, root, &path, &key, 1);
	if (ret)
		goto fail;

	leaf = path.nodes[0];
	ptr = btrfs_item_ptr_offset(leaf, path.slots[0]);
	write_extent_buffer(leaf, &inode, ptr, sizeof(inode));
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(root, &path);

fail:
	btrfs_release_path(root, &path);
	return ret;
}

static int relocate_extents_range(struct btrfs_root *fs_root,
				  struct btrfs_root *ext2_root,
				  u64 start_byte, u64 end_byte)
{
	struct btrfs_fs_info *info = fs_root->fs_info;
	struct btrfs_root *extent_root = info->extent_root;
	struct btrfs_root *cur_root = NULL;
	struct btrfs_trans_handle *trans;
	struct btrfs_extent_data_ref *dref;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_item *ei;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_key extent_key;
	struct btrfs_path path;
	struct extent_io_tree reloc_tree;
	unsigned long ptr;
	unsigned long end;
	u64 cur_byte;
	u64 num_bytes;
	u64 ref_root;
	u64 num_extents;
	int pass = 0;
	int ret;

	btrfs_init_path(&path);
	extent_io_tree_init(&reloc_tree);

	key.objectid = start_byte;
	key.offset = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0)
		goto fail;
	if (ret > 0) {
		ret = btrfs_previous_item(extent_root, &path, 0,
					  BTRFS_EXTENT_ITEM_KEY);
		if (ret < 0)
			goto fail;
		if (ret == 0) {
			leaf = path.nodes[0];
			btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
			if (key.objectid + key.offset > start_byte)
				start_byte = key.objectid;
		}
	}
	btrfs_release_path(extent_root, &path);
again:
	cur_root = (pass % 2 == 0) ? ext2_root : fs_root;
	num_extents = 0;

	trans = btrfs_start_transaction(cur_root, 1);
	BUG_ON(!trans);

	cur_byte = start_byte;
	while (1) {
		key.objectid = cur_byte;
		key.offset = 0;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		ret = btrfs_search_slot(trans, extent_root,
					&key, &path, 0, 0);
		if (ret < 0)
			goto fail;
next:
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, &path);
			if (ret < 0)
				goto fail;
			if (ret > 0)
				break;
			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid < cur_byte ||
		    key.type != BTRFS_EXTENT_ITEM_KEY) {
			path.slots[0]++;
			goto next;
		}
		if (key.objectid >= end_byte)
			break;

		num_extents++;

		cur_byte = key.objectid;
		num_bytes = key.offset;
		ei = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_extent_item);
		BUG_ON(!(btrfs_extent_flags(leaf, ei) &
			 BTRFS_EXTENT_FLAG_DATA));

		ptr = btrfs_item_ptr_offset(leaf, path.slots[0]);
		end = ptr + btrfs_item_size_nr(leaf, path.slots[0]);

		ptr += sizeof(struct btrfs_extent_item);

		while (ptr < end) {
			iref = (struct btrfs_extent_inline_ref *)ptr;
			key.type = btrfs_extent_inline_ref_type(leaf, iref);
			BUG_ON(key.type != BTRFS_EXTENT_DATA_REF_KEY);
			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			ref_root = btrfs_extent_data_ref_root(leaf, dref);
			extent_key.objectid =
				btrfs_extent_data_ref_objectid(leaf, dref);
			extent_key.offset =
				btrfs_extent_data_ref_offset(leaf, dref);
			extent_key.type = BTRFS_EXTENT_DATA_KEY;
			BUG_ON(btrfs_extent_data_ref_count(leaf, dref) != 1);

			if (ref_root == cur_root->root_key.objectid)
				break;

			ptr += btrfs_extent_inline_ref_size(key.type);
		}

		if (ptr >= end) {
			path.slots[0]++;
			goto next;
		}

		ret = relocate_one_reference(trans, cur_root, cur_byte,
					     num_bytes, &extent_key,
					     &reloc_tree);
		if (ret < 0)
			goto fail;

		cur_byte += num_bytes;
		btrfs_release_path(extent_root, &path);

		if (trans->blocks_used >= 4096) {
			ret = btrfs_commit_transaction(trans, cur_root);
			BUG_ON(ret);
			trans = btrfs_start_transaction(cur_root, 1);
			BUG_ON(!trans);
		}
	}
	btrfs_release_path(cur_root, &path);

	ret = btrfs_commit_transaction(trans, cur_root);
	BUG_ON(ret);

	if (num_extents > 0 && pass++ < 16)
		goto again;

	ret = (num_extents > 0) ? -1 : 0;
fail:
	btrfs_release_path(cur_root, &path);
	extent_io_tree_cleanup(&reloc_tree);
	return ret;
}

/*
 * relocate data in system chunk
 */
static int cleanup_sys_chunk(struct btrfs_root *fs_root,
			     struct btrfs_root *ext2_root)
{
	struct btrfs_block_group_cache *cache;
	int i, ret = 0;
	u64 offset = 0;
	u64 end_byte;

	while(1) {
		cache = btrfs_lookup_block_group(fs_root->fs_info, offset);
		if (!cache)
			break;

		end_byte = cache->key.objectid + cache->key.offset;
		if (cache->flags & BTRFS_BLOCK_GROUP_SYSTEM) {
			ret = relocate_extents_range(fs_root, ext2_root,
						     cache->key.objectid,
						     end_byte);
			if (ret)
				goto fail;
		}
		offset = end_byte;
	}
	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		offset = btrfs_sb_offset(i);
		offset &= ~((u64)STRIPE_LEN - 1);

		ret = relocate_extents_range(fs_root, ext2_root,
					     offset, offset + STRIPE_LEN);
		if (ret)
			goto fail;
	}
	ret = 0;
fail:
	return ret;
}

static int fixup_chunk_mapping(struct btrfs_root *root)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *chunk_root = info->chunk_root;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_path path;
	struct btrfs_chunk chunk;
	unsigned long ptr;
	u32 size;
	u64 type;
	int ret;

	btrfs_init_path(&path);

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);

	/*
	 * recow the whole chunk tree. this will move all chunk tree blocks
	 * into system block group.
	 */
	memset(&key, 0, sizeof(key));
	while (1) {
		ret = btrfs_search_slot(trans, chunk_root, &key, &path, 0, 1);
		if (ret < 0)
			goto err;

		ret = btrfs_next_leaf(chunk_root, &path);
		if (ret < 0)
			goto err;
		if (ret > 0)
			break;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		btrfs_release_path(chunk_root, &path);
	}
	btrfs_release_path(chunk_root, &path);

	/* fixup the system chunk array in super block */
	btrfs_set_super_sys_array_size(info->super_copy, 0);

	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.offset = 0;
	key.type = BTRFS_CHUNK_ITEM_KEY;

	ret = btrfs_search_slot(trans, chunk_root, &key, &path, 0, 0);
	if (ret < 0)
		goto err;
	BUG_ON(ret != 0);
	while(1) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(chunk_root, &path);
			if (ret < 0)
				goto err;
			if (ret > 0)
				break;
			leaf = path.nodes[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_CHUNK_ITEM_KEY)
			goto next;

		ptr = btrfs_item_ptr_offset(leaf, path.slots[0]);
		size = btrfs_item_size_nr(leaf, path.slots[0]);
		BUG_ON(size != sizeof(chunk));
		read_extent_buffer(leaf, &chunk, ptr, size);
		type = btrfs_stack_chunk_type(&chunk);

		if (!(type & BTRFS_BLOCK_GROUP_SYSTEM))
			goto next;

		ret = btrfs_add_system_chunk(trans, chunk_root, &key,
					     &chunk, size);
		if (ret)
			goto err;
next:
		path.slots[0]++;
	}

	ret = btrfs_commit_transaction(trans, root);
	BUG_ON(ret);
err:
	btrfs_release_path(chunk_root, &path);
	return ret;
}

int do_convert(const char *devname, int datacsum, int packing, int noxattr)
{
	int i, ret;
	int fd = -1;
	u32 blocksize;
	u64 blocks[7];
	u64 total_bytes;
	u64 super_bytenr;
	ext2_filsys ext2_fs;
	struct btrfs_root *root;
	struct btrfs_root *ext2_root;

	ret = open_ext2fs(devname, &ext2_fs);
	if (ret) {
		fprintf(stderr, "unable to open the Ext2fs\n");
		goto fail;
	}
	blocksize = ext2_fs->blocksize;
	total_bytes = (u64)ext2_fs->super->s_blocks_count * blocksize;
	if (blocksize < 4096) {
		fprintf(stderr, "block size is too small\n");
		goto fail;
	}
	if (!(ext2_fs->super->s_feature_incompat &
	      EXT2_FEATURE_INCOMPAT_FILETYPE)) {
		fprintf(stderr, "filetype feature is missing\n");
		goto fail;
	}
	for (i = 0; i < 7; i++) {
		ret = ext2_alloc_block(ext2_fs, 0, blocks + i);
		if (ret) {
			fprintf(stderr, "not enough free space\n");
			goto fail;
		}
		blocks[i] *= blocksize;
	}
	super_bytenr = blocks[0];
	fd = open(devname, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "unable to open %s\n", devname);
		goto fail;
	}
	ret = make_btrfs(fd, devname, ext2_fs->super->s_volume_name,
			 blocks, total_bytes, blocksize, blocksize,
			 blocksize, blocksize);
	if (ret) {
		fprintf(stderr, "unable to create initial ctree: %s\n",
			strerror(-ret));
		goto fail;
	}
	/* create a system chunk that maps the whole device */
	ret = prepare_system_chunk(fd, super_bytenr, blocksize);
	if (ret) {
		fprintf(stderr, "unable to update system chunk\n");
		goto fail;
	}
	root = open_ctree_fd(fd, devname, super_bytenr, O_RDWR);
	if (!root) {
		fprintf(stderr, "unable to open ctree\n");
		goto fail;
	}
	ret = cache_free_extents(root, ext2_fs);
	if (ret) {
		fprintf(stderr, "error during cache_free_extents %d\n", ret);
		goto fail;
	}
	root->fs_info->extent_ops = &extent_ops;
	/* recover block allocation bitmap */
	for (i = 0; i < 7; i++) {
		blocks[i] /= blocksize;
		ext2_free_block(ext2_fs, blocks[i]);
	}
	ret = init_btrfs(root);
	if (ret) {
		fprintf(stderr, "unable to setup the root tree\n");
		goto fail;
	}
	printf("creating btrfs metadata.\n");
	ret = copy_inodes(root, ext2_fs, datacsum, packing, noxattr);
	if (ret) {
		fprintf(stderr, "error during copy_inodes %d\n", ret);
		goto fail;
	}
	printf("creating ext2fs image file.\n");
	ext2_root = link_subvol(root, "ext2_saved", EXT2_IMAGE_SUBVOL_OBJECTID);
	if (!ext2_root) {
		fprintf(stderr, "unable to create subvol\n");
		goto fail;
	}
	ret = create_ext2_image(ext2_root, ext2_fs, "image");
	if (ret) {
		fprintf(stderr, "error during create_ext2_image %d\n", ret);
		goto fail;
	}
	printf("cleaning up system chunk.\n");
	ret = cleanup_sys_chunk(root, ext2_root);
	if (ret) {
		fprintf(stderr, "error during cleanup_sys_chunk %d\n", ret);
		goto fail;
	}
	ret = close_ctree(root);
	if (ret) {
		fprintf(stderr, "error during close_ctree %d\n", ret);
		goto fail;
	}
	close_ext2fs(ext2_fs);

	/*
	 * If this step succeed, we get a mountable btrfs. Otherwise
	 * the ext2fs is left unchanged.
	 */
	ret = migrate_super_block(fd, super_bytenr, blocksize);
	if (ret) {
		fprintf(stderr, "unable to migrate super block\n");
		goto fail;
	}

	root = open_ctree_fd(fd, devname, 0, O_RDWR);
	if (!root) {
		fprintf(stderr, "unable to open ctree\n");
		goto fail;
	}
	/* move chunk tree into system chunk. */
	ret = fixup_chunk_mapping(root);
	if (ret) {
		fprintf(stderr, "error during fixup_chunk_tree\n");
		goto fail;
	}
	ret = close_ctree(root);
	close(fd);

	printf("conversion complete.\n");
	return 0;
fail:
	if (fd != -1)
		close(fd);
	fprintf(stderr, "conversion aborted.\n");
	return -1;
}

static int may_rollback(struct btrfs_root *root)
{
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_multi_bio *multi = NULL;
	u64 bytenr;
	u64 length;
	u64 physical;
	u64 total_bytes;
	int num_stripes;
	int ret;

	if (btrfs_super_num_devices(info->super_copy) != 1)
		goto fail;

	bytenr = BTRFS_SUPER_INFO_OFFSET;
	total_bytes = btrfs_super_total_bytes(root->fs_info->super_copy);

	while (1) {
		ret = btrfs_map_block(&info->mapping_tree, WRITE, bytenr,
				      &length, &multi, 0, NULL);
		if (ret)
			goto fail;

		num_stripes = multi->num_stripes;
		physical = multi->stripes[0].physical;
		kfree(multi);

		if (num_stripes != 1 || physical != bytenr)
			goto fail;

		bytenr += length;
		if (bytenr >= total_bytes)
			break;
	}
	return 0;
fail:
	return -1;
}

int do_rollback(const char *devname, int force)
{
	int fd = -1;
	int ret;
	int i;
	struct btrfs_root *root;
	struct btrfs_root *ext2_root;
	struct btrfs_root *chunk_root;
	struct btrfs_dir_item *dir;
	struct btrfs_inode_item *inode;
	struct btrfs_file_extent_item *fi;
	struct btrfs_trans_handle *trans;
	struct extent_buffer *leaf;
	struct btrfs_block_group_cache *cache1;
	struct btrfs_block_group_cache *cache2;
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_io_tree io_tree;
	char *buf = NULL;
	char *name;
	u64 bytenr;
	u64 num_bytes;
	u64 root_dir;
	u64 objectid;
	u64 offset;
	u64 start;
	u64 end;
	u64 sb_bytenr;
	u64 first_free;
	u64 total_bytes;
	u32 sectorsize;

	extent_io_tree_init(&io_tree);

	fd = open(devname, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "unable to open %s\n", devname);
		goto fail;
	}
	root = open_ctree_fd(fd, devname, 0, O_RDWR);
	if (!root) {
		fprintf(stderr, "unable to open ctree\n");
		goto fail;
	}
	ret = may_rollback(root);
	if (ret < 0) {
		fprintf(stderr, "unable to do rollback\n");
		goto fail;
	}

	sectorsize = root->sectorsize;
	buf = malloc(sectorsize);
	if (!buf) {
		fprintf(stderr, "unable to allocate memory\n");
		goto fail;
	}

	btrfs_init_path(&path);

	key.objectid = EXT2_IMAGE_SUBVOL_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	ext2_root = btrfs_read_fs_root(root->fs_info, &key);
	if (!ext2_root || IS_ERR(ext2_root)) {
		fprintf(stderr, "unable to open subvol %llu\n",
			key.objectid);
		goto fail;
	}

	name = "image";
	root_dir = btrfs_root_dirid(&root->root_item);
	dir = btrfs_lookup_dir_item(NULL, ext2_root, &path,
				   root_dir, name, strlen(name), 0);
	if (!dir || IS_ERR(dir)) {
		fprintf(stderr, "unable to find file %s\n", name);
		goto fail;
	}
	leaf = path.nodes[0];
	btrfs_dir_item_key_to_cpu(leaf, dir, &key);
	btrfs_release_path(ext2_root, &path);

	objectid = key.objectid;

	ret = btrfs_lookup_inode(NULL, ext2_root, &path, &key, 0);
	if (ret) {
		fprintf(stderr, "unable to find inode item\n");
		goto fail;
	}
	leaf = path.nodes[0];
	inode = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_inode_item);
	total_bytes = btrfs_inode_size(leaf, inode);
	btrfs_release_path(ext2_root, &path);

	key.objectid = objectid;
	key.offset = 0;
	btrfs_set_key_type(&key, BTRFS_EXTENT_DATA_KEY);
	ret = btrfs_search_slot(NULL, ext2_root, &key, &path, 0, 0);
	if (ret != 0) {
		fprintf(stderr, "unable to find first file extent\n");
		btrfs_release_path(ext2_root, &path);
		goto fail;
	}

	/* build mapping tree for the relocated blocks */
	for (offset = 0; offset < total_bytes; ) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret != 0)
				break;	
			continue;
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid != objectid || key.offset != offset ||
		    btrfs_key_type(&key) != BTRFS_EXTENT_DATA_KEY)
			break;

		fi = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(leaf, fi) != BTRFS_FILE_EXTENT_REG)
			break;
		if (btrfs_file_extent_compression(leaf, fi) ||
		    btrfs_file_extent_encryption(leaf, fi) ||
		    btrfs_file_extent_other_encoding(leaf, fi))
			break;

		bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
		/* skip holes and direct mapped extents */
		if (bytenr == 0 || bytenr == offset)
			goto next_extent;

		bytenr += btrfs_file_extent_offset(leaf, fi);
		num_bytes = btrfs_file_extent_num_bytes(leaf, fi);

		cache1 = btrfs_lookup_block_group(root->fs_info, offset);
		cache2 =  btrfs_lookup_block_group(root->fs_info,
						   offset + num_bytes - 1);
		if (!cache1 || cache1 != cache2 ||
		    (!(cache1->flags & BTRFS_BLOCK_GROUP_SYSTEM) &&
		     !intersect_with_sb(offset, num_bytes)))
			break;

		set_extent_bits(&io_tree, offset, offset + num_bytes - 1,
				EXTENT_LOCKED, GFP_NOFS);
		set_state_private(&io_tree, offset, bytenr);
next_extent:
		offset += btrfs_file_extent_num_bytes(leaf, fi);
		path.slots[0]++;
	}
	btrfs_release_path(ext2_root, &path);

	if (offset < total_bytes) {
		fprintf(stderr, "unable to build extent mapping\n");
		goto fail;
	}

	first_free = BTRFS_SUPER_INFO_OFFSET + 2 * sectorsize - 1;
	first_free &= ~((u64)sectorsize - 1);
	/* backup for extent #0 should exist */
	if(!test_range_bit(&io_tree, 0, first_free - 1, EXTENT_LOCKED, 1)) {
		fprintf(stderr, "no backup for the first extent\n");
		goto fail;
	}
	/* force no allocation from system block group */
	root->fs_info->system_allocs = -1;
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);
	/*
	 * recow the whole chunk tree, this will remove all chunk tree blocks
	 * from system block group
	 */
	chunk_root = root->fs_info->chunk_root;
	memset(&key, 0, sizeof(key));
	while (1) {
		ret = btrfs_search_slot(trans, chunk_root, &key, &path, 0, 1);
		if (ret < 0)
			break;

		ret = btrfs_next_leaf(chunk_root, &path);
		if (ret)
			break;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		btrfs_release_path(chunk_root, &path);
	}
	btrfs_release_path(chunk_root, &path);

	offset = 0;
	num_bytes = 0;
	while(1) {
		cache1 = btrfs_lookup_block_group(root->fs_info, offset);
		if (!cache1)
			break;

		if (cache1->flags & BTRFS_BLOCK_GROUP_SYSTEM)
			num_bytes += btrfs_block_group_used(&cache1->item);

		offset = cache1->key.objectid + cache1->key.offset;
	}
	/* only extent #0 left in system block group? */
	if (num_bytes > first_free) {
		fprintf(stderr, "unable to empty system block group\n");
		goto fail;
	}
	/* create a system chunk that maps the whole device */
	ret = prepare_system_chunk_sb(root->fs_info->super_copy);
	if (ret) {
		fprintf(stderr, "unable to update system chunk\n");
		goto fail;
	}

	ret = btrfs_commit_transaction(trans, root);
	BUG_ON(ret);

	ret = close_ctree(root);
	if (ret) {
		fprintf(stderr, "error during close_ctree %d\n", ret);
		goto fail;
	}

	/* zero btrfs super block mirrors */
	memset(buf, 0, sectorsize);
	for (i = 1 ; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		if (bytenr >= total_bytes)
			break;
		ret = pwrite(fd, buf, sectorsize, bytenr);
	}

	sb_bytenr = (u64)-1;
	/* copy all relocated blocks back */
	while(1) {
		ret = find_first_extent_bit(&io_tree, 0, &start, &end,
					    EXTENT_LOCKED);
		if (ret)
			break;

		ret = get_state_private(&io_tree, start, &bytenr);
		BUG_ON(ret);

		clear_extent_bits(&io_tree, start, end, EXTENT_LOCKED,
				  GFP_NOFS);

		while (start <= end) {
			if (start == BTRFS_SUPER_INFO_OFFSET) {
				sb_bytenr = bytenr;
				goto next_sector;
			}
			ret = pread(fd, buf, sectorsize, bytenr);
			if (ret < 0) {
				fprintf(stderr, "error during pread %d\n", ret);
				goto fail;
			}
			BUG_ON(ret != sectorsize);
			ret = pwrite(fd, buf, sectorsize, start);
			if (ret < 0) {
				fprintf(stderr, "error during pwrite %d\n", ret);
				goto fail;
			}
			BUG_ON(ret != sectorsize);
next_sector:
			start += sectorsize;
			bytenr += sectorsize;
		}
	}

	ret = fsync(fd);
	if (ret) {
		fprintf(stderr, "error during fsync %d\n", ret);
		goto fail;
	}
	/*
	 * finally, overwrite btrfs super block.
	 */
	ret = pread(fd, buf, sectorsize, sb_bytenr);
	if (ret < 0) {
		fprintf(stderr, "error during pread %d\n", ret);
		goto fail;
	}
	BUG_ON(ret != sectorsize);
	ret = pwrite(fd, buf, sectorsize, BTRFS_SUPER_INFO_OFFSET);
	if (ret < 0) {
		fprintf(stderr, "error during pwrite %d\n", ret);
		goto fail;
	}
	BUG_ON(ret != sectorsize);
	ret = fsync(fd);
	if (ret) {
		fprintf(stderr, "error during fsync %d\n", ret);
		goto fail;
	}

	close(fd);
	free(buf);
	extent_io_tree_cleanup(&io_tree);
	printf("rollback complete.\n");
	return 0;

fail:
	if (fd != -1)
		close(fd);
	free(buf);
	fprintf(stderr, "rollback aborted.\n");
	return -1;
}

static void print_usage(void)
{
	printf("usage: btrfs-convert [-d] [-i] [-n] [-r] device\n");
	printf("\t-d disable data checksum\n");
	printf("\t-i ignore xattrs and ACLs\n");
	printf("\t-n disable packing of small files\n");
	printf("\t-r roll back to ext2fs\n");
}

int main(int argc, char *argv[])
{
	int ret;
	int packing = 1;
	int noxattr = 0;
	int datacsum = 1;
	int rollback = 0;
	char *file;
	while(1) {
		int c = getopt(argc, argv, "dinr");
		if (c < 0)
			break;
		switch(c) {
			case 'd':
				datacsum = 0;
				break;
			case 'i':
				noxattr = 1;
				break;
			case 'n':
				packing = 0;
				break;
			case 'r':
				rollback = 1;
				break;
			default:
				print_usage();
				return 1;
		}
	}
	argc = argc - optind;
	if (argc != 1) {
		print_usage();
		return 1;
	}

	file = argv[optind];
	if (check_mounted(file)) {
		fprintf(stderr, "%s is mounted\n", file);
		return 1;
	}

	if (rollback) {
		ret = do_rollback(file, 0);
	} else {
		ret = do_convert(file, datacsum, packing, noxattr);
	}
	if (ret)
		return 1;
	return 0;
}
