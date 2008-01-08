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
#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <linux/fs.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "crc32c.h"
#include "utils.h"
#include "ext2fs/ext2_fs.h"
#include "ext2fs/ext2fs.h"

#define INO_OFFSET (BTRFS_FIRST_FREE_OBJECTID - EXT2_ROOT_INO)

/*
 * Open Ext2fs in readonly mode, read block allocation bitmap and
 * inode bitmap into memory.
 */
static int open_ext2fs(const char *name, ext2_filsys *ret_fs)
{
	int mnt_flags;
	errcode_t ret;
	ext2_filsys ext2_fs;

	ret = ext2fs_check_if_mounted(name, &mnt_flags);
	if (ret) {
		fprintf(stderr, "ext2fs_check_if_mounted: %s\n",
			error_message(ret));
		return -1;
	}
	if (mnt_flags & EXT2_MF_MOUNTED) {
		fprintf(stderr, "%s is mounted\n", name);
		return -1;
	}
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

static int custom_alloc_extent(struct btrfs_root *root, u64 num_bytes,
			       u64 hint_byte, struct btrfs_key *ins)
{
	ext2_filsys fs = (ext2_filsys)root->fs_info->priv_data;
	u32 blocksize = fs->blocksize;
	u64 first = 0;
	u64 block;
	u64 bytenr;
	int ret;

	block = hint_byte / blocksize;
	BUG_ON(block != (blk_t)block);
	BUG_ON(num_bytes != blocksize);
	while (1) {
		ret = ext2_alloc_block(fs, block, &block);
		if (ret)
			return ret;
		/* all free blocks are pinned */
		if (first == block)
			return -ENOSPC;
		if (first == 0)
			first = block;

		bytenr = block * blocksize;
		if (!test_range_bit(&root->fs_info->pinned_extents, bytenr,
				    bytenr + blocksize - 1, EXTENT_DIRTY, 0))
			break;

		ext2_free_block(fs, block);
		block++;
	}
	ins->objectid = bytenr;
	ins->offset = blocksize;
	btrfs_set_key_type(ins, BTRFS_EXTENT_ITEM_KEY);
	return 0;
}

static int custom_free_extent(struct btrfs_root *root, u64 bytenr,
			      u64 num_bytes)
{
	u64 block;
	ext2_filsys fs = (ext2_filsys)root->fs_info->priv_data;

	BUG_ON(bytenr & (fs->blocksize - 1));
	block = bytenr / fs->blocksize;
	while (num_bytes > 0) {
		ext2_free_block(fs, block);
		block++;
		num_bytes -= fs->blocksize;
	}
	return 0;
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
			    struct ext2_dir_entry *old,
			    int offset, int blocksize,
			    char *buf,void *priv_data)
{
	int ret;
	int file_type;
	u64 objectid;
        u64 inode_size;
	char dotdot[] = "..";
	struct btrfs_key location;
	struct ext2_dir_entry_2 *dirent = (struct ext2_dir_entry_2 *)old;
	struct dir_iterate_data *idata = (struct dir_iterate_data *)priv_data;

	objectid = dirent->inode + INO_OFFSET;
	if (!strncmp(dirent->name, dotdot, dirent->name_len)) {
		if (dirent->name_len == 2) {
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

	file_type = dirent->file_type;
	BUG_ON(file_type > EXT2_FT_SYMLINK);
	ret = btrfs_insert_dir_item(idata->trans, idata->root,
				    dirent->name, dirent->name_len,
				    idata->objectid, &location,
				    filetype_conversion_table[file_type]);
	if (ret)
		goto fail;
	ret = btrfs_insert_inode_ref(idata->trans, idata->root,
				     dirent->name, dirent->name_len,
				     objectid, idata->objectid);
	if (ret)
		goto fail;
	inode_size = btrfs_stack_inode_size(idata->inode) +
		     dirent->name_len * 2;
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
					     objectid, objectid);
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
	struct btrfs_fs_info *fs_info = root->fs_info;

	ret = pread(fs_info->fp, buffer, num_bytes, bytenr);
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
	struct btrfs_key ins_key;
	struct btrfs_path path;
	struct btrfs_extent_item extent_item;
	u32 blocksize = root->sectorsize;
	u64 nblocks;
	u64 bytes_used;

	ret = btrfs_insert_file_extent(trans, root, objectid, file_pos,
				       disk_bytenr, num_bytes, num_bytes);
	if (ret || disk_bytenr == 0)
		return ret;

	nblocks = btrfs_stack_inode_nblocks(inode) + num_bytes / 512;
	btrfs_set_stack_inode_nblocks(inode, nblocks);
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
			ret = btrfs_csum_file_block(trans, root, inode,
						objectid, file_pos + offset,
						buffer, blocksize);
			if (ret)
				break;
		}
		free(buffer);
		if (ret)
			goto fail;
	}

	bytes_used = btrfs_root_used(&root->root_item);
	btrfs_set_root_used(&root->root_item, bytes_used + num_bytes);
	ins_key.objectid = disk_bytenr;
	ins_key.offset = num_bytes;
	btrfs_set_key_type(&ins_key, BTRFS_EXTENT_ITEM_KEY);
	btrfs_set_stack_extent_refs(&extent_item, 1);
	ret = btrfs_insert_item(trans, extent_root, &ins_key,
				&extent_item, sizeof(extent_item));
	if (ret == 0) {
		bytes_used = btrfs_super_bytes_used(&info->super_copy);
		btrfs_set_super_bytes_used(&info->super_copy, bytes_used +
					   num_bytes);
		btrfs_init_path(&path);
		ret = btrfs_insert_extent_backref(trans, extent_root, &path,
					disk_bytenr, root->root_key.objectid,
					trans->transid, objectid, file_pos);
		if (ret)
			goto fail;
		ret = btrfs_update_block_group(trans, root, disk_bytenr,
					       num_bytes, 1, 0, 1);
	} else if (ret == -EEXIST) {
		ret = btrfs_inc_extent_ref(trans, root, disk_bytenr, num_bytes,
					   root->root_key.objectid,
					   trans->transid, objectid, file_pos);
	}
	if (ret)
		goto fail;
	btrfs_extent_post_op(trans, extent_root);
	return 0;
fail:
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
	int checksum;
	int errcode;
};

