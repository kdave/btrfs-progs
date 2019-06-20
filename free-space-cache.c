/*
 * Copyright (C) 2008 Red Hat.  All rights reserved.
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
#include "ctree.h"
#include "free-space-cache.h"
#include "transaction.h"
#include "disk-io.h"
#include "extent_io.h"
#include "kernel-lib/crc32c.h"
#include "kernel-lib/bitops.h"
#include "common/internal.h"
#include "common/utils.h"

/*
 * Kernel always uses PAGE_CACHE_SIZE for sectorsize, but we don't have
 * anything like that in userspace and have to get the value from the
 * filesystem
 */
#define BITS_PER_BITMAP(sectorsize)		((sectorsize) * 8)
#define MAX_CACHE_BYTES_PER_GIG	SZ_32K

static int link_free_space(struct btrfs_free_space_ctl *ctl,
			   struct btrfs_free_space *info);
static void merge_space_tree(struct btrfs_free_space_ctl *ctl);

struct io_ctl {
	void *cur, *orig;
	void *buffer;
	struct btrfs_root *root;
	unsigned long size;
	u64 total_size;
	int index;
	int num_pages;
	unsigned check_crcs:1;
};

static int io_ctl_init(struct io_ctl *io_ctl, u64 size, u64 ino,
		       struct btrfs_root *root)
{
	memset(io_ctl, 0, sizeof(struct io_ctl));
	io_ctl->num_pages = DIV_ROUND_UP(size, root->fs_info->sectorsize);
	io_ctl->buffer = kzalloc(size, GFP_NOFS);
	if (!io_ctl->buffer)
		return -ENOMEM;
	io_ctl->total_size = size;
	io_ctl->root = root;
	if (ino != BTRFS_FREE_INO_OBJECTID)
		io_ctl->check_crcs = 1;
	return 0;
}

static void io_ctl_free(struct io_ctl *io_ctl)
{
	kfree(io_ctl->buffer);
}

static void io_ctl_unmap_page(struct io_ctl *io_ctl)
{
	if (io_ctl->cur) {
		io_ctl->cur = NULL;
		io_ctl->orig = NULL;
	}
}

static void io_ctl_map_page(struct io_ctl *io_ctl, int clear)
{
	BUG_ON(io_ctl->index >= io_ctl->num_pages);
	io_ctl->cur = io_ctl->buffer + (io_ctl->index++ *
					io_ctl->root->fs_info->sectorsize);
	io_ctl->orig = io_ctl->cur;
	io_ctl->size = io_ctl->root->fs_info->sectorsize;
	if (clear)
		memset(io_ctl->cur, 0, io_ctl->root->fs_info->sectorsize);
}

static void io_ctl_drop_pages(struct io_ctl *io_ctl)
{
	io_ctl_unmap_page(io_ctl);
}

static int io_ctl_prepare_pages(struct io_ctl *io_ctl, struct btrfs_root *root,
				struct btrfs_path *path, u64 ino)
{
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key key;
	u64 bytenr, len;
	u64 total_read = 0;
	int ret = 0;

	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret) {
		fprintf(stderr,
		       "Couldn't find file extent item for free space inode"
		       " %Lu\n", ino);
		btrfs_release_path(path);
		return -EINVAL;
	}

	while (total_read < io_ctl->total_size) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(root, path);
			if (ret) {
				ret = -EINVAL;
				break;
			}
		}
		leaf = path->nodes[0];

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != ino) {
			ret = -EINVAL;
			break;
		}

		if (key.type != BTRFS_EXTENT_DATA_KEY) {
			ret = -EINVAL;
			break;
		}

		fi = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(path->nodes[0], fi) !=
		    BTRFS_FILE_EXTENT_REG) {
			fprintf(stderr, "Not the file extent type we wanted\n");
			ret = -EINVAL;
			break;
		}

		bytenr = btrfs_file_extent_disk_bytenr(leaf, fi) +
			btrfs_file_extent_offset(leaf, fi);
		len = btrfs_file_extent_num_bytes(leaf, fi);
		ret = read_data_from_disk(root->fs_info,
					  io_ctl->buffer + key.offset, bytenr,
					  len, 0);
		if (ret)
			break;
		total_read += len;
		path->slots[0]++;
	}

	btrfs_release_path(path);
	return ret;
}

