/*
 * Copyright (C) 2012 STRATO.  All rights reserved.
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
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "common/help.h"
#include "common/open-utils.h"
#include "common/messages.h"
#include "cmds/commands.h"
#include "ioctl.h"

static const char * const quota_cmd_group_usage[] = {
	"btrfs quota <command> [options] <path>",
	NULL
};

static int quota_ctl(int cmd, int argc, char **argv)
{
	int ret = 0;
	int fd;
	char *path = argv[1];
	struct btrfs_ioctl_quota_ctl_args args;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 2))
		return -1;

	memset(&args, 0, sizeof(args));
	args.cmd = cmd;

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_QUOTA_CTL, &args);
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		error("quota command failed: %m");
		return 1;
	}
	return 0;
}

static const char * const cmd_quota_enable_usage[] = {
	"btrfs quota enable <path>",
	"Enable subvolume quota support for a filesystem.",
	"Any data already present on the filesystem will not count towards",
	"the space usage numbers. It is recommended to enable quota for a",
	"filesystem before writing any data to it.",
	NULL
};

static int cmd_quota_enable(const struct cmd_struct *cmd, int argc, char **argv)
{
	int ret;

	clean_args_no_options(cmd, argc, argv);

	ret = quota_ctl(BTRFS_QUOTA_CTL_ENABLE, argc, argv);

	if (ret < 0)
		usage(cmd);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(quota_enable, "enable");

static const char * const cmd_quota_disable_usage[] = {
	"btrfs quota disable <path>",
	"Disable subvolume quota support for a filesystem.",
	NULL
};

static int cmd_quota_disable(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	int ret;

	clean_args_no_options(cmd, argc, argv);

	ret = quota_ctl(BTRFS_QUOTA_CTL_DISABLE, argc, argv);

	if (ret < 0)
		usage(cmd);
	return ret;
}
static DEFINE_SIMPLE_COMMAND(quota_disable, "disable");

static const char * const cmd_quota_rescan_usage[] = {
	"btrfs quota rescan [-sw] <path>",
	"Trash all qgroup numbers and scan the metadata again with the current config.",
	"",
	"-s|--status         show status of a running rescan operation",
	"-w|--wait           start rescan and wait for it to finish (can be already in progress)",
	"-W|--wait-norescan  wait for rescan to finish without starting it",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_quota_rescan(const struct cmd_struct *cmd, int argc, char **argv)
{
	int ret = 0;
	int fd;
	int e;
	char *path = NULL;
	struct btrfs_ioctl_quota_rescan_args args;
	unsigned long ioctlnum = BTRFS_IOC_QUOTA_RESCAN;
	DIR *dirstream = NULL;
	bool wait_for_completion = false;

	optind = 0;
	while (1) {
		static const struct option long_options[] = {
			{"status", no_argument, NULL, 's'},
			{"wait", no_argument, NULL, 'w'},
			{"wait-norescan", no_argument, NULL, 'W'},
			{NULL, 0, NULL, 0}
		};
		int c;

		c = getopt_long(argc, argv, "swW", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 's':
			ioctlnum = BTRFS_IOC_QUOTA_RESCAN_STATUS;
			break;
		case 'w':
			ioctlnum = BTRFS_IOC_QUOTA_RESCAN;
			wait_for_completion = true;
			break;
		case 'W':
			ioctlnum = 0;
			wait_for_completion = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (ioctlnum == BTRFS_IOC_QUOTA_RESCAN_STATUS && wait_for_completion) {
		error("switch -w cannot be used with -s");
		return 1;
	}

	if (check_argc_exact(argc - optind, 1))
		return 1;

	memset(&args, 0, sizeof(args));

	path = argv[optind];
	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	if (ioctlnum) {
		ret = ioctl(fd, ioctlnum, &args);
		e = errno;
	}

	if (ioctlnum == BTRFS_IOC_QUOTA_RESCAN_STATUS) {
		close_file_or_dir(fd, dirstream);
		if (ret < 0) {
			error("could not obtain quota rescan status: %m");
			return 1;
		}
		if (!args.flags)
			pr_verbose(LOG_DEFAULT, "no rescan operation in progress\n");
		else
			pr_verbose(LOG_DEFAULT, "rescan operation running (current key %lld)\n",
				args.progress);
		return 0;
	}

	if (ioctlnum == BTRFS_IOC_QUOTA_RESCAN && ret == 0) {
		pr_verbose(LOG_DEFAULT, "quota rescan started\n");
		fflush(stdout);
	} else if (ret < 0 && (!wait_for_completion || e != EINPROGRESS)) {
		error("quota rescan failed: %m");
		close_file_or_dir(fd, dirstream);
		return 1;
	}

	if (wait_for_completion) {
		ret = ioctl(fd, BTRFS_IOC_QUOTA_RESCAN_WAIT, &args);
		e = errno;
		if (ret < 0) {
			error("quota rescan wait failed: %m");
			close_file_or_dir(fd, dirstream);
			return 1;
		}
	}

	close_file_or_dir(fd, dirstream);
	return 0;
}
static DEFINE_SIMPLE_COMMAND(quota_rescan, "rescan");

static const char quota_cmd_group_info[] =
"manage filesystem quota settings";

static const struct cmd_group quota_cmd_group = {
	quota_cmd_group_usage, quota_cmd_group_info, {
		&cmd_struct_quota_enable,
		&cmd_struct_quota_disable,
		&cmd_struct_quota_rescan,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(quota);