static int block_iterate_proc(ext2_filsys ext2_fs,
			      u64 disk_block, u64 file_block,
		              struct blk_iterate_data *idata)
{
	int ret;
	u32 blocksize = ext2_fs->blocksize;
	struct btrfs_root *root = idata->root;
	struct btrfs_trans_handle *trans = idata->trans;

	if ((file_block > idata->first_block + idata->num_blocks) ||
	    (disk_block != idata->disk_block + idata->num_blocks) ||
	    (idata->num_blocks >= BTRFS_BLOCK_GROUP_SIZE / blocksize)) {
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
		idata->first_block = file_block;
		idata->disk_block = disk_block;
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
			       ext2_filsys ext2_fs, ext2_ino_t ext2_ino)
{
	int ret;
	char *buffer = NULL;
	errcode_t err;
	u32 last_block;
	u32 sectorsize = root->sectorsize;
	u64 inode_size = btrfs_stack_inode_size(btrfs_inode);
	u32 inode_flags = btrfs_stack_inode_flags(btrfs_inode);
	struct blk_iterate_data data = {
		.trans		= trans,
		.root		= root,
		.inode		= btrfs_inode,
		.objectid	= objectid,
		.first_block	= 0,
		.disk_block	= 0,
		.num_blocks	= 0,
		.checksum	= 1,
		.errcode	= 0,
	};

	if (inode_flags & BTRFS_INODE_NODATASUM)
		data.checksum = 0;
	err = ext2fs_block_iterate2(ext2_fs, ext2_ino, BLOCK_FLAG_DATA_ONLY,
				    NULL, __block_iterate_proc, &data);
	if (err)
		goto error;
	ret = data.errcode;
	if (ret)
		goto fail;

	if (data.first_block == 0 && data.num_blocks > 0 &&
	    inode_size <= BTRFS_MAX_INLINE_DATA_SIZE(root)) {
		u64 num_bytes = data.num_blocks * sectorsize;
		u64 disk_bytenr = data.disk_block * sectorsize;

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
		ret = create_file_extents(trans, root, objectid,
					  btrfs_inode, ext2_fs, ext2_ino);
		btrfs_set_stack_inode_size(btrfs_inode, inode_size);
		return ret;
	}

	pathname = (char *)&(ext2_inode->i_block[0]);
	BUG_ON(pathname[inode_size] != 0);
	ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
					 pathname, inode_size + 1);
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
			   struct ext2_inode *src)
{
	btrfs_set_stack_inode_generation(dst, 1);
	btrfs_set_stack_inode_size(dst, src->i_size);
	btrfs_set_stack_inode_nblocks(dst, src->i_blocks);
	btrfs_set_stack_inode_block_group(dst, 0);
	btrfs_set_stack_inode_nblocks(dst, 0);
	btrfs_set_stack_inode_nlink(dst, src->i_links_count);
	btrfs_set_stack_inode_uid(dst, src->i_uid | (src->i_uid_high << 16));
	btrfs_set_stack_inode_gid(dst, src->i_gid | (src->i_gid_high << 16));
	btrfs_set_stack_inode_mode(dst, src->i_mode);
	btrfs_set_stack_inode_rdev(dst, 0);
	btrfs_set_stack_inode_flags(dst, 0);
	btrfs_set_stack_inode_compat_flags(dst, 0);
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
			     int datacsum)
{
	int ret;
	errcode_t err;
	struct ext2_inode ext2_inode;
	struct btrfs_key inode_key;
	struct btrfs_inode_item btrfs_inode;

	err = ext2fs_read_inode(ext2_fs, ext2_ino, &ext2_inode);
	if (err)
		goto error;

	if (!ext2_inode.i_links_count &&
	    (!ext2_inode.i_mode || ext2_inode.i_dtime)) {
		printf("skip inode %u\n", ext2_ino);
		return 0;
	}
	copy_inode_item(&btrfs_inode, &ext2_inode);
	if (!datacsum && S_ISREG(ext2_inode.i_mode)) {
		u32 flags = btrfs_stack_inode_flags(&btrfs_inode) |
			    BTRFS_INODE_NODATASUM;
		btrfs_set_stack_inode_flags(&btrfs_inode, flags);
	}

	switch (ext2_inode.i_mode & S_IFMT) {
	case S_IFREG:
		ret = create_file_extents(trans, root, objectid, &btrfs_inode,
					  ext2_fs, ext2_ino);
		break;
	case S_IFDIR:
		ret = create_dir_entries(trans, root, objectid, &btrfs_inode,
					 ext2_fs, ext2_ino);
		break;
	case S_IFLNK:
		ret = create_symbol_link(trans, root, objectid, &btrfs_inode,
					 ext2_fs, ext2_ino, &ext2_inode);
		break;
	default:
		ret = 0;
		break;
	}
	if (ret)
		return ret;
	inode_key.objectid = objectid;
	inode_key.offset = 0;
	btrfs_set_key_type(&inode_key, BTRFS_INODE_ITEM_KEY);
	ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);
	return ret;