static int io_ctl_check_generation(struct io_ctl *io_ctl, u64 generation)
{
	__le64 *gen;

	/*
	 * Skip the crc area.  If we don't check crcs then we just have a 64bit
	 * chunk at the front of the first page.
	 */
	if (io_ctl->check_crcs) {
		io_ctl->cur += sizeof(u32) * io_ctl->num_pages;
		io_ctl->size -= sizeof(u64) +
			(sizeof(u32) * io_ctl->num_pages);
	} else {
		io_ctl->cur += sizeof(u64);
		io_ctl->size -= sizeof(u64) * 2;
	}

	gen = io_ctl->cur;
	if (le64_to_cpu(*gen) != generation) {
		printk("btrfs: space cache generation "
		       "(%Lu) does not match inode (%Lu)\n", *gen,
		       generation);
		io_ctl_unmap_page(io_ctl);
		return -EIO;
	}
	io_ctl->cur += sizeof(u64);
	return 0;
}

static int io_ctl_check_crc(struct io_ctl *io_ctl, int index)
{
	u32 *tmp, val;
	u32 crc = ~(u32)0;
	unsigned offset = 0;

	if (!io_ctl->check_crcs) {
		io_ctl_map_page(io_ctl, 0);
		return 0;
	}

	if (index == 0)
		offset = sizeof(u32) * io_ctl->num_pages;

	tmp = io_ctl->buffer;
	tmp += index;
	val = *tmp;

	io_ctl_map_page(io_ctl, 0);
	crc = crc32c(crc, io_ctl->orig + offset,
			io_ctl->root->fs_info->sectorsize - offset);
	btrfs_csum_final(crc, (u8 *)&crc);
	if (val != crc) {
		printk("btrfs: csum mismatch on free space cache\n");
		io_ctl_unmap_page(io_ctl);
		return -EIO;
	}

	return 0;
}

static int io_ctl_read_entry(struct io_ctl *io_ctl,
			    struct btrfs_free_space *entry, u8 *type)
{
	struct btrfs_free_space_entry *e;
	int ret;

	if (!io_ctl->cur) {
		ret = io_ctl_check_crc(io_ctl, io_ctl->index);
		if (ret)
			return ret;
	}

	e = io_ctl->cur;
	entry->offset = le64_to_cpu(e->offset);
	entry->bytes = le64_to_cpu(e->bytes);
	*type = e->type;
	io_ctl->cur += sizeof(struct btrfs_free_space_entry);
	io_ctl->size -= sizeof(struct btrfs_free_space_entry);

	if (io_ctl->size >= sizeof(struct btrfs_free_space_entry))
		return 0;

	io_ctl_unmap_page(io_ctl);

	return 0;
}

static int io_ctl_read_bitmap(struct io_ctl *io_ctl,
			      struct btrfs_free_space *entry)
{
	int ret;

	ret = io_ctl_check_crc(io_ctl, io_ctl->index);
	if (ret)
		return ret;

	memcpy(entry->bitmap, io_ctl->cur, io_ctl->root->fs_info->sectorsize);
	io_ctl_unmap_page(io_ctl);

	return 0;
}


static int __load_free_space_cache(struct btrfs_root *root,
			    struct btrfs_free_space_ctl *ctl,
			    struct btrfs_path *path, u64 offset)
{
	struct btrfs_free_space_header *header;
	struct btrfs_inode_item *inode_item;
	struct extent_buffer *leaf;
	struct io_ctl io_ctl;
	struct btrfs_key key;
	struct btrfs_key inode_location;
	struct btrfs_disk_key disk_key;
	struct btrfs_free_space *e, *n;
	struct list_head bitmaps;
	u64 num_entries;
	u64 num_bitmaps;
	u64 generation;
	u64 inode_size;
	u8 type;
	int ret = 0;

	INIT_LIST_HEAD(&bitmaps);

