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
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"

#ifdef __CHECKER__
#define BLKGETSIZE64 0
static inline int ioctl(int fd, int define, u64 *size) { return 0; }
#endif

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
	inode_map.flags = 0;
	btrfs_set_key_type(&inode_map, BTRFS_INODE_ITEM_KEY);
	inode_map.offset = 0;

	memset(&inode_item, 0, sizeof(inode_item));
	btrfs_set_inode_generation(&inode_item, root->fs_info->generation);
	btrfs_set_inode_size(&inode_item, 6);
	btrfs_set_inode_nlink(&inode_item, 1);
	btrfs_set_inode_nblocks(&inode_item, 1);
	btrfs_set_inode_mode(&inode_item, S_IFDIR | 0555);

	if (root->fs_info->tree_root == root)
		btrfs_set_super_root_dir(root->fs_info->disk_super, objectid);

	ret = btrfs_insert_inode(trans, root, objectid, &inode_item);
	if (ret)
		goto error;
	ret = btrfs_insert_dir_item(trans, root, buf, 1, objectid,
				    &inode_map, 1);
	if (ret)
		goto error;
	ret = btrfs_insert_dir_item(trans, root, buf, 2, objectid,
				    &inode_map, 1);
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
	u64 group_size_blocks;
	u64 total_blocks;
	u64 cur_start;
	int ret;
	struct btrfs_block_group_cache *cache;

	root = root->fs_info->extent_root;
	/* first we bootstrap the things into cache */
	group_size_blocks = BTRFS_BLOCK_GROUP_SIZE / root->blocksize;
	cache = malloc(sizeof(*cache));
	cache->key.objectid = 0;
	cache->key.offset = group_size_blocks;
	cache->key.flags = 0;
	btrfs_set_key_type(&cache->key, BTRFS_BLOCK_GROUP_ITEM_KEY);
	memset(&cache->item, 0, sizeof(cache->item));
	btrfs_set_block_group_used(&cache->item,
			   btrfs_super_blocks_used(root->fs_info->disk_super));
	ret = radix_tree_insert(&root->fs_info->block_group_radix,
				group_size_blocks - 1, (void *)cache);
	BUG_ON(ret);

	total_blocks = btrfs_super_total_blocks(root->fs_info->disk_super);
	cur_start = group_size_blocks;
	while(cur_start < total_blocks) {
		cache = malloc(sizeof(*cache));
		cache->key.objectid = cur_start;
		cache->key.offset = group_size_blocks;
		cache->key.flags = 0;
		btrfs_set_key_type(&cache->key, BTRFS_BLOCK_GROUP_ITEM_KEY);
		memset(&cache->item, 0, sizeof(cache->item));
		ret = radix_tree_insert(&root->fs_info->block_group_radix,
					cur_start + group_size_blocks - 1,
					(void *)cache);
		BUG_ON(ret);
		cur_start += group_size_blocks;
	}
	/* then insert all the items */
	cur_start = 0;
	while(cur_start < total_blocks) {
		cache = radix_tree_lookup(&root->fs_info->block_group_radix,
					  cur_start + group_size_blocks - 1);
		BUG_ON(!cache);
		ret = btrfs_insert_block_group(trans, root, &cache->key,
					       &cache->item);
		BUG_ON(ret);
		cur_start += group_size_blocks;
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
			"default", strlen("default"),
			btrfs_super_root_dir(root->fs_info->disk_super),
			&location, 1);
	if (ret)
		goto err;
	btrfs_commit_transaction(trans, root, root->fs_info->disk_super);
	ret = close_ctree(root, &super);
err:
	return ret;
}

