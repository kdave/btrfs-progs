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

#define _XOPEN_SOURCE 600
#define __USE_XOPEN2K
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "crc32c.h"
#include "utils.h"
#include "print-tree.h"

static int check_tree_block(struct btrfs_root *root, struct extent_buffer *buf)
{

	struct btrfs_fs_devices *fs_devices;
	int ret = 1;

	if (buf->start != btrfs_header_bytenr(buf))
		return ret;

	fs_devices = root->fs_info->fs_devices;
	while (fs_devices) {
		if (!memcmp_extent_buffer(buf, fs_devices->fsid,
					  (unsigned long)btrfs_header_fsid(buf),
					  BTRFS_FSID_SIZE)) {
			ret = 0;
			break;
		}
		fs_devices = fs_devices->seed;
	}
	return ret;
}

u32 btrfs_csum_data(struct btrfs_root *root, char *data, u32 seed, size_t len)
{
	return crc32c(seed, data, len);
}

void btrfs_csum_final(u32 crc, char *result)
{
	*(__le32 *)result = ~cpu_to_le32(crc);
}

int csum_tree_block_size(struct extent_buffer *buf, u16 csum_size,
			 int verify)
{
	char *result;
	u32 len;
	u32 crc = ~(u32)0;

	result = malloc(csum_size * sizeof(char));
	if (!result)
		return 1;

	len = buf->len - BTRFS_CSUM_SIZE;
	crc = crc32c(crc, buf->data + BTRFS_CSUM_SIZE, len);
	btrfs_csum_final(crc, result);

	if (verify) {
		if (memcmp_extent_buffer(buf, result, 0, csum_size)) {
			printk("checksum verify failed on %llu wanted %X "
			       "found %X\n", (unsigned long long)buf->start,
			       *((int *)result), *((int *)buf));
			free(result);
			return 1;
		}
	} else {
		write_extent_buffer(buf, result, 0, csum_size);
	}
	free(result);
	return 0;
}

int csum_tree_block(struct btrfs_root *root, struct extent_buffer *buf,
		    int verify)
{
	u16 csum_size =
		btrfs_super_csum_size(&root->fs_info->super_copy);
	return csum_tree_block_size(buf, csum_size, verify);
}

struct extent_buffer *btrfs_find_tree_block(struct btrfs_root *root,
					    u64 bytenr, u32 blocksize)
{
	return find_extent_buffer(&root->fs_info->extent_cache,
				  bytenr, blocksize);
}

struct extent_buffer *btrfs_find_create_tree_block(struct btrfs_root *root,
						 u64 bytenr, u32 blocksize)
{
	return alloc_extent_buffer(&root->fs_info->extent_cache, bytenr,
				   blocksize);
}

int readahead_tree_block(struct btrfs_root *root, u64 bytenr, u32 blocksize,
			 u64 parent_transid)
{
	int ret;
	int dev_nr;
	struct extent_buffer *eb;
	u64 length;
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;

	eb = btrfs_find_tree_block(root, bytenr, blocksize);
	if (eb && btrfs_buffer_uptodate(eb, parent_transid)) {
		free_extent_buffer(eb);
		return 0;
	}

	dev_nr = 0;
	length = blocksize;
	ret = btrfs_map_block(&root->fs_info->mapping_tree, READ,
			      bytenr, &length, &multi, 0);
	BUG_ON(ret);
	device = multi->stripes[0].dev;
	device->total_ios++;
	blocksize = min(blocksize, (u32)(64 * 1024));
	readahead(device->fd, multi->stripes[0].physical, blocksize);
	kfree(multi);
	return 0;
}

static int verify_parent_transid(struct extent_io_tree *io_tree,
				 struct extent_buffer *eb, u64 parent_transid)
{
	int ret;

	if (!parent_transid || btrfs_header_generation(eb) == parent_transid)
		return 0;

	if (extent_buffer_uptodate(eb) &&
	    btrfs_header_generation(eb) == parent_transid) {
		ret = 0;
		goto out;
	}
	printk("parent transid verify failed on %llu wanted %llu found %llu\n",
	       (unsigned long long)eb->start,
	       (unsigned long long)parent_transid,
	       (unsigned long long)btrfs_header_generation(eb));
	ret = 1;
out:
	clear_extent_buffer_uptodate(io_tree, eb);
	return ret;

}


