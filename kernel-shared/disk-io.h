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

#ifndef __BTRFS_DISK_IO_H__
#define __BTRFS_DISK_IO_H__

#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "kernel-lib/sizes.h"

#define BTRFS_SUPER_MIRROR_MAX	 3
#define BTRFS_SUPER_MIRROR_SHIFT 12

enum btrfs_open_ctree_flags {
	/* Open filesystem for writes */
	OPEN_CTREE_WRITES		= (1U << 0),
	/* Allow to open filesystem with some broken tree roots (eg log root) */
	OPEN_CTREE_PARTIAL		= (1U << 1),
	/* If primary root pinters are invalid, try backup copies */
	OPEN_CTREE_BACKUP_ROOT		= (1U << 2),
	/* Allow reading all superblock copies if the primary is damaged */
	OPEN_CTREE_RECOVER_SUPER	= (1U << 3),
	/* Restoring filesystem image */
	OPEN_CTREE_RESTORE		= (1U << 4),
	/* Do not read block groups (extent tree) */
	OPEN_CTREE_NO_BLOCK_GROUPS	= (1U << 5),
	/* Open all devices in O_EXCL mode */
	OPEN_CTREE_EXCLUSIVE		= (1U << 6),
	/* Do not scan devices */
	OPEN_CTREE_NO_DEVICES		= (1U << 7),
	/*
	 * Don't print error messages if bytenr or checksums do not match in
	 * tree block headers. Turn on by OPEN_CTREE_SUPPRESS_ERROR
	 */
	OPEN_CTREE_SUPPRESS_CHECK_BLOCK_ERRORS	= (1U << 8),
	/* Return the chunk root */
	__OPEN_CTREE_RETURN_CHUNK_ROOT	= (1U << 9),
	OPEN_CTREE_CHUNK_ROOT_ONLY	= OPEN_CTREE_PARTIAL +
					  OPEN_CTREE_SUPPRESS_CHECK_BLOCK_ERRORS +
					  __OPEN_CTREE_RETURN_CHUNK_ROOT,
	/*
	 * TODO: cleanup: Split the open_ctree_flags into more independent
	 * Tree bits.
	 * Like split PARTIAL into SKIP_CSUM/SKIP_EXTENT
	 */

	/* Ignore UUID mismatches */
	OPEN_CTREE_IGNORE_FSID_MISMATCH	= (1U << 10),

	/*
	 * Allow open_ctree_fs_info() to return an incomplete fs_info with
	 * system chunks from super block only.
	 * It's useful when chunks are corrupted.
	 * Makes no sense for open_ctree variants returning btrfs_root.
	 */
	OPEN_CTREE_IGNORE_CHUNK_TREE_ERROR = (1U << 11),

	/*
	 * Allow to open fs with temporary superblock (BTRFS_MAGIC_PARTIAL),
	 * such fs contains very basic tree layout, just able to be opened.
	 * Such temporary super is used for mkfs or convert.
	 */
	OPEN_CTREE_TEMPORARY_SUPER = (1U << 12),

	/*
	 * Invalidate the free space tree (i.e., clear the FREE_SPACE_TREE_VALID
	 * compat_ro bit).
	 */
	OPEN_CTREE_INVALIDATE_FST = (1U << 13),

	/* For print-tree, print HIDDEN instead of filenames/xattrs/refs */
	OPEN_CTREE_HIDE_NAMES = (1U << 14),

	/*
	 * Allow certain commands like check/restore to ignore transid
	 * mismatch.
	 */
	OPEN_CTREE_ALLOW_TRANSID_MISMATCH = (1U << 15),

	/*
	 * Do not check checksums at all for data and metadata, eg. when the
	 * superblock type of checksum does not match the actual checksum items
	 * stored in the csum tree during conversion.
	 */
	OPEN_CTREE_SKIP_CSUM_CHECK	= (1U << 16),
};

/*
 * Modes of superblock access
 */
enum btrfs_read_sb_flags {
	SBREAD_DEFAULT		= 0,
	/* Reading superblock during recovery */
	SBREAD_RECOVER		= (1 << 0),

	/*
	 * Read superblock with the fake signature, cannot be used with
	 * SBREAD_RECOVER
	 */
	SBREAD_TEMPORARY = (1 << 1),

	/*
	 * Equivalent of OPEN_CTREE_IGNORE_FSID_MISMATCH, allow to read
	 * superblock that has mismatched sb::fsid and sb::dev_item.fsid
	 */
	SBREAD_IGNORE_FSID_MISMATCH = (1 << 2),
};

/*
 * Use macro to define mirror super block position,
 * so we can use it in static array initialization
 */
#define BTRFS_SB_MIRROR_OFFSET(mirror)	((u64)(SZ_16K) << \
		(BTRFS_SUPER_MIRROR_SHIFT * (mirror)))

static inline u64 btrfs_sb_offset(int mirror)
{
	if (mirror)
		return BTRFS_SB_MIRROR_OFFSET(mirror);
	return BTRFS_SUPER_INFO_OFFSET;
}

struct btrfs_device;

int read_whole_eb(struct btrfs_fs_info *info, struct extent_buffer *eb, int mirror);
struct extent_buffer* read_tree_block(struct btrfs_fs_info *fs_info, u64 bytenr,
		u64 parent_transid);

