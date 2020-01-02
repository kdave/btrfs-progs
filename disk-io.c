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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include "kerncompat.h"
#include "kernel-lib/radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "crypto/crc32c.h"
#include "common/utils.h"
#include "print-tree.h"
#include "common/rbtree-utils.h"
#include "common/device-scan.h"
#include "crypto/hash.h"

/* specified errno for check_tree_block */
#define BTRFS_BAD_BYTENR		(-1)
#define BTRFS_BAD_FSID			(-2)
#define BTRFS_BAD_LEVEL			(-3)
#define BTRFS_BAD_NRITEMS		(-4)

/* Calculate max possible nritems for a leaf/node */
static u32 max_nritems(u8 level, u32 nodesize)
{

	if (level == 0)
		return ((nodesize - sizeof(struct btrfs_header)) /
			sizeof(struct btrfs_item));
	return ((nodesize - sizeof(struct btrfs_header)) /
		sizeof(struct btrfs_key_ptr));
}

static int check_tree_block(struct btrfs_fs_info *fs_info,
			    struct extent_buffer *buf)
{

	struct btrfs_fs_devices *fs_devices = fs_info->fs_devices;
	u32 nodesize = fs_info->nodesize;
	bool fsid_match = false;
	int ret = BTRFS_BAD_FSID;

	if (buf->start != btrfs_header_bytenr(buf))
		return BTRFS_BAD_BYTENR;
	if (btrfs_header_level(buf) >= BTRFS_MAX_LEVEL)
		return BTRFS_BAD_LEVEL;
	if (btrfs_header_nritems(buf) > max_nritems(btrfs_header_level(buf),
						    nodesize))
		return BTRFS_BAD_NRITEMS;

	/* Only leaf can be empty */
	if (btrfs_header_nritems(buf) == 0 &&
	    btrfs_header_level(buf) != 0)
		return BTRFS_BAD_NRITEMS;

	while (fs_devices) {
	         /*
                 * Checking the incompat flag is only valid for the current
                 * fs. For seed devices it's forbidden to have their uuid
                 * changed so reading ->fsid in this case is fine
                 */
		if (fs_devices == fs_info->fs_devices &&
		    btrfs_fs_incompat(fs_info, METADATA_UUID))
			fsid_match = !memcmp_extent_buffer(buf,
						   fs_devices->metadata_uuid,
						   btrfs_header_fsid(),
						   BTRFS_FSID_SIZE);
		else
			fsid_match = !memcmp_extent_buffer(buf,
						    fs_devices->fsid,
						    btrfs_header_fsid(),
						    BTRFS_FSID_SIZE);


		if (fs_info->ignore_fsid_mismatch || fsid_match) {
			ret = 0;
			break;
		}
		fs_devices = fs_devices->seed;
	}
	return ret;
}

static void print_tree_block_error(struct btrfs_fs_info *fs_info,
				struct extent_buffer *eb,
				int err)
{
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE] = {'\0'};
	char found_uuid[BTRFS_UUID_UNPARSED_SIZE] = {'\0'};
	u8 buf[BTRFS_UUID_SIZE];

	if (!err)
		return;

	fprintf(stderr, "bad tree block %llu, ", eb->start);
	switch (err) {
	case BTRFS_BAD_FSID:
		read_extent_buffer(eb, buf, btrfs_header_fsid(),
				   BTRFS_UUID_SIZE);
		uuid_unparse(buf, found_uuid);
		uuid_unparse(fs_info->fs_devices->metadata_uuid, fs_uuid);
		fprintf(stderr, "fsid mismatch, want=%s, have=%s\n",
			fs_uuid, found_uuid);
		break;
	case BTRFS_BAD_BYTENR:
		fprintf(stderr, "bytenr mismatch, want=%llu, have=%llu\n",
			eb->start, btrfs_header_bytenr(eb));
		break;
	case BTRFS_BAD_LEVEL:
		fprintf(stderr, "bad level, %u > %d\n",
			btrfs_header_level(eb), BTRFS_MAX_LEVEL);
		break;
	case BTRFS_BAD_NRITEMS:
		fprintf(stderr, "invalid nr_items: %u\n",
			btrfs_header_nritems(eb));
		break;
	}
}

int btrfs_csum_data(u16 csum_type, const u8 *data, u8 *out, size_t len)
{
	memset(out, 0, BTRFS_CSUM_SIZE);

	switch (csum_type) {
	case BTRFS_CSUM_TYPE_CRC32:
		return hash_crc32c(data, len, out);
	case BTRFS_CSUM_TYPE_XXHASH:
		return hash_xxhash(data, len, out);
	case BTRFS_CSUM_TYPE_SHA256:
		return hash_sha256(data, len, out);
	case BTRFS_CSUM_TYPE_BLAKE2:
		return hash_blake2b(data, len, out);
	default:
		fprintf(stderr, "ERROR: unknown csum type: %d\n", csum_type);
		ASSERT(0);
	}

	return -1;
}

static int __csum_tree_block_size(struct extent_buffer *buf, u16 csum_size,
				  int verify, int silent, u16 csum_type)
{
	u8 result[BTRFS_CSUM_SIZE];
	u32 len;

	len = buf->len - BTRFS_CSUM_SIZE;
	btrfs_csum_data(csum_type, (u8 *)buf->data + BTRFS_CSUM_SIZE,
			result, len);

	if (verify) {
		if (memcmp_extent_buffer(buf, result, 0, csum_size)) {
			/* FIXME: format */
			if (!silent)
				printk("checksum verify failed on %llu found %08X wanted %08X\n",
				       (unsigned long long)buf->start,
				       result[0],
				       buf->data[0]);
			return 1;
		}
	} else {
		write_extent_buffer(buf, result, 0, csum_size);
	}
	return 0;
}

int csum_tree_block_size(struct extent_buffer *buf, u16 csum_size, int verify,
			 u16 csum_type)
{
	return __csum_tree_block_size(buf, csum_size, verify, 0, csum_type);
}

int verify_tree_block_csum_silent(struct extent_buffer *buf, u16 csum_size,
				  u16 csum_type)
{
	return __csum_tree_block_size(buf, csum_size, 1, 1, csum_type);
}

int csum_tree_block(struct btrfs_fs_info *fs_info,
		    struct extent_buffer *buf, int verify)
{
	u16 csum_size = btrfs_super_csum_size(fs_info->super_copy);
	u16 csum_type = btrfs_super_csum_type(fs_info->super_copy);

	if (verify && fs_info->suppress_check_block_errors)
		return verify_tree_block_csum_silent(buf, csum_size, csum_type);
	return csum_tree_block_size(buf, csum_size, verify, csum_type);
}

struct extent_buffer *btrfs_find_tree_block(struct btrfs_fs_info *fs_info,
					    u64 bytenr, u32 blocksize)
{
	return find_extent_buffer(&fs_info->extent_cache,
				  bytenr, blocksize);
}

struct extent_buffer* btrfs_find_create_tree_block(
		struct btrfs_fs_info *fs_info, u64 bytenr)
{
	return alloc_extent_buffer(fs_info, bytenr, fs_info->nodesize);
}

void readahead_tree_block(struct btrfs_fs_info *fs_info, u64 bytenr,
		u64 parent_transid)
{
	struct extent_buffer *eb;
	u64 length;
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;

	eb = btrfs_find_tree_block(fs_info, bytenr, fs_info->nodesize);
	if (!(eb && btrfs_buffer_uptodate(eb, parent_transid)) &&
	    !btrfs_map_block(fs_info, READ, bytenr, &length, &multi, 0,
			     NULL)) {
		device = multi->stripes[0].dev;
		device->total_ios++;
		readahead(device->fd, multi->stripes[0].physical,
				fs_info->nodesize);
	}

	free_extent_buffer(eb);
	kfree(multi);
}

static int verify_parent_transid(struct extent_io_tree *io_tree,
				 struct extent_buffer *eb, u64 parent_transid,
				 int ignore)
{
	int ret;

	if (!parent_transid || btrfs_header_generation(eb) == parent_transid)
		return 0;

	if (extent_buffer_uptodate(eb) &&
	    btrfs_header_generation(eb) == parent_transid) {
		ret = 0;
		goto out;
	}
	printk("parent transid verify failed on %llu wanted %llu found %llu\n",
	       (unsigned long long)eb->start,
	       (unsigned long long)parent_transid,
	       (unsigned long long)btrfs_header_generation(eb));
	if (ignore) {
		eb->flags |= EXTENT_BAD_TRANSID;
		printk("Ignoring transid failure\n");
		return 0;
	}

	ret = 1;
out:
	clear_extent_buffer_uptodate(eb);
	return ret;

}


