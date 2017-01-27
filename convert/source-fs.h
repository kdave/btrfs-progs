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

#ifndef __BTRFS_CONVERT_SOURCE_FS_H__
#define __BTRFS_CONVERT_SOURCE_FS_H__

#include "kerncompat.h"

#define CONV_IMAGE_SUBVOL_OBJECTID BTRFS_FIRST_FREE_OBJECTID

struct task_ctx {
	uint32_t max_copy_inodes;
	uint32_t cur_copy_inodes;
	struct task_info *info;
};

struct btrfs_convert_context;
struct btrfs_root;
struct btrfs_trans_handle;
struct btrfs_inode_item;

struct btrfs_convert_operations {
	const char *name;
	int (*open_fs)(struct btrfs_convert_context *cctx, const char *devname);
	int (*read_used_space)(struct btrfs_convert_context *cctx);
	int (*copy_inodes)(struct btrfs_convert_context *cctx,
			 struct btrfs_root *root, int datacsum,
			 int packing, int noxattr, struct task_ctx *p);
	void (*close_fs)(struct btrfs_convert_context *cctx);
	int (*check_state)(struct btrfs_convert_context *cctx);
};

struct blk_iterate_data {
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	struct btrfs_root *convert_root;
	struct btrfs_inode_item *inode;
	u64 convert_ino;
	u64 objectid;
	u64 first_block;
	u64 disk_block;
	u64 num_blocks;
	u64 boundary;
	int checksum;
	int errcode;
};

void init_convert_context(struct btrfs_convert_context *cctx);
void clean_convert_context(struct btrfs_convert_context *cctx);
int block_iterate_proc(u64 disk_block, u64 file_block,
		              struct blk_iterate_data *idata);
void init_blk_iterate_data(struct blk_iterate_data *data,
				  struct btrfs_trans_handle *trans,
				  struct btrfs_root *root,
				  struct btrfs_inode_item *inode,
				  u64 objectid, int checksum);
int convert_insert_dirent(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 const char *name, size_t name_len,
				 u64 dir, u64 objectid,
				 u8 file_type, u64 index_cnt,
				 struct btrfs_inode_item *inode);
int read_disk_extent(struct btrfs_root *root, u64 bytenr,
		            u32 num_bytes, char *buffer);
int record_file_blocks(struct blk_iterate_data *data,
			      u64 file_block, u64 disk_block, u64 num_blocks);

#endif
