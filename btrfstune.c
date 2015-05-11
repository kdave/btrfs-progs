/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
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
#include <dirent.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "utils.h"

static char *device;
static int force = 0;

static int update_seeding_flag(struct btrfs_root *root, int set_flag)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *disk_super;
	u64 super_flags;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_flags(disk_super);
	if (set_flag) {
		if (super_flags & BTRFS_SUPER_FLAG_SEEDING) {
			if (force)
				return 0;
			else
				fprintf(stderr, "seeding flag is already set on %s\n", device);
			return 1;
		}
		super_flags |= BTRFS_SUPER_FLAG_SEEDING;
	} else {
		if (!(super_flags & BTRFS_SUPER_FLAG_SEEDING)) {
			fprintf(stderr, "seeding flag is not set on %s\n",
				device);
			return 1;
		}
		super_flags &= ~BTRFS_SUPER_FLAG_SEEDING;
		fprintf(stderr, "Warning: Seeding flag cleared.\n");
	}

	trans = btrfs_start_transaction(root, 1);
	btrfs_set_super_flags(disk_super, super_flags);
	btrfs_commit_transaction(trans, root);

	return 0;
}

static int enable_extrefs_flag(struct btrfs_root *root)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *disk_super;
	u64 super_flags;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_incompat_flags(disk_super);
	super_flags |= BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF;
	trans = btrfs_start_transaction(root, 1);
	btrfs_set_super_incompat_flags(disk_super, super_flags);
	btrfs_commit_transaction(trans, root);

	return 0;
}

static int enable_skinny_metadata(struct btrfs_root *root)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *disk_super;
	u64 super_flags;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_incompat_flags(disk_super);
	super_flags |= BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA;
	trans = btrfs_start_transaction(root, 1);
	btrfs_set_super_incompat_flags(disk_super, super_flags);
	btrfs_commit_transaction(trans, root);

	return 0;
}

static int change_header_uuid(struct btrfs_root *root, struct extent_buffer *eb)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	int same_fsid = 1;
	int same_chunk_tree_uuid = 1;
	int ret;

	/* Check for whether we need to change fs/chunk id */
	if (!fs_info->new_fsid && !fs_info->new_chunk_tree_uuid)
		return 0;
	if (fs_info->new_fsid)
		same_fsid = !memcmp_extent_buffer(eb, fs_info->new_fsid,
					  btrfs_header_fsid(), BTRFS_FSID_SIZE);
	if (fs_info->new_chunk_tree_uuid)
		same_chunk_tree_uuid =
			!memcmp_extent_buffer(eb, fs_info->new_chunk_tree_uuid,
					      btrfs_header_chunk_tree_uuid(eb),
					      BTRFS_UUID_SIZE);
	if (same_fsid && same_chunk_tree_uuid)
		return 0;
	if (!same_fsid)
		write_extent_buffer(eb, fs_info->new_fsid, btrfs_header_fsid(),
				    BTRFS_FSID_SIZE);
	if (!same_chunk_tree_uuid)
		write_extent_buffer(eb, fs_info->new_chunk_tree_uuid,
				    btrfs_header_chunk_tree_uuid(eb),
				    BTRFS_UUID_SIZE);
	ret = write_tree_block(NULL, root, eb);

	return ret;
}

static int change_extents_uuid(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root = fs_info->extent_root;
	struct btrfs_path *path;
	struct btrfs_key key = {0, 0, 0};
	int ret = 0;

	if (!fs_info->new_fsid && !fs_info->new_chunk_tree_uuid)
		return 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Here we don't use transaction as it will takes a lot of reserve
	 * space, and that will make a near-full btrfs unable to change uuid
	 */
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;

	while (1) {
		struct btrfs_extent_item *ei;
		struct extent_buffer *eb;
		u64 flags;
		u64 bytenr;

		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY)
			goto next;
		ei = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_extent_item);
		flags = btrfs_extent_flags(path->nodes[0], ei);
		if (!(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK))
			goto next;

		bytenr = key.objectid;
		eb = read_tree_block(root, bytenr, root->nodesize, 0);
		if (IS_ERR(eb)) {
			fprintf(stderr, "Failed to read tree block: %llu\n",
				bytenr);
			ret = PTR_ERR(eb);
			goto out;
		}
		ret = change_header_uuid(root, eb);
		free_extent_buffer(eb);
		if (ret < 0) {
			fprintf(stderr, "Failed to change uuid of tree block: %llu\n",
				bytenr);
			goto out;
		}