struct extent_buffer *read_tree_block(struct btrfs_root *root, u64 bytenr,
				     u32 blocksize, u64 parent_transid)
{
	int ret;
	int dev_nr;
	struct extent_buffer *eb;
	u64 length;
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	int mirror_num = 0;
	int num_copies;

	eb = btrfs_find_create_tree_block(root, bytenr, blocksize);
	if (!eb)
		return NULL;

	if (btrfs_buffer_uptodate(eb, parent_transid))
		return eb;

	dev_nr = 0;
	length = blocksize;
	while (1) {
		ret = btrfs_map_block(&root->fs_info->mapping_tree, READ,
				      eb->start, &length, &multi, mirror_num);
		BUG_ON(ret);
		device = multi->stripes[0].dev;
		eb->fd = device->fd;
		device->total_ios++;
		eb->dev_bytenr = multi->stripes[0].physical;
		kfree(multi);
		ret = read_extent_from_disk(eb);
		if (ret == 0 && check_tree_block(root, eb) == 0 &&
		    csum_tree_block(root, eb, 1) == 0 &&
		    verify_parent_transid(eb->tree, eb, parent_transid) == 0) {
			btrfs_set_buffer_uptodate(eb);
			return eb;
		}
		num_copies = btrfs_num_copies(&root->fs_info->mapping_tree,
					      eb->start, eb->len);
		if (num_copies == 1) {
			break;
		}
		mirror_num++;
		if (mirror_num > num_copies) {
			break;
		}
	}
	free_extent_buffer(eb);
	return NULL;
}

int write_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct extent_buffer *eb)
{
	int ret;
	int dev_nr;
	u64 length;
	struct btrfs_multi_bio *multi = NULL;

	if (check_tree_block(root, eb))
		BUG();
	if (!btrfs_buffer_uptodate(eb, trans->transid))
		BUG();

	btrfs_set_header_flag(eb, BTRFS_HEADER_FLAG_WRITTEN);
	csum_tree_block(root, eb, 0);

	dev_nr = 0;
	length = eb->len;
	ret = btrfs_map_block(&root->fs_info->mapping_tree, WRITE,
			      eb->start, &length, &multi, 0);

	while(dev_nr < multi->num_stripes) {
		BUG_ON(ret);
		eb->fd = multi->stripes[dev_nr].dev->fd;
		eb->dev_bytenr = multi->stripes[dev_nr].physical;
		multi->stripes[dev_nr].dev->total_ios++;
		dev_nr++;
		ret = write_extent_to_disk(eb);
		BUG_ON(ret);
	}
	kfree(multi);
	return 0;
}

static int __setup_root(u32 nodesize, u32 leafsize, u32 sectorsize,
			u32 stripesize, struct btrfs_root *root,
			struct btrfs_fs_info *fs_info, u64 objectid)
{
	root->node = NULL;
	root->commit_root = NULL;
	root->sectorsize = sectorsize;
	root->nodesize = nodesize;
	root->leafsize = leafsize;
	root->stripesize = stripesize;
	root->ref_cows = 0;
	root->track_dirty = 0;

	root->fs_info = fs_info;
	root->objectid = objectid;
	root->last_trans = 0;
	root->highest_inode = 0;
	root->last_inode_alloc = 0;

	INIT_LIST_HEAD(&root->dirty_list);
	memset(&root->root_key, 0, sizeof(root->root_key));
	memset(&root->root_item, 0, sizeof(root->root_item));
	root->root_key.objectid = objectid;
	return 0;
}

static int update_cowonly_root(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root)
{
	int ret;
	u64 old_root_bytenr;
	struct btrfs_root *tree_root = root->fs_info->tree_root;

	btrfs_write_dirty_block_groups(trans, root);
	while(1) {
		old_root_bytenr = btrfs_root_bytenr(&root->root_item);
		if (old_root_bytenr == root->node->start)
			break;
		btrfs_set_root_bytenr(&root->root_item,
				       root->node->start);
		btrfs_set_root_generation(&root->root_item,
					  trans->transid);
		root->root_item.level = btrfs_header_level(root->node);
		ret = btrfs_update_root(trans, tree_root,
					&root->root_key,
					&root->root_item);
		BUG_ON(ret);
		btrfs_write_dirty_block_groups(trans, root);
	}
	return 0;
}

