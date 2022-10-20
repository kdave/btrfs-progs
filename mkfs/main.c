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

#include "kerncompat.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <uuid/uuid.h>
#include "kernel-lib/list.h"
#include "kernel-lib/list_sort.h"
#include "kernel-lib/rbtree.h"
#include "kernel-lib/sizes.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/zoned.h"
#include "crypto/crc32c.h"
#include "common/defs.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/utils.h"
#include "common/path-utils.h"
#include "common/device-utils.h"
#include "common/device-scan.h"
#include "common/help.h"
#include "common/rbtree-utils.h"
#include "common/parse-utils.h"
#include "common/fsfeatures.h"
#include "common/box.h"
#include "common/units.h"
#include "common/string-utils.h"
#include "check/qgroup-verify.h"
#include "mkfs/common.h"
#include "mkfs/rootdir.h"

struct mkfs_allocation {
	u64 data;
	u64 metadata;
	u64 mixed;
	u64 system;
};

static bool opt_zero_end = true;
static bool opt_discard = true;
static bool opt_zoned = true;
static int opt_oflags = O_RDWR;

struct prepare_device_progress {
	char *file;
	u64 dev_block_count;
	u64 block_count;
	int ret;
};

static int create_metadata_block_groups(struct btrfs_root *root, bool mixed,
				struct mkfs_allocation *allocation)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *trans;
	struct btrfs_space_info *sinfo;
	u64 flags = BTRFS_BLOCK_GROUP_METADATA;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	u64 system_group_size = BTRFS_MKFS_SYSTEM_GROUP_SIZE;
	int ret;

	if (btrfs_is_zoned(fs_info)) {
		/* Two zones are reserved for superblock */
		system_group_size = fs_info->zone_size;
	}

	if (mixed)
		flags |= BTRFS_BLOCK_GROUP_DATA;

	/* Create needed space info to trace extents reservation */
	ret = update_space_info(fs_info, flags, 0, 0, &sinfo);
	if (ret < 0)
		return ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	root->fs_info->system_allocs = 1;
	/*
	 * We already created the block group item for our temporary system
	 * chunk in make_btrfs(), so account for the size here.
	 */
	allocation->system += system_group_size;
	if (ret)
		return ret;

	if (mixed) {
		ret = btrfs_alloc_chunk(trans, fs_info,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_METADATA |
					BTRFS_BLOCK_GROUP_DATA);
		if (ret == -ENOSPC) {
			error("no space to allocate data/metadata chunk");
			goto err;
		}
		if (ret)
			return ret;
		ret = btrfs_make_block_group(trans, fs_info, 0,
					     BTRFS_BLOCK_GROUP_METADATA |
					     BTRFS_BLOCK_GROUP_DATA,
					     chunk_start, chunk_size);
		if (ret)
			return ret;
		allocation->mixed += chunk_size;
	} else {
		ret = btrfs_alloc_chunk(trans, fs_info,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_METADATA);
		if (ret == -ENOSPC) {
			error("no space to allocate metadata chunk");
			goto err;
		}
		if (ret)
			return ret;
		ret = btrfs_make_block_group(trans, fs_info, 0,
					     BTRFS_BLOCK_GROUP_METADATA,
					     chunk_start, chunk_size);
		allocation->metadata += chunk_size;
		if (ret)
			return ret;
	}

	root->fs_info->system_allocs = 0;
	ret = btrfs_commit_transaction(trans, root);
err:
	return ret;
}

static int create_data_block_groups(struct btrfs_trans_handle *trans,
		struct btrfs_root *root, bool mixed,
		struct mkfs_allocation *allocation)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	int ret = 0;

	if (!mixed) {
		struct btrfs_space_info *sinfo;

		ret = update_space_info(fs_info, BTRFS_BLOCK_GROUP_DATA,
					0, 0, &sinfo);
		if (ret < 0)
			return ret;

		ret = btrfs_alloc_chunk(trans, fs_info,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_DATA);
		if (ret == -ENOSPC) {
			error("no space to allocate data chunk");
			goto err;
		}
		if (ret)
			return ret;
		ret = btrfs_make_block_group(trans, fs_info, 0,
					     BTRFS_BLOCK_GROUP_DATA,
					     chunk_start, chunk_size);
		allocation->data += chunk_size;
		if (ret)
			return ret;
	}

err:
	return ret;
}

static int make_root_dir(struct btrfs_trans_handle *trans,
		struct btrfs_root *root)
{
	struct btrfs_key location;
	int ret;

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
			btrfs_super_root_dir(root->fs_info->super_copy),
			&location, BTRFS_FT_DIR, 0);
	if (ret)
		goto err;

	ret = btrfs_insert_inode_ref(trans, root->fs_info->tree_root,
			     "default", 7, location.objectid,
			     BTRFS_ROOT_TREE_DIR_OBJECTID, 0);
	if (ret)
		goto err;

err:
	return ret;
}

