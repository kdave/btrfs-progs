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

#include "kerncompat.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>

#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "utils.h"
#include "task-utils.h"
#include "help.h"
#include "mkfs/common.h"
#include "convert/common.h"
#include "convert/source-fs.h"
#include "fsfeatures.h"

const struct btrfs_convert_operations ext2_convert_ops;

static const struct btrfs_convert_operations *convert_operations[] = {
#if BTRFSCONVERT_EXT2
	&ext2_convert_ops,
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
		printf("copy inodes [%c] [%10llu/%10llu]\r",
		       work_indicator[count % 4],
		       (unsigned long long)priv->cur_copy_inodes,
		       (unsigned long long)priv->max_copy_inodes);
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
	u32 blocksize = root->sectorsize;
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
					    root->fs_info->csum_root,
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
	struct btrfs_block_group_cache *bg_cache;
	u64 len = *ret_len;
	u64 disk_bytenr;
	int i;
	int ret;
	u32 datacsum = convert_flags & CONVERT_FLAG_DATACSUM;

	if (bytenr != round_down(bytenr, root->sectorsize)) {
		error("bytenr not sectorsize aligned: %llu",
				(unsigned long long)bytenr);
		return -EINVAL;
	}
	if (len != round_down(len, root->sectorsize)) {
		error("length not sectorsize aligned: %llu",
				(unsigned long long)len);
		return -EINVAL;
	}
	len = min_t(u64, len, BTRFS_MAX_EXTENT_SIZE);

	/*
	 * Skip sb ranges first
	 * [0, 1M), [sb_offset(1), +64K), [sb_offset(2), +64K].
	 *
	 * Or we will insert a hole into current image file, and later
	 * migrate block will fail as there is already a file extent.
	 */
	if (bytenr < 1024 * 1024) {
		*ret_len = 1024 * 1024 - bytenr;
		return 0;
	}
	for (i = 1; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		u64 cur = btrfs_sb_offset(i);

		if (bytenr >= cur && bytenr < cur + BTRFS_STRIPE_LEN) {
			*ret_len = cur + BTRFS_STRIPE_LEN - bytenr;
			return 0;
		}
	}
	for (i = 1; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		u64 cur = btrfs_sb_offset(i);

		/*
		 *      |--reserved--|
		 * |----range-------|
		 * May still need to go through file extent inserts
		 */
		if (bytenr < cur && bytenr + len >= cur) {
			len = min_t(u64, len, cur - bytenr);
			break;
		}
		/*
		 * |--reserved--|
		 *      |---range---|
		 * Drop out, no need to insert anything
		 */
		if (bytenr >= cur && bytenr < cur + BTRFS_STRIPE_LEN) {
			*ret_len = cur + BTRFS_STRIPE_LEN - bytenr;
			return 0;
		}
	}

	cache = search_cache_extent(used, bytenr);
	if (cache) {
		if (cache->start <= bytenr) {
			/*
			 * |///////Used///////|
			 *	|<--insert--->|
			 *	bytenr
			 */
			len = min_t(u64, len, cache->start + cache->size -
				    bytenr);
			disk_bytenr = bytenr;
		} else {
			/*
			 *		|//Used//|
			 *  |<-insert-->|
			 *  bytenr
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
		 */
		disk_bytenr = 0;
		datacsum = 0;
	}

	if (disk_bytenr) {
		/* Check if the range is in a data block group */
		bg_cache = btrfs_lookup_block_group(root->fs_info, bytenr);
		if (!bg_cache)
			return -ENOENT;
		if (!(bg_cache->flags & BTRFS_BLOCK_GROUP_DATA))
			return -EINVAL;

		/* The extent should never cross block group boundary */
		len = min_t(u64, len, bg_cache->key.objectid +
			    bg_cache->key.offset - bytenr);
	}

	if (len != round_down(len, root->sectorsize)) {
		error("remaining length not sectorsize aligned: %llu",
				(unsigned long long)len);
		return -EINVAL;
	}
	ret = btrfs_record_file_extent(trans, root, ino, inode, bytenr,
				       disk_bytenr, len);
	if (ret < 0)
		return ret;

	if (datacsum)
		ret = csum_disk_extent(trans, root, bytenr, len);
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
				      u64 ino, u64 start, u64 len,
				      u32 convert_flags)
{
	u64 cur_off = start;
	u64 cur_len = len;
	u64 hole_start = start;
	u64 hole_len;
	struct cache_extent *cache;
	struct btrfs_key key;
	struct extent_buffer *eb;
	int ret = 0;

	while (cur_off < start + len) {
		cache = lookup_cache_extent(used, cur_off, cur_len);
		if (!cache)
			break;
		cur_off = max(cache->start, cur_off);
		cur_len = min(cache->start + cache->size, start + len) -
			  cur_off;
		BUG_ON(cur_len < root->sectorsize);

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

		/* Write the data */
		ret = write_and_map_eb(root, eb);
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
		cur_len = start + len - cur_off;
	}
	/* Last hole */
	if (start + len - hole_start > 0)
		ret = btrfs_record_file_extent(trans, root, ino, inode,
				hole_start, 0, start + len - hole_start);
	return ret;
}

/*
 * Relocate the used ext2 data in reserved ranges
 * [0,1M)
 * [btrfs_sb_offset(1), +BTRFS_STRIPE_LEN)
 * [btrfs_sb_offset(2), +BTRFS_STRIPE_LEN)
 */
static int migrate_reserved_ranges(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct cache_tree *used,
				   struct btrfs_inode_item *inode, int fd,
				   u64 ino, u64 total_bytes, u32 convert_flags)
{
	u64 cur_off;
	u64 cur_len;
	int ret = 0;

	/* 0 ~ 1M */
	cur_off = 0;
	cur_len = 1024 * 1024;
	ret = migrate_one_reserved_range(trans, root, used, inode, fd, ino,
					 cur_off, cur_len, convert_flags);
	if (ret < 0)
		return ret;

	/* second sb(fisrt sb is included in 0~1M) */
	cur_off = btrfs_sb_offset(1);
	cur_len = min(total_bytes, cur_off + BTRFS_STRIPE_LEN) - cur_off;
	if (cur_off > total_bytes)
		return ret;
	ret = migrate_one_reserved_range(trans, root, used, inode, fd, ino,
					 cur_off, cur_len, convert_flags);
	if (ret < 0)
		return ret;

	/* Last sb */
	cur_off = btrfs_sb_offset(2);
	cur_len = min(total_bytes, cur_off + BTRFS_STRIPE_LEN) - cur_off;
	if (cur_off > total_bytes)
		return ret;
	ret = migrate_one_reserved_range(trans, root, used, inode, fd, ino,
					 cur_off, cur_len, convert_flags);
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
	int ret;

	ret = wipe_one_reserved_range(tree, 0, 1024 * 1024, min_stripe_size,
				      ensure_size);
	if (ret < 0)
		return ret;
	ret = wipe_one_reserved_range(tree, btrfs_sb_offset(1),
			BTRFS_STRIPE_LEN, min_stripe_size, ensure_size);
	if (ret < 0)
		return ret;
	ret = wipe_one_reserved_range(tree, btrfs_sb_offset(2),
			BTRFS_STRIPE_LEN, min_stripe_size, ensure_size);
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
	u64 min_stripe_size = 2 * 16 * 1024 * 1024;
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
	 * Always round up the start bytenr, to avoid metadata extent corss
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

/*
 * Read used space, and since we have the used space,
 * calcuate data_chunks and free for later mkfs
 */
static int convert_read_used_space(struct btrfs_convert_context *cctx)
{
	int ret;

	ret = cctx->convert_ops->read_used_space(cctx);
	if (ret)
		return ret;

	ret = calculate_available_space(cctx);
	return ret;
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
	if (!trans)
		return -ENOMEM;

	cache_tree_init(&used_tmp);
	btrfs_init_path(&path);

	ret = btrfs_find_free_objectid(trans, root, BTRFS_FIRST_FREE_OBJECTID,
				       &ino);
	if (ret < 0)
		goto out;
	ret = btrfs_new_inode(trans, root, ino, 0400 | S_IFREG);
	if (ret < 0)
		goto out;
	ret = btrfs_change_inode_flags(trans, root, ino, flags);
	if (ret < 0)
		goto out;
	ret = btrfs_add_link(trans, root, ino, BTRFS_FIRST_FREE_OBJECTID, name,
			     strlen(name), BTRFS_FT_REG_FILE, NULL, 1);
	if (ret < 0)
		goto out;

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
	cur = 1024 * 1024;
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

static struct btrfs_root* link_subvol(struct btrfs_root *root,
		const char *base, u64 root_objectid)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *new_root = NULL;
	struct btrfs_path path;
	struct btrfs_inode_item *inode_item;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 dirid = btrfs_root_dirid(&root->root_item);
	u64 index = 2;
	char buf[BTRFS_NAME_LEN + 1]; /* for snprintf null */
	int len;
	int i;
	int ret;

	len = strlen(base);
	if (len == 0 || len > BTRFS_NAME_LEN)
		return NULL;

	btrfs_init_path(&path);
	key.objectid = dirid;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret <= 0) {
		error("search for DIR_INDEX dirid %llu failed: %d",
				(unsigned long long)dirid, ret);
		goto fail;
	}

	if (path.slots[0] > 0) {
		path.slots[0]--;
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.objectid == dirid && key.type == BTRFS_DIR_INDEX_KEY)
			index = key.offset + 1;
	}
	btrfs_release_path(&path);

	trans = btrfs_start_transaction(root, 1);
	if (!trans) {
		error("unable to start transaction");
		goto fail;
	}

	key.objectid = dirid;
	key.offset = 0;
	key.type =  BTRFS_INODE_ITEM_KEY;

	ret = btrfs_lookup_inode(trans, root, &path, &key, 1);
	if (ret) {
		error("search for INODE_ITEM %llu failed: %d",
				(unsigned long long)dirid, ret);
		goto fail;
	}
	leaf = path.nodes[0];
	inode_item = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_inode_item);

	key.objectid = root_objectid;
	key.offset = (u64)-1;
	key.type = BTRFS_ROOT_ITEM_KEY;

	memcpy(buf, base, len);
	for (i = 0; i < 1024; i++) {
		ret = btrfs_insert_dir_item(trans, root, buf, len,
					    dirid, &key, BTRFS_FT_DIR, index);
		if (ret != -EEXIST)
			break;
		len = snprintf(buf, ARRAY_SIZE(buf), "%s%d", base, i);
		if (len < 1 || len > BTRFS_NAME_LEN) {
			ret = -EINVAL;
			break;
		}
	}
	if (ret)
		goto fail;

	btrfs_set_inode_size(leaf, inode_item, len * 2 +
			     btrfs_inode_size(leaf, inode_item));
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(&path);

	/* add the backref first */
	ret = btrfs_add_root_ref(trans, tree_root, root_objectid,
				 BTRFS_ROOT_BACKREF_KEY,
				 root->root_key.objectid,
				 dirid, index, buf, len);
	if (ret) {
		error("unable to add root backref for %llu: %d",
				root->root_key.objectid, ret);
		goto fail;
	}

	/* now add the forward ref */
	ret = btrfs_add_root_ref(trans, tree_root, root->root_key.objectid,
				 BTRFS_ROOT_REF_KEY, root_objectid,
				 dirid, index, buf, len);
	if (ret) {
		error("unable to add root ref for %llu: %d",
				root->root_key.objectid, ret);
		goto fail;
	}

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		error("transaction commit failed: %d", ret);
		goto fail;
	}

	new_root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(new_root)) {
		error("unable to fs read root: %lu", PTR_ERR(new_root));
		new_root = NULL;
	}