	key.objectid = BTRFS_FREE_SPACE_OBJECTID;
	key.offset = offset;
	key.type = 0;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0) {
		return 0;
	} else if (ret > 0) {
		btrfs_release_path(path);
		return 0;
	}

	leaf = path->nodes[0];
	header = btrfs_item_ptr(leaf, path->slots[0],
				struct btrfs_free_space_header);
	num_entries = btrfs_free_space_entries(leaf, header);
	num_bitmaps = btrfs_free_space_bitmaps(leaf, header);
	generation = btrfs_free_space_generation(leaf, header);
	btrfs_free_space_key(leaf, header, &disk_key);
	btrfs_disk_key_to_cpu(&inode_location, &disk_key);
	btrfs_release_path(path);

	ret = btrfs_search_slot(NULL, root, &inode_location, path, 0, 0);
	if (ret) {
		fprintf(stderr, "Couldn't find free space inode %d\n", ret);
		return 0;
	}

	leaf = path->nodes[0];
	inode_item = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_inode_item);

	inode_size = btrfs_inode_size(leaf, inode_item);
	if (!inode_size || !btrfs_inode_generation(leaf, inode_item)) {
		btrfs_release_path(path);
		return 0;
	}

	if (btrfs_inode_generation(leaf, inode_item) != generation) {
		fprintf(stderr,
		       "free space inode generation (%llu) did not match "
		       "free space cache generation (%llu)\n",
		       (unsigned long long)btrfs_inode_generation(leaf,
								  inode_item),
		       (unsigned long long)generation);
		btrfs_release_path(path);
		return 0;
	}

	btrfs_release_path(path);

	if (!num_entries)
		return 0;

	ret = io_ctl_init(&io_ctl, inode_size, inode_location.objectid, root);
	if (ret)
		return ret;

	ret = io_ctl_prepare_pages(&io_ctl, root, path,
				   inode_location.objectid);
	if (ret)
		goto out;

	ret = io_ctl_check_crc(&io_ctl, 0);
	if (ret)
		goto free_cache;

	ret = io_ctl_check_generation(&io_ctl, generation);
	if (ret)
		goto free_cache;

	while (num_entries) {
		e = calloc(1, sizeof(*e));
		if (!e)
			goto free_cache;

		ret = io_ctl_read_entry(&io_ctl, e, &type);
		if (ret) {
			free(e);
			goto free_cache;
		}

		if (!e->bytes) {
			free(e);
			goto free_cache;
		}

		if (type == BTRFS_FREE_SPACE_EXTENT) {
			ret = link_free_space(ctl, e);
			if (ret) {
				fprintf(stderr,
				       "Duplicate entries in free space cache\n");
				free(e);
				goto free_cache;
			}
		} else {
			BUG_ON(!num_bitmaps);
			num_bitmaps--;
			e->bitmap = kzalloc(ctl->sectorsize, GFP_NOFS);
			if (!e->bitmap) {
				free(e);
				goto free_cache;
			}
			ret = link_free_space(ctl, e);
			ctl->total_bitmaps++;
			if (ret) {
				fprintf(stderr,
				       "Duplicate entries in free space cache\n");
				free(e->bitmap);
				free(e);
				goto free_cache;
			}
			list_add_tail(&e->list, &bitmaps);
		}

		num_entries--;
	}

	io_ctl_unmap_page(&io_ctl);

	/*
	 * We add the bitmaps at the end of the entries in order that
	 * the bitmap entries are added to the cache.
	 */
	list_for_each_entry_safe(e, n, &bitmaps, list) {
		list_del_init(&e->list);
		ret = io_ctl_read_bitmap(&io_ctl, e);
		if (ret)
			goto free_cache;
	}

	io_ctl_drop_pages(&io_ctl);
	merge_space_tree(ctl);
	ret = 1;
out:
	io_ctl_free(&io_ctl);
	return ret;
free_cache:
	io_ctl_drop_pages(&io_ctl);
	__btrfs_remove_free_space_cache(ctl);
	goto out;
}

int load_free_space_cache(struct btrfs_fs_info *fs_info,
			  struct btrfs_block_group_cache *block_group)
{
	struct btrfs_free_space_ctl *ctl = block_group->free_space_ctl;
	struct btrfs_path *path;
	u64 used = btrfs_block_group_used(&block_group->item);
	int ret = 0;
	u64 bg_free;
	s64 diff;

	path = btrfs_alloc_path();
	if (!path)
		return 0;

	ret = __load_free_space_cache(fs_info->tree_root, ctl, path,
				      block_group->key.objectid);
	btrfs_free_path(path);