static int __recow_root(struct btrfs_trans_handle *trans, struct btrfs_root *root)
{
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = 0;
	key.offset = 0;

	/* Get a path to the left-most leaves */
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

	while (true) {
		struct btrfs_key found_key;

		/*
		 * Our parent nodes must not be newer than the leaf, thus if
		 * the leaf is as new as the transaction, no need to re-COW.
		 */
		if (btrfs_header_generation(path.nodes[0]) == trans->transid)
			goto next;

		/*
		 * Grab the key of current tree block and do a COW search to
		 * the current tree block.
		 */
		btrfs_item_key_to_cpu(path.nodes[0], &key, 0);
		btrfs_release_path(&path);

		/* This will ensure this leaf and all its parent get COWed */
		ret = btrfs_search_slot(trans, root, &key, &path, 0, 1);
		if (ret < 0)
			goto out;
		ret = 0;
		btrfs_item_key_to_cpu(path.nodes[0], &found_key, 0);
		ASSERT(btrfs_comp_cpu_keys(&key, &found_key) == 0);

next:
		ret = btrfs_next_leaf(root, &path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}
out:
	btrfs_release_path(&path);
	return ret;
}

static int recow_global_roots(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root;
	struct rb_node *n;
	int ret = 0;

	for (n = rb_first(&fs_info->global_roots_tree); n; n = rb_next(n)) {
		root = rb_entry(n, struct btrfs_root, rb_node);
		ret = __recow_root(trans, root);
		if (ret)
			return ret;
	}

	return ret;
}

static int recow_roots(struct btrfs_trans_handle *trans,
		       struct btrfs_root *root)
{
	struct btrfs_fs_info *info = root->fs_info;
	int ret;

	ret = __recow_root(trans, info->fs_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->tree_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->chunk_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->dev_root);
	if (ret)
		return ret;

	if (btrfs_fs_compat_ro(info, BLOCK_GROUP_TREE)) {
		ret = __recow_root(trans, info->block_group_root);
		if (ret)
			return ret;
        }
	ret = recow_global_roots(trans);
	if (ret)
		return ret;
	return 0;
}

static int create_one_raid_group(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 type,
			      struct mkfs_allocation *allocation)

{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 chunk_start;
	u64 chunk_size;
	int ret;

	ret = btrfs_alloc_chunk(trans, fs_info,
				&chunk_start, &chunk_size, type);
	if (ret == -ENOSPC) {
		error("not enough free space to allocate chunk");
		exit(1);
	}
	if (ret)
		return ret;

	ret = btrfs_make_block_group(trans, fs_info, 0,
				     type, chunk_start, chunk_size);

	type &= BTRFS_BLOCK_GROUP_TYPE_MASK;
	if (type == BTRFS_BLOCK_GROUP_DATA) {
		allocation->data += chunk_size;
	} else if (type == BTRFS_BLOCK_GROUP_METADATA) {
		allocation->metadata += chunk_size;
	} else if (type == BTRFS_BLOCK_GROUP_SYSTEM) {
		allocation->system += chunk_size;
	} else if (type ==
			(BTRFS_BLOCK_GROUP_METADATA | BTRFS_BLOCK_GROUP_DATA)) {
		allocation->mixed += chunk_size;
	} else {
		error("unrecognized profile type: 0x%llx", type);
		ret = -EINVAL;
	}

	return ret;
}

static int create_raid_groups(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 data_profile,
			      u64 metadata_profile, bool mixed,
			      struct mkfs_allocation *allocation)
{
	int ret = 0;

	if (metadata_profile) {
		u64 meta_flags = BTRFS_BLOCK_GROUP_METADATA;

		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_SYSTEM |
					    metadata_profile, allocation);
		if (ret)
			return ret;

		if (mixed)
			meta_flags |= BTRFS_BLOCK_GROUP_DATA;

		ret = create_one_raid_group(trans, root, meta_flags |
					    metadata_profile, allocation);
		if (ret)
			return ret;

	}
	if (!mixed && data_profile) {
		ret = create_one_raid_group(trans, root,
					    BTRFS_BLOCK_GROUP_DATA |
					    data_profile, allocation);
		if (ret)
			return ret;
	}

	return ret;
}

static void print_usage(int ret)
{
	printf("Usage: mkfs.btrfs [options] dev [ dev ... ]\n");
	printf("Options:\n");
	printf("  allocation profiles:\n");
	printf("\t-d|--data PROFILE           data profile, raid0, raid1, raid1c3, raid1c4, raid5, raid6, raid10, dup or single\n");
	printf("\t-m|--metadata PROFILE       metadata profile, values like for data profile\n");
	printf("\t-M|--mixed                  mix metadata and data together\n");
	printf("  features:\n");
	printf("\t--csum TYPE\n");
	printf("\t--checksum TYPE             checksum algorithm to use, crc32c (default), xxhash, sha256, blake2\n");
	printf("\t-n|--nodesize SIZE          size of btree nodes\n");
	printf("\t-s|--sectorsize SIZE        data block size (may not be mountable by current kernel)\n");
	printf("\t-O|--features LIST          comma separated list of filesystem features (use '-O list-all' to list features)\n");
	printf("\t-R|--runtime-features LIST  comma separated list of runtime features (use '-R list-all' to list runtime features)\n");
	printf("\t-L|--label LABEL            set the filesystem label\n");
	printf("\t-U|--uuid UUID              specify the filesystem UUID (must be unique)\n");
	printf("  creation:\n");
	printf("\t-b|--byte-count SIZE        set size of each device to SIZE (filesystem size is sum of all device sizes)\n");
	printf("\t-r|--rootdir DIR            copy files from DIR to the image root directory\n");
	printf("\t--shrink                    (with --rootdir) shrink the filled filesystem to minimal size\n");
	printf("\t-K|--nodiscard              do not perform whole device TRIM\n");
	printf("\t-f|--force                  force overwrite of existing filesystem\n");
	printf("  general:\n");
	printf("\t-q|--quiet                  no messages except errors\n");
	printf("\t-v|--verbose                increase verbosity level, default is 1\n");
	printf("\t-V|--version                print the mkfs.btrfs version and exit\n");
	printf("\t--help                      print this help and exit\n");
	printf("  deprecated:\n");
	printf("\t-l|--leafsize SIZE          removed in 6.0, use --nodesize\n");
	exit(ret);
}

static int zero_output_file(int out_fd, u64 size)
{
	int loop_num;
	u64 location = 0;
	char buf[SZ_4K];
	int ret = 0, i;
	ssize_t written;

	memset(buf, 0, SZ_4K);

	/* Only zero out the first 1M */
	loop_num = SZ_1M / SZ_4K;
	for (i = 0; i < loop_num; i++) {
		written = pwrite64(out_fd, buf, SZ_4K, location);
		if (written != SZ_4K)
			ret = -EIO;
		location += SZ_4K;
	}

	/* Then enlarge the file to size */
	written = pwrite64(out_fd, buf, 1, size - 1);
	if (written < 1)
		ret = -EIO;
	return ret;
}

static int _cmp_device_by_id(void *priv, struct list_head *a,
			     struct list_head *b)
{
	return list_entry(a, struct btrfs_device, dev_list)->devid -
	       list_entry(b, struct btrfs_device, dev_list)->devid;
}

static void list_all_devices(struct btrfs_root *root)
{
	struct btrfs_fs_devices *fs_devices;
	struct btrfs_device *device;
	int number_of_devices = 0;
	u64 total_block_count = 0;

	fs_devices = root->fs_info->fs_devices;

	list_for_each_entry(device, &fs_devices->devices, dev_list)
		number_of_devices++;

	list_sort(NULL, &fs_devices->devices, _cmp_device_by_id);

	printf("Number of devices:  %d\n", number_of_devices);
	/* printf("Total devices size: %10s\n", */
		/* pretty_size(total_block_count)); */
	printf("Devices:\n");
	printf("   ID        SIZE  PATH\n");
	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		printf("  %3llu  %10s  %s\n",
			device->devid,
			pretty_size(device->total_bytes),
			device->name);
		total_block_count += device->total_bytes;
	}

	printf("\n");
}

