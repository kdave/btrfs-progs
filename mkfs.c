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

#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#endif

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
#include "version.h"

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

static int make_root_dir(struct btrfs_root *root)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key location;
	u64 bytes_used;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	bytes_used = btrfs_super_bytes_used(&root->fs_info->super_copy);

	root->fs_info->system_allocs = 1;
	ret = btrfs_make_block_group(trans, root, bytes_used,
				     BTRFS_BLOCK_GROUP_SYSTEM,
				     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     0, BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	BUG_ON(ret);

	ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
				&chunk_start, &chunk_size,
				BTRFS_BLOCK_GROUP_METADATA);
	BUG_ON(ret);
	ret = btrfs_make_block_group(trans, root, 0,
				     BTRFS_BLOCK_GROUP_METADATA,
				     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     chunk_start, chunk_size);
	BUG_ON(ret);

	root->fs_info->system_allocs = 0;
	btrfs_commit_transaction(trans, root);
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(!trans);

	ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
				&chunk_start, &chunk_size,
				BTRFS_BLOCK_GROUP_DATA);
	BUG_ON(ret);
	ret = btrfs_make_block_group(trans, root, 0,
				     BTRFS_BLOCK_GROUP_DATA,
				     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     chunk_start, chunk_size);
	BUG_ON(ret);

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
			&location, BTRFS_FT_DIR, 0);
	if (ret)
		goto err;

	ret = btrfs_insert_inode_ref(trans, root->fs_info->tree_root,
			     "default", 7, location.objectid,
			     BTRFS_ROOT_TREE_DIR_OBJECTID, 0);
	if (ret)
		goto err;

	btrfs_commit_transaction(trans, root);
err:
	return ret;
}

