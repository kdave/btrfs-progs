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
#include "transaction.h"
#include "crc32c.h"

int btrfs_map_bh_to_logical(struct btrfs_root *root, struct extent_buffer *buf,
			    u64 logical)
{
	buf->fd = root->fs_info->fp;
	buf->dev_bytenr = logical;
	return 0;
}

static int check_tree_block(struct btrfs_root *root, struct extent_buffer *buf)
{
	if (buf->start != btrfs_header_bytenr(buf))
		BUG();

	if (memcmp_extent_buffer(buf, root->fs_info->fsid,
				 (unsigned long)btrfs_header_fsid(buf),
				 BTRFS_FSID_SIZE))
		BUG();
	return 0;
}

u32 btrfs_csum_data(struct btrfs_root *root, char *data, u32 seed, size_t len)
{
	return crc32c(seed, data, len);
}

void btrfs_csum_final(u32 crc, char *result)
{
	*(__le32 *)result = ~cpu_to_le32(crc);
}

static int csum_tree_block(struct btrfs_root *root, struct extent_buffer *buf,
			   int verify)
{
	char result[BTRFS_CRC32_SIZE];
	u32 len;
	u32 crc = ~(u32)0;

	len = buf->len - BTRFS_CSUM_SIZE;
	crc = crc32c(crc, buf->data + BTRFS_CSUM_SIZE, len);
	btrfs_csum_final(crc, result);

	if (verify) {
		if (memcmp_extent_buffer(buf, result, 0, BTRFS_CRC32_SIZE)) {
			printk("checksum verify failed on %llu\n", buf->start);
			return 1;
		}
	} else {
		write_extent_buffer(buf, result, 0, BTRFS_CRC32_SIZE);
	}
	return 0;
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

int readahead_tree_block(struct btrfs_root *root, u64 bytenr, u32 blocksize)
{
	return 0;
}

struct extent_buffer *read_tree_block(struct btrfs_root *root, u64 bytenr,
				     u32 blocksize)
{
	int ret;
	struct extent_buffer *eb;

	eb = btrfs_find_create_tree_block(root, bytenr, blocksize);
	if (!eb)
		return NULL;
	if (!btrfs_buffer_uptodate(eb)) {
		btrfs_map_bh_to_logical(root, eb, eb->start);
		ret = read_extent_from_disk(eb);
		if (ret) {
			free_extent_buffer(eb);
			return NULL;
		}
		btrfs_set_buffer_uptodate(eb);
	}
	return eb;
}

int write_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct extent_buffer *eb)
{
	if (check_tree_block(root, eb))
		BUG();
	if (!btrfs_buffer_uptodate(eb))
		BUG();
	btrfs_map_bh_to_logical(root, eb, eb->start);
	csum_tree_block(root, eb, 0);
	return write_extent_to_disk(eb);
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
	root->fs_info = fs_info;
	root->objectid = objectid;
	root->last_trans = 0;
	root->highest_inode = 0;
	root->last_inode_alloc = 0;
	memset(&root->root_key, 0, sizeof(root->root_key));
	memset(&root->root_item, 0, sizeof(root->root_item));
	root->root_key.objectid = objectid;
	return 0;
}

