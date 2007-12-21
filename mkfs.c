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

#define _XOPEN_SOURCE 500
#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <linux/fs.h>
#include <ctype.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"

#ifdef __CHECKER__
#define BLKGETSIZE64 0
static inline int ioctl(int fd, int define, u64 *size) { return 0; }
#endif

static u64 parse_size(char *s)
{
	int len = strlen(s);
	char c;
	u64 mult = 1;

	if (!isdigit(s[len - 1])) {
		c = tolower(s[len - 1]);
		switch (c) {
		case 'g':
			mult *= 1024;
		case 'm':
			mult *= 1024;
		case 'k':
			mult *= 1024;
		case 'b':
			break;
		default:
			fprintf(stderr, "Unknown size descriptor %c\n", c);
			exit(1);
		}
		s[len - 1] = '\0';
	}
	return atol(s) * mult;
}

static int __make_root_dir(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root, u64 objectid)
{
	int ret;
	char buf[8];
	struct btrfs_key inode_map;
	struct btrfs_inode_item inode_item;

	buf[0] = '.';
	buf[1] = '.';

	inode_map.objectid = objectid;
	btrfs_set_key_type(&inode_map, BTRFS_INODE_ITEM_KEY);
	inode_map.offset = 0;

	memset(&inode_item, 0, sizeof(inode_item));
	btrfs_set_inode_generation(&inode_item, root->fs_info->generation);
	btrfs_set_inode_size(&inode_item, 0);
	btrfs_set_inode_nlink(&inode_item, 1);
	btrfs_set_inode_nblocks(&inode_item, 0);
	btrfs_set_inode_mode(&inode_item, S_IFDIR | 0555);

	if (root->fs_info->tree_root == root)
		btrfs_set_super_root_dir(root->fs_info->disk_super, objectid);

	ret = btrfs_insert_inode(trans, root, objectid, &inode_item);
	if (ret)
		goto error;

	ret = btrfs_insert_inode_ref(trans, root, "..", 2, objectid, objectid);
	if (ret)
		goto error;
	btrfs_set_root_dirid(&root->root_item, objectid);
	ret = 0;
error:
	return ret;
}

static int make_block_groups(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root)
{
	u64 group_size;
	u64 total_bytes;
	u64 cur_start;
	int ret;
	u64 nr = 0;
	struct btrfs_block_group_cache *cache;
	struct cache_tree *bg_cache = &root->fs_info->block_group_cache;

	root = root->fs_info->extent_root;

	/* first we bootstrap the things into cache */
	group_size = BTRFS_BLOCK_GROUP_SIZE;
	cache = malloc(sizeof(*cache));
	cache->key.objectid = 0;
	cache->key.offset = group_size;
	cache->cache.start = 0;
	cache->cache.size = group_size;
	btrfs_set_key_type(&cache->key, BTRFS_BLOCK_GROUP_ITEM_KEY);

	memset(&cache->item, 0, sizeof(cache->item));
	btrfs_set_block_group_used(&cache->item,
			   btrfs_super_bytes_used(root->fs_info->disk_super));
	ret = insert_existing_cache_extent(bg_cache, &cache->cache);
	BUG_ON(ret);

	total_bytes = btrfs_super_total_bytes(root->fs_info->disk_super);
	cur_start = group_size;
	while(cur_start < total_bytes) {
		cache = malloc(sizeof(*cache));
		cache->key.objectid = cur_start;
		cache->key.offset = group_size;
		cache->cache.start = cur_start;
		cache->cache.size = group_size;
		btrfs_set_key_type(&cache->key, BTRFS_BLOCK_GROUP_ITEM_KEY);
		memset(&cache->item, 0, sizeof(cache->item));
		if (nr % 3)
			cache->item.flags |= BTRFS_BLOCK_GROUP_DATA;

		ret = insert_existing_cache_extent(bg_cache, &cache->cache);
		BUG_ON(ret);
		cur_start += group_size;
		nr++;
	}
	/* then insert all the items */
	cur_start = 0;
	while(cur_start < total_bytes) {
		struct cache_extent *ce;
		ce = find_first_cache_extent(bg_cache, cur_start);
		BUG_ON(!ce);
		cache = container_of(ce, struct btrfs_block_group_cache,
					cache);
		ret = btrfs_insert_block_group(trans, root, &cache->key,
					       &cache->item);
		BUG_ON(ret);
		cur_start += group_size;
	}
	return 0;
}