static int commit_tree_roots(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root;
	struct list_head *next;
	struct extent_buffer *eb;

	if (fs_info->readonly)
		return 0;

	eb = fs_info->tree_root->node;
	extent_buffer_get(eb);
	btrfs_cow_block(trans, fs_info->tree_root, eb, NULL, 0, &eb);
	free_extent_buffer(eb);

	while(!list_empty(&fs_info->dirty_cowonly_roots)) {
		next = fs_info->dirty_cowonly_roots.next;
		list_del_init(next);
		root = list_entry(next, struct btrfs_root, dirty_list);
		update_cowonly_root(trans, root);
	}
	return 0;
}

static int __commit_transaction(struct btrfs_trans_handle *trans,
				struct btrfs_root *root)
{
	u64 start;
	u64 end;
	struct extent_buffer *eb;
	struct extent_io_tree *tree = &root->fs_info->extent_cache;
	int ret;

	while(1) {
		ret = find_first_extent_bit(tree, 0, &start, &end,
					    EXTENT_DIRTY);
		if (ret)
			break;
		while(start <= end) {
			eb = find_first_extent_buffer(tree, start);
			BUG_ON(!eb || eb->start != start);
			ret = write_tree_block(trans, root, eb);
			BUG_ON(ret);
			start += eb->len;
			clear_extent_buffer_dirty(eb);
			free_extent_buffer(eb);
		}
	}
	return 0;
}

int btrfs_commit_transaction(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root)
{
	int ret = 0;
	struct btrfs_fs_info *fs_info = root->fs_info;

	if (root->commit_root == root->node)
		goto commit_tree;

	free_extent_buffer(root->commit_root);
	root->commit_root = NULL;

	btrfs_set_root_bytenr(&root->root_item, root->node->start);
	btrfs_set_root_generation(&root->root_item, trans->transid);
	root->root_item.level = btrfs_header_level(root->node);
	ret = btrfs_update_root(trans, root->fs_info->tree_root,
				&root->root_key, &root->root_item);
	BUG_ON(ret);
commit_tree:
	ret = commit_tree_roots(trans, fs_info);
	BUG_ON(ret);
	ret = __commit_transaction(trans, root);
	BUG_ON(ret);
	write_ctree_super(trans, root);
	btrfs_finish_extent_commit(trans, fs_info->extent_root,
			           &fs_info->pinned_extents);
	btrfs_free_transaction(root, trans);
	free_extent_buffer(root->commit_root);
	root->commit_root = NULL;
	fs_info->running_transaction = NULL;
	return 0;
}

static int find_and_setup_root(struct btrfs_root *tree_root,
			       struct btrfs_fs_info *fs_info,
			       u64 objectid, struct btrfs_root *root)
{
	int ret;
	u32 blocksize;
	u64 generation;

	__setup_root(tree_root->nodesize, tree_root->leafsize,
		     tree_root->sectorsize, tree_root->stripesize,
		     root, fs_info, objectid);
	ret = btrfs_find_last_root(tree_root, objectid,
				   &root->root_item, &root->root_key);
	BUG_ON(ret);

	blocksize = btrfs_level_size(root, btrfs_root_level(&root->root_item));
	generation = btrfs_root_generation(&root->root_item);
	root->node = read_tree_block(root, btrfs_root_bytenr(&root->root_item),
				     blocksize, generation);
	BUG_ON(!root->node);
	return 0;
}

static int find_and_setup_log_root(struct btrfs_root *tree_root,
			       struct btrfs_fs_info *fs_info,
			       struct btrfs_super_block *disk_super)
{
	u32 blocksize;
	u64 blocknr = btrfs_super_log_root(disk_super);
	struct btrfs_root *log_root = malloc(sizeof(struct btrfs_root));

