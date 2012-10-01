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
#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <uuid/uuid.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <mntent.h>
#include <linux/loop.h>
#include <linux/major.h>
#include <linux/kdev_t.h>
#include <limits.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "crc32c.h"
#include "utils.h"
#include "volumes.h"
#include "ioctl.h"

#ifdef __CHECKER__
#define BLKGETSIZE64 0
static inline int ioctl(int fd, int define, u64 *size) { return 0; }
#endif

#ifndef BLKDISCARD
#define BLKDISCARD	_IO(0x12,119)
#endif

static int
discard_blocks(int fd, u64 start, u64 len)
{
	u64 range[2] = { start, len };

	if (ioctl(fd, BLKDISCARD, &range) < 0)
		return errno;
	return 0;
}

static u64 reference_root_table[] = {
	[1] =	BTRFS_ROOT_TREE_OBJECTID,
	[2] =	BTRFS_EXTENT_TREE_OBJECTID,
	[3] =	BTRFS_CHUNK_TREE_OBJECTID,
	[4] =	BTRFS_DEV_TREE_OBJECTID,
	[5] =	BTRFS_FS_TREE_OBJECTID,
	[6] =	BTRFS_CSUM_TREE_OBJECTID,
};

int make_btrfs(int fd, const char *device, const char *label,
	       u64 blocks[7], u64 num_bytes, u32 nodesize,
	       u32 leafsize, u32 sectorsize, u32 stripesize)
{
	struct btrfs_super_block super;
	struct extent_buffer *buf;
	struct btrfs_root_item root_item;
	struct btrfs_disk_key disk_key;
	struct btrfs_extent_item *extent_item;
	struct btrfs_inode_item *inode_item;
	struct btrfs_chunk *chunk;
	struct btrfs_dev_item *dev_item;
	struct btrfs_dev_extent *dev_extent;
	u8 chunk_tree_uuid[BTRFS_UUID_SIZE];
	u8 *ptr;
	int i;
	int ret;
	u32 itemoff;
	u32 nritems = 0;
	u64 first_free;
	u64 ref_root;
	u32 array_size;
	u32 item_size;

	first_free = BTRFS_SUPER_INFO_OFFSET + sectorsize * 2 - 1;
	first_free &= ~((u64)sectorsize - 1);

	memset(&super, 0, sizeof(super));

	num_bytes = (num_bytes / sectorsize) * sectorsize;
	uuid_generate(super.fsid);
	uuid_generate(super.dev_item.uuid);
	uuid_generate(chunk_tree_uuid);

	btrfs_set_super_bytenr(&super, blocks[0]);
	btrfs_set_super_num_devices(&super, 1);
	strncpy((char *)&super.magic, BTRFS_MAGIC, sizeof(super.magic));
	btrfs_set_super_generation(&super, 1);
	btrfs_set_super_root(&super, blocks[1]);
	btrfs_set_super_chunk_root(&super, blocks[3]);
	btrfs_set_super_total_bytes(&super, num_bytes);
	btrfs_set_super_bytes_used(&super, 6 * leafsize);
	btrfs_set_super_sectorsize(&super, sectorsize);
	btrfs_set_super_leafsize(&super, leafsize);
	btrfs_set_super_nodesize(&super, nodesize);
	btrfs_set_super_stripesize(&super, stripesize);
	btrfs_set_super_csum_type(&super, BTRFS_CSUM_TYPE_CRC32);
	btrfs_set_super_chunk_root_generation(&super, 1);
	btrfs_set_super_cache_generation(&super, -1);
	if (label)
		strncpy(super.label, label, BTRFS_LABEL_SIZE - 1);

	buf = malloc(sizeof(*buf) + max(sectorsize, leafsize));

	/* create the tree of root objects */
	memset(buf->data, 0, leafsize);
	buf->len = leafsize;
	btrfs_set_header_bytenr(buf, blocks[1]);
	btrfs_set_header_nritems(buf, 4);
	btrfs_set_header_generation(buf, 1);
	btrfs_set_header_backref_rev(buf, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(buf, BTRFS_ROOT_TREE_OBJECTID);
	write_extent_buffer(buf, super.fsid, (unsigned long)
			    btrfs_header_fsid(buf), BTRFS_FSID_SIZE);

	write_extent_buffer(buf, chunk_tree_uuid, (unsigned long)
			    btrfs_header_chunk_tree_uuid(buf),
			    BTRFS_UUID_SIZE);

	/* create the items for the root tree */
	memset(&root_item, 0, sizeof(root_item));
	inode_item = &root_item.inode;
	btrfs_set_stack_inode_generation(inode_item, 1);
	btrfs_set_stack_inode_size(inode_item, 3);
	btrfs_set_stack_inode_nlink(inode_item, 1);
	btrfs_set_stack_inode_nbytes(inode_item, leafsize);
	btrfs_set_stack_inode_mode(inode_item, S_IFDIR | 0755);
	btrfs_set_root_refs(&root_item, 1);
	btrfs_set_root_used(&root_item, leafsize);
	btrfs_set_root_generation(&root_item, 1);

	memset(&disk_key, 0, sizeof(disk_key));
	btrfs_set_disk_key_type(&disk_key, BTRFS_ROOT_ITEM_KEY);
	btrfs_set_disk_key_offset(&disk_key, 0);
	nritems = 0;

	itemoff = __BTRFS_LEAF_DATA_SIZE(leafsize) - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, blocks[2]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf, nritems),
			    sizeof(root_item));
	write_extent_buffer(buf, &root_item, btrfs_item_ptr_offset(buf,
			    nritems), sizeof(root_item));
	nritems++;

	itemoff = itemoff - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, blocks[4]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_DEV_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf, nritems),
			    sizeof(root_item));
	write_extent_buffer(buf, &root_item,
			    btrfs_item_ptr_offset(buf, nritems),
			    sizeof(root_item));
	nritems++;

	itemoff = itemoff - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, blocks[5]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_FS_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf, nritems),
			    sizeof(root_item));
	write_extent_buffer(buf, &root_item,
			    btrfs_item_ptr_offset(buf, nritems),
			    sizeof(root_item));
	nritems++;

	itemoff = itemoff - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, blocks[6]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_CSUM_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf, nritems),
			    sizeof(root_item));
	write_extent_buffer(buf, &root_item,
			    btrfs_item_ptr_offset(buf, nritems),
			    sizeof(root_item));
	nritems++;


	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, leafsize, blocks[1]);
	BUG_ON(ret != leafsize);

	/* create the items for the extent tree */
	memset(buf->data+sizeof(struct btrfs_header), 0,
		leafsize-sizeof(struct btrfs_header));
	nritems = 0;
	itemoff = __BTRFS_LEAF_DATA_SIZE(leafsize);
	for (i = 1; i < 7; i++) {
		BUG_ON(blocks[i] < first_free);
		BUG_ON(blocks[i] < blocks[i - 1]);

		/* create extent item */
		itemoff -= sizeof(struct btrfs_extent_item) +
			   sizeof(struct btrfs_tree_block_info);
		btrfs_set_disk_key_objectid(&disk_key, blocks[i]);
		btrfs_set_disk_key_offset(&disk_key, leafsize);
		btrfs_set_disk_key_type(&disk_key, BTRFS_EXTENT_ITEM_KEY);
		btrfs_set_item_key(buf, &disk_key, nritems);
		btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems),
				      itemoff);
		btrfs_set_item_size(buf, btrfs_item_nr(buf, nritems),
				    sizeof(struct btrfs_extent_item) +
				    sizeof(struct btrfs_tree_block_info));
		extent_item = btrfs_item_ptr(buf, nritems,
					     struct btrfs_extent_item);
		btrfs_set_extent_refs(buf, extent_item, 1);
		btrfs_set_extent_generation(buf, extent_item, 1);
		btrfs_set_extent_flags(buf, extent_item,
				       BTRFS_EXTENT_FLAG_TREE_BLOCK);
		nritems++;

		/* create extent ref */
		ref_root = reference_root_table[i];
		btrfs_set_disk_key_objectid(&disk_key, blocks[i]);
		btrfs_set_disk_key_offset(&disk_key, ref_root);
		btrfs_set_disk_key_type(&disk_key, BTRFS_TREE_BLOCK_REF_KEY);
		btrfs_set_item_key(buf, &disk_key, nritems);
		btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems),
				      itemoff);
		btrfs_set_item_size(buf, btrfs_item_nr(buf, nritems), 0);
		nritems++;
	}
	btrfs_set_header_bytenr(buf, blocks[2]);
	btrfs_set_header_owner(buf, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, leafsize, blocks[2]);
	BUG_ON(ret != leafsize);

	/* create the chunk tree */
	memset(buf->data+sizeof(struct btrfs_header), 0,
		leafsize-sizeof(struct btrfs_header));
	nritems = 0;
	item_size = sizeof(*dev_item);
	itemoff = __BTRFS_LEAF_DATA_SIZE(leafsize) - item_size;

	/* first device 1 (there is no device 0) */
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_DEV_ITEMS_OBJECTID);
	btrfs_set_disk_key_offset(&disk_key, 1);
	btrfs_set_disk_key_type(&disk_key, BTRFS_DEV_ITEM_KEY);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf, nritems), item_size);

	dev_item = btrfs_item_ptr(buf, nritems, struct btrfs_dev_item);
	btrfs_set_device_id(buf, dev_item, 1);
	btrfs_set_device_generation(buf, dev_item, 0);
	btrfs_set_device_total_bytes(buf, dev_item, num_bytes);
	btrfs_set_device_bytes_used(buf, dev_item,
				    BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	btrfs_set_device_io_align(buf, dev_item, sectorsize);
	btrfs_set_device_io_width(buf, dev_item, sectorsize);
	btrfs_set_device_sector_size(buf, dev_item, sectorsize);
	btrfs_set_device_type(buf, dev_item, 0);

	write_extent_buffer(buf, super.dev_item.uuid,
			    (unsigned long)btrfs_device_uuid(dev_item),
			    BTRFS_UUID_SIZE);
	write_extent_buffer(buf, super.fsid,
			    (unsigned long)btrfs_device_fsid(dev_item),
			    BTRFS_UUID_SIZE);
	read_extent_buffer(buf, &super.dev_item, (unsigned long)dev_item,
			   sizeof(*dev_item));

	nritems++;
	item_size = btrfs_chunk_item_size(1);
	itemoff = itemoff - item_size;

	/* then we have chunk 0 */
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_disk_key_offset(&disk_key, 0);
	btrfs_set_disk_key_type(&disk_key, BTRFS_CHUNK_ITEM_KEY);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf,  nritems), item_size);

	chunk = btrfs_item_ptr(buf, nritems, struct btrfs_chunk);
	btrfs_set_chunk_length(buf, chunk, BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	btrfs_set_chunk_owner(buf, chunk, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_chunk_stripe_len(buf, chunk, 64 * 1024);
	btrfs_set_chunk_type(buf, chunk, BTRFS_BLOCK_GROUP_SYSTEM);
	btrfs_set_chunk_io_align(buf, chunk, sectorsize);
	btrfs_set_chunk_io_width(buf, chunk, sectorsize);
	btrfs_set_chunk_sector_size(buf, chunk, sectorsize);
	btrfs_set_chunk_num_stripes(buf, chunk, 1);
	btrfs_set_stripe_devid_nr(buf, chunk, 0, 1);
	btrfs_set_stripe_offset_nr(buf, chunk, 0, 0);
	nritems++;

	write_extent_buffer(buf, super.dev_item.uuid,
			    (unsigned long)btrfs_stripe_dev_uuid(&chunk->stripe),
			    BTRFS_UUID_SIZE);

	/* copy the key for the chunk to the system array */
	ptr = super.sys_chunk_array;
	array_size = sizeof(disk_key);

	memcpy(ptr, &disk_key, sizeof(disk_key));
	ptr += sizeof(disk_key);

	/* copy the chunk to the system array */
	read_extent_buffer(buf, ptr, (unsigned long)chunk, item_size);
	array_size += item_size;
	ptr += item_size;
	btrfs_set_super_sys_array_size(&super, array_size);

	btrfs_set_header_bytenr(buf, blocks[3]);
	btrfs_set_header_owner(buf, BTRFS_CHUNK_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, leafsize, blocks[3]);

	/* create the device tree */
	memset(buf->data+sizeof(struct btrfs_header), 0,
		leafsize-sizeof(struct btrfs_header));
	nritems = 0;
	itemoff = __BTRFS_LEAF_DATA_SIZE(leafsize) -
		sizeof(struct btrfs_dev_extent);

	btrfs_set_disk_key_objectid(&disk_key, 1);
	btrfs_set_disk_key_offset(&disk_key, 0);
	btrfs_set_disk_key_type(&disk_key, BTRFS_DEV_EXTENT_KEY);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(buf, nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(buf,  nritems),
			    sizeof(struct btrfs_dev_extent));
	dev_extent = btrfs_item_ptr(buf, nritems, struct btrfs_dev_extent);
	btrfs_set_dev_extent_chunk_tree(buf, dev_extent,
					BTRFS_CHUNK_TREE_OBJECTID);
	btrfs_set_dev_extent_chunk_objectid(buf, dev_extent,
					BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_dev_extent_chunk_offset(buf, dev_extent, 0);

	write_extent_buffer(buf, chunk_tree_uuid,
		    (unsigned long)btrfs_dev_extent_chunk_tree_uuid(dev_extent),
		    BTRFS_UUID_SIZE);

	btrfs_set_dev_extent_length(buf, dev_extent,
				    BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	nritems++;

	btrfs_set_header_bytenr(buf, blocks[4]);
	btrfs_set_header_owner(buf, BTRFS_DEV_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, leafsize, blocks[4]);

	/* create the FS root */
	memset(buf->data+sizeof(struct btrfs_header), 0,
		leafsize-sizeof(struct btrfs_header));
	btrfs_set_header_bytenr(buf, blocks[5]);
	btrfs_set_header_owner(buf, BTRFS_FS_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, 0);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, leafsize, blocks[5]);
	BUG_ON(ret != leafsize);

	/* finally create the csum root */
	memset(buf->data+sizeof(struct btrfs_header), 0,
		leafsize-sizeof(struct btrfs_header));
	btrfs_set_header_bytenr(buf, blocks[6]);
	btrfs_set_header_owner(buf, BTRFS_CSUM_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, 0);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, leafsize, blocks[6]);
	BUG_ON(ret != leafsize);

	/* and write out the super block */
	BUG_ON(sizeof(super) > sectorsize);
	memset(buf->data, 0, sectorsize);
	memcpy(buf->data, &super, sizeof(super));
	buf->len = sectorsize;
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, sectorsize, blocks[0]);
	BUG_ON(ret != sectorsize);


	free(buf);
	return 0;
}