static int make_root_dir(int fd) {
	struct btrfs_root *root;
	struct btrfs_super_block super;
	struct btrfs_trans_handle *trans;
	int ret;
	struct btrfs_key location;

	root = open_ctree_fd(fd, &super);

	if (!root) {
		fprintf(stderr, "ctree init failed\n");
		return -1;
	}
	trans = btrfs_start_transaction(root, 1);
	ret = make_block_groups(trans, root);
	ret = __make_root_dir(trans, root->fs_info->tree_root,
			      BTRFS_ROOT_TREE_DIR_OBJECTID);
	if (ret)
		goto err;
	ret = __make_root_dir(trans, root, BTRFS_FIRST_FREE_OBJECTID);
	if (ret)
		goto err;
	memcpy(&location, &root->fs_info->fs_root->root_key, sizeof(location));
	location.offset = (u64)-1;
	ret = btrfs_insert_dir_item(trans, root->fs_info->tree_root,
			"default", 7,
			btrfs_super_root_dir(root->fs_info->disk_super),
			&location, BTRFS_FT_DIR);
	if (ret)
		goto err;

	ret = btrfs_insert_inode_ref(trans, root->fs_info->tree_root,
			     "default", 7, location.objectid,
			     BTRFS_ROOT_TREE_DIR_OBJECTID);
	if (ret)
		goto err;

	btrfs_commit_transaction(trans, root, root->fs_info->disk_super);
	ret = close_ctree(root, &super);
err:
	return ret;
}