static int is_temp_block_group(struct extent_buffer *node,
			       struct btrfs_block_group_item *bgi,
			       u64 data_profile, u64 meta_profile,
			       u64 sys_profile)
{
	u64 flag = btrfs_block_group_flags(node, bgi);
	u64 flag_type = flag & BTRFS_BLOCK_GROUP_TYPE_MASK;
	u64 flag_profile = flag & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	u64 used = btrfs_block_group_used(node, bgi);

	/*
	 * Chunks meets all the following conditions is a temp chunk
	 * 1) Empty chunk
	 * Temp chunk is always empty.
	 *
	 * 2) profile mismatch with mkfs profile.
	 * Temp chunk is always in SINGLE
	 *
	 * 3) Size differs with mkfs_alloc
	 * Special case for SINGLE/SINGLE btrfs.
	 * In that case, temp data chunk and real data chunk are always empty.
	 * So we need to use mkfs_alloc to be sure which chunk is the newly
	 * allocated.
	 *
	 * Normally, new chunk size is equal to mkfs one (One chunk)
	 * If it has multiple chunks, we just refuse to delete any one.
	 * As they are all single, so no real problem will happen.
	 * So only use condition 1) and 2) to judge them.
	 */
	if (used != 0)
		return 0;
	switch (flag_type) {
	case BTRFS_BLOCK_GROUP_DATA:
	case BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_METADATA:
		data_profile &= BTRFS_BLOCK_GROUP_PROFILE_MASK;
		if (flag_profile != data_profile)
			return 1;
		break;
	case BTRFS_BLOCK_GROUP_METADATA:
		meta_profile &= BTRFS_BLOCK_GROUP_PROFILE_MASK;
		if (flag_profile != meta_profile)
			return 1;
		break;
	case BTRFS_BLOCK_GROUP_SYSTEM:
		sys_profile &= BTRFS_BLOCK_GROUP_PROFILE_MASK;
		if (flag_profile != sys_profile)
			return 1;
		break;
	}
	return 0;
}

/* Note: if current is a block group, it will skip it anyway */
static int next_block_group(struct btrfs_root *root,
			    struct btrfs_path *path)
{
	struct btrfs_key key;
	int ret = 0;

	while (1) {
		ret = btrfs_next_item(root, path);
		if (ret)
			goto out;

		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		if (key.type == BTRFS_BLOCK_GROUP_ITEM_KEY)
			goto out;
	}
out:
	return ret;
}

/* This function will cleanup  */
static int cleanup_temp_chunks(struct btrfs_fs_info *fs_info,
			       struct mkfs_allocation *alloc,
			       u64 data_profile, u64 meta_profile,
			       u64 sys_profile)
{
	struct btrfs_trans_handle *trans = NULL;
	struct btrfs_block_group_item *bgi;
	struct btrfs_root *root = btrfs_block_group_root(fs_info);
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_path path;
	int ret = 0;

	btrfs_init_path(&path);
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	key.objectid = 0;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = 0;

	while (1) {
		/*
		 * as the rest of the loop may modify the tree, we need to
		 * start a new search each time.
		 */
		ret = btrfs_search_slot(trans, root, &key, &path, 0, 0);
		if (ret < 0)
			goto out;
		/* Don't pollute ret for >0 case */
		if (ret > 0)
			ret = 0;

		btrfs_item_key_to_cpu(path.nodes[0], &found_key,
				      path.slots[0]);
		if (found_key.objectid < key.objectid)
			goto out;
		if (found_key.type != BTRFS_BLOCK_GROUP_ITEM_KEY) {
			ret = next_block_group(root, &path);
			if (ret < 0)
				goto out;
			if (ret > 0) {
				ret = 0;
				goto out;
			}
			btrfs_item_key_to_cpu(path.nodes[0], &found_key,
					      path.slots[0]);
		}

		bgi = btrfs_item_ptr(path.nodes[0], path.slots[0],
				     struct btrfs_block_group_item);
		if (is_temp_block_group(path.nodes[0], bgi,
					data_profile, meta_profile,
					sys_profile)) {
			u64 flags = btrfs_block_group_flags(path.nodes[0], bgi);

			ret = btrfs_remove_block_group(trans,
					found_key.objectid, found_key.offset);
			if (ret < 0)
				goto out;

			if ((flags & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
			    BTRFS_BLOCK_GROUP_DATA)
				alloc->data -= found_key.offset;
			else if ((flags & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
				 BTRFS_BLOCK_GROUP_METADATA)
				alloc->metadata -= found_key.offset;
			else if ((flags & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
				 BTRFS_BLOCK_GROUP_SYSTEM)
				alloc->system -= found_key.offset;
			else if ((flags & BTRFS_BLOCK_GROUP_TYPE_MASK) ==
				 (BTRFS_BLOCK_GROUP_METADATA |
				  BTRFS_BLOCK_GROUP_DATA))
				alloc->mixed -= found_key.offset;
		}
		btrfs_release_path(&path);
		key.objectid = found_key.objectid + found_key.offset;
	}
out:
	if (trans)
		btrfs_commit_transaction(trans, root);
	btrfs_release_path(&path);
	return ret;
}

/*
 * Just update chunk allocation info, since --rootdir may allocate new
 * chunks which is not updated in @allocation structure.
 */
static void update_chunk_allocation(struct btrfs_fs_info *fs_info,
				    struct mkfs_allocation *allocation)
{
	struct btrfs_block_group *bg_cache;
	const u64 mixed_flag = BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_METADATA;
	u64 search_start = 0;

	allocation->mixed = 0;
	allocation->data = 0;
	allocation->metadata = 0;
	allocation->system = 0;
	while (1) {
		bg_cache = btrfs_lookup_first_block_group(fs_info,
							  search_start);
		if (!bg_cache)
			break;
		if ((bg_cache->flags & mixed_flag) == mixed_flag)
			allocation->mixed += bg_cache->length;
		else if (bg_cache->flags & BTRFS_BLOCK_GROUP_DATA)
			allocation->data += bg_cache->length;
		else if (bg_cache->flags & BTRFS_BLOCK_GROUP_METADATA)
			allocation->metadata += bg_cache->length;
		else
			allocation->system += bg_cache->length;
		search_start = bg_cache->start + bg_cache->length;
	}
}

static int create_data_reloc_tree(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_inode_item *inode;
	struct btrfs_root *root;
	struct btrfs_path path;
	struct btrfs_key key = {
		.objectid = BTRFS_DATA_RELOC_TREE_OBJECTID,
		.type = BTRFS_ROOT_ITEM_KEY,
	};
	u64 ino = BTRFS_FIRST_FREE_OBJECTID;
	char *name = "..";
	int ret;

	root = btrfs_create_tree(trans, fs_info, &key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto out;
	}
	/* Update dirid as created tree has default dirid 0 */
	btrfs_set_root_dirid(&root->root_item, ino);
	ret = btrfs_update_root(trans, fs_info->tree_root, &root->root_key,
				&root->root_item);
	if (ret < 0)
		goto out;

	/* Cache this tree so it can be cleaned up at close_ctree() */
	ret = rb_insert(&fs_info->fs_root_tree, &root->rb_node,
			btrfs_fs_roots_compare_roots);
	if (ret < 0)
		goto out;

	/* Insert INODE_ITEM */
	ret = btrfs_new_inode(trans, root, ino, 0755 | S_IFDIR);
	if (ret < 0)
		goto out;

	/* then INODE_REF */
	ret = btrfs_insert_inode_ref(trans, root, name, strlen(name), ino, ino,
				     0);
	if (ret < 0)
		goto out;

	/* Update nlink of that inode item */
	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	btrfs_init_path(&path);

	ret = btrfs_search_slot(trans, root, &key, &path, 0, 1);
	if (ret > 0) {
		ret = -ENOENT;
		btrfs_release_path(&path);
		goto out;
	}
	if (ret < 0) {
		btrfs_release_path(&path);
		goto out;
	}
	inode = btrfs_item_ptr(path.nodes[0], path.slots[0],
			       struct btrfs_inode_item);
	btrfs_set_inode_nlink(path.nodes[0], inode, 1);
	btrfs_mark_buffer_dirty(path.nodes[0]);
	btrfs_release_path(&path);
	return 0;
out:
	btrfs_abort_transaction(trans, ret);
	return ret;
}

static int create_uuid_tree(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root;
	struct btrfs_key key = {
		.objectid = BTRFS_UUID_TREE_OBJECTID,
		.type = BTRFS_ROOT_ITEM_KEY,
	};
	int ret = 0;

	ASSERT(fs_info->uuid_root == NULL);
	root = btrfs_create_tree(trans, fs_info, &key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto out;
	}

	add_root_to_dirty_list(root);
	fs_info->uuid_root = root;
	ret = btrfs_uuid_tree_add(trans, fs_info->fs_root->root_item.uuid,
				  BTRFS_UUID_KEY_SUBVOL,
				  fs_info->fs_root->root_key.objectid);
	if (ret < 0)
		btrfs_abort_transaction(trans, ret);

out:
	return ret;
}

static int create_global_root(struct btrfs_trans_handle *trans, u64 objectid,
			      int root_id)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root;
	struct btrfs_key key = {
		.objectid = objectid,
		.type = BTRFS_ROOT_ITEM_KEY,
		.offset = root_id,
	};
	int ret = 0;

	root = btrfs_create_tree(trans, fs_info, &key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto out;
	}
	ret = btrfs_global_root_insert(fs_info, root);
out:
	if (ret)
		btrfs_abort_transaction(trans, ret);
	return ret;
}

static int create_global_roots(struct btrfs_trans_handle *trans,
			       int nr_global_roots)
{
	int ret, i;

	for (i = 1; i < nr_global_roots; i++) {
		ret = create_global_root(trans, BTRFS_EXTENT_TREE_OBJECTID, i);
		if (ret)
			return ret;
		ret = create_global_root(trans, BTRFS_CSUM_TREE_OBJECTID, i);
		if (ret)
			return ret;
		ret = create_global_root(trans, BTRFS_FREE_SPACE_TREE_OBJECTID, i);
		if (ret)
			return ret;
	}

	btrfs_set_super_nr_global_roots(trans->fs_info->super_copy,
					nr_global_roots);

	return 0;
}

static int insert_qgroup_items(struct btrfs_trans_handle *trans,
			       struct btrfs_fs_info *fs_info,
			       u64 qgroupid)
{
	struct btrfs_path path;
	struct btrfs_root *quota_root = fs_info->quota_root;
	struct btrfs_key key;
	int ret;

	if (qgroupid >> BTRFS_QGROUP_LEVEL_SHIFT) {
		error("qgroup level other than 0 is not supported yet");
		return -ENOTTY;
	}

	key.objectid = 0;
	key.type = BTRFS_QGROUP_INFO_KEY;
	key.offset = qgroupid;

	btrfs_init_path(&path);
	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(struct btrfs_qgroup_info_item));
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_LIMIT_KEY;
	key.offset = qgroupid;
	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(struct btrfs_qgroup_limit_item));
	btrfs_release_path(&path);
	return ret;
}

