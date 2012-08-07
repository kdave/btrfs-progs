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
#include <unistd.h>

#include "ctree.h"
#include "ioctl.h"

#include "commands.h"

static const char * const quota_cmd_group_usage[] = {
	"btrfs quota <command> [options] <path>",
	NULL
};

int quota_ctl(int cmd, int argc, char **argv)
{
	int ret = 0;
	int fd;
	int e;
	char *path = argv[1];
	struct btrfs_ioctl_quota_ctl_args args;

	if (check_argc_exact(argc, 2))
		return -1;

	memset(&args, 0, sizeof(args));
	args.cmd = cmd;

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 12;
	}

	ret = ioctl(fd, BTRFS_IOC_QUOTA_CTL, &args);
	e = errno;
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "ERROR: quota command failed: %s\n",
			strerror(e));
		return 30;
	}
	return 0;
}

static const char * const cmd_quota_enable_usage[] = {
	"btrfs quota enable <path>",
	"Enable subvolume quota support for a filesystem.",
	NULL
};

static int cmd_quota_enable(int argc, char **argv)
{
	int ret = quota_ctl(BTRFS_QUOTA_CTL_ENABLE, argc, argv);
	if (ret < 0)
		usage(cmd_quota_enable_usage);
	return ret;
}

static const char * const cmd_quota_disable_usage[] = {
	"btrfs quota disable <path>",
	"Disable subvolume quota support for a filesystem.",
	NULL
};

static int cmd_quota_disable(int argc, char **argv)
{
	int ret = quota_ctl(BTRFS_QUOTA_CTL_DISABLE, argc, argv);
	if (ret < 0)
		usage(cmd_quota_disable_usage);
	return ret;
}

static const char * const cmd_quota_rescan_usage[] = {
	"btrfs quota rescan <path>",
	"Rescan the subvolume for a changed quota setting.",
	NULL
};

static int cmd_quota_rescan(int argc, char **argv)
{
	int ret = quota_ctl(BTRFS_QUOTA_CTL_RESCAN, argc, argv);
	if (ret < 0)
		usage(cmd_quota_rescan_usage);
	return ret;
}

const struct cmd_group quota_cmd_group = {
	quota_cmd_group_usage, NULL, {
		{ "enable", cmd_quota_enable, cmd_quota_enable_usage, NULL, 0 },
		{ "disable", cmd_quota_disable, cmd_quota_disable_usage, 0, 0 },
		{ "rescan", cmd_quota_rescan, cmd_quota_rescan_usage, NULL, 0 },
		{ 0, 0, 0, 0, 0 }
	}
};

int cmd_quota(int argc, char **argv)
{
	return handle_command_group(&quota_cmd_group, argc, argv);
}