int read_whole_eb(struct btrfs_fs_info *info, struct extent_buffer *eb, int mirror)
{
	unsigned long offset = 0;
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	int ret = 0;
	u64 read_len;
	unsigned long bytes_left = eb->len;

	while (bytes_left) {
		read_len = bytes_left;
		device = NULL;

		if (!info->on_restoring &&
		    eb->start != BTRFS_SUPER_INFO_OFFSET) {
			ret = btrfs_map_block(info, READ, eb->start + offset,
					      &read_len, &multi, mirror, NULL);
			if (ret) {
				printk("Couldn't map the block %Lu\n", eb->start + offset);
				kfree(multi);
				return -EIO;
			}
			device = multi->stripes[0].dev;

			if (device->fd <= 0) {
				kfree(multi);
				return -EIO;
			}

			eb->fd = device->fd;
			device->total_ios++;
			eb->dev_bytenr = multi->stripes[0].physical;
			kfree(multi);
			multi = NULL;
		} else {
			/* special case for restore metadump */
			list_for_each_entry(device, &info->fs_devices->devices, dev_list) {
				if (device->devid == 1)
					break;
			}

			eb->fd = device->fd;
			eb->dev_bytenr = eb->start;
			device->total_ios++;
		}

		if (read_len > bytes_left)
			read_len = bytes_left;

		ret = read_extent_from_disk(eb, offset, read_len);
		if (ret)
			return -EIO;
		offset += read_len;
		bytes_left -= read_len;
	}
	return 0;
}

struct extent_buffer* read_tree_block(struct btrfs_fs_info *fs_info, u64 bytenr,
		u64 parent_transid)
{
	int ret;
	struct extent_buffer *eb;
	u64 best_transid = 0;
	u32 sectorsize = fs_info->sectorsize;
	int mirror_num = 1;
	int good_mirror = 0;
	int candidate_mirror = 0;
	int num_copies;
	int ignore = 0;

	/*
	 * Don't even try to create tree block for unaligned tree block
	 * bytenr.
	 * Such unaligned tree block will free overlapping extent buffer,
	 * causing use-after-free bugs for fuzzed images.
	 */
	if (bytenr < sectorsize || !IS_ALIGNED(bytenr, sectorsize)) {
		error("tree block bytenr %llu is not aligned to sectorsize %u",
		      bytenr, sectorsize);
		return ERR_PTR(-EIO);
	}

	eb = btrfs_find_create_tree_block(fs_info, bytenr);
	if (!eb)
		return ERR_PTR(-ENOMEM);

	if (btrfs_buffer_uptodate(eb, parent_transid))
		return eb;

	num_copies = btrfs_num_copies(fs_info, eb->start, eb->len);
	while (1) {
		ret = read_whole_eb(fs_info, eb, mirror_num);
		if (ret == 0 && csum_tree_block(fs_info, eb, 1) == 0 &&
		    check_tree_block(fs_info, eb) == 0 &&
		    verify_parent_transid(eb->tree, eb, parent_transid, ignore)
		    == 0) {
			if (eb->flags & EXTENT_BAD_TRANSID &&
			    list_empty(&eb->recow)) {
				list_add_tail(&eb->recow,
					      &fs_info->recow_ebs);
				eb->refs++;
			}

			/*
			 * check_tree_block() is less strict to allow btrfs
			 * check to get raw eb with bad key order and fix it.
			 * But we still need to try to get a good copy if
			 * possible, or bad key order can go into tools like
			 * btrfs ins dump-tree.
			 */
			if (btrfs_header_level(eb))
				ret = btrfs_check_node(fs_info, NULL, eb);
			else
				ret = btrfs_check_leaf(fs_info, NULL, eb);
			if (!ret || candidate_mirror == mirror_num) {
				btrfs_set_buffer_uptodate(eb);
				return eb;
			}
			if (candidate_mirror <= 0)
				candidate_mirror = mirror_num;
		}
		if (ignore) {
			if (candidate_mirror > 0) {
				mirror_num = candidate_mirror;
				continue;
			}
			if (check_tree_block(fs_info, eb)) {
				if (!fs_info->suppress_check_block_errors)
					print_tree_block_error(fs_info, eb,
						check_tree_block(fs_info, eb));
			} else {
				if (!fs_info->suppress_check_block_errors)
					fprintf(stderr, "Csum didn't match\n");
			}
			ret = -EIO;
			break;
		}
		if (num_copies == 1) {
			ignore = 1;
			continue;
		}
		if (btrfs_header_generation(eb) > best_transid) {
			best_transid = btrfs_header_generation(eb);
			good_mirror = mirror_num;
		}
		mirror_num++;
		if (mirror_num > num_copies) {
			if (candidate_mirror > 0)
				mirror_num = candidate_mirror;
			else
				mirror_num = good_mirror;
			ignore = 1;
			continue;
		}
	}
	/*
	 * We failed to read this tree block, it be should deleted right now
	 * to avoid stale cache populate the cache.
	 */
	free_extent_buffer_nocache(eb);
	return ERR_PTR(ret);
}

int read_extent_data(struct btrfs_fs_info *fs_info, char *data, u64 logical,
		     u64 *len, int mirror)
{
	u64 offset = 0;
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	int ret = 0;
	u64 max_len = *len;

	ret = btrfs_map_block(fs_info, READ, logical, len, &multi, mirror,
			      NULL);
	if (ret) {
		fprintf(stderr, "Couldn't map the block %llu\n",
				logical + offset);
		goto err;
	}
	device = multi->stripes[0].dev;

	if (*len > max_len)
		*len = max_len;
	if (device->fd < 0) {
		ret = -EIO;
		goto err;
	}

	ret = pread64(device->fd, data, *len, multi->stripes[0].physical);
	if (ret != *len)
		ret = -EIO;
	else
		ret = 0;
err:
	kfree(multi);
	return ret;
}

int write_and_map_eb(struct btrfs_fs_info *fs_info, struct extent_buffer *eb)
{
	int ret;
	int dev_nr;
	u64 length;
	u64 *raid_map = NULL;
	struct btrfs_multi_bio *multi = NULL;

	dev_nr = 0;
	length = eb->len;
	ret = btrfs_map_block(fs_info, WRITE, eb->start, &length,
			      &multi, 0, &raid_map);

	if (raid_map) {
		ret = write_raid56_with_parity(fs_info, eb, multi,
					       length, raid_map);
		BUG_ON(ret);
	} else while (dev_nr < multi->num_stripes) {
		BUG_ON(ret);
		eb->fd = multi->stripes[dev_nr].dev->fd;
		eb->dev_bytenr = multi->stripes[dev_nr].physical;
		multi->stripes[dev_nr].dev->total_ios++;
		dev_nr++;
		ret = write_extent_to_disk(eb);
		BUG_ON(ret);
	}
	kfree(raid_map);
	kfree(multi);
	return 0;
}

int write_tree_block(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info,
		     struct extent_buffer *eb)
{
	if (check_tree_block(fs_info, eb)) {
		print_tree_block_error(fs_info, eb,
				check_tree_block(fs_info, eb));
		BUG();
	}

	if (trans && !btrfs_buffer_uptodate(eb, trans->transid))
		BUG();

	btrfs_set_header_flag(eb, BTRFS_HEADER_FLAG_WRITTEN);
	csum_tree_block(fs_info, eb, 0);

	return write_and_map_eb(fs_info, eb);
}

void btrfs_setup_root(struct btrfs_root *root, struct btrfs_fs_info *fs_info,
		      u64 objectid)
{
	root->node = NULL;
	root->commit_root = NULL;
	root->ref_cows = 0;
	root->track_dirty = 0;

	root->fs_info = fs_info;
	root->objectid = objectid;
	root->last_trans = 0;
	root->last_inode_alloc = 0;

	INIT_LIST_HEAD(&root->dirty_list);
	INIT_LIST_HEAD(&root->unaligned_extent_recs);
	memset(&root->root_key, 0, sizeof(root->root_key));
	memset(&root->root_item, 0, sizeof(root->root_item));
	root->root_key.objectid = objectid;
}

static int find_and_setup_root(struct btrfs_root *tree_root,
			       struct btrfs_fs_info *fs_info,
			       u64 objectid, struct btrfs_root *root)
{
	int ret;
	u64 generation;

	btrfs_setup_root(root, fs_info, objectid);
	ret = btrfs_find_last_root(tree_root, objectid,
				   &root->root_item, &root->root_key);
	if (ret)
		return ret;

	generation = btrfs_root_generation(&root->root_item);
	root->node = read_tree_block(fs_info,
			btrfs_root_bytenr(&root->root_item), generation);
	if (!extent_buffer_uptodate(root->node))
		return -EIO;

	return 0;
}