next:
		ret = btrfs_next_item(root, path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}

out:
	btrfs_free_path(path);
	return ret;
}

static int change_device_uuid(struct btrfs_root *root, struct extent_buffer *eb,
			      int slot)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_dev_item *di;
	int ret = 0;

	di = btrfs_item_ptr(eb, slot, struct btrfs_dev_item);
	if (fs_info->new_fsid) {
		if (!memcmp_extent_buffer(eb, fs_info->new_fsid,
					  (unsigned long)btrfs_device_fsid(di),
					  BTRFS_FSID_SIZE))
			return ret;
		write_extent_buffer(eb, fs_info->new_fsid,
				    (unsigned long)btrfs_device_fsid(di),
				    BTRFS_FSID_SIZE);
		ret = write_tree_block(NULL, root, eb);
	}
	return ret;
}

static int change_devices_uuid(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root = fs_info->chunk_root;
	struct btrfs_path *path;
	struct btrfs_key key = {0, 0, 0};
	int ret = 0;

	/*
	 * Unlike change_extents_uuid, we only need to change fsid in dev_item
	 */
	if (!fs_info->new_fsid)
		return 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	/* No transaction again */
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;

	while (1) {
		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		if (key.type != BTRFS_DEV_ITEM_KEY ||
		    key.objectid != BTRFS_DEV_ITEMS_OBJECTID)
			goto next;
		ret = change_device_uuid(root, path->nodes[0], path->slots[0]);
		if (ret < 0)
			goto out;
next:
		ret = btrfs_next_item(root, path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}
out:
	btrfs_free_path(path);
	return ret;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfstune [options] device\n");
	fprintf(stderr, "\t-S value\tpositive value will enable seeding, zero to disable, negative is not allowed\n");
	fprintf(stderr, "\t-r \t\tenable extended inode refs\n");
	fprintf(stderr, "\t-x \t\tenable skinny metadata extent refs\n");
	fprintf(stderr, "\t-f \t\tforce to set or clear flags, make sure that you are aware of the dangers\n");
}

int main(int argc, char *argv[])
{
	struct btrfs_root *root;
	int success = 0;
	int total = 0;
	int extrefs_flag = 0;
	int seeding_flag = 0;
	u64 seeding_value = 0;
	int skinny_flag = 0;
	int ret;

	optind = 1;
	while(1) {
		int c = getopt(argc, argv, "S:rxf");
		if (c < 0)
			break;
		switch(c) {
		case 'S':
			seeding_flag = 1;
			seeding_value = arg_strtou64(optarg);
			break;
		case 'r':
			extrefs_flag = 1;
			break;
		case 'x':
			skinny_flag = 1;
			break;
		case 'f':
			force = 1;
			break;
		default:
			print_usage();
			return 1;
		}
	}

	set_argv0(argv);
	argc = argc - optind;
	device = argv[optind];
	if (check_argc_exact(argc, 1)) {
		print_usage();
		return 1;
	}

	if (!(seeding_flag + extrefs_flag + skinny_flag)) {
		fprintf(stderr,
			"ERROR: At least one option should be assigned.\n");
		print_usage();
		return 1;
	}

	ret = check_mounted(device);
	if (ret < 0) {
		fprintf(stderr, "Could not check mount status: %s\n",
			strerror(-ret));
		return 1;
	} else if (ret) {
		fprintf(stderr, "%s is mounted\n", device);
		return 1;
	}

	root = open_ctree(device, 0, OPEN_CTREE_WRITES);

	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		return 1;
	}

	if (seeding_flag) {
		if (!seeding_value && !force) {
			fprintf(stderr, "Warning: This is dangerous, clearing the seeding flag may cause the derived device not to be mountable!\n");
			ret = ask_user("We are going to clear the seeding flag, are you sure?");
			if (!ret) {
				fprintf(stderr, "Clear seeding flag canceled\n");
				return 1;
			}
		}

		ret = update_seeding_flag(root, seeding_value);
		if (!ret)
			success++;
		total++;
	}

	if (extrefs_flag) {
		enable_extrefs_flag(root);
		success++;
		total++;
	}

	if (skinny_flag) {
		enable_skinny_metadata(root);
		success++;
		total++;
	}

	if (success == total) {
		ret = 0;
	} else {
		root->fs_info->readonly = 1;
		ret = 1;
		fprintf(stderr, "btrfstune failed\n");
	}
	close_ctree(root);

	return ret;
}