error:
	fprintf(stderr, "ext2fs_read_inode: %s\n", error_message(err));
	return -1;
}

static int copy_disk_extent(struct btrfs_root *root, u64 dst_bytenr,
		            u64 src_bytenr, u32 num_bytes)
{
	int ret;
	char *buffer;
	struct btrfs_fs_info *fs_info = root->fs_info;

	buffer = malloc(num_bytes);
	if (!buffer)
		return -ENOMEM;
	ret = pread(fs_info->fp, buffer, num_bytes, src_bytenr);
	if (ret != num_bytes)
		goto fail;
	ret = pwrite(fs_info->fp, buffer, num_bytes, dst_bytenr);
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
 * scan ext2's inode bitmap and copy all used inode.
 */
static int copy_inodes(struct btrfs_root *root, ext2_filsys ext2_fs,
		       int datacsum)
{
	int ret;
	ext2_ino_t ext2_ino;
	u64 objectid;
	struct btrfs_trans_handle *trans;

	trans = btrfs_start_transaction(root, 1);
	if (!trans)
		return -ENOMEM;
	ext2_ino = ext2_fs->inode_map->start;
	for (; ext2_ino <= ext2_fs->inode_map->end; ext2_ino++) {
		if (ext2fs_fast_test_inode_bitmap(ext2_fs->inode_map,
						  ext2_ino)) {
			/* skip special inode in ext2fs */
			if (ext2_ino < EXT2_GOOD_OLD_FIRST_INO &&
			    ext2_ino != EXT2_ROOT_INO)
				continue;
			objectid = ext2_ino + INO_OFFSET;
			ret = copy_single_inode(trans, root, objectid,
					ext2_fs, ext2_ino, datacsum);
			if (ret)
				return ret;
		}
		if (trans->blocks_used >= 8192) {
			ret = btrfs_commit_transaction(trans, root);
			BUG_ON(ret);
			trans = btrfs_start_transaction(root, 1);
			BUG_ON(!trans);
		}
	}
	ret = btrfs_commit_transaction(trans, root);
	BUG_ON(ret);

	return ret;
}
static int lookup_extent_item(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     u64 bytenr, u64 num_bytes)
{
	int ret;
	struct btrfs_key key;
	struct btrfs_path path;
	btrfs_init_path(&path);
	key.objectid = bytenr;
	key.offset = num_bytes;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	ret = btrfs_search_slot(trans, root->fs_info->extent_root,
				&key, &path, 0, 0);
	btrfs_release_path(root, &path);
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
	u64 bytenr;
	u32 blocksize = ext2_fs->blocksize;
	u32 block = start_byte / blocksize;
	u32 last_block = (end_byte + blocksize - 1) / blocksize;
	int ret;
	struct blk_iterate_data data = {
		.trans		= trans,
		.root		= root,
		.inode		= inode,
		.objectid	= objectid,
		.first_block	= block,
		.disk_block	= block,
		.num_blocks	= 0,
		.checksum 	= 0,
		.errcode	= 0,
	};
	for (; start_byte < end_byte; block++, start_byte += blocksize) {
		if (!ext2fs_fast_test_block_bitmap(ext2_fs->block_map, block))
			continue;
		/* the bit may be set by us, check extent tree */
		bytenr = (u64)block * blocksize;
		ret = lookup_extent_item(trans, root, bytenr, blocksize);
		if (ret < 0)
			goto fail;
		if (ret == 0)
			continue;

		ret = block_iterate_proc(ext2_fs, block, block, &data);
		if (ret & BLOCK_ABORT)
			break;
	}
	ret = data.errcode;
	if (ret)
		return ret;
	if (data.num_blocks > 0) {
		ret = record_file_blocks(trans, root, objectid, inode,
					 data.first_block, data.disk_block,
					 data.num_blocks, 0);
		if (ret)
			return ret;
		data.first_block += data.num_blocks;
	}
	if (last_block > data.first_block) {
		ret = record_file_blocks(trans, root, objectid, inode,
					 data.first_block, 0, last_block -
					 data.first_block, 0);
		if (ret)
			return ret;
	}
fail:
	return 0;
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
	struct extent_buffer *leaf;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_root *extent_root = fs_info->extent_root;
	struct btrfs_trans_handle *trans;
	struct btrfs_extent_ref *ref_item;
	u64 bytenr;
	u64 num_bytes;
	u64 ref_root;
	u64 ref_owner;
	u64 objectid;
	u64 new_block;
	u64 last_byte;
	u64 first_free;
	u64 total_bytes;
	u32 sectorsize = root->sectorsize;
	int slot;
	int file_extent;

	total_bytes = btrfs_super_total_bytes(&fs_info->super_copy);
	first_free =  BTRFS_SUPER_INFO_OFFSET + sectorsize * 2 - 1;
	first_free &= ~((u64)sectorsize - 1);

	memset(&btrfs_inode, 0, sizeof(btrfs_inode));
	btrfs_set_stack_inode_generation(&btrfs_inode, 1);
	btrfs_set_stack_inode_size(&btrfs_inode, total_bytes);
	btrfs_set_stack_inode_nlink(&btrfs_inode, 1);
	btrfs_set_stack_inode_nblocks(&btrfs_inode, 0);
	btrfs_set_stack_inode_mode(&btrfs_inode, S_IFREG | 0400);
	btrfs_set_stack_inode_flags(&btrfs_inode, BTRFS_INODE_NODATASUM);
	btrfs_init_path(&path);
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);

	objectid = btrfs_root_dirid(&root->root_item);
	ret = btrfs_find_free_objectid(trans, root, objectid, &objectid);
	if (ret)
		goto fail;

	/*
	 * copy the first a few blocks to new positions. the relocation is
	 * reuqired for block 0 and default btrfs super block.
	 */
	for (last_byte = 0; last_byte < first_free; last_byte += sectorsize) {
		ret = ext2_alloc_block(ext2_fs, 0, &new_block);
		if (ret)
			goto fail;
		new_block *= sectorsize;
		ret = copy_disk_extent(root, new_block, last_byte, sectorsize);
		if (ret)
			goto fail;
		ret = record_file_extent(trans, root, objectid,
					 &btrfs_inode, last_byte,
					 new_block, sectorsize, 0);
		if (ret)
			goto fail;
	}
again:
	if (trans->blocks_used >= 8192) {
		ret = btrfs_commit_transaction(trans, root);
		BUG_ON(ret);
		trans = btrfs_start_transaction(root, 1);
		BUG_ON(!trans);
	}

	key.objectid = last_byte;
	key.offset = 0;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	btrfs_release_path(extent_root, &path);
	ret = btrfs_search_slot(trans, fs_info->extent_root,
				&key, &path, 0, 0);
	if (ret < 0)
		goto fail;

	leaf = path.nodes[0];
	slot = path.slots[0];
	while(1) {
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, &path);
			if (ret < 0)
				goto fail;
			if (ret > 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (last_byte > key.objectid ||
		    key.type != BTRFS_EXTENT_ITEM_KEY) {
			slot++;
			continue;
		}
		/*
		 * Check backref to distinguish extent items for normal
		 * files (files that correspond to files in Ext2fs) from
		 * extent items for ctree blocks.
		 */
		bytenr = key.objectid;
		num_bytes = key.offset;
		file_extent = 0;
		while (1) {
			if (slot >= btrfs_header_nritems(leaf)) {
				ret = btrfs_next_leaf(extent_root, &path);
				if (ret > 0)
					break;
				if (ret < 0)
					goto fail;
				leaf = path.nodes[0];
				slot = path.slots[0];
			}
			btrfs_item_key_to_cpu(leaf, &key, slot);
			if (key.objectid != bytenr)
				break;
			if (key.type != BTRFS_EXTENT_REF_KEY) {
				slot++;
				continue;
			}
			ref_item = btrfs_item_ptr(leaf, slot,
						  struct btrfs_extent_ref);
			ref_root = btrfs_ref_root(leaf, ref_item);
			ref_owner = btrfs_ref_objectid(leaf, ref_item);
			if ((ref_root == BTRFS_FS_TREE_OBJECTID) &&
			    (ref_owner >= BTRFS_FIRST_FREE_OBJECTID)) {
				file_extent = 1;
				break;
			}
			slot++;
		}
		if (!file_extent)
			continue;

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
		goto again;
	}
	if (total_bytes > last_byte) {
		ret = create_image_file_range(trans, root, objectid,
					      &btrfs_inode, last_byte,
					      total_bytes, ext2_fs);
		if (ret)
			goto fail;
	}
	/*
	 * otime isn't used currently, so we can store some data in it.
	 * These data are used by do_rollback to check whether the image
	 * file have been modified.
	 */
	btrfs_set_stack_timespec_sec(&btrfs_inode.otime, trans->transid);
	btrfs_set_stack_timespec_nsec(&btrfs_inode.otime,
				      total_bytes / sectorsize);
	ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);
	if (ret)
		goto fail;

	location.objectid = objectid;
	location.offset = 0;
	btrfs_set_key_type(&location, BTRFS_INODE_ITEM_KEY);
	ret = btrfs_insert_dir_item(trans, root, name, strlen(name),
				    btrfs_root_dirid(&root->root_item),
				    &location, EXT2_FT_REG_FILE);
	if (ret)
		goto fail;
	ret = btrfs_insert_inode_ref(trans, root, name, strlen(name),
				     objectid,
				     btrfs_root_dirid(&root->root_item));
	if (ret)
		goto fail;
	ret = btrfs_commit_transaction(trans, root);
	BUG_ON(ret);
