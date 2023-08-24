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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include "kerncompat.h"
#include "kernel-shared/extent_io.h"
#include "kernel-lib/list.h"
#include "kernel-lib/raid56.h"
#include "kernel-lib/bitmap.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/messages.h"
#include "common/utils.h"
#include "common/device-utils.h"
#include "common/internal.h"

static void free_extent_buffer_final(struct extent_buffer *eb);

void extent_buffer_init_cache(struct btrfs_fs_info *fs_info)
{
	fs_info->max_cache_size = total_memory() / 4;
	fs_info->cache_size = 0;
	INIT_LIST_HEAD(&fs_info->lru);
}

void extent_buffer_free_cache(struct btrfs_fs_info *fs_info)
{
	struct extent_buffer *eb;

	while(!list_empty(&fs_info->lru)) {
		eb = list_entry(fs_info->lru.next, struct extent_buffer, lru);
		if (eb->refs) {
			/*
			 * Reset extent buffer refs to 1, so the
			 * free_extent_buffer_nocache() can free it for sure.
			 */
			eb->refs = 1;
			fprintf(stderr,
				"extent buffer leak: start %llu len %u\n",
				(unsigned long long)eb->start, eb->len);
			free_extent_buffer_nocache(eb);
		} else {
			free_extent_buffer_final(eb);
		}
	}

	free_extent_cache_tree(&fs_info->extent_cache);
	fs_info->cache_size = 0;
}

/*
 * extent_buffer_bitmap_set - set an area of a bitmap
 * @eb: the extent buffer
 * @start: offset of the bitmap item in the extent buffer
 * @pos: bit number of the first bit
 * @len: number of bits to set
 */
void extent_buffer_bitmap_set(struct extent_buffer *eb, unsigned long start,
                              unsigned long pos, unsigned long len)
{
	u8 *p = (u8 *)eb->data + start + BIT_BYTE(pos);
	const unsigned int size = pos + len;
	int bits_to_set = BITS_PER_BYTE - (pos % BITS_PER_BYTE);
	u8 mask_to_set = BITMAP_FIRST_BYTE_MASK(pos);

	while (len >= bits_to_set) {
		*p |= mask_to_set;
		len -= bits_to_set;
		bits_to_set = BITS_PER_BYTE;
		mask_to_set = ~0;
		p++;
	}
	if (len) {
		mask_to_set &= BITMAP_LAST_BYTE_MASK(size);
		*p |= mask_to_set;
	}
}

/*
 * extent_buffer_bitmap_clear - clear an area of a bitmap
 * @eb: the extent buffer
 * @start: offset of the bitmap item in the extent buffer
 * @pos: bit number of the first bit
 * @len: number of bits to clear
 */
void extent_buffer_bitmap_clear(struct extent_buffer *eb, unsigned long start,
                                unsigned long pos, unsigned long len)
{
	u8 *p = (u8 *)eb->data + start + BIT_BYTE(pos);
	const unsigned int size = pos + len;
	int bits_to_clear = BITS_PER_BYTE - (pos % BITS_PER_BYTE);
	u8 mask_to_clear = BITMAP_FIRST_BYTE_MASK(pos);

	while (len >= bits_to_clear) {
		*p &= ~mask_to_clear;
		len -= bits_to_clear;
		bits_to_clear = BITS_PER_BYTE;
		mask_to_clear = ~0;
		p++;
	}
	if (len) {
		mask_to_clear &= BITMAP_LAST_BYTE_MASK(size);
		*p &= ~mask_to_clear;
	}
}

static struct extent_buffer *__alloc_extent_buffer(struct btrfs_fs_info *info,
						   u64 bytenr, u32 blocksize)
{
	struct extent_buffer *eb;

	eb = calloc(1, sizeof(struct extent_buffer) + blocksize);
	if (!eb)
		return NULL;

	eb->start = bytenr;
	eb->len = blocksize;
	eb->refs = 1;
	eb->flags = 0;
	eb->cache_node.start = bytenr;
	eb->cache_node.size = blocksize;
	eb->fs_info = info;
	INIT_LIST_HEAD(&eb->recow);
	INIT_LIST_HEAD(&eb->lru);
	memset_extent_buffer(eb, 0, 0, blocksize);

	return eb;
}

