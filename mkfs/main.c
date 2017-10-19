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
#include "androidcompat.h"

#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#include <stdio.h>
#include <stdlib.h>
/* #include <sys/dir.h> included via androidcompat.h */
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <ctype.h>
#include <blkid/blkid.h>
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "transaction.h"
#include "utils.h"
#include "list_sort.h"
#include "help.h"
#include "mkfs/common.h"
#include "mkfs/rootdir.h"
#include "fsfeatures.h"

static int verbose = 1;

struct mkfs_allocation {
	u64 data;
	u64 metadata;
	u64 mixed;
	u64 system;
};

static int create_metadata_block_groups(struct btrfs_root *root, int mixed,
				struct mkfs_allocation *allocation)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *trans;
	u64 bytes_used;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	bytes_used = btrfs_super_bytes_used(fs_info->super_copy);

	root->fs_info->system_allocs = 1;
	ret = btrfs_make_block_group(trans, fs_info, bytes_used,
				     BTRFS_BLOCK_GROUP_SYSTEM,
				     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     0, BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	allocation->system += BTRFS_MKFS_SYSTEM_GROUP_SIZE;
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
					     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
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
					     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
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
		struct btrfs_root *root, int mixed,
		struct mkfs_allocation *allocation)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 chunk_start = 0;
	u64 chunk_size = 0;
	int ret = 0;

	if (!mixed) {
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
					     BTRFS_FIRST_CHUNK_TREE_OBJECTID,
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

static int __recow_root(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root)
{
	struct extent_buffer *tmp;
	int ret;

	if (trans->transid != btrfs_root_generation(&root->root_item)) {
		extent_buffer_get(root->node);
		ret = __btrfs_cow_block(trans, root, root->node,
					NULL, 0, &tmp, 0, 0);
		if (ret)
			return ret;
		free_extent_buffer(tmp);
	}

	return 0;
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
	ret = __recow_root(trans, info->extent_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->chunk_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->dev_root);
	if (ret)
		return ret;
	ret = __recow_root(trans, info->csum_root);
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
				     type, BTRFS_FIRST_CHUNK_TREE_OBJECTID,
				     chunk_start, chunk_size);

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
		error("unrecognized profile type: 0x%llx",
				(unsigned long long)type);
		ret = -EINVAL;
	}

	return ret;
}

static int create_raid_groups(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 data_profile,
			      u64 metadata_profile, int mixed,
			      struct mkfs_allocation *allocation)
{
	int ret;

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
	ret = recow_roots(trans, root);

	return ret;
}

static int create_tree(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, u64 objectid)
{
	struct btrfs_key location;
	struct btrfs_root_item root_item;
	struct extent_buffer *tmp;
	int ret;

	ret = btrfs_copy_root(trans, root, root->node, &tmp, objectid);
	if (ret)
		return ret;

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

	return ret;
}

static void print_usage(int ret)
{
	printf("Usage: mkfs.btrfs [options] dev [ dev ... ]\n");
	printf("Options:\n");
	printf("  allocation profiles:\n");
	printf("\t-d|--data PROFILE       data profile, raid0, raid1, raid5, raid6, raid10, dup or single\n");
	printf("\t-m|--metadata PROFILE   metadata profile, values like for data profile\n");
	printf("\t-M|--mixed              mix metadata and data together\n");
	printf("  features:\n");
	printf("\t-n|--nodesize SIZE      size of btree nodes\n");
	printf("\t-s|--sectorsize SIZE    data block size (may not be mountable by current kernel)\n");
	printf("\t-O|--features LIST      comma separated list of filesystem features (use '-O list-all' to list features)\n");
	printf("\t-L|--label LABEL        set the filesystem label\n");
	printf("\t-U|--uuid UUID          specify the filesystem UUID (must be unique)\n");
	printf("  creation:\n");
	printf("\t-b|--byte-count SIZE    set filesystem size to SIZE (on the first device)\n");
	printf("\t-S|--subvol NAME        create a subvolume with NAME and copy files from ROOTDIR to the subvolume\n");
	printf("\t-r|--rootdir DIR        copy files from DIR to the image root directory\n");
	printf("\t-K|--nodiscard          do not perform whole device TRIM\n");
	printf("\t-f|--force              force overwrite of existing filesystem\n");
	printf("  general:\n");
	printf("\t-q|--quiet              no messages except errors\n");
	printf("\t-V|--version            print the mkfs.btrfs version and exit\n");
	printf("\t--help                  print this help and exit\n");
	printf("  deprecated:\n");
	printf("\t-A|--alloc-start START  the offset to start the filesystem\n");
	printf("\t-l|--leafsize SIZE      deprecated, alias for nodesize\n");
	exit(ret);
}

