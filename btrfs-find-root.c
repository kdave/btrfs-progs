/*
 * Copyright (C) 2011 Red Hat.  All rights reserved.
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
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"
#include "volumes.h"
#include "utils.h"
#include "crc32c.h"
#include "extent-cache.h"
#include "find-root.h"

static u16 csum_size = 0;
static u64 search_objectid = BTRFS_ROOT_TREE_OBJECTID;
static u64 search_generation = 0;
static unsigned long search_level = 0;

static void usage(void)
{
	fprintf(stderr, "Usage: find-roots [-o search_objectid] "
		"[ -g search_generation ] [ -l search_level ] <device>\n");
}

static int csum_block(void *buf, u32 len)
{
	char *result;
	u32 crc = ~(u32)0;
	int ret = 0;

	result = malloc(csum_size * sizeof(char));
	if (!result) {
		fprintf(stderr, "No memory\n");
		return 1;
	}

	len -= BTRFS_CSUM_SIZE;
	crc = crc32c(crc, buf + BTRFS_CSUM_SIZE, len);
	btrfs_csum_final(crc, result);

	if (memcmp(buf, result, csum_size))
		ret = 1;
	free(result);
	return ret;
}

static struct btrfs_root *open_ctree_broken(int fd, const char *device)
{
	struct btrfs_fs_info *fs_info;
	struct btrfs_super_block *disk_super;
	struct btrfs_fs_devices *fs_devices = NULL;
	struct extent_buffer *eb;
	int ret;

	fs_info = btrfs_new_fs_info(0, BTRFS_SUPER_INFO_OFFSET);
	if (!fs_info) {
		fprintf(stderr, "Failed to allocate memory for fs_info\n");
		return NULL;
	}

	ret = btrfs_scan_fs_devices(fd, device, &fs_devices, 0, 1);
	if (ret)
		goto out;

	fs_info->fs_devices = fs_devices;

	ret = btrfs_open_devices(fs_devices, O_RDONLY);
	if (ret)
		goto out_devices;

	disk_super = fs_info->super_copy;
	ret = btrfs_read_dev_super(fs_devices->latest_bdev,
				   disk_super, fs_info->super_bytenr, 1);
	if (ret) {
		printk("No valid btrfs found\n");
		goto out_devices;
	}

	memcpy(fs_info->fsid, &disk_super->fsid, BTRFS_FSID_SIZE);

	ret = btrfs_check_fs_compatibility(disk_super, 0);
	if (ret)
		goto out_devices;

	ret = btrfs_setup_chunk_tree_and_device_map(fs_info);
	if (ret)
		goto out_chunk;

	eb = fs_info->chunk_root->node;
	read_extent_buffer(eb, fs_info->chunk_tree_uuid,
			   btrfs_header_chunk_tree_uuid(eb), BTRFS_UUID_SIZE);

	return fs_info->chunk_root;
out_chunk:
	free_extent_buffer(fs_info->chunk_root->node);
	btrfs_cleanup_all_caches(fs_info);
out_devices:
	btrfs_close_devices(fs_info->fs_devices);
out:
	btrfs_free_fs_info(fs_info);
	return NULL;
}

static int search_iobuf(struct btrfs_root *root, void *iobuf,
			size_t iobuf_size, off_t offset)
{
	u64 gen = search_generation;
	u64 objectid = search_objectid;
	u32 size = btrfs_super_nodesize(root->fs_info->super_copy);
	u8 level = search_level;
	size_t block_off = 0;

	while (block_off < iobuf_size) {
		void *block = iobuf + block_off;
		struct btrfs_header *header = block;
		u64 h_byte, h_level, h_gen, h_owner;

//		printf("searching %Lu\n", offset + block_off);
		h_byte = btrfs_stack_header_bytenr(header);
		h_owner = btrfs_stack_header_owner(header);
		h_level = header->level;
		h_gen = btrfs_stack_header_generation(header);

		if (h_owner != objectid)
			goto next;
		if (h_byte != (offset + block_off))
			goto next;
		if (h_level < level)
			goto next;
		level = h_level;
		if (csum_block(block, size)) {
			fprintf(stderr, "Well block %Lu seems good, "
				"but the csum doesn't match\n",
				h_byte);
			goto next;
		}
		if (h_gen != gen) {
			fprintf(stderr, "Well block %Lu seems great, "
				"but generation doesn't match, "
				"have=%Lu, want=%Lu level %Lu\n", h_byte,
				h_gen, gen, h_level);
			goto next;
		}
		printf("Found tree root at %Lu gen %Lu level %Lu\n", h_byte,
		       h_gen, h_level);
		return 0;
next:
		block_off += size;
	}

	return 1;
}

static int read_physical(struct btrfs_root *root, int fd, u64 offset,
			 u64 bytenr, u64 len)
{
	char *iobuf = malloc(len);
	ssize_t done;
	size_t total_read = 0;
	int ret = 1;

	if (!iobuf) {
		fprintf(stderr, "No memory\n");
		return -1;
	}

	while (total_read < len) {
		done = pread64(fd, iobuf + total_read, len - total_read,
			       bytenr + total_read);
		if (done < 0) {
			fprintf(stderr, "Failed to read: %s\n",
				strerror(errno));
			ret = -1;
			goto out;
		}
		total_read += done;
	}

	ret = search_iobuf(root, iobuf, total_read, offset);
out:
	free(iobuf);
	return ret;
}

static int find_root(struct btrfs_root *root)
{
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	u64 metadata_offset = 0, metadata_size = 0;
	off_t offset = 0;
	off_t bytenr;
	int fd;
	int err;
	int ret = 1;

	printf("Super think's the tree root is at %Lu, chunk root %Lu\n",
	       btrfs_super_root(root->fs_info->super_copy),
	       btrfs_super_chunk_root(root->fs_info->super_copy));

	err = btrfs_next_metadata(&root->fs_info->mapping_tree,
				  &metadata_offset, &metadata_size);
	if (err)
		return ret;

	offset = metadata_offset;
	while (1) {
		u64 map_length = 4096;
		u64 type;

		if (offset >
		    btrfs_super_total_bytes(root->fs_info->super_copy)) {
			printf("Went past the fs size, exiting");
			break;
		}
		if (offset >= (metadata_offset + metadata_size)) {
			err = btrfs_next_metadata(&root->fs_info->mapping_tree,
						  &metadata_offset,
						  &metadata_size);
			if (err) {
				printf("No more metdata to scan, exiting\n");
				break;
			}
			offset = metadata_offset;
		}
		err = __btrfs_map_block(&root->fs_info->mapping_tree, READ,
				      offset, &map_length, &type,
				      &multi, 0, NULL);
		if (err) {
			offset += map_length;
			continue;
		}

		if (!(type & BTRFS_BLOCK_GROUP_METADATA)) {
			offset += map_length;
			kfree(multi);
			continue;
		}

		device = multi->stripes[0].dev;
		fd = device->fd;
		bytenr = multi->stripes[0].physical;
		kfree(multi);

		err = read_physical(root, fd, offset, bytenr, map_length);
		if (!err) {
			ret = 0;
			break;
		} else if (err < 0) {
			ret = err;
			break;
		}
		offset += map_length;
	}
	return ret;
}

/*
 * Get reliable generation and level for given root.
 *
 * We have two sources of gen/level: superblock and tree root.
 * superblock include the following level:
 *   Root, chunk, log
 * and the following generations:
 *   Root, chunk, uuid
 * Other gen/leven can only be read from its btrfs_tree_root if possible.
 *
 * Currently we only believe things from superblock.
 */