fail:
	btrfs_init_path(&path);
	return new_root;
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
	struct btrfs_root *extent_root = fs_info->extent_root;
	struct cache_tree *data_chunks = &cctx->data_chunks;
	struct cache_extent *cache;
	u64 max_chunk_size;
	int ret = 0;

	/*
	 * Don't create data chunk over 10% of the convert device
	 * And for single chunk, don't create chunk larger than 1G.
	 */
	max_chunk_size = cfg->num_bytes / 10;
	max_chunk_size = min((u64)(1024 * 1024 * 1024), max_chunk_size);
	max_chunk_size = round_down(max_chunk_size, extent_root->sectorsize);

	for (cache = first_cache_extent(data_chunks); cache;
	     cache = next_cache_extent(cache)) {
		u64 cur = cache->start;

		while (cur < cache->start + cache->size) {
			u64 len;
			u64 cur_backup = cur;

			len = min(max_chunk_size,
				  cache->start + cache->size - cur);
			ret = btrfs_alloc_data_chunk(trans, extent_root,
					&cur_backup, len,
					BTRFS_BLOCK_GROUP_DATA, 1);
			if (ret < 0)
				break;
			ret = btrfs_make_block_group(trans, extent_root, 0,
					BTRFS_BLOCK_GROUP_DATA,
					BTRFS_FIRST_CHUNK_TREE_OBJECTID,
					cur, len);
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
	 * any meta/sys chunk allcated before all data chunks are inserted.
	 * Or we screw up the chunk layout just like the old implement.
	 */
	fs_info->avoid_sys_chunk_alloc = 1;
	fs_info->avoid_meta_chunk_alloc = 1;
	trans = btrfs_start_transaction(root, 1);
	if (!trans) {
		error("unable to start transaction");
		ret = -EINVAL;
		goto err;
	}
	ret = btrfs_fix_block_accounting(trans, root);
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
	struct extent_buffer *buf;
	struct btrfs_super_block *super;
	u32 len;
	u32 bytenr;

	buf = malloc(sizeof(*buf) + BTRFS_SUPER_INFO_SIZE);
	if (!buf)
		return -ENOMEM;

	buf->len = BTRFS_SUPER_INFO_SIZE;
	ret = pread(fd, buf->data, BTRFS_SUPER_INFO_SIZE, old_bytenr);
	if (ret != BTRFS_SUPER_INFO_SIZE)
		goto fail;

	super = (struct btrfs_super_block *)buf->data;
	BUG_ON(btrfs_super_bytenr(super) != old_bytenr);
	btrfs_set_super_bytenr(super, BTRFS_SUPER_INFO_OFFSET);

	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, BTRFS_SUPER_INFO_SIZE,
		BTRFS_SUPER_INFO_OFFSET);
	if (ret != BTRFS_SUPER_INFO_SIZE)
		goto fail;

	ret = fsync(fd);
	if (ret)
		goto fail;

	memset(buf->data, 0, BTRFS_SUPER_INFO_SIZE);
	for (bytenr = 0; bytenr < BTRFS_SUPER_INFO_OFFSET; ) {
		len = BTRFS_SUPER_INFO_OFFSET - bytenr;
		if (len > BTRFS_SUPER_INFO_SIZE)
			len = BTRFS_SUPER_INFO_SIZE;
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
	btrfs_set_stack_chunk_stripe_len(chunk, BTRFS_STRIPE_LEN);
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
		const char *fslabel, int progress, u64 features)
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
	struct btrfs_mkfs_config mkfs_cfg;

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

	blocksize = cctx.blocksize;
	total_bytes = (u64)blocksize * (u64)cctx.block_count;
	if (blocksize < 4096) {
		error("block size is too small: %u < 4096", blocksize);
		goto fail;
	}
	if (btrfs_check_nodesize(nodesize, blocksize, features))
		goto fail;
	fd = open(devname, O_RDWR);
	if (fd < 0) {
		error("unable to open %s: %s", devname, strerror(errno));
		goto fail;
	}
	btrfs_parse_features_to_string(features_buf, features);
	if (features == BTRFS_MKFS_DEFAULT_FEATURES)
		strcat(features_buf, " (default)");

	printf("create btrfs filesystem:\n");
	printf("\tblocksize: %u\n", blocksize);
	printf("\tnodesize:  %u\n", nodesize);
	printf("\tfeatures:  %s\n", features_buf);

	memset(&mkfs_cfg, 0, sizeof(mkfs_cfg));
	mkfs_cfg.label = cctx.volume_name;
	mkfs_cfg.num_bytes = total_bytes;
	mkfs_cfg.nodesize = nodesize;
	mkfs_cfg.sectorsize = blocksize;
	mkfs_cfg.stripesize = blocksize;
	mkfs_cfg.features = features;

	ret = make_convert_btrfs(fd, &mkfs_cfg, &cctx);
	if (ret) {
		error("unable to create initial ctree: %s", strerror(-ret));
		goto fail;
	}

	root = open_ctree_fd(fd, devname, mkfs_cfg.super_bytenr,
			     OPEN_CTREE_WRITES | OPEN_CTREE_FS_PARTIAL);
	if (!root) {
		error("unable to open ctree");
		goto fail;
	}
	ret = init_btrfs(&mkfs_cfg, root, &cctx, convert_flags);
	if (ret) {
		error("unable to setup the root tree: %d", ret);
		goto fail;
	}

	printf("creating %s image file\n", cctx.convert_ops->name);
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

	printf("creating btrfs metadata");
	ctx.max_copy_inodes = (cctx.inodes_count - cctx.free_inodes_count);
	ctx.cur_copy_inodes = 0;

	if (progress) {
		ctx.info = task_init(print_copied_inodes, after_copied_inodes,
				     &ctx);
		task_start(ctx.info);
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

	image_root = link_subvol(root, subvol_name, CONV_IMAGE_SUBVOL_OBJECTID);
	if (!image_root) {
		error("unable to link subvolume %s", subvol_name);
		goto fail;
	}

	memset(root->fs_info->super_copy->label, 0, BTRFS_LABEL_SIZE);
	if (convert_flags & CONVERT_FLAG_COPY_LABEL) {
		__strncpy_null(root->fs_info->super_copy->label,
				cctx.volume_name, BTRFS_LABEL_SIZE - 1);
		printf("copy label '%s'\n", root->fs_info->super_copy->label);
	} else if (convert_flags & CONVERT_FLAG_SET_LABEL) {
		strcpy(root->fs_info->super_copy->label, fslabel);
		printf("set label to '%s'\n", fslabel);
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

	root = open_ctree_fd(fd, devname, 0,
			OPEN_CTREE_WRITES | OPEN_CTREE_FS_PARTIAL);
	if (!root) {
		error("unable to open ctree for finalization");
		goto fail;
	}
	root->fs_info->finalize_on_close = 1;
	close_ctree(root);
	close(fd);

	printf("conversion complete");
	return 0;
fail:
	clean_convert_context(&cctx);
	if (fd != -1)
		close(fd);
	warning(
"an error occurred during conversion, filesystem is partially created but not finalized and not mountable");
	return -1;
}

/*
 * Check if a non 1:1 mapped chunk can be rolled back.
 * For new convert, it's OK while for old convert it's not.
 */
static int may_rollback_chunk(struct btrfs_fs_info *fs_info, u64 bytenr)
{
	struct btrfs_block_group_cache *bg;
	struct btrfs_key key;
	struct btrfs_path path;
	struct btrfs_root *extent_root = fs_info->extent_root;
	u64 bg_start;
	u64 bg_end;
	int ret;

	bg = btrfs_lookup_first_block_group(fs_info, bytenr);
	if (!bg)
		return -ENOENT;
	bg_start = bg->key.objectid;
	bg_end = bg->key.objectid + bg->key.offset;

	key.objectid = bg_end;
	key.type = BTRFS_METADATA_ITEM_KEY;
	key.offset = 0;
	btrfs_init_path(&path);

	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

	while (1) {
		struct btrfs_extent_item *ei;

		ret = btrfs_previous_extent_item(extent_root, &path, bg_start);
		if (ret > 0) {
			ret = 0;
			break;
		}
		if (ret < 0)
			break;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.type == BTRFS_METADATA_ITEM_KEY)
			continue;
		/* Now it's EXTENT_ITEM_KEY only */
		ei = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_extent_item);
		/*
		 * Found data extent, means this is old convert must follow 1:1
		 * mapping.
		 */
		if (btrfs_extent_flags(path.nodes[0], ei)
				& BTRFS_EXTENT_FLAG_DATA) {
			ret = -EINVAL;
			break;
		}
	}
	btrfs_release_path(&path);
	return ret;
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
		if (ret) {
			if (ret == -ENOENT) {
				/* removed block group at the tail */
				if (length == (u64)-1)
					break;

				/* removed block group in the middle */
				goto next;
			}
			goto fail;
		}

		num_stripes = multi->num_stripes;
		physical = multi->stripes[0].physical;
		free(multi);

		if (num_stripes != 1) {
			error("num stripes for bytenr %llu is not 1", bytenr);
			goto fail;
		}

		/*
		 * Extra check for new convert, as metadata chunk from new
		 * convert is much more free than old convert, it doesn't need
		 * to do 1:1 mapping.
		 */
		if (physical != bytenr) {
			/*
			 * Check if it's a metadata chunk and has only metadata
			 * extent.
			 */
			ret = may_rollback_chunk(info, bytenr);
			if (ret < 0)
				goto fail;
		}
next:
		bytenr += length;
		if (bytenr >= total_bytes)
			break;
	}
	return 0;