static u64 device_size(int fd, struct stat *st)
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

static int zero_blocks(int fd, off_t start, size_t len)
{
	char *buf = malloc(len);
	int ret = 0;
	ssize_t written;

	if (!buf)
		return -ENOMEM;
	memset(buf, 0, len);
	written = pwrite(fd, buf, len, start);
	if (written != len)
		ret = -EIO;
	free(buf);
	return ret;
}

static int zero_dev_start(int fd)
{
	off_t start = 0;
	size_t len = 2 * 1024 * 1024;

#ifdef __sparc__
	/* don't overwrite the disk labels on sparc */
	start = 1024;
	len -= 1024;
#endif
	return zero_blocks(fd, start, len);
}

static int zero_dev_end(int fd, u64 dev_size)
{
	size_t len = 2 * 1024 * 1024;
	off_t start = dev_size - len;

	return zero_blocks(fd, start, len);
}

int btrfs_add_to_fsid(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, int fd, char *path,
		      u64 block_count, u32 io_width, u32 io_align,
		      u32 sectorsize)
{
	struct btrfs_super_block *disk_super;
	struct btrfs_super_block *super = &root->fs_info->super_copy;
	struct btrfs_device *device;
	struct btrfs_dev_item *dev_item;
	char *buf;
	u64 total_bytes;
	u64 num_devs;
	int ret;

	device = kmalloc(sizeof(*device), GFP_NOFS);
	if (!device)
		return -ENOMEM;
	buf = kmalloc(sectorsize, GFP_NOFS);
	if (!buf) {
		kfree(device);
		return -ENOMEM;
	}
	BUG_ON(sizeof(*disk_super) > sectorsize);
	memset(buf, 0, sectorsize);

	disk_super = (struct btrfs_super_block *)buf;
	dev_item = &disk_super->dev_item;

	uuid_generate(device->uuid);
	device->devid = 0;
	device->type = 0;
	device->io_width = io_width;
	device->io_align = io_align;
	device->sector_size = sectorsize;
	device->fd = fd;
	device->writeable = 1;
	device->total_bytes = block_count;
	device->bytes_used = 0;
	device->total_ios = 0;
	device->dev_root = root->fs_info->dev_root;

	ret = btrfs_add_device(trans, root, device);
	BUG_ON(ret);

	total_bytes = btrfs_super_total_bytes(super) + block_count;
	btrfs_set_super_total_bytes(super, total_bytes);

	num_devs = btrfs_super_num_devices(super) + 1;
	btrfs_set_super_num_devices(super, num_devs);

	memcpy(disk_super, super, sizeof(*disk_super));

	printf("adding device %s id %llu\n", path,
	       (unsigned long long)device->devid);

	btrfs_set_super_bytenr(disk_super, BTRFS_SUPER_INFO_OFFSET);
	btrfs_set_stack_device_id(dev_item, device->devid);
	btrfs_set_stack_device_type(dev_item, device->type);
	btrfs_set_stack_device_io_align(dev_item, device->io_align);
	btrfs_set_stack_device_io_width(dev_item, device->io_width);
	btrfs_set_stack_device_sector_size(dev_item, device->sector_size);
	btrfs_set_stack_device_total_bytes(dev_item, device->total_bytes);
	btrfs_set_stack_device_bytes_used(dev_item, device->bytes_used);
	memcpy(&dev_item->uuid, device->uuid, BTRFS_UUID_SIZE);

	ret = pwrite(fd, buf, sectorsize, BTRFS_SUPER_INFO_OFFSET);
	BUG_ON(ret != sectorsize);

	kfree(buf);
	list_add(&device->dev_list, &root->fs_info->fs_devices->devices);
	device->fs_devices = root->fs_info->fs_devices;
	return 0;
}