	if (blocknr == 0)
		return 0;

	blocksize = btrfs_level_size(tree_root,
			     btrfs_super_log_root_level(disk_super));

	__setup_root(tree_root->nodesize, tree_root->leafsize,
		     tree_root->sectorsize, tree_root->stripesize,
		     log_root, fs_info, BTRFS_TREE_LOG_OBJECTID);

	log_root->node = read_tree_block(tree_root, blocknr,
				     blocksize,
				     btrfs_super_generation(disk_super) + 1);

	fs_info->log_root_tree = log_root;
	BUG_ON(!log_root->node);
	return 0;
}


int btrfs_free_fs_root(struct btrfs_fs_info *fs_info,
		       struct btrfs_root *root)
{
	if (root->node)
		free_extent_buffer(root->node);
	if (root->commit_root)
		free_extent_buffer(root->commit_root);
	kfree(root);
	return 0;
}

static int free_fs_roots(struct btrfs_fs_info *fs_info)
{
	struct cache_extent *cache;
	struct btrfs_root *root;

	while (1) {
		cache = find_first_cache_extent(&fs_info->fs_root_cache, 0);
		if (!cache)
			break;
		root = container_of(cache, struct btrfs_root, cache);
		remove_cache_extent(&fs_info->fs_root_cache, cache);
		btrfs_free_fs_root(fs_info, root);
	}
	return 0;
}

struct btrfs_root *btrfs_read_fs_root_no_cache(struct btrfs_fs_info *fs_info,
					       struct btrfs_key *location)
{
	struct btrfs_root *root;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_path *path;
	struct extent_buffer *l;
	u64 generation;
	u32 blocksize;
	int ret = 0;

	root = malloc(sizeof(*root));
	if (!root)
		return ERR_PTR(-ENOMEM);
	memset(root, 0, sizeof(*root));
	if (location->offset == (u64)-1) {
		ret = find_and_setup_root(tree_root, fs_info,
					  location->objectid, root);
		if (ret) {
			free(root);
			return ERR_PTR(ret);
		}
		goto insert;
	}

	__setup_root(tree_root->nodesize, tree_root->leafsize,
		     tree_root->sectorsize, tree_root->stripesize,
		     root, fs_info, location->objectid);

	path = btrfs_alloc_path();
	BUG_ON(!path);
	ret = btrfs_search_slot(NULL, tree_root, location, path, 0, 0);
	if (ret != 0) {
		if (ret > 0)
			ret = -ENOENT;
		goto out;
	}
	l = path->nodes[0];
	read_extent_buffer(l, &root->root_item,
	       btrfs_item_ptr_offset(l, path->slots[0]),
	       sizeof(root->root_item));
	memcpy(&root->root_key, location, sizeof(*location));
	ret = 0;
out:
	btrfs_release_path(root, path);
	btrfs_free_path(path);
	if (ret) {
		free(root);
		return ERR_PTR(ret);
	}
	generation = btrfs_root_generation(&root->root_item);
	blocksize = btrfs_level_size(root, btrfs_root_level(&root->root_item));
	root->node = read_tree_block(root, btrfs_root_bytenr(&root->root_item),
				     blocksize, generation);
	BUG_ON(!root->node);
insert:
	root->ref_cows = 1;
	return root;
}

struct btrfs_root *btrfs_read_fs_root(struct btrfs_fs_info *fs_info,
				      struct btrfs_key *location)
{
	struct btrfs_root *root;
	struct cache_extent *cache;
	int ret;

	if (location->objectid == BTRFS_ROOT_TREE_OBJECTID)
		return fs_info->tree_root;
	if (location->objectid == BTRFS_EXTENT_TREE_OBJECTID)
		return fs_info->extent_root;
	if (location->objectid == BTRFS_CHUNK_TREE_OBJECTID)
		return fs_info->chunk_root;
	if (location->objectid == BTRFS_DEV_TREE_OBJECTID)
		return fs_info->dev_root;
	if (location->objectid == BTRFS_CSUM_TREE_OBJECTID)
		return fs_info->csum_root;
	
	BUG_ON(location->objectid == BTRFS_TREE_RELOC_OBJECTID ||
	       location->offset != (u64)-1);