static int setup_quota_root(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_qgroup_status_item *qsi;
	struct btrfs_root *quota_root;
	struct btrfs_path path;
	struct btrfs_key key;
	int qgroup_repaired = 0;
	int ret;

	/* One to modify tree root, one for quota root */
	trans = btrfs_start_transaction(fs_info->tree_root, 2);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}
	ret = btrfs_create_root(trans, fs_info, BTRFS_QUOTA_TREE_OBJECTID);
	if (ret < 0) {
		error("failed to create quota root: %d (%m)", ret);
		goto fail;
	}
	quota_root = fs_info->quota_root;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_STATUS_KEY;
	key.offset = 0;

	btrfs_init_path(&path);
	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(*qsi));
	if (ret < 0) {
		error("failed to insert qgroup status item: %d (%m)", ret);
		goto fail;
	}

	qsi = btrfs_item_ptr(path.nodes[0], path.slots[0],
			     struct btrfs_qgroup_status_item);
	btrfs_set_qgroup_status_generation(path.nodes[0], qsi, 0);
	btrfs_set_qgroup_status_rescan(path.nodes[0], qsi, 0);

	/* Mark current status info inconsistent, and fix it later */
	btrfs_set_qgroup_status_flags(path.nodes[0], qsi,
			BTRFS_QGROUP_STATUS_FLAG_ON |
			BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT);
	btrfs_release_path(&path);

	/* Currently mkfs will only create one subvolume */
	ret = insert_qgroup_items(trans, fs_info, BTRFS_FS_TREE_OBJECTID);
	if (ret < 0) {
		error("failed to insert qgroup items: %d (%m)", ret);
		goto fail;
	}

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		return ret;
	}

	/*
	 * Qgroup is setup but with wrong info, use qgroup-verify
	 * infrastructure to repair them.  (Just acts as offline rescan)
	 */
	ret = qgroup_verify_all(fs_info);
	if (ret < 0) {
		error("qgroup rescan failed: %d (%m)", ret);
		return ret;
	}
	ret = repair_qgroups(fs_info, &qgroup_repaired, true);
	if (ret < 0)
		error("failed to fill qgroup info: %d (%m)", ret);
	return ret;
fail:
	btrfs_abort_transaction(trans, ret);
	return ret;
}

/* Thread callback for device preparation */
static void *prepare_one_device(void *ctx)
{
	struct prepare_device_progress *prepare_ctx = ctx;
	int fd;

	fd = open(prepare_ctx->file, opt_oflags);
	if (fd < 0) {
		error("unable to open %s: %m", prepare_ctx->file);
		prepare_ctx->ret = -errno;
		return NULL;
	}
	prepare_ctx->ret = btrfs_prepare_device(fd, prepare_ctx->file,
				&prepare_ctx->dev_block_count,
				prepare_ctx->block_count,
				(bconf.verbose ? PREP_DEVICE_VERBOSE : 0) |
				(opt_zero_end ? PREP_DEVICE_ZERO_END : 0) |
				(opt_discard ? PREP_DEVICE_DISCARD : 0) |
				(opt_zoned ? PREP_DEVICE_ZONED : 0));
	close(fd);

	return NULL;
}

