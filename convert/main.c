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

/*
 * Btrfs convert design:
 *
 * The overall design of btrfs convert is like the following:
 *
 * |<------------------Old fs----------------------------->|
 * |<- used ->| |<- used ->|                    |<- used ->|
 *                           ||
 *                           \/
 * |<---------------Btrfs fs------------------------------>|
 * |<-   Old data chunk  ->|< new chunk (D/M/S)>|<- ODC  ->|
 * |<-Old-FE->| |<-Old-FE->|<- Btrfs extents  ->|<-Old-FE->|
 *
 * ODC    = Old data chunk, btrfs chunks containing old fs data
 *          Mapped 1:1 (logical address == device offset)
 * Old-FE = file extents pointing to old fs.
 *
 * So old fs used space is (mostly) kept as is, while btrfs will insert
 * its chunk (Data/Meta/Sys) into large enough free space.
 * In this way, we can create different profiles for metadata/data for
 * converted fs.
 *
 * We must reserve and relocate 3 ranges for btrfs:
 * * [0, 1M)                    - area never used for any data except the first
 *                                superblock
 * * [btrfs_sb_offset(1), +64K) - 1st superblock backup copy
 * * [btrfs_sb_offset(2), +64K) - 2nd, dtto
 *
 * Most work is spent handling corner cases around these reserved ranges.
 *
 * Detailed workflow is:
 * 1)   Scan old fs used space and calculate data chunk layout
 * 1.1) Scan old fs
 *      We can a map used space of old fs
 *
 * 1.2) Calculate data chunk layout - this is the hard part
 *      New data chunks must meet 3 conditions using result from 1.1
 *      a. Large enough to be a chunk
 *      b. Doesn't intersect reserved ranges
 *      c. Covers all the remaining old fs used space
 *
 *      NOTE: This can be simplified if we don't need to handle backup supers
 *
 * 1.3) Calculate usable space for new btrfs chunks
 *      Btrfs chunk usable space must meet 3 conditions using result from 1.2
 *      a. Large enough to be a chunk
 *      b. Doesn't intersect reserved ranges
 *      c. Doesn't cover any data chunks in 1.1
 *
 * 2)   Create basic btrfs filesystem structure
 *      Initial metadata and sys chunks are inserted in the first available
 *      space found in step 1.3
 *      Then insert all data chunks into the basic btrfs
 *
 * 3)   Create convert image
 *      We need to relocate reserved ranges here.
 *      After this step, the convert image is done, and we can use the image
 *      as reflink source to create old files
 *
 * 4)   Iterate old fs to create files
 *      We just reflink file extents from old fs to newly created files on
 *      btrfs.
 */

#include "kerncompat.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <uuid/uuid.h>

#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/transaction.h"
#include "common/utils.h"
#include "common/task-utils.h"
#include "common/path-utils.h"
#include "common/help.h"
#include "common/parse-utils.h"
#include "mkfs/common.h"
#include "convert/common.h"
#include "convert/source-fs.h"
#include "crypto/crc32c.h"
#include "common/fsfeatures.h"
#include "common/device-scan.h"
#include "common/box.h"
#include "common/open-utils.h"
#include "common/repair.h"

extern const struct btrfs_convert_operations ext2_convert_ops;
extern const struct btrfs_convert_operations reiserfs_convert_ops;

static const struct btrfs_convert_operations *convert_operations[] = {
#if BTRFSCONVERT_EXT2
	&ext2_convert_ops,
#endif
#if BTRFSCONVERT_REISERFS
	&reiserfs_convert_ops,
#endif
};

static void *print_copied_inodes(void *p)
{
	struct task_ctx *priv = p;
	const char work_indicator[] = { '.', 'o', 'O', 'o' };
	u64 count = 0;

	task_period_start(priv->info, 1000 /* 1s */);
	while (1) {
		count++;
		pthread_mutex_lock(&priv->mutex);
		printf("Copy inodes [%c] [%10llu/%10llu]\r",
		       work_indicator[count % 4],
		       (unsigned long long)priv->cur_copy_inodes,
		       (unsigned long long)priv->max_copy_inodes);
		pthread_mutex_unlock(&priv->mutex);
		fflush(stdout);
		task_period_wait(priv->info);
	}

	return NULL;
}

static int after_copied_inodes(void *p)
{
	printf("\n");
	fflush(stdout);

	return 0;
}

static inline int copy_inodes(struct btrfs_convert_context *cctx,
			      struct btrfs_root *root, u32 convert_flags,
			      struct task_ctx *p)
{
	return cctx->convert_ops->copy_inodes(cctx, root, convert_flags, p);
}

static inline void convert_close_fs(struct btrfs_convert_context *cctx)
{
	cctx->convert_ops->close_fs(cctx);
}

static inline int convert_check_state(struct btrfs_convert_context *cctx)
{
	return cctx->convert_ops->check_state(cctx);
}

static int csum_disk_extent(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    u64 disk_bytenr, u64 num_bytes)
{
	u32 blocksize = root->fs_info->sectorsize;
	u64 offset;
	char *buffer;
	int ret = 0;

	buffer = malloc(blocksize);
	if (!buffer)
		return -ENOMEM;
	for (offset = 0; offset < num_bytes; offset += blocksize) {
		ret = read_disk_extent(root, disk_bytenr + offset,
					blocksize, buffer);
		if (ret)
			break;
		ret = btrfs_csum_file_block(trans,
					    disk_bytenr + num_bytes,
					    disk_bytenr + offset,
					    buffer, blocksize);
		if (ret)
			break;
	}
	free(buffer);
	return ret;
}

static int create_image_file_range(struct btrfs_trans_handle *trans,
				      struct btrfs_root *root,
				      struct cache_tree *used,
				      struct btrfs_inode_item *inode,
				      u64 ino, u64 bytenr, u64 *ret_len,
				      u32 convert_flags)
{
	struct cache_extent *cache;
	struct btrfs_block_group *bg_cache;
	u64 len = *ret_len;
	u64 disk_bytenr;
	int i;
	int ret;
	u32 datacsum = convert_flags & CONVERT_FLAG_DATACSUM;

	if (bytenr != round_down(bytenr, root->fs_info->sectorsize)) {
		error("bytenr not sectorsize aligned: %llu",
				(unsigned long long)bytenr);
		return -EINVAL;
	}
	if (len != round_down(len, root->fs_info->sectorsize)) {
		error("length not sectorsize aligned: %llu",
				(unsigned long long)len);
		return -EINVAL;
	}
	len = min_t(u64, len, BTRFS_MAX_EXTENT_SIZE);

	/*
	 * Skip reserved ranges first
	 *
	 * Or we will insert a hole into current image file, and later
	 * migrate block will fail as there is already a file extent.
	 */
	for (i = 0; i < ARRAY_SIZE(btrfs_reserved_ranges); i++) {
		const struct simple_range *reserved = &btrfs_reserved_ranges[i];

		/*
		 * |-- reserved --|
		 *         |--range---|
		 * or
		 * |---- reserved ----|
		 *    |-- range --|
		 * Skip to reserved range end
		 */
		if (bytenr >= reserved->start && bytenr < range_end(reserved)) {
			*ret_len = range_end(reserved) - bytenr;
			return 0;
		}

		/*
		 *      |---reserved---|
		 * |----range-------|
		 * Leading part may still create a file extent
		 */
		if (bytenr < reserved->start &&
		    bytenr + len >= range_end(reserved)) {
			len = min_t(u64, len, reserved->start - bytenr);
			break;
		}
	}

	/* Check if we are going to insert regular file extent, or hole */
	cache = search_cache_extent(used, bytenr);
	if (cache) {
		if (cache->start <= bytenr) {
			/*
			 * |///////Used///////|
			 *	|<--insert--->|
			 *	bytenr
			 * Insert one real file extent
			 */
			len = min_t(u64, len, cache->start + cache->size -
				    bytenr);
			disk_bytenr = bytenr;
		} else {
			/*
			 *		|//Used//|
			 *  |<-insert-->|
			 *  bytenr
			 *  Insert one hole
			 */
			len = min(len, cache->start - bytenr);
			disk_bytenr = 0;
			datacsum = 0;
		}
	} else {
		/*
		 * |//Used//|		|EOF
		 *	    |<-insert-->|
		 *	    bytenr
		 * Insert one hole
		 */
		disk_bytenr = 0;
		datacsum = 0;
	}

	if (disk_bytenr) {
		/* Check if the range is in a data block group */
		bg_cache = btrfs_lookup_block_group(root->fs_info, bytenr);
		if (!bg_cache) {
			error("missing data block for bytenr %llu", bytenr);
			return -ENOENT;
		}
		if (!(bg_cache->flags & BTRFS_BLOCK_GROUP_DATA)) {
			error(
	"data bytenr %llu is covered by non-data block group %llu flags 0x%llu",
			      bytenr, bg_cache->start, bg_cache->flags);
			return -EINVAL;
		}

		/* The extent should never cross block group boundary */
		len = min_t(u64, len, bg_cache->start + bg_cache->length -
				bytenr);
	}

	if (len != round_down(len, root->fs_info->sectorsize)) {
		error("remaining length not sectorsize aligned: %llu",
				(unsigned long long)len);
		return -EINVAL;
	}
	ret = btrfs_record_file_extent(trans, root, ino, inode, bytenr,
				       disk_bytenr, len);
	if (ret < 0)
		return ret;

	if (datacsum) {
		ret = csum_disk_extent(trans, root, bytenr, len);
		if (ret < 0) {
			errno = -ret;
			error(
		"failed to calculate csum for bytenr %llu len %llu: %m",
			      bytenr, len);
		}
	}
	*ret_len = len;
	return ret;
}