int mkfs(int fd, char *pathname, u64 num_blocks, u32 blocksize)
{
	struct btrfs_super_block super;
	struct btrfs_leaf *empty_leaf;
	struct btrfs_root_item root_item;
	struct btrfs_item item;
	struct btrfs_extent_item extent_item;
	struct btrfs_inode_item *inode_item;
	struct btrfs_device_item dev_item;
	char *block;
	int ret;
	u32 itemoff;
	u32 start_block = BTRFS_SUPER_INFO_OFFSET / blocksize;
	u16 item_size;

	btrfs_set_super_generation(&super, 1);
	btrfs_set_super_blocknr(&super, start_block);
	btrfs_set_super_root(&super, start_block + 1);
	strcpy((char *)(&super.magic), BTRFS_MAGIC);
	btrfs_set_super_blocksize(&super, blocksize);
	btrfs_set_super_total_blocks(&super, num_blocks);
	btrfs_set_super_blocks_used(&super, start_block + 5);
	btrfs_set_super_device_block_start(&super, 0);
	btrfs_set_super_device_num_blocks(&super, num_blocks);
	btrfs_set_super_device_root(&super, start_block + 2);
	btrfs_set_super_device_id(&super, 1);
	btrfs_set_super_last_device_id(&super, 1);
	uuid_generate(super.fsid);

	block = malloc(blocksize);
	memset(block, 0, blocksize);
	BUG_ON(sizeof(super) > blocksize);
	memcpy(block, &super, sizeof(super));
	ret = pwrite(fd, block, blocksize, BTRFS_SUPER_INFO_OFFSET);
	BUG_ON(ret != blocksize);

	/* create the tree of root objects */
	empty_leaf = malloc(blocksize);
	memset(empty_leaf, 0, blocksize);
	btrfs_set_header_blocknr(&empty_leaf->header, start_block + 1);
	btrfs_set_header_nritems(&empty_leaf->header, 2);
	btrfs_set_header_generation(&empty_leaf->header, 0);
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

	btrfs_set_root_dirid(&root_item, 0);
	btrfs_set_root_refs(&root_item, 1);
	btrfs_set_disk_key_offset(&item.key, 0);
	btrfs_set_disk_key_flags(&item.key, 0);
	btrfs_set_item_size(&item, sizeof(root_item));
	btrfs_set_disk_key_type(&item.key, BTRFS_ROOT_ITEM_KEY);

	itemoff = __BTRFS_LEAF_DATA_SIZE(blocksize) - sizeof(root_item);
	btrfs_set_root_blocknr(&root_item, start_block + 3);
	btrfs_set_item_offset(&item, itemoff);
	btrfs_set_disk_key_objectid(&item.key, BTRFS_EXTENT_TREE_OBJECTID);
	memcpy(empty_leaf->items, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + itemoff,
		&root_item, sizeof(root_item));

	btrfs_set_root_blocknr(&root_item, start_block + 4);
	itemoff = itemoff - sizeof(root_item);
	btrfs_set_item_offset(&item, itemoff);
	btrfs_set_disk_key_objectid(&item.key, BTRFS_FS_TREE_OBJECTID);
	memcpy(empty_leaf->items + 1, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + itemoff,
		&root_item, sizeof(root_item));
	ret = pwrite(fd, empty_leaf, blocksize, (start_block + 1) * blocksize);

	/* create the item for the dev tree */
	btrfs_set_header_blocknr(&empty_leaf->header, start_block + 2);
	btrfs_set_header_nritems(&empty_leaf->header, 1);
	btrfs_set_disk_key_objectid(&item.key, 0);
	btrfs_set_disk_key_offset(&item.key, num_blocks);
	btrfs_set_disk_key_flags(&item.key, 0);
	btrfs_set_disk_key_type(&item.key, BTRFS_DEV_ITEM_KEY);

	item_size = sizeof(struct btrfs_device_item) + strlen(pathname);
	itemoff = __BTRFS_LEAF_DATA_SIZE(blocksize) - item_size;
	btrfs_set_item_offset(&item, itemoff);
	btrfs_set_item_size(&item, item_size);
	btrfs_set_device_pathlen(&dev_item, strlen(pathname));
	btrfs_set_device_id(&dev_item, 1);
	memcpy(empty_leaf->items, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + itemoff, &dev_item,
	       sizeof(dev_item));
	memcpy(btrfs_leaf_data(empty_leaf) + itemoff + sizeof(dev_item),
	       pathname, strlen(pathname));
	ret = pwrite(fd, empty_leaf, blocksize, (start_block + 2) * blocksize);
	if (ret != blocksize)
		return -1;

	/* create the items for the extent tree */
	btrfs_set_header_blocknr(&empty_leaf->header, start_block + 3);
	btrfs_set_header_nritems(&empty_leaf->header, 5);

	/* item1, reserve blocks 0-16 */
	btrfs_set_disk_key_objectid(&item.key, 0);
	btrfs_set_disk_key_offset(&item.key, start_block + 1);
	btrfs_set_disk_key_flags(&item.key, 0);
	btrfs_set_disk_key_type(&item.key, BTRFS_EXTENT_ITEM_KEY);
	itemoff = __BTRFS_LEAF_DATA_SIZE(blocksize) -
			sizeof(struct btrfs_extent_item);
	btrfs_set_item_offset(&item, itemoff);
	btrfs_set_item_size(&item, sizeof(struct btrfs_extent_item));
	btrfs_set_extent_refs(&extent_item, 1);
	btrfs_set_extent_owner(&extent_item, BTRFS_ROOT_TREE_OBJECTID);
	memcpy(empty_leaf->items, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + btrfs_item_offset(&item),
		&extent_item, btrfs_item_size(&item));

	/* item2, give block 17 to the root */
	btrfs_set_disk_key_objectid(&item.key, start_block + 1);
	btrfs_set_disk_key_offset(&item.key, 1);
	itemoff = itemoff - sizeof(struct btrfs_extent_item);
	btrfs_set_item_offset(&item, itemoff);
	memcpy(empty_leaf->items + 1, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + btrfs_item_offset(&item),
		&extent_item, btrfs_item_size(&item));

	/* item3, give block 18 to the dev root */
	btrfs_set_disk_key_objectid(&item.key, start_block + 2);
	btrfs_set_disk_key_offset(&item.key, 1);
	itemoff = itemoff - sizeof(struct btrfs_extent_item);
	btrfs_set_item_offset(&item, itemoff);
	memcpy(empty_leaf->items + 2, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + btrfs_item_offset(&item),
		&extent_item, btrfs_item_size(&item));

	/* item4, give block 19 to the extent root */
	btrfs_set_disk_key_objectid(&item.key, start_block + 3);
	btrfs_set_disk_key_offset(&item.key, 1);
	itemoff = itemoff - sizeof(struct btrfs_extent_item);
	btrfs_set_item_offset(&item, itemoff);
	memcpy(empty_leaf->items + 3, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + btrfs_item_offset(&item),
		&extent_item, btrfs_item_size(&item));

	/* item5, give block 20 to the FS root */
	btrfs_set_disk_key_objectid(&item.key, start_block + 4);
	btrfs_set_disk_key_offset(&item.key, 1);
	itemoff = itemoff - sizeof(struct btrfs_extent_item);
	btrfs_set_item_offset(&item, itemoff);
	memcpy(empty_leaf->items + 4, &item, sizeof(item));
	memcpy(btrfs_leaf_data(empty_leaf) + btrfs_item_offset(&item),
		&extent_item, btrfs_item_size(&item));
	ret = pwrite(fd, empty_leaf, blocksize, (start_block + 3) * blocksize);
	if (ret != blocksize)
		return -1;

	/* finally create the FS root */
	btrfs_set_header_blocknr(&empty_leaf->header, start_block + 4);
	btrfs_set_header_nritems(&empty_leaf->header, 0);
	ret = pwrite(fd, empty_leaf, blocksize, (start_block + 4) * blocksize);
	if (ret != blocksize)
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
	return 0; }