	cache = find_cache_extent(&fs_info->fs_root_cache,
				  location->objectid, 1);
	if (cache)
		return container_of(cache, struct btrfs_root, cache);

	root = btrfs_read_fs_root_no_cache(fs_info, location);
	if (IS_ERR(root))
		return root;

	root->cache.start = location->objectid;
	root->cache.size = 1;
	ret = insert_existing_cache_extent(&fs_info->fs_root_cache,
					   &root->cache);
	BUG_ON(ret);
	return root;
}

struct btrfs_root *open_ctree(const char *filename, u64 sb_bytenr, int writes)
{
	int fp;
	struct btrfs_root *root;
	int flags = O_CREAT | O_RDWR;

	if (!writes)
		flags = O_RDONLY;

	fp = open(filename, flags, 0600);
	if (fp < 0) {
		fprintf (stderr, "Could not open %s\n", filename);
		return NULL;
	}
	root = open_ctree_fd(fp, filename, sb_bytenr, writes);
	close(fp);

	return root;
}

struct btrfs_root *open_ctree_fd(int fp, const char *path, u64 sb_bytenr,
				 int writes)
{
	u32 sectorsize;
	u32 nodesize;
	u32 leafsize;
	u32 blocksize;
	u32 stripesize;
	u64 generation;
	struct btrfs_key key;
	struct btrfs_root *tree_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *extent_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *chunk_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *dev_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *csum_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_fs_info *fs_info = malloc(sizeof(*fs_info));
	int ret;
	struct btrfs_super_block *disk_super;
	struct btrfs_fs_devices *fs_devices = NULL;
	u64 total_devs;
	u64 features;

	if (sb_bytenr == 0)
		sb_bytenr = BTRFS_SUPER_INFO_OFFSET;

	ret = btrfs_scan_one_device(fp, path, &fs_devices,
				    &total_devs, sb_bytenr);

	if (ret) {
		fprintf(stderr, "No valid Btrfs found on %s\n", path);
		return NULL;
	}

	if (total_devs != 1) {
		ret = btrfs_scan_for_fsid(fs_devices, total_devs, 1);
		BUG_ON(ret);
	}

	memset(fs_info, 0, sizeof(*fs_info));
	fs_info->tree_root = tree_root;
	fs_info->extent_root = extent_root;
	fs_info->chunk_root = chunk_root;
	fs_info->dev_root = dev_root;
	fs_info->csum_root = csum_root;

	if (!writes)
		fs_info->readonly = 1;

	extent_io_tree_init(&fs_info->extent_cache);
	extent_io_tree_init(&fs_info->free_space_cache);
	extent_io_tree_init(&fs_info->block_group_cache);
	extent_io_tree_init(&fs_info->pinned_extents);
	extent_io_tree_init(&fs_info->pending_del);
	extent_io_tree_init(&fs_info->extent_ins);
	cache_tree_init(&fs_info->fs_root_cache);

	cache_tree_init(&fs_info->mapping_tree.cache_tree);

	mutex_init(&fs_info->fs_mutex);
	fs_info->fs_devices = fs_devices;
	INIT_LIST_HEAD(&fs_info->dirty_cowonly_roots);
	INIT_LIST_HEAD(&fs_info->space_info);

	__setup_root(4096, 4096, 4096, 4096, tree_root,
		     fs_info, BTRFS_ROOT_TREE_OBJECTID);

	if (writes)
		ret = btrfs_open_devices(fs_devices, O_RDWR);
	else
		ret = btrfs_open_devices(fs_devices, O_RDONLY);
	BUG_ON(ret);

	fs_info->super_bytenr = sb_bytenr;
	disk_super = &fs_info->super_copy;
	ret = btrfs_read_dev_super(fs_devices->latest_bdev,
				   disk_super, sb_bytenr);
	if (ret) {
		printk("No valid btrfs found\n");
		BUG_ON(1);
	}

	memcpy(fs_info->fsid, &disk_super->fsid, BTRFS_FSID_SIZE);


	features = btrfs_super_incompat_flags(disk_super) &
		   ~BTRFS_FEATURE_INCOMPAT_SUPP;
	if (features) {
		printk("couldn't open because of unsupported "
		       "option features (%Lx).\n", features);
		BUG_ON(1);
	}

	features = btrfs_super_incompat_flags(disk_super);
	if (!(features & BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF)) {
		features |= BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF;
		btrfs_set_super_incompat_flags(disk_super, features);
	}

	features = btrfs_super_compat_ro_flags(disk_super) &
		~BTRFS_FEATURE_COMPAT_RO_SUPP;
	if (writes && features) {
		printk("couldn't open RDWR because of unsupported "
		       "option features (%Lx).\n", features);
		BUG_ON(1);
	}

	nodesize = btrfs_super_nodesize(disk_super);
	leafsize = btrfs_super_leafsize(disk_super);
	sectorsize = btrfs_super_sectorsize(disk_super);
	stripesize = btrfs_super_stripesize(disk_super);
	tree_root->nodesize = nodesize;
	tree_root->leafsize = leafsize;
	tree_root->sectorsize = sectorsize;
	tree_root->stripesize = stripesize;

	ret = btrfs_read_sys_array(tree_root);
	BUG_ON(ret);
	blocksize = btrfs_level_size(tree_root,
				     btrfs_super_chunk_root_level(disk_super));
	generation = btrfs_super_chunk_root_generation(disk_super);

	__setup_root(nodesize, leafsize, sectorsize, stripesize,
		     chunk_root, fs_info, BTRFS_CHUNK_TREE_OBJECTID);

	chunk_root->node = read_tree_block(chunk_root,
					   btrfs_super_chunk_root(disk_super),
					   blocksize, generation);

	BUG_ON(!chunk_root->node);

	read_extent_buffer(chunk_root->node, fs_info->chunk_tree_uuid,
	         (unsigned long)btrfs_header_chunk_tree_uuid(chunk_root->node),
		 BTRFS_UUID_SIZE);

	if (!(btrfs_super_flags(disk_super) & BTRFS_SUPER_FLAG_METADUMP)) {
		ret = btrfs_read_chunk_tree(chunk_root);
		BUG_ON(ret);
	}

	blocksize = btrfs_level_size(tree_root,
				     btrfs_super_root_level(disk_super));
	generation = btrfs_super_generation(disk_super);

	tree_root->node = read_tree_block(tree_root,
					  btrfs_super_root(disk_super),
					  blocksize, generation);
	BUG_ON(!tree_root->node);
	ret = find_and_setup_root(tree_root, fs_info,
				  BTRFS_EXTENT_TREE_OBJECTID, extent_root);
	BUG_ON(ret);
	extent_root->track_dirty = 1;

	ret = find_and_setup_root(tree_root, fs_info,
				  BTRFS_DEV_TREE_OBJECTID, dev_root);
	BUG_ON(ret);
	dev_root->track_dirty = 1;

	ret = find_and_setup_root(tree_root, fs_info,
				  BTRFS_CSUM_TREE_OBJECTID, csum_root);
	BUG_ON(ret);
	csum_root->track_dirty = 1;

	BUG_ON(ret);

	find_and_setup_log_root(tree_root, fs_info, disk_super);

	fs_info->generation = generation + 1;
	btrfs_read_block_groups(fs_info->tree_root);

	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	fs_info->fs_root = btrfs_read_fs_root(fs_info, &key);

	fs_info->data_alloc_profile = (u64)-1;
	fs_info->metadata_alloc_profile = (u64)-1;
	fs_info->system_alloc_profile = fs_info->metadata_alloc_profile;

	return fs_info->fs_root;
}