/*
 * Relocate old fs data in one reserved ranges
 *
 * Since all old fs data in reserved range is not covered by any chunk nor
 * data extent, we don't need to handle any reference but add new
 * extent/reference, which makes codes more clear
 */
static int migrate_one_reserved_range(struct btrfs_trans_handle *trans,
				      struct btrfs_root *root,
				      struct cache_tree *used,
				      struct btrfs_inode_item *inode, int fd,
				      u64 ino, const struct simple_range *range,
				      u32 convert_flags)
{
	u64 cur_off = range->start;
	u64 cur_len = range->len;
	u64 hole_start = range->start;
	u64 hole_len;
	struct cache_extent *cache;
	struct btrfs_key key;
	struct extent_buffer *eb;
	int ret = 0;

	/*
	 * It's possible that there are holes in reserved range:
	 * |<---------------- Reserved range ---------------------->|
	 *      |<- Old fs data ->|   |<- Old fs data ->|
	 * So here we need to iterate through old fs used space and only
	 * migrate ranges that covered by old fs data.
	 */
	while (cur_off < range_end(range)) {
		cache = search_cache_extent(used, cur_off);
		if (!cache)
			break;
		cur_off = max(cache->start, cur_off);
		if (cur_off >= range_end(range))
			break;
		cur_len = min(cache->start + cache->size, range_end(range)) -
			  cur_off;
		BUG_ON(cur_len < root->fs_info->sectorsize);

		/* reserve extent for the data */
		ret = btrfs_reserve_extent(trans, root, cur_len, 0, 0, (u64)-1,
					   &key, 1);
		if (ret < 0)
			break;

		eb = malloc(sizeof(*eb) + cur_len);
		if (!eb) {
			ret = -ENOMEM;
			break;
		}

		ret = pread(fd, eb->data, cur_len, cur_off);
		if (ret < cur_len) {
			ret = (ret < 0 ? ret : -EIO);
			free(eb);
			break;
		}
		eb->start = key.objectid;
		eb->len = key.offset;
		eb->fs_info = root->fs_info;

		/* Write the data */
		ret = write_and_map_eb(root->fs_info, eb);
		free(eb);
		if (ret < 0)
			break;

		/* Now handle extent item and file extent things */
		ret = btrfs_record_file_extent(trans, root, ino, inode, cur_off,
					       key.objectid, key.offset);
		if (ret < 0)
			break;
		/* Finally, insert csum items */
		if (convert_flags & CONVERT_FLAG_DATACSUM)
			ret = csum_disk_extent(trans, root, key.objectid,
					       key.offset);

		/* Don't forget to insert hole */
		hole_len = cur_off - hole_start;
		if (hole_len) {
			ret = btrfs_record_file_extent(trans, root, ino, inode,
					hole_start, 0, hole_len);
			if (ret < 0)
				break;
		}

		cur_off += key.offset;
		hole_start = cur_off;
		cur_len = range_end(range) - cur_off;
	}
	/*
	 * Last hole
	 * |<---- reserved -------->|
	 * |<- Old fs data ->|      |
	 *                   | Hole |
	 */
	if (range_end(range) - hole_start > 0)
		ret = btrfs_record_file_extent(trans, root, ino, inode,
				hole_start, 0, range_end(range) - hole_start);
	return ret;
}

/*
 * Relocate the used source fs data in reserved ranges
 */
static int migrate_reserved_ranges(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct cache_tree *used,
				   struct btrfs_inode_item *inode, int fd,
				   u64 ino, u64 total_bytes, u32 convert_flags)
{
	int i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(btrfs_reserved_ranges); i++) {
		const struct simple_range *range = &btrfs_reserved_ranges[i];

		if (range->start > total_bytes)
			return ret;
		ret = migrate_one_reserved_range(trans, root, used, inode, fd,
						 ino, range, convert_flags);
		if (ret < 0)
			return ret;
	}

	return ret;
}

/*
 * Helper for expand and merge extent_cache for wipe_one_reserved_range() to
 * handle wiping a range that exists in cache.
 */
static int _expand_extent_cache(struct cache_tree *tree,
				struct cache_extent *entry,
				u64 min_stripe_size, int backward)
{
	struct cache_extent *ce;
	int diff;

	if (entry->size >= min_stripe_size)
		return 0;
	diff = min_stripe_size - entry->size;

	if (backward) {
		ce = prev_cache_extent(entry);
		if (!ce)
			goto expand_back;
		if (ce->start + ce->size >= entry->start - diff) {
			/* Directly merge with previous extent */
			ce->size = entry->start + entry->size - ce->start;
			remove_cache_extent(tree, entry);
			free(entry);
			return 0;
		}
expand_back:
		/* No overlap, normal extent */
		if (entry->start < diff) {
			error("cannot find space for data chunk layout");
			return -ENOSPC;
		}
		entry->start -= diff;
		entry->size += diff;
		return 0;
	}
	ce = next_cache_extent(entry);
	if (!ce)
		goto expand_after;
	if (entry->start + entry->size + diff >= ce->start) {
		/* Directly merge with next extent */
		entry->size = ce->start + ce->size - entry->start;
		remove_cache_extent(tree, ce);
		free(ce);
		return 0;
	}
expand_after:
	entry->size += diff;
	return 0;
}

/*
 * Remove one reserve range from given cache tree
 * if min_stripe_size is non-zero, it will ensure for split case,
 * all its split cache extent is no smaller than @min_strip_size / 2.
 */