static int find_and_setup_log_root(struct btrfs_root *tree_root,
			       struct btrfs_fs_info *fs_info,
			       struct btrfs_super_block *disk_super)
{
	u64 blocknr = btrfs_super_log_root(disk_super);
	struct btrfs_root *log_root = malloc(sizeof(struct btrfs_root));

	if (!log_root)
		return -ENOMEM;

	if (blocknr == 0) {
		free(log_root);
		return 0;
	}

	btrfs_setup_root(log_root, fs_info,
			 BTRFS_TREE_LOG_OBJECTID);

	log_root->node = read_tree_block(fs_info, blocknr,
				     btrfs_super_generation(disk_super) + 1);

	fs_info->log_root_tree = log_root;

	if (!extent_buffer_uptodate(log_root->node)) {
		free_extent_buffer(log_root->node);
		free(log_root);
		fs_info->log_root_tree = NULL;
		return -EIO;
	}

	return 0;
}

int btrfs_free_fs_root(struct btrfs_root *root)
{
	if (root->node)
		free_extent_buffer(root->node);
	if (root->commit_root)
		free_extent_buffer(root->commit_root);
	kfree(root);
	return 0;
}

static void __free_fs_root(struct rb_node *node)
{
	struct btrfs_root *root;

	root = container_of(node, struct btrfs_root, rb_node);
	btrfs_free_fs_root(root);
}

FREE_RB_BASED_TREE(fs_roots, __free_fs_root);

struct btrfs_root *btrfs_read_fs_root_no_cache(struct btrfs_fs_info *fs_info,
					       struct btrfs_key *location)
{
	struct btrfs_root *root;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_path *path;
	struct extent_buffer *l;
	u64 generation;
	int ret = 0;

	root = calloc(1, sizeof(*root));
	if (!root)
		return ERR_PTR(-ENOMEM);
	if (location->offset == (u64)-1) {
		ret = find_and_setup_root(tree_root, fs_info,
					  location->objectid, root);
		if (ret) {
			free(root);
			return ERR_PTR(ret);
		}
		goto insert;
	}

	btrfs_setup_root(root, fs_info,
			 location->objectid);

	path = btrfs_alloc_path();
	if (!path) {
		free(root);
		return ERR_PTR(-ENOMEM);
	}

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
	btrfs_free_path(path);
	if (ret) {
		free(root);
		return ERR_PTR(ret);
	}
	generation = btrfs_root_generation(&root->root_item);
	root->node = read_tree_block(fs_info,
			btrfs_root_bytenr(&root->root_item), generation);
	if (!extent_buffer_uptodate(root->node)) {
		free(root);
		return ERR_PTR(-EIO);
	}
insert:
	root->ref_cows = 1;
	return root;
}

static int btrfs_fs_roots_compare_objectids(struct rb_node *node,
					    void *data)
{
	u64 objectid = *((u64 *)data);
	struct btrfs_root *root;

	root = rb_entry(node, struct btrfs_root, rb_node);
	if (objectid > root->objectid)
		return 1;
	else if (objectid < root->objectid)
		return -1;
	else
		return 0;
}

int btrfs_fs_roots_compare_roots(struct rb_node *node1, struct rb_node *node2)
{
	struct btrfs_root *root;

	root = rb_entry(node2, struct btrfs_root, rb_node);
	return btrfs_fs_roots_compare_objectids(node1, (void *)&root->objectid);
}

struct btrfs_root *btrfs_read_fs_root(struct btrfs_fs_info *fs_info,
				      struct btrfs_key *location)
{
	struct btrfs_root *root;
	struct rb_node *node;
	int ret;
	u64 objectid = location->objectid;

	if (location->objectid == BTRFS_ROOT_TREE_OBJECTID)
		return fs_info->tree_root;
	if (location->objectid == BTRFS_EXTENT_TREE_OBJECTID)
		return fs_info->extent_root;
	if (location->objectid == BTRFS_CHUNK_TREE_OBJECTID)
		return fs_info->chunk_root;
	if (location->objectid == BTRFS_DEV_TREE_OBJECTID)
		return fs_info->dev_root;
	if (location->objectid == BTRFS_CSUM_TREE_OBJECTID)
		return fs_info->csum_root;
	if (location->objectid == BTRFS_UUID_TREE_OBJECTID)
		return fs_info->uuid_root ? fs_info->uuid_root : ERR_PTR(-ENOENT);
	if (location->objectid == BTRFS_QUOTA_TREE_OBJECTID)
		return fs_info->quota_enabled ? fs_info->quota_root :
				ERR_PTR(-ENOENT);
	if (location->objectid == BTRFS_FREE_SPACE_TREE_OBJECTID)
		return fs_info->free_space_root ? fs_info->free_space_root :
						ERR_PTR(-ENOENT);

	BUG_ON(location->objectid == BTRFS_TREE_RELOC_OBJECTID ||
	       location->offset != (u64)-1);

	node = rb_search(&fs_info->fs_root_tree, (void *)&objectid,
			 btrfs_fs_roots_compare_objectids, NULL);
	if (node)
		return container_of(node, struct btrfs_root, rb_node);

	root = btrfs_read_fs_root_no_cache(fs_info, location);
	if (IS_ERR(root))
		return root;

	ret = rb_insert(&fs_info->fs_root_tree, &root->rb_node,
			btrfs_fs_roots_compare_roots);
	BUG_ON(ret);
	return root;
}

void btrfs_free_fs_info(struct btrfs_fs_info *fs_info)
{
	if (fs_info->quota_root)
		free(fs_info->quota_root);

	free(fs_info->tree_root);
	free(fs_info->extent_root);
	free(fs_info->chunk_root);
	free(fs_info->dev_root);
	free(fs_info->csum_root);
	free(fs_info->free_space_root);
	free(fs_info->uuid_root);
	free(fs_info->super_copy);
	free(fs_info->log_root_tree);
	free(fs_info);
}

struct btrfs_fs_info *btrfs_new_fs_info(int writable, u64 sb_bytenr)
{
	struct btrfs_fs_info *fs_info;

	fs_info = calloc(1, sizeof(struct btrfs_fs_info));
	if (!fs_info)
		return NULL;

	fs_info->tree_root = calloc(1, sizeof(struct btrfs_root));
	fs_info->extent_root = calloc(1, sizeof(struct btrfs_root));
	fs_info->chunk_root = calloc(1, sizeof(struct btrfs_root));
	fs_info->dev_root = calloc(1, sizeof(struct btrfs_root));
	fs_info->csum_root = calloc(1, sizeof(struct btrfs_root));
	fs_info->quota_root = calloc(1, sizeof(struct btrfs_root));
	fs_info->free_space_root = calloc(1, sizeof(struct btrfs_root));
	fs_info->uuid_root = calloc(1, sizeof(struct btrfs_root));
	fs_info->super_copy = calloc(1, BTRFS_SUPER_INFO_SIZE);

	if (!fs_info->tree_root || !fs_info->extent_root ||
	    !fs_info->chunk_root || !fs_info->dev_root ||
	    !fs_info->csum_root || !fs_info->quota_root ||
	    !fs_info->free_space_root || !fs_info->uuid_root ||
	    !fs_info->super_copy)
		goto free_all;

	extent_io_tree_init(&fs_info->extent_cache);
	extent_io_tree_init(&fs_info->free_space_cache);
	extent_io_tree_init(&fs_info->block_group_cache);
	extent_io_tree_init(&fs_info->pinned_extents);
	extent_io_tree_init(&fs_info->extent_ins);
	fs_info->excluded_extents = NULL;

	fs_info->fs_root_tree = RB_ROOT;
	cache_tree_init(&fs_info->mapping_tree.cache_tree);

	mutex_init(&fs_info->fs_mutex);
	INIT_LIST_HEAD(&fs_info->dirty_cowonly_roots);
	INIT_LIST_HEAD(&fs_info->space_info);
	INIT_LIST_HEAD(&fs_info->recow_ebs);

	if (!writable)
		fs_info->readonly = 1;

	fs_info->super_bytenr = sb_bytenr;
	fs_info->data_alloc_profile = (u64)-1;
	fs_info->metadata_alloc_profile = (u64)-1;
	fs_info->system_alloc_profile = fs_info->metadata_alloc_profile;
	return fs_info;
free_all:
	btrfs_free_fs_info(fs_info);
	return NULL;
}