int btrfs_prepare_device(int fd, char *file, int zero_end, u64 *block_count_ret,
			   u64 max_block_count, int *mixed, int nodiscard)
{
	u64 block_count;
	u64 bytenr;
	struct stat st;
	int i, ret;

	ret = fstat(fd, &st);
	if (ret < 0) {
		fprintf(stderr, "unable to stat %s\n", file);
		exit(1);
	}

	block_count = device_size(fd, &st);
	if (block_count == 0) {
		fprintf(stderr, "unable to find %s size\n", file);
		exit(1);
	}
	if (max_block_count)
		block_count = min(block_count, max_block_count);
	zero_end = 1;

	if (block_count < 1024 * 1024 * 1024 && !(*mixed)) {
		printf("SMALL VOLUME: forcing mixed metadata/data groups\n");
		*mixed = 1;
	}

	if (!nodiscard) {
		/*
		 * We intentionally ignore errors from the discard ioctl.  It is
		 * not necessary for the mkfs functionality but just an optimization.
		 */
		discard_blocks(fd, 0, block_count);
	}

	ret = zero_dev_start(fd);
	if (ret) {
		fprintf(stderr, "failed to zero device start %d\n", ret);
		exit(1);
	}

	for (i = 0 ; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		if (bytenr >= block_count)
			break;
		zero_blocks(fd, bytenr, BTRFS_SUPER_INFO_SIZE);
	}

	if (zero_end) {
		ret = zero_dev_end(fd, block_count);
		if (ret) {
			fprintf(stderr, "failed to zero device end %d\n", ret);
			exit(1);
		}
	}
	*block_count_ret = block_count;
	return 0;
}