fail:
	btrfs_release_path(root, &path);
	return ret;
}
struct btrfs_root *create_subvol(struct btrfs_root *root, const char *name)
{
	int ret;
	u64 objectid;
	struct btrfs_key location;
	struct btrfs_root_item root_item;
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *new_root;
	struct extent_buffer *tmp;

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);

	objectid = btrfs_super_root_dir(&fs_info->super_copy);
	ret = btrfs_find_free_objectid(trans, root, objectid, &objectid);
	if (ret)
		goto fail;
	ret = btrfs_copy_root(trans, root, root->node, &tmp, objectid);
	if (ret)
		goto fail;
	memcpy(&root_item, &root->root_item, sizeof(root_item));
	btrfs_set_root_bytenr(&root_item, tmp->start);
	btrfs_set_root_level(&root_item, btrfs_header_level(tmp));
	free_extent_buffer(tmp);

	location.objectid = objectid;
	location.offset = 1;
	btrfs_set_key_type(&location, BTRFS_ROOT_ITEM_KEY);
	ret = btrfs_insert_root(trans, root->fs_info->tree_root,
				&location, &root_item);
	if (ret)
		goto fail;
	location.offset = (u64)-1;
	ret = btrfs_insert_dir_item(trans, tree_root, name, strlen(name),
				    btrfs_super_root_dir(&fs_info->super_copy),
				    &location, BTRFS_FT_DIR);
	if (ret)
		goto fail;
	ret = btrfs_insert_inode_ref(trans, tree_root, name, strlen(name),
				     objectid,
				     btrfs_super_root_dir(&fs_info->super_copy));
	if (ret)
		goto fail;
	ret = btrfs_commit_transaction(trans, root);
	BUG_ON(ret);
	new_root = btrfs_read_fs_root(fs_info, &location);
	if (!new_root || IS_ERR(new_root))
		goto fail;
	trans = btrfs_start_transaction(new_root, 1);
	BUG_ON(!trans);
	ret = btrfs_make_root_dir(trans, new_root, BTRFS_FIRST_FREE_OBJECTID);
	if (ret)
		goto fail;
	ret = btrfs_commit_transaction(trans, new_root);
	BUG_ON(ret);
	return new_root;
