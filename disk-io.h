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

#ifndef __DISKIO__
#define __DISKIO__

#define BTRFS_SUPER_INFO_OFFSET (64 * 1024)
#define BTRFS_SUPER_INFO_SIZE 4096

#define BTRFS_SUPER_MIRROR_MAX	 3
#define BTRFS_SUPER_MIRROR_SHIFT 12

static inline u64 btrfs_sb_offset(int mirror)
{
	u64 start = 16 * 1024;
	if (mirror)
		return start << (BTRFS_SUPER_MIRROR_SHIFT * mirror);
	return BTRFS_SUPER_INFO_OFFSET;
}

struct btrfs_device;

int read_whole_eb(struct btrfs_fs_info *info, struct extent_buffer *eb, int mirror);
struct extent_buffer *read_tree_block(struct btrfs_root *root, u64 bytenr,
				      u32 blocksize, u64 parent_transid);
int readahead_tree_block(struct btrfs_root *root, u64 bytenr, u32 blocksize,
			 u64 parent_transid);
int write_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct extent_buffer *eb);
struct extent_buffer *btrfs_find_create_tree_block(struct btrfs_root *root,
						   u64 bytenr, u32 blocksize);

int __setup_root(u32 nodesize, u32 leafsize, u32 sectorsize,
                        u32 stripesize, struct btrfs_root *root,
                        struct btrfs_fs_info *fs_info, u64 objectid);
int clean_tree_block(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root, struct extent_buffer *buf);
struct btrfs_root *open_ctree(const char *filename, u64 sb_bytenr, int writes);
struct btrfs_root *open_ctree_fd(int fp, const char *path, u64 sb_bytenr,
				 int writes);
struct btrfs_fs_info *open_ctree_fs_info_restore(const char *filename,
					 u64 sb_bytenr, u64 root_tree_bytenr,
					 int writes, int partial);
struct btrfs_fs_info *open_ctree_fs_info(const char *filename,
					 u64 sb_bytenr, u64 root_tree_bytenr,
					 int writes, int partial);
int close_ctree(struct btrfs_root *root);
int write_all_supers(struct btrfs_root *root);
int write_ctree_super(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root);
int btrfs_read_dev_super(int fd, struct btrfs_super_block *sb, u64 sb_bytenr);
int btrfs_map_bh_to_logical(struct btrfs_root *root, struct extent_buffer *bh,
			    u64 logical);
struct extent_buffer *btrfs_find_tree_block(struct btrfs_root *root,
					    u64 bytenr, u32 blocksize);
struct btrfs_root *btrfs_read_fs_root(struct btrfs_fs_info *fs_info,
				      struct btrfs_key *location);
struct btrfs_root *btrfs_read_fs_root_no_cache(struct btrfs_fs_info *fs_info,
					       struct btrfs_key *location);
int btrfs_free_fs_root(struct btrfs_fs_info *fs_info, struct btrfs_root *root);
void btrfs_mark_buffer_dirty(struct extent_buffer *buf);
int btrfs_buffer_uptodate(struct extent_buffer *buf, u64 parent_transid);
int btrfs_set_buffer_uptodate(struct extent_buffer *buf);
int wait_on_tree_block_writeback(struct btrfs_root *root,
				 struct extent_buffer *buf);
u32 btrfs_csum_data(struct btrfs_root *root, char *data, u32 seed, size_t len);
void btrfs_csum_final(u32 crc, char *result);

int btrfs_commit_transaction(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root);
int btrfs_open_device(struct btrfs_device *dev);
int csum_tree_block_size(struct extent_buffer *buf, u16 csum_sectorsize,
			 int verify);
int csum_tree_block(struct btrfs_root *root, struct extent_buffer *buf,
		    int verify);
int btrfs_read_buffer(struct extent_buffer *buf, u64 parent_transid);
#endif

/* raid6.c */
void raid6_gen_syndrome(int disks, size_t bytes, void **ptrs);
