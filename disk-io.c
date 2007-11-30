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

static u64 allocated_bytes = 0;
int cache_max = 10000;

int btrfs_map_bh_to_logical(struct btrfs_root *root, struct btrfs_buffer *bh,
			     u64 logical)
{
	bh->fd = root->fs_info->fp;
	bh->dev_bytenr = logical;
	return 0;
}

static int check_tree_block(struct btrfs_root *root, struct btrfs_buffer *buf)
{
	if (buf->bytenr != btrfs_header_bytenr(&buf->node.header))
		BUG();
	if (memcmp(root->fs_info->disk_super->fsid, buf->node.header.fsid,
		   sizeof(buf->node.header.fsid)))
		BUG();
	return 0;
}

static int free_some_buffers(struct btrfs_root *root)
{
	struct list_head *node, *next;
	struct btrfs_buffer *b;
	if (root->fs_info->cache_size < cache_max)
		return 0;
	list_for_each_safe(node, next, &root->fs_info->cache) {
		b = list_entry(node, struct btrfs_buffer, cache);
		if (b->count == 1) {
			BUG_ON(!list_empty(&b->dirty));
			list_del_init(&b->cache);
			btrfs_block_release(root, b);
			if (root->fs_info->cache_size < cache_max)
				break;
		}
	}
	return 0;
}

struct btrfs_buffer *alloc_tree_block(struct btrfs_root *root, u64 bytenr,
				      u32 blocksize)
{
	struct btrfs_buffer *buf;
	int ret;

	buf = malloc(sizeof(struct btrfs_buffer) + blocksize);
	if (!buf)
		return buf;
	allocated_bytes += blocksize;

	buf->bytenr = bytenr;
	buf->count = 2;
	buf->size = blocksize;
	buf->cache_node.start = bytenr;
	buf->cache_node.size = blocksize;

	INIT_LIST_HEAD(&buf->dirty);
	free_some_buffers(root);

	ret = insert_existing_cache_extent(&root->fs_info->extent_cache,
					   &buf->cache_node);

	list_add_tail(&buf->cache, &root->fs_info->cache);
	root->fs_info->cache_size += blocksize;
	if (ret) {
		free(buf);
		return NULL;
	}
	return buf;
}

struct btrfs_buffer *find_tree_block(struct btrfs_root *root, u64 bytenr,
				     u32 blocksize)
{
	struct btrfs_buffer *buf;
	struct cache_extent *cache;

	cache = find_cache_extent(&root->fs_info->extent_cache,
				  bytenr, blocksize);
	if (cache) {
		buf = container_of(cache, struct btrfs_buffer, cache_node);
		buf->count++;
	} else {
		buf = alloc_tree_block(root, bytenr, blocksize);
		if (!buf) {
			BUG();
			return NULL;
		}
	}
	return buf;
}

struct btrfs_buffer *read_tree_block(struct btrfs_root *root, u64 bytenr,
				     u32 blocksize)
{
	struct btrfs_buffer *buf;
	int ret;
	struct cache_extent *cache;

	cache = find_cache_extent(&root->fs_info->extent_cache,
				  bytenr, blocksize);
	if (cache) {
		buf = container_of(cache, struct btrfs_buffer, cache_node);
		buf->count++;
		if (check_tree_block(root, buf))
			BUG();
	} else {
		buf = alloc_tree_block(root, bytenr, blocksize);
		if (!buf)
			return NULL;
		btrfs_map_bh_to_logical(root, buf, bytenr);
		ret = pread(buf->fd, &buf->node, blocksize,
			    buf->dev_bytenr);
		if (ret != blocksize) {
			free(buf);
			return NULL;
		}
		if (check_tree_block(root, buf))
			BUG();
	}
	return buf;
}

int dirty_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct btrfs_buffer *buf)
{
	if (!list_empty(&buf->dirty))
		return 0;
	list_add_tail(&buf->dirty, &root->fs_info->trans);
	buf->count++;
	if (check_tree_block(root, buf))
		BUG();
	return 0;
}

int clean_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct btrfs_buffer *buf)
{
	if (!list_empty(&buf->dirty)) {
		list_del_init(&buf->dirty);
		btrfs_block_release(root, buf);
	}
	return 0;
}

int btrfs_csum_node(struct btrfs_root *root, struct btrfs_node *node)
{
	u32 crc = ~(u32)0;
	size_t len = btrfs_level_size(root, btrfs_header_level(&node->header)) -
				      BTRFS_CSUM_SIZE;

	crc = crc32c(crc, (char *)(node) + BTRFS_CSUM_SIZE, len);
	crc = ~cpu_to_le32(crc);
	memcpy(node->header.csum, &crc, BTRFS_CRC32_SIZE);
	return 0;
}

int btrfs_csum_super(struct btrfs_root *root, struct btrfs_super_block *super)
{
	u32 crc = ~(u32)0;
	char block[512];
	size_t len = 512 - BTRFS_CSUM_SIZE;

	memset(block, 0, 512);
	memcpy(block, super, sizeof(*super));

	crc = crc32c(crc, block + BTRFS_CSUM_SIZE, len);
	crc = ~cpu_to_le32(crc);
	memcpy(super->csum, &crc, BTRFS_CRC32_SIZE);
	return 0;
}