struct extent_buffer *btrfs_clone_extent_buffer(struct extent_buffer *src)
{
	struct extent_buffer *new;

	new = __alloc_extent_buffer(src->fs_info, src->start, src->len);
	if (!new)
		return NULL;

	copy_extent_buffer_full(new, src);
	new->flags |= EXTENT_BUFFER_DUMMY;

	return new;
}

static void free_extent_buffer_final(struct extent_buffer *eb)
{
	BUG_ON(eb->refs);
	list_del_init(&eb->lru);
	if (!(eb->flags & EXTENT_BUFFER_DUMMY)) {
		remove_cache_extent(&eb->fs_info->extent_cache, &eb->cache_node);
		BUG_ON(eb->fs_info->cache_size < eb->len);
		eb->fs_info->cache_size -= eb->len;
	}
	free(eb);
}

static void free_extent_buffer_internal(struct extent_buffer *eb, bool free_now)
{
	if (!eb || IS_ERR(eb))
		return;

	eb->refs--;
	BUG_ON(eb->refs < 0);
	if (eb->refs == 0) {
		if (eb->flags & EXTENT_BUFFER_DIRTY) {
			warning(
			"dirty eb leak (aborted trans): start %llu len %u",
				eb->start, eb->len);
		}
		list_del_init(&eb->recow);
		if (eb->flags & EXTENT_BUFFER_DUMMY || free_now)
			free_extent_buffer_final(eb);
	}
}

void free_extent_buffer(struct extent_buffer *eb)
{
	free_extent_buffer_internal(eb, 0);
}

void free_extent_buffer_nocache(struct extent_buffer *eb)
{
	free_extent_buffer_internal(eb, 1);
}

void free_extent_buffer_stale(struct extent_buffer *eb)
{
	free_extent_buffer_internal(eb, 1);
}

struct extent_buffer *find_extent_buffer(struct btrfs_fs_info *fs_info,
					 u64 bytenr)
{
	struct extent_buffer *eb = NULL;
	struct cache_extent *cache;

	cache = lookup_cache_extent(&fs_info->extent_cache, bytenr,
				    fs_info->nodesize);
	if (cache && cache->start == bytenr &&
	    cache->size == fs_info->nodesize) {
		eb = container_of(cache, struct extent_buffer, cache_node);
		list_move_tail(&eb->lru, &fs_info->lru);
		eb->refs++;
	}
	return eb;
}

struct extent_buffer *find_first_extent_buffer(struct btrfs_fs_info *fs_info,
					       u64 start)
{
	struct extent_buffer *eb = NULL;
	struct cache_extent *cache;

	cache = search_cache_extent(&fs_info->extent_cache, start);
	if (cache) {
		eb = container_of(cache, struct extent_buffer, cache_node);
		list_move_tail(&eb->lru, &fs_info->lru);
		eb->refs++;
	}
	return eb;
}

static void trim_extent_buffer_cache(struct btrfs_fs_info *fs_info)
{
	struct extent_buffer *eb, *tmp;

	list_for_each_entry_safe(eb, tmp, &fs_info->lru, lru) {
		if (eb->refs == 0)
			free_extent_buffer_final(eb);
		if (fs_info->cache_size <= ((fs_info->max_cache_size * 9) / 10))
			break;
	}
}

struct extent_buffer *alloc_extent_buffer(struct btrfs_fs_info *fs_info,
					  u64 bytenr, u32 blocksize)
{
	struct extent_buffer *eb;
	struct cache_extent *cache;

	cache = lookup_cache_extent(&fs_info->extent_cache, bytenr, blocksize);
	if (cache && cache->start == bytenr &&
	    cache->size == blocksize) {
		eb = container_of(cache, struct extent_buffer, cache_node);
		list_move_tail(&eb->lru, &fs_info->lru);
		eb->refs++;
	} else {
		int ret;

		if (cache) {
			eb = container_of(cache, struct extent_buffer,
					  cache_node);
			free_extent_buffer(eb);
		}
		eb = __alloc_extent_buffer(fs_info, bytenr, blocksize);
		if (!eb)
			return NULL;
		ret = insert_cache_extent(&fs_info->extent_cache, &eb->cache_node);
		if (ret) {
			free(eb);
			return NULL;
		}
		list_add_tail(&eb->lru, &fs_info->lru);
		fs_info->cache_size += blocksize;
		if (fs_info->cache_size >= fs_info->max_cache_size)
			trim_extent_buffer_cache(fs_info);
	}
	return eb;
}