static int commit_tree_roots(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info)
{
	int ret;
	u64 old_extent_bytenr;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *extent_root = fs_info->extent_root;

	btrfs_write_dirty_block_groups(trans, fs_info->extent_root);
	while(1) {
		old_extent_bytenr = btrfs_root_bytenr(&extent_root->root_item);
		if (old_extent_bytenr == extent_root->node->start)
			break;
		btrfs_set_root_bytenr(&extent_root->root_item,
				       extent_root->node->start);
		extent_root->root_item.level =
			btrfs_header_level(extent_root->node);
		ret = btrfs_update_root(trans, tree_root,
					&extent_root->root_key,
					&extent_root->root_item);
		BUG_ON(ret);
		btrfs_write_dirty_block_groups(trans, fs_info->extent_root);
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
	struct btrfs_root *new_root = NULL;
	struct btrfs_fs_info *fs_info = root->fs_info;

	if (root->commit_root == root->node)
		goto commit_tree;

	new_root = malloc(sizeof(*new_root));
	if (!new_root)
		return -ENOMEM;
	memcpy(new_root, root, sizeof(*new_root));
	new_root->node = root->commit_root;
	root->commit_root = NULL;

	root->root_key.offset = trans->transid;
	btrfs_set_root_bytenr(&root->root_item, root->node->start);
	root->root_item.level = btrfs_header_level(root->node);
	ret = btrfs_insert_root(trans, fs_info->tree_root,
				&root->root_key, &root->root_item);
	BUG_ON(ret);

	btrfs_set_root_refs(&new_root->root_item, 0);
	ret = btrfs_update_root(trans, root->fs_info->tree_root,
				&new_root->root_key, &new_root->root_item);
	BUG_ON(ret);

	ret = commit_tree_roots(trans, fs_info);
	BUG_ON(ret);
	ret = __commit_transaction(trans, root);
	BUG_ON(ret);
	write_ctree_super(trans, root);
	btrfs_finish_extent_commit(trans, fs_info->extent_root,
			           &fs_info->pinned_extents);
	btrfs_free_transaction(root, trans);
	fs_info->running_transaction = NULL;

	trans = btrfs_start_transaction(root, 1);
	ret = btrfs_drop_snapshot(trans, new_root);
	BUG_ON(ret);
	ret = btrfs_del_root(trans, fs_info->tree_root, &new_root->root_key);
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
	if (new_root) {
		free_extent_buffer(new_root->node);
		free(new_root);
	}
	return 0;
}

static int find_and_setup_root(struct btrfs_root *tree_root,
			       struct btrfs_fs_info *fs_info,
			       u64 objectid, struct btrfs_root *root)
{
	int ret;
	u32 blocksize;

	__setup_root(tree_root->nodesize, tree_root->leafsize,
		     tree_root->sectorsize, tree_root->stripesize,
		     root, fs_info, objectid);
	ret = btrfs_find_last_root(tree_root, objectid,
				   &root->root_item, &root->root_key);
	BUG_ON(ret);

	blocksize = btrfs_level_size(root, btrfs_root_level(&root->root_item));
	root->node = read_tree_block(root, btrfs_root_bytenr(&root->root_item),
				     blocksize);
	BUG_ON(!root->node);
	return 0;
}

int btrfs_free_fs_root(struct btrfs_fs_info *fs_info, struct btrfs_root *root)
{
	if (root->node)
		free_extent_buffer(root->node);
	if (root->commit_root)
		free_extent_buffer(root->commit_root);

	free(root);
	return 0;
}

struct btrfs_root *btrfs_read_fs_root(struct btrfs_fs_info *fs_info,
				      struct btrfs_key *location)
{
	struct btrfs_root *root;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_path *path;
	struct extent_buffer *l;
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
	blocksize = btrfs_level_size(root, btrfs_root_level(&root->root_item));
	root->node = read_tree_block(root, btrfs_root_bytenr(&root->root_item),
				     blocksize);
	BUG_ON(!root->node);
insert:
	root->ref_cows = 1;
	return root;
}

struct btrfs_root *open_ctree(char *filename, u64 sb_bytenr)
{
	int fp;

	fp = open(filename, O_CREAT | O_RDWR, 0600);
	if (fp < 0) {
		return NULL;
	}
	return open_ctree_fd(fp, sb_bytenr);
}

struct btrfs_root *open_ctree_fd(int fp, u64 sb_bytenr)
{
	u32 sectorsize;
	u32 nodesize;
	u32 leafsize;
	u32 blocksize;
	u32 stripesize;
	struct btrfs_root *root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *tree_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *extent_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_fs_info *fs_info = malloc(sizeof(*fs_info));
	int ret;
	struct btrfs_super_block *disk_super;

	if (sb_bytenr == 0)
		sb_bytenr = BTRFS_SUPER_INFO_OFFSET;

	fs_info->fp = fp;
	fs_info->running_transaction = NULL;
	fs_info->fs_root = root;
	fs_info->tree_root = tree_root;
	fs_info->extent_root = extent_root;
	fs_info->extent_ops = NULL;
	fs_info->priv_data = NULL;
	extent_io_tree_init(&fs_info->extent_cache);
	extent_io_tree_init(&fs_info->free_space_cache);
	extent_io_tree_init(&fs_info->block_group_cache);
	extent_io_tree_init(&fs_info->pinned_extents);
	extent_io_tree_init(&fs_info->pending_del);
	extent_io_tree_init(&fs_info->extent_ins);

	mutex_init(&fs_info->fs_mutex);

	__setup_root(512, 512, 512, 512, tree_root,
		     fs_info, BTRFS_ROOT_TREE_OBJECTID);

	fs_info->sb_buffer = read_tree_block(tree_root, sb_bytenr, 512);
	BUG_ON(!fs_info->sb_buffer);
	read_extent_buffer(fs_info->sb_buffer, &fs_info->super_copy, 0,
			   sizeof(fs_info->super_copy));
	read_extent_buffer(fs_info->sb_buffer, fs_info->fsid,
			   (unsigned long)btrfs_super_fsid(fs_info->sb_buffer),
			   BTRFS_FSID_SIZE);

	disk_super = &fs_info->super_copy;
	if (strncmp((char *)(&disk_super->magic), BTRFS_MAGIC,
		    sizeof(disk_super->magic))) {
		printk("No valid btrfs found\n");
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

	blocksize = btrfs_level_size(tree_root,
				     btrfs_super_root_level(disk_super));
	tree_root->node = read_tree_block(tree_root,
					  btrfs_super_root(disk_super),
					  blocksize);
	BUG_ON(!tree_root->node);
	ret = find_and_setup_root(tree_root, fs_info,
				  BTRFS_EXTENT_TREE_OBJECTID, extent_root);
	BUG_ON(ret);
	ret = find_and_setup_root(tree_root, fs_info,
				  BTRFS_FS_TREE_OBJECTID, root);
	BUG_ON(ret);
	root->ref_cows = 1;
	fs_info->generation = btrfs_super_generation(disk_super) + 1;
	btrfs_read_block_groups(root);
	return root;
}

int write_ctree_super(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root)
{
	int ret;
	struct btrfs_root *tree_root = root->fs_info->tree_root;
	btrfs_set_super_generation(&root->fs_info->super_copy,
				   trans->transid);
	btrfs_set_super_root(&root->fs_info->super_copy,
			     tree_root->node->start);
	btrfs_set_super_root_level(&root->fs_info->super_copy,
				   btrfs_header_level(tree_root->node));
	write_extent_buffer(root->fs_info->sb_buffer,
			    &root->fs_info->super_copy, 0,
			    sizeof(root->fs_info->super_copy));
	ret = write_tree_block(trans, root, root->fs_info->sb_buffer);
	if (ret)
		fprintf(stderr, "failed to write new super block err %d\n", ret);
	return ret;
}

int close_ctree(struct btrfs_root *root)
{
	int ret;
	struct btrfs_trans_handle *trans;
	struct btrfs_fs_info *fs_info = root->fs_info;

	trans = btrfs_start_transaction(root, 1);
	btrfs_commit_transaction(trans, root);
	trans = btrfs_start_transaction(root, 1);
	ret = commit_tree_roots(trans, root->fs_info);
	BUG_ON(ret);
	ret = __commit_transaction(trans, root);
	BUG_ON(ret);
	write_ctree_super(trans, root);
	btrfs_free_transaction(root, trans);
	btrfs_free_block_groups(root->fs_info);
	close(root->fs_info->fp);
	if (root->node)
		free_extent_buffer(root->node);
	if (root->fs_info->extent_root->node)
		free_extent_buffer(root->fs_info->extent_root->node);
	if (root->fs_info->tree_root->node)
		free_extent_buffer(root->fs_info->tree_root->node);
	free_extent_buffer(root->commit_root);
	free_extent_buffer(root->fs_info->sb_buffer);

	extent_io_tree_cleanup(&fs_info->extent_cache);
	extent_io_tree_cleanup(&fs_info->free_space_cache);
	extent_io_tree_cleanup(&fs_info->block_group_cache);
	extent_io_tree_cleanup(&fs_info->pinned_extents);
	extent_io_tree_cleanup(&fs_info->pending_del);
	extent_io_tree_cleanup(&fs_info->extent_ins);

	free(fs_info->tree_root);
	free(fs_info->extent_root);
	free(fs_info->fs_root);
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

int btrfs_buffer_uptodate(struct extent_buffer *eb)
{
	return extent_buffer_uptodate(eb);
}

int btrfs_set_buffer_uptodate(struct extent_buffer *eb)
{
	return set_extent_buffer_uptodate(eb);
}