int btrfs_make_root_dir(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid)
{
	int ret;
	struct btrfs_inode_item inode_item;
	time_t now = time(NULL);

	memset(&inode_item, 0, sizeof(inode_item));
	btrfs_set_stack_inode_generation(&inode_item, trans->transid);
	btrfs_set_stack_inode_size(&inode_item, 0);
	btrfs_set_stack_inode_nlink(&inode_item, 1);
	btrfs_set_stack_inode_nbytes(&inode_item, root->leafsize);
	btrfs_set_stack_inode_mode(&inode_item, S_IFDIR | 0755);
	btrfs_set_stack_timespec_sec(&inode_item.atime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.atime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.ctime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.ctime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.mtime, now);
	btrfs_set_stack_timespec_nsec(&inode_item.mtime, 0);
	btrfs_set_stack_timespec_sec(&inode_item.otime, 0);
	btrfs_set_stack_timespec_nsec(&inode_item.otime, 0);

	if (root->fs_info->tree_root == root)
		btrfs_set_super_root_dir(&root->fs_info->super_copy, objectid);

	ret = btrfs_insert_inode(trans, root, objectid, &inode_item);
	if (ret)
		goto error;

	ret = btrfs_insert_inode_ref(trans, root, "..", 2, objectid, objectid, 0);
	if (ret)
		goto error;

	btrfs_set_root_dirid(&root->root_item, objectid);
	ret = 0;
error:
	return ret;
}

