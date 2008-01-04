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
#include "utils.h"

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

static int make_root_dir(int fd) {
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;
	struct btrfs_key location;
	int ret;

	root = open_ctree_fd(fd, 0);

	if (!root) {
		fprintf(stderr, "ctree init failed\n");
		return -1;
	}
	trans = btrfs_start_transaction(root, 1);
	ret = btrfs_make_block_groups(trans, root);
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
	u64 blocks[4];
	char *buf = malloc(sectorsize);

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
	for (i = 0; i < 4; i++)
		blocks[i] = BTRFS_SUPER_INFO_OFFSET + leafsize * i;
	ret = make_btrfs(fd, blocks, block_count, nodesize, leafsize,
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

