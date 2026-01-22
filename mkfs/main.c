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
#include <blkid/blkid.h>
#include <linux/version.h>
#include "kernel-lib/list.h"
#include "kernel-lib/list_sort.h"
#include "kernel-lib/rbtree.h"
#include "kernel-lib/sizes.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/zoned.h"
#include "crypto/hash.h"
#include "common/defs.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/cpu-utils.h"
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
#include "common/string-table.h"
#include "common/root-tree-utils.h"
#include "cmds/commands.h"
#include "check/qgroup-verify.h"
#include "mkfs/common.h"
#include "mkfs/rootdir.h"

struct mkfs_allocation {
	u64 data;
	u64 metadata;
	u64 mixed;
	u64 system;
	u64 remap;
};

static bool opt_zero_end = true;
static bool opt_discard = true;
static bool opt_zoned = true;
static int opt_oflags = O_RDWR;

struct prepare_device_progress {
	int fd;
	char *file;
	u64 dev_byte_count;
	u64 byte_count;
	int ret;
};

static int create_metadata_block_groups(struct btrfs_root *root,
					u64 incompat_flags,
					struct mkfs_allocation *allocation,
					u64 metadata_profile)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *trans;
	struct btrfs_space_info *sinfo;
	u64 flags = BTRFS_BLOCK_GROUP_METADATA;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	u64 system_group_size = BTRFS_MKFS_SYSTEM_GROUP_SIZE;
	bool mixed = incompat_flags & BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS;
	bool remap_tree = incompat_flags & BTRFS_FEATURE_INCOMPAT_REMAP_TREE;
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

	if (remap_tree) {
		ret = btrfs_alloc_chunk(trans, fs_info,
					&chunk_start, &chunk_size,
					BTRFS_BLOCK_GROUP_METADATA_REMAP);
		if (ret == -ENOSPC) {
			error("no space to allocate remap chunk");
			goto err;
		}
		if (ret)
			return ret;
		ret = btrfs_make_block_group(trans, fs_info, 0,
					     BTRFS_BLOCK_GROUP_METADATA_REMAP,
					     chunk_start, chunk_size);
		allocation->remap += chunk_size;
		if (ret)
			return ret;
	}

	root->fs_info->system_allocs = 0;
	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
	}
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
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int ret;

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
		UASSERT(btrfs_comp_cpu_keys(&key, &found_key) == 0);

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
	if (btrfs_fs_incompat(info, RAID_STRIPE_TREE)) {
		ret = __recow_root(trans, info->stripe_root);
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

static const char * const mkfs_usage[] = {
	"mkfs.btrfs [options] <dev> [<dev...>]",
	"Create a BTRFS filesystem on a device or multiple devices",
	"",
	"Allocation profiles:",
	OPTLINE("-d|--data PROFILE", "data profile, raid0, raid1, raid1c3, raid1c4, raid5, raid6, raid10, dup or single"),
	OPTLINE("-m|--metadata PROFILE", "metadata profile, values like for data profile"),
	OPTLINE("-M|--mixed","mix metadata and data together"),
	"",
	"Features:",
	OPTLINE("--csum TYPE", ""),
	OPTLINE("--checksum TYPE", "checksum algorithm to use, crc32c (default), xxhash, sha256, blake2"),
	OPTLINE("-n|--nodesize SIZE", "size of btree nodes, 16K (default), 4K, 8K, 32K, 64K"),
	OPTLINE("-s|--sectorsize SIZE", "data block size (default 4K, may not be mountable by current kernel if CPU page size is different)"),
#if EXPERIMENTAL
	OPTLINE("","experimental: 2K (needs experimental subpage support for 2K on x86_64)"),
#endif
	OPTLINE("-O|--features LIST", "comma separated list of filesystem features (use '-O list-all' to list features)"),
	OPTLINE("-L|--label LABEL", "set the filesystem label (maximum length 255)"),
	OPTLINE("-U|--uuid UUID", "specify the filesystem UUID (must be unique for a filesystem with multiple devices)"),
	OPTLINE("--device-uuid UUID", "specify the filesystem device UUID (a.k.a sub-uuid) (for single device filesystem only)"),
	"",
	"Creation:",
	OPTLINE("-b|--byte-count SIZE", "set size of each device to SIZE (filesystem size is sum of all device sizes)"),
	OPTLINE("-r|--rootdir DIR", "copy files from DIR to the image root directory, can be combined with --subvol"),
	OPTLINE("--compress ALGO[:LEVEL]", "compress files by algorithm and level, ALGO can be 'no' (default), zstd, lzo, zlib"),
	OPTLINE("", "Built-in:"),
#if COMPRESSION_ZSTD
	OPTLINE("", "- ZSTD: yes (levels 1..15)"),
#else
	OPTLINE("", "- ZSTD: no"),
#endif
#if COMPRESSION_LZO
	OPTLINE("", "- LZO: yes"),
#else
	OPTLINE("", "- LZO: no"),
#endif
	OPTLINE("", "- ZLIB: yes (levels 1..9)"),
	OPTLINE("-u|--subvol TYPE:SUBDIR", "create SUBDIR as subvolume rather than normal directory, can be specified multiple times"),
	OPTLINE("", "TYPE is one of:"),
	OPTLINE("", "- rw - (default) create a writeable subvolume in SUBDIR"),
	OPTLINE("", "- ro - create the subvolume as read-only"),
	OPTLINE("", "- default - the SUBDIR will be a subvolume and also set as default (can be specified only once)"),
	OPTLINE("", "- default-ro - like 'default' and is created as read-only subvolume (can be specified only once)"),
	OPTLINE("--inode-flags FLAGS:PATH", "specify that path to have inode flags, can be specified multiple times"),
	OPTLINE("", "FLAGS is one of:"),
	OPTLINE("", "- nodatacow - disable data CoW, implies nodatasum for regular files"),
	OPTLINE("", "- nodatasum - disable data checksum only"),
	OPTLINE("--reflink", "(with --rootdir) write file data by cloning ranges"),
	OPTLINE("--shrink", "(with --rootdir) shrink the filled filesystem to minimal size"),
	OPTLINE("-K|--nodiscard", "do not perform whole device TRIM"),
	OPTLINE("-f|--force", "force overwrite of existing filesystem"),
	"",
	"General:",
	OPTLINE("-q|--quiet", "no messages except errors"),
	OPTLINE("-v|--verbose", "increase verbosity level, default is 1"),
	OPTLINE("-V|--version", "print the mkfs.btrfs version, builtin features and exit"),
	OPTLINE("--help", "print this help and exit"),
	"",
	"Deprecated:",
	OPTLINE("-R|--runtime-features LIST", "removed in 6.3, use -O|--features"),
	NULL
};

static const struct cmd_struct mkfs_cmd = {
	.usagestr = mkfs_usage
};

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
		written = pwrite(out_fd, buf, SZ_4K, location);
		if (written != SZ_4K)
			ret = -EIO;
		location += SZ_4K;
	}

	/* Then enlarge the file to size */
	written = pwrite(out_fd, buf, 1, size - 1);
	if (written < 1)
		ret = -EIO;
	return ret;
}