static int wipe_one_reserved_range(struct cache_tree *tree,
				   u64 start, u64 len, u64 min_stripe_size,
				   int ensure_size)
{
	struct cache_extent *cache;
	int ret;

	BUG_ON(ensure_size && min_stripe_size == 0);
	/*
	 * The logical here is simplified to handle special cases only
	 * So we don't need to consider merge case for ensure_size
	 */
	BUG_ON(min_stripe_size && (min_stripe_size < len * 2 ||
	       min_stripe_size / 2 < BTRFS_STRIPE_LEN));

	/* Also, wipe range should already be aligned */
	BUG_ON(start != round_down(start, BTRFS_STRIPE_LEN) ||
	       start + len != round_up(start + len, BTRFS_STRIPE_LEN));

	min_stripe_size /= 2;

	cache = lookup_cache_extent(tree, start, len);
	if (!cache)
		return 0;

	if (start <= cache->start) {
		/*
		 *	|--------cache---------|
		 * |-wipe-|
		 */
		BUG_ON(start + len <= cache->start);

		/*
		 * The wipe size is smaller than min_stripe_size / 2,
		 * so the result length should still meet min_stripe_size
		 * And no need to do alignment
		 */
		cache->size -= (start + len - cache->start);
		if (cache->size == 0) {
			remove_cache_extent(tree, cache);
			free(cache);
			return 0;
		}

		BUG_ON(ensure_size && cache->size < min_stripe_size);

		cache->start = start + len;
		return 0;
	} else if (start > cache->start && start + len < cache->start +
		   cache->size) {
		/*
		 * |-------cache-----|
		 *	|-wipe-|
		 */
		u64 old_start = cache->start;
		u64 old_len = cache->size;
		u64 insert_start = start + len;
		u64 insert_len;

		cache->size = start - cache->start;
		/* Expand the leading half part if needed */
		if (ensure_size && cache->size < min_stripe_size) {
			ret = _expand_extent_cache(tree, cache,
					min_stripe_size, 1);
			if (ret < 0)
				return ret;
		}

		/* And insert the new one */
		insert_len = old_start + old_len - start - len;
		ret = add_merge_cache_extent(tree, insert_start, insert_len);
		if (ret < 0)
			return ret;

		/* Expand the last half part if needed */
		if (ensure_size && insert_len < min_stripe_size) {
			cache = lookup_cache_extent(tree, insert_start,
						    insert_len);
			if (!cache || cache->start != insert_start ||
			    cache->size != insert_len)
				return -ENOENT;
			ret = _expand_extent_cache(tree, cache,
					min_stripe_size, 0);
		}

		return ret;
	}
	/*
	 * |----cache-----|
	 *		|--wipe-|
	 * Wipe len should be small enough and no need to expand the
	 * remaining extent
	 */
	cache->size = start - cache->start;
	BUG_ON(ensure_size && cache->size < min_stripe_size);
	return 0;
}

/*
 * Remove reserved ranges from given cache_tree
 *
 * It will remove the following ranges
 * 1) 0~1M
 * 2) 2nd superblock, +64K (make sure chunks are 64K aligned)
 * 3) 3rd superblock, +64K
 *
 * @min_stripe must be given for safety check
 * and if @ensure_size is given, it will ensure affected cache_extent will be
 * larger than min_stripe_size
 */
static int wipe_reserved_ranges(struct cache_tree *tree, u64 min_stripe_size,
				int ensure_size)
{
	int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(btrfs_reserved_ranges); i++) {
		const struct simple_range *range = &btrfs_reserved_ranges[i];

		ret = wipe_one_reserved_range(tree, range->start, range->len,
					      min_stripe_size, ensure_size);
		if (ret < 0)
			return ret;
	}
	return ret;
}

static int calculate_available_space(struct btrfs_convert_context *cctx)
{
	struct cache_tree *used = &cctx->used_space;
	struct cache_tree *data_chunks = &cctx->data_chunks;
	struct cache_tree *free = &cctx->free_space;
	struct cache_extent *cache;
	u64 cur_off = 0;
	/*
	 * Twice the minimal chunk size, to allow later wipe_reserved_ranges()
	 * works without need to consider overlap
	 */
	u64 min_stripe_size = SZ_32M;
	int ret;

	/* Calculate data_chunks */
	for (cache = first_cache_extent(used); cache;
	     cache = next_cache_extent(cache)) {
		u64 cur_len;

		if (cache->start + cache->size < cur_off)
			continue;
		if (cache->start > cur_off + min_stripe_size)
			cur_off = cache->start;
		cur_len = max(cache->start + cache->size - cur_off,
			      min_stripe_size);
		/* data chunks should never exceed device boundary */
		cur_len = min(cctx->total_bytes - cur_off, cur_len);
		ret = add_merge_cache_extent(data_chunks, cur_off, cur_len);
		if (ret < 0)
			goto out;
		cur_off += cur_len;
	}
	/*
	 * remove reserved ranges, so we won't ever bother relocating an old
	 * filesystem extent to other place.
	 */
	ret = wipe_reserved_ranges(data_chunks, min_stripe_size, 1);
	if (ret < 0)
		goto out;

	cur_off = 0;
	/*
	 * Calculate free space
	 * Always round up the start bytenr, to avoid metadata extent cross
	 * stripe boundary, as later mkfs_convert() won't have all the extent
	 * allocation check
	 */
	for (cache = first_cache_extent(data_chunks); cache;
	     cache = next_cache_extent(cache)) {
		if (cache->start < cur_off)
			continue;
		if (cache->start > cur_off) {
			u64 insert_start;
			u64 len;

			len = cache->start - round_up(cur_off,
						      BTRFS_STRIPE_LEN);
			insert_start = round_up(cur_off, BTRFS_STRIPE_LEN);

			ret = add_merge_cache_extent(free, insert_start, len);
			if (ret < 0)
				goto out;
		}
		cur_off = cache->start + cache->size;
	}
	/* Don't forget the last range */
	if (cctx->total_bytes > cur_off) {
		u64 len = cctx->total_bytes - cur_off;
		u64 insert_start;

		insert_start = round_up(cur_off, BTRFS_STRIPE_LEN);

		ret = add_merge_cache_extent(free, insert_start, len);
		if (ret < 0)
			goto out;
	}

	/* Remove reserved bytes */
	ret = wipe_reserved_ranges(free, min_stripe_size, 0);
out:
	return ret;
}

static int copy_free_space_tree(struct btrfs_convert_context *cctx)
{
	struct cache_tree *src = &cctx->free_space;
	struct cache_tree *dst = &cctx->free_space_initial;
	struct cache_extent *cache;
	int ret = 0;

	for (cache = search_cache_extent(src, 0);
	     cache;
	     cache = next_cache_extent(cache)) {
		ret = add_merge_cache_extent(dst, cache->start, cache->size);
		if (ret < 0)
			return ret;
		cctx->free_bytes_initial += cache->size;
	}
	return ret;
}

/*
 * Read used space, and since we have the used space,
 * calculate data_chunks and free for later mkfs
 */
static int convert_read_used_space(struct btrfs_convert_context *cctx)
{
	int ret;

	ret = cctx->convert_ops->read_used_space(cctx);
	if (ret)
		return ret;

	ret = calculate_available_space(cctx);
	if (ret < 0)
		return ret;

	return copy_free_space_tree(cctx);
}

/*
 * Create the fs image file of old filesystem.
 *
 * This is completely fs independent as we have cctx->used, only
 * need to create file extents pointing to all the positions.
 */
