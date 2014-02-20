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
#include "commands.h"
#include "utils.h"

static const char * const rescue_cmd_group_usage[] = {
	"btrfs rescue <command> [options] <path>",
	NULL
};

int btrfs_recover_chunk_tree(char *path, int verbose, int yes);
int btrfs_recover_superblocks(char *path, int verbose, int yes);

const char * const cmd_chunk_recover_usage[] = {
	"btrfs rescue chunk-recover [options] <device>",
	"Recover the chunk tree by scanning the devices one by one.",
	"",
	"-y	Assume an answer of `yes' to all questions",
	"-v	Verbose mode",
	"-h	Help",
	NULL
};

const char * const cmd_super_recover_usage[] = {
	"btrfs rescue super-recover [options] <device>",
	"Recover bad superblocks from good copies",
	"",
	"-y	Assume an answer of `yes' to all questions",
	"-v	Verbose mode",
	NULL
};

int cmd_chunk_recover(int argc, char *argv[])
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
			usage(cmd_chunk_recover_usage);
		}
	}

	argc = argc - optind;
	if (argc == 0)
		usage(cmd_chunk_recover_usage);

	file = argv[optind];

	ret = check_mounted(file);
	if (ret < 0) {
		fprintf(stderr, "Could not check mount status: %s\n",
			strerror(-ret));
		return 1;
	} else if (ret) {
		fprintf(stderr, "the device is busy\n");
		return 1;
	}

	ret = btrfs_recover_chunk_tree(file, verbose, yes);
	if (!ret) {
		fprintf(stdout, "Recover the chunk tree successfully.\n");
	} else if (ret > 0) {
		ret = 0;
		fprintf(stdout, "Abort to rebuild the on-disk chunk tree.\n");
	} else {
		fprintf(stdout, "Fail to recover the chunk tree.\n");
	}
	return ret;
}

/*
 * return codes:
 *   0 : All superblocks are valid, no need to recover
 *   1 : Usage or syntax error
 *   2 : Recover all bad superblocks successfully
 *   3 : Fail to Recover bad supeblocks
 *   4 : Abort to recover bad superblocks
 */
int cmd_super_recover(int argc, char **argv)
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
			usage(cmd_super_recover_usage);
		}
	}
	argc = argc - optind;
	if (argc != 1)
		usage(cmd_super_recover_usage);

	dname = argv[optind];
	ret = check_mounted(dname);
	if (ret < 0) {
		fprintf(stderr, "Could not check mount status: %s\n",
			strerror(-ret));
		return 1;
	} else if (ret) {
		fprintf(stderr, "the device is busy\n");
		return 1;
	}
	ret = btrfs_recover_superblocks(dname, verbose, yes);
	return ret;
}

const struct cmd_group rescue_cmd_group = {
	rescue_cmd_group_usage, NULL, {
		{ "chunk-recover", cmd_chunk_recover, cmd_chunk_recover_usage, NULL, 0},
		{ "super-recover", cmd_super_recover, cmd_super_recover_usage, NULL, 0},
		{ 0, 0, 0, 0, 0 }
	}
};

int cmd_rescue(int argc, char **argv)
{
	return handle_command_group(&rescue_cmd_group, argc, argv);
}