int btrfs_check_fs_compatibility(struct btrfs_super_block *sb,
				 unsigned int flags)
{
	u64 features;

	features = btrfs_super_incompat_flags(sb) &
		   ~BTRFS_FEATURE_INCOMPAT_SUPP;
	if (features) {
		printk("couldn't open because of unsupported "
		       "option features (%llx).\n",
		       (unsigned long long)features);
		return -ENOTSUP;
	}

	features = btrfs_super_incompat_flags(sb);
	if (!(features & BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF)) {
		features |= BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF;
		btrfs_set_super_incompat_flags(sb, features);
	}

	features = btrfs_super_compat_ro_flags(sb);
	if (flags & OPEN_CTREE_WRITES) {
		if (flags & OPEN_CTREE_INVALIDATE_FST) {
			/* Clear the FREE_SPACE_TREE_VALID bit on disk... */
			features &= ~BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID;
			btrfs_set_super_compat_ro_flags(sb, features);
			/* ... and ignore the free space tree bit. */
			features &= ~BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE;
		}
		if (features & ~BTRFS_FEATURE_COMPAT_RO_SUPP) {
			printk("couldn't open RDWR because of unsupported "
			       "option features (0x%llx)\n",
			       (unsigned long long)features);
			return -ENOTSUP;
		}

	}
	return 0;
}

static int find_best_backup_root(struct btrfs_super_block *super)
{
	struct btrfs_root_backup *backup;
	u64 orig_gen = btrfs_super_generation(super);
	u64 gen = 0;
	int best_index = 0;
	int i;

	for (i = 0; i < BTRFS_NUM_BACKUP_ROOTS; i++) {
		backup = super->super_roots + i;
		if (btrfs_backup_tree_root_gen(backup) != orig_gen &&
		    btrfs_backup_tree_root_gen(backup) > gen) {
			best_index = i;
			gen = btrfs_backup_tree_root_gen(backup);
		}
	}
	return best_index;
}

static int setup_root_or_create_block(struct btrfs_fs_info *fs_info,
				      unsigned flags,
				      struct btrfs_root *info_root,
				      u64 objectid, char *str)
{
	struct btrfs_root *root = fs_info->tree_root;
	int ret;

	ret = find_and_setup_root(root, fs_info, objectid, info_root);
	if (ret) {
		if (!(flags & OPEN_CTREE_PARTIAL)) {
			error("could not setup %s tree", str);
			return -EIO;
		}
		warning("could not setup %s tree, skipping it", str);
		/*
		 * Need a blank node here just so we don't screw up in the
		 * million of places that assume a root has a valid ->node
		 */
		info_root->node =
			btrfs_find_create_tree_block(fs_info, 0);
		if (!info_root->node)
			return -ENOMEM;
		clear_extent_buffer_uptodate(info_root->node);
	}

	return 0;
}

int btrfs_setup_all_roots(struct btrfs_fs_info *fs_info, u64 root_tree_bytenr,
			  unsigned flags)
{
	struct btrfs_super_block *sb = fs_info->super_copy;
	struct btrfs_root *root;
	struct btrfs_key key;
	u64 generation;
	int ret;

	root = fs_info->tree_root;
	btrfs_setup_root(root, fs_info, BTRFS_ROOT_TREE_OBJECTID);
	generation = btrfs_super_generation(sb);

	if (!root_tree_bytenr && !(flags & OPEN_CTREE_BACKUP_ROOT)) {
		root_tree_bytenr = btrfs_super_root(sb);
	} else if (flags & OPEN_CTREE_BACKUP_ROOT) {
		struct btrfs_root_backup *backup;
		int index = find_best_backup_root(sb);
		if (index >= BTRFS_NUM_BACKUP_ROOTS) {
			fprintf(stderr, "Invalid backup root number\n");
			return -EIO;
		}
		backup = fs_info->super_copy->super_roots + index;
		root_tree_bytenr = btrfs_backup_tree_root(backup);
		generation = btrfs_backup_tree_root_gen(backup);
	}

	root->node = read_tree_block(fs_info, root_tree_bytenr, generation);
	if (!extent_buffer_uptodate(root->node)) {
		fprintf(stderr, "Couldn't read tree root\n");
		return -EIO;
	}

	ret = setup_root_or_create_block(fs_info, flags, fs_info->extent_root,
					 BTRFS_EXTENT_TREE_OBJECTID, "extent");
	if (ret)
		return ret;
	fs_info->extent_root->track_dirty = 1;

	ret = find_and_setup_root(root, fs_info, BTRFS_DEV_TREE_OBJECTID,
				  fs_info->dev_root);
	if (ret) {
		printk("Couldn't setup device tree\n");
		return -EIO;
	}
	fs_info->dev_root->track_dirty = 1;

	ret = setup_root_or_create_block(fs_info, flags, fs_info->csum_root,
					 BTRFS_CSUM_TREE_OBJECTID, "csum");
	if (ret)
		return ret;
	fs_info->csum_root->track_dirty = 1;

	ret = find_and_setup_root(root, fs_info, BTRFS_UUID_TREE_OBJECTID,
				  fs_info->uuid_root);
	if (ret) {
		free(fs_info->uuid_root);
		fs_info->uuid_root = NULL;
	} else {
		fs_info->uuid_root->track_dirty = 1;
	}

	ret = find_and_setup_root(root, fs_info, BTRFS_QUOTA_TREE_OBJECTID,
				  fs_info->quota_root);
	if (ret) {
		free(fs_info->quota_root);
		fs_info->quota_root = NULL;
	} else {
		fs_info->quota_enabled = 1;
	}

	if (btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE)) {
		ret = find_and_setup_root(root, fs_info, BTRFS_FREE_SPACE_TREE_OBJECTID,
					  fs_info->free_space_root);
		if (ret) {
			printk("Couldn't read free space tree\n");
			return -EIO;
		}
		fs_info->free_space_root->track_dirty = 1;
	}

	ret = find_and_setup_log_root(root, fs_info, sb);
	if (ret) {
		printk("Couldn't setup log root tree\n");
		if (!(flags & OPEN_CTREE_PARTIAL))
			return -EIO;
	}

	fs_info->generation = generation;
	fs_info->last_trans_committed = generation;
	if (extent_buffer_uptodate(fs_info->extent_root->node) &&
	    !(flags & OPEN_CTREE_NO_BLOCK_GROUPS)) {
		ret = btrfs_read_block_groups(fs_info);
		/*
		 * If we don't find any blockgroups (ENOENT) we're either
		 * restoring or creating the filesystem, where it's expected,
		 * anything else is error
		 */
		if (ret < 0 && ret != -ENOENT) {
			errno = -ret;
			error("failed to read block groups: %m");
			return ret;
		}
	}

	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	fs_info->fs_root = btrfs_read_fs_root(fs_info, &key);

	if (IS_ERR(fs_info->fs_root))
		return -EIO;
	return 0;
}

void btrfs_release_all_roots(struct btrfs_fs_info *fs_info)
{
	if (fs_info->free_space_root)
		free_extent_buffer(fs_info->free_space_root->node);
	if (fs_info->quota_root)
		free_extent_buffer(fs_info->quota_root->node);
	if (fs_info->csum_root)
		free_extent_buffer(fs_info->csum_root->node);
	if (fs_info->dev_root)
		free_extent_buffer(fs_info->dev_root->node);
	if (fs_info->extent_root)
		free_extent_buffer(fs_info->extent_root->node);
	if (fs_info->tree_root)
		free_extent_buffer(fs_info->tree_root->node);
	if (fs_info->log_root_tree)
		free_extent_buffer(fs_info->log_root_tree->node);
	if (fs_info->chunk_root)
		free_extent_buffer(fs_info->chunk_root->node);
	if (fs_info->uuid_root)
		free_extent_buffer(fs_info->uuid_root->node);
}

static void free_map_lookup(struct cache_extent *ce)
{
	struct map_lookup *map;

	map = container_of(ce, struct map_lookup, ce);
	kfree(map);
}

FREE_EXTENT_CACHE_BASED_TREE(mapping_cache, free_map_lookup);

void btrfs_cleanup_all_caches(struct btrfs_fs_info *fs_info)
{
	while (!list_empty(&fs_info->recow_ebs)) {
		struct extent_buffer *eb;
		eb = list_first_entry(&fs_info->recow_ebs,
				      struct extent_buffer, recow);
		list_del_init(&eb->recow);
		free_extent_buffer(eb);
	}
	free_mapping_cache_tree(&fs_info->mapping_tree.cache_tree);
	extent_io_tree_cleanup(&fs_info->extent_cache);
	extent_io_tree_cleanup(&fs_info->free_space_cache);
	extent_io_tree_cleanup(&fs_info->block_group_cache);
	extent_io_tree_cleanup(&fs_info->pinned_extents);
	extent_io_tree_cleanup(&fs_info->extent_ins);
}