int write_tree_block(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		     struct btrfs_buffer *buf)
{
	int ret;

	if (buf->bytenr != btrfs_header_bytenr(&buf->node.header))
		BUG();
	btrfs_map_bh_to_logical(root, buf, buf->bytenr);
	if (check_tree_block(root, buf))
		BUG();

	btrfs_csum_node(root, &buf->node);

	ret = pwrite(buf->fd, &buf->node, buf->size,
		     buf->dev_bytenr);
	if (ret != buf->size)
		return ret;
	return 0;
}

static int __commit_transaction(struct btrfs_trans_handle *trans, struct
				btrfs_root *root)
{
	struct btrfs_buffer *b;
	int ret = 0;
	int wret;
	while(!list_empty(&root->fs_info->trans)) {
		b = list_entry(root->fs_info->trans.next, struct btrfs_buffer,
			       dirty);
		list_del_init(&b->dirty);
		wret = write_tree_block(trans, root, b);
		if (wret)
			ret = wret;
		btrfs_block_release(root, b);
	}
	return ret;
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
		if (old_extent_bytenr == extent_root->node->bytenr)
			break;
		btrfs_set_root_bytenr(&extent_root->root_item,
				       extent_root->node->bytenr);
		extent_root->root_item.level =
			btrfs_header_level(&extent_root->node->node.header);
		ret = btrfs_update_root(trans, tree_root,
					&extent_root->root_key,
					&extent_root->root_item);
		BUG_ON(ret);
		btrfs_write_dirty_block_groups(trans, fs_info->extent_root);
	}
	return 0;
}

int btrfs_commit_transaction(struct btrfs_trans_handle *trans, struct
			     btrfs_root *root, struct btrfs_super_block *s)
{
	int ret = 0;
	struct btrfs_buffer *snap = root->commit_root;
	struct btrfs_key snap_key;

	if (root->commit_root == root->node)
		return 0;

	memcpy(&snap_key, &root->root_key, sizeof(snap_key));
	root->root_key.offset++;

	btrfs_set_root_bytenr(&root->root_item, root->node->bytenr);
	root->root_item.level =
			btrfs_header_level(&root->node->node.header);
	ret = btrfs_insert_root(trans, root->fs_info->tree_root,
				&root->root_key, &root->root_item);
	BUG_ON(ret);

	ret = commit_tree_roots(trans, root->fs_info);
	BUG_ON(ret);

	ret = __commit_transaction(trans, root);
	BUG_ON(ret);

	write_ctree_super(trans, root, s);
	btrfs_finish_extent_commit(trans, root->fs_info->extent_root);
	btrfs_finish_extent_commit(trans, root->fs_info->tree_root);

	root->commit_root = root->node;
	root->node->count++;
	ret = btrfs_drop_snapshot(trans, root, snap);
	BUG_ON(ret);

	ret = btrfs_del_root(trans, root->fs_info->tree_root, &snap_key);
	BUG_ON(ret);
	root->fs_info->generation = root->root_key.offset + 1;

	return ret;
}

static int __setup_root(struct btrfs_super_block *super,
			struct btrfs_root *root,
			struct btrfs_fs_info *fs_info,
			u64 objectid, int fp)
{
	root->node = NULL;
	root->commit_root = NULL;
	root->sectorsize = btrfs_super_sectorsize(super);
	root->nodesize = btrfs_super_nodesize(super);
	root->leafsize = btrfs_super_leafsize(super);
	root->stripesize = btrfs_super_stripesize(super);
	root->ref_cows = 0;
	root->fs_info = fs_info;
	memset(&root->root_key, 0, sizeof(root->root_key));
	memset(&root->root_item, 0, sizeof(root->root_item));
	root->root_key.objectid = objectid;
	return 0;
}

struct btrfs_buffer *read_root_block(struct btrfs_root *root, u64 bytenr,
					    u8 level)
{
	struct btrfs_buffer *node;
	u32 size = btrfs_level_size(root, level);

	node = read_tree_block(root, bytenr, size);
	BUG_ON(!node);
	return node;
}

static int find_and_setup_root(struct btrfs_super_block *super,
			       struct btrfs_root *tree_root,
			       struct btrfs_fs_info *fs_info,
			       u64 objectid,
			       struct btrfs_root *root, int fp)
{
	int ret;

	__setup_root(super, root, fs_info, objectid, fp);
	ret = btrfs_find_last_root(tree_root, objectid,
				   &root->root_item, &root->root_key);
	BUG_ON(ret);
	root->node = read_root_block(root,
				     btrfs_root_bytenr(&root->root_item),
				     root->root_item.level);
	BUG_ON(!root->node);
	return 0;
}

struct btrfs_root *open_ctree(char *filename, struct btrfs_super_block *super)
{
	int fp;

	fp = open(filename, O_CREAT | O_RDWR, 0600);
	if (fp < 0) {
		return NULL;
	}
	return open_ctree_fd(fp, super);
}