int BOX_MAIN(mkfs)(int argc, char **argv)
{
	char *file;
	struct btrfs_root *root;
	struct btrfs_fs_info *fs_info;
	struct btrfs_trans_handle *trans;
	struct open_ctree_flags ocf = { 0 };
	int fd = -1;
	int ret = 0;
	int close_ret;
	int i;
	bool ssd = false;
	bool shrink_rootdir = false;
	u64 source_dir_size = 0;
	u64 min_dev_size;
	u64 shrink_size;
	int device_count = 0;
	int saved_optind;
	pthread_t *t_prepare = NULL;
	struct prepare_device_progress *prepare_ctx = NULL;
	struct mkfs_allocation allocation = { 0 };
	struct btrfs_mkfs_config mkfs_cfg;
	u64 system_group_size;
	/* Options */
	bool force_overwrite = false;
	struct btrfs_mkfs_features features = btrfs_mkfs_default_features;
	enum btrfs_csum_type csum_type = BTRFS_CSUM_TYPE_CRC32;
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE] = { 0 };
	u32 nodesize = 0;
	bool nodesize_forced = false;
	u32 sectorsize = 0;
	u32 stripesize = 4096;
	u64 metadata_profile = 0;
	bool metadata_profile_set = false;
	u64 data_profile = 0;
	bool data_profile_set = false;
	u64 block_count = 0;
	u64 dev_block_count = 0;
	bool mixed = false;
	char *label = NULL;
	int nr_global_roots = sysconf(_SC_NPROCESSORS_ONLN);
	char *source_dir = NULL;
	bool source_dir_set = false;

	crc32c_optimization_init();
	btrfs_config_init();
	btrfs_assert_feature_buf_size();

	while(1) {
		int c;
		enum {
			GETOPT_VAL_SHRINK = GETOPT_VAL_FIRST,
			GETOPT_VAL_CHECKSUM,
			GETOPT_VAL_GLOBAL_ROOTS,
		};
		static const struct option long_options[] = {
			{ "byte-count", required_argument, NULL, 'b' },
			{ "csum", required_argument, NULL,
				GETOPT_VAL_CHECKSUM },
			{ "checksum", required_argument, NULL,
				GETOPT_VAL_CHECKSUM },
			{ "force", no_argument, NULL, 'f' },
			{ "leafsize", required_argument, NULL, 'l' },
			{ "label", required_argument, NULL, 'L'},
			{ "metadata", required_argument, NULL, 'm' },
			{ "mixed", no_argument, NULL, 'M' },
			{ "nodesize", required_argument, NULL, 'n' },
			{ "sectorsize", required_argument, NULL, 's' },
			{ "data", required_argument, NULL, 'd' },
			{ "version", no_argument, NULL, 'V' },
			{ "rootdir", required_argument, NULL, 'r' },
			{ "nodiscard", no_argument, NULL, 'K' },
			{ "features", required_argument, NULL, 'O' },
			{ "runtime-features", required_argument, NULL, 'R' },
			{ "uuid", required_argument, NULL, 'U' },
			{ "quiet", 0, NULL, 'q' },
			{ "verbose", 0, NULL, 'v' },
			{ "shrink", no_argument, NULL, GETOPT_VAL_SHRINK },
#if EXPERIMENTAL
			{ "num-global-roots", required_argument, NULL, GETOPT_VAL_GLOBAL_ROOTS },
#endif
			{ "help", no_argument, NULL, GETOPT_VAL_HELP },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "A:b:fl:n:s:m:d:L:R:O:r:U:VvMKq",
				long_options, NULL);
		if (c < 0)
			break;
		switch(c) {
			case 'f':
				force_overwrite = true;
				break;
			case 'd':
				ret = parse_bg_profile(optarg, &data_profile);
				if (ret) {
					error("unknown data profile %s", optarg);
					exit(1);
				}
				data_profile_set = true;
				break;
			case 'l':
				/* Deprecated in 4.0 */
				error("--leafsize has been removed in 6.0, use --nodesize");
				ret = 1;
				goto error;
			case 'n':
				nodesize = parse_size_from_string(optarg);
				nodesize_forced = true;
				break;
			case 'L':
				free(label);
				ret = strlen(optarg);
				if (ret >= BTRFS_LABEL_SIZE) {
					error("label %s is too long (max %d)",
						optarg, BTRFS_LABEL_SIZE - 1);
					exit(1);
				}
				label = strdup(optarg);
				break;
			case 'm':
				ret = parse_bg_profile(optarg, &metadata_profile);
				if (ret) {
					error("unknown metadata profile %s", optarg);
					exit(1);
				}
				metadata_profile_set = true;
				break;
			case 'M':
				mixed = true;
				break;
			case 'O': {
				char *orig = strdup(optarg);
				char *tmp = orig;

				tmp = btrfs_parse_fs_features(tmp, &features);
				if (tmp) {
					error("unrecognized filesystem feature '%s'",
							tmp);
					free(orig);
					goto error;
				}
				free(orig);
				if (features.runtime_flags &
				    BTRFS_FEATURE_RUNTIME_LIST_ALL) {
					btrfs_list_all_fs_features(NULL);
					goto success;
				}
				break;
				}
			case 'R': {
				char *orig = strdup(optarg);
				char *tmp = orig;

				tmp = btrfs_parse_runtime_features(tmp,
						&features);
				if (tmp) {
					error("unrecognized runtime feature '%s'",
					      tmp);
					free(orig);
					goto error;
				}
				free(orig);
				if (features.runtime_flags &
				    BTRFS_FEATURE_RUNTIME_LIST_ALL) {
					btrfs_list_all_runtime_features(NULL);
					goto success;
				}
				break;
				}
			case 's':
				sectorsize = parse_size_from_string(optarg);
				break;
			case 'b':
				block_count = parse_size_from_string(optarg);
				opt_zero_end = false;
				break;
			case 'v':
				bconf_be_verbose();
				break;
			case 'V':
				printf("mkfs.btrfs, part of %s\n",
						PACKAGE_STRING);
				goto success;
			case 'r':
				free(source_dir);
				source_dir = strdup(optarg);
				source_dir_set = true;
				break;
			case 'U':
				strncpy(fs_uuid, optarg,
					BTRFS_UUID_UNPARSED_SIZE - 1);
				break;
			case 'K':
				opt_discard = false;
				break;
			case 'q':
				bconf_be_quiet();
				break;
			case GETOPT_VAL_SHRINK:
				shrink_rootdir = true;
				break;
			case GETOPT_VAL_CHECKSUM:
				csum_type = parse_csum_type(optarg);
				break;
			case GETOPT_VAL_GLOBAL_ROOTS:
				btrfs_warn_experimental("Feature: num-global-roots is part of exten-tree-v2");
				nr_global_roots = (int)arg_strtou64(optarg);
				break;
			case GETOPT_VAL_HELP:
			default:
				print_usage(c != GETOPT_VAL_HELP);
		}
	}

	if (bconf.verbose) {
		printf("%s\n", PACKAGE_STRING);
		printf("See %s for more information.\n\n", PACKAGE_URL);
	}

	if (!sectorsize)
		sectorsize = (u32)sysconf(_SC_PAGESIZE);
	if (btrfs_check_sectorsize(sectorsize))
		goto error;

	if (!nodesize)
		nodesize = max_t(u32, sectorsize, BTRFS_MKFS_DEFAULT_NODE_SIZE);

	stripesize = sectorsize;
	saved_optind = optind;
	device_count = argc - optind;
	if (device_count == 0)
		print_usage(1);

	opt_zoned = !!(features.incompat_flags & BTRFS_FEATURE_INCOMPAT_ZONED);

	if (source_dir_set && device_count > 1) {
		error("the option -r is limited to a single device");
		goto error;
	}
	if (shrink_rootdir && !source_dir_set) {
		error("the option --shrink must be used with --rootdir");
		goto error;
	}

	if (*fs_uuid) {
		uuid_t dummy_uuid;

		if (uuid_parse(fs_uuid, dummy_uuid) != 0) {
			error("could not parse UUID: %s", fs_uuid);
			goto error;
		}
		if (!test_uuid_unique(fs_uuid)) {
			error("non-unique UUID: %s", fs_uuid);
			goto error;
		}
	}

	while (device_count-- > 0) {
		file = argv[optind++];
		if (source_dir_set && path_exists(file) == 0)
			ret = 0;
		else if (path_is_block_device(file) == 1)
			ret = test_dev_for_mkfs(file, force_overwrite);
		else
			ret = test_status_for_mkfs(file, force_overwrite);

		if (ret)
			goto error;
	}

	optind = saved_optind;
	device_count = argc - optind;

	file = argv[optind++];
	ssd = device_get_rotational(file);
	if (opt_zoned) {
		if (!zone_size(file)) {
			error("zoned: %s: zone size undefined", file);
			exit(1);
		}
	} else if (zoned_model(file) == ZONED_HOST_MANAGED) {
		if (bconf.verbose)
			printf(
	"Zoned: %s: host-managed device detected, setting zoned feature\n",
			       file);
		opt_zoned = true;
		features.incompat_flags |= BTRFS_FEATURE_INCOMPAT_ZONED;
	}

	/*
	* Set default profiles according to number of added devices.
	* For mixed groups defaults are single/single.
	*/
	if (!mixed) {
		u64 tmp;

		if (!metadata_profile_set) {
			if (device_count > 1)
				tmp = BTRFS_MKFS_DEFAULT_META_MULTI_DEVICE;
			else
				tmp = BTRFS_MKFS_DEFAULT_META_ONE_DEVICE;
			metadata_profile = tmp;
		}
		if (!data_profile_set) {
			if (device_count > 1)
				tmp = BTRFS_MKFS_DEFAULT_DATA_MULTI_DEVICE;
			else
				tmp = BTRFS_MKFS_DEFAULT_DATA_ONE_DEVICE;
			data_profile = tmp;
		}
	} else {
		if (metadata_profile_set || data_profile_set) {
			if (metadata_profile != data_profile) {
				error(
	"with mixed block groups data and metadata profiles must be the same");
				goto error;
			}
		}

		if (!nodesize_forced)
			nodesize = sectorsize;
	}

	/*
	 * FS features that can be set by other means than -O
	 * just set the bit here
	 */
	if (mixed)
		features.incompat_flags |= BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS;

	if ((data_profile | metadata_profile) & BTRFS_BLOCK_GROUP_RAID56_MASK) {
		features.incompat_flags |= BTRFS_FEATURE_INCOMPAT_RAID56;
		warning("RAID5/6 support has known problems is strongly discouraged\n"
			"\t to be used besides testing or evaluation.\n");
	}

	if ((data_profile | metadata_profile) &
	    (BTRFS_BLOCK_GROUP_RAID1C3 | BTRFS_BLOCK_GROUP_RAID1C4)) {
		features.incompat_flags |= BTRFS_FEATURE_INCOMPAT_RAID1C34;
	}

	/* Extent tree v2 comes with a set of mandatory features. */
	if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2) {
		features.incompat_flags |= BTRFS_FEATURE_INCOMPAT_NO_HOLES;
		features.compat_ro_flags |=
			BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |
			BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID |
			BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE;

		if (!nr_global_roots) {
			error("you must set a non-zero num-global-roots value");
			exit(1);
		}
	}

	/* Block group tree feature requires no-holes and free-space-tree. */
	if (features.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE &&
	    (!(features.incompat_flags & BTRFS_FEATURE_INCOMPAT_NO_HOLES) ||
	     !(features.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE))) {
		error("block group tree requires no-holes and free-space-tree features");
		exit(1);
	}
	if (opt_zoned) {
		if (source_dir_set) {
			error("the option -r and zoned mode are incompatible");
			exit(1);
		}

		if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS) {
			error("cannot enable mixed-bg in zoned mode");
			exit(1);
		}

		if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_RAID56) {
			error("cannot enable RAID5/6 in zoned mode");
			exit(1);
		}
	}

	if (btrfs_check_nodesize(nodesize, sectorsize, &features))
		goto error;

	if (sectorsize < sizeof(struct btrfs_super_block)) {
		error("sectorsize smaller than superblock: %u < %zu",
				sectorsize, sizeof(struct btrfs_super_block));
		goto error;
	}

	min_dev_size = btrfs_min_dev_size(nodesize, mixed, metadata_profile,
					  data_profile);
	/*
	 * Enlarge the destination file or create a new one, using the size
	 * calculated from source dir.
	 *
	 * This must be done before minimal device size checks.
	 */
	if (source_dir_set) {
		int oflags = O_RDWR;
		struct stat statbuf;

		if (path_exists(file) == 0)
			oflags |= O_CREAT;

		fd = open(file, oflags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
					 S_IROTH);
		if (fd < 0) {
			error("unable to open %s: %m", file);
			goto error;
		}

		ret = fstat(fd, &statbuf);
		if (ret < 0) {
			error("unable to stat %s: %m", file);
			ret = -errno;
			goto error;
		}

		/*
		 * Block_count not specified, use file/device size first.
		 * Or we will always use source_dir_size calculated for mkfs.
		 */
		if (!block_count)
			block_count = device_get_partition_size_fd_stat(fd, &statbuf);
		source_dir_size = btrfs_mkfs_size_dir(source_dir, sectorsize,
				min_dev_size, metadata_profile, data_profile);
		if (block_count < source_dir_size)
			block_count = source_dir_size;
		ret = zero_output_file(fd, block_count);
		if (ret) {
			error("unable to zero the output file");
			close(fd);
			goto error;
		}
		/* our "device" is the new image file */
		dev_block_count = block_count;
		close(fd);
	}
	/* Check device/block_count after the nodesize is determined */
	if (block_count && block_count < min_dev_size) {
		error("size %llu is too small to make a usable filesystem",
			block_count);
		error("minimum size for btrfs filesystem is %llu",
			min_dev_size);
		goto error;
	}
	/*
	 * 2 zones for the primary superblock
	 * 1 zone for the system block group
	 * 1 zone for a metadata block group
	 * 1 zone for a data block group
	 */
	if (opt_zoned && block_count && block_count < 5 * zone_size(file)) {
		error("size %llu is too small to make a usable filesystem",
			block_count);
		error("minimum size for a zoned btrfs filesystem is %llu",
			min_dev_size);
		goto error;
	}

	for (i = saved_optind; i < saved_optind + device_count; i++) {
		char *path;

		path = argv[i];
		ret = test_minimum_size(path, min_dev_size);
		if (ret < 0) {
			error("failed to check size for %s: %m", path);
			goto error;
		}
		if (ret > 0) {
			error("'%s' is too small to make a usable filesystem",
				path);
			error("minimum size for each btrfs device is %llu",
				min_dev_size);
			goto error;
		}
	}
	ret = test_num_disk_vs_raid(metadata_profile, data_profile,
			device_count, mixed, ssd);
	if (ret)
		goto error;

	if (opt_zoned && (!zoned_profile_supported(BTRFS_BLOCK_GROUP_METADATA | metadata_profile) ||
		      !zoned_profile_supported(BTRFS_BLOCK_GROUP_DATA | data_profile))) {
		error("zoned mode does not yet support RAID/DUP profiles, please specify '-d single -m single' manually");
		goto error;
	}

	t_prepare = calloc(device_count, sizeof(*t_prepare));
	prepare_ctx = calloc(device_count, sizeof(*prepare_ctx));

	if (!t_prepare || !prepare_ctx) {
		error_msg(ERROR_MSG_MEMORY, "thread for preparing devices");
		goto error;
	}

	opt_oflags = O_RDWR;
	for (i = 0; i < device_count; i++) {
		if (opt_zoned &&
		    zoned_model(argv[optind + i - 1]) == ZONED_HOST_MANAGED) {
			opt_oflags |= O_DIRECT;
			break;
		}
	}

	/* Start threads */
	for (i = 0; i < device_count; i++) {
		prepare_ctx[i].file = argv[optind + i - 1];
		prepare_ctx[i].block_count = block_count;
		prepare_ctx[i].dev_block_count = block_count;
		ret = pthread_create(&t_prepare[i], NULL, prepare_one_device,
				     &prepare_ctx[i]);
		if (ret) {
			errno = -ret;
			error("failed to create thread for prepare device %s: %m",
					prepare_ctx[i].file);
			goto error;
		}
	}

	/* Wait for threads */
	for (i = 0; i < device_count; i++)
		pthread_join(t_prepare[i], NULL);
	ret = prepare_ctx[0].ret;

	if (ret) {
		error("unable prepare device: %s", prepare_ctx[0].file);
		goto error;
	}

	device_count--;
	fd = open(file, opt_oflags);
	if (fd < 0) {
		error("unable to open %s: %m", file);
		goto error;
	}
	dev_block_count = prepare_ctx[0].dev_block_count;
	if (block_count && block_count > dev_block_count) {
		error("%s is smaller than requested size, expected %llu, found %llu",
		      file, block_count, dev_block_count);
		goto error;
	}

	/* To create the first block group and chunk 0 in make_btrfs */
	system_group_size = (opt_zoned ? zone_size(file) : BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	if (dev_block_count < system_group_size) {
		error("device is too small to make filesystem, must be at least %llu",
				system_group_size);
		goto error;
	}

	if (btrfs_bg_type_to_tolerated_failures(metadata_profile) <
	    btrfs_bg_type_to_tolerated_failures(data_profile))
		warning("metadata has lower redundancy than data!\n");

	printf("NOTE: several default settings have changed in version 5.15, please make sure\n");
	printf("      this does not affect your deployments:\n");
	printf("      - DUP for metadata (-m dup)\n");
	printf("      - enabled no-holes (-O no-holes)\n");
	printf("      - enabled free-space-tree (-R free-space-tree)\n");
	printf("\n");

	mkfs_cfg.label = label;
	memcpy(mkfs_cfg.fs_uuid, fs_uuid, sizeof(mkfs_cfg.fs_uuid));
	mkfs_cfg.num_bytes = dev_block_count;
	mkfs_cfg.nodesize = nodesize;
	mkfs_cfg.sectorsize = sectorsize;
	mkfs_cfg.stripesize = stripesize;
	mkfs_cfg.features = features;
	mkfs_cfg.csum_type = csum_type;
	mkfs_cfg.leaf_data_size = __BTRFS_LEAF_DATA_SIZE(nodesize);
	if (opt_zoned)
		mkfs_cfg.zone_size = zone_size(file);
	else
		mkfs_cfg.zone_size = 0;

	ret = make_btrfs(fd, &mkfs_cfg);
	if (ret) {
		errno = -ret;
		error("error during mkfs: %m");
		goto error;
	}

	ocf.filename = file;
	ocf.flags = OPEN_CTREE_WRITES | OPEN_CTREE_TEMPORARY_SUPER;
	fs_info = open_ctree_fs_info(&ocf);
	if (!fs_info) {
		error("open ctree failed");
		goto error;
	}
	close(fd);
	fd = -1;
	root = fs_info->fs_root;

	ret = create_metadata_block_groups(root, mixed, &allocation);
	if (ret) {
		error("failed to create default block groups: %d", ret);
		goto error;
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		errno = -PTR_ERR(trans);
		error_msg(ERROR_MSG_START_TRANS, "%m");
		goto error;
	}

	ret = create_data_block_groups(trans, root, mixed, &allocation);
	if (ret) {
		error("failed to create default data block groups: %d", ret);
		goto error;
	}

	if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2) {
		ret = create_global_roots(trans, nr_global_roots);
		if (ret) {
			error("failed to create global roots: %d", ret);
			goto error;
		}
	}

	ret = make_root_dir(trans, root);
	if (ret) {
		error("failed to setup the root directory: %d", ret);
		goto error;
	}

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		goto out;
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		errno = -PTR_ERR(trans);
		error_msg(ERROR_MSG_START_TRANS, "%m");
		goto error;
	}

	if (device_count == 0)
		goto raid_groups;

	while (device_count-- > 0) {
		int dev_index = argc - saved_optind - device_count - 1;
		file = argv[optind++];

		fd = open(file, opt_oflags);
		if (fd < 0) {
			error("unable to open %s: %m", file);
			goto error;
		}
		ret = btrfs_device_already_in_root(root, fd,
						   BTRFS_SUPER_INFO_OFFSET);
		if (ret) {
			error("skipping duplicate device %s in the filesystem",
				file);
			close(fd);
			continue;
		}
		dev_block_count = prepare_ctx[dev_index].dev_block_count;

		if (prepare_ctx[dev_index].ret) {
			errno = -prepare_ctx[dev_index].ret;
			error("unable to prepare device %s: %m",
					prepare_ctx[dev_index].file);
			goto error;
		}

		ret = btrfs_add_to_fsid(trans, root, fd, file, dev_block_count,
					sectorsize, sectorsize, sectorsize);
		if (ret) {
			error("unable to add %s to filesystem: %d", file, ret);
			goto error;
		}
		if (bconf.verbose >= 2) {
			struct btrfs_device *device;

			device = container_of(fs_info->fs_devices->devices.next,
					struct btrfs_device, dev_list);
			printf("adding device %s id %llu\n", file, device->devid);
		}
	}

