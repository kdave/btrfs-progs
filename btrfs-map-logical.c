/*
 * Copyright (C) 2009 Oracle.  All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/transaction.h"
#include "kernel-lib/list.h"
#include "kernel-lib/radix-tree.h"
#include "common/utils.h"
#include "common/help.h"

#define BUFFER_SIZE SZ_64K

/* we write the mirror info to stdout unless they are dumping the data
 * to stdout
 * */
static FILE *info_file;

static int map_one_extent(struct btrfs_fs_info *fs_info,
			  u64 *logical_ret, u64 *len_ret, int search_forward)
{
	struct btrfs_root *extent_root;
	struct btrfs_path *path;
	struct btrfs_key key;
	u64 logical;
	u64 len = 0;
	int ret = 0;

	BUG_ON(!logical_ret);
	logical = *logical_ret;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = logical;
	key.type = 0;
	key.offset = 0;

	extent_root = btrfs_extent_root(fs_info, logical);
	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	BUG_ON(ret == 0);
	ret = 0;

again:
	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	if ((search_forward && key.objectid < logical) ||
	    (!search_forward && key.objectid > logical) ||
	    (key.type != BTRFS_EXTENT_ITEM_KEY &&
	     key.type != BTRFS_METADATA_ITEM_KEY)) {
		if (!search_forward)
			ret = btrfs_previous_extent_item(extent_root,
							 path, 0);
		else
			ret = btrfs_next_extent_item(extent_root,
						     path, 0);
		if (ret)
			goto out;
		goto again;
	}
	logical = key.objectid;
	if (key.type == BTRFS_METADATA_ITEM_KEY)
		len = fs_info->nodesize;
	else
		len = key.offset;

out:
	btrfs_free_path(path);
	if (!ret) {
		*logical_ret = logical;
		if (len_ret)
			*len_ret = len;
	}
	return ret;
}

static int __print_mapping_info(struct btrfs_fs_info *fs_info, u64 logical,
				u64 len, int mirror_num)
{
	struct btrfs_multi_bio *multi = NULL;
	u64 cur_offset = 0;
	u64 cur_len;
	int ret = 0;

	while (cur_offset < len) {
		struct btrfs_device *device;
		int i;

		cur_len = len - cur_offset;
		ret = btrfs_map_block(fs_info, READ, logical + cur_offset,
				      &cur_len, &multi, mirror_num, NULL);
		if (ret) {
			errno = -ret;
			fprintf(info_file,
				"Error: fails to map mirror%d logical %llu: %m\n",
				mirror_num, logical);
			return ret;
		}
		for (i = 0; i < multi->num_stripes; i++) {
			device = multi->stripes[i].dev;
			fprintf(info_file,
				"mirror %d logical %llu physical %llu device %s\n",
				mirror_num, logical + cur_offset,
				multi->stripes[0].physical,
				device->name);
		}
		free(multi);
		multi = NULL;
		cur_offset += cur_len;
	}
	return ret;
}

/*
 * Logical and len is the exact value of a extent.
 * And offset is the offset inside the extent. It's only used for case
 * where user only want to print part of the extent.
 *
 * Caller *MUST* ensure the range [logical,logical+len) are in one extent.
 * Or we can encounter the following case, causing a -ENOENT error:
 * |<-----given parameter------>|
 *		|<------ Extent A ----->|
 */
static int print_mapping_info(struct btrfs_fs_info *fs_info, u64 logical,
			      u64 len)
{
	int num_copies;
	int mirror_num;
	int ret = 0;

	num_copies = btrfs_num_copies(fs_info, logical, len);
	for (mirror_num = 1; mirror_num <= num_copies; mirror_num++) {
		ret = __print_mapping_info(fs_info, logical, len, mirror_num);
		if (ret < 0)
			return ret;
	}
	return ret;
}

/* Same requisition as print_mapping_info function */
static int write_extent_content(struct btrfs_fs_info *fs_info, int out_fd,
				u64 logical, u64 length, int mirror)
{
	char buffer[BUFFER_SIZE];
	u64 cur_offset = 0;
	u64 cur_len;
	int ret = 0;

	while (cur_offset < length) {
		cur_len = min_t(u64, length - cur_offset, BUFFER_SIZE);
		ret = read_data_from_disk(fs_info, buffer,
					  logical + cur_offset, &cur_len,
					  mirror);
		if (ret < 0) {
			errno = -ret;
			fprintf(stderr,
				"Failed to read extent at [%llu, %llu]: %m\n",
				logical, logical + length);
			return ret;
		}
		ret = write(out_fd, buffer, cur_len);
		if (ret < 0 || ret != cur_len) {
			if (ret > 0)
				ret = -EINTR;
			errno = -ret;
			fprintf(stderr, "output file write failed: %m\n");
			return ret;
		}
		cur_offset += cur_len;
	}
	return ret;
}