/* checks if a device is a loop device */
int is_loop_device (const char* device) {
	struct stat statbuf;

	if(stat(device, &statbuf) < 0)
		return -errno;

	return (S_ISBLK(statbuf.st_mode) &&
		MAJOR(statbuf.st_rdev) == LOOP_MAJOR);
}


/* Takes a loop device path (e.g. /dev/loop0) and returns
 * the associated file (e.g. /images/my_btrfs.img) */
int resolve_loop_device(const char* loop_dev, char* loop_file, int max_len)
{
	int loop_fd;
	int ret_ioctl;
	struct loop_info loopinfo;

	if ((loop_fd = open(loop_dev, O_RDONLY)) < 0)
		return -errno;

	ret_ioctl = ioctl(loop_fd, LOOP_GET_STATUS, &loopinfo);
	close(loop_fd);

	if (ret_ioctl == 0) {
		strncpy(loop_file, loopinfo.lo_name, max_len);
		if (max_len > 0)
			loop_file[max_len-1] = 0;
	} else
		return -errno;

	return 0;
}

/* Checks whether a and b are identical or device
 * files associated with the same block device
 */
int is_same_blk_file(const char* a, const char* b)
{
	struct stat st_buf_a, st_buf_b;
	char real_a[PATH_MAX];
	char real_b[PATH_MAX];

	if(!realpath(a, real_a) ||
	   !realpath(b, real_b))
	{
		return -errno;
	}

	/* Identical path? */
	if(strcmp(real_a, real_b) == 0)
		return 1;

	if(stat(a, &st_buf_a) < 0 ||
	   stat(b, &st_buf_b) < 0)
	{
		if (errno == ENOENT)
			return 0;
		return -errno;
	}

	/* Same blockdevice? */
	if(S_ISBLK(st_buf_a.st_mode) &&
	   S_ISBLK(st_buf_b.st_mode) &&
	   st_buf_a.st_rdev == st_buf_b.st_rdev)
	{
		return 1;
	}

	/* Hardlink? */
	if (st_buf_a.st_dev == st_buf_b.st_dev &&
	    st_buf_a.st_ino == st_buf_b.st_ino)
	{
		return 1;
	}

	return 0;
}

