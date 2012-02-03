/*
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
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "volumes.h"

#include "commands.h"

static const char balance_cmd_group_usage[] =
	"btrfs [filesystem] balance [<command>] [options] <path>";

static const char balance_cmd_group_info[] =
	"'btrfs filesystem balance' command is deprecated, please use\n"
	"'btrfs balance start' command instead.";

static int do_balance(const char *path, struct btrfs_ioctl_balance_args *args)
{
	int fd;
	int ret;
	int e;

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access to '%s'\n", path);
		return 12;
	}

	ret = ioctl(fd, BTRFS_IOC_BALANCE_V2, args);
	e = errno;
	close(fd);

	if (ret < 0) {
		if (e == ECANCELED) {
			if (args->state & BTRFS_BALANCE_STATE_PAUSE_REQ)
				fprintf(stderr, "balance paused by user\n");
			if (args->state & BTRFS_BALANCE_STATE_CANCEL_REQ)
				fprintf(stderr, "balance canceled by user\n");
		} else {
			fprintf(stderr, "ERROR: error during balancing '%s' "
				"- %s\n", path, strerror(e));
			if (e != EINPROGRESS)
				fprintf(stderr, "There may be more info in "
					"syslog - try dmesg | tail\n");
			return 19;
		}
	} else {
		printf("Done, had to relocate %llu out of %llu chunks\n",
		       (unsigned long long)args->stat.completed,
		       (unsigned long long)args->stat.considered);
	}

	return 0;
}

const struct cmd_group balance_cmd_group = {
	balance_cmd_group_usage, balance_cmd_group_info, {
		{ 0, 0, 0, 0, 0 }
	}
};

int cmd_balance(int argc, char **argv)
{
	if (argc == 2) {
		/* old 'btrfs filesystem balance <path>' syntax */
		struct btrfs_ioctl_balance_args args;

		memset(&args, 0, sizeof(args));
		args.flags |= BTRFS_BALANCE_TYPE_MASK;

		return do_balance(argv[1], &args);
	}

	return handle_command_group(&balance_cmd_group, argc, argv);
}