int btrfs_scan_fs_devices(int fd, const char *path,
			  struct btrfs_fs_devices **fs_devices,
			  u64 sb_bytenr, unsigned sbflags,
			  int skip_devices)
{
	u64 total_devs;
	u64 dev_size;
	off_t seek_ret;
	int ret;
	if (!sb_bytenr)
		sb_bytenr = BTRFS_SUPER_INFO_OFFSET;

	seek_ret = lseek(fd, 0, SEEK_END);
	if (seek_ret < 0)
		return -errno;

	dev_size = seek_ret;
	lseek(fd, 0, SEEK_SET);
	if (sb_bytenr > dev_size) {
		error("superblock bytenr %llu is larger than device size %llu",
				(unsigned long long)sb_bytenr,
				(unsigned long long)dev_size);
		return -EINVAL;
	}

	ret = btrfs_scan_one_device(fd, path, fs_devices,
				    &total_devs, sb_bytenr, sbflags);
	if (ret) {
		fprintf(stderr, "No valid Btrfs found on %s\n", path);
		return ret;
	}

	if (!skip_devices && total_devs != 1) {
		ret = btrfs_scan_devices();
		if (ret)
			return ret;
	}
	return 0;
}

int btrfs_setup_chunk_tree_and_device_map(struct btrfs_fs_info *fs_info,
					  u64 chunk_root_bytenr)
{
	struct btrfs_super_block *sb = fs_info->super_copy;
	u64 generation;
	int ret;

	btrfs_setup_root(fs_info->chunk_root, fs_info,
			BTRFS_CHUNK_TREE_OBJECTID);

	ret = btrfs_read_sys_array(fs_info);
	if (ret)
		return ret;

	generation = btrfs_super_chunk_root_generation(sb);

	if (chunk_root_bytenr && !IS_ALIGNED(chunk_root_bytenr,
					    fs_info->sectorsize)) {
		warning("chunk_root_bytenr %llu is unaligned to %u, ignore it",
			chunk_root_bytenr, fs_info->sectorsize);
		chunk_root_bytenr = 0;
	}

	if (!chunk_root_bytenr)
		chunk_root_bytenr = btrfs_super_chunk_root(sb);
	else
		generation = 0;

	fs_info->chunk_root->node = read_tree_block(fs_info,
						    chunk_root_bytenr,
						    generation);
	if (!extent_buffer_uptodate(fs_info->chunk_root->node)) {
		if (fs_info->ignore_chunk_tree_error) {
			warning("cannot read chunk root, continue anyway");
			fs_info->chunk_root = NULL;
			return 0;
		} else {
			error("cannot read chunk root");
			return -EIO;
		}
	}

	if (!(btrfs_super_flags(sb) & BTRFS_SUPER_FLAG_METADUMP)) {
		ret = btrfs_read_chunk_tree(fs_info);
		if (ret) {
			fprintf(stderr, "Couldn't read chunk tree\n");
			return ret;
		}
	}
	return 0;
}

static struct btrfs_fs_info *__open_ctree_fd(int fp, const char *path,
					     u64 sb_bytenr,
					     u64 root_tree_bytenr,
					     u64 chunk_root_bytenr,
					     unsigned flags)
{
	struct btrfs_fs_info *fs_info;
	struct btrfs_super_block *disk_super;
	struct btrfs_fs_devices *fs_devices = NULL;
	struct extent_buffer *eb;
	int ret;
	int oflags;
	unsigned sbflags = SBREAD_DEFAULT;

	if (sb_bytenr == 0)
		sb_bytenr = BTRFS_SUPER_INFO_OFFSET;

	/* try to drop all the caches */
	if (posix_fadvise(fp, 0, 0, POSIX_FADV_DONTNEED))
		fprintf(stderr, "Warning, could not drop caches\n");

	fs_info = btrfs_new_fs_info(flags & OPEN_CTREE_WRITES, sb_bytenr);
	if (!fs_info) {
		fprintf(stderr, "Failed to allocate memory for fs_info\n");
		return NULL;
	}
	if (flags & OPEN_CTREE_RESTORE)
		fs_info->on_restoring = 1;
	if (flags & OPEN_CTREE_SUPPRESS_CHECK_BLOCK_ERRORS)
		fs_info->suppress_check_block_errors = 1;
	if (flags & OPEN_CTREE_IGNORE_FSID_MISMATCH)
		fs_info->ignore_fsid_mismatch = 1;
	if (flags & OPEN_CTREE_IGNORE_CHUNK_TREE_ERROR)
		fs_info->ignore_chunk_tree_error = 1;

	if ((flags & OPEN_CTREE_RECOVER_SUPER)
	     && (flags & OPEN_CTREE_TEMPORARY_SUPER)) {
		fprintf(stderr,
	"cannot open a filesystem with temporary super block for recovery");
		goto out;
	}

	if (flags & OPEN_CTREE_TEMPORARY_SUPER)
		sbflags = SBREAD_TEMPORARY;

	if (flags & OPEN_CTREE_IGNORE_FSID_MISMATCH)
		sbflags |= SBREAD_IGNORE_FSID_MISMATCH;

	ret = btrfs_scan_fs_devices(fp, path, &fs_devices, sb_bytenr, sbflags,
			(flags & OPEN_CTREE_NO_DEVICES));
	if (ret)
		goto out;

	fs_info->fs_devices = fs_devices;
	if (flags & OPEN_CTREE_WRITES)
		oflags = O_RDWR;
	else
		oflags = O_RDONLY;

	if (flags & OPEN_CTREE_EXCLUSIVE)
		oflags |= O_EXCL;

	ret = btrfs_open_devices(fs_devices, oflags);
	if (ret)
		goto out;

	disk_super = fs_info->super_copy;
	if (flags & OPEN_CTREE_RECOVER_SUPER)
		ret = btrfs_read_dev_super(fs_devices->latest_bdev, disk_super,
				sb_bytenr, SBREAD_RECOVER);
	else
		ret = btrfs_read_dev_super(fp, disk_super, sb_bytenr,
				sbflags);
	if (ret) {
		printk("No valid btrfs found\n");
		goto out_devices;
	}

	if (btrfs_super_flags(disk_super) & BTRFS_SUPER_FLAG_CHANGING_FSID &&
	    !fs_info->ignore_fsid_mismatch) {
		fprintf(stderr, "ERROR: Filesystem UUID change in progress\n");
		goto out_devices;
	}

	ASSERT(!memcmp(disk_super->fsid, fs_devices->fsid, BTRFS_FSID_SIZE));
	if (btrfs_fs_incompat(fs_info, METADATA_UUID))
		ASSERT(!memcmp(disk_super->metadata_uuid,
			       fs_devices->metadata_uuid, BTRFS_FSID_SIZE));

	fs_info->sectorsize = btrfs_super_sectorsize(disk_super);
	fs_info->nodesize = btrfs_super_nodesize(disk_super);
	fs_info->stripesize = btrfs_super_stripesize(disk_super);

	ret = btrfs_check_fs_compatibility(fs_info->super_copy, flags);
	if (ret)
		goto out_devices;

	ret = btrfs_setup_chunk_tree_and_device_map(fs_info, chunk_root_bytenr);
	if (ret)
		goto out_chunk;

	/* Chunk tree root is unable to read, return directly */
	if (!fs_info->chunk_root)
		return fs_info;

	eb = fs_info->chunk_root->node;
	read_extent_buffer(eb, fs_info->chunk_tree_uuid,
			   btrfs_header_chunk_tree_uuid(eb),
			   BTRFS_UUID_SIZE);

	ret = btrfs_setup_all_roots(fs_info, root_tree_bytenr, flags);
	if (ret && !(flags & __OPEN_CTREE_RETURN_CHUNK_ROOT) &&
	    !fs_info->ignore_chunk_tree_error)
		goto out_chunk;

	return fs_info;

out_chunk:
	btrfs_release_all_roots(fs_info);
	btrfs_cleanup_all_caches(fs_info);
out_devices:
	btrfs_close_devices(fs_devices);
out:
	btrfs_free_fs_info(fs_info);
	return NULL;
}