static int create_image(struct btrfs_root *root,
			   struct btrfs_mkfs_config *cfg,
			   struct btrfs_convert_context *cctx, int fd,
			   u64 size, char *name, u32 convert_flags)
{
	struct btrfs_inode_item buf;
	struct btrfs_trans_handle *trans;
	struct btrfs_path path;
	struct btrfs_key key;
	struct cache_extent *cache;
	struct cache_tree used_tmp;
	u64 cur;
	u64 ino;
	u64 flags = BTRFS_INODE_READONLY;
	int ret;

	if (!(convert_flags & CONVERT_FLAG_DATACSUM))
		flags |= BTRFS_INODE_NODATASUM;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	cache_tree_init(&used_tmp);
	btrfs_init_path(&path);

	ret = btrfs_find_free_objectid(trans, root, BTRFS_FIRST_FREE_OBJECTID,
				       &ino);
	if (ret < 0) {
		errno = -ret;
		error("failed to find free objectid for root %llu: %m",
			root->root_key.objectid);
		goto out;
	}
	ret = btrfs_new_inode(trans, root, ino, 0400 | S_IFREG);
	if (ret < 0) {
		errno = -ret;
		error("failed to create new inode for root %llu: %m",
			root->root_key.objectid);
		goto out;
	}
	ret = btrfs_change_inode_flags(trans, root, ino, flags);
	if (ret < 0) {
		errno = -ret;
		error("failed to change inode flag for ino %llu root %llu: %m",
			ino, root->root_key.objectid);
		goto out;
	}
	ret = btrfs_add_link(trans, root, ino, BTRFS_FIRST_FREE_OBJECTID, name,
			     strlen(name), BTRFS_FT_REG_FILE, NULL, 1, 0);
	if (ret < 0) {
		errno = -ret;
		error("failed to link ino %llu to '/%s' in root %llu: %m",
			ino, name, root->root_key.objectid);
		goto out;
	}

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(trans, root, &key, &path, 0, 1);
	if (ret) {
		ret = (ret > 0 ? -ENOENT : ret);
		goto out;
	}
	read_extent_buffer(path.nodes[0], &buf,
			btrfs_item_ptr_offset(path.nodes[0], path.slots[0]),
			sizeof(buf));
	btrfs_release_path(&path);

	/*
	 * Create a new used space cache, which doesn't contain the reserved
	 * range
	 */
	for (cache = first_cache_extent(&cctx->used_space); cache;
	     cache = next_cache_extent(cache)) {
		ret = add_cache_extent(&used_tmp, cache->start, cache->size);
		if (ret < 0)
			goto out;
	}
	ret = wipe_reserved_ranges(&used_tmp, 0, 0);
	if (ret < 0)
		goto out;

	/*
	 * Start from 1M, as 0~1M is reserved, and create_image_file_range()
	 * can't handle bytenr 0(will consider it as a hole)
	 */
	cur = SZ_1M;
	while (cur < size) {
		u64 len = size - cur;

		ret = create_image_file_range(trans, root, &used_tmp,
						&buf, ino, cur, &len,
						convert_flags);
		if (ret < 0)
			goto out;
		cur += len;
	}
	/* Handle the reserved ranges */
	ret = migrate_reserved_ranges(trans, root, &cctx->used_space, &buf, fd,
			ino, cfg->num_bytes, convert_flags);

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, root, &key, &path, 0, 1);
	if (ret) {
		ret = (ret > 0 ? -ENOENT : ret);
		goto out;
	}
	btrfs_set_stack_inode_size(&buf, cfg->num_bytes);
	write_extent_buffer(path.nodes[0], &buf,
			btrfs_item_ptr_offset(path.nodes[0], path.slots[0]),
			sizeof(buf));
out:
	free_extent_cache_tree(&used_tmp);
	btrfs_release_path(&path);
	btrfs_commit_transaction(trans, root);
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
	if (ret)
		return ret;

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
	if (!new_root || IS_ERR(new_root)) {
		error("unable to fs read root: %lu", PTR_ERR(new_root));
		return PTR_ERR(new_root);
	}

	ret = btrfs_make_root_dir(trans, new_root, BTRFS_FIRST_FREE_OBJECTID);

	return ret;
}

/*
 * New make_btrfs() has handle system and meta chunks quite well.
 * So only need to add remaining data chunks.
 */
static int make_convert_data_block_groups(struct btrfs_trans_handle *trans,
					  struct btrfs_fs_info *fs_info,
					  struct btrfs_mkfs_config *cfg,
					  struct btrfs_convert_context *cctx)
{
	struct cache_tree *data_chunks = &cctx->data_chunks;
	struct cache_extent *cache;
	u64 max_chunk_size;
	int ret = 0;

	/*
	 * Don't create data chunk over 10% of the convert device
	 * And for single chunk, don't create chunk larger than 1G.
	 */
	max_chunk_size = cfg->num_bytes / 10;
	max_chunk_size = min((u64)(SZ_1G), max_chunk_size);
	max_chunk_size = round_down(max_chunk_size, fs_info->sectorsize);

	for (cache = first_cache_extent(data_chunks); cache;
	     cache = next_cache_extent(cache)) {
		u64 cur = cache->start;

		while (cur < cache->start + cache->size) {
			u64 len;
			u64 cur_backup = cur;

			len = min(max_chunk_size,
				  cache->start + cache->size - cur);
			ret = btrfs_alloc_data_chunk(trans, fs_info, &cur_backup, len);
			if (ret < 0)
				break;
			ret = btrfs_make_block_group(trans, fs_info, 0,
					BTRFS_BLOCK_GROUP_DATA, cur, len);
			if (ret < 0)
				break;
			cur += len;
		}
	}
	return ret;
}

/*
 * Init the temp btrfs to a operational status.
 *
 * It will fix the extent usage accounting(XXX: Do we really need?) and
 * insert needed data chunks, to ensure all old fs data extents are covered
 * by DATA chunks, preventing wrong chunks are allocated.
 *
 * And also create convert image subvolume and relocation tree.
 * (XXX: Not need again?)
 * But the convert image subvolume is *NOT* linked to fs tree yet.
 */
static int init_btrfs(struct btrfs_mkfs_config *cfg, struct btrfs_root *root,
			 struct btrfs_convert_context *cctx, u32 convert_flags)
{
	struct btrfs_key location;
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = root->fs_info;
	int ret;

	/*
	 * Don't alloc any metadata/system chunk, as we don't want
	 * any meta/sys chunk allocated before all data chunks are inserted.
	 * Or we screw up the chunk layout just like the old implement.
	 */
	fs_info->avoid_sys_chunk_alloc = 1;
	fs_info->avoid_meta_chunk_alloc = 1;
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		error("unable to start transaction");
		ret = PTR_ERR(trans);
		goto err;
	}
	ret = btrfs_fix_block_accounting(trans);
	if (ret)
		goto err;
	ret = make_convert_data_block_groups(trans, fs_info, cfg, cctx);
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

	/* subvol for fs image file */
	ret = create_subvol(trans, root, CONV_IMAGE_SUBVOL_OBJECTID);
	if (ret < 0) {
		error("failed to create subvolume image root: %d", ret);
		goto err;
	}
	/* subvol for data relocation tree */
	ret = create_subvol(trans, root, BTRFS_DATA_RELOC_TREE_OBJECTID);
	if (ret < 0) {
		error("failed to create DATA_RELOC root: %d", ret);
		goto err;
	}

	ret = btrfs_commit_transaction(trans, root);
	fs_info->avoid_sys_chunk_alloc = 0;
	fs_info->avoid_meta_chunk_alloc = 0;
err:
	return ret;
}

/*
 * Migrate super block to its default position and zero 0 ~ 16k
 */