/* checks if a and b are identical or device
 * files associated with the same block device or
 * if one file is a loop device that uses the other
 * file.
 */
int is_same_loop_file(const char* a, const char* b)
{
	char res_a[PATH_MAX];
	char res_b[PATH_MAX];
	const char* final_a;
	const char* final_b;
	int ret;

	/* Resolve a if it is a loop device */
	if((ret = is_loop_device(a)) < 0) {
		if (ret == -ENOENT)
			return 0;
		return ret;
	} else if (ret) {
		if ((ret = resolve_loop_device(a, res_a, sizeof(res_a))) < 0)
			return ret;

		final_a = res_a;
	} else {
		final_a = a;
	}

	/* Resolve b if it is a loop device */
	if ((ret = is_loop_device(b)) < 0) {
		if (ret == -ENOENT)
			return 0;
		return ret;
	} else if (ret) {
		if((ret = resolve_loop_device(b, res_b, sizeof(res_b))) < 0)
			return ret;

		final_b = res_b;
	} else {
		final_b = b;
	}

	return is_same_blk_file(final_a, final_b);
}

/* Checks if a file exists and is a block or regular file*/
int is_existing_blk_or_reg_file(const char* filename)
{
	struct stat st_buf;

	if(stat(filename, &st_buf) < 0) {
		if(errno == ENOENT)
			return 0;
		else
			return -errno;
	}

	return (S_ISBLK(st_buf.st_mode) || S_ISREG(st_buf.st_mode));
}

/* Checks if a file is used (directly or indirectly via a loop device)
 * by a device in fs_devices
 */
int blk_file_in_dev_list(struct btrfs_fs_devices* fs_devices, const char* file)
{
	int ret;
	struct list_head *head;
	struct list_head *cur;
	struct btrfs_device *device;

	head = &fs_devices->devices;
	list_for_each(cur, head) {
		device = list_entry(cur, struct btrfs_device, dev_list);

		if((ret = is_same_loop_file(device->name, file)))
			return ret;
	}

	return 0;
}

/*
 * returns 1 if the device was mounted, < 0 on error or 0 if everything
 * is safe to continue.
 */
int check_mounted(const char* file)
{
	int fd;
	int ret;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		fprintf (stderr, "check_mounted(): Could not open %s\n", file);
		return -errno;
	}

	ret =  check_mounted_where(fd, file, NULL, 0, NULL);
	close(fd);

	return ret;
}

int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_dev_ret)
{
	int ret;
	u64 total_devs = 1;
	int is_btrfs;
	struct btrfs_fs_devices *fs_devices_mnt = NULL;
	FILE *f;
	struct mntent *mnt;

	/* scan the initial device */
	ret = btrfs_scan_one_device(fd, file, &fs_devices_mnt,
				    &total_devs, BTRFS_SUPER_INFO_OFFSET);
	is_btrfs = (ret >= 0);

	/* scan other devices */
	if (is_btrfs && total_devs > 1) {
		if((ret = btrfs_scan_for_fsid(fs_devices_mnt, total_devs, 1)))
			return ret;
	}

	/* iterate over the list of currently mountes filesystems */
	if ((f = setmntent ("/proc/mounts", "r")) == NULL)
		return -errno;

	while ((mnt = getmntent (f)) != NULL) {
		if(is_btrfs) {
			if(strcmp(mnt->mnt_type, "btrfs") != 0)
				continue;

			ret = blk_file_in_dev_list(fs_devices_mnt, mnt->mnt_fsname);
		} else {
			/* ignore entries in the mount table that are not
			   associated with a file*/
			if((ret = is_existing_blk_or_reg_file(mnt->mnt_fsname)) < 0)
				goto out_mntloop_err;
			else if(!ret)
				continue;

			ret = is_same_loop_file(file, mnt->mnt_fsname);
		}

		if(ret < 0)
			goto out_mntloop_err;
		else if(ret)
			break;
	}

	/* Did we find an entry in mnt table? */
	if (mnt && size && where) {
		strncpy(where, mnt->mnt_dir, size);
		where[size-1] = 0;
	}
	if (fs_dev_ret)
		*fs_dev_ret = fs_devices_mnt;

	ret = (mnt != NULL);

out_mntloop_err:
	endmntent (f);

	return ret;
}