int btrfs_read_dev_super(int fd, struct btrfs_super_block *sb, u64 sb_bytenr)
{
	u8 fsid[BTRFS_FSID_SIZE];
	struct btrfs_super_block buf;
	int i;
	int ret;
	u64 transid = 0;
	u64 bytenr;

	if (sb_bytenr != BTRFS_SUPER_INFO_OFFSET) {
		ret = pread64(fd, &buf, sizeof(buf), sb_bytenr);
		if (ret < sizeof(buf))
			return -1;

		if (btrfs_super_bytenr(&buf) != sb_bytenr ||
		    strncmp((char *)(&buf.magic), BTRFS_MAGIC,
			    sizeof(buf.magic)))
			return -1;

		memcpy(sb, &buf, sizeof(*sb));
		return 0;
	}

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = pread64(fd, &buf, sizeof(buf), bytenr);
		if (ret < sizeof(buf))
			break;

		if (btrfs_super_bytenr(&buf) != bytenr ||
		    strncmp((char *)(&buf.magic), BTRFS_MAGIC,
			    sizeof(buf.magic)))
			continue;

		if (i == 0)
			memcpy(fsid, buf.fsid, sizeof(fsid));
		else if (memcmp(fsid, buf.fsid, sizeof(fsid)))
			continue;

		if (btrfs_super_generation(&buf) > transid) {
			memcpy(sb, &buf, sizeof(*sb));
			transid = btrfs_super_generation(&buf);
		}
	}

	return transid > 0 ? 0 : -1;
}