fail:
	return NULL;
}

/*
 * Fixup block accounting. The initial block accounting created by
 * make_block_groups isn't accuracy in this case.
 */
static int fixup_block_accounting(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root)
{
	int ret;
	int slot;
	u64 start = 0;
	u64 bytes_used = 0;
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_block_group_cache *cache;
	struct btrfs_fs_info *fs_info = root->fs_info;

	while(1) {
		cache = btrfs_lookup_block_group(fs_info, start);
		if (!cache)
			break;
		start = cache->key.objectid + cache->key.offset;
		btrfs_set_block_group_used(&cache->item, 0);
	}

	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	btrfs_set_key_type(&key, BTRFS_EXTENT_ITEM_KEY);
	ret = btrfs_search_slot(trans, root->fs_info->extent_root,
				&key, &path, 0, 0);
	if (ret < 0)
		return ret;
	while(1) {
		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0)
				return ret;
			if (ret > 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.type == BTRFS_EXTENT_ITEM_KEY) {
			bytes_used += key.offset;
			ret = btrfs_update_block_group(trans, root,
				  key.objectid, key.offset, 1, 0, 1);
			BUG_ON(ret);
		}
		path.slots[0]++;
	}
	btrfs_set_super_bytes_used(&root->fs_info->super_copy, bytes_used);
	btrfs_release_path(root, &path);
	return 0;
}