static void get_root_gen_and_level(u64 objectid, struct btrfs_fs_info *fs_info,
				   u64 *ret_gen, u8 *ret_level)
{
	struct btrfs_super_block *super = fs_info->super_copy;
	u64 gen = (u64)-1;
	u8 level = (u8)-1;

	switch (objectid) {
	case BTRFS_ROOT_TREE_OBJECTID:
		level = btrfs_super_root_level(super);
		gen = btrfs_super_generation(super);
		break;
	case BTRFS_CHUNK_TREE_OBJECTID:
		level = btrfs_super_chunk_root_level(super);
		gen = btrfs_super_chunk_root_generation(super);
		printf("Search for chunk root is not supported yet\n");
		break;
	case BTRFS_TREE_LOG_OBJECTID:
		level = btrfs_super_log_root_level(super);
		gen = btrfs_super_log_root_transid(super);
		break;
	case BTRFS_UUID_TREE_OBJECTID:
		gen = btrfs_super_uuid_tree_generation(super);
		break;
	}
	if (gen != (u64)-1) {
		printf("Superblock thinks the generation is %llu\n", gen);
		if (ret_gen)
			*ret_gen = gen;
	} else {
		printf("Superblock doesn't contain generation info for root %llu\n",
		       objectid);
	}
	if (level != (u8)-1) {
		printf("Superblock thinks the level is %u\n", level);
		if (ret_level)
			*ret_level = level;
	} else {
		printf("Superblock doesn't contain the level info for root %llu\n",
		       objectid);
	}
}

