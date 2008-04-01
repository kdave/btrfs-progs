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
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <linux/fs.h>
#include <ctype.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "utils.h"

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

static int make_root_dir(int fd, const char *device_name) {
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;
	struct btrfs_key location;
	u64 bytes_used;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	int ret;

	root = open_ctree_fd(fd, device_name, 0);

	if (!root) {
		fprintf(stderr, "ctree init failed\n");
		return -1;
	}
	trans = btrfs_start_transaction(root, 1);
	bytes_used = btrfs_super_bytes_used(&root->fs_info->super_copy);

	root->fs_info->force_system_allocs = 1;
	ret = btrfs_make_block_group(trans, root, bytes_used,
				     BTRFS_BLOCK_GROUP_SYSTEM,
				     BTRFS_CHUNK_TREE_OBJECTID,
				     0, BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	BUG_ON(ret);
	ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
				&chunk_start, &chunk_size,
				BTRFS_BLOCK_GROUP_METADATA);
	BUG_ON(ret);
	ret = btrfs_make_block_group(trans, root, 0,
				     BTRFS_BLOCK_GROUP_METADATA,
				     BTRFS_CHUNK_TREE_OBJECTID,
				     chunk_start, chunk_size);
	BUG_ON(ret);

	root->fs_info->force_system_allocs = 0;
	btrfs_commit_transaction(trans, root);
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);

	ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
				&chunk_start, &chunk_size,
				BTRFS_BLOCK_GROUP_DATA);
	BUG_ON(ret);
	ret = btrfs_make_block_group(trans, root, 0,
				     BTRFS_BLOCK_GROUP_DATA,
				     BTRFS_CHUNK_TREE_OBJECTID,
				     chunk_start, chunk_size);
	BUG_ON(ret);

	// ret = btrfs_make_block_group(trans, root, 0, 1);
	ret = btrfs_make_root_dir(trans, root->fs_info->tree_root,
			      BTRFS_ROOT_TREE_DIR_OBJECTID);
	if (ret)
		goto err;
	ret = btrfs_make_root_dir(trans, root, BTRFS_FIRST_FREE_OBJECTID);
	if (ret)
		goto err;
	memcpy(&location, &root->fs_info->fs_root->root_key, sizeof(location));
	location.offset = (u64)-1;
	ret = btrfs_insert_dir_item(trans, root->fs_info->tree_root,
			"default", 7,
			btrfs_super_root_dir(&root->fs_info->super_copy),
			&location, BTRFS_FT_DIR);
	if (ret)
		goto err;

	ret = btrfs_insert_inode_ref(trans, root->fs_info->tree_root,
			     "default", 7, location.objectid,
			     BTRFS_ROOT_TREE_DIR_OBJECTID);
	if (ret)
		goto err;

	btrfs_commit_transaction(trans, root);
	ret = close_ctree(root);
err:
	return ret;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: mkfs.btrfs [options] dev [ dev ... ]\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "\t -b --byte-count total number of bytes in the FS\n");
	fprintf(stderr, "\t -l --leafsize size of btree leaves\n");
	fprintf(stderr, "\t -n --nodesize size of btree leaves\n");
	fprintf(stderr, "\t -s --sectorsize min block allocation\n");
	exit(1);
}

static struct option long_options[] = {
	{ "byte-count", 1, NULL, 'b' },
	{ "leafsize", 1, NULL, 'l' },
	{ "nodesize", 1, NULL, 'n' },
	{ "sectorsize", 1, NULL, 's' },
	{ 0, 0, 0, 0}
};

int main(int ac, char **av)
{
	char *file;
	u64 block_count = 0;
	u64 dev_block_count = 0;
	int fd;
	int first_fd;
	int ret;
	int i;
	u32 leafsize = 16 * 1024;
	u32 sectorsize = 4096;
	u32 nodesize = 16 * 1024;
	u32 stripesize = 4096;
	u64 blocks[6];
	int zero_end = 1;
	int option_index = 0;
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;

	while(1) {
		int c;
		c = getopt_long(ac, av, "b:l:n:s:", long_options,
				&option_index);
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
				sectorsize = parse_size(optarg);
				break;
			case 'b':
				block_count = parse_size(optarg);
				zero_end = 0;
				break;
			default:
				print_usage();
		}
	}
	sectorsize = max(sectorsize, (u32)getpagesize());
	if (leafsize < sectorsize || (leafsize & (sectorsize - 1))) {
		fprintf(stderr, "Illegal leafsize %u\n", leafsize);
		exit(1);
	}
	if (nodesize < sectorsize || (nodesize & (sectorsize - 1))) {
		fprintf(stderr, "Illegal nodesize %u\n", nodesize);
		exit(1);
	}
	ac = ac - optind;
	if (ac == 0)
		print_usage();

	file = av[optind++];
	ret = check_mounted(file);
	if (ret < 0) {
		fprintf(stderr, "error checking %s mount status\n", file);
		exit(1);
	}
	if (ret == 1) {
		fprintf(stderr, "%s is mounted\n", file);
		exit(1);
	}
	ac--;
	fd = open(file, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "unable to open %s\n", file);
		exit(1);
	}
	first_fd = fd;
	ret = btrfs_prepare_device(fd, file, zero_end, &dev_block_count);
	if (block_count == 0)
		block_count = dev_block_count;

	for (i = 0; i < 6; i++)
		blocks[i] = BTRFS_SUPER_INFO_OFFSET + leafsize * i;

	ret = make_btrfs(fd, file, blocks, block_count, nodesize, leafsize,
			 sectorsize, stripesize);
	if (ret) {
		fprintf(stderr, "error during mkfs %d\n", ret);
		exit(1);
	}
	ret = make_root_dir(fd, file);
	if (ret) {
		fprintf(stderr, "failed to setup the root directory\n");
		exit(1);
	}
	printf("fs created on %s nodesize %u leafsize %u sectorsize %u bytes %llu\n",
	       file, nodesize, leafsize, sectorsize,
	       (unsigned long long)block_count);

	if (ac == 0)
		goto done;

	btrfs_register_one_device(file);
	root = open_ctree(file, 0);

	if (!root) {
		fprintf(stderr, "ctree init failed\n");
		return -1;
	}
	trans = btrfs_start_transaction(root, 1);

	zero_end = 1;
	while(ac-- > 0) {
		file = av[optind++];
		ret = check_mounted(file);
		if (ret < 0) {
			fprintf(stderr, "error checking %s mount status\n",
				file);
			exit(1);
		}
		if (ret == 1) {
			fprintf(stderr, "%s is mounted\n", file);
			exit(1);
		}
		fd = open(file, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "unable to open %s\n", file);
			exit(1);
		}
		fprintf(stderr, "adding device %s\n", file);
		ret = btrfs_prepare_device(fd, file, zero_end,
					   &dev_block_count);

		BUG_ON(ret);

		ret = btrfs_add_to_fsid(trans, root, fd, dev_block_count,
					sectorsize, sectorsize, sectorsize);
		BUG_ON(ret);
		close(fd);
		btrfs_register_one_device(file);
	}
	btrfs_commit_transaction(trans, root);
	ret = close_ctree(root);
	BUG_ON(ret);
done:
	return 0;
}