/*
 * Allocate a dummy extent buffer which won't be inserted into extent buffer
 * cache.
 *
 * This mostly allows super block read write using existing eb infrastructure
 * without pulluting the eb cache.
 *
 * This is especially important to avoid injecting eb->start == SZ_64K, as
 * fuzzed image could have invalid tree bytenr covers super block range,
 * and cause ref count underflow.
 */
struct extent_buffer *alloc_dummy_extent_buffer(struct btrfs_fs_info *fs_info,
						u64 bytenr, u32 blocksize)
{
	struct extent_buffer *ret;

	ret = __alloc_extent_buffer(fs_info, bytenr, blocksize);
	if (!ret)
		return NULL;

	ret->flags |= EXTENT_BUFFER_DUMMY;

	return ret;
}

static int read_raid56(struct btrfs_fs_info *fs_info, void *buf, u64 logical,
		       u64 len, int mirror, struct btrfs_multi_bio *multi,
		       u64 *raid_map)
{
	const int tolerance = (multi->type & BTRFS_RAID_RAID6 ? 2 : 1);
	const int num_stripes = multi->num_stripes;
	const u64 full_stripe_start = raid_map[0];
	void **pointers = NULL;
	unsigned long *failed_stripe_bitmap = NULL;
	int failed_a = -1;
	int failed_b = -1;
	int i;
	int ret;

	/* Only read repair should go this path */
	ASSERT(mirror > 1);
	ASSERT(raid_map);

	/* The read length should be inside one stripe */
	ASSERT(len <= BTRFS_STRIPE_LEN);

	pointers = calloc(num_stripes, sizeof(void *));
	if (!pointers) {
		ret = -ENOMEM;
		goto out;
	}
	/* Allocate memory for the full stripe */
	for (i = 0; i < num_stripes; i++) {
		pointers[i] = malloc(BTRFS_STRIPE_LEN);
		if (!pointers[i]) {
			ret = -ENOMEM;
			goto out;
		}
	}

	failed_stripe_bitmap = bitmap_zalloc(num_stripes);
	if (!failed_stripe_bitmap) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Read the full stripe.
	 *
	 * The stripes in @multi is not rotated, thus can be used to read from
	 * disk directly.
	 */
	for (i = 0; i < num_stripes; i++) {
		ret = btrfs_pread(multi->stripes[i].dev->fd, pointers[i],
				  BTRFS_STRIPE_LEN, multi->stripes[i].physical,
				  fs_info->zoned);
		if (ret < BTRFS_STRIPE_LEN)
			set_bit(i, failed_stripe_bitmap);
	}

	/*
	 * Get the failed index.
	 *
	 * Since we're reading using mirror_num > 1 already, it means the data
	 * stripe where @logical lies in is definitely corrupted.
	 */
	set_bit((logical - full_stripe_start) / BTRFS_STRIPE_LEN, failed_stripe_bitmap);

	/*
	 * For RAID6, we don't have good way to exhaust all the combinations,
	 * so here we can only go through the map to see if we have missing devices.
	 *
	 * If we only have one failed stripe (marked by above set_bit()), then
	 * we have no better idea, fallback to use P corruption.
	 */
	if (multi->type & BTRFS_BLOCK_GROUP_RAID6 &&
	    bitmap_weight(failed_stripe_bitmap, num_stripes) < 2)
		set_bit(num_stripes - 2, failed_stripe_bitmap);

	/* Damaged beyond repair already. */
	if (bitmap_weight(failed_stripe_bitmap, num_stripes) > tolerance) {
		ret = -EIO;
		goto out;
	}

	for_each_set_bit(i, failed_stripe_bitmap, num_stripes) {
		if (failed_a < 0)
			failed_a = i;
		else if (failed_b < 0)
			failed_b = i;
	}

	/* Rebuild the full stripe */
	ret = raid56_recov(num_stripes, BTRFS_STRIPE_LEN, multi->type,
			   failed_a, failed_b, pointers);
	ASSERT(ret == 0);

	/* Now copy the data back to original buf */
	memcpy(buf, pointers[failed_a] + (logical - full_stripe_start) %
			BTRFS_STRIPE_LEN, len);
	ret = 0;
out:
	free(failed_stripe_bitmap);
	for (i = 0; i < num_stripes; i++)
		free(pointers[i]);
	free(pointers);
	return ret;
}

