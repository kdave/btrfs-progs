/*
 * Copyright (C) 2013 SUSE.  All rights reserved.
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

#include <getopt.h>
#include "ctree.h"
#include "volumes.h"
#include "transaction.h"
#include "disk-io.h"
#include "cmds/commands.h"
#include "common/utils.h"
#include "common/help.h"
#include "cmds/rescue.h"

static const char * const rescue_cmd_group_usage[] = {
	"btrfs rescue <command> [options] <path>",
	NULL
};

static const char * const cmd_rescue_chunk_recover_usage[] = {
	"btrfs rescue chunk-recover [options] <device>",
	"Recover the chunk tree by scanning the devices one by one.",
	"",
	"-y	Assume an answer of `yes' to all questions",
	"-v	Verbose mode",
	"-h	Help",
	NULL
};

static int cmd_rescue_chunk_recover(const struct cmd_struct *cmd,
				    int argc, char *argv[])
{
	int ret = 0;
	char *file;
	int yes = 0;
	int verbose = 0;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "yvh");
		if (c < 0)
			break;
		switch (c) {
		case 'y':
			yes = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	file = argv[optind];

	ret = check_mounted(file);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		return 1;
	} else if (ret) {
		error("the device is busy");
		return 1;
	}

	ret = btrfs_recover_chunk_tree(file, verbose, yes);
	if (!ret) {
		fprintf(stdout, "Chunk tree recovered successfully\n");
	} else if (ret > 0) {
		ret = 0;
		fprintf(stdout, "Chunk tree recovery aborted\n");
	} else {
		fprintf(stdout, "Chunk tree recovery failed\n");
	}
	return ret;
}
static DEFINE_SIMPLE_COMMAND(rescue_chunk_recover, "chunk-recover");

static const char * const cmd_rescue_super_recover_usage[] = {
	"btrfs rescue super-recover [options] <device>",
	"Recover bad superblocks from good copies",
	"",
	"-y	Assume an answer of `yes' to all questions",
	"-v	Verbose mode",
	NULL
};

/*
 * return codes:
 *   0 : All superblocks are valid, no need to recover
 *   1 : Usage or syntax error
 *   2 : Recover all bad superblocks successfully
 *   3 : Fail to Recover bad superblocks
 *   4 : Abort to recover bad superblocks
 */
static int cmd_rescue_super_recover(const struct cmd_struct *cmd,
				    int argc, char **argv)
{
	int ret;
	int verbose = 0;
	int yes = 0;
	char *dname;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "vy");
		if (c < 0)
			break;
		switch (c) {
		case 'v':
			verbose = 1;
			break;
		case 'y':
			yes = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	if (check_argc_exact(argc - optind, 1))
		return 1;

	dname = argv[optind];
	ret = check_mounted(dname);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		return 1;
	} else if (ret) {
		error("the device is busy");
		return 1;
	}
	ret = btrfs_recover_superblocks(dname, verbose, yes);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(rescue_super_recover, "super-recover");

static const char * const cmd_rescue_zero_log_usage[] = {
	"btrfs rescue zero-log <device>",
	"Clear the tree log. Usable if it's corrupted and prevents mount.",
	NULL
};

static int cmd_rescue_zero_log(const struct cmd_struct *cmd,
			       int argc, char **argv)
{
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *sb;
	char *devname;
	int ret;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc, 2))
		return 1;

	devname = argv[optind];
	ret = check_mounted(devname);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		goto out;
	} else if (ret) {
		error("%s is currently mounted", devname);
		ret = -EBUSY;
		goto out;
	}

	root = open_ctree(devname, 0, OPEN_CTREE_WRITES | OPEN_CTREE_PARTIAL);
	if (!root) {
		error("could not open ctree");
		return 1;
	}

	sb = root->fs_info->super_copy;
	printf("Clearing log on %s, previous log_root %llu, level %u\n",
			devname,
			(unsigned long long)btrfs_super_log_root(sb),
			(unsigned)btrfs_super_log_root_level(sb));
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	btrfs_set_super_log_root(sb, 0);
	btrfs_set_super_log_root_level(sb, 0);
	btrfs_commit_transaction(trans, root);
	close_ctree(root);

out:
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(rescue_zero_log, "zero-log");

static const char * const cmd_rescue_fix_device_size_usage[] = {
	"btrfs rescue fix-device-size <device>",
	"Re-align device and super block sizes. Usable if newer kernel refuse to mount it due to mismatch super size",
	NULL
};

static int cmd_rescue_fix_device_size(const struct cmd_struct *cmd,
				      int argc, char **argv)
{
	struct btrfs_fs_info *fs_info;
	char *devname;
	int ret;

	clean_args_no_options(cmd, argc, argv);

	if (check_argc_exact(argc, 2))
		return 1;

	devname = argv[optind];
	ret = check_mounted(devname);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		goto out;
	} else if (ret) {
		error("%s is currently mounted", devname);
		ret = -EBUSY;
		goto out;
	}

	fs_info = open_ctree_fs_info(devname, 0, 0, 0, OPEN_CTREE_WRITES |
				     OPEN_CTREE_PARTIAL);
	if (!fs_info) {
		error("could not open btrfs");
		ret = -EIO;
		goto out;
	}

	ret = btrfs_fix_device_and_super_size(fs_info);
	if (ret > 0)
		ret = 0;
	close_ctree(fs_info->tree_root);
out:
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(rescue_fix_device_size, "fix-device-size");

static const char rescue_cmd_group_info[] =
"toolbox for specific rescue operations";

static const struct cmd_group rescue_cmd_group = {
	rescue_cmd_group_usage, rescue_cmd_group_info, {
		&cmd_struct_rescue_chunk_recover,
		&cmd_struct_rescue_super_recover,
		&cmd_struct_rescue_zero_log,
		&cmd_struct_rescue_fix_device_size,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(rescue);