void readahead_tree_block(struct btrfs_fs_info *fs_info, u64 bytenr,
			  u64 parent_transid);
struct extent_buffer* btrfs_find_create_tree_block(
		struct btrfs_fs_info *fs_info, u64 bytenr);

void btrfs_setup_root(struct btrfs_root *root, struct btrfs_fs_info *fs_info,
		      u64 objectid);
int clean_tree_block(struct extent_buffer *buf);

void btrfs_free_fs_info(struct btrfs_fs_info *fs_info);
struct btrfs_fs_info *btrfs_new_fs_info(int writable, u64 sb_bytenr);
int btrfs_check_fs_compatibility(struct btrfs_super_block *sb,
				 unsigned int flags);
int btrfs_setup_all_roots(struct btrfs_fs_info *fs_info, u64 root_tree_bytenr,
			  unsigned flags);
void btrfs_release_all_roots(struct btrfs_fs_info *fs_info);
void btrfs_cleanup_all_caches(struct btrfs_fs_info *fs_info);
int btrfs_scan_fs_devices(int fd, const char *path,
			  struct btrfs_fs_devices **fs_devices, u64 sb_bytenr,
			  unsigned sbflags, int skip_devices);
int btrfs_setup_chunk_tree_and_device_map(struct btrfs_fs_info *fs_info,
			  u64 chunk_root_bytenr);

struct btrfs_root *open_ctree(const char *filename, u64 sb_bytenr,
			      unsigned flags);
struct btrfs_root *open_ctree_fd(int fp, const char *path, u64 sb_bytenr,
				 unsigned flags);
struct open_ctree_flags {
	const char *filename;
	u64 sb_bytenr;
	u64 root_tree_bytenr;
	u64 chunk_tree_bytenr;
	unsigned flags;
};

struct btrfs_fs_info *open_ctree_fs_info(struct open_ctree_flags *ocf);
int close_ctree_fs_info(struct btrfs_fs_info *fs_info);
static inline int close_ctree(struct btrfs_root *root)
{
	if (!root)
		return 0;
	return close_ctree_fs_info(root->fs_info);
}

int write_all_supers(struct btrfs_fs_info *fs_info);
int write_ctree_super(struct btrfs_trans_handle *trans);
int btrfs_check_super(struct btrfs_super_block *sb, unsigned sbflags);
int btrfs_read_dev_super(int fd, struct btrfs_super_block *sb, u64 sb_bytenr,
		unsigned sbflags);
int btrfs_map_bh_to_logical(struct btrfs_root *root, struct extent_buffer *bh,
			    u64 logical);
struct extent_buffer *btrfs_find_tree_block(struct btrfs_fs_info *fs_info,
					    u64 bytenr, u32 blocksize);
struct btrfs_root *btrfs_read_fs_root(struct btrfs_fs_info *fs_info,
				      struct btrfs_key *location);
struct btrfs_root *btrfs_read_fs_root_no_cache(struct btrfs_fs_info *fs_info,
					       struct btrfs_key *location);
int btrfs_free_fs_root(struct btrfs_root *root);
void btrfs_mark_buffer_dirty(struct extent_buffer *buf);
int btrfs_buffer_uptodate(struct extent_buffer *buf, u64 parent_transid);
int btrfs_set_buffer_uptodate(struct extent_buffer *buf);
int btrfs_csum_data(struct btrfs_fs_info *fs_info, u16 csum_type, const u8 *data,
		    u8 *out, size_t len);

int btrfs_open_device(struct btrfs_device *dev);
int csum_tree_block_size(struct extent_buffer *buf, u16 csum_sectorsize,
			 int verify, u16 csum_type);
int verify_tree_block_csum_silent(struct extent_buffer *buf, u16 csum_size,
				  u16 csum_type);
int btrfs_read_buffer(struct extent_buffer *buf, u64 parent_transid);
int write_tree_block(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info,
		     struct extent_buffer *eb);
int write_and_map_eb(struct btrfs_fs_info *fs_info, struct extent_buffer *eb);
int btrfs_fs_roots_compare_roots(struct rb_node *node1, struct rb_node *node2);
struct btrfs_root *btrfs_create_tree(struct btrfs_trans_handle *trans,
				     struct btrfs_fs_info *fs_info,
				     struct btrfs_key *key);
int btrfs_delete_and_free_root(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root);
struct btrfs_root *btrfs_csum_root(struct btrfs_fs_info *fs_info, u64 bytenr);
struct btrfs_root *btrfs_extent_root(struct btrfs_fs_info *fs_inf, u64 bytenr);
struct btrfs_root *btrfs_global_root(struct btrfs_fs_info *fs_info,
				     struct btrfs_key *key);
u64 btrfs_global_root_id(struct btrfs_fs_info *fs_info, u64 bytenr);
int btrfs_global_root_insert(struct btrfs_fs_info *fs_info,
			     struct btrfs_root *root);

static inline struct btrfs_root *btrfs_block_group_root(
						struct btrfs_fs_info *fs_info)
{
	if (btrfs_fs_incompat(fs_info, EXTENT_TREE_V2))
		return fs_info->block_group_root;
	return btrfs_extent_root(fs_info, 0);
}

#endif