raid_groups:
	ret = create_raid_groups(trans, root, data_profile,
			 metadata_profile, mixed, &allocation);
	if (ret) {
		error("unable to create raid groups: %d", ret);
		goto out;
	}

	/*
	 * Commit current transaction so we can COW all existing tree blocks
	 * to newly created raid groups.
	 * As currently we use btrfs_search_slot() to COW tree blocks in
	 * recow_roots(), if a tree block is already modified in current trans,
	 * it won't be re-COWed, thus it will stay in temporary chunks.
	 */
	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "before recowing trees: %m");
		goto out;
	}
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		errno = -PTR_ERR(trans);
		error_msg(ERROR_MSG_START_TRANS, "%m");
		goto error;
	}
	/* COW all tree blocks to newly created chunks */
	ret = recow_roots(trans, root);
	if (ret) {
		errno = -ret;
		error("unable to COW tree blocks to new profiles: %m");
		goto out;
	}

	ret = create_data_reloc_tree(trans);
	if (ret) {
		error("unable to create data reloc tree: %d", ret);
		goto out;
	}

	ret = create_uuid_tree(trans);
	if (ret)
		warning(
	"unable to create uuid tree, will be created after mount: %d", ret);

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		goto out;
	}

	ret = cleanup_temp_chunks(fs_info, &allocation, data_profile,
				  metadata_profile, metadata_profile);
	if (ret < 0) {
		error("failed to cleanup temporary chunks: %d", ret);
		goto out;
	}

	if (source_dir_set) {
		ret = btrfs_mkfs_fill_dir(source_dir, root, bconf.verbose);
		if (ret) {
			error("error while filling filesystem: %d", ret);
			goto out;
		}
		if (shrink_rootdir) {
			ret = btrfs_mkfs_shrink_fs(fs_info, &shrink_size,
						   shrink_rootdir);
			if (ret < 0) {
				error("error while shrinking filesystem: %d",
					ret);
				goto out;
			}
		}
	}

	if (features.runtime_flags & BTRFS_FEATURE_RUNTIME_QUOTA) {
		ret = setup_quota_root(fs_info);
		if (ret < 0) {
			error("failed to initialize quota: %d (%m)", ret);
			goto out;
		}
	}
	if (bconf.verbose) {
		char features_buf[BTRFS_FEATURE_STRING_BUF_SIZE];

		update_chunk_allocation(fs_info, &allocation);
		printf("Label:              %s\n", label);
		printf("UUID:               %s\n", mkfs_cfg.fs_uuid);
		printf("Node size:          %u\n", nodesize);
		printf("Sector size:        %u\n", sectorsize);
		printf("Filesystem size:    %s\n",
			pretty_size(btrfs_super_total_bytes(fs_info->super_copy)));
		printf("Block group profiles:\n");
		if (allocation.data)
			printf("  Data:             %-8s %16s\n",
				btrfs_group_profile_str(data_profile),
				pretty_size(allocation.data));
		if (allocation.metadata)
			printf("  Metadata:         %-8s %16s\n",
				btrfs_group_profile_str(metadata_profile),
				pretty_size(allocation.metadata));
		if (allocation.mixed)
			printf("  Data+Metadata:    %-8s %16s\n",
				btrfs_group_profile_str(data_profile),
				pretty_size(allocation.mixed));
		printf("  System:           %-8s %16s\n",
			btrfs_group_profile_str(metadata_profile),
			pretty_size(allocation.system));
		printf("SSD detected:       %s\n", ssd ? "yes" : "no");
		printf("Zoned device:       %s\n", opt_zoned ? "yes" : "no");
		if (opt_zoned)
			printf("  Zone size:        %s\n",
			       pretty_size(fs_info->zone_size));
		btrfs_parse_fs_features_to_string(features_buf, &features);
#if EXPERIMENTAL
		printf("Features:           %s\n", features_buf);
#else
		printf("Incompat features:  %s\n", features_buf);
		btrfs_parse_runtime_features_to_string(features_buf, &features);
		printf("Runtime features:   %s\n", features_buf);
#endif
		printf("Checksum:           %s",
		       btrfs_super_csum_name(mkfs_cfg.csum_type));
		printf("\n");

		list_all_devices(root);

		if (mkfs_cfg.csum_type == BTRFS_CSUM_TYPE_SHA256) {
			printf(
"NOTE: you may need to manually load kernel module implementing accelerated SHA256 in case\n"
"      the generic implementation is built-in, before mount. Check lsmod or /proc/crypto\n\n"
);
		}
	}

	/*
	 * The filesystem is now fully set up, commit the remaining changes and
	 * fix the signature as the last step before closing the devices.
	 */
	fs_info->finalize_on_close = 1;
out:
	close_ret = close_ctree(root);

	if (!close_ret) {
		optind = saved_optind;
		device_count = argc - optind;
		while (device_count-- > 0) {
			file = argv[optind++];
			if (path_is_block_device(file) == 1)
				btrfs_register_one_device(file);
		}
	}

	if (!ret && close_ret) {
		ret = close_ret;
		error("failed to close ctree, the filesystem may be inconsistent: %d",
		      ret);
	}

	btrfs_close_all_devices();
	free(t_prepare);
	free(prepare_ctx);
	free(label);
	free(source_dir);

	return !!ret;

error:
	if (fd > 0)
		close(fd);

	free(t_prepare);
	free(prepare_ctx);
	free(label);
	free(source_dir);
	exit(1);
success:
	exit(0);
}
