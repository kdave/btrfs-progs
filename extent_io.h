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

#ifndef __EXTENTMAP__
#define __EXTENTMAP__

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#include "extent-cache.h"
#include "list.h"
#else
#include <btrfs/kerncompat.h>
#include <btrfs/extent-cache.h>
#include <btrfs/list.h>
#endif /* BTRFS_FLAT_INCLUDES */

#define EXTENT_DIRTY 1
#define EXTENT_WRITEBACK (1 << 1)
#define EXTENT_UPTODATE (1 << 2)
#define EXTENT_LOCKED (1 << 3)
#define EXTENT_NEW (1 << 4)
#define EXTENT_DELALLOC (1 << 5)
#define EXTENT_DEFRAG (1 << 6)
#define EXTENT_DEFRAG_DONE (1 << 7)
#define EXTENT_BUFFER_FILLED (1 << 8)
#define EXTENT_CSUM (1 << 9)
#define EXTENT_IOBITS (EXTENT_LOCKED | EXTENT_WRITEBACK)

struct extent_io_tree {
	struct cache_tree state;
	struct cache_tree cache;
	struct list_head lru;
	u64 cache_size;
};

struct extent_state {
	struct cache_extent cache_node;
	u64 start;
	u64 end;
	int refs;
	unsigned long state;
	u64 xprivate;
};

struct extent_buffer {
	struct cache_extent cache_node;
	u64 start;
	u64 dev_bytenr;
	u32 len;
	struct extent_io_tree *tree;
	struct list_head lru;
	int refs;
	int flags;
	int fd;
	char data[];
};

static inline void extent_buffer_get(struct extent_buffer *eb)
{
	eb->refs++;
}

void extent_io_tree_init(struct extent_io_tree *tree);
void extent_io_tree_cleanup(struct extent_io_tree *tree);
int set_extent_bits(struct extent_io_tree *tree, u64 start,
		    u64 end, int bits, gfp_t mask);
int clear_extent_bits(struct extent_io_tree *tree, u64 start,
		      u64 end, int bits, gfp_t mask);
int find_first_extent_bit(struct extent_io_tree *tree, u64 start,
			  u64 *start_ret, u64 *end_ret, int bits);
int test_range_bit(struct extent_io_tree *tree, u64 start, u64 end,
		   int bits, int filled);
int set_extent_dirty(struct extent_io_tree *tree, u64 start,
		     u64 end, gfp_t mask);
int clear_extent_dirty(struct extent_io_tree *tree, u64 start,
		       u64 end, gfp_t mask);
int extent_buffer_uptodate(struct extent_buffer *eb);
int set_extent_buffer_uptodate(struct extent_buffer *eb);
int clear_extent_buffer_uptodate(struct extent_io_tree *tree,
				struct extent_buffer *eb);
int set_state_private(struct extent_io_tree *tree, u64 start, u64 xprivate);
int get_state_private(struct extent_io_tree *tree, u64 start, u64 *xprivate);
struct extent_buffer *find_extent_buffer(struct extent_io_tree *tree,
					 u64 bytenr, u32 blocksize);
struct extent_buffer *find_first_extent_buffer(struct extent_io_tree *tree,
					       u64 start);
struct extent_buffer *alloc_extent_buffer(struct extent_io_tree *tree,
					  u64 bytenr, u32 blocksize);
void free_extent_buffer(struct extent_buffer *eb);
int read_extent_from_disk(struct extent_buffer *eb,
			  unsigned long offset, unsigned long len);
int write_extent_to_disk(struct extent_buffer *eb);
int memcmp_extent_buffer(struct extent_buffer *eb, const void *ptrv,
			 unsigned long start, unsigned long len);
void read_extent_buffer(struct extent_buffer *eb, void *dst,
			unsigned long start, unsigned long len);
void write_extent_buffer(struct extent_buffer *eb, const void *src,
			 unsigned long start, unsigned long len);
void copy_extent_buffer(struct extent_buffer *dst, struct extent_buffer *src,
			unsigned long dst_offset, unsigned long src_offset,
			unsigned long len);
void memcpy_extent_buffer(struct extent_buffer *dst, unsigned long dst_offset,
			  unsigned long src_offset, unsigned long len);
void memmove_extent_buffer(struct extent_buffer *dst, unsigned long dst_offset,
			   unsigned long src_offset, unsigned long len);
void memset_extent_buffer(struct extent_buffer *eb, char c,
			  unsigned long start, unsigned long len);
int set_extent_buffer_dirty(struct extent_buffer *eb);
int clear_extent_buffer_dirty(struct extent_buffer *eb);
#endif