static int migrate_super_block(int fd, u64 old_bytenr)
{
	int ret;
	struct btrfs_super_block super;
	u8 result[BTRFS_CSUM_SIZE] = {};
	u32 len;
	u32 bytenr;

	ret = pread(fd, &super, BTRFS_SUPER_INFO_SIZE, old_bytenr);
	if (ret != BTRFS_SUPER_INFO_SIZE)
		goto fail;

	BUG_ON(btrfs_super_bytenr(&super) != old_bytenr);
	btrfs_set_super_bytenr(&super, BTRFS_SUPER_INFO_OFFSET);

	btrfs_csum_data(NULL, btrfs_super_csum_type(&super),
			(u8 *)&super + BTRFS_CSUM_SIZE, result,
			BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
	memcpy(&super.csum[0], result, BTRFS_CSUM_SIZE);
	ret = pwrite(fd, &super , BTRFS_SUPER_INFO_SIZE,
		BTRFS_SUPER_INFO_OFFSET);
	if (ret != BTRFS_SUPER_INFO_SIZE)
		goto fail;

	ret = fsync(fd);
	if (ret)
		goto fail;

	memset(&super, 0, BTRFS_SUPER_INFO_SIZE);
	for (bytenr = 0; bytenr < BTRFS_SUPER_INFO_OFFSET; ) {
		len = BTRFS_SUPER_INFO_OFFSET - bytenr;
		if (len > BTRFS_SUPER_INFO_SIZE)
			len = BTRFS_SUPER_INFO_SIZE;
		ret = pwrite(fd, &super, len, bytenr);
		if (ret != len) {
			fprintf(stderr, "unable to zero fill device\n");
			break;
		}
		bytenr += len;
	}
	ret = 0;
	fsync(fd);
fail:
	if (ret > 0)
		ret = -1;
	return ret;
}

static int convert_open_fs(const char *devname,
			   struct btrfs_convert_context *cctx)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(convert_operations); i++) {
		int ret = convert_operations[i]->open_fs(cctx, devname);

		if (ret == 0) {
			cctx->convert_ops = convert_operations[i];
			return ret;
		}
	}

	error("no file system found to convert");
	return -1;
}

static int do_convert(const char *devname, u32 convert_flags, u32 nodesize,
		const char *fslabel, int progress, u64 features, u16 csum_type,
		char fsid[BTRFS_UUID_UNPARSED_SIZE])
{
	int ret;
	int fd = -1;
	u32 blocksize;
	u64 total_bytes;
	struct btrfs_root *root;
	struct btrfs_root *image_root;
	struct btrfs_convert_context cctx;
	struct btrfs_key key;
	char subvol_name[SOURCE_FS_NAME_LEN + 8];
	struct task_ctx ctx;
	char features_buf[64];
	char fsid_str[BTRFS_UUID_UNPARSED_SIZE];
	struct btrfs_mkfs_config mkfs_cfg;
	bool btrfs_sb_committed = false;

	memset(&mkfs_cfg, 0, sizeof(mkfs_cfg));
	init_convert_context(&cctx);
	ret = convert_open_fs(devname, &cctx);
	if (ret)
		goto fail;
	ret = convert_check_state(&cctx);
	if (ret)
		warning(
		"source filesystem is not clean, running filesystem check is recommended");
	ret = convert_read_used_space(&cctx);
	if (ret)
		goto fail;

	ASSERT(cctx.total_bytes != 0);
	blocksize = cctx.blocksize;
	total_bytes = (u64)blocksize * (u64)cctx.block_count;
	if (blocksize < 4096) {
		error("block size is too small: %u < 4096", blocksize);
		goto fail;
	}
	if (blocksize != getpagesize())
		warning(
"blocksize %u is not equal to the page size %u, converted filesystem won't mount on this system",
			blocksize, getpagesize());

	if (btrfs_check_nodesize(nodesize, blocksize, features))
		goto fail;
	fd = open(devname, O_RDWR);
	if (fd < 0) {
		error("unable to open %s: %m", devname);
		goto fail;
	}
	btrfs_parse_fs_features_to_string(features_buf, features);
	if (features == BTRFS_MKFS_DEFAULT_FEATURES)
		strcat(features_buf, " (default)");

	if (convert_flags & CONVERT_FLAG_COPY_FSID) {
		uuid_unparse(cctx.fs_uuid, mkfs_cfg.fs_uuid);
		if (!test_uuid_unique(mkfs_cfg.fs_uuid))
			warning("non-unique UUID (copy): %s", mkfs_cfg.fs_uuid);
	} else if (fsid[0] == 0) {
		uuid_t uuid;

		uuid_generate(uuid);
		uuid_unparse(uuid, mkfs_cfg.fs_uuid);
	} else {
		memcpy(mkfs_cfg.fs_uuid, fsid, BTRFS_UUID_UNPARSED_SIZE);
		if (!test_uuid_unique(mkfs_cfg.fs_uuid))
			warning("non-unique UUID (user set): %s", mkfs_cfg.fs_uuid);
	}

	printf("Source filesystem:\n");
	printf("  Type:           %s\n", cctx.convert_ops->name);
	printf("  Label:          %s\n", cctx.label);
	printf("  Blocksize:      %u\n", blocksize);
	uuid_unparse(cctx.fs_uuid, fsid_str);
	printf("  UUID:           %s\n", fsid_str);
	printf("Target filesystem:\n");
	printf("  Label:          %s\n", fslabel);
	printf("  Blocksize:      %u\n", blocksize);
	printf("  Nodesize:       %u\n", nodesize);
	printf("  UUID:           %s\n", mkfs_cfg.fs_uuid);
	printf("  Checksum:       %s\n", btrfs_super_csum_name(csum_type));
	printf("  Features:       %s\n", features_buf);
	printf("    Data csum:    %s\n", (convert_flags & CONVERT_FLAG_DATACSUM) ?  "yes" : "no");
	printf("    Inline data:  %s\n", (convert_flags & CONVERT_FLAG_INLINE_DATA) ?  "yes" : "no");
	printf("    Copy xattr:   %s\n", (convert_flags & CONVERT_FLAG_XATTR) ? "yes" : "no");
	printf("Reported stats:\n");
	printf("  Total space:    %12llu\n", cctx.total_bytes);
	printf("  Free space:     %12llu (%.2f%%)\n", cctx.free_bytes_initial,
			100.0 * cctx.free_bytes_initial / cctx.total_bytes);
	printf("  Inode count:    %12llu\n", cctx.inodes_count);
	printf("  Free inodes:    %12llu\n", cctx.free_inodes_count);
	printf("  Block count:    %12llu\n", cctx.block_count);

	mkfs_cfg.csum_type = csum_type;
	mkfs_cfg.label = cctx.label;
	mkfs_cfg.num_bytes = total_bytes;
	mkfs_cfg.nodesize = nodesize;
	mkfs_cfg.sectorsize = blocksize;
	mkfs_cfg.stripesize = blocksize;
	mkfs_cfg.features = features;
	mkfs_cfg.leaf_data_size = __BTRFS_LEAF_DATA_SIZE(nodesize);

	printf("Create initial btrfs filesystem\n");
	ret = make_convert_btrfs(fd, &mkfs_cfg, &cctx);
	if (ret) {
		errno = -ret;
		error("unable to create initial ctree: %m");
		goto fail;
	}

	root = open_ctree_fd(fd, devname, mkfs_cfg.super_bytenr,
			     OPEN_CTREE_WRITES | OPEN_CTREE_TEMPORARY_SUPER);
	if (!root) {
		error("unable to open ctree");
		goto fail;
	}
	ret = init_btrfs(&mkfs_cfg, root, &cctx, convert_flags);
	if (ret) {
		error("unable to setup the root tree: %d", ret);
		goto fail;
	}

	printf("Create %s image file\n", cctx.convert_ops->name);
	snprintf(subvol_name, sizeof(subvol_name), "%s_saved",
			cctx.convert_ops->name);
	key.objectid = CONV_IMAGE_SUBVOL_OBJECTID;
	key.offset = (u64)-1;
	key.type = BTRFS_ROOT_ITEM_KEY;
	image_root = btrfs_read_fs_root(root->fs_info, &key);
	if (!image_root) {
		error("unable to create image subvolume");
		goto fail;
	}
	ret = create_image(image_root, &mkfs_cfg, &cctx, fd,
			      mkfs_cfg.num_bytes, "image",
			      convert_flags);
	if (ret) {
		error("failed to create %s/image: %d", subvol_name, ret);
		goto fail;
	}

	printf("Create btrfs metadata\n");
	ret = pthread_mutex_init(&ctx.mutex, NULL);
	if (ret) {
		error("failed to initialize mutex: %d", ret);
		goto fail;
	}
	ctx.max_copy_inodes = (cctx.inodes_count - cctx.free_inodes_count);
	ctx.cur_copy_inodes = 0;

	if (progress) {
		ctx.info = task_init(print_copied_inodes, after_copied_inodes,
				     &ctx);
		task_start(ctx.info, NULL, NULL);
	}
	ret = copy_inodes(&cctx, root, convert_flags, &ctx);
	if (ret) {
		error("error during copy_inodes %d", ret);
		goto fail;
	}
	if (progress) {
		task_stop(ctx.info);
		task_deinit(ctx.info);
	}

	image_root = btrfs_mksubvol(root, subvol_name,
				    CONV_IMAGE_SUBVOL_OBJECTID, true);
	if (!image_root) {
		error("unable to link subvolume %s", subvol_name);
		goto fail;
	}

	memset(root->fs_info->super_copy->label, 0, BTRFS_LABEL_SIZE);
	if (convert_flags & CONVERT_FLAG_COPY_LABEL) {
		__strncpy_null(root->fs_info->super_copy->label,
				cctx.label, BTRFS_LABEL_SIZE - 1);
		printf("Copy label '%s'\n", root->fs_info->super_copy->label);
	} else if (convert_flags & CONVERT_FLAG_SET_LABEL) {
		strcpy(root->fs_info->super_copy->label, fslabel);
		printf("Set label to '%s'\n", fslabel);
	}

	ret = close_ctree(root);
	if (ret) {
		error("close_ctree failed: %d", ret);
		goto fail;
	}
	convert_close_fs(&cctx);
	clean_convert_context(&cctx);

	/*
	 * If this step succeed, we get a mountable btrfs. Otherwise
	 * the source fs is left unchanged.
	 */
	ret = migrate_super_block(fd, mkfs_cfg.super_bytenr);
	if (ret) {
		error("unable to migrate super block: %d", ret);
		goto fail;
	}
	btrfs_sb_committed = true;

	root = open_ctree_fd(fd, devname, 0,
			     OPEN_CTREE_WRITES | OPEN_CTREE_TEMPORARY_SUPER);
	if (!root) {
		error("unable to open ctree for finalization");
		goto fail;
	}
	root->fs_info->finalize_on_close = 1;
	close_ctree(root);
	close(fd);

	printf("Conversion complete\n");
	return 0;
fail:
	clean_convert_context(&cctx);
	if (fd != -1)
		close(fd);
	if (btrfs_sb_committed)
		warning(
"error during conversion, filesystem is partially created but not finalized and not mountable");
	else
		warning(
"error during conversion, the original filesystem is not modified");
	return -1;
}