	bg_free = block_group->key.offset - used - block_group->bytes_super;
	diff = ctl->free_space - bg_free;
	if (ret == 1 && diff) {
		fprintf(stderr,
		       "block group %llu has wrong amount of free space, free space cache has %llu block group has %llu\n",
		       block_group->key.objectid, ctl->free_space, bg_free);
		__btrfs_remove_free_space_cache(ctl);
		/*
		 * Due to btrfs_reserve_extent() can happen out of a
		 * transaction, but all btrfs_release_extent() happens inside
		 * a transaction, so under heavy race it's possible that free
		 * space cache has less free space, and both kernel just discard
		 * such cache. But if we find some case where free space cache
		 * has more free space, this means under certain case such
		 * cache can be loaded and cause double allocate.
		 *
		 * Detect such possibility here.
		 */
		if (diff > 0)
			error(
"free space cache has more free space than block group item, this could leads to serious corruption, please contact btrfs developers");
		ret = -1;
	}

	if (ret < 0) {
		if (diff <= 0)
			ret = 0;

		fprintf(stderr,
		       "failed to load free space cache for block group %llu\n",
		       block_group->key.objectid);
	}

	return ret;
}

static inline unsigned long offset_to_bit(u64 bitmap_start, u32 unit,
					  u64 offset)
{
	BUG_ON(offset < bitmap_start);
	offset -= bitmap_start;
	return (unsigned long)(offset / unit);
}

static inline unsigned long bytes_to_bits(u64 bytes, u32 unit)
{
	return (unsigned long)(bytes / unit);
}

static int tree_insert_offset(struct rb_root *root, u64 offset,
			      struct rb_node *node, int bitmap)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct btrfs_free_space *info;

	while (*p) {
		parent = *p;
		info = rb_entry(parent, struct btrfs_free_space, offset_index);

		if (offset < info->offset) {
			p = &(*p)->rb_left;
		} else if (offset > info->offset) {
			p = &(*p)->rb_right;
		} else {
			/*
			 * we could have a bitmap entry and an extent entry
			 * share the same offset.  If this is the case, we want
			 * the extent entry to always be found first if we do a
			 * linear search through the tree, since we want to have
			 * the quickest allocation time, and allocating from an
			 * extent is faster than allocating from a bitmap.  So
			 * if we're inserting a bitmap and we find an entry at
			 * this offset, we want to go right, or after this entry
			 * logically.  If we are inserting an extent and we've
			 * found a bitmap, we want to go left, or before
			 * logically.
			 */
			if (bitmap) {
				if (info->bitmap)
					return -EEXIST;
				p = &(*p)->rb_right;
			} else {
				if (!info->bitmap)
					return -EEXIST;
				p = &(*p)->rb_left;
			}
		}
	}

	rb_link_node(node, parent, p);
	rb_insert_color(node, root);

	return 0;
}

/*
 * searches the tree for the given offset.
 *
 * fuzzy - If this is set, then we are trying to make an allocation, and we just
 * want a section that has at least bytes size and comes at or after the given
 * offset.
 */