static void print_one_result(struct cache_extent *tree_block,
			     u8 level, u64 generation,
			     struct btrfs_find_root_filter *filter)
{
	int unsure = 0;

	if (filter->match_gen == (u64)-1 || filter->match_level == (u8)-1)
		unsure = 1;
	printf("Well block %llu(gen: %llu level: %u) seems good, ",
	       tree_block->start, generation, level);
	if (unsure)
		printf("but we are unsure about the correct generation/level\n");
	else
		printf("but generation/level doesn't match, want gen: %llu level: %u\n",
		       filter->match_gen, filter->match_level);
}

static void print_find_root_result(struct cache_tree *result,
				   struct btrfs_find_root_filter *filter)
{
	struct btrfs_find_root_gen_cache *gen_cache;
	struct cache_extent *cache;
	struct cache_extent *tree_block;
	u64 generation = 0;
	u8 level = 0;

	for (cache = last_cache_extent(result);
	     cache; cache = prev_cache_extent(cache)) {
		gen_cache = container_of(cache,
				struct btrfs_find_root_gen_cache, cache);
		level = gen_cache->highest_level;
		generation = cache->start;
		if (level == filter->match_level &&
		    generation == filter->match_gen)
			continue;
		for (tree_block = last_cache_extent(&gen_cache->eb_tree);
		     tree_block; tree_block = prev_cache_extent(tree_block))
			print_one_result(tree_block, level, generation, filter);
	}
}

int main(int argc, char **argv)
{
	struct btrfs_root *root;
	struct btrfs_find_root_filter filter = {0};
	struct cache_tree result;
	struct cache_extent *found;
	int opt;
	int ret;

	/* Default to search root tree */
	filter.objectid = BTRFS_ROOT_TREE_OBJECTID;
	filter.match_gen = (u64)-1;
	filter.match_level = (u8)-1;
	while ((opt = getopt(argc, argv, "l:o:g:")) != -1) {
		switch(opt) {
			case 'o':
				filter.objectid = arg_strtou64(optarg);
				break;
			case 'g':
				filter.generation = arg_strtou64(optarg);
				break;
			case 'l':
				filter.level = arg_strtou64(optarg);
				break;
			default:
				usage();
				exit(1);
		}
	}

	set_argv0(argv);
	argc = argc - optind;
	if (check_argc_min(argc, 1)) {
		usage();
		exit(1);
	}

	root = open_ctree(argv[optind], 0, OPEN_CTREE_CHUNK_ONLY);

	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		exit(1);
	}
	cache_tree_init(&result);

	get_root_gen_and_level(filter.objectid, root->fs_info,
			       &filter.match_gen, &filter.match_level);
	ret = btrfs_find_root_search(root, &filter, &result, &found);
	if (ret < 0) {
		fprintf(stderr, "Fail to search the tree root: %s\n",
			strerror(-ret));
		goto out;
	}
	if (ret > 0) {
		printf("Found tree root at %llu gen %llu level %u\n",
		       found->start, filter.match_gen, filter.match_level);
		ret = 0;
	}
	print_find_root_result(&result, &filter);
out:
	btrfs_find_root_free(&result);
	close_ctree(root);
	return ret;
}