static u64 parse_profile(const char *s)
{
	if (strcasecmp(s, "raid0") == 0) {
		return BTRFS_BLOCK_GROUP_RAID0;
	} else if (strcasecmp(s, "raid1") == 0) {
		return BTRFS_BLOCK_GROUP_RAID1;
	} else if (strcasecmp(s, "raid5") == 0) {
		return BTRFS_BLOCK_GROUP_RAID5;
	} else if (strcasecmp(s, "raid6") == 0) {
		return BTRFS_BLOCK_GROUP_RAID6;
	} else if (strcasecmp(s, "raid10") == 0) {
		return BTRFS_BLOCK_GROUP_RAID10;
	} else if (strcasecmp(s, "dup") == 0) {
		return BTRFS_BLOCK_GROUP_DUP;
	} else if (strcasecmp(s, "single") == 0) {
		return 0;
	} else {
		error("unknown profile %s", s);
		exit(1);
	}
	/* not reached */
	return 0;
}

static char *parse_label(const char *input)
{
	int len = strlen(input);

	if (len >= BTRFS_LABEL_SIZE) {
		error("label %s is too long (max %d)", input,
			BTRFS_LABEL_SIZE - 1);
		exit(1);
	}
	return strdup(input);
}

static char *parse_subvol_name(const char *input)
{
	int len = strlen(input);

	if (len >= BTRFS_SUBVOL_NAME_MAX) {
		error("subvolume name %s is too long (max %d)",
			input, BTRFS_SUBVOL_NAME_MAX - 1);
		exit(1);
	}
	return strdup(input);
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

static int is_ssd(const char *file)
{
	blkid_probe probe;
	char wholedisk[PATH_MAX];
	char sysfs_path[PATH_MAX];
	dev_t devno;
	int fd;
	char rotational;
	int ret;

	probe = blkid_new_probe_from_filename(file);
	if (!probe)
		return 0;

	/* Device number of this disk (possibly a partition) */
	devno = blkid_probe_get_devno(probe);
	if (!devno) {
		blkid_free_probe(probe);
		return 0;
	}

	/* Get whole disk name (not full path) for this devno */
	ret = blkid_devno_to_wholedisk(devno,
			wholedisk, sizeof(wholedisk), NULL);
	if (ret) {
		blkid_free_probe(probe);
		return 0;
	}

	snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/queue/rotational",
		 wholedisk);

	blkid_free_probe(probe);

	fd = open(sysfs_path, O_RDONLY);
	if (fd < 0) {
		return 0;
	}

	if (read(fd, &rotational, 1) < 1) {
		close(fd);
		return 0;
	}
	close(fd);

	return rotational == '0';
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
	u64 flag = btrfs_disk_block_group_flags(node, bgi);
	u64 flag_type = flag & BTRFS_BLOCK_GROUP_TYPE_MASK;
	u64 flag_profile = flag & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	u64 used = btrfs_disk_block_group_used(node, bgi);

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
	struct btrfs_root *root = fs_info->extent_root;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_path path;
	int ret = 0;

	btrfs_init_path(&path);
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));

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
			u64 flags = btrfs_disk_block_group_flags(path.nodes[0],
							     bgi);

			ret = btrfs_free_block_group(trans, fs_info,
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
	struct btrfs_block_group_cache *bg_cache;
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
			allocation->mixed += bg_cache->key.offset;
		else if (bg_cache->flags & BTRFS_BLOCK_GROUP_DATA)
			allocation->data += bg_cache->key.offset;
		else if (bg_cache->flags & BTRFS_BLOCK_GROUP_METADATA)
			allocation->metadata += bg_cache->key.offset;
		else
			allocation->system += bg_cache->key.offset;
		search_start = bg_cache->key.objectid + bg_cache->key.offset;
	}
}