struct btrfs_fs_info *open_ctree_fs_info(const char *filename,
					 u64 sb_bytenr, u64 root_tree_bytenr,
					 u64 chunk_root_bytenr,
					 unsigned flags)
{
	int fp;
	int ret;
	struct btrfs_fs_info *info;
	int oflags = O_RDWR;
	struct stat st;

	ret = stat(filename, &st);
	if (ret < 0) {
		error("cannot stat '%s': %m", filename);
		return NULL;
	}
	if (!(((st.st_mode & S_IFMT) == S_IFREG) || ((st.st_mode & S_IFMT) == S_IFBLK))) {
		error("not a regular file or block device: %s", filename);
		return NULL;
	}

	if (!(flags & OPEN_CTREE_WRITES))
		oflags = O_RDONLY;

	fp = open(filename, oflags);
	if (fp < 0) {
		error("cannot open '%s': %m", filename);
		return NULL;
	}
	info = __open_ctree_fd(fp, filename, sb_bytenr, root_tree_bytenr,
			       chunk_root_bytenr, flags);
	close(fp);
	return info;
}

struct btrfs_root *open_ctree(const char *filename, u64 sb_bytenr,
			      unsigned flags)
{
	struct btrfs_fs_info *info;

	/* This flags may not return fs_info with any valid root */
	BUG_ON(flags & OPEN_CTREE_IGNORE_CHUNK_TREE_ERROR);
	info = open_ctree_fs_info(filename, sb_bytenr, 0, 0, flags);
	if (!info)
		return NULL;
	if (flags & __OPEN_CTREE_RETURN_CHUNK_ROOT)
		return info->chunk_root;
	return info->fs_root;
}

struct btrfs_root *open_ctree_fd(int fp, const char *path, u64 sb_bytenr,
				 unsigned flags)
{
	struct btrfs_fs_info *info;

	/* This flags may not return fs_info with any valid root */
	if (flags & OPEN_CTREE_IGNORE_CHUNK_TREE_ERROR) {
		error("invalid open_ctree flags: 0x%llx",
				(unsigned long long)flags);
		return NULL;
	}
	info = __open_ctree_fd(fp, path, sb_bytenr, 0, 0, flags);
	if (!info)
		return NULL;
	if (flags & __OPEN_CTREE_RETURN_CHUNK_ROOT)
		return info->chunk_root;
	return info->fs_root;
}

/*
 * Check if the super is valid:
 * - nodesize/sectorsize - minimum, maximum, alignment
 * - tree block starts   - alignment
 * - number of devices   - something sane
 * - sys array size      - maximum
 */
int btrfs_check_super(struct btrfs_super_block *sb, unsigned sbflags)
{
	u8 result[BTRFS_CSUM_SIZE];
	u16 csum_type;
	int csum_size;
	u8 *metadata_uuid;

	if (btrfs_super_magic(sb) != BTRFS_MAGIC) {
		if (btrfs_super_magic(sb) == BTRFS_MAGIC_TEMPORARY) {
			if (!(sbflags & SBREAD_TEMPORARY)) {
				error("superblock magic doesn't match");
				return -EIO;
			}
		}
	}

	csum_type = btrfs_super_csum_type(sb);
	if (csum_type >= btrfs_super_num_csums()) {
		error("unsupported checksum algorithm %u", csum_type);
		return -EIO;
	}
	csum_size = btrfs_super_csum_size(sb);

	btrfs_csum_data(csum_type, (u8 *)sb + BTRFS_CSUM_SIZE,
			result, BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);

	if (memcmp(result, sb->csum, csum_size)) {
		error("superblock checksum mismatch");
		return -EIO;
	}
	if (btrfs_super_root_level(sb) >= BTRFS_MAX_LEVEL) {
		error("tree_root level too big: %d >= %d",
			btrfs_super_root_level(sb), BTRFS_MAX_LEVEL);
		goto error_out;
	}
	if (btrfs_super_chunk_root_level(sb) >= BTRFS_MAX_LEVEL) {
		error("chunk_root level too big: %d >= %d",
			btrfs_super_chunk_root_level(sb), BTRFS_MAX_LEVEL);
		goto error_out;
	}
	if (btrfs_super_log_root_level(sb) >= BTRFS_MAX_LEVEL) {
		error("log_root level too big: %d >= %d",
			btrfs_super_log_root_level(sb), BTRFS_MAX_LEVEL);
		goto error_out;
	}

	if (!IS_ALIGNED(btrfs_super_root(sb), 4096)) {
		error("tree_root block unaligned: %llu", btrfs_super_root(sb));
		goto error_out;
	}
	if (!IS_ALIGNED(btrfs_super_chunk_root(sb), 4096)) {
		error("chunk_root block unaligned: %llu",
			btrfs_super_chunk_root(sb));
		goto error_out;
	}
	if (!IS_ALIGNED(btrfs_super_log_root(sb), 4096)) {
		error("log_root block unaligned: %llu",
			btrfs_super_log_root(sb));
		goto error_out;
	}
	if (btrfs_super_nodesize(sb) < 4096) {
		error("nodesize too small: %u < 4096",
			btrfs_super_nodesize(sb));
		goto error_out;
	}
	if (!IS_ALIGNED(btrfs_super_nodesize(sb), 4096)) {
		error("nodesize unaligned: %u", btrfs_super_nodesize(sb));
		goto error_out;
	}
	if (btrfs_super_sectorsize(sb) < 4096) {
		error("sectorsize too small: %u < 4096",
			btrfs_super_sectorsize(sb));
		goto error_out;
	}
	if (!IS_ALIGNED(btrfs_super_sectorsize(sb), 4096)) {
		error("sectorsize unaligned: %u", btrfs_super_sectorsize(sb));
		goto error_out;
	}
	if (btrfs_super_total_bytes(sb) == 0) {
		error("invalid total_bytes 0");
		goto error_out;
	}
	if (btrfs_super_bytes_used(sb) < 6 * btrfs_super_nodesize(sb)) {
		error("invalid bytes_used %llu", btrfs_super_bytes_used(sb));
		goto error_out;
	}
	if ((btrfs_super_stripesize(sb) != 4096)
		&& (btrfs_super_stripesize(sb) != btrfs_super_sectorsize(sb))) {
		error("invalid stripesize %u", btrfs_super_stripesize(sb));
		goto error_out;
	}

	if (btrfs_super_incompat_flags(sb) & BTRFS_FEATURE_INCOMPAT_METADATA_UUID)
		metadata_uuid = sb->metadata_uuid;
	else
		metadata_uuid = sb->fsid;

	if (memcmp(metadata_uuid, sb->dev_item.fsid, BTRFS_FSID_SIZE) != 0) {
		char fsid[BTRFS_UUID_UNPARSED_SIZE];
		char dev_fsid[BTRFS_UUID_UNPARSED_SIZE];

		uuid_unparse(sb->metadata_uuid, fsid);
		uuid_unparse(sb->dev_item.fsid, dev_fsid);
		if (sbflags & SBREAD_IGNORE_FSID_MISMATCH) {
			warning("ignored: dev_item fsid mismatch: %s != %s",
					dev_fsid, fsid);
		} else {
			error("dev_item UUID does not match fsid: %s != %s",
					dev_fsid, fsid);
			goto error_out;
		}
	}

	/*
	 * Hint to catch really bogus numbers, bitflips or so
	 */
	if (btrfs_super_num_devices(sb) > (1UL << 31)) {
		warning("suspicious number of devices: %llu",
			btrfs_super_num_devices(sb));
	}

	if (btrfs_super_num_devices(sb) == 0) {
		error("number of devices is 0");
		goto error_out;
	}

	/*
	 * Obvious sys_chunk_array corruptions, it must hold at least one key
	 * and one chunk
	 */
	if (btrfs_super_sys_array_size(sb) > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE) {
		error("system chunk array too big %u > %u",
		      btrfs_super_sys_array_size(sb),
		      BTRFS_SYSTEM_CHUNK_ARRAY_SIZE);
		goto error_out;
	}
	if (btrfs_super_sys_array_size(sb) < sizeof(struct btrfs_disk_key)
			+ sizeof(struct btrfs_chunk)) {
		error("system chunk array too small %u < %zu",
		      btrfs_super_sys_array_size(sb),
		      sizeof(struct btrfs_disk_key) +
		      sizeof(struct btrfs_chunk));
		goto error_out;
	}

	return 0;

error_out:
	error("superblock checksum matches but it has invalid members");
	return -EIO;
}