__attribute__((noreturn))
static void print_usage(void)
{
	printf("usage: btrfs-map-logical [options] device\n");
	printf("\t-l Logical extent to map\n");
	printf("\t-c Copy of the extent to read (usually 1 or 2)\n");
	printf("\t-o Output file to hold the extent\n");
	printf("\t-b Number of bytes to read\n");
	exit(1);
}

int main(int argc, char **argv)
{
	struct cache_tree root_cache;
	struct btrfs_root *root;
	char *dev;
	char *output_file = NULL;
	u64 copy = 0;
	u64 logical = 0;
	u64 bytes = 0;
	u64 cur_logical = 0;
	u64 cur_len = 0;
	int out_fd = -1;
	int found = 0;
	int ret = 0;

	while(1) {
		int c;
		static const struct option long_options[] = {
			/* { "byte-count", 1, NULL, 'b' }, */
			{ "logical", required_argument, NULL, 'l' },
			{ "copy", required_argument, NULL, 'c' },
			{ "output", required_argument, NULL, 'o' },
			{ "bytes", required_argument, NULL, 'b' },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "l:c:o:b:", long_options, NULL);
		if (c < 0)
			break;
		switch(c) {
			case 'l':
				logical = arg_strtou64(optarg);
				break;
			case 'c':
				copy = arg_strtou64(optarg);
				break;
			case 'b':
				bytes = arg_strtou64(optarg);
				break;
			case 'o':
				output_file = strdup(optarg);
				break;
			default:
				print_usage();
		}
	}
	set_argv0(argv);
	if (check_argc_min(argc - optind, 1))
		return 1;
	if (logical == 0)
		print_usage();

	dev = argv[optind];

	radix_tree_init();
	cache_tree_init(&root_cache);

	root = open_ctree(dev, 0, 0);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		free(output_file);
		exit(1);
	}

	info_file = stdout;
	if (output_file) {
		if (strcmp(output_file, "-") == 0) {
			out_fd = 1;
			info_file = stderr;
		} else {
			out_fd = open(output_file, O_RDWR | O_CREAT, 0600);
			if (out_fd < 0)
				goto close;
			ret = ftruncate(out_fd, 0);
			if (ret) {
				ret = 1;
				close(out_fd);
				goto close;
			}
			info_file = stdout;
		}
	}

	if (bytes == 0)
		bytes = root->fs_info->nodesize;
	cur_logical = logical;
	cur_len = bytes;

	/* First find the nearest extent */
	ret = map_one_extent(root->fs_info, &cur_logical, &cur_len, 0);
	if (ret < 0) {
		errno = -ret;
		fprintf(stderr, "Failed to find extent at [%llu,%llu): %m\n",
			cur_logical, cur_logical + cur_len);
		goto out_close_fd;
	}
	/*
	 * Normally, search backward should be OK, but for special case like
	 * given logical is quite small where no extents are before it,
	 * we need to search forward.
	 */
	if (ret > 0) {
		ret = map_one_extent(root->fs_info, &cur_logical, &cur_len, 1);
		if (ret < 0) {
			errno = -ret;
			fprintf(stderr,
				"Failed to find extent at [%llu,%llu): %m\n",
				cur_logical, cur_logical + cur_len);
			goto out_close_fd;
		}
		if (ret > 0) {
			fprintf(stderr,
				"Failed to find any extent at [%llu,%llu)\n",
				cur_logical, cur_logical + cur_len);
			goto out_close_fd;
		}
	}

	while (cur_logical + cur_len >= logical && cur_logical < logical +
	       bytes) {
		u64 real_logical;
		u64 real_len;

		found = 1;
		ret = map_one_extent(root->fs_info, &cur_logical, &cur_len, 1);
		if (ret < 0)
			goto out_close_fd;
		if (ret > 0)
			break;
		/* check again if there is overlap. */
		if (cur_logical + cur_len < logical ||
		    cur_logical >= logical + bytes)
			break;

		real_logical = max(logical, cur_logical);
		real_len = min(logical + bytes, cur_logical + cur_len) -
			   real_logical;

		ret = print_mapping_info(root->fs_info, real_logical, real_len);
		if (ret < 0)
			goto out_close_fd;
		if (output_file && out_fd != -1) {
			ret = write_extent_content(root->fs_info, out_fd,
					real_logical, real_len, copy);
			if (ret < 0)
				goto out_close_fd;
		}

		cur_logical += cur_len;
	}

	if (!found) {
		fprintf(stderr, "No extent found at range [%llu,%llu)\n",
			logical, logical + bytes);
	}
out_close_fd:
	if (output_file && out_fd != 1)
		close(out_fd);
close:
	free(output_file);
	close_ctree(root);
	if (ret < 0)
		ret = 1;
	btrfs_close_all_devices();
	return ret;
}