int main(int argc, char **argv)
{
	char *file;
	struct btrfs_root *root;
	struct btrfs_fs_info *fs_info;
	struct btrfs_trans_handle *trans;
	char *label = NULL;
	u64 block_count = 0;
	u64 dev_block_count = 0;
	u64 alloc_start = 0;
	u64 metadata_profile = 0;
	u64 data_profile = 0;
	u32 nodesize = max_t(u32, sysconf(_SC_PAGESIZE),
			BTRFS_MKFS_DEFAULT_NODE_SIZE);
	u32 sectorsize = 4096;
	u32 stripesize = 4096;
	int zero_end = 1;
	int fd = -1;
	int ret;
	int close_ret;
	int i;
	int mixed = 0;
	int nodesize_forced = 0;
	int data_profile_opt = 0;
	int metadata_profile_opt = 0;
	int discard = 1;
	int ssd = 0;
	int force_overwrite = 0;
	char *subvol_name = NULL;
	int subvol_name_set = 0;
	char *source_dir = NULL;
	int source_dir_set = 0;
	u64 num_of_meta_chunks = 0;
	u64 size_of_data = 0;
	u64 source_dir_size = 0;
	u64 min_dev_size;
	int dev_cnt = 0;
	int saved_optind;
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE] = { 0 };
	u64 features = BTRFS_MKFS_DEFAULT_FEATURES;
	struct mkfs_allocation allocation = { 0 };
	struct btrfs_mkfs_config mkfs_cfg;

	while(1) {
		int c;
		static const struct option long_options[] = {
			{ "alloc-start", required_argument, NULL, 'A'},
			{ "byte-count", required_argument, NULL, 'b' },
			{ "force", no_argument, NULL, 'f' },
			{ "leafsize", required_argument, NULL, 'l' },
			{ "label", required_argument, NULL, 'L'},
			{ "metadata", required_argument, NULL, 'm' },
			{ "mixed", no_argument, NULL, 'M' },
			{ "nodesize", required_argument, NULL, 'n' },
			{ "sectorsize", required_argument, NULL, 's' },
			{ "data", required_argument, NULL, 'd' },
			{ "version", no_argument, NULL, 'V' },
			{ "subvol", required_argument, NULL, 'S'},
			{ "rootdir", required_argument, NULL, 'r' },
			{ "nodiscard", no_argument, NULL, 'K' },
			{ "features", required_argument, NULL, 'O' },
			{ "uuid", required_argument, NULL, 'U' },
			{ "quiet", 0, NULL, 'q' },
			{ "help", no_argument, NULL, GETOPT_VAL_HELP },
			{ NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv, "A:b:fl:n:s:m:d:L:O:S:r:U:VMKq",
				long_options, NULL);
		if (c < 0)
			break;
		switch(c) {
			case 'A':
				alloc_start = parse_size(optarg);
				break;
			case 'f':
				force_overwrite = 1;
				break;
			case 'd':
				data_profile = parse_profile(optarg);
				data_profile_opt = 1;
				break;
			case 'l':
				warning("--leafsize is deprecated, use --nodesize");
				/* fall through */
			case 'n':
				nodesize = parse_size(optarg);
				nodesize_forced = 1;
				break;
			case 'L':
				label = parse_label(optarg);
				break;
			case 'm':
				metadata_profile = parse_profile(optarg);
				metadata_profile_opt = 1;
				break;
			case 'M':
				mixed = 1;
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
				if (features & BTRFS_FEATURE_LIST_ALL) {
					btrfs_list_all_fs_features(0);
					goto success;
				}
				break;
				}
			case 's':
				sectorsize = parse_size(optarg);
				break;
			case 'b':
				block_count = parse_size(optarg);
				zero_end = 0;
				break;
			case 'V':
				printf("mkfs.btrfs, part of %s\n",
						PACKAGE_STRING);
				goto success;
			case 'S':
				subvol_name = parse_subvol_name(optarg);
				subvol_name_set = 1;
				break;
			case 'r':
				source_dir = optarg;
				source_dir_set = 1;
				break;
			case 'U':
				strncpy(fs_uuid, optarg,
					BTRFS_UUID_UNPARSED_SIZE - 1);
				break;
			case 'K':
				discard = 0;
				break;
			case 'q':
				verbose = 0;
				break;
			case GETOPT_VAL_HELP:
			default:
				print_usage(c != GETOPT_VAL_HELP);
		}
	}

	if (subvol_name_set && !source_dir_set) {
		error("root directory needs to be set");
		goto error;
	}

	if (verbose) {
		printf("%s\n", PACKAGE_STRING);
		printf("See %s for more information.\n\n", PACKAGE_URL);
	}

	sectorsize = max(sectorsize, (u32)sysconf(_SC_PAGESIZE));
	stripesize = sectorsize;
	saved_optind = optind;
	dev_cnt = argc - optind;
	if (dev_cnt == 0)
		print_usage(1);

	if (source_dir_set && dev_cnt > 1) {
		error("the option -r is limited to a single device");
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

	while (dev_cnt-- > 0) {
		file = argv[optind++];
		if (is_block_device(file) == 1)
			ret = test_dev_for_mkfs(file, force_overwrite);
		else
			ret = test_status_for_mkfs(file, force_overwrite);

		if (ret)
			goto error;
	}

	optind = saved_optind;
	dev_cnt = argc - optind;

	file = argv[optind++];
	ssd = is_ssd(file);

	/*
	* Set default profiles according to number of added devices.
	* For mixed groups defaults are single/single.
	*/
	if (!mixed) {
		if (!metadata_profile_opt) {
			if (dev_cnt == 1 && ssd && verbose)
				printf("Detected a SSD, turning off metadata "
				"duplication.  Mkfs with -m dup if you want to "
				"force metadata duplication.\n");

			metadata_profile = (dev_cnt > 1) ?
					BTRFS_BLOCK_GROUP_RAID1 : (ssd) ?
					0: BTRFS_BLOCK_GROUP_DUP;
		}
		if (!data_profile_opt) {
			data_profile = (dev_cnt > 1) ?
				BTRFS_BLOCK_GROUP_RAID0 : 0; /* raid0 or single */
		}
	} else {
		u32 best_nodesize = max_t(u32, sysconf(_SC_PAGESIZE), sectorsize);

		if (metadata_profile_opt || data_profile_opt) {
			if (metadata_profile != data_profile) {
				error(
	"with mixed block groups data and metadata profiles must be the same");
				goto error;
			}
		}

		if (!nodesize_forced)
			nodesize = best_nodesize;
	}

	/*
	 * FS features that can be set by other means than -O
	 * just set the bit here
	 */
	if (mixed)
		features |= BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS;

	if ((data_profile | metadata_profile) &
	    (BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)) {
		features |= BTRFS_FEATURE_INCOMPAT_RAID56;
	}

	if (btrfs_check_nodesize(nodesize, sectorsize,
				 features))
		goto error;

	if (sectorsize < sizeof(struct btrfs_super_block)) {
		error("sectorsize smaller than superblock: %u < %zu",
				sectorsize, sizeof(struct btrfs_super_block));
		goto error;
	}

	min_dev_size = btrfs_min_dev_size(nodesize, mixed, metadata_profile,
					  data_profile);
	/* Check device/block_count after the nodesize is determined */
	if (block_count && block_count < min_dev_size) {
		error("size %llu is too small to make a usable filesystem",
			block_count);
		error("minimum size for btrfs filesystem is %llu",
			min_dev_size);
		goto error;
	}
	for (i = saved_optind; i < saved_optind + dev_cnt; i++) {
		char *path;

		path = argv[i];
		ret = test_minimum_size(path, min_dev_size);
		if (ret < 0) {
			error("failed to check size for %s: %s",
				path, strerror(-ret));
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
			dev_cnt, mixed, ssd);
	if (ret)
		goto error;

	dev_cnt--;

	if (!source_dir_set) {
		/*
		 * open without O_EXCL so that the problem should not
		 * occur by the following processing.
		 * (btrfs_register_one_device() fails if O_EXCL is on)
		 */
		fd = open(file, O_RDWR);
		if (fd < 0) {
			error("unable to open %s: %s", file, strerror(errno));
			goto error;
		}
		ret = btrfs_prepare_device(fd, file, &dev_block_count,
				block_count,
				(zero_end ? PREP_DEVICE_ZERO_END : 0) |
				(discard ? PREP_DEVICE_DISCARD : 0) |
				(verbose ? PREP_DEVICE_VERBOSE : 0));
		if (ret) {
			goto error;
		}
		if (block_count && block_count > dev_block_count) {
			error("%s is smaller than requested size, expected %llu, found %llu",
					file,
					(unsigned long long)block_count,
					(unsigned long long)dev_block_count);
			goto error;
		}
	} else {
		fd = open(file, O_CREAT | O_RDWR,
				S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
		if (fd < 0) {
			error("unable to open %s: %s", file, strerror(errno));
			goto error;
		}

		source_dir_size = btrfs_mkfs_size_dir(source_dir, sectorsize,
					&num_of_meta_chunks, &size_of_data);
		if(block_count < source_dir_size)
			block_count = source_dir_size;
		ret = zero_output_file(fd, block_count);
		if (ret) {
			error("unable to zero the output file");
			goto error;
		}
		/* our "device" is the new image file */
		dev_block_count = block_count;
	}

	/* To create the first block group and chunk 0 in make_btrfs */
	if (dev_block_count < BTRFS_MKFS_SYSTEM_GROUP_SIZE) {
		error("device is too small to make filesystem, must be at least %llu",
				(unsigned long long)BTRFS_MKFS_SYSTEM_GROUP_SIZE);
		goto error;
	}

	if (group_profile_max_safe_loss(metadata_profile) <
		group_profile_max_safe_loss(data_profile)){
		warning("metadata has lower redundancy than data!\n");
	}

	mkfs_cfg.label = label;
	memcpy(mkfs_cfg.fs_uuid, fs_uuid, sizeof(mkfs_cfg.fs_uuid));
	mkfs_cfg.num_bytes = dev_block_count;
	mkfs_cfg.nodesize = nodesize;
	mkfs_cfg.sectorsize = sectorsize;
	mkfs_cfg.stripesize = stripesize;
	mkfs_cfg.features = features;

	ret = make_btrfs(fd, &mkfs_cfg);
	if (ret) {
		error("error during mkfs: %s", strerror(-ret));
		goto error;
	}

	fs_info = open_ctree_fs_info(file, 0, 0, 0,
			OPEN_CTREE_WRITES | OPEN_CTREE_FS_PARTIAL);
	if (!fs_info) {
		error("open ctree failed");
		goto error;
	}
	close(fd);
	fd = -1;
	root = fs_info->fs_root;
	fs_info->alloc_start = alloc_start;

	ret = create_metadata_block_groups(root, mixed, &allocation);
	if (ret) {
		error("failed to create default block groups: %d", ret);
		goto error;
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		error("failed to start transaction");
		goto error;
	}

	ret = create_data_block_groups(trans, root, mixed, &allocation);
	if (ret) {
		error("failed to create default data block groups: %d", ret);
		goto error;
	}

	ret = make_root_dir(trans, root);
	if (ret) {
		error("failed to setup the root directory: %d", ret);
		goto error;
	}

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		error("unable to commit transaction: %d", ret);
		goto out;
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		error("failed to start transaction");
		goto error;
	}

	if (dev_cnt == 0)
		goto raid_groups;

	while (dev_cnt-- > 0) {
		file = argv[optind++];

		/*
		 * open without O_EXCL so that the problem should not
		 * occur by the following processing.
		 * (btrfs_register_one_device() fails if O_EXCL is on)
		 */
		fd = open(file, O_RDWR);
		if (fd < 0) {
			error("unable to open %s: %s", file, strerror(errno));
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
		ret = btrfs_prepare_device(fd, file, &dev_block_count,
				block_count,
				(verbose ? PREP_DEVICE_VERBOSE : 0) |
				(zero_end ? PREP_DEVICE_ZERO_END : 0) |
				(discard ? PREP_DEVICE_DISCARD : 0));
		if (ret) {
			goto error;
		}

		ret = btrfs_add_to_fsid(trans, root, fd, file, dev_block_count,
					sectorsize, sectorsize, sectorsize);
		if (ret) {
			error("unable to add %s to filesystem: %d", file, ret);
			goto out;
		}
		if (verbose >= 2) {
			struct btrfs_device *device;

			device = container_of(fs_info->fs_devices->devices.next,
					struct btrfs_device, dev_list);
			printf("adding device %s id %llu\n", file,
				(unsigned long long)device->devid);
		}
	}

raid_groups:
	if (!source_dir_set) {
		ret = create_raid_groups(trans, root, data_profile,
				 metadata_profile, mixed, &allocation);
		if (ret) {
			error("unable to create raid groups: %d", ret);
			goto out;
		}
	}

	ret = create_tree(trans, root, BTRFS_DATA_RELOC_TREE_OBJECTID);
	if (ret) {
		error("unable to create data reloc tree: %d", ret);
		goto out;
	}

	ret = btrfs_commit_transaction(trans, root);
	if (ret) {
		error("unable to commit transaction: %d", ret);
		goto out;
	}

	ret = cleanup_temp_chunks(fs_info, &allocation, data_profile,
				  metadata_profile, metadata_profile);
	if (ret < 0) {
		error("failed to cleanup temporary chunks: %d", ret);
		goto out;
	}

	if (source_dir_set) {
		if (subvol_name_set) {
			u64 dirid, objectid;
			struct btrfs_root *file_root;

			dirid = btrfs_root_dirid(&fs_info->tree_root->root_item);
			ret = btrfs_find_free_objectid(NULL, fs_info->tree_root,
					dirid, &objectid);
			if (ret) {
				error("unable to find a free objectid: %d", ret);
				goto out;
			}
			trans = btrfs_start_transaction(root, 1);
			if (!trans) {
				error("unable to start transaction");
				ret = -EINVAL;
				goto out;
			}
			ret = create_tree(trans, root, objectid);
			if (ret < 0) {
				error("failed to create subvolume: %d", ret);
				goto out;
			}
			ret = btrfs_commit_transaction(trans, root);
			if (ret) {
				error("unable to commit transaction: %d", ret);
				goto out;
			}
			file_root = btrfs_mksubvol(root, subvol_name, objectid,
					0);
			if (!file_root) {
				error("unable to link the subvolume %s",
						subvol_name);
				goto out;
			}
			ret = btrfs_mkfs_fill_dir(source_dir, file_root, verbose);
			if (ret) {
				error("error while filling filesystem: %d", ret);
				goto out;
			}
		} else {
			ret = btrfs_mkfs_fill_dir(source_dir, root, verbose);
			if (ret) {
				error("error while filling filesystem: %d", ret);
				goto out;
			}
		}
	}

	if (verbose) {
		char features_buf[64];

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
		btrfs_parse_features_to_string(features_buf, features);
		printf("Incompat features:  %s", features_buf);
		printf("\n");

		list_all_devices(root);
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
		dev_cnt = argc - optind;
		while (dev_cnt-- > 0) {
			file = argv[optind++];
			if (is_block_device(file) == 1)
				btrfs_register_one_device(file);
		}
	}

	btrfs_close_all_devices();
	free(label);
	free(subvol_name);

	return !!ret;
error:
	if (fd > 0)
		close(fd);

	free(label);
	free(subvol_name);
	exit(1);
success:
	exit(0);
}
