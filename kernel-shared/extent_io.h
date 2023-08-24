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

#ifndef __BTRFS_EXTENT_IO_H__
#define __BTRFS_EXTENT_IO_H__

#include "kerncompat.h"
#include "common/extent-cache.h"
#include "kernel-lib/list.h"

struct extent_io_tree;

#define EXTENT_BUFFER_UPTODATE		(1U << 0)
#define EXTENT_BUFFER_DIRTY		(1U << 1)
#define EXTENT_BUFFER_BAD_TRANSID	(1U << 2)
#define EXTENT_BUFFER_DUMMY		(1U << 3)

#define BLOCK_GROUP_DATA	(1U << 1)
#define BLOCK_GROUP_METADATA	(1U << 2)
#define BLOCK_GROUP_SYSTEM	(1U << 4)

/*
 * The extent buffer bitmap operations are done with byte granularity instead of
 * word granularity for two reasons:
 * 1. The bitmaps must be little-endian on disk.
 * 2. Bitmap items are not guaranteed to be aligned to a word and therefore a
 *    single word in a bitmap may straddle two pages in the extent buffer.
 */
#define BIT_BYTE(nr) ((nr) / BITS_PER_BYTE)
#define BYTE_MASK ((1 << BITS_PER_BYTE) - 1)
#define BITMAP_FIRST_BYTE_MASK(start) \
	((BYTE_MASK << ((start) & (BITS_PER_BYTE - 1))) & BYTE_MASK)
#define BITMAP_LAST_BYTE_MASK(nbits) \
	(BYTE_MASK >> (-(nbits) & (BITS_PER_BYTE - 1)))

static inline int le_test_bit(int nr, const u8 *addr)
{
	return 1U & (addr[BIT_BYTE(nr)] >> (nr & (BITS_PER_BYTE-1)));
}

struct btrfs_fs_info;
struct btrfs_trans_handle;

struct extent_buffer {
	struct cache_extent cache_node;
	u64 start;
	struct list_head lru;
	struct list_head recow;
	u32 len;
	int refs;
	u32 flags;
	struct btrfs_fs_info *fs_info;
	char data[] __attribute__((aligned(8)));
};

static inline void extent_buffer_get(struct extent_buffer *eb)
{
	eb->refs++;
}

static inline int set_extent_buffer_uptodate(struct extent_buffer *eb)
{
	eb->flags |= EXTENT_BUFFER_UPTODATE;
	return 0;
}

static inline int clear_extent_buffer_uptodate(struct extent_buffer *eb)
{
	eb->flags &= ~EXTENT_BUFFER_UPTODATE;
	return 0;
}

static inline int extent_buffer_uptodate(struct extent_buffer *eb)
{
	if (!eb || IS_ERR(eb))
		return 0;
	if (eb->flags & EXTENT_BUFFER_UPTODATE)
		return 1;
	return 0;
}

struct extent_buffer *find_extent_buffer(struct btrfs_fs_info *fs_info,
					 u64 bytenr);
struct extent_buffer *find_first_extent_buffer(struct btrfs_fs_info *fs_info,
					       u64 start);
struct extent_buffer *alloc_extent_buffer(struct btrfs_fs_info *fs_info,
					  u64 bytenr, u32 blocksize);
struct extent_buffer *btrfs_clone_extent_buffer(struct extent_buffer *src);
struct extent_buffer *alloc_dummy_extent_buffer(struct btrfs_fs_info *fs_info,
						u64 bytenr, u32 blocksize);
void free_extent_buffer(struct extent_buffer *eb);
void free_extent_buffer_nocache(struct extent_buffer *eb);
void free_extent_buffer_stale(struct extent_buffer *eb);
int memcmp_extent_buffer(const struct extent_buffer *eb, const void *ptrv,
			 unsigned long start, unsigned long len);
void read_extent_buffer(const struct extent_buffer *eb, void *dst,
			unsigned long start, unsigned long len);
void write_extent_buffer_fsid(const struct extent_buffer *eb, const void *src);
void write_extent_buffer_chunk_tree_uuid(const struct extent_buffer *eb,
		const void *src);
void write_extent_buffer(const struct extent_buffer *eb, const void *src,
			 unsigned long start, unsigned long len);
void copy_extent_buffer_full(const struct extent_buffer *dst,
			     const struct extent_buffer *src);
void copy_extent_buffer(const struct extent_buffer *dst,
			const struct extent_buffer *src,
			unsigned long dst_offset, unsigned long src_offset,
			unsigned long len);
void memcpy_extent_buffer(const struct extent_buffer *dst,
			  unsigned long dst_offset, unsigned long src_offset,
			  unsigned long len);
void memmove_extent_buffer(const struct extent_buffer *dst,
			   const unsigned long dst_offset,
			   unsigned long src_offset, unsigned long len);
void memset_extent_buffer(const struct extent_buffer *eb, char c,
			  unsigned long start, unsigned long len);
int extent_buffer_test_bit(const struct extent_buffer *eb, unsigned long start,
			   unsigned long nr);
int set_extent_buffer_dirty(struct extent_buffer *eb);
int btrfs_clear_buffer_dirty(struct btrfs_trans_handle *trans,
			     struct extent_buffer *eb);
int read_data_from_disk(struct btrfs_fs_info *info, void *buf, u64 logical,
			u64 *len, int mirror);
int write_data_to_disk(struct btrfs_fs_info *info, const void *buf, u64 offset,
		       u64 bytes);
void extent_buffer_bitmap_clear(struct extent_buffer *eb, unsigned long start,
                                unsigned long pos, unsigned long len);
void extent_buffer_bitmap_set(struct extent_buffer *eb, unsigned long start,
                              unsigned long pos, unsigned long len);
void extent_buffer_init_cache(struct btrfs_fs_info *fs_info);
void extent_buffer_free_cache(struct btrfs_fs_info *fs_info);
void btrfs_readahead_node_child(struct extent_buffer *node, int slot);

#endif