fail:
	return -1;
}

static int do_rollback(const char *devname)
{
	int fd = -1;
	int ret;
	int i;
	struct btrfs_root *root;
	struct btrfs_root *image_root;
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
		error("unable to open %s: %s", devname, strerror(errno));
		goto fail;
	}
	root = open_ctree_fd(fd, devname, 0, OPEN_CTREE_WRITES);
	if (!root) {
		error("unable to open ctree");
		goto fail;
	}
	ret = may_rollback(root);
	if (ret < 0) {
		error("unable to do rollback: %d", ret);
		goto fail;
	}

	sectorsize = root->sectorsize;
	buf = malloc(sectorsize);
	if (!buf) {
		error("unable to allocate memory");
		goto fail;
	}

	btrfs_init_path(&path);

	key.objectid = CONV_IMAGE_SUBVOL_OBJECTID;
	key.type = BTRFS_ROOT_BACKREF_KEY;
	key.offset = BTRFS_FS_TREE_OBJECTID;
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root, &key, &path, 0,
				0);
	btrfs_release_path(&path);
	if (ret > 0) {
		error("unable to convert ext2 image subvolume, is it deleted?");
		goto fail;
	} else if (ret < 0) {
		error("unable to open ext2_saved, id %llu: %s",
			(unsigned long long)key.objectid, strerror(-ret));
		goto fail;
	}

	key.objectid = CONV_IMAGE_SUBVOL_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	image_root = btrfs_read_fs_root(root->fs_info, &key);
	if (!image_root || IS_ERR(image_root)) {
		error("unable to open subvolume %llu: %ld",
			(unsigned long long)key.objectid, PTR_ERR(image_root));
		goto fail;
	}

	name = "image";
	root_dir = btrfs_root_dirid(&root->root_item);
	dir = btrfs_lookup_dir_item(NULL, image_root, &path,
				   root_dir, name, strlen(name), 0);
	if (!dir || IS_ERR(dir)) {
		error("unable to find file %s: %ld", name, PTR_ERR(dir));
		goto fail;
	}
	leaf = path.nodes[0];
	btrfs_dir_item_key_to_cpu(leaf, dir, &key);
	btrfs_release_path(&path);

	objectid = key.objectid;

	ret = btrfs_lookup_inode(NULL, image_root, &path, &key, 0);
	if (ret) {
		error("unable to find inode item: %d", ret);
		goto fail;
	}
	leaf = path.nodes[0];
	inode = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_inode_item);
	total_bytes = btrfs_inode_size(leaf, inode);
	btrfs_release_path(&path);

	key.objectid = objectid;
	key.offset = 0;
	key.type = BTRFS_EXTENT_DATA_KEY;
	ret = btrfs_search_slot(NULL, image_root, &key, &path, 0, 0);
	if (ret != 0) {
		error("unable to find first file extent: %d", ret);
		btrfs_release_path(&path);
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
		    key.type != BTRFS_EXTENT_DATA_KEY)
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
		cache2 = btrfs_lookup_block_group(root->fs_info,
						  offset + num_bytes - 1);
		/*
		 * Here we must take consideration of old and new convert
		 * behavior.
		 * For old convert case, sign, there is no consist chunk type
		 * that will cover the extent. META/DATA/SYS are all possible.
		 * Just ensure relocate one is in SYS chunk.
		 * For new convert case, they are all covered by DATA chunk.
		 *
		 * So, there is not valid chunk type check for it now.
		 */
		if (cache1 != cache2)
			break;

		set_extent_bits(&io_tree, offset, offset + num_bytes - 1,
				EXTENT_LOCKED);
		set_state_private(&io_tree, offset, bytenr);
