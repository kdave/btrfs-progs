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
#include "transaction.h"
#include "disk-io.h"
#include "commands.h"
#include "utils.h"

static const char * const rescue_cmd_group_usage[] = {
	"btrfs rescue <command> [options] <path>",
	NULL
};

int btrfs_recover_chunk_tree(char *path, int verbose, int yes);
int btrfs_recover_superblocks(char *path, int verbose, int yes);

static const char * const cmd_rescue_chunk_recover_usage[] = {
	"btrfs rescue chunk-recover [options] <device>",
	"Recover the chunk tree by scanning the devices one by one.",
	"",
	"-y	Assume an answer of `yes' to all questions",
	"-v	Verbose mode",
	"-h	Help",
	NULL
};

static int cmd_rescue_chunk_recover(int argc, char *argv[])
{
	int ret = 0;
	char *file;
	int yes = 0;
	int verbose = 0;

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
		case 'h':
		default:
			usage(cmd_rescue_chunk_recover_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_rescue_chunk_recover_usage);

	file = argv[optind];

	ret = check_mounted(file);
	if (ret < 0) {
		error("could not check mount status: %s", strerror(-ret));
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
 *   3 : Fail to Recover bad supeblocks
 *   4 : Abort to recover bad superblocks
 */
static int cmd_rescue_super_recover(int argc, char **argv)
{
	int ret;
	int verbose = 0;
	int yes = 0;
	char *dname;

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
			usage(cmd_rescue_super_recover_usage);
		}
	}
	if (check_argc_exact(argc - optind, 1))
		usage(cmd_rescue_super_recover_usage);

	dname = argv[optind];
	ret = check_mounted(dname);
	if (ret < 0) {
		error("could not check mount status: %s", strerror(-ret));
		return 1;
	} else if (ret) {
		error("the device is busy");
		return 1;
	}
	ret = btrfs_recover_superblocks(dname, verbose, yes);
	return ret;
}

static const char * const cmd_rescue_zero_log_usage[] = {
	"btrfs rescue zero-log <device>",
	"Clear the tree log. Usable if it's corrupted and prevents mount.",
	"",
	NULL
};

static int cmd_rescue_zero_log(int argc, char **argv)
{
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *sb;
	char *devname;
	int ret;

	clean_args_no_options(argc, argv, cmd_rescue_zero_log_usage);

	if (check_argc_exact(argc, 2))
		usage(cmd_rescue_zero_log_usage);

	devname = argv[optind];
	ret = check_mounted(devname);
	if (ret < 0) {
		error("could not check mount status: %s", strerror(-ret));
		goto out;
	} else if (ret) {
		error("%s is currently mounted", devname);
		ret = -EBUSY;
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
	btrfs_set_super_log_root(sb, 0);
	btrfs_set_super_log_root_level(sb, 0);
	btrfs_commit_transaction(trans, root);
	close_ctree(root);

out:
	return !!ret;
}

static const char rescue_cmd_group_info[] =
"toolbox for specific rescue operations";

const struct cmd_group rescue_cmd_group = {
	rescue_cmd_group_usage, rescue_cmd_group_info, {
		{ "chunk-recover", cmd_rescue_chunk_recover,
			cmd_rescue_chunk_recover_usage, NULL, 0},
		{ "super-recover", cmd_rescue_super_recover,
			cmd_rescue_super_recover_usage, NULL, 0},
		{ "zero-log", cmd_rescue_zero_log, cmd_rescue_zero_log_usage, NULL, 0},
		NULL_CMD_STRUCT
	}
};

int cmd_rescue(int argc, char **argv)
{
	return handle_command_group(&rescue_cmd_group, argc, argv);
}