static void list_all_devices(struct btrfs_root *root, bool is_zoned)
{
	struct btrfs_fs_devices *fs_devices;
	struct btrfs_device *device;
	int number_of_devices = 0;
	struct string_table *tab;
	int row, col;

	fs_devices = root->fs_info->fs_devices;

	list_for_each_entry(device, &fs_devices->devices, dev_list)
		number_of_devices++;

	list_sort(NULL, &fs_devices->devices, cmp_device_id);

	printf("Number of devices:  %d\n", number_of_devices);
	printf("Devices:\n");
	if (is_zoned)
		tab = table_create(4, number_of_devices + 1);
	else
		tab = table_create(3, number_of_devices + 1);
	tab->spacing = STRING_TABLE_SPACING_2;
	col = 0;
	table_printf(tab, col++, 0, ">   ID");
	table_printf(tab, col++, 0, ">      SIZE");
	if (is_zoned)
		table_printf(tab, col++, 0, ">ZONES");
	table_printf(tab, col++, 0, "<PATH");

	row = 1;
	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		col = 0;
		table_printf(tab, col++, row, ">%llu", device->devid);
		table_printf(tab, col++, row, ">%s", pretty_size(device->total_bytes));
		if (is_zoned)
			table_printf(tab, col++, row, ">%u", device->zone_info->nr_zones);
		table_printf(tab, col++, row, "<%s", device->name);
		row++;
	}
	table_dump(tab);
	printf("\n");
	table_free(tab);
}

static bool is_temp_block_group(struct extent_buffer *node,
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
		return false;
	switch (flag_type) {
	case BTRFS_BLOCK_GROUP_DATA:
	case BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_METADATA:
		data_profile &= BTRFS_BLOCK_GROUP_PROFILE_MASK;
		if (flag_profile != data_profile)
			return true;
		break;
	case BTRFS_BLOCK_GROUP_METADATA:
		meta_profile &= BTRFS_BLOCK_GROUP_PROFILE_MASK;
		if (flag_profile != meta_profile)
			return true;
		break;
	case BTRFS_BLOCK_GROUP_SYSTEM:
		sys_profile &= BTRFS_BLOCK_GROUP_PROFILE_MASK;
		if (flag_profile != sys_profile)
			return true;
		break;
	}
	return false;
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

static int discard_logical_range(struct btrfs_fs_info *fs_info, u64 start, u64 len)
{
	int ret;
	u64 cur_offset = 0;
	u64 cur_len;

	while (cur_offset < len) {
		struct btrfs_multi_bio *multi = NULL;
		struct btrfs_device *device;

		cur_len = len - cur_offset;
		ret = btrfs_map_block(fs_info, WRITE, start + cur_offset, &cur_len, &multi, 0, NULL);
		if (ret)
			return ret;

		if (multi->type & BTRFS_BLOCK_GROUP_RAID56_MASK) {
			free(multi);
			return 0;
		}

		cur_len = min(cur_len, len - cur_offset);

		for (int i = 0; i < multi->num_stripes; i++) {
			device = multi->stripes[i].dev;

			ret = device_discard_blocks(device->fd,
						    multi->stripes[i].physical,
						    cur_len);

			if (ret < 0) {
				free(multi);

				if (ret == -EOPNOTSUPP)
					ret = 0;

				return ret;
			}
		}
		free(multi);
		multi = NULL;
		cur_offset += cur_len;
	}
	return 0;
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
	struct btrfs_path path = { 0 };
	int ret = 0;

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

			if (opt_discard) {
				ret = discard_logical_range(fs_info,
							    found_key.objectid,
							    found_key.offset);
				if (ret < 0)
					goto out;
			}

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
	if (trans) {
		ret = btrfs_commit_transaction(trans, root);
		if (ret) {
			errno = -ret;
			error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		}
	}
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

	root = btrfs_create_tree(trans, &key);
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
	struct btrfs_path path = { 0 };
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

/*
 * Workaround for squota so the enable_gen can be properly used.
 */
static int touch_root_subvol(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key = {
		.objectid = BTRFS_FIRST_FREE_OBJECTID,
		.type = BTRFS_INODE_ITEM_KEY,
		.offset = 0,
	};
	struct extent_buffer *leaf;
	int slot;
	struct btrfs_path path = { 0 };
	int ret;

	trans = btrfs_start_transaction(fs_info->fs_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}
	ret = btrfs_search_slot(trans, fs_info->fs_root, &key, &path, 0, 1);
	if (ret)
		goto fail;
	leaf = path.nodes[0];
	slot = path.slots[0];
	btrfs_item_key_to_cpu(leaf, &key, slot);
	btrfs_mark_buffer_dirty(leaf);
	ret = btrfs_commit_transaction(trans, fs_info->fs_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		return ret;
	}
	btrfs_release_path(&path);
	return 0;
fail:
	btrfs_abort_transaction(trans, ret);
	btrfs_release_path(&path);
	return ret;
}

static int setup_quota_root(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_qgroup_status_item *qsi;
	struct btrfs_root *quota_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int qgroup_repaired = 0;
	bool simple = btrfs_fs_incompat(fs_info, SIMPLE_QUOTA);
	int flags;
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
		errno = -ret;
		error("failed to create quota root: %m");
		goto fail;
	}
	quota_root = fs_info->quota_root;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_STATUS_KEY;
	key.offset = 0;

	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(*qsi));
	if (ret < 0) {
		errno = -ret;
		error("failed to insert qgroup status item: %m");
		goto fail;
	}

	qsi = btrfs_item_ptr(path.nodes[0], path.slots[0],
			     struct btrfs_qgroup_status_item);
	btrfs_set_qgroup_status_generation(path.nodes[0], qsi, trans->transid);
	btrfs_set_qgroup_status_rescan(path.nodes[0], qsi, 0);
	flags = BTRFS_QGROUP_STATUS_FLAG_ON;
	if (simple) {
		btrfs_set_qgroup_status_enable_gen(path.nodes[0], qsi, trans->transid);
		flags |= BTRFS_QGROUP_STATUS_FLAG_SIMPLE_MODE;
	}
	else {
		flags |= BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT;
	}

	btrfs_set_qgroup_status_version(path.nodes[0], qsi, 1);
	btrfs_set_qgroup_status_flags(path.nodes[0], qsi, flags);
	btrfs_release_path(&path);

	/* Currently mkfs will only create one subvolume */
	ret = insert_qgroup_items(trans, fs_info, BTRFS_FS_TREE_OBJECTID);
	if (ret < 0) {
		errno = -ret;
		error("failed to insert qgroup items: %m");
		goto fail;
	}

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		return ret;
	}

	/* Hack to count the default subvol metadata by dirtying it */
	if (simple) {
		ret = touch_root_subvol(fs_info);
		if (ret) {
			errno = -ret;
			error("failed to touch root dir for simple quota accounting: %m");
			goto fail;
		}
	}

	/*
	 * Qgroup is setup but with wrong info, use qgroup-verify
	 * infrastructure to repair them.  (Just acts as offline rescan)
	 */
	ret = qgroup_verify_all(fs_info);
	if (ret < 0) {
		errno = -ret;
		error("qgroup rescan failed: %m");
		return ret;
	}
	ret = repair_qgroups(fs_info, &qgroup_repaired, true);
	if (ret < 0) {
		errno = -ret;
		error("failed to fill qgroup info: %m");
	}
	return ret;