int main(int ac, char **av)
{
	char *file;
	u64 block_count = 0;
	int fd;
	struct stat st;
	int ret;
	int i;
	char *buf = malloc(4096);
	char *realpath_name;

	radix_tree_init();

	if (ac >= 2) {
		file = av[1];
		if (ac == 3) {
			block_count = atoi(av[2]);
			if (!block_count) {
				fprintf(stderr, "error finding block count\n");
				exit(1);
			}
		}
	} else {
		fprintf(stderr, "usage: mkfs.btrfs file [block count]\n");
		exit(1);
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
	block_count /= 4096;
	if (block_count < 256) {
		fprintf(stderr, "device %s is too small\n", file);
		exit(1);
	}
	memset(buf, 0, 4096);
	for(i = 0; i < 64; i++) {
		ret = write(fd, buf, 4096);
		if (ret != 4096) {
			fprintf(stderr, "unable to zero fill device\n");
			exit(1);
		}
	}
	realpath_name = realpath(file, NULL);
	ret = mkfs(fd, realpath_name, block_count, 4096);
	if (ret) {
		fprintf(stderr, "error during mkfs %d\n", ret);
		exit(1);
	}
	ret = make_root_dir(fd);
	if (ret) {
		fprintf(stderr, "failed to setup the root directory\n");
		exit(1);
	}
	printf("fs created on %s blocksize %d blocks %Lu\n",
	       file, 4096, block_count);
	return 0;
}