int read_data_from_disk(struct btrfs_fs_info *info, void *buf, u64 logical,
			u64 *len, int mirror)
{
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	u64 read_len = *len;
	u64 *raid_map = NULL;
	int ret;

	ret = btrfs_map_block(info, READ, logical, &read_len, &multi, mirror,
			      &raid_map);
	if (ret) {
		fprintf(stderr, "Couldn't map the block %llu\n", logical);
		return -EIO;
	}
	read_len = min(*len, read_len);

	/* We need to rebuild from P/Q */
	if (mirror > 1 && multi->type & BTRFS_BLOCK_GROUP_RAID56_MASK) {
		ret = read_raid56(info, buf, logical, read_len, mirror, multi,
				  raid_map);
		free(multi);
		free(raid_map);
		*len = read_len;
		return ret;
	}
	free(raid_map);
	device = multi->stripes[0].dev;

	if (device->fd <= 0) {
		kfree(multi);
		return -EIO;
	}

	ret = btrfs_pread(device->fd, buf, read_len,
			  multi->stripes[0].physical, info->zoned);
	kfree(multi);
	if (ret < 0) {
		fprintf(stderr, "Error reading %llu, %d\n", logical,
			ret);
		return ret;
	}
	if (ret != read_len) {
		fprintf(stderr,
			"Short read for %llu, read %d, read_len %llu\n",
			logical, ret, read_len);
		return -EIO;
	}
	*len = read_len;

	return 0;
}

/*
 * Write the data in @buf to logical bytenr @offset.
 *
 * Such data will be written to all mirrors and RAID56 P/Q will also be
 * properly handled.
 */
int write_data_to_disk(struct btrfs_fs_info *info, const void *buf, u64 offset,
		       u64 bytes)
{
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	u64 bytes_left = bytes;
	u64 this_len;
	u64 total_write = 0;
	u64 *raid_map = NULL;
	u64 dev_bytenr;
	int dev_nr;
	int ret = 0;

	while (bytes_left > 0) {
		this_len = bytes_left;
		dev_nr = 0;

		ret = btrfs_map_block(info, WRITE, offset, &this_len, &multi,
				      0, &raid_map);
		if (ret) {
			fprintf(stderr, "Couldn't map the block %llu\n",
				offset);
			return -EIO;
		}

		if (raid_map) {
			struct extent_buffer *eb;
			u64 stripe_len = this_len;

			this_len = min(this_len, bytes_left);
			this_len = min(this_len, (u64)info->nodesize);

			eb = malloc(sizeof(struct extent_buffer) + this_len);
			if (!eb) {
				error_msg(ERROR_MSG_MEMORY, "extent buffer");
				ret = -ENOMEM;
				goto out;
			}

			memset(eb, 0, sizeof(struct extent_buffer) + this_len);
			eb->start = offset;
			eb->len = this_len;

			memcpy(eb->data, buf + total_write, this_len);
			ret = write_raid56_with_parity(info, eb, multi,
						       stripe_len, raid_map);
			BUG_ON(ret < 0);

			free(eb);
			kfree(raid_map);
			raid_map = NULL;
		} else while (dev_nr < multi->num_stripes) {
			device = multi->stripes[dev_nr].dev;
			if (device->fd <= 0) {
				kfree(multi);
				return -EIO;
			}

			dev_bytenr = multi->stripes[dev_nr].physical;
			this_len = min(this_len, bytes_left);
			dev_nr++;
			device->total_ios++;

			ret = btrfs_pwrite(device->fd, buf + total_write,
					   this_len, dev_bytenr, info->zoned);
			if (ret != this_len) {
				if (ret < 0) {
					fprintf(stderr, "Error writing to "
						"device %d\n", errno);
					ret = -errno;
					kfree(multi);
					return ret;
				} else {
					fprintf(stderr, "Short write\n");
					kfree(multi);
					return -EIO;
				}
			}
		}

		BUG_ON(bytes_left < this_len);

		bytes_left -= this_len;
		offset += this_len;
		total_write += this_len;

		kfree(multi);
		multi = NULL;
	}
	return 0;

out:
	kfree(raid_map);
	return ret;
}