/*
 * Read out data of convert image which is in btrfs reserved ranges so we can
 * use them to overwrite the ranges during rollback.
 */
static int read_reserved_ranges(struct btrfs_root *root, u64 ino,
				u64 total_bytes, char *reserved_ranges[])
{
	int i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(btrfs_reserved_ranges); i++) {
		const struct simple_range *range = &btrfs_reserved_ranges[i];

		if (range->start + range->len >= total_bytes)
			break;
		ret = btrfs_read_file(root, ino, range->start, range->len,
				      reserved_ranges[i]);
		if (ret < range->len) {
			error(
	"failed to read data of convert image, offset=%llu len=%llu ret=%d",
			      range->start, range->len, ret);
			if (ret >= 0)
				ret = -EIO;
			break;
		}
		ret = 0;
	}
	return ret;
}

static bool is_subset_of_reserved_ranges(u64 start, u64 len)
{
	int i;
	bool ret = false;

	for (i = 0; i < ARRAY_SIZE(btrfs_reserved_ranges); i++) {
		const struct simple_range *range = &btrfs_reserved_ranges[i];

		if (start >= range->start && start + len <= range_end(range)) {
			ret = true;
			break;
		}
	}
	return ret;
}

static bool is_chunk_direct_mapped(struct btrfs_fs_info *fs_info, u64 start)
{
	struct cache_extent *ce;
	struct map_lookup *map;
	bool ret = false;

	ce = search_cache_extent(&fs_info->mapping_tree.cache_tree, start);
	if (!ce)
		goto out;
	if (ce->start > start || ce->start + ce->size < start)
		goto out;

	map = container_of(ce, struct map_lookup, ce);

	/* Not SINGLE chunk */
	if (map->num_stripes != 1)
		goto out;

	/* Chunk's logical doesn't match with physical, not 1:1 mapped */
	if (map->ce.start != map->stripes[0].physical)
		goto out;
	ret = true;
out:
	return ret;
}

/*
 * Iterate all file extents of the convert image.
 *
 * All file extents except ones in btrfs_reserved_ranges must be mapped 1:1
 * on disk. (Means their file_offset must match their on disk bytenr)
 *
 * File extents in reserved ranges can be relocated to other place, and in
 * that case we will read them out for later use.
 */
static int check_convert_image(struct btrfs_root *image_root, u64 ino,
			       u64 total_size, char *reserved_ranges[])
{
	struct btrfs_key key;
	struct btrfs_path path;
	struct btrfs_fs_info *fs_info = image_root->fs_info;
	u64 checked_bytes = 0;
	int ret;

	key.objectid = ino;
	key.offset = 0;
	key.type = BTRFS_EXTENT_DATA_KEY;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, image_root, &key, &path, 0, 0);
	/*
	 * It's possible that some fs doesn't store any (including sb)
	 * data into 0~1M range, and NO_HOLES is enabled.
	 *
	 * So we only need to check if ret < 0
	 */
	if (ret < 0) {
		errno = -ret;
		error("failed to iterate file extents at offset 0: %m");
		btrfs_release_path(&path);
		return ret;
	}

	/* Loop from the first file extents */
	while (1) {
		struct btrfs_file_extent_item *fi;
		struct extent_buffer *leaf = path.nodes[0];
		u64 disk_bytenr;
		u64 file_offset;
		u64 ram_bytes;
		int slot = path.slots[0];

		if (slot >= btrfs_header_nritems(leaf))
			goto next;
		btrfs_item_key_to_cpu(leaf, &key, slot);

		/*
		 * Iteration is done, exit normally, we have extra check out of
		 * the loop
		 */
		if (key.objectid != ino || key.type != BTRFS_EXTENT_DATA_KEY) {
			ret = 0;
			break;
		}
		file_offset = key.offset;
		fi = btrfs_item_ptr(leaf, slot, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(leaf, fi) != BTRFS_FILE_EXTENT_REG) {
			ret = -EINVAL;
			error(
		"ino %llu offset %llu doesn't have a regular file extent",
				ino, file_offset);
			break;
		}
		if (btrfs_file_extent_compression(leaf, fi) ||
		    btrfs_file_extent_encryption(leaf, fi) ||
		    btrfs_file_extent_other_encoding(leaf, fi)) {
			ret = -EINVAL;
			error(
			"ino %llu offset %llu doesn't have a plain file extent",
				ino, file_offset);
			break;
		}

		disk_bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
		ram_bytes = btrfs_file_extent_ram_bytes(leaf, fi);

		checked_bytes += ram_bytes;
		/* Skip hole */
		if (disk_bytenr == 0)
			goto next;

		/*
		 * Most file extents must be 1:1 mapped, which means 2 things:
		 * 1) File extent file offset == disk_bytenr
		 * 2) That data chunk's logical == chunk's physical
		 *
		 * So file extent's file offset == physical position on disk.
		 *
		 * And after rolling back btrfs reserved range, other part
		 * remains what old fs used to be.
		 */
		if (file_offset != disk_bytenr ||
		    !is_chunk_direct_mapped(fs_info, disk_bytenr)) {
			/*
			 * Only file extent in btrfs reserved ranges are
			 * allowed to be non-1:1 mapped
			 */
			if (!is_subset_of_reserved_ranges(file_offset,
							ram_bytes)) {
				ret = -EINVAL;
				error(
		"ino %llu offset %llu file extent should not be relocated",
					ino, file_offset);
				break;
			}
		}
next:
		ret = btrfs_next_item(image_root, &path);
		if (ret) {
			if (ret > 0)
				ret = 0;
			break;
		}
	}
	btrfs_release_path(&path);
	if (ret)
		return ret;
	/*
	 * For HOLES mode (without NO_HOLES), we must ensure file extents
	 * cover the whole range of the image
	 */
	if (!ret && !btrfs_fs_incompat(fs_info, NO_HOLES)) {
		if (checked_bytes != total_size) {
			ret = -EINVAL;
			error("inode %llu has some file extents not checked",
				ino);
			return ret;
		}
	}

	/* So far so good, read old data located in btrfs reserved ranges */
	ret = read_reserved_ranges(image_root, ino, total_size,
				   reserved_ranges);
	return ret;
}