/* Gets the mount point of btrfs filesystem that is using the specified device.
 * Returns 0 is everything is good, <0 if we have an error.
 * TODO: Fix this fucntion and check_mounted to work with multiple drive BTRFS
 * setups.
 */
int get_mountpt(char *dev, char *mntpt, size_t size)
{
       struct mntent *mnt;
       FILE *f;
       int ret = 0;

       f = setmntent("/proc/mounts", "r");
       if (f == NULL)
               return -errno;

       while ((mnt = getmntent(f)) != NULL )
       {
               if (strcmp(dev, mnt->mnt_fsname) == 0)
               {
                       strncpy(mntpt, mnt->mnt_dir, size);
                       if (size)
                                mntpt[size-1] = 0;
                       break;
               }
       }

       if (mnt == NULL)
       {
               /* We didn't find an entry so lets report an error */
               ret = -1;
       }

       return ret;
}

struct pending_dir {
	struct list_head list;
	char name[256];
};

void btrfs_register_one_device(char *fname)
{
	struct btrfs_ioctl_vol_args args;
	int fd;
	int ret;
	int e;

	fd = open("/dev/btrfs-control", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "failed to open /dev/btrfs-control "
			"skipping device registration\n");
		return;
	}
	strncpy(args.name, fname, BTRFS_PATH_NAME_MAX);
	args.name[BTRFS_PATH_NAME_MAX-1] = 0;
	ret = ioctl(fd, BTRFS_IOC_SCAN_DEV, &args);
	e = errno;
	if(ret<0){
		fprintf(stderr, "ERROR: unable to scan the device '%s' - %s\n",
			fname, strerror(e));
	}
	close(fd);
}

int btrfs_scan_one_dir(char *dirname, int run_ioctl)
{
	DIR *dirp = NULL;
	struct dirent *dirent;
	struct pending_dir *pending;
	struct stat st;
	int ret;
	int fd;
	int dirname_len;
	int pathlen;
	char *fullpath;
	struct list_head pending_list;
	struct btrfs_fs_devices *tmp_devices;
	u64 num_devices;

	INIT_LIST_HEAD(&pending_list);

	pending = malloc(sizeof(*pending));
	if (!pending)
		return -ENOMEM;
	strcpy(pending->name, dirname);

again:
	dirname_len = strlen(pending->name);
	pathlen = 1024;
	fullpath = malloc(pathlen);
	dirname = pending->name;

	if (!fullpath) {
		ret = -ENOMEM;
		goto fail;
	}
	dirp = opendir(dirname);
	if (!dirp) {
		fprintf(stderr, "Unable to open %s for scanning\n", dirname);
		return -ENOENT;
	}
	while(1) {
		dirent = readdir(dirp);
		if (!dirent)
			break;
		if (dirent->d_name[0] == '.')
			continue;
		if (dirname_len + strlen(dirent->d_name) + 2 > pathlen) {
			ret = -EFAULT;
			goto fail;
		}
		snprintf(fullpath, pathlen, "%s/%s", dirname, dirent->d_name);
		ret = lstat(fullpath, &st);
		if (ret < 0) {
			fprintf(stderr, "failed to stat %s\n", fullpath);
			continue;
		}
		if (S_ISLNK(st.st_mode))
			continue;
		if (S_ISDIR(st.st_mode)) {
			struct pending_dir *next = malloc(sizeof(*next));
			if (!next) {
				ret = -ENOMEM;
				goto fail;
			}
			strcpy(next->name, fullpath);
			list_add_tail(&next->list, &pending_list);
		}
		if (!S_ISBLK(st.st_mode)) {
			continue;
		}
		fd = open(fullpath, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "failed to read %s: %s\n", fullpath,
					strerror(errno));
			continue;
		}
		ret = btrfs_scan_one_device(fd, fullpath, &tmp_devices,
					    &num_devices,
					    BTRFS_SUPER_INFO_OFFSET);
		if (ret == 0 && run_ioctl > 0) {
			btrfs_register_one_device(fullpath);
		}
		close(fd);
	}
	if (!list_empty(&pending_list)) {
		free(pending);
		pending = list_entry(pending_list.next, struct pending_dir,
				     list);
		list_del(&pending->list);
		closedir(dirp);
		dirp = NULL;
		goto again;
	}
	ret = 0;
fail:
	free(pending);
	if (dirp)
		closedir(dirp);
	return ret;
}