int set_extent_buffer_dirty(struct extent_buffer *eb)
{
	struct extent_io_tree *tree = &eb->fs_info->dirty_buffers;
	if (!(eb->flags & EXTENT_BUFFER_DIRTY)) {
		eb->flags |= EXTENT_BUFFER_DIRTY;
		set_extent_dirty(tree, eb->start, eb->start + eb->len - 1,
				 GFP_NOFS);
		extent_buffer_get(eb);
	}
	return 0;
}

int btrfs_clear_buffer_dirty(struct btrfs_trans_handle *trans,
			     struct extent_buffer *eb)
{
	struct extent_io_tree *tree = &eb->fs_info->dirty_buffers;
	if (eb->flags & EXTENT_BUFFER_DIRTY) {
		eb->flags &= ~EXTENT_BUFFER_DIRTY;
		clear_extent_dirty(tree, eb->start, eb->start + eb->len - 1,
				   NULL);
		free_extent_buffer(eb);
	}
	return 0;
}

int memcmp_extent_buffer(const struct extent_buffer *eb, const void *ptrv,
			 unsigned long start, unsigned long len)
{
	return memcmp(eb->data + start, ptrv, len);
}

void read_extent_buffer(const struct extent_buffer *eb, void *dst,
			unsigned long start, unsigned long len)
{
	memcpy(dst, eb->data + start, len);
}

void write_extent_buffer_fsid(const struct extent_buffer *eb, const void *src)
{
	write_extent_buffer(eb, src, btrfs_header_fsid(), BTRFS_FSID_SIZE);
}

void write_extent_buffer_chunk_tree_uuid(const struct extent_buffer *eb,
		const void *src)
{
	write_extent_buffer(eb, src, btrfs_header_chunk_tree_uuid(eb), BTRFS_FSID_SIZE);
}

void write_extent_buffer(const struct extent_buffer *eb, const void *src,
			 unsigned long start, unsigned long len)
{
	memcpy((void *)eb->data + start, src, len);
}

void copy_extent_buffer_full(const struct extent_buffer *dst,
			     const struct extent_buffer *src)
{
	copy_extent_buffer(dst, src, 0, 0, src->len);
}

void copy_extent_buffer(const struct extent_buffer *dst,
			const struct extent_buffer *src,
			unsigned long dst_offset, unsigned long src_offset,
			unsigned long len)
{
	memcpy((void *)dst->data + dst_offset, src->data + src_offset, len);
}

void memmove_extent_buffer(const struct extent_buffer *dst, unsigned long dst_offset,
			   unsigned long src_offset, unsigned long len)
{
	memmove((void *)dst->data + dst_offset, dst->data + src_offset, len);
}

void memset_extent_buffer(const struct extent_buffer *eb, char c,
			  unsigned long start, unsigned long len)
{
	memset((void *)eb->data + start, c, len);
}

int extent_buffer_test_bit(const struct extent_buffer *eb, unsigned long start,
			   unsigned long nr)
{
	return le_test_bit(nr, (u8 *)eb->data + start);
}

/*
 * btrfs_readahead_node_child - readahead a node's child block
 * @node:	parent node we're reading from
 * @slot:	slot in the parent node for the child we want to read
 *
 * A helper for readahead_tree_block, we simply read the bytenr pointed at the
 * slot in the node provided.
 */
void btrfs_readahead_node_child(struct extent_buffer *node, int slot)
{
	readahead_tree_block(node->fs_info, btrfs_node_blockptr(node, slot),
			     btrfs_node_ptr_generation(node, slot));
}