/*
 * btrfs rollback is just reverted convert:
 * |<---------------Btrfs fs------------------------------>|
 * |<-   Old data chunk  ->|< new chunk (D/M/S)>|<- ODC  ->|
 * |<-Old-FE->| |<-Old-FE->|<- Btrfs extents  ->|<-Old-FE->|
 *                           ||
 *                           \/
 * |<------------------Old fs----------------------------->|
 * |<- used ->| |<- used ->|                    |<- used ->|
 *
 * However things are much easier than convert, we don't really need to
 * do the complex space calculation, but only to handle btrfs reserved space
 *
 * |<---------------------------Btrfs fs----------------------------->|
 * |   RSV 1   |  | Old  |   |    RSV 2  | | Old  | |   RSV 3   |
 * |   0~1M    |  | Fs   |   | SB2 + 64K | | Fs   | | SB3 + 64K |
 *
 * On the other hand, the converted fs image in btrfs is a completely
 * valid old fs.
 *
 * |<-----------------Converted fs image in btrfs-------------------->|
 * |   RSV 1   |  | Old  |   |    RSV 2  | | Old  | |   RSV 3   |
 * | Relocated |  | Fs   |   | Relocated | | Fs   | | Relocated |
 *
 * Used space in fs image should be at the same physical position on disk.
 * We only need to recover the data in reserved ranges, so the whole
 * old fs is back.
 *
 * The idea to rollback is also straightforward, we just "read" out the data
 * of reserved ranges, and write them back to there they should be.
 * Then the old fs is back.
 */
static int do_rollback(const char *devname)
{
	struct btrfs_root *root;
	struct btrfs_root *image_root;
	struct btrfs_fs_info *fs_info;
	struct btrfs_key key;
	struct btrfs_path path;
	struct btrfs_dir_item *dir;
	struct btrfs_inode_item *inode_item;
	struct btrfs_root_ref *root_ref_item;
	char *image_name = "image";
	char dir_name[PATH_MAX];
	int name_len;
	char fsid_str[BTRFS_UUID_UNPARSED_SIZE];
	char *reserved_ranges[ARRAY_SIZE(btrfs_reserved_ranges)] = { NULL };
	u64 total_bytes;
	u64 fsize;
	u64 root_dir;
	u64 ino;
	int fd = -1;
	int ret;
	int i;

	printf("Open filesystem for rollback:\n");

	for (i = 0; i < ARRAY_SIZE(btrfs_reserved_ranges); i++) {
		const struct simple_range *range = &btrfs_reserved_ranges[i];

		reserved_ranges[i] = calloc(1, range->len);
		if (!reserved_ranges[i]) {
			ret = -ENOMEM;
			goto free_mem;
		}
	}
	fd = open(devname, O_RDWR);
	if (fd < 0) {
		error("unable to open %s: %m", devname);
		ret = -EIO;
		goto free_mem;
	}
	fsize = lseek(fd, 0, SEEK_END);

	/*
	 * For rollback, we don't really need to write anything so open it
	 * read-only.  The write part will happen after we close the
	 * filesystem.
	 */
	root = open_ctree_fd(fd, devname, 0, 0);
	if (!root) {
		error("unable to open ctree");
		ret = -EIO;
		goto free_mem;
	}
	fs_info = root->fs_info;

	printf("  Label:           %s\n", fs_info->super_copy->label);
	uuid_unparse(fs_info->super_copy->fsid, fsid_str);
	printf("  UUID:            %s\n", fsid_str);

	/*
	 * Search root backref first, or after subvolume deletion (orphan),
	 * we can still rollback the image.
	 */
	key.objectid = CONV_IMAGE_SUBVOL_OBJECTID;
	key.type = BTRFS_ROOT_BACKREF_KEY;
	key.offset = BTRFS_FS_TREE_OBJECTID;
	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, fs_info->tree_root, &key, &path, 0, 0);
	if (ret > 0) {
		error("unable to find source fs image subvolume, is it deleted?");
		ret = -ENOENT;
		goto close_fs;
	} else if (ret < 0) {
		errno = -ret;
		error("failed to find source fs image subvolume: %m");
		goto close_fs;
	}
	/* (256 ROOT_BACKREF 5) */
	/* root backref key dirid 256 sequence 3 name ext2_saved */
	root_ref_item = btrfs_item_ptr(path.nodes[0], path.slots[0], struct btrfs_root_ref);
	name_len = btrfs_root_ref_name_len(path.nodes[0], root_ref_item);
	if (name_len > sizeof(dir_name))
		name_len = sizeof(dir_name) - 1;
	read_extent_buffer(path.nodes[0], dir_name, (unsigned long)(root_ref_item + 1), name_len);
	dir_name[sizeof(dir_name) - 1] = 0;

	printf("  Restoring from:  %s/%s\n", dir_name, image_name);

	btrfs_release_path(&path);

	/* Search convert subvolume */
	key.objectid = CONV_IMAGE_SUBVOL_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	image_root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(image_root)) {
		ret = PTR_ERR(image_root);
		errno = -ret;
		error("failed to open convert image subvolume: %m");
		goto close_fs;
	}

	/* Search the image file */
	root_dir = btrfs_root_dirid(&image_root->root_item);
	dir = btrfs_lookup_dir_item(NULL, image_root, &path, root_dir,
			image_name, strlen(image_name), 0);

	if (!dir || IS_ERR(dir)) {
		btrfs_release_path(&path);
		if (dir)
			ret = PTR_ERR(dir);
		else
			ret = -ENOENT;
		errno = -ret;
		error("failed to locate file %s: %m", image_name);
		goto close_fs;
	}
	btrfs_dir_item_key_to_cpu(path.nodes[0], dir, &key);
	btrfs_release_path(&path);

	/* Get total size of the original image */
	ino = key.objectid;

	ret = btrfs_lookup_inode(NULL, image_root, &path, &key, 0);

	if (ret < 0) {
		btrfs_release_path(&path);
		errno = -ret;
		error("unable to find inode %llu: %m", ino);
		goto close_fs;
	}
	inode_item = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_inode_item);
	total_bytes = btrfs_inode_size(path.nodes[0], inode_item);
	btrfs_release_path(&path);

	/* Check if we can rollback the image */
	ret = check_convert_image(image_root, ino, total_bytes, reserved_ranges);
	if (ret < 0) {
		error("old fs image can't be rolled back");
		goto close_fs;
	}