int mkfs(int fd, char *pathname, u64 num_bytes, u32 nodesize, u32 leafsize,
	 u32 sectorsize, u32 stripesize)
{
	struct btrfs_super_block super;
	struct btrfs_leaf *empty_leaf;
	struct btrfs_root_item root_item;
	struct btrfs_item item;
	struct btrfs_extent_item extent_item;
	struct btrfs_inode_item *inode_item;
	char *block;
	int ret;
	u32 itemoff;
	u32 start_block = BTRFS_SUPER_INFO_OFFSET;
	u32 first_free = BTRFS_SUPER_INFO_OFFSET + sectorsize;

	btrfs_set_super_generation(&super, 1);
	btrfs_set_super_bytenr(&super, start_block);
	btrfs_set_super_root_level(&super, 0);
	btrfs_set_super_root(&super, first_free);
	strcpy((char *)(&super.magic), BTRFS_MAGIC);

printf("blocksize is %d\n", leafsize);
	btrfs_set_super_sectorsize(&super, sectorsize);
	btrfs_set_super_leafsize(&super, leafsize);
	btrfs_set_super_nodesize(&super, nodesize);
	btrfs_set_super_stripesize(&super, stripesize);

	num_bytes = (num_bytes / sectorsize) * sectorsize;
	btrfs_set_super_total_bytes(&super, num_bytes);
	btrfs_set_super_bytes_used(&super, start_block + 3 * leafsize +
				   sectorsize);
	uuid_generate(super.fsid);

	block = malloc(sectorsize);
	memset(block, 0, sectorsize);
	BUG_ON(sizeof(super) > sectorsize);
	memcpy(block, &super, sizeof(super));
	ret = pwrite(fd, block, sectorsize, BTRFS_SUPER_INFO_OFFSET);
	BUG_ON(ret != sectorsize);

	/* create the tree of root objects */
	empty_leaf = malloc(leafsize);
	memset(empty_leaf, 0, leafsize);
	btrfs_set_header_bytenr(&empty_leaf->header, first_free);
	btrfs_set_header_nritems(&empty_leaf->header, 2);
	btrfs_set_header_generation(&empty_leaf->header, 1);
	btrfs_set_header_owner(&empty_leaf->header, BTRFS_ROOT_TREE_OBJECTID);
	memcpy(empty_leaf->header.fsid, super.fsid,
	       sizeof(empty_leaf->header.fsid));

	/* create the items for the root tree */
	inode_item = &root_item.inode;
	memset(inode_item, 0, sizeof(*inode_item));
	btrfs_set_inode_generation(inode_item, 1);
	btrfs_set_inode_size(inode_item, 3);
	btrfs_set_inode_nlink(inode_item, 1);
	btrfs_set_inode_nblocks(inode_item, 1);
	btrfs_set_inode_mode(inode_item, S_IFDIR | 0755);

	// memset(&root_item, 0, sizeof(root_item));
	btrfs_set_root_dirid(&root_item, 0);
	btrfs_set_root_refs(&root_item, 1);
	btrfs_set_disk_key_offset(&item.key, 0);
	btrfs_set_item_size(&item, sizeof(root_item));
	btrfs_set_disk_key_type(&item.key, BTRFS_ROOT_ITEM_KEY);

	itemoff = __BTRFS_LEAF_DATA_SIZE(leafsize) - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, first_free + leafsize);
	root_item.level = 0;
	btrfs_set_item_offset(&item, itemoff);
	btrfs_set_disk_key_objectid(&item.key, BTRFS_EXTENT_TREE_OBJECTID);
	memcpy(empty_leaf->items, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + itemoff,
		&root_item, sizeof(root_item));

	btrfs_set_root_bytenr(&root_item, first_free + leafsize * 2);
	btrfs_set_root_bytes_used(&root_item, 1);
	itemoff = itemoff - sizeof(root_item);
	btrfs_set_item_offset(&item, itemoff);
	btrfs_set_disk_key_objectid(&item.key, BTRFS_FS_TREE_OBJECTID);
	memcpy(empty_leaf->items + 1, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + itemoff,
		&root_item, sizeof(root_item));
	ret = pwrite(fd, empty_leaf, leafsize, first_free);

	/* create the items for the extent tree */
	btrfs_set_header_bytenr(&empty_leaf->header, first_free + leafsize);
	btrfs_set_header_nritems(&empty_leaf->header, 4);

	/* item1, reserve blocks 0-16 */
	btrfs_set_disk_key_objectid(&item.key, 0);
	btrfs_set_disk_key_offset(&item.key, first_free);
	btrfs_set_disk_key_type(&item.key, 0);
	btrfs_set_disk_key_type(&item.key, BTRFS_EXTENT_ITEM_KEY);
	itemoff = __BTRFS_LEAF_DATA_SIZE(leafsize) -
			sizeof(struct btrfs_extent_item);
	btrfs_set_item_offset(&item, itemoff);
	btrfs_set_item_size(&item, sizeof(struct btrfs_extent_item));
	btrfs_set_extent_refs(&extent_item, 1);
	memcpy(empty_leaf->items, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + btrfs_item_offset(&item),
		&extent_item, btrfs_item_size(&item));

	/* item2, give block 17 to the root */
	btrfs_set_disk_key_objectid(&item.key, first_free);
	btrfs_set_disk_key_offset(&item.key, leafsize);
	itemoff = itemoff - sizeof(struct btrfs_extent_item);
	btrfs_set_item_offset(&item, itemoff);
	memcpy(empty_leaf->items + 1, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + btrfs_item_offset(&item),
		&extent_item, btrfs_item_size(&item));

	/* item3, give block 18 to the extent root */
	btrfs_set_disk_key_objectid(&item.key, first_free + leafsize);
	btrfs_set_disk_key_offset(&item.key, leafsize);
	itemoff = itemoff - sizeof(struct btrfs_extent_item);
	btrfs_set_item_offset(&item, itemoff);
	memcpy(empty_leaf->items + 2, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + btrfs_item_offset(&item),
		&extent_item, btrfs_item_size(&item));

	/* item4, give block 19 to the FS root */
	btrfs_set_disk_key_objectid(&item.key, first_free + leafsize * 2);
	btrfs_set_disk_key_offset(&item.key, leafsize);
	itemoff = itemoff - sizeof(struct btrfs_extent_item);
	btrfs_set_item_offset(&item, itemoff);
	memcpy(empty_leaf->items + 3, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + btrfs_item_offset(&item),
		&extent_item, btrfs_item_size(&item));
	ret = pwrite(fd, empty_leaf, leafsize, first_free + leafsize);
	if (ret != leafsize)
		return -1;

	/* finally create the FS root */
	btrfs_set_header_bytenr(&empty_leaf->header, first_free + leafsize * 2);
	btrfs_set_header_nritems(&empty_leaf->header, 0);
	ret = pwrite(fd, empty_leaf, leafsize, first_free + leafsize * 2);
	if (ret != leafsize)
		return -1;
	return 0;
}