static struct btrfs_free_space *
tree_search_offset(struct btrfs_free_space_ctl *ctl,
		   u64 offset, int bitmap_only, int fuzzy)
{
	struct rb_node *n = ctl->free_space_offset.rb_node;
	struct btrfs_free_space *entry, *prev = NULL;
	u32 sectorsize = ctl->sectorsize;

	/* find entry that is closest to the 'offset' */
	while (1) {
		if (!n) {
			entry = NULL;
			break;
		}

		entry = rb_entry(n, struct btrfs_free_space, offset_index);
		prev = entry;

		if (offset < entry->offset)
			n = n->rb_left;
		else if (offset > entry->offset)
			n = n->rb_right;
		else
			break;
	}

	if (bitmap_only) {
		if (!entry)
			return NULL;
		if (entry->bitmap)
			return entry;

		/*
		 * bitmap entry and extent entry may share same offset,
		 * in that case, bitmap entry comes after extent entry.
		 */
		n = rb_next(n);
		if (!n)
			return NULL;
		entry = rb_entry(n, struct btrfs_free_space, offset_index);
		if (entry->offset != offset)
			return NULL;

		WARN_ON(!entry->bitmap);
		return entry;
	} else if (entry) {
		if (entry->bitmap) {
			/*
			 * if previous extent entry covers the offset,
			 * we should return it instead of the bitmap entry
			 */
			n = rb_prev(&entry->offset_index);
			if (n) {
				prev = rb_entry(n, struct btrfs_free_space,
						offset_index);
				if (!prev->bitmap &&
				    prev->offset + prev->bytes > offset)
					entry = prev;
			}
		}
		return entry;
	}

	if (!prev)
		return NULL;

	/* find last entry before the 'offset' */
	entry = prev;
	if (entry->offset > offset) {
		n = rb_prev(&entry->offset_index);
		if (n) {
			entry = rb_entry(n, struct btrfs_free_space,
					offset_index);
			BUG_ON(entry->offset > offset);
		} else {
			if (fuzzy)
				return entry;
			else
				return NULL;
		}
	}

	if (entry->bitmap) {
		n = rb_prev(&entry->offset_index);
		if (n) {
			prev = rb_entry(n, struct btrfs_free_space,
					offset_index);
			if (!prev->bitmap &&
			    prev->offset + prev->bytes > offset)
				return prev;
		}
		if (entry->offset + BITS_PER_BITMAP(sectorsize) * ctl->unit > offset)
			return entry;
	} else if (entry->offset + entry->bytes > offset)
		return entry;

	if (!fuzzy)
		return NULL;

	while (1) {
		if (entry->bitmap) {
			if (entry->offset + BITS_PER_BITMAP(sectorsize) *
			    ctl->unit > offset)
				break;
		} else {
			if (entry->offset + entry->bytes > offset)
				break;
		}

		n = rb_next(&entry->offset_index);
		if (!n)
			return NULL;
		entry = rb_entry(n, struct btrfs_free_space, offset_index);
	}
	return entry;
}

void unlink_free_space(struct btrfs_free_space_ctl *ctl,
		       struct btrfs_free_space *info)
{
	rb_erase(&info->offset_index, &ctl->free_space_offset);
	ctl->free_extents--;
	ctl->free_space -= info->bytes;
}

static int link_free_space(struct btrfs_free_space_ctl *ctl,
			   struct btrfs_free_space *info)
{
	int ret = 0;

	BUG_ON(!info->bitmap && !info->bytes);
	ret = tree_insert_offset(&ctl->free_space_offset, info->offset,
				 &info->offset_index, (info->bitmap != NULL));
	if (ret)
		return ret;

	ctl->free_space += info->bytes;
	ctl->free_extents++;
	return ret;
}

static int search_bitmap(struct btrfs_free_space_ctl *ctl,
			 struct btrfs_free_space *bitmap_info, u64 *offset,
			 u64 *bytes)
{
	unsigned long found_bits = 0;
	unsigned long bits, i;
	unsigned long next_zero;
	u32 sectorsize = ctl->sectorsize;

	i = offset_to_bit(bitmap_info->offset, ctl->unit,
			  max_t(u64, *offset, bitmap_info->offset));
	bits = bytes_to_bits(*bytes, ctl->unit);

	for_each_set_bit_from(i, bitmap_info->bitmap, BITS_PER_BITMAP(sectorsize)) {
		next_zero = find_next_zero_bit(bitmap_info->bitmap,
					       BITS_PER_BITMAP(sectorsize), i);
		if ((next_zero - i) >= bits) {
			found_bits = next_zero - i;
			break;
		}
		i = next_zero;
	}

	if (found_bits) {
		*offset = (u64)(i * ctl->unit) + bitmap_info->offset;
		*bytes = (u64)(found_bits) * ctl->unit;
		return 0;
	}

	return -1;
}

struct btrfs_free_space *
btrfs_find_free_space(struct btrfs_free_space_ctl *ctl, u64 offset, u64 bytes)
{
	return tree_search_offset(ctl, offset, 0, 0);
}

static void try_merge_free_space(struct btrfs_free_space_ctl *ctl,
				struct btrfs_free_space *info)
{
	struct btrfs_free_space *left_info;
	struct btrfs_free_space *right_info;
	u64 offset = info->offset;
	u64 bytes = info->bytes;