struct btrfs_root *open_ctree_fd(int fp, struct btrfs_super_block *super)
{
	struct btrfs_root *root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *extent_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_root *tree_root = malloc(sizeof(struct btrfs_root));
	struct btrfs_fs_info *fs_info = malloc(sizeof(*fs_info));
	int ret;

	INIT_LIST_HEAD(&fs_info->trans);
	INIT_LIST_HEAD(&fs_info->cache);
	cache_tree_init(&fs_info->extent_cache);
	cache_tree_init(&fs_info->pending_tree);
	cache_tree_init(&fs_info->pinned_tree);
	cache_tree_init(&fs_info->del_pending);
	cache_tree_init(&fs_info->block_group_cache);
	fs_info->cache_size = 0;
	fs_info->fp = fp;
	fs_info->running_transaction = NULL;
	fs_info->fs_root = root;
	fs_info->tree_root = tree_root;
	fs_info->extent_root = extent_root;
	fs_info->last_inode_alloc = 0;
	fs_info->last_inode_alloc_dirid = 0;
	fs_info->disk_super = super;
	memset(&fs_info->last_insert, 0, sizeof(fs_info->last_insert));

	ret = pread(fp, super, sizeof(struct btrfs_super_block),
		     BTRFS_SUPER_INFO_OFFSET);
	if (ret == 0 || btrfs_super_root(super) == 0) {
		BUG();
		return NULL;
	}
	BUG_ON(ret < 0);

	__setup_root(super, tree_root, fs_info, BTRFS_ROOT_TREE_OBJECTID, fp);
	tree_root->node = read_root_block(tree_root, btrfs_super_root(super),
					  btrfs_super_root_level(super));
	BUG_ON(!tree_root->node);

	ret = find_and_setup_root(super, tree_root, fs_info,
				  BTRFS_EXTENT_TREE_OBJECTID, extent_root, fp);
	BUG_ON(ret);

	ret = find_and_setup_root(super, tree_root, fs_info,
				  BTRFS_FS_TREE_OBJECTID, root, fp);
	BUG_ON(ret);

	root->commit_root = root->node;
	root->node->count++;
	root->ref_cows = 1;
	root->fs_info->generation = root->root_key.offset + 1;
	btrfs_read_block_groups(root);
	return root;
}

int write_ctree_super(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_super_block *s)
{
	int ret;

	btrfs_set_super_root(s, root->fs_info->tree_root->node->bytenr);
	btrfs_set_super_root_level(s,
	      btrfs_header_level(&root->fs_info->tree_root->node->node.header));
	btrfs_csum_super(root, s);

	ret = pwrite(root->fs_info->fp, s, sizeof(*s),
		     BTRFS_SUPER_INFO_OFFSET);
	if (ret != sizeof(*s)) {
		fprintf(stderr, "failed to write new super block err %d\n", ret);
		return ret;
	}
	return 0;
}

static int drop_cache(struct btrfs_root *root)
{
	while(!list_empty(&root->fs_info->cache)) {
		struct btrfs_buffer *b = list_entry(root->fs_info->cache.next,
						    struct btrfs_buffer,
						    cache);
		list_del_init(&b->cache);
		btrfs_block_release(root, b);
	}
	return 0;
}

int close_ctree(struct btrfs_root *root, struct btrfs_super_block *s)
{
	int ret;
	struct btrfs_trans_handle *trans;

	trans = root->fs_info->running_transaction;
	btrfs_commit_transaction(trans, root, s);
	ret = commit_tree_roots(trans, root->fs_info);
	BUG_ON(ret);
	ret = __commit_transaction(trans, root);
	BUG_ON(ret);
	write_ctree_super(trans, root, s);
	drop_cache(root);
	BUG_ON(!list_empty(&root->fs_info->trans));

	btrfs_free_block_groups(root->fs_info);
	close(root->fs_info->fp);
	if (root->node)
		btrfs_block_release(root, root->node);
	if (root->fs_info->extent_root->node)
		btrfs_block_release(root->fs_info->extent_root,
				    root->fs_info->extent_root->node);
	if (root->fs_info->tree_root->node)
		btrfs_block_release(root->fs_info->tree_root,
				    root->fs_info->tree_root->node);
	btrfs_block_release(root, root->commit_root);
	free(root);
	printf("on close %llu blocks are allocated\n",
	       (unsigned long long)allocated_bytes);
	return 0;
}

void btrfs_block_release(struct btrfs_root *root, struct btrfs_buffer *buf)
{
	buf->count--;
	if (buf->count < 0)
		BUG();
	if (buf->count == 0) {
		BUG_ON(!list_empty(&buf->cache));
		BUG_ON(!list_empty(&buf->dirty));

		remove_cache_extent(&root->fs_info->extent_cache,
				    &buf->cache_node);
		BUG_ON(allocated_bytes == 0);
		allocated_bytes -= buf->size;
		BUG_ON(root->fs_info->cache_size == 0);
		root->fs_info->cache_size -= buf->size;

		memset(buf, 0, sizeof(*buf));
		free(buf);
	}
}

