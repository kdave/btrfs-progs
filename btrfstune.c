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

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
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
#include "version.h"

static char *device;

static int update_seeding_flag(struct btrfs_root *root, int set_flag)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *disk_super;
	u64 super_flags;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_flags(disk_super);
	if (set_flag) {
		if (super_flags & BTRFS_SUPER_FLAG_SEEDING) {
			fprintf(stderr, "seeding flag is already set on %s\n",
				device);
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

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfstune [options] device\n");
	fprintf(stderr, "\t-S value\tenable/disable seeding\n");
	fprintf(stderr, "\t-r \t\tenable extended inode refs\n");
	fprintf(stderr, "\t-x enable skinny metadata extent refs\n");
}

int main(int argc, char *argv[])
{
	struct btrfs_root *root;
	int success = 0;
	int extrefs_flag = 0;
	int seeding_flag = 0;
	u64 seeding_value = 0;
	int skinny_flag = 0;
	int ret;

	optind = 1;
	while(1) {
		int c = getopt(argc, argv, "S:rx");
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
		default:
			print_usage();
			return 1;
		}
	}

	argc = argc - optind;
	device = argv[optind];
	if (argc != 1) {
		print_usage();
		return 1;
	}

	if (!(seeding_flag + extrefs_flag + skinny_flag)) {
		fprintf(stderr,
			"ERROR: At least one option should be assigned.\n");
		print_usage();
		return 1;
	}

	if (check_mounted(device)) {
		fprintf(stderr, "%s is mounted\n", device);
		return 1;
	}

	root = open_ctree(device, 0, OPEN_CTREE_WRITES);

	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		return 1;
	}

	if (seeding_flag) {
		ret = update_seeding_flag(root, seeding_value);
		if (!ret)
			success++;
	}

	if (extrefs_flag) {
		enable_extrefs_flag(root);
		success++;
	}

	if (skinny_flag) {
		enable_skinny_metadata(root);
		success++;
	}

	if (success > 0) {
		ret = 0;
	} else {
		root->fs_info->readonly = 1;
		ret = 1;
		fprintf(stderr, "btrfstune failed\n");
	}
	close_ctree(root);

	return ret;
}