	/*
	 * first we want to see if there is free space adjacent to the range we
	 * are adding, if there is remove that struct and add a new one to
	 * cover the entire range
	 */
	right_info = tree_search_offset(ctl, offset + bytes, 0, 0);
	if (right_info && rb_prev(&right_info->offset_index))
		left_info = rb_entry(rb_prev(&right_info->offset_index),
				     struct btrfs_free_space, offset_index);
	else
		left_info = tree_search_offset(ctl, offset - 1, 0, 0);

	if (right_info && !right_info->bitmap) {
		unlink_free_space(ctl, right_info);
		info->bytes += right_info->bytes;
		free(right_info);
	}

	if (left_info && !left_info->bitmap &&
	    left_info->offset + left_info->bytes == offset) {
		unlink_free_space(ctl, left_info);
		info->offset = left_info->offset;
		info->bytes += left_info->bytes;
		free(left_info);
	}
}

void btrfs_dump_free_space(struct btrfs_block_group_cache *block_group,
			   u64 bytes)
{
	struct btrfs_free_space_ctl *ctl = block_group->free_space_ctl;
	struct btrfs_free_space *info;
	struct rb_node *n;
	int count = 0;

	for (n = rb_first(&ctl->free_space_offset); n; n = rb_next(n)) {
		info = rb_entry(n, struct btrfs_free_space, offset_index);
		if (info->bytes >= bytes && !block_group->ro)
			count++;
		printk("entry offset %llu, bytes %llu, bitmap %s\n",
		       (unsigned long long)info->offset,
		       (unsigned long long)info->bytes,
		       (info->bitmap) ? "yes" : "no");
	}
	printk("%d blocks of free space at or bigger than bytes is \n", count);
}

int btrfs_init_free_space_ctl(struct btrfs_block_group_cache *block_group,
			      int sectorsize)
{
	struct btrfs_free_space_ctl *ctl;

	ctl = calloc(1, sizeof(*ctl));
	if (!ctl)
		return -ENOMEM;

	ctl->sectorsize = sectorsize;
	ctl->unit = sectorsize;
	ctl->start = block_group->key.objectid;
	ctl->private = block_group;
	block_group->free_space_ctl = ctl;

	return 0;
}

void __btrfs_remove_free_space_cache(struct btrfs_free_space_ctl *ctl)
{
	struct btrfs_free_space *info;
	struct rb_node *node;

	while ((node = rb_last(&ctl->free_space_offset)) != NULL) {
		info = rb_entry(node, struct btrfs_free_space, offset_index);
		unlink_free_space(ctl, info);
		free(info->bitmap);
		free(info);
	}
}

void btrfs_remove_free_space_cache(struct btrfs_block_group_cache *block_group)
{
	__btrfs_remove_free_space_cache(block_group->free_space_ctl);
}

int btrfs_add_free_space(struct btrfs_free_space_ctl *ctl, u64 offset,
			 u64 bytes)
{
	struct btrfs_free_space *info;
	int ret = 0;

	info = calloc(1, sizeof(*info));
	if (!info)
		return -ENOMEM;

	info->offset = offset;
	info->bytes = bytes;

	try_merge_free_space(ctl, info);

	ret = link_free_space(ctl, info);
	if (ret)
		printk(KERN_CRIT "btrfs: unable to add free space :%d\n", ret);

	return ret;
}

/*
 * Merges all the free space cache and kills the bitmap entries since we just
 * want to use the free space cache to verify it's correct, no reason to keep
 * the bitmaps around to confuse things.
 */
static void merge_space_tree(struct btrfs_free_space_ctl *ctl)
{
	struct btrfs_free_space *e, *prev = NULL;
	struct rb_node *n;
	int ret;
	u32 sectorsize = ctl->sectorsize;

again:
	prev = NULL;
	for (n = rb_first(&ctl->free_space_offset); n; n = rb_next(n)) {
		e = rb_entry(n, struct btrfs_free_space, offset_index);
		if (e->bitmap) {
			u64 offset = e->offset, bytes = ctl->unit;
			u64 end;

			end = e->offset + (u64)(BITS_PER_BITMAP(sectorsize) * ctl->unit);

			unlink_free_space(ctl, e);
			while (!(search_bitmap(ctl, e, &offset, &bytes))) {
				ret = btrfs_add_free_space(ctl, offset,
							   bytes);
				BUG_ON(ret);
				offset += bytes;
				if (offset >= end)
					break;
				bytes = ctl->unit;
			}
			free(e->bitmap);
			free(e);
			goto again;
		}
		if (!prev)
			goto next;
		if (prev->offset + prev->bytes == e->offset) {
			unlink_free_space(ctl, prev);
			unlink_free_space(ctl, e);
			prev->bytes += e->bytes;
			free(e);
			link_free_space(ctl, prev);
			goto again;
		}
next:
		prev = e;
	}
}

