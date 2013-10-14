/*
 * Copyright (C) 2013 Oracle.  All rights reserved.
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

#include <sys/ioctl.h>
#include <unistd.h>
#include <getopt.h>

#include "ctree.h"
#include "ioctl.h"

#include "commands.h"
#include "utils.h"

static const char * const dedup_cmd_group_usage[] = {
	"btrfs dedup <command> [options] <path>",
	NULL
};

static int dedup_ctl(char *path, struct btrfs_ioctl_dedup_args *args)
{
	int ret = 0;
	int fd;
	int e;
	DIR *dirstream = NULL;

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return -EACCES;
	}

	ret = ioctl(fd, BTRFS_IOC_DEDUP_CTL, args);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		fprintf(stderr, "ERROR: dedup command failed: %s\n",
			strerror(e));
		if (args->cmd == BTRFS_DEDUP_CTL_DISABLE ||
		    args->cmd == BTRFS_DEDUP_CTL_SET_BS)
			fprintf(stderr, "please refer to 'dmesg | tail' for more info\n");
		return -EINVAL;
	}
	return 0;
}

static const char * const cmd_dedup_enable_usage[] = {
	"btrfs dedup enable <path>",
	"Enable data deduplication support for a filesystem.",
	NULL
};

static int cmd_dedup_enable(int argc, char **argv)
{
	struct btrfs_ioctl_dedup_args dargs;

	if (check_argc_exact(argc, 2))
		usage(cmd_dedup_enable_usage);

	dargs.cmd = BTRFS_DEDUP_CTL_ENABLE;

	return dedup_ctl(argv[1], &dargs);
}

static const char * const cmd_dedup_disable_usage[] = {
	"btrfs dedup disable <path>",
	"Disable data deduplication support for a filesystem.",
	NULL
};

static int cmd_dedup_disable(int argc, char **argv)
{
	struct btrfs_ioctl_dedup_args dargs;

	if (check_argc_exact(argc, 2))
		usage(cmd_dedup_disable_usage);

	dargs.cmd = BTRFS_DEDUP_CTL_DISABLE;

	return dedup_ctl(argv[1], &dargs);
}

static int dedup_set_bs(char *path, struct btrfs_ioctl_dedup_args *dargs)
{
	return dedup_ctl(path, dargs);
}

static const char * const cmd_dedup_on_usage[] = {
	"btrfs dedup on [-b|--bs size] <path>",
	"Switch on data deduplication or change the dedup blocksize.",
	"",
	"-b|--bs <size>  set dedup blocksize",
	NULL
};

static struct option longopts[] = {
	{"bs", required_argument, NULL, 'b'},
	{0, 0, 0, 0}
};

static int cmd_dedup_on(int argc, char **argv)
{
	struct btrfs_ioctl_dedup_args dargs;
	u64 bs = 8192;

	optind = 1;
	while (1) {
		int longindex;

		int c = getopt_long(argc, argv, "b:", longopts, &longindex);
		if (c < 0)
			break;

		switch (c) {
		case 'b':
			bs = parse_size(optarg);
			break;
		default:
			usage(cmd_dedup_on_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_dedup_on_usage);

	dargs.cmd = BTRFS_DEDUP_CTL_SET_BS;
	dargs.bs = bs;

	return dedup_set_bs(argv[optind], &dargs);
}

static const char * const cmd_dedup_off_usage[] = {
	"btrfs dedup off <path>",
	"Switch off data deduplication.",
	NULL
};

static int cmd_dedup_off(int argc, char **argv)
{
	struct btrfs_ioctl_dedup_args dargs;

	if (check_argc_exact(argc, 2))
		usage(cmd_dedup_off_usage);

	dargs.cmd = BTRFS_DEDUP_CTL_SET_BS;
	dargs.bs = 0;

	return dedup_set_bs(argv[1], &dargs);
}

const struct cmd_group dedup_cmd_group = {
	dedup_cmd_group_usage, NULL, {
		{ "enable", cmd_dedup_enable, cmd_dedup_enable_usage, NULL, 0 },
		{ "disable", cmd_dedup_disable, cmd_dedup_disable_usage, 0, 0 },
		{ "on", cmd_dedup_on, cmd_dedup_on_usage, NULL, 0},
		{ "off", cmd_dedup_off, cmd_dedup_off_usage, NULL, 0},
		NULL_CMD_STRUCT
	}
};

int cmd_dedup(int argc, char **argv)
{
	return handle_command_group(&dedup_cmd_group, argc, argv);
}