int btrfs_scan_for_fsid(struct btrfs_fs_devices *fs_devices, u64 total_devs,
			int run_ioctls)
{
	int ret;

	ret = btrfs_scan_block_devices(run_ioctls);
	if (ret)
		ret = btrfs_scan_one_dir("/dev", run_ioctls);
	return ret;
}

int btrfs_device_already_in_root(struct btrfs_root *root, int fd,
				 int super_offset)
{
	struct btrfs_super_block *disk_super;
	char *buf;
	int ret = 0;

	buf = malloc(BTRFS_SUPER_INFO_SIZE);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}
	ret = pread(fd, buf, BTRFS_SUPER_INFO_SIZE, super_offset);
	if (ret != BTRFS_SUPER_INFO_SIZE)
		goto brelse;

	ret = 0;
	disk_super = (struct btrfs_super_block *)buf;
	if (strncmp((char *)(&disk_super->magic), BTRFS_MAGIC,
	    sizeof(disk_super->magic)))
		goto brelse;

	if (!memcmp(disk_super->fsid, root->fs_info->super_copy.fsid,
		    BTRFS_FSID_SIZE))
		ret = 1;
brelse:
	free(buf);
out:
	return ret;
}

static char *size_strs[] = { "", "KB", "MB", "GB", "TB",
			    "PB", "EB", "ZB", "YB"};
char *pretty_sizes(u64 size)
{
	int num_divs = 0;
        int pretty_len = 16;
	float fraction;
	char *pretty;

	if( size < 1024 ){
		fraction = size;
		num_divs = 0;
	} else {
		u64 last_size = size;
		num_divs = 0;
		while(size >= 1024){
			last_size = size;
			size /= 1024;
			num_divs ++;
		}

		if (num_divs > ARRAY_SIZE(size_strs))
			return NULL;
		fraction = (float)last_size / 1024;
	}
	pretty = malloc(pretty_len);
	snprintf(pretty, pretty_len, "%.2f%s", fraction, size_strs[num_divs]);
	return pretty;
}

/*
 * Checks to make sure that the label matches our requirements.
 * Returns:
       0    if everything is safe and usable
      -1    if the label is too long
      -2    if the label contains an invalid character
 */
int check_label(char *input)
{
       int i;
       int len = strlen(input);

       if (len > BTRFS_LABEL_SIZE) {
               return -1;
       }

       for (i = 0; i < len; i++) {
               if (input[i] == '/' || input[i] == '\\') {
                       return -2;
               }
       }

       return 0;
}

int btrfs_scan_block_devices(int run_ioctl)
{

	struct stat st;
	int ret;
	int fd;
	struct btrfs_fs_devices *tmp_devices;
	u64 num_devices;
	FILE *proc_partitions;
	int i;
	char buf[1024];
	char fullpath[110];
	int scans = 0;
	int special;

scan_again:
	proc_partitions = fopen("/proc/partitions","r");
	if (!proc_partitions) {
		fprintf(stderr, "Unable to open '/proc/partitions' for scanning\n");
		return -ENOENT;
	}
	/* skip the header */
	for(i=0; i < 2 ; i++)
		if(!fgets(buf, 1023, proc_partitions)){
		fprintf(stderr, "Unable to read '/proc/partitions' for scanning\n");
		fclose(proc_partitions);
		return -ENOENT;
	}

	strcpy(fullpath,"/dev/");
	while(fgets(buf, 1023, proc_partitions)) {
		i = sscanf(buf," %*d %*d %*d %99s", fullpath+5);

		/*
		 * multipath and MD devices may register as a btrfs filesystem
		 * both through the original block device and through
		 * the special (/dev/mapper or /dev/mdX) entry.
		 * This scans the special entries last
		 */
		special = strncmp(fullpath, "/dev/dm-", strlen("/dev/dm-")) == 0;
		if (!special)
			special = strncmp(fullpath, "/dev/md", strlen("/dev/md")) == 0;

		if (scans == 0 && special)
			continue;
		if (scans > 0 && !special)
			continue;

		ret = lstat(fullpath, &st);
		if (ret < 0) {
			fprintf(stderr, "failed to stat %s\n", fullpath);
			continue;
		}
		if (!S_ISBLK(st.st_mode)) {
			continue;
		}

		fd = open(fullpath, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "failed to read %s\n", fullpath);
			continue;
		}
		ret = btrfs_scan_one_device(fd, fullpath, &tmp_devices,
					    &num_devices,
					    BTRFS_SUPER_INFO_OFFSET);
		if (ret == 0 && run_ioctl > 0) {
			btrfs_register_one_device(fullpath);
		}
		close(fd);
	}

	fclose(proc_partitions);

	if (scans == 0) {
		scans++;
		goto scan_again;
	}
	return 0;
}