static int recow_roots(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root)
{
	int ret;
	struct extent_buffer *tmp;
	struct btrfs_fs_info *info = root->fs_info;

	ret = __btrfs_cow_block(trans, info->fs_root, info->fs_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	ret = __btrfs_cow_block(trans, info->tree_root, info->tree_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	ret = __btrfs_cow_block(trans, info->extent_root,
				info->extent_root->node, NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	ret = __btrfs_cow_block(trans, info->chunk_root, info->chunk_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);


	ret = __btrfs_cow_block(trans, info->dev_root, info->dev_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	ret = __btrfs_cow_block(trans, info->csum_root, info->csum_root->node,
				NULL, 0, &tmp, 0, 0);
	BUG_ON(ret);
	free_extent_buffer(tmp);

	return 0;
}

static int create_one_raid_group(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 type)
{
	u64 chunk_start;
	u64 chunk_size;
	int ret;

	ret = btrfs_alloc_chunk(trans, root->fs_info->extent_root,
				&chunk_start, &chunk_size, type);
	BUG_ON(ret);
	ret = btrfs_make_block_group(trans, root->fs_info->extent_root, 0,
				     type, BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     chunk_start, chunk_size);
	BUG_ON(ret);
	return ret;
}

static int create_raid_groups(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 data_profile,
			      u64 metadata_profile)
{
	u64 num_devices = btrfs_super_num_devices(&root->fs_info->super_copy);
	u64 allowed;
	int ret;

	if (num_devices == 1)
		allowed = BTRFS_BLOCK_GROUP_DUP;
	else if (num_devices >= 4) {
		allowed = BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1 |
			BTRFS_BLOCK_GROUP_RAID10;
	} else
		allowed = BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID1;

	if (allowed & metadata_profile) {
		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_SYSTEM |
					    (allowed & metadata_profile));
		BUG_ON(ret);

		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_METADATA |
					    (allowed & metadata_profile));
		BUG_ON(ret);

		ret = recow_roots(trans, root);
		BUG_ON(ret);
	}
	if (num_devices > 1 && (allowed & data_profile)) {
		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_DATA |
					    (allowed & data_profile));
		BUG_ON(ret);
	}
	return 0;
}

static int create_data_reloc_tree(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root)
{
	struct btrfs_key location;
	struct btrfs_root_item root_item;
	struct extent_buffer *tmp;
	u64 objectid = BTRFS_DATA_RELOC_TREE_OBJECTID;
	int ret;

	ret = btrfs_copy_root(trans, root, root->node, &tmp, objectid);
	BUG_ON(ret);

	memcpy(&root_item, &root->root_item, sizeof(root_item));
	btrfs_set_root_bytenr(&root_item, tmp->start);
	btrfs_set_root_level(&root_item, btrfs_header_level(tmp));
	btrfs_set_root_generation(&root_item, trans->transid);
	free_extent_buffer(tmp);

	location.objectid = objectid;
	location.type = BTRFS_ROOT_ITEM_KEY;
	location.offset = 0;
	ret = btrfs_insert_root(trans, root->fs_info->tree_root,
				&location, &root_item);
	BUG_ON(ret);
	return 0;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: mkfs.btrfs [options] dev [ dev ... ]\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "\t -A --alloc-start the offset to start the FS\n");
	fprintf(stderr, "\t -b --byte-count total number of bytes in the FS\n");
	fprintf(stderr, "\t -d --data data profile, raid0, raid1, raid10 or single\n");
	fprintf(stderr, "\t -l --leafsize size of btree leaves\n");
	fprintf(stderr, "\t -L --label set a label\n");
	fprintf(stderr, "\t -m --metadata metadata profile, values like data profile\n");
	fprintf(stderr, "\t -n --nodesize size of btree nodes\n");
	fprintf(stderr, "\t -s --sectorsize min block allocation\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

static void print_version(void)
{
	fprintf(stderr, "mkfs.btrfs, part of %s\n", BTRFS_BUILD_VERSION);
	exit(0);
}

static u64 parse_profile(char *s)
{
	if (strcmp(s, "raid0") == 0) {
		return BTRFS_BLOCK_GROUP_RAID0;
	} else if (strcmp(s, "raid1") == 0) {
		return BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_DUP;
	} else if (strcmp(s, "raid10") == 0) {
		return BTRFS_BLOCK_GROUP_RAID10 | BTRFS_BLOCK_GROUP_DUP;
	} else if (strcmp(s, "single") == 0) {
		return 0;
	} else {
		fprintf(stderr, "Unknown option %s\n", s);
		print_usage();
	}
	return 0;
}

static char *parse_label(char *input)
{
	int i;
	int len = strlen(input);

	if (len > BTRFS_LABEL_SIZE) {
		fprintf(stderr, "Label %s is too long (max %d)\n", input,
			BTRFS_LABEL_SIZE);
		exit(1);
	}
	for (i = 0; i < len; i++) {
		if (input[i] == '/' || input[i] == '\\') {
			fprintf(stderr, "invalid label %s\n", input);
			exit(1);
		}
	}
	return strdup(input);
}

static struct option long_options[] = {
	{ "alloc-start", 1, NULL, 'A'},
	{ "byte-count", 1, NULL, 'b' },
	{ "leafsize", 1, NULL, 'l' },
	{ "label", 1, NULL, 'L'},
	{ "metadata", 1, NULL, 'm' },
	{ "nodesize", 1, NULL, 'n' },
	{ "sectorsize", 1, NULL, 's' },
	{ "data", 1, NULL, 'd' },
	{ "version", 0, NULL, 'V' },
	{ 0, 0, 0, 0}
};

int main(int ac, char **av)
{
	char *file;
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;
	char *label = NULL;
	char *first_file;
	u64 block_count = 0;
	u64 dev_block_count = 0;
	u64 blocks[7];
	u64 alloc_start = 0;
	u64 metadata_profile = BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_DUP;
	u64 data_profile = BTRFS_BLOCK_GROUP_RAID0;
	u32 leafsize = getpagesize();
	u32 sectorsize = 4096;
	u32 nodesize = leafsize;
	u32 stripesize = 4096;
	int zero_end = 1;
	int option_index = 0;
	int fd;
	int first_fd;
	int ret;
	int i;

	while(1) {
		int c;
		c = getopt_long(ac, av, "A:b:l:n:s:m:d:L:V", long_options,
				&option_index);
		if (c < 0)
			break;
		switch(c) {
			case 'A':
				alloc_start = parse_size(optarg);
				break;
			case 'd':
				data_profile = parse_profile(optarg);
				break;
			case 'l':
				leafsize = parse_size(optarg);
				break;
			case 'L':
				label = parse_label(optarg);
				break;
			case 'm':
				metadata_profile = parse_profile(optarg);
				break;
			case 'n':
				nodesize = parse_size(optarg);
				break;
			case 's':
				sectorsize = parse_size(optarg);
				break;
			case 'b':
				block_count = parse_size(optarg);
				if (block_count < 256*1024*1024) {
					fprintf(stderr, "File system size "
						"%llu bytes is too small, "
						"256M is required at least\n",
						(unsigned long long)block_count);
					exit(1);
				}
				zero_end = 0;
				break;
			case 'V':
				print_version();
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

	printf("\nWARNING! - %s IS EXPERIMENTAL\n", BTRFS_BUILD_VERSION);
	printf("WARNING! - see http://btrfs.wiki.kernel.org before using\n\n");

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
	first_file = file;
	ret = btrfs_prepare_device(fd, file, zero_end, &dev_block_count);
	if (block_count == 0)
		block_count = dev_block_count;

	blocks[0] = BTRFS_SUPER_INFO_OFFSET;
	for (i = 1; i < 7; i++) {
		blocks[i] = BTRFS_SUPER_INFO_OFFSET + 1024 * 1024 +
			leafsize * i;
	}

	ret = make_btrfs(fd, file, label, blocks, block_count,
			 nodesize, leafsize,
			 sectorsize, stripesize);
	if (ret) {
		fprintf(stderr, "error during mkfs %d\n", ret);
		exit(1);
	}
	root = open_ctree(file, 0, O_RDWR);
	root->fs_info->alloc_start = alloc_start;

	ret = make_root_dir(root);
	if (ret) {
		fprintf(stderr, "failed to setup the root directory\n");
		exit(1);
	}

	trans = btrfs_start_transaction(root, 1);

	if (ac == 0)
		goto raid_groups;

	btrfs_register_one_device(file);
	if (!root) {
		fprintf(stderr, "ctree init failed\n");
		return -1;
	}

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
		ret = btrfs_device_already_in_root(root, fd,
						   BTRFS_SUPER_INFO_OFFSET);
		if (ret) {
			fprintf(stderr, "skipping duplicate device %s in FS\n",
				file);
			close(fd);
			continue;
		}
		ret = btrfs_prepare_device(fd, file, zero_end,
					   &dev_block_count);

		BUG_ON(ret);

		ret = btrfs_add_to_fsid(trans, root, fd, file, dev_block_count,
					sectorsize, sectorsize, sectorsize);
		BUG_ON(ret);
		btrfs_register_one_device(file);
	}

raid_groups:
	ret = create_raid_groups(trans, root, data_profile,
				 metadata_profile);
	BUG_ON(ret);

	ret = create_data_reloc_tree(trans, root);
	BUG_ON(ret);

	printf("fs created label %s on %s\n\tnodesize %u leafsize %u "
	    "sectorsize %u size %s\n",
	    label, first_file, nodesize, leafsize, sectorsize,
	    pretty_sizes(btrfs_super_total_bytes(&root->fs_info->super_copy)));

	printf("%s\n", BTRFS_BUILD_VERSION);
	btrfs_commit_transaction(trans, root);
	ret = close_ctree(root);
	BUG_ON(ret);

	free(label);
	return 0;
}

