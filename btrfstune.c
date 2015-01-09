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

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfstune [options] device\n");
	fprintf(stderr, "\t-S value\tpositive value will enable seeding, zero to disable, negative is not allowed\n");
	fprintf(stderr, "\t-r \t\tenable extended inode refs\n");
	fprintf(stderr, "\t-x \t\tenable skinny metadata extent refs\n");
	fprintf(stderr, "\t-f \t\tforce to clear flags, make sure that you are aware of the dangers\n");
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
	int force = 0;
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