close_fs:
	btrfs_release_path(&path);
	close_ctree_fs_info(fs_info);
	if (ret)
		goto free_mem;

	/*
	 * Everything is OK, just write back old fs data into btrfs reserved
	 * ranges
	 *
	 * Here, we starts from the backup blocks first, so if something goes
	 * wrong, the fs is still mountable
	 */

	for (i = ARRAY_SIZE(btrfs_reserved_ranges) - 1; i >= 0; i--) {
		u64 real_size;
		const struct simple_range *range = &btrfs_reserved_ranges[i];

		if (range_end(range) >= fsize)
			continue;

		real_size = min(range_end(range), fsize) - range->start;
		ret = pwrite(fd, reserved_ranges[i], real_size, range->start);
		if (ret < real_size) {
			if (ret < 0)
				ret = -errno;
			else
				ret = -EIO;
			errno = -ret;
			error("failed to recover range [%llu, %llu): %m",
			      range->start, real_size);
			goto free_mem;
		}
		ret = 0;
	}

free_mem:
	for (i = 0; i < ARRAY_SIZE(btrfs_reserved_ranges); i++)
		free(reserved_ranges[i]);
	if (ret)
		error("rollback failed");
	else
		printf("Rollback succeeded\n");
	return ret;
}

static void print_usage(void)
{
	printf("usage: btrfs-convert [options] device\n");
	printf("options:\n");
	printf("\t-d|--no-datasum        disable data checksum, sets NODATASUM\n");
	printf("\t-i|--no-xattr          ignore xattrs and ACLs\n");
	printf("\t-n|--no-inline         disable inlining of small files to metadata\n");
	printf("\t--csum TYPE\n");
	printf("\t--checksum TYPE        checksum algorithm to use (default: crc32c)\n");
	printf("\t-N|--nodesize SIZE     set filesystem metadata nodesize\n");
	printf("\t-r|--rollback          roll back to the original filesystem\n");
	printf("\t-l|--label LABEL       set filesystem label\n");
	printf("\t-L|--copy-label        use label from converted filesystem\n");
	printf("\t--uuid SPEC            new, copy or user-defined conforming UUID\n");
	printf("\t-p|--progress          show converting progress (default)\n");
	printf("\t-O|--features LIST     comma separated list of filesystem features\n");
	printf("\t--no-progress          show only overview, not the detailed progress\n");
	printf("\n");
	printf("Supported filesystems:\n");
	printf("\text2/3/4: %s\n", BTRFSCONVERT_EXT2 ? "yes" : "no");
	printf("\treiserfs: %s\n", BTRFSCONVERT_REISERFS ? "yes" : "no");
}

int BOX_MAIN(convert)(int argc, char *argv[])
{
	int ret;
	int packing = 1;
	int noxattr = 0;
	int datacsum = 1;
	u32 nodesize = max_t(u32, sysconf(_SC_PAGESIZE),
			BTRFS_MKFS_DEFAULT_NODE_SIZE);
	int rollback = 0;
	int copylabel = 0;
	int usage_error = 0;
	int progress = 1;
	char *file;
	char fslabel[BTRFS_LABEL_SIZE] = { 0 };
	u64 features = BTRFS_MKFS_DEFAULT_FEATURES;
	u16 csum_type = BTRFS_CSUM_TYPE_CRC32;
	u32 copy_fsid = 0;
	char fsid[BTRFS_UUID_UNPARSED_SIZE] = {0};

	crc32c_optimization_init();
	printf("btrfs-convert from %s\n\n", PACKAGE_STRING);

	while(1) {
		enum { GETOPT_VAL_NO_PROGRESS = 256, GETOPT_VAL_CHECKSUM,
			GETOPT_VAL_UUID };
		static const struct option long_options[] = {
			{ "no-progress", no_argument, NULL,
				GETOPT_VAL_NO_PROGRESS },
			{ "no-datasum", no_argument, NULL, 'd' },
			{ "no-inline", no_argument, NULL, 'n' },
			{ "no-xattr", no_argument, NULL, 'i' },
			{ "checksum", required_argument, NULL,
				GETOPT_VAL_CHECKSUM },
			{ "csum", required_argument, NULL,
				GETOPT_VAL_CHECKSUM },
			{ "rollback", no_argument, NULL, 'r' },
			{ "features", required_argument, NULL, 'O' },
			{ "progress", no_argument, NULL, 'p' },
			{ "label", required_argument, NULL, 'l' },
			{ "copy-label", no_argument, NULL, 'L' },
			{ "uuid", required_argument, NULL, GETOPT_VAL_UUID },
			{ "nodesize", required_argument, NULL, 'N' },
			{ "help", no_argument, NULL, GETOPT_VAL_HELP},
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "dinN:rl:LpO:", long_options, NULL);

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
			case 'N':
				nodesize = parse_size_from_string(optarg);
				break;
			case 'r':
				rollback = 1;
				break;
			case 'l':
				copylabel = CONVERT_FLAG_SET_LABEL;
				if (strlen(optarg) >= BTRFS_LABEL_SIZE) {
					warning(
					"label too long, trimmed to %d bytes",
						BTRFS_LABEL_SIZE - 1);
				}
				__strncpy_null(fslabel, optarg, BTRFS_LABEL_SIZE - 1);
				break;
			case 'L':
				copylabel = CONVERT_FLAG_COPY_LABEL;
				break;
			case 'p':
				progress = 1;
				break;
			case 'O': {
				char *orig = strdup(optarg);
				char *tmp = orig;

				tmp = btrfs_parse_fs_features(tmp, &features);
				if (tmp) {
					error("unrecognized filesystem feature: %s",
							tmp);
					free(orig);
					exit(1);
				}
				free(orig);
				if (features & BTRFS_FEATURE_LIST_ALL) {
					btrfs_list_all_fs_features(
						~BTRFS_CONVERT_ALLOWED_FEATURES);
					exit(0);
				}
				if (features & ~BTRFS_CONVERT_ALLOWED_FEATURES) {
					char buf[64];

					btrfs_parse_fs_features_to_string(buf,
						features & ~BTRFS_CONVERT_ALLOWED_FEATURES);
					error("features not allowed for convert: %s",
						buf);
					exit(1);
				}

				break;
				}
			case GETOPT_VAL_NO_PROGRESS:
				progress = 0;
				break;
			case GETOPT_VAL_CHECKSUM:
				csum_type = parse_csum_type(optarg);
				break;
			case GETOPT_VAL_UUID:
				copy_fsid = 0;
				fsid[0] = 0;
				if (strcmp(optarg, "copy") == 0) {
					copy_fsid = CONVERT_FLAG_COPY_FSID;
				} else if (strcmp(optarg, "new") == 0) {
					/* Generated later */
				} else {
					uuid_t uuid;

					if (uuid_parse(optarg, uuid) != 0) {
						error("invalid UUID: %s\n", optarg);
						return 1;
					}
					strncpy(fsid, optarg, sizeof(fsid));
				}
				break;
			case GETOPT_VAL_HELP:
			default:
				print_usage();
				return c != GETOPT_VAL_HELP;
		}
	}
	set_argv0(argv);
	if (check_argc_exact(argc - optind, 1)) {
		print_usage();
		return 1;
	}

	if (rollback && (!datacsum || noxattr || !packing)) {
		fprintf(stderr,
			"Usage error: -d, -i, -n options do not apply to rollback\n");
		usage_error++;
	}

	if (usage_error) {
		print_usage();
		return 1;
	}

	file = argv[optind];
	ret = check_mounted(file);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		return 1;
	} else if (ret) {
		error("%s is mounted", file);
		return 1;
	}

	if (rollback) {
		ret = do_rollback(file);
	} else {
		u32 cf = 0;

		cf |= datacsum ? CONVERT_FLAG_DATACSUM : 0;
		cf |= packing ? CONVERT_FLAG_INLINE_DATA : 0;
		cf |= noxattr ? 0 : CONVERT_FLAG_XATTR;
		cf |= copy_fsid;
		cf |= copylabel;
		ret = do_convert(file, cf, nodesize, fslabel, progress, features,
				 csum_type, fsid);
	}
	if (ret)
		return 1;
	return 0;
}