fail:
	btrfs_abort_transaction(trans, ret);
	return ret;
}

static int setup_raid_stripe_tree_root(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *stripe_root;
	struct btrfs_key key = {
		.objectid = BTRFS_RAID_STRIPE_TREE_OBJECTID,
		.type = BTRFS_ROOT_ITEM_KEY,
	};
	int ret;

	trans = btrfs_start_transaction(fs_info->tree_root, 0);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	stripe_root = btrfs_create_tree(trans, &key);
	if (IS_ERR(stripe_root))  {
		ret = PTR_ERR(stripe_root);
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	fs_info->stripe_root = stripe_root;
	add_root_to_dirty_list(stripe_root);

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		return ret;
	}

	return 0;
}

static int setup_remap_tree_root(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *remap_root;
	struct btrfs_key key = {
		.objectid = BTRFS_REMAP_TREE_OBJECTID,
		.type = BTRFS_ROOT_ITEM_KEY,
	};
	int ret;

	trans = btrfs_start_transaction(fs_info->tree_root, 0);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	remap_root = btrfs_create_tree(trans, &key);
	if (IS_ERR(remap_root))  {
		ret = PTR_ERR(remap_root);
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	fs_info->remap_root = remap_root;
	add_root_to_dirty_list(remap_root);

	btrfs_set_super_remap_root(fs_info->super_copy,
				   remap_root->root_item.bytenr);
	btrfs_set_super_remap_root_generation(fs_info->super_copy,
					      remap_root->root_item.generation);
	btrfs_set_super_remap_root_level(fs_info->super_copy,
					 remap_root->root_item.level);

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		return ret;
	}

	return 0;
}

/* Thread callback for device preparation */
static void *prepare_one_device(void *ctx)
{
	struct prepare_device_progress *prepare_ctx = ctx;

	prepare_ctx->fd = open(prepare_ctx->file, opt_oflags);
	if (prepare_ctx->fd < 0) {
		error("unable to open %s: %m", prepare_ctx->file);
		prepare_ctx->ret = -errno;
		return NULL;
	}
	prepare_ctx->ret = btrfs_prepare_device(prepare_ctx->fd,
				prepare_ctx->file,
				&prepare_ctx->dev_byte_count,
				prepare_ctx->byte_count,
				(bconf.verbose ? PREP_DEVICE_VERBOSE : 0) |
				(opt_zero_end ? PREP_DEVICE_ZERO_END : 0) |
				(opt_discard ? PREP_DEVICE_DISCARD : 0) |
				(opt_zoned ? PREP_DEVICE_ZONED : 0));
	return NULL;
}

static int parse_compression(const char *str, enum btrfs_compression_type *type,
			     unsigned int *level)
{
	const char *colon;
	size_t type_size;
	unsigned int default_level, max_level;

	*level = 0;
	if (strcmp(str, "no") == 0) {
		*type = BTRFS_COMPRESS_NONE;
		return 0;
	}

	colon = strstr(str, ":");

	if (colon)
		type_size = colon - str;
	else
		type_size = strlen(str);

	if (strncmp(str, "zlib", type_size) == 0) {
		*type = BTRFS_COMPRESS_ZLIB;
		max_level = ZLIB_BTRFS_MAX_LEVEL;
		default_level = ZLIB_BTRFS_DEFAULT_LEVEL;
	} else if (strncmp(str, "lzo", type_size) == 0) {
#if COMPRESSION_LZO
		*type = BTRFS_COMPRESS_LZO;
		max_level = 1;
		default_level = 1;
#else
		error("lzo support not compiled in");
		return 1;
#endif
	} else if (strncmp(str, "zstd", type_size) == 0) {
#if COMPRESSION_ZSTD
		*type = BTRFS_COMPRESS_ZSTD;
		max_level = ZSTD_BTRFS_MAX_LEVEL;
		default_level = ZSTD_BTRFS_DEFAULT_LEVEL;
#else
		error("zstd support not compiled in");
		return 1;
#endif
	} else {
		return 1;
	}

	if (colon) {
		u64 tmplevel = arg_strtou64(colon + 1);

		if (tmplevel > UINT_MAX)
			return 1;

		if (tmplevel == 0)
			*level = default_level;
		else if (tmplevel > max_level) {
			error("compression level %llu out of range [1..%u]",
			      tmplevel, max_level);
			return 1;
		} else {
			*level = tmplevel;
		}
	}

	return 0;
}

static int parse_subvolume(const char *path, struct list_head *subvols,
			   bool *has_default_subvol)
{
	struct rootdir_subvol *subvol;
	char *colon;
	bool valid_prefix = false;

	subvol = calloc(1, sizeof(struct rootdir_subvol));
	if (!subvol) {
		error_mem(NULL);
		return 1;
	}

	colon = strstr(optarg, ":");

	if (colon) {
		if (!string_has_prefix(optarg, "default:")) {
			subvol->is_default = true;
			valid_prefix = true;
		} else if (!string_has_prefix(optarg, "ro:")) {
			subvol->readonly = true;
			valid_prefix = true;
		} else if (!string_has_prefix(optarg, "rw:")) {
			subvol->readonly = false;
			valid_prefix = true;
		} else if (!string_has_prefix(optarg, "default-ro:")) {
			subvol->is_default = true;
			subvol->readonly = true;
			valid_prefix = true;
		}
	}

	if (arg_copy_path(subvol->dir, valid_prefix ? colon + 1 : optarg,
			  sizeof(subvol->dir))) {
		error("--subvol path too long");
		free(subvol);
		return 1;
	}

	if (subvol->is_default) {
		if (*has_default_subvol) {
			error("default subvolume can only be specified once");
			free(subvol);
			return 1;
		}
		*has_default_subvol = true;
	}

	list_add_tail(&subvol->list, subvols);

	return 0;
}