int btrfs_clear_free_space_cache(struct btrfs_fs_info *fs_info,
				 struct btrfs_block_group_cache *bg)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_disk_key location;
	struct btrfs_free_space_header *sc_header;
	struct extent_buffer *node;
	u64 ino;
	int slot;
	int ret;

	trans = btrfs_start_transaction(tree_root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	btrfs_init_path(&path);

	key.objectid = BTRFS_FREE_SPACE_OBJECTID;
	key.type = 0;
	key.offset = bg->key.objectid;

	ret = btrfs_search_slot(trans, tree_root, &key, &path, -1, 1);
	if (ret > 0) {
		ret = 0;
		goto out;
	}
	if (ret < 0)
		goto out;

	node = path.nodes[0];
	slot = path.slots[0];
	sc_header = btrfs_item_ptr(node, slot, struct btrfs_free_space_header);
	btrfs_free_space_key(node, sc_header, &location);
	ino = btrfs_disk_key_objectid(&location);

	/* Delete the free space header, as we have the ino to continue */
	ret = btrfs_del_item(trans, tree_root, &path);
	if (ret < 0) {
		error("failed to remove free space header for block group %llu: %d",
		      bg->key.objectid, ret);
		goto out;
	}
	btrfs_release_path(&path);

	/* Iterate from the end of the free space cache inode */
	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = (u64)-1;
	ret = btrfs_search_slot(trans, tree_root, &key, &path, -1, 1);
	if (ret < 0) {
		error("failed to locate free space cache extent for block group %llu: %d",
		      bg->key.objectid, ret);
		goto out;
	}
	while (1) {
		struct btrfs_file_extent_item *fi;
		u64 disk_bytenr;
		u64 disk_num_bytes;

		ret = btrfs_previous_item(tree_root, &path, ino,
					  BTRFS_EXTENT_DATA_KEY);
		if (ret > 0) {
			ret = 0;
			break;
		}
		if (ret < 0) {
			error(
	"failed to locate free space cache extent for block group %llu: %d",
				bg->key.objectid, ret);
			goto out;
		}
		node = path.nodes[0];
		slot = path.slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);
		fi = btrfs_item_ptr(node, slot, struct btrfs_file_extent_item);
		disk_bytenr = btrfs_file_extent_disk_bytenr(node, fi);
		disk_num_bytes = btrfs_file_extent_disk_num_bytes(node, fi);

		ret = btrfs_free_extent(trans, tree_root, disk_bytenr,
					disk_num_bytes, 0, tree_root->objectid,
					ino, key.offset);
		if (ret < 0) {
			error("failed to remove backref for disk bytenr %llu: %d",
			      disk_bytenr, ret);
			goto out;
		}
		ret = btrfs_del_item(trans, tree_root, &path);
		if (ret < 0) {
			error(
	"failed to remove free space extent data for ino %llu offset %llu: %d",
			      ino, key.offset, ret);
			goto out;
		}
	}
	btrfs_release_path(&path);

	/* Now delete free space cache inode item */
	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(trans, tree_root, &key, &path, -1, 1);
	if (ret > 0)
		warning("free space inode %llu not found, ignore", ino);
	if (ret < 0) {
		error(
	"failed to locate free space cache inode %llu for block group %llu: %d",
		      ino, bg->key.objectid, ret);
		goto out;
	}
	ret = btrfs_del_item(trans, tree_root, &path);
	if (ret < 0) {
		error(
	"failed to delete free space cache inode %llu for block group %llu: %d",
		      ino, bg->key.objectid, ret);
	}
out:
	btrfs_release_path(&path);
	if (!ret)
		btrfs_commit_transaction(trans, tree_root);
	return ret;
}