next_extent:
		offset += btrfs_file_extent_num_bytes(leaf, fi);
		path.slots[0]++;
	}
	btrfs_release_path(&path);

	if (offset < total_bytes) {
		error("unable to build extent mapping (offset %llu, total_bytes %llu)",
				(unsigned long long)offset,
				(unsigned long long)total_bytes);
		error("converted filesystem after balance is unable to rollback");
		goto fail;
	}

	first_free = BTRFS_SUPER_INFO_OFFSET + 2 * sectorsize - 1;
	first_free &= ~((u64)sectorsize - 1);
	/* backup for extent #0 should exist */
	if(!test_range_bit(&io_tree, 0, first_free - 1, EXTENT_LOCKED, 1)) {
		error("no backup for the first extent");
		goto fail;
	}
	/* force no allocation from system block group */
	root->fs_info->system_allocs = -1;
	trans = btrfs_start_transaction(root, 1);
	if (!trans) {
		error("unable to start transaction");
		goto fail;
	}
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
		btrfs_release_path(&path);
	}
	btrfs_release_path(&path);

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
		error(
	"unable to empty system block group (num_bytes %llu, first_free %llu",
				(unsigned long long)num_bytes,
				(unsigned long long)first_free);
		goto fail;
	}
	/* create a system chunk that maps the whole device */
	ret = prepare_system_chunk_sb(root->fs_info->super_copy);
	if (ret) {
		error("unable to update system chunk: %d", ret);
		goto fail;
	}

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		error("transaction commit failed: %d", ret);
		goto fail;
	}

	ret = close_ctree(root);
	if (ret) {
		error("close_ctree failed: %d", ret);
		goto fail;
	}

	/* zero btrfs super block mirrors */
	memset(buf, 0, sectorsize);
	for (i = 1 ; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		if (bytenr >= total_bytes)
			break;
		ret = pwrite(fd, buf, sectorsize, bytenr);
		if (ret != sectorsize) {
			error("zeroing superblock mirror %d failed: %d",
					i, ret);
			goto fail;
		}
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
				error("reading superblock at %llu failed: %d",
						(unsigned long long)bytenr, ret);
				goto fail;
			}
			BUG_ON(ret != sectorsize);
			ret = pwrite(fd, buf, sectorsize, start);
			if (ret < 0) {
				error("writing superblock at %llu failed: %d",
						(unsigned long long)start, ret);
				goto fail;
			}
			BUG_ON(ret != sectorsize);