static int parse_inode_flags(const char *option, struct list_head *inode_flags_list)
{
	struct rootdir_inode_flags_entry *entry = NULL;
	char *colon;
	char *option_dup = NULL;
	char *token;
	int ret;

	option_dup = strdup(option);
	if (!option_dup) {
		ret = -ENOMEM;
		error_mem(NULL);
		goto cleanup;
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		ret = -ENOMEM;
		error_mem(NULL);
		goto cleanup;
	}
	colon = strstr(option_dup, ":");
	if (!colon) {
		error("invalid inode flags: %s", option);
		ret = -EINVAL;
		goto cleanup;
	}
	*colon = '\0';

	token = strtok(option_dup, ",");
	while (token) {
		if (token == NULL)
			break;
		if (strcmp(token, "nodatacow") == 0) {
			entry->nodatacow = true;
		} else if (strcmp(token, "nodatasum") == 0) {
			entry->nodatasum = true;
		} else {
			error("unknown flag: %s", token);
			ret = -EINVAL;
			goto cleanup;
		}
		token = strtok(NULL, ",");
	}

	if (arg_copy_path(entry->inode_path, colon + 1, sizeof(entry->inode_path))) {
		error("--inode-flags path too long");
		ret = -E2BIG;
		goto cleanup;
	}
	list_add_tail(&entry->list, inode_flags_list);
	free(option_dup);
	return 0;
cleanup:
	free(option_dup);
	free(entry);
	return ret;
}

static int queue_discard_logical(struct btrfs_fs_info *fs_info, u64 start, u64 len)
{
	struct btrfs_multi_bio *multi = NULL;
	int ret;
	u64 cur_offset = 0;
	u64 cur_len = 0;

	while (cur_offset < len) {
		struct btrfs_device *device;

		cur_len = len - cur_offset;
		ret = btrfs_map_block(fs_info, WRITE, start + cur_offset, &cur_len, &multi, 0, NULL);
		if (ret)
			return ret;

		if (multi->type & BTRFS_BLOCK_GROUP_RAID56_MASK) {
			free(multi);
			break;
		}

		cur_len = min(cur_len, len - cur_offset);

		for (int i = 0; i < multi->num_stripes; i++) {
			device = multi->stripes[i].dev;

			ret = add_merge_cache_extent(&device->discard,
					multi->stripes[i].physical, cur_len);
			if (ret < 0) {
				free(multi);
				return ret;
			}
		}
		free(multi);
		multi = NULL;
		cur_offset += cur_len;
	}
	return 0;
}