static int init_btrfs(struct btrfs_root *root)
{
	int ret;
	struct btrfs_key location;
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = root->fs_info;

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);
	ret = btrfs_make_block_groups(trans, root);
	if (ret)
		goto err;
	ret = fixup_block_accounting(trans, root);
	if (ret)
		goto err;
	ret = btrfs_make_root_dir(trans, fs_info->tree_root,
				  BTRFS_ROOT_TREE_DIR_OBJECTID);
	if (ret)
		goto err;
	memcpy(&location, &root->root_key, sizeof(location));
	location.offset = (u64)-1;
	ret = btrfs_insert_dir_item(trans, fs_info->tree_root, "default", 7,
				btrfs_super_root_dir(&fs_info->super_copy),
				&location, BTRFS_FT_DIR);
	if (ret)
		goto err;
	ret = btrfs_insert_inode_ref(trans, fs_info->tree_root, "default", 7,
				location.objectid,
				btrfs_super_root_dir(&fs_info->super_copy));
	if (ret)
		goto err;
	btrfs_set_root_dirid(&fs_info->fs_root->root_item,
			     BTRFS_FIRST_FREE_OBJECTID);
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
	char *buf;
	u64 bytenr;
	u32 crc =  ~(u32)0;
	u32 len = 512 - BTRFS_CSUM_SIZE;
	struct btrfs_super_block *super;

	ret = fsync(fd);
	if (ret)
		goto fail;

	BUG_ON(sectorsize < sizeof(super));
	buf = malloc(sectorsize);
	if (!buf)
		return -ENOMEM;
	ret = pread(fd, buf, sectorsize, old_bytenr);
	if (ret != sectorsize)
		goto fail;

	super = (struct btrfs_super_block *)buf;
	BUG_ON(btrfs_super_bytenr(super) != old_bytenr);
	btrfs_set_super_bytenr(super, BTRFS_SUPER_INFO_OFFSET);

	crc = crc32c(crc, buf + BTRFS_CSUM_SIZE, len);
	crc = ~cpu_to_le32(crc);
	memcpy(super->csum, &crc, BTRFS_CRC32_SIZE);

	ret = pwrite(fd, buf, sectorsize, BTRFS_SUPER_INFO_OFFSET);
	if (ret < 0)
		goto fail;
	/* How to handle this case? */
	BUG_ON(ret != sectorsize);

	ret = fsync(fd);
	if (ret)
		goto fail;

	memset(buf, 0, sectorsize);
	for (bytenr = 0; bytenr < BTRFS_SUPER_INFO_OFFSET; ) {
		len = BTRFS_SUPER_INFO_OFFSET - bytenr;
		if (len > sectorsize)
			len = sectorsize;
		ret = pwrite(fd, buf, len, bytenr);
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
int do_convert(const char *devname, int datacsum)
{
	int i, fd, ret;
	u32 blocksize;
	u64 blocks[4];
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
	for (i = 0; i < 4; i++) {
		ret = ext2_alloc_block(ext2_fs, 0, blocks + i);
		if (ret) {
			fprintf(stderr, "free space isn't enough\n");
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
	ret = make_btrfs(fd, blocks, total_bytes, blocksize,
			 blocksize, blocksize, blocksize);
	if (ret) {
		fprintf(stderr, "unable to create initial ctree\n");
		goto fail;
	}
	root = open_ctree_fd(fd, super_bytenr);
	if (!root) {
		fprintf(stderr, "unable to open ctree\n");
		goto fail;
	}
	fd = dup(fd);
	if (fd < 0) {
		fprintf(stderr, "unable to duplicate file descriptor\n");
		goto fail;
	}
	root->fs_info->priv_data = ext2_fs;
	root->fs_info->extent_ops = &extent_ops;
	ret = init_btrfs(root);
	if (ret) {
		fprintf(stderr, "unable to setup the root tree\n");
		goto fail;
	}
	ext2_root = create_subvol(root, "ext2_saved");
	if (!ext2_root) {
		fprintf(stderr, "unable to create subvol\n");
		goto fail;
	}
	printf("creating btrfs metadata.\n");
	ret = copy_inodes(root, ext2_fs, datacsum);
	if (ret) {
		fprintf(stderr, "error during copy_inodes %d\n", ret);
		goto fail;
	}
	printf("creating ext2fs image file.\n");
	ret = create_ext2_image(ext2_root, ext2_fs, "image");
	if (ret) {
		fprintf(stderr, "error during create_ext2_image %d\n", ret);
		goto fail;
	}
	btrfs_free_fs_root(ext2_root->fs_info, ext2_root);
	ret = close_ctree(root);
	if (ret) {
		fprintf(stderr, "error during close_ctree %d\n", ret);
		goto fail;
	}
	close_ext2fs(ext2_fs);

	/* finally migrate super block to its default postion */
	ret = migrate_super_block(fd, super_bytenr, blocksize);
	if (ret) {
		fprintf(stderr, "unable to migrate super block\n");
		goto fail;
	}
	close(fd);
	printf("conversion complete.\n");
	return 0;
fail:
	fprintf(stderr, "conversion aborted.\n");
	return -1;
}

int do_rollback(const char *devname, int force)
{
	int fd;
	int ret;
	int modified = 0;
	struct btrfs_root *root;
	struct btrfs_root *ext2_root;
	struct btrfs_dir_item *dir;
	struct btrfs_inode_item *inode;
	struct btrfs_file_extent_item *fi;
	struct btrfs_inode_timespec *tspec;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	struct btrfs_path path;
	char *buf;
	char *name;
	u64 bytenr;
	u64 num_bytes;
	u64 root_dir;
	u64 objectid;
	u64 offset;
	u64 first_free;
	u64 last_trans;
	u64 total_bytes;

	fd = open(devname, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "unable to open %s\n", devname);
		goto fail;
	}
	root = open_ctree_fd(fd, 0);
	if (!root) {
		fprintf(stderr, "unable to open ctree\n");
		goto fail;
	}
	fd = dup(fd);
	if (fd < 0) {
		fprintf(stderr, "unable to duplicate file descriptor\n");
		goto fail;
	}

	first_free = BTRFS_SUPER_INFO_OFFSET + root->sectorsize * 2 - 1;
	first_free &= ~((u64)root->sectorsize - 1);
	buf = malloc(first_free);
	if (!buf) {
		fprintf(stderr, "unable to allocate memory\n");
		goto fail;
	}

	btrfs_init_path(&path);
	name = "ext2_saved";
	root_dir = btrfs_super_root_dir(&root->fs_info->super_copy);
	dir = btrfs_lookup_dir_item(NULL, root->fs_info->tree_root, &path,
				   root_dir, name, strlen(name), 0);
	if (!dir || IS_ERR(dir)) {
		fprintf(stderr, "unable to find subvol %s\n", name);
		goto fail;
	}
	leaf = path.nodes[0];
	btrfs_dir_item_key_to_cpu(leaf, dir, &key);
	btrfs_release_path(root->fs_info->tree_root, &path);

	ext2_root = btrfs_read_fs_root(root->fs_info, &key);
	if (!ext2_root || IS_ERR(ext2_root)) {
		fprintf(stderr, "unable to open subvol %s\n", name);
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
	tspec = btrfs_inode_otime(inode);
	/*
	 * get image file size and transaction id stored in 'otime' field.
	 * see comments in create_ext2_image.
	 */
	last_trans = btrfs_timespec_sec(leaf, tspec);
	total_bytes = btrfs_timespec_nsec(leaf, tspec);
	total_bytes *= root->sectorsize;
	btrfs_release_path(ext2_root, &path);
	if (total_bytes != btrfs_inode_size(leaf, inode)) {
		fprintf(stderr, "image file size mismatch\n");
		goto fail;
	}

	key.objectid = objectid;
	key.offset = 0;
	btrfs_set_key_type(&key, BTRFS_EXTENT_DATA_KEY);
	ret = btrfs_search_slot(NULL, ext2_root, &key, &path, 0, 0);
	if (ret != 0) {
		fprintf(stderr, "unable to find first file extent\n");
		goto fail;
	}

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
		if (btrfs_file_extent_generation(leaf, fi) > last_trans) {
			modified = 1;
			break;
		}
		if (btrfs_file_extent_type(leaf, fi) != BTRFS_FILE_EXTENT_REG)
			break;

		if (offset >= first_free)
			goto next;

		bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
		if (bytenr == 0)
			break;
		bytenr += btrfs_file_extent_offset(leaf, fi);
		num_bytes = btrfs_file_extent_num_bytes(leaf, fi);
		if (num_bytes > first_free - offset)
			num_bytes = first_free - offset;

		ret = pread(fd, buf + offset, num_bytes, bytenr);
		if (ret != num_bytes) {
			fprintf(stderr, "unable to read required data\n");
			btrfs_release_path(ext2_root, &path);
			goto fail;
		}
next:
		offset += btrfs_file_extent_num_bytes(leaf, fi);
		path.slots[0]++;
	}
	btrfs_release_path(ext2_root, &path);

	if (modified) {
		fprintf(stderr, "image file has been modified\n");
		goto fail;
	}
	if (offset < total_bytes) {
		fprintf(stderr, "unable to check all file extents\n");
		goto fail;
	}

	btrfs_free_fs_root(ext2_root->fs_info, ext2_root);
	ret = close_ctree(root);
	if (ret) {
		fprintf(stderr, "error during close_ctree %d\n", ret);
		goto fail;
	}

	ret = pwrite(fd, buf, first_free, 0);
	if (ret < 0) {
		fprintf(stderr, "error during pwrite %d\n", ret);
		goto fail;
	}
	/* How to handle this case? */
	BUG_ON(ret != first_free);
	ret = fsync(fd);
	if (ret) {
		fprintf(stderr, "error during fsync %d\n", ret);
		goto fail;
	}
	close(fd);
	free(buf);
	printf("rollback complete.\n");
	return 0;
fail:
	fprintf(stderr, "rollback aborted.\n");
	return -1;
}

static void print_usage(void)
{
	printf("usage: btrfs-convert [-d] [-r] device\n");
	printf("\t-d disable data checksum\n");
	printf("\t-r roll back to ext2fs\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	int ret;
	int datacsum = 1;
	int rollback = 0;
	char *file;

	while(1) {
		int c = getopt(argc, argv, "dr");
		if (c < 0)
			break;
		switch(c) {
			case 'd':
				datacsum = 0;
				break;
			case 'r':
				rollback = 1;
				break;
			default:
				print_usage();
		}
	}
	argc = argc - optind;
	if (argc == 1) {
		file = argv[optind];
	} else {
		print_usage();
	}
	if (rollback) {
		ret = do_rollback(file, 0);
	} else {
		ret = do_convert(file, datacsum);
	}
	return ret;
}