next_sector:
			start += sectorsize;
			bytenr += sectorsize;
		}
	}

	ret = fsync(fd);
	if (ret < 0) {
		error("fsync failed: %s", strerror(errno));
		goto fail;
	}
	/*
	 * finally, overwrite btrfs super block.
	 */
	ret = pread(fd, buf, sectorsize, sb_bytenr);
	if (ret < 0) {
		error("reading primary superblock failed: %s",
				strerror(errno));
		goto fail;
	}
	BUG_ON(ret != sectorsize);
	ret = pwrite(fd, buf, sectorsize, BTRFS_SUPER_INFO_OFFSET);
	if (ret < 0) {
		error("writing primary superblock failed: %s",
				strerror(errno));
		goto fail;
	}
	BUG_ON(ret != sectorsize);
	ret = fsync(fd);
	if (ret < 0) {
		error("fsync failed: %s", strerror(errno));
		goto fail;
	}

	close(fd);
	free(buf);
	extent_io_tree_cleanup(&io_tree);
	printf("rollback complete\n");
	return 0;

fail:
	if (fd != -1)
		close(fd);
	free(buf);
	error("rollback aborted");
	return -1;
}

static void print_usage(void)
{
	printf("usage: btrfs-convert [options] device\n");
	printf("options:\n");
	printf("\t-d|--no-datasum        disable data checksum, sets NODATASUM\n");
	printf("\t-i|--no-xattr          ignore xattrs and ACLs\n");
	printf("\t-n|--no-inline         disable inlining of small files to metadata\n");
	printf("\t-N|--nodesize SIZE     set filesystem metadata nodesize\n");
	printf("\t-r|--rollback          roll back to the original filesystem\n");
	printf("\t-l|--label LABEL       set filesystem label\n");
	printf("\t-L|--copy-label        use label from converted filesystem\n");
	printf("\t-p|--progress          show converting progress (default)\n");
	printf("\t-O|--features LIST     comma separated list of filesystem features\n");
	printf("\t--no-progress          show only overview, not the detailed progress\n");
	printf("\n");
	printf("Supported filesystems:\n");
	printf("\text2/3/4: %s\n", BTRFSCONVERT_EXT2 ? "yes" : "no");
}

int main(int argc, char *argv[])
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
	char fslabel[BTRFS_LABEL_SIZE];
	u64 features = BTRFS_MKFS_DEFAULT_FEATURES;

	while(1) {
		enum { GETOPT_VAL_NO_PROGRESS = 256 };
		static const struct option long_options[] = {
			{ "no-progress", no_argument, NULL,
				GETOPT_VAL_NO_PROGRESS },
			{ "no-datasum", no_argument, NULL, 'd' },
			{ "no-inline", no_argument, NULL, 'n' },
			{ "no-xattr", no_argument, NULL, 'i' },
			{ "rollback", no_argument, NULL, 'r' },
			{ "features", required_argument, NULL, 'O' },
			{ "progress", no_argument, NULL, 'p' },
			{ "label", required_argument, NULL, 'l' },
			{ "copy-label", no_argument, NULL, 'L' },
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
				nodesize = parse_size(optarg);
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

					btrfs_parse_features_to_string(buf,
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
		error("could not check mount status: %s", strerror(-ret));
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
		cf |= copylabel;
		ret = do_convert(file, cf, nodesize, fslabel, progress, features);
	}
	if (ret)
		return 1;
	return 0;
}