/*
 * btrfs_read_dev_super - read a valid superblock from a block device
 * @fd:		file descriptor of the device
 * @sb:		buffer where the superblock is going to be read in
 * @sb_bytenr:  offset of the particular superblock copy we want
 * @sbflags:	flags controlling how the superblock is read
 *
 * This function is used by various btrfs commands to obtain a valid superblock.
 *
 * It's mode of operation is controlled by the @sb_bytenr and @sbdflags
 * parameters. If SBREAD_RECOVER flag is set and @sb_bytenr is
 * BTRFS_SUPER_INFO_OFFSET then the function reads all 3 superblock copies and
 * returns the newest one. If SBREAD_RECOVER is not set then only a single
 * copy is read, which one is decided by @sb_bytenr. If @sb_bytenr !=
 * BTRFS_SUPER_INFO_OFFSET then the @sbflags is effectively ignored and only a
 * single copy is read.
 */
int btrfs_read_dev_super(int fd, struct btrfs_super_block *sb, u64 sb_bytenr,
			 unsigned sbflags)
{
	u8 fsid[BTRFS_FSID_SIZE];
	u8 metadata_uuid[BTRFS_FSID_SIZE];
	int fsid_is_initialized = 0;
	char tmp[BTRFS_SUPER_INFO_SIZE];
	struct btrfs_super_block *buf = (struct btrfs_super_block *)tmp;
	int i;
	int ret;
	int max_super = sbflags & SBREAD_RECOVER ? BTRFS_SUPER_MIRROR_MAX : 1;
	u64 transid = 0;
	bool metadata_uuid_set = false;
	u64 bytenr;

	if (sb_bytenr != BTRFS_SUPER_INFO_OFFSET) {
		ret = pread64(fd, buf, BTRFS_SUPER_INFO_SIZE, sb_bytenr);
		/* real error */
		if (ret < 0)
			return -errno;

		/* Not large enough sb, return -ENOENT instead of normal -EIO */
		if (ret < BTRFS_SUPER_INFO_SIZE)
			return -ENOENT;

		if (btrfs_super_bytenr(buf) != sb_bytenr)
			return -EIO;

		ret = btrfs_check_super(buf, sbflags);
		if (ret < 0)
			return ret;
		memcpy(sb, buf, BTRFS_SUPER_INFO_SIZE);
		return 0;
	}

	/*
	* we would like to check all the supers, but that would make
	* a btrfs mount succeed after a mkfs from a different FS.
	* So, we need to add a special mount option to scan for
	* later supers, using BTRFS_SUPER_MIRROR_MAX instead
	*/

	for (i = 0; i < max_super; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = pread64(fd, buf, BTRFS_SUPER_INFO_SIZE, bytenr);
		if (ret < BTRFS_SUPER_INFO_SIZE)
			break;

		if (btrfs_super_bytenr(buf) != bytenr )
			continue;
		/* if magic is NULL, the device was removed */
		if (btrfs_super_magic(buf) == 0 && i == 0)
			break;
		if (btrfs_check_super(buf, sbflags))
			continue;

		if (!fsid_is_initialized) {
			if (btrfs_super_incompat_flags(buf) &
			    BTRFS_FEATURE_INCOMPAT_METADATA_UUID) {
				metadata_uuid_set = true;
				memcpy(metadata_uuid, buf->metadata_uuid,
				       sizeof(metadata_uuid));
			}
			memcpy(fsid, buf->fsid, sizeof(fsid));
			fsid_is_initialized = 1;
		} else if (memcmp(fsid, buf->fsid, sizeof(fsid)) ||
			   (metadata_uuid_set && memcmp(metadata_uuid,
							buf->metadata_uuid,
							sizeof(metadata_uuid)))) {
			/*
			 * the superblocks (the original one and
			 * its backups) contain data of different
			 * filesystems -> the super cannot be trusted
			 */
			continue;
		}

		if (btrfs_super_generation(buf) > transid) {
			memcpy(sb, buf, BTRFS_SUPER_INFO_SIZE);
			transid = btrfs_super_generation(buf);
		}
	}

	return transid > 0 ? 0 : -1;
}

static int write_dev_supers(struct btrfs_fs_info *fs_info,
			    struct btrfs_super_block *sb,
			    struct btrfs_device *device)
{
	u64 bytenr;
	u8 result[BTRFS_CSUM_SIZE];
	int i, ret;
	u16 csum_type = btrfs_super_csum_type(sb);

	/*
	 * We need to write super block after all metadata written.
	 * This is the equivalent of kernel pre-flush for FUA.
	 */
	ret = fsync(device->fd);
	if (ret < 0) {
		error(
		"failed to write super block for devid %llu: flush error: %m",
			device->devid);
		return -errno;
	}
	if (fs_info->super_bytenr != BTRFS_SUPER_INFO_OFFSET) {
		btrfs_set_super_bytenr(sb, fs_info->super_bytenr);
		btrfs_csum_data(csum_type, (u8 *)sb + BTRFS_CSUM_SIZE, result,
				BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
		memcpy(&sb->csum[0], result, BTRFS_CSUM_SIZE);

		/*
		 * super_copy is BTRFS_SUPER_INFO_SIZE bytes and is
		 * zero filled, we can use it directly
		 */
		ret = pwrite64(device->fd, fs_info->super_copy,
				BTRFS_SUPER_INFO_SIZE,
				fs_info->super_bytenr);
		if (ret != BTRFS_SUPER_INFO_SIZE) {
			errno = EIO;
			error(
		"failed to write super block for devid %llu: write error: %m",
				device->devid);
			return -EIO;
		}
		ret = fsync(device->fd);
		if (ret < 0) {
			error(
		"failed to write super block for devid %llu: flush error: %m",
				device->devid);
			return -errno;
		}
		return 0;
	}

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		if (bytenr + BTRFS_SUPER_INFO_SIZE > device->total_bytes)
			break;

		btrfs_set_super_bytenr(sb, bytenr);

		btrfs_csum_data(csum_type, (u8 *)sb + BTRFS_CSUM_SIZE, result,
				      BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
		memcpy(&sb->csum[0], result, BTRFS_CSUM_SIZE);

		/*
		 * super_copy is BTRFS_SUPER_INFO_SIZE bytes and is
		 * zero filled, we can use it directly
		 */
		ret = pwrite64(device->fd, fs_info->super_copy,
				BTRFS_SUPER_INFO_SIZE, bytenr);
		if (ret != BTRFS_SUPER_INFO_SIZE) {
			errno = EIO;
			error(
		"failed to write super block for devid %llu: write error: %m",
				device->devid);
			return -errno;
		}
		/*
		 * Flush after the primary sb write, this is the equivalent of
		 * kernel post-flush for FUA write.
		 */
		if (i == 0) {
			ret = fsync(device->fd);
			if (ret < 0) {
				error(
		"failed to write super block for devid %llu: flush error: %m",
					device->devid);
				return -errno;
			}
		}
	}

	return 0;
}

/*
 * copy all the root pointers into the super backup array.
 * this will bump the backup pointer by one when it is
 * done
 */
static void backup_super_roots(struct btrfs_fs_info *info)
{
	struct btrfs_root_backup *root_backup;
	int next_backup;
	int last_backup;

	last_backup = find_best_backup_root(info->super_copy);
	next_backup = (last_backup + 1) % BTRFS_NUM_BACKUP_ROOTS;

	/* just overwrite the last backup if we're at the same generation */
	root_backup = info->super_copy->super_roots + last_backup;
	if (btrfs_backup_tree_root_gen(root_backup) ==
	    btrfs_header_generation(info->tree_root->node))
		next_backup = last_backup;

	root_backup = info->super_copy->super_roots + next_backup;

	/*
	 * make sure all of our padding and empty slots get zero filled
	 * regardless of which ones we use today
	 */
	memset(root_backup, 0, sizeof(*root_backup));
	btrfs_set_backup_tree_root(root_backup, info->tree_root->node->start);
	btrfs_set_backup_tree_root_gen(root_backup,
			       btrfs_header_generation(info->tree_root->node));
	btrfs_set_backup_tree_root_level(root_backup,
			       btrfs_header_level(info->tree_root->node));

	btrfs_set_backup_chunk_root(root_backup, info->chunk_root->node->start);
	btrfs_set_backup_chunk_root_gen(root_backup,
			       btrfs_header_generation(info->chunk_root->node));
	btrfs_set_backup_chunk_root_level(root_backup,
			       btrfs_header_level(info->chunk_root->node));

	btrfs_set_backup_extent_root(root_backup, info->extent_root->node->start);
	btrfs_set_backup_extent_root_gen(root_backup,
			       btrfs_header_generation(info->extent_root->node));
	btrfs_set_backup_extent_root_level(root_backup,
			       btrfs_header_level(info->extent_root->node));
	/*
	 * we might commit during log recovery, which happens before we set
	 * the fs_root.  Make sure it is valid before we fill it in.
	 */
	if (info->fs_root && info->fs_root->node) {
		btrfs_set_backup_fs_root(root_backup,
					 info->fs_root->node->start);
		btrfs_set_backup_fs_root_gen(root_backup,
			       btrfs_header_generation(info->fs_root->node));
		btrfs_set_backup_fs_root_level(root_backup,
			       btrfs_header_level(info->fs_root->node));
	}

	btrfs_set_backup_dev_root(root_backup, info->dev_root->node->start);
	btrfs_set_backup_dev_root_gen(root_backup,
			       btrfs_header_generation(info->dev_root->node));
	btrfs_set_backup_dev_root_level(root_backup,
				       btrfs_header_level(info->dev_root->node));

	btrfs_set_backup_csum_root(root_backup, info->csum_root->node->start);
	btrfs_set_backup_csum_root_gen(root_backup,
			       btrfs_header_generation(info->csum_root->node));
	btrfs_set_backup_csum_root_level(root_backup,
			       btrfs_header_level(info->csum_root->node));

	btrfs_set_backup_total_bytes(root_backup,
			     btrfs_super_total_bytes(info->super_copy));
	btrfs_set_backup_bytes_used(root_backup,
			     btrfs_super_bytes_used(info->super_copy));
	btrfs_set_backup_num_devices(root_backup,
			     btrfs_super_num_devices(info->super_copy));
};

int write_all_supers(struct btrfs_fs_info *fs_info)
{
	struct list_head *head = &fs_info->fs_devices->devices;
	struct btrfs_device *dev;
	struct btrfs_super_block *sb;
	struct btrfs_dev_item *dev_item;
	int ret;
	u64 flags;

	backup_super_roots(fs_info);
	sb = fs_info->super_copy;
	dev_item = &sb->dev_item;
	list_for_each_entry(dev, head, dev_list) {
		if (!dev->writeable)
			continue;

		btrfs_set_stack_device_generation(dev_item, 0);
		btrfs_set_stack_device_type(dev_item, dev->type);
		btrfs_set_stack_device_id(dev_item, dev->devid);
		btrfs_set_stack_device_total_bytes(dev_item, dev->total_bytes);
		btrfs_set_stack_device_bytes_used(dev_item, dev->bytes_used);
		btrfs_set_stack_device_io_align(dev_item, dev->io_align);
		btrfs_set_stack_device_io_width(dev_item, dev->io_width);
		btrfs_set_stack_device_sector_size(dev_item, dev->sector_size);
		memcpy(dev_item->uuid, dev->uuid, BTRFS_UUID_SIZE);
		memcpy(dev_item->fsid, fs_info->fs_devices->metadata_uuid,
		       BTRFS_FSID_SIZE);

		flags = btrfs_super_flags(sb);
		btrfs_set_super_flags(sb, flags | BTRFS_HEADER_FLAG_WRITTEN);

		ret = write_dev_supers(fs_info, sb, dev);
		if (ret < 0)
			return ret;
	}
	return 0;
}

int write_ctree_super(struct btrfs_trans_handle *trans)
{
	int ret;
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *chunk_root = fs_info->chunk_root;

	if (fs_info->readonly)
		return 0;

	btrfs_set_super_generation(fs_info->super_copy,
				   trans->transid);
	btrfs_set_super_root(fs_info->super_copy,
			     tree_root->node->start);
	btrfs_set_super_root_level(fs_info->super_copy,
				   btrfs_header_level(tree_root->node));
	btrfs_set_super_chunk_root(fs_info->super_copy,
				   chunk_root->node->start);
	btrfs_set_super_chunk_root_level(fs_info->super_copy,
					 btrfs_header_level(chunk_root->node));
	btrfs_set_super_chunk_root_generation(fs_info->super_copy,
				btrfs_header_generation(chunk_root->node));

	ret = write_all_supers(fs_info);
	if (ret)
		fprintf(stderr, "failed to write new super block err %d\n", ret);
	return ret;
}

int close_ctree_fs_info(struct btrfs_fs_info *fs_info)
{
	int ret;
	int err = 0;
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root = fs_info->tree_root;

	if (fs_info->last_trans_committed !=
	    fs_info->generation) {
		BUG_ON(!root);
		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			err = PTR_ERR(trans);
			goto skip_commit;
		}
		btrfs_commit_transaction(trans, root);
		trans = btrfs_start_transaction(root, 1);
		BUG_ON(IS_ERR(trans));
		ret = commit_tree_roots(trans, fs_info);
		BUG_ON(ret);
		ret = __commit_transaction(trans, root);
		BUG_ON(ret);
		ret = write_ctree_super(trans);
		kfree(trans);
		if (ret) {
			err = ret;
			goto skip_commit;
		}
	}