u64 device_size(int fd, struct stat *st)
{
	u64 size;
	if (S_ISREG(st->st_mode)) {
		return st->st_size;
	}
	if (!S_ISBLK(st->st_mode)) {
		return 0;
	}
	if (ioctl(fd, BLKGETSIZE64, &size) >= 0) {
		return size;
	}
	return 0;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: mkfs.btrfs [ -l leafsize ] [ -n nodesize] dev [ blocks ]\n");
	exit(1);
}

int main(int ac, char **av)
{
	char *file;
	u64 block_count = 0;
	int fd;
	struct stat st;
	int ret;
	int i;
	u32 leafsize = 16 * 1024;
	u32 sectorsize = 4096;
	u32 nodesize = 16 * 1024;
	u32 stripesize = 4096;
	char *buf = malloc(sectorsize);
	char *realpath_name;

	while(1) {
		int c;
		c = getopt(ac, av, "l:n:s:");
		if (c < 0)
			break;
		switch(c) {
			case 'l':
				leafsize = parse_size(optarg);
				break;
			case 'n':
				nodesize = parse_size(optarg);
				break;
			case 's':
				stripesize = parse_size(optarg);
				break;
			default:
				print_usage();
		}
	}
	if (leafsize < sectorsize || (leafsize & (sectorsize - 1))) {
		fprintf(stderr, "Illegal leafsize %u\n", leafsize);
		exit(1);
	}
	if (nodesize < sectorsize || (nodesize & (sectorsize - 1))) {
		fprintf(stderr, "Illegal nodesize %u\n", nodesize);
		exit(1);
	}
	ac = ac - optind;
	if (ac >= 1) {
		file = av[optind];
		if (ac == 2) {
			block_count = parse_size(av[optind + 1]);
			if (!block_count) {
				fprintf(stderr, "error finding block count\n");
				exit(1);
			}
		}
	} else {
		print_usage();
	}
	fd = open(file, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "unable to open %s\n", file);
		exit(1);
	}
	ret = fstat(fd, &st);
	if (ret < 0) {
		fprintf(stderr, "unable to stat %s\n", file);
		exit(1);
	}
	if (block_count == 0) {
		block_count = device_size(fd, &st);
		if (block_count == 0) {
			fprintf(stderr, "unable to find %s size\n", file);
			exit(1);
		}
	}
	block_count /= sectorsize;
	block_count *= sectorsize;

	if (block_count < 256 * 1024 * 1024) {
		fprintf(stderr, "device %s is too small\n", file);
		exit(1);
	}
	memset(buf, 0, sectorsize);
	for(i = 0; i < 64; i++) {
		ret = write(fd, buf, sectorsize);
		if (ret != sectorsize) {
			fprintf(stderr, "unable to zero fill device\n");
			exit(1);
		}
	}
	realpath_name = realpath(file, NULL);
	ret = mkfs(fd, realpath_name, block_count, nodesize, leafsize,
		   sectorsize, stripesize);
	if (ret) {
		fprintf(stderr, "error during mkfs %d\n", ret);
		exit(1);
	}
	ret = make_root_dir(fd);
	if (ret) {
		fprintf(stderr, "failed to setup the root directory\n");
		exit(1);
	}
	printf("fs created on %s nodesize %u leafsize %u sectorsize %u bytes %llu\n",
	       file, nodesize, leafsize, sectorsize,
	       (unsigned long long)block_count);
	return 0;
}