int write_dev_supers(struct btrfs_root *root, struct btrfs_super_block *sb,
		     struct btrfs_device *device)
{
	u64 bytenr;
	u32 crc;
	int i, ret;

	if (root->fs_info->super_bytenr != BTRFS_SUPER_INFO_OFFSET) {
		btrfs_set_super_bytenr(sb, root->fs_info->super_bytenr);

		crc = ~(u32)0;
		crc = btrfs_csum_data(NULL, (char *)sb + BTRFS_CSUM_SIZE, crc,
				      BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
		btrfs_csum_final(crc, (char *)&sb->csum[0]);

		ret = pwrite64(device->fd, sb, BTRFS_SUPER_INFO_SIZE,
			       root->fs_info->super_bytenr);
		BUG_ON(ret != BTRFS_SUPER_INFO_SIZE);
		return 0;
	}

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		if (bytenr + BTRFS_SUPER_INFO_SIZE >= device->total_bytes)
			break;

		btrfs_set_super_bytenr(sb, bytenr);

		crc = ~(u32)0;
		crc = btrfs_csum_data(NULL, (char *)sb + BTRFS_CSUM_SIZE, crc,
				      BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
		btrfs_csum_final(crc, (char *)&sb->csum[0]);

		ret = pwrite64(device->fd, sb, BTRFS_SUPER_INFO_SIZE, bytenr);
		BUG_ON(ret != BTRFS_SUPER_INFO_SIZE);
	}
	return 0;
}

int write_all_supers(struct btrfs_root *root)
{
	struct list_head *cur;
	struct list_head *head = &root->fs_info->fs_devices->devices;
	struct btrfs_device *dev;
	struct btrfs_super_block *sb;
	struct btrfs_dev_item *dev_item;
	int ret;
	u64 flags;

	sb = &root->fs_info->super_copy;
	dev_item = &sb->dev_item;
	list_for_each(cur, head) {
		dev = list_entry(cur, struct btrfs_device, dev_list);
		if (!dev->writeable)
			continue;

		btrfs_set_stack_device_generation(dev_item, 0);
		btrfs_set_stack_device_type(dev_item, dev->type);
		btrfs_set_stack_device_id(dev_item, dev->devid);
		btrfs_set_stack_device_total_bytes(dev_item, dev->total_bytes);
		btrfs_set_stack_device_bytes_used(dev_item, dev->bytes_used);
		btrfs_set_stack_device_io_align(dev_item, dev->io_align);
		btrfs_set_stack_device_io_width(dev_item, dev->io_width);
		btrfs_set_stack_device_sector_size(dev_item, dev->sector_size);
		memcpy(dev_item->uuid, dev->uuid, BTRFS_UUID_SIZE);
		memcpy(dev_item->fsid, dev->fs_devices->fsid, BTRFS_UUID_SIZE);

		flags = btrfs_super_flags(sb);
		btrfs_set_super_flags(sb, flags | BTRFS_HEADER_FLAG_WRITTEN);

		ret = write_dev_supers(root, sb, dev);
		BUG_ON(ret);
	}
	return 0;
}

int write_ctree_super(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root)
{
	int ret;
	struct btrfs_root *tree_root = root->fs_info->tree_root;
	struct btrfs_root *chunk_root = root->fs_info->chunk_root;

	if (root->fs_info->readonly)
		return 0;

	btrfs_set_super_generation(&root->fs_info->super_copy,
				   trans->transid);
	btrfs_set_super_root(&root->fs_info->super_copy,
			     tree_root->node->start);
	btrfs_set_super_root_level(&root->fs_info->super_copy,
				   btrfs_header_level(tree_root->node));
	btrfs_set_super_chunk_root(&root->fs_info->super_copy,
				   chunk_root->node->start);
	btrfs_set_super_chunk_root_level(&root->fs_info->super_copy,
					 btrfs_header_level(chunk_root->node));
	btrfs_set_super_chunk_root_generation(&root->fs_info->super_copy,
				btrfs_header_generation(chunk_root->node));

	ret = write_all_supers(root);
	if (ret)
		fprintf(stderr, "failed to write new super block err %d\n", ret);
	return ret;
}

static int close_all_devices(struct btrfs_fs_info *fs_info)
{
	struct list_head *list;
	struct list_head *next;
	struct btrfs_device *device;

	return 0;

	list = &fs_info->fs_devices->devices;
	list_for_each(next, list) {
		device = list_entry(next, struct btrfs_device, dev_list);
		close(device->fd);
	}
	return 0;
}

int close_ctree(struct btrfs_root *root)
{
	int ret;
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = root->fs_info;

	trans = btrfs_start_transaction(root, 1);
	btrfs_commit_transaction(trans, root);
	trans = btrfs_start_transaction(root, 1);
	ret = commit_tree_roots(trans, fs_info);
	BUG_ON(ret);
	ret = __commit_transaction(trans, root);
	BUG_ON(ret);
	write_ctree_super(trans, root);
	btrfs_free_transaction(root, trans);
	btrfs_free_block_groups(fs_info);

	free_fs_roots(fs_info);

	if (fs_info->extent_root->node)
		free_extent_buffer(fs_info->extent_root->node);
	if (fs_info->tree_root->node)
		free_extent_buffer(fs_info->tree_root->node);
	if (fs_info->chunk_root->node)
		free_extent_buffer(fs_info->chunk_root->node);
	if (fs_info->dev_root->node)
		free_extent_buffer(fs_info->dev_root->node);
	if (fs_info->csum_root->node)
		free_extent_buffer(fs_info->csum_root->node);

	if (root->fs_info->log_root_tree) {
		if (root->fs_info->log_root_tree->node)
			free_extent_buffer(root->fs_info->log_root_tree->node);
		free(root->fs_info->log_root_tree);
	}

	close_all_devices(root->fs_info);
	extent_io_tree_cleanup(&fs_info->extent_cache);
	extent_io_tree_cleanup(&fs_info->free_space_cache);
	extent_io_tree_cleanup(&fs_info->block_group_cache);
	extent_io_tree_cleanup(&fs_info->pinned_extents);
	extent_io_tree_cleanup(&fs_info->pending_del);
	extent_io_tree_cleanup(&fs_info->extent_ins);

	free(fs_info->tree_root);
	free(fs_info->extent_root);
	free(fs_info->chunk_root);
	free(fs_info->dev_root);
	free(fs_info->csum_root);
	free(fs_info);

	return 0;
}

int clean_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct extent_buffer *eb)
{
	return clear_extent_buffer_dirty(eb);
}

int wait_on_tree_block_writeback(struct btrfs_root *root,
				 struct extent_buffer *eb)
{
	return 0;
}

void btrfs_mark_buffer_dirty(struct extent_buffer *eb)
{
	set_extent_buffer_dirty(eb);
}

int btrfs_buffer_uptodate(struct extent_buffer *buf, u64 parent_transid)
{
	int ret;

	ret = extent_buffer_uptodate(buf);
	if (!ret)
		return ret;

	ret = verify_parent_transid(buf->tree, buf, parent_transid);
	return !ret;
}

int btrfs_set_buffer_uptodate(struct extent_buffer *eb)
{
	return set_extent_buffer_uptodate(eb);
}