	if (fs_info->finalize_on_close) {
		btrfs_set_super_magic(fs_info->super_copy, BTRFS_MAGIC);
		root->fs_info->finalize_on_close = 0;
		ret = write_all_supers(fs_info);
		if (ret)
			fprintf(stderr,
				"failed to write new super block err %d\n", ret);
	}

skip_commit:
	btrfs_free_block_groups(fs_info);

	free_fs_roots_tree(&fs_info->fs_root_tree);

	btrfs_release_all_roots(fs_info);
	ret = btrfs_close_devices(fs_info->fs_devices);
	btrfs_cleanup_all_caches(fs_info);
	btrfs_free_fs_info(fs_info);
	if (!err)
		err = ret;
	return err;
}

int clean_tree_block(struct extent_buffer *eb)
{
	return clear_extent_buffer_dirty(eb);
}

void btrfs_mark_buffer_dirty(struct extent_buffer *eb)
{
	set_extent_buffer_dirty(eb);
}

int btrfs_buffer_uptodate(struct extent_buffer *buf, u64 parent_transid)
{
	int ret;

	ret = extent_buffer_uptodate(buf);
	if (!ret)
		return ret;

	ret = verify_parent_transid(buf->tree, buf, parent_transid, 1);
	return !ret;
}

int btrfs_set_buffer_uptodate(struct extent_buffer *eb)
{
	return set_extent_buffer_uptodate(eb);
}

struct btrfs_root *btrfs_create_tree(struct btrfs_trans_handle *trans,
				     struct btrfs_fs_info *fs_info,
				     u64 objectid)
{
	struct extent_buffer *leaf;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *root;
	struct btrfs_key key;
	int ret = 0;

	root = kzalloc(sizeof(*root), GFP_KERNEL);
	if (!root)
		return ERR_PTR(-ENOMEM);

	btrfs_setup_root(root, fs_info, objectid);
	root->root_key.objectid = objectid;
	root->root_key.type = BTRFS_ROOT_ITEM_KEY;
	root->root_key.offset = 0;

	leaf = btrfs_alloc_free_block(trans, root, fs_info->nodesize, objectid,
			NULL, 0, 0, 0);
	if (IS_ERR(leaf)) {
		ret = PTR_ERR(leaf);
		leaf = NULL;
		goto fail;
	}

	memset_extent_buffer(leaf, 0, 0, sizeof(struct btrfs_header));
	btrfs_set_header_bytenr(leaf, leaf->start);
	btrfs_set_header_generation(leaf, trans->transid);
	btrfs_set_header_backref_rev(leaf, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(leaf, objectid);
	root->node = leaf;
	write_extent_buffer(leaf, fs_info->fs_devices->metadata_uuid,
			    btrfs_header_fsid(), BTRFS_FSID_SIZE);
	write_extent_buffer(leaf, fs_info->chunk_tree_uuid,
			    btrfs_header_chunk_tree_uuid(leaf),
			    BTRFS_UUID_SIZE);
	btrfs_mark_buffer_dirty(leaf);

	extent_buffer_get(root->node);
	root->commit_root = root->node;
	root->track_dirty = 1;

	root->root_item.flags = 0;
	root->root_item.byte_limit = 0;
	btrfs_set_root_bytenr(&root->root_item, leaf->start);
	btrfs_set_root_generation(&root->root_item, trans->transid);
	btrfs_set_root_level(&root->root_item, 0);
	btrfs_set_root_refs(&root->root_item, 1);
	btrfs_set_root_used(&root->root_item, leaf->len);
	btrfs_set_root_last_snapshot(&root->root_item, 0);
	btrfs_set_root_dirid(&root->root_item, 0);
	memset(root->root_item.uuid, 0, BTRFS_UUID_SIZE);
	root->root_item.drop_level = 0;

	key.objectid = objectid;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_insert_root(trans, tree_root, &key, &root->root_item);
	if (ret)
		goto fail;

	return root;

fail:
	if (leaf)
		free_extent_buffer(leaf);

	kfree(root);
	return ERR_PTR(ret);
}