static int discard_all_devices(struct btrfs_fs_info *fs_info)
{
	struct btrfs_device *dev;

	list_for_each_entry(dev, &fs_info->fs_devices->devices, dev_list) {
		if (!dev->writeable)
			continue;
		for (struct cache_extent *cache = first_cache_extent(&dev->discard);
		     cache; cache = next_cache_extent(cache)) {
			int ret;

			ret = device_discard_blocks(dev->fd, cache->start, cache->size);
			if (ret == EOPNOTSUPP)
				return 0;
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

static int discard_free_space(struct btrfs_fs_info *fs_info, u64 metadata_profile)
{
	struct btrfs_root *free_space_root;
	struct btrfs_path path = { 0 };
	struct extent_buffer *leaf;
	int ret;
	struct btrfs_key key = {
		.objectid = BTRFS_FREE_SPACE_TREE_OBJECTID,
		.type = BTRFS_ROOT_ITEM_KEY,
		.offset = 0,
	};

	if (!btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE))
		return 0;

	/*
	 * Follow the kernel in not doing discard for RAID5 or 6.  We don't
	 * have to worry about data here, as --rootdir only works with
	 * single-device filesystems, and the data block groups are empty
	 * otherwise.
	 */

	if (metadata_profile & BTRFS_BLOCK_GROUP_RAID56_MASK)
		return 0;

	free_space_root = btrfs_global_root(fs_info, &key);

	key.objectid = 0;
	key.type = 0;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, free_space_root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

	while (true) {
		leaf = path.nodes[0];

		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(free_space_root, &path);
			if (ret)
				break;

			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);

		if (key.type == BTRFS_FREE_SPACE_EXTENT_KEY) {
			ret = queue_discard_logical(fs_info, key.objectid, key.offset);
			if (ret < 0)
				goto out;
		} else if (key.type == BTRFS_FREE_SPACE_BITMAP_KEY) {
			int size, nrbits;
			void *bitmap;
			unsigned long start_bit, end_bit;

			size = btrfs_item_size(leaf, path.slots[0]);
			bitmap = malloc(size);
			if (!bitmap) {
				ret = -ENOMEM;
				goto out;
			}

			read_extent_buffer(leaf, bitmap,
					   btrfs_item_ptr_offset(leaf, path.slots[0]),
					   size);

			nrbits = div_u64(key.offset, fs_info->sectorsize);
			start_bit = find_next_bit_le(bitmap, nrbits, 0);

			while (start_bit < nrbits) {
				u64 addr, length;

				end_bit = find_next_zero_bit_le(bitmap, nrbits, start_bit);
				addr = key.objectid + (start_bit * fs_info->sectorsize);
				length = (end_bit - start_bit) * fs_info->sectorsize;

				ret = queue_discard_logical(fs_info, addr, length);
				if (ret < 0) {
					free(bitmap);
					goto out;
				}

				start_bit = find_next_bit_le(bitmap, nrbits, end_bit);
			}

			free(bitmap);
		}

		path.slots[0]++;
	}
	btrfs_release_path(&path);

	/* Every discard range is properly queued. Now submit the real discard request. */
	return discard_all_devices(fs_info);

out:
	btrfs_release_path(&path);
	return ret;
}

int BOX_MAIN(mkfs)(int argc, char **argv)
{
	char *file;
	struct btrfs_root *root;
	struct btrfs_fs_info *fs_info;
	struct btrfs_trans_handle *trans;
	struct open_ctree_args oca = { 0 };
	int ret = 0;
	int close_ret;
	int i;
	bool ssd = false;
	bool shrink_rootdir = false, do_reflink = false;
	u64 source_dir_size = 0;
	u64 min_dev_size;
	u64 shrink_size;
	int device_count = 0;
	int saved_optind;
	pthread_t *t_prepare = NULL;
	struct prepare_device_progress *prepare_ctx = NULL;
	struct mkfs_allocation allocation = { 0 };
	struct btrfs_mkfs_config mkfs_cfg;
	/* Options */
	bool force_overwrite = false;
	struct btrfs_mkfs_features features = btrfs_mkfs_default_features;
	enum btrfs_csum_type csum_type = BTRFS_CSUM_TYPE_CRC32;
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE] = { 0 };
	char dev_uuid[BTRFS_UUID_UNPARSED_SIZE] = { 0 };
	u32 nodesize = 0;
	bool nodesize_forced = false;
	u32 sectorsize = 0;
	u32 stripesize = 4096;
	u64 metadata_profile = 0;
	bool metadata_profile_set = false;
	u64 data_profile = 0;
	bool data_profile_set = false;
	u64 byte_count = 0;
	u64 dev_byte_count = 0;
	bool mixed = false;
	char *label = NULL;
	int nr_global_roots = sysconf(_SC_NPROCESSORS_ONLN);
	char *source_dir = NULL;
	struct rootdir_subvol *rds;
	struct rootdir_inode_flags_entry *rif;
	bool has_default_subvol = false;
	enum btrfs_compression_type compression = BTRFS_COMPRESS_NONE;
	unsigned int compression_level = 0;
	LIST_HEAD(subvols);
	LIST_HEAD(inode_flags_list);

	cpu_detect_flags();
	hash_init_accel();
	btrfs_config_init();
	btrfs_assert_feature_buf_size();

	while(1) {
		int c;
		enum {
			GETOPT_VAL_SHRINK = GETOPT_VAL_FIRST,
			GETOPT_VAL_CHECKSUM,
			GETOPT_VAL_GLOBAL_ROOTS,
			GETOPT_VAL_DEVICE_UUID,
			GETOPT_VAL_INODE_FLAGS,
			GETOPT_VAL_COMPRESS,
			GETOPT_VAL_REFLINK,
		};
		static const struct option long_options[] = {
			{ "byte-count", required_argument, NULL, 'b' },
			{ "csum", required_argument, NULL,
				GETOPT_VAL_CHECKSUM },
			{ "checksum", required_argument, NULL,
				GETOPT_VAL_CHECKSUM },
			{ "force", no_argument, NULL, 'f' },
			{ "label", required_argument, NULL, 'L'},
			{ "metadata", required_argument, NULL, 'm' },
			{ "mixed", no_argument, NULL, 'M' },
			{ "nodesize", required_argument, NULL, 'n' },
			{ "sectorsize", required_argument, NULL, 's' },
			{ "data", required_argument, NULL, 'd' },
			{ "version", no_argument, NULL, 'V' },
			{ "rootdir", required_argument, NULL, 'r' },
			{ "subvol", required_argument, NULL, 'u' },
			{ "inode-flags", required_argument, NULL, GETOPT_VAL_INODE_FLAGS },
			{ "nodiscard", no_argument, NULL, 'K' },
			{ "features", required_argument, NULL, 'O' },
			{ "runtime-features", required_argument, NULL, 'R' },
			{ "uuid", required_argument, NULL, 'U' },
			{ "device-uuid", required_argument, NULL,
				GETOPT_VAL_DEVICE_UUID },
			{ "quiet", 0, NULL, 'q' },
			{ "verbose", 0, NULL, 'v' },
			{ "shrink", no_argument, NULL, GETOPT_VAL_SHRINK },
			{ "compress", required_argument, NULL,
				GETOPT_VAL_COMPRESS },
			{ "reflink", no_argument, NULL, GETOPT_VAL_REFLINK },
#if EXPERIMENTAL
			{ "param", required_argument, NULL, GETOPT_VAL_PARAM },
			{ "num-global-roots", required_argument, NULL, GETOPT_VAL_GLOBAL_ROOTS },
#endif
			{ "help", no_argument, NULL, GETOPT_VAL_HELP },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "A:b:fn:s:m:d:L:R:O:r:U:VvMKqu:",
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
			case 'n':
				nodesize = arg_strtou64_with_suffix(optarg);
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
					ret = 1;
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

				warning("runtime features are deprecated, use -O|--features instead");
				tmp = btrfs_parse_runtime_features(tmp,
						&features);
				if (tmp) {
					error("unrecognized runtime feature '%s'",
					      tmp);
					free(orig);
					ret = 1;
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
				sectorsize = arg_strtou64_with_suffix(optarg);
				break;
			case 'b':
				byte_count = arg_strtou64_with_suffix(optarg);
				opt_zero_end = false;
				break;
			case 'v':
				bconf_be_verbose();
				break;
			case 'V':
				help_builtin_features("mkfs.btrfs, part of ");
				goto success;
			case 'r':
				free(source_dir);
				source_dir = strdup(optarg);
				break;
			case 'u':
				ret = parse_subvolume(optarg, &subvols, &has_default_subvol);
				if (ret)
					exit(1);
				break;
			case 'U':
				strncpy_null(fs_uuid, optarg, BTRFS_UUID_UNPARSED_SIZE);
				break;
			case 'K':
				opt_discard = false;
				break;
			case 'q':
				bconf_be_quiet();
				break;
			case GETOPT_VAL_INODE_FLAGS:
				ret = parse_inode_flags(optarg, &inode_flags_list);
				if (ret)
					goto error;
				break;
			case GETOPT_VAL_COMPRESS:
				if (parse_compression(optarg, &compression, &compression_level)) {
					ret = 1;
					goto error;
				}
				break;
			case GETOPT_VAL_DEVICE_UUID:
				strncpy_null(dev_uuid, optarg, BTRFS_UUID_UNPARSED_SIZE);
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
			case GETOPT_VAL_PARAM:
				bconf_save_param(optarg);
				break;
			case GETOPT_VAL_REFLINK:
				do_reflink = true;
				break;
			case GETOPT_VAL_HELP:
			default:
				usage(&mkfs_cmd, c != GETOPT_VAL_HELP);
		}
	}

	pr_verbose(LOG_DEFAULT, "%s\n", PACKAGE_STRING);
	pr_verbose(LOG_DEFAULT, "See %s for more information.\n\n", PACKAGE_URL);

	if (!sectorsize)
		sectorsize = (u32)SZ_4K;
	if (btrfs_check_sectorsize(sectorsize)) {
		ret = 1;
		goto error;
	}

	if (!nodesize)
		nodesize = max_t(u32, sectorsize, BTRFS_MKFS_DEFAULT_NODE_SIZE);

	stripesize = sectorsize;
	saved_optind = optind;
	device_count = argc - optind;
	if (device_count == 0)
		usage(&mkfs_cmd, 1);

	opt_zoned = !!(features.incompat_flags & BTRFS_FEATURE_INCOMPAT_ZONED);

	if (source_dir && device_count > 1) {
		error("the option -r is limited to a single device");
		ret = 1;
		goto error;
	}
	if (shrink_rootdir && source_dir == NULL) {
		error("the option --shrink must be used with --rootdir");
		ret = 1;
		goto error;
	}
	if (do_reflink && source_dir == NULL) {
		error("the option --reflink must be used with --rootdir");
		ret = 1;
		goto error;
	}
	if (!list_empty(&subvols) && source_dir == NULL) {
		error("option --subvol must be used with --rootdir");
		ret = 1;
		goto error;
	}
	if (!list_empty(&inode_flags_list) && source_dir == NULL) {
		error("option --inode-flags must be used with --rootdir");
		ret = 1;
		goto error;
	}

	if (source_dir) {
		char *canonical = realpath(source_dir, NULL);

		if (!canonical) {
			error("could not get canonical path to %s", source_dir);
			ret = 1;
			goto error;
		}

		free(source_dir);
		source_dir = canonical;
	} else {
		if (compression != BTRFS_COMPRESS_NONE) {
			error("--compression must be used with --rootdir");
			ret = 1;
			goto error;
		}
	}

	ret = btrfs_mkfs_validate_subvols(source_dir, &subvols);
	if (ret < 0)
		goto error;

	ret = btrfs_mkfs_validate_inode_flags(source_dir, &inode_flags_list);
	if (ret < 0)
		goto error;

	if (*fs_uuid) {
		uuid_t dummy_uuid;

		if (uuid_parse(fs_uuid, dummy_uuid) != 0) {
			error("could not parse UUID: %s", fs_uuid);
			ret = 1;
			goto error;
		}
		/* We allow non-unique fsid for single device btrfs filesystem. */
		if (device_count != 1 && !test_uuid_unique(fs_uuid)) {
			error("non-unique UUID: %s", fs_uuid);
			ret = 1;
			goto error;
		}
	}

	if (*dev_uuid) {
		uuid_t dummy_uuid;

		if (uuid_parse(dev_uuid, dummy_uuid) != 0) {
			error("could not parse device UUID: %s", dev_uuid);
			ret = 1;
			goto error;
		}
		/* We allow non-unique device uuid for single device filesystem. */
		if (device_count != 1 && !test_uuid_unique(dev_uuid)) {
			error("the option --device-uuid %s can be used only for a single device filesystem",
			      dev_uuid);
			ret = 1;
			goto error;
		}
	}

	for (i = 0; i < device_count; i++) {
		file = argv[optind++];

		if (source_dir && path_exists(file) == 0)
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
		pr_verbose(LOG_DEFAULT, "zoned: %s: host-managed device detected, setting zoned feature\n",
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
				ret = 1;
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

	/* Remap tree feature requires block-group-tree, no-holes, and free-space-tree. */
	if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_REMAP_TREE) {
		features.incompat_flags |= BTRFS_FEATURE_INCOMPAT_NO_HOLES;
		features.compat_ro_flags |=
			BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |
			BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID |
			BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE;
	}

	if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_REMAP_TREE &&
	    features.incompat_flags & BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS) {
		error("remap-tree not supported with mixed-bg");
		exit(1);
	}

	if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_REMAP_TREE &&
	    features.incompat_flags & BTRFS_FEATURE_INCOMPAT_ZONED) {
		error("remap-tree not supported for zoned devices");
		exit(1);
	}

	/*
	 * Block group tree feature requires no-holes and free-space-tree.
	 * And if those dependency is disabled, also disable block-group-tree feature.
	 */
	if (features.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE &&
	    (!(features.incompat_flags & BTRFS_FEATURE_INCOMPAT_NO_HOLES) ||
	     !(features.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE))) {
		warning("disabling block-group-tree feature due to missing no-holes and free-space-tree features");
		features.compat_ro_flags &= ~BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE;
	}

	if (opt_zoned) {
		const int blkid_version =  blkid_get_library_version(NULL, NULL);

		if (source_dir) {
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

		if (blkid_version < 2380)
			warning("libblkid < 2.38 does not support zoned mode's superblock location, update recommended");
	}

	if (btrfs_check_nodesize(nodesize, sectorsize, &features)) {
		ret = 1;
		goto error;
	}

	/* This is also fixed in kernel, but the flag has no real meaning anymore. */
	if (nodesize > sysconf(_SC_PAGE_SIZE))
		features.incompat_flags |= BTRFS_FEATURE_INCOMPAT_BIG_METADATA;

	min_dev_size = btrfs_min_dev_size(nodesize, mixed,
					  opt_zoned ? zone_size(file) : 0,
					  metadata_profile, data_profile);
	if (byte_count) {
		byte_count = round_down(byte_count, sectorsize);
		if (opt_zoned)
			byte_count = round_down(byte_count,  zone_size(file));
	}

	/*
	 * Enlarge the destination file or create a new one, using the size
	 * calculated from source dir.
	 *
	 * This must be done before minimal device size checks.
	 */
	if (source_dir) {
		int oflags = O_RDWR;
		struct stat statbuf;
		int fd;

		if (path_exists(file) == 0)
			oflags |= O_CREAT;

		fd = open(file, oflags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
					 S_IROTH);
		if (fd < 0) {
			error("unable to open %s: %m", file);
			ret = 1;
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
		if (!byte_count) {
			ret = device_get_partition_size_fd_stat(fd, &statbuf, &byte_count);
			if (ret < 0) {
				errno = -ret;
				error("failed to get device size for %s: %m", file);
				goto error;
			}
			byte_count = round_down(byte_count, sectorsize);
		}
		source_dir_size = btrfs_mkfs_size_dir(source_dir, sectorsize,
				min_dev_size, metadata_profile, data_profile);
		UASSERT(IS_ALIGNED(source_dir_size, sectorsize));
		if (byte_count < source_dir_size) {
			if (S_ISREG(statbuf.st_mode)) {
				byte_count = source_dir_size;
			} else {
				warning(
"the target device %llu (%s) is smaller than the calculated source directory size %llu (%s), mkfs may fail",
					byte_count, pretty_size(byte_count),
					source_dir_size, pretty_size(source_dir_size));
			}
		}
		ret = zero_output_file(fd, byte_count);
		if (ret) {
			error("unable to zero the output file");
			close(fd);
			goto error;
		}
		/* our "device" is the new image file */
		dev_byte_count = byte_count;
		close(fd);
	}
	/* Check device/byte_count after the nodesize is determined */
	if (byte_count && byte_count < min_dev_size) {
		error("size %llu is too small to make a usable filesystem", byte_count);
		error("minimum size for a %sbtrfs filesystem is %llu",
		      opt_zoned ? "zoned mode " : "", min_dev_size);
		ret = 1;
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

	if (opt_zoned && device_count) {
		switch (data_profile & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
		case BTRFS_BLOCK_GROUP_DUP:
		case BTRFS_BLOCK_GROUP_RAID1:
		case BTRFS_BLOCK_GROUP_RAID1C3:
		case BTRFS_BLOCK_GROUP_RAID1C4:
		case BTRFS_BLOCK_GROUP_RAID0:
		case BTRFS_BLOCK_GROUP_RAID10:
#if EXPERIMENTAL
			features.incompat_flags |= BTRFS_FEATURE_INCOMPAT_RAID_STRIPE_TREE;
#endif
			break;
		default:
			break;
		}
	}

	if (opt_zoned) {
		u64 metadata = BTRFS_BLOCK_GROUP_METADATA | metadata_profile;
		u64 data = BTRFS_BLOCK_GROUP_DATA | data_profile;
		bool rst = false;

		if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_RAID_STRIPE_TREE)
			rst = true;

		if (!zoned_profile_supported(metadata, rst) ||
		    !zoned_profile_supported(data, rst)) {
			error("zoned mode does not yet support the selected RAID profiles");
			ret = 1;
			goto error;
		}
	}

	t_prepare = calloc(device_count, sizeof(*t_prepare));
	prepare_ctx = calloc(device_count, sizeof(*prepare_ctx));

	if (!t_prepare || !prepare_ctx) {
		error_mem("thread for preparing devices");
		ret = 1;
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
		prepare_ctx[i].byte_count = byte_count;
		prepare_ctx[i].dev_byte_count = byte_count;
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

	dev_byte_count = prepare_ctx[0].dev_byte_count;
	if (byte_count && byte_count > dev_byte_count) {
		error("%s is smaller than requested size, expected %llu, found %llu",
		      file, byte_count, dev_byte_count);
		ret = 1;
		goto error;
	}

	if (btrfs_bg_type_to_tolerated_failures(metadata_profile) <
	    btrfs_bg_type_to_tolerated_failures(data_profile))
		warning("metadata has lower redundancy than data!\n");

	mkfs_cfg.label = label;
	memcpy(mkfs_cfg.fs_uuid, fs_uuid, sizeof(mkfs_cfg.fs_uuid));
	memcpy(mkfs_cfg.dev_uuid, dev_uuid, sizeof(mkfs_cfg.dev_uuid));
	mkfs_cfg.num_bytes = dev_byte_count;
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

	ret = make_btrfs(prepare_ctx[0].fd, &mkfs_cfg);
	if (ret) {
		errno = -ret;
		error("error during mkfs: %m");
		goto error;
	}

	oca.filename = file;
	oca.flags = OPEN_CTREE_WRITES | OPEN_CTREE_TEMPORARY_SUPER |
		    OPEN_CTREE_EXCLUSIVE;
	fs_info = open_ctree_fs_info(&oca);
	if (!fs_info) {
		error("open ctree failed");
		ret = 1;
		goto error;
	}

	root = fs_info->fs_root;

	ret = create_metadata_block_groups(root, features.incompat_flags,
					   &allocation, metadata_profile);
	if (ret) {
		errno = -ret;
		error("failed to create default block groups: %m");
		goto error;
	}

	if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_RAID_STRIPE_TREE) {
		ret = setup_raid_stripe_tree_root(fs_info);
		if (ret < 0) {
			errno = -ret;
			error("failed to initialize raid-stripe-tree: %m");
			goto out;
		}
	}

	if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_REMAP_TREE) {
		ret = setup_remap_tree_root(fs_info);
		if (ret < 0) {
			errno = -ret;
			error("failed to initialize remap-tree: %m");
			goto out;
		}
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		errno = -PTR_ERR(trans);
		error_msg(ERROR_MSG_START_TRANS, "%m");
		ret = 1;
		goto error;
	}

	ret = create_data_block_groups(trans, root, mixed, &allocation);
	if (ret) {
		errno = -ret;
		error("failed to create default data block groups: %m");
		goto error;
	}

	if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_ZONED) {
		ret = create_data_block_groups(trans, root, mixed, &allocation);
		if (ret) {
			errno = -ret;
			error("failed to create data relocation block groups: %m");
			goto error;
		}
	}

	if (features.incompat_flags & BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2) {
		ret = create_global_roots(trans, nr_global_roots);
		if (ret) {
			errno = -ret;
			error("failed to create global roots: %m");
			goto error;
		}
	}

	ret = make_root_dir(trans, root);
	if (ret) {
		errno = -ret;
		error("failed to setup the root directory: %m");
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
		ret = 1;
		goto error;
	}

	if (device_count == 0)
		goto raid_groups;

	for (i = 1; i < device_count; i++) {
		ret = btrfs_device_already_in_root(root, prepare_ctx[i].fd,
						   BTRFS_SUPER_INFO_OFFSET);
		if (ret) {
			error("skipping duplicate device %s in the filesystem",
				file);
			continue;
		}
		dev_byte_count = prepare_ctx[i].dev_byte_count;

		if (prepare_ctx[i].ret) {
			errno = -prepare_ctx[i].ret;
			error("unable to prepare device %s: %m", prepare_ctx[i].file);
			ret = 1;
			goto error;
		}

		ret = btrfs_add_to_fsid(trans, root, prepare_ctx[i].fd,
					prepare_ctx[i].file, dev_byte_count,
					sectorsize, sectorsize, sectorsize);
		if (ret) {
			errno = -ret;
			error("unable to add %s to filesystem: %m",
			      prepare_ctx[i].file);
			goto error;
		}
		if (bconf.verbose >= LOG_INFO) {
			struct btrfs_device *device;

			device = container_of(fs_info->fs_devices->devices.next,
					struct btrfs_device, dev_list);
			printf("adding device %s id %llu\n", file, device->devid);
		}
	}

	if (opt_zoned)
		btrfs_get_dev_zone_info_all_devices(fs_info);

raid_groups:
	ret = create_raid_groups(trans, root, data_profile,
			 metadata_profile, mixed, &allocation);
	if (ret) {
		errno = -ret;
		error("unable to create raid groups: %m");
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
		ret = 1;
		goto error;
	}
	/* COW all tree blocks to newly created chunks */
	ret = recow_roots(trans, root);
	if (ret) {
		errno = -ret;
		error("unable to COW tree blocks to new profiles: %m");
		goto out;
	}

	if (!(features.incompat_flags & BTRFS_FEATURE_INCOMPAT_REMAP_TREE)) {
		ret = btrfs_make_subvolume(trans, BTRFS_DATA_RELOC_TREE_OBJECTID,
					false);
		if (ret) {
			errno = -ret;
			error("unable to create data reloc tree: %m");
			goto out;
		}
	}

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		goto out;
	}

	if (source_dir) {
		pr_verbose(LOG_DEFAULT, "Rootdir from:       %s\n", source_dir);

		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			errno = -PTR_ERR(trans);
			error_msg(ERROR_MSG_START_TRANS, "%m");
			goto out;
		}

		pr_verbose(LOG_DEFAULT, "  Compress:         %s%s%s\n",
			   compression == BTRFS_COMPRESS_ZSTD ? "zstd" :
			   compression == BTRFS_COMPRESS_LZO ? "lzo" :
			   compression == BTRFS_COMPRESS_ZLIB ? "zlib" : "no",
			   compression_level > 0 ? ":" : "",
			   compression_level > 0 ?
				   pretty_size_mode(compression_level, UNITS_RAW) :
				   "");

		/* Print subvolumes now as btrfs_mkfs_fill_dir() deletes the list. */
		list_for_each_entry(rds, &subvols, list) {
			pr_verbose(LOG_DEFAULT, "  Subvolume (%s%s):  %s%s\n",
				   rds->is_default ? "d" : "",
				   rds->readonly ? "ro" : "rw",
				   rds->is_default ? "" : " ",
				   rds->dir);
		}
		list_for_each_entry(rif, &inode_flags_list, list) {
			pr_verbose(LOG_DEFAULT, "  Inode flags (%s):  %s\n",
				   rif->nodatacow ? "NODATACOW" : "",
				   rif->inode_path);
		}

		ret = btrfs_mkfs_fill_dir(trans, source_dir, root,
					  &subvols, &inode_flags_list,
					  compression, compression_level,
					  do_reflink);
		if (ret) {
			errno = -ret;
			error("error while filling filesystem: %m");
			btrfs_abort_transaction(trans, ret);
			goto out;
		}

		ret = btrfs_commit_transaction(trans, root);
		if (ret) {
			errno = -ret;
			error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
			goto out;
		}

		if (shrink_rootdir) {
			pr_verbose(LOG_DEFAULT, "  Shrink:           yes\n");
			ret = btrfs_mkfs_shrink_fs(fs_info, &shrink_size,
						   shrink_rootdir);
			if (ret < 0) {
				errno = -ret;
				error("error while shrinking filesystem: %m");
				goto out;
			}
		} else {
			pr_verbose(LOG_DEFAULT, "  Shrink:           no\n");
		}
	}

	ret = btrfs_rebuild_uuid_tree(fs_info);
	if (ret < 0)
		goto out;

	ret = cleanup_temp_chunks(fs_info, &allocation, data_profile,
				  metadata_profile, metadata_profile);
	if (ret < 0) {
		errno = -ret;
		error("failed to cleanup temporary chunks: %m");
		goto out;
	}

	if (features.runtime_flags & BTRFS_FEATURE_RUNTIME_QUOTA ||
	    features.incompat_flags & BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA) {
		ret = setup_quota_root(fs_info);
		if (ret < 0) {
			errno = -ret;
			error("failed to initialize quota: %m");
			goto out;
		}
	}
	if (bconf.verbose) {
		char features_buf[BTRFS_FEATURE_STRING_BUF_SIZE];

		update_chunk_allocation(fs_info, &allocation);
		printf("Label:              %s\n", label);
		printf("UUID:               %s\n", mkfs_cfg.fs_uuid);
		if (dev_uuid[0] != 0)
			printf("Device UUID:        %s\n", mkfs_cfg.dev_uuid);
		printf("Node size:          %u\n", nodesize);
		printf("Sector size:        %u\t(CPU page size: %lu)\n",
		       sectorsize, sysconf(_SC_PAGESIZE));
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
		if (allocation.remap)
			printf("  Remap:            %-8s %16s\n",
				btrfs_group_profile_str(metadata_profile),
				pretty_size(allocation.remap));
		printf("  System:           %-8s %16s\n",
			btrfs_group_profile_str(metadata_profile),
			pretty_size(allocation.system));
		printf("SSD detected:       %s\n", ssd ? "yes" : "no");
		printf("Zoned device:       %s\n", opt_zoned ? "yes" : "no");
		if (opt_zoned) {
			printf("  Zone size:        %s\n",
			       pretty_size(fs_info->zone_size));
			if (zoned_model(file) == ZONED_NONE)
				printf("  Mode:             emulated\n");
			else
				printf("  Mode:             host managed\n");
		}
		btrfs_parse_fs_features_to_string(features_buf, &features);
		printf("Features:           %s\n", features_buf);
		printf("Checksum:           %s\n",
		       btrfs_super_csum_name(mkfs_cfg.csum_type));

		list_all_devices(root, opt_zoned);

		if (mkfs_cfg.csum_type == BTRFS_CSUM_TYPE_SHA256) {
			u32 kernel_version = get_running_kernel_version();

			if (kernel_version < KERNEL_VERSION(6,16,0)) {
				printf(
"NOTE: in kernels < v6.16 you may need to manually load kernel module implementing accelerated SHA256 in case\n"
"      the generic implementation is built-in, before mount. Check lsmod or /proc/crypto\n\n"
				);
			}
		}
	}

	if (opt_discard) {
		ret = discard_free_space(fs_info, metadata_profile);
		if (ret < 0) {
			errno = -ret;
			error("failed to discard free space: %m");
			goto out;
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
		errno = -ret;
		error("failed to close ctree, filesystem may be inconsistent: %m");
	}

	btrfs_close_all_devices();

error:
	if (prepare_ctx) {
		for (i = 0; i < device_count; i++)
			close(prepare_ctx[i].fd);
	}
	free(t_prepare);
	free(prepare_ctx);
	free(label);
	free(source_dir);

	while (!list_empty(&subvols)) {
		struct rootdir_subvol *head;

		head = list_entry(subvols.next, struct rootdir_subvol, list);
		list_del(&head->list);
		free(head);
	}
	while (!list_empty(&inode_flags_list)) {
		rif = list_entry(inode_flags_list.next,
				 struct rootdir_inode_flags_entry, list);
		list_del(&rif->list);
		free(rif);
	}

	return !!ret;

success:
	exit(0);
}
