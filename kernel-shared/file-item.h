/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_FILE_ITEM_H
#define BTRFS_FILE_ITEM_H

#include "kerncompat.h"
#include <stdbool.h>
#include <stddef.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/compression.h"

struct bio;
struct inode;
struct btrfs_ordered_sum;
struct btrfs_inode;
struct btrfs_trans_handle;
struct extent_map;
struct extent_buffer;
struct list_head;

#define BTRFS_FILE_EXTENT_INLINE_DATA_START		\
		(offsetof(struct btrfs_file_extent_item, disk_bytenr))

static inline u32 BTRFS_MAX_INLINE_DATA_SIZE(const struct btrfs_fs_info *info)
{
	return BTRFS_MAX_ITEM_SIZE(info) - BTRFS_FILE_EXTENT_INLINE_DATA_START;
}

/*
 * Return the number of bytes used by the item on disk, minus the size of any
 * extent headers.  If a file is compressed on disk, this is the compressed
 * size.
 */
static inline u32 btrfs_file_extent_inline_item_len(
						const struct extent_buffer *eb,
						int nr)
{
	return btrfs_item_size(eb, nr) - BTRFS_FILE_EXTENT_INLINE_DATA_START;
}

static inline unsigned long btrfs_file_extent_inline_start(
				const struct btrfs_file_extent_item *e)
{
	return (unsigned long)e + BTRFS_FILE_EXTENT_INLINE_DATA_START;
}

static inline u32 btrfs_file_extent_calc_inline_size(u32 datasize)
{
	return BTRFS_FILE_EXTENT_INLINE_DATA_START + datasize;
}

int btrfs_del_csums(struct btrfs_trans_handle *trans,
		    struct btrfs_root *root, u64 bytenr, u64 len);
blk_status_t btrfs_lookup_bio_sums(struct inode *inode, struct bio *bio, u8 *dst);
int btrfs_insert_hole_extent(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 objectid, u64 pos,
			     u64 num_bytes);
int btrfs_lookup_file_extent(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     struct btrfs_path *path, u64 objectid,
			     u64 bytenr, int mod);
int btrfs_csum_file_blocks(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct btrfs_ordered_sum *sums);
blk_status_t btrfs_csum_one_bio(struct btrfs_inode *inode, struct bio *bio,
				u64 offset, bool one_ordered);
int btrfs_lookup_csums_range(struct btrfs_root *root, u64 start, u64 end,
			     struct list_head *list, int search_commit,
			     bool nowait);
void btrfs_extent_item_to_extent_map(struct btrfs_inode *inode,
				     const struct btrfs_path *path,
				     struct btrfs_file_extent_item *fi,
				     struct extent_map *em);
int btrfs_inode_clear_file_extent_range(struct btrfs_inode *inode, u64 start,
					u64 len);
int btrfs_inode_set_file_extent_range(struct btrfs_inode *inode, u64 start, u64 len);
void btrfs_inode_safe_disk_i_size_write(struct btrfs_inode *inode, u64 new_i_size);
u64 btrfs_file_extent_end(const struct btrfs_path *path);

/*
 * MODIFIED:
 *  - This function doesn't exist in the kernel.
 */
int btrfs_insert_file_extent(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     u64 ino, u64 file_pos,
			     struct btrfs_file_extent_item *stack_fi);
int btrfs_csum_file_block(struct btrfs_trans_handle *trans, u64 logical,
			  u64 csum_objectid, u32 csum_type, const char *data);
struct btrfs_csum_item *
btrfs_lookup_csum(struct btrfs_trans_handle *trans,
		  struct btrfs_root *root,
		  struct btrfs_path *path,
		  u64 bytenr, u64 csum_objectid, u16 csum_type, int cow);
int btrfs_insert_inline_extent(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 objectid,
			       u64 offset, const char *buffer, size_t size,
			       enum btrfs_compression_type comp, u64 ram_bytes);
/*
 * For symlink we allow up to PATH_MAX - 1 (PATH_MAX includes the terminating NUL,
 * but fs doesn't store that terminating NUL).
 *
 * But for inlined data extents, the up limit is sectorsize - 1 (inclusive), or a
 * regular extent should be created instead.
 */
static inline u32 btrfs_symlink_max_size(struct btrfs_fs_info *fs_info)
{
	return min_t(u32, BTRFS_MAX_INLINE_DATA_SIZE(fs_info),
		     PATH_MAX - 1);
}

static inline u32 btrfs_data_inline_max_size(struct btrfs_fs_info *fs_info)
{
	return min_t(u32, BTRFS_MAX_INLINE_DATA_SIZE(fs_info),
		     fs_info->sectorsize - 1);
}

#endif
