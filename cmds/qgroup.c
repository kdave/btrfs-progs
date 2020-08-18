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
#include <getopt.h>

#include <btrfsutil.h>

#include "kernel-shared/ctree.h"
#include "ioctl.h"

#include "cmds/commands.h"
#include "qgroup.h"
#include "common/utils.h"
#include "common/help.h"

static const char * const qgroup_cmd_group_usage[] = {
	"btrfs qgroup <command> [options] <path>",
	NULL
};

static int _cmd_qgroup_assign(const struct cmd_struct *cmd, int assign,
			      int argc, char **argv)
{
	int ret = 0;
	int fd;
	bool rescan = true;
	char *path;
	struct btrfs_ioctl_qgroup_assign_args args;
	DIR *dirstream = NULL;

	optind = 0;
	while (1) {
		enum { GETOPT_VAL_RESCAN = 256, GETOPT_VAL_NO_RESCAN };
		static const struct option long_options[] = {
			{ "rescan", no_argument, NULL, GETOPT_VAL_RESCAN },
			{ "no-rescan", no_argument, NULL, GETOPT_VAL_NO_RESCAN },
			{ NULL, 0, NULL, 0 }
		};
		int c;

		c = getopt_long(argc, argv, "", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case GETOPT_VAL_RESCAN:
			rescan = true;
			break;
		case GETOPT_VAL_NO_RESCAN:
			rescan = false;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 3))
		return 1;

	memset(&args, 0, sizeof(args));
	args.assign = assign;
	args.src = parse_qgroupid(argv[optind]);
	args.dst = parse_qgroupid(argv[optind + 1]);

	path = argv[optind + 2];

	/*
	 * FIXME src should accept subvol path
	 */
	if (btrfs_qgroup_level(args.src) >= btrfs_qgroup_level(args.dst)) {
		error("bad relation requested: %s", path);
		return 1;
	}
	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_QGROUP_ASSIGN, &args);
	if (ret < 0) {
		error("unable to assign quota group: %s",
				errno == ENOTCONN ? "quota not enabled"
						: strerror(errno));
		close_file_or_dir(fd, dirstream);
		return 1;
	}

	/*
	 * If ret > 0, it means assign caused qgroup data inconsistent state.
	 * Schedule a quota rescan if requested.
	 *
	 * The return value change only happens in newer kernel. But will not
	 * cause problem since old kernel has a bug that will never clear
	 * INCONSISTENT bit.
	 */
	if (ret > 0) {
		if (rescan) {
			struct btrfs_ioctl_quota_rescan_args qargs;

			printf("Quota data changed, rescan scheduled\n");
			memset(&qargs, 0, sizeof(qargs));
			ret = ioctl(fd, BTRFS_IOC_QUOTA_RESCAN, &qargs);
			if (ret < 0)
				error("quota rescan failed: %m");
		} else {
			warning("quotas may be inconsistent, rescan needed");
			ret = 0;
		}
	}
	close_file_or_dir(fd, dirstream);
	return ret;
}

static int _cmd_qgroup_create(int create, int argc, char **argv)
{
	int ret = 0;
	int fd;
	char *path;
	struct btrfs_ioctl_qgroup_create_args args;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc - optind, 2))
		return 1;

	memset(&args, 0, sizeof(args));
	args.create = create;
	args.qgroupid = parse_qgroupid(argv[optind]);
	path = argv[optind + 1];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_QGROUP_CREATE, &args);
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		error("unable to %s quota group: %s",
			create ? "create":"destroy",
				errno == ENOTCONN ? "quota not enabled"
						: strerror(errno));
		return 1;
	}
	return 0;
}

static const char * const cmd_qgroup_assign_usage[] = {
	"btrfs qgroup assign [options] <src> <dst> <path>",
	"Assign SRC as the child qgroup of DST",
	"",
	"--rescan       schedule qutoa rescan if needed",
	"--no-rescan    don't schedule quota rescan",
	NULL
};

static int cmd_qgroup_assign(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	return _cmd_qgroup_assign(cmd, 1, argc, argv);
}
static DEFINE_SIMPLE_COMMAND(qgroup_assign, "assign");

static const char * const cmd_qgroup_remove_usage[] = {
	"btrfs qgroup remove [options] <src> <dst> <path>",
	"Remove a child qgroup SRC from DST.",
	"",
	"--rescan       schedule qutoa rescan if needed",
	"--no-rescan    don't schedule quota rescan",
	NULL
};

static int cmd_qgroup_remove(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	return _cmd_qgroup_assign(cmd, 0, argc, argv);
}
static DEFINE_SIMPLE_COMMAND(qgroup_remove, "remove");

static const char * const cmd_qgroup_create_usage[] = {
	"btrfs qgroup create <qgroupid> <path>",
	"Create a subvolume quota group.",
	NULL
};

static int cmd_qgroup_create(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	clean_args_no_options(cmd, argc, argv);

	return _cmd_qgroup_create(1, argc, argv);
}
static DEFINE_SIMPLE_COMMAND(qgroup_create, "create");

static const char * const cmd_qgroup_destroy_usage[] = {
	"btrfs qgroup destroy <qgroupid> <path>",
	"Destroy a quota group.",
	NULL
};

static int cmd_qgroup_destroy(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	clean_args_no_options(cmd, argc, argv);

	return _cmd_qgroup_create(0, argc, argv);
}
static DEFINE_SIMPLE_COMMAND(qgroup_destroy, "destroy");

static const char * const cmd_qgroup_show_usage[] = {
	"btrfs qgroup show [options] <path>",
	"Show subvolume quota groups.",
	"",
	"-p             print parent qgroup id",
	"-c             print child qgroup id",
	"-r             print limit of referenced size of qgroup",
	"-e             print limit of exclusive size of qgroup",
	"-F             list all qgroups which impact the given path",
	"               (including ancestral qgroups)",
	"-f             list all qgroups which impact the given path",
	"               (excluding ancestral qgroups)",
	HELPINFO_UNITS_LONG,
	"--sort=qgroupid,rfer,excl,max_rfer,max_excl",
	"               list qgroups sorted by specified items",
	"               you can use '+' or '-' in front of each item.",
	"               (+:ascending, -:descending, ascending default)",
	"--sync         force sync of the filesystem before getting info",
	NULL
};

static int cmd_qgroup_show(const struct cmd_struct *cmd, int argc, char **argv)
{
	char *path;
	int ret = 0;
	int fd;
	DIR *dirstream = NULL;
	u64 qgroupid;
	int filter_flag = 0;
	unsigned unit_mode;
	int sync = 0;
	enum btrfs_util_error err;

	struct btrfs_qgroup_comparer_set *comparer_set;
	struct btrfs_qgroup_filter_set *filter_set;
	filter_set = btrfs_qgroup_alloc_filter_set();
	comparer_set = btrfs_qgroup_alloc_comparer_set();

	unit_mode = get_unit_mode_from_arg(&argc, argv, 0);

	optind = 0;
	while (1) {
		int c;
		enum {
			GETOPT_VAL_SORT = 256,
			GETOPT_VAL_SYNC
		};
		static const struct option long_options[] = {
			{"sort", required_argument, NULL, GETOPT_VAL_SORT},
			{"sync", no_argument, NULL, GETOPT_VAL_SYNC},
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "pcreFf", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'p':
			btrfs_qgroup_setup_print_column(
				BTRFS_QGROUP_PARENT);
			break;
		case 'c':
			btrfs_qgroup_setup_print_column(
				BTRFS_QGROUP_CHILD);
			break;
		case 'r':
			btrfs_qgroup_setup_print_column(
				BTRFS_QGROUP_MAX_RFER);
			break;
		case 'e':
			btrfs_qgroup_setup_print_column(
				BTRFS_QGROUP_MAX_EXCL);
			break;
		case 'F':
			filter_flag |= 0x1;
			break;
		case 'f':
			filter_flag |= 0x2;
			break;
		case GETOPT_VAL_SORT:
			ret = btrfs_qgroup_parse_sort_string(optarg,
							     &comparer_set);
			if (ret < 0) {
				errno = -ret;
				error("cannot parse sort string: %m");
				return 1;
			}
			if (ret > 0) {
				error("unrecognized format of sort string");
				return 1;
			}
			break;
		case GETOPT_VAL_SYNC:
			sync = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	btrfs_qgroup_setup_units(unit_mode);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];
	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0) {
		free(filter_set);
		free(comparer_set);
		return 1;
	}

	if (sync) {
		err = btrfs_util_sync_fd(fd);
		if (err)
			warning("sync ioctl failed on '%s': %m", path);
	}

	if (filter_flag) {
		ret = lookup_path_rootid(fd, &qgroupid);
		if (ret < 0) {
			errno = -ret;
			error("cannot resolve rootid for %s: %m", path);
			close_file_or_dir(fd, dirstream);
			goto out;
		}
		if (filter_flag & 0x1)
			btrfs_qgroup_setup_filter(&filter_set,
					BTRFS_QGROUP_FILTER_ALL_PARENT,
					qgroupid);
		if (filter_flag & 0x2)
			btrfs_qgroup_setup_filter(&filter_set,
					BTRFS_QGROUP_FILTER_PARENT,
					qgroupid);
	}
	ret = btrfs_show_qgroups(fd, filter_set, comparer_set);
	close_file_or_dir(fd, dirstream);
	free(filter_set);
	free(comparer_set);

out:
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(qgroup_show, "show");

static const char * const cmd_qgroup_limit_usage[] = {
	"btrfs qgroup limit [options] <size>|none [<qgroupid>] <path>",
	"Set the limits a subvolume quota group.",
	"",
	"-c   limit amount of data after compression. This is the default,",
	"     it is currently not possible to turn off this option.",
	"-e   limit space exclusively assigned to this qgroup",
	NULL
};

static int cmd_qgroup_limit(const struct cmd_struct *cmd, int argc, char **argv)
{
	int ret = 0;
	int fd;
	char *path = NULL;
	struct btrfs_ioctl_qgroup_limit_args args;
	unsigned long long size;
	int compressed = 0;
	int exclusive = 0;
	DIR *dirstream = NULL;
	enum btrfs_util_error err;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "ce");
		if (c < 0)
			break;
		switch (c) {
		case 'c':
			compressed = 1;
			break;
		case 'e':
			exclusive = 1;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 2))
		return 1;

	if (!strcasecmp(argv[optind], "none"))
		size = -1ULL;
	else
		size = parse_size(argv[optind]);

	memset(&args, 0, sizeof(args));
	if (compressed)
		args.lim.flags |= BTRFS_QGROUP_LIMIT_RFER_CMPR |
				  BTRFS_QGROUP_LIMIT_EXCL_CMPR;
	if (exclusive) {
		args.lim.flags |= BTRFS_QGROUP_LIMIT_MAX_EXCL;
		args.lim.max_exclusive = size;
	} else {
		args.lim.flags |= BTRFS_QGROUP_LIMIT_MAX_RFER;
		args.lim.max_referenced = size;
	}

	if (argc - optind == 2) {
		args.qgroupid = 0;
		path = argv[optind + 1];
		err = btrfs_util_is_subvolume(path);
		if (err) {
			error_btrfs_util(err);
			return 1;
		}
		/*
		 * keep qgroupid at 0, this indicates that the subvolume the
		 * fd refers to is to be limited
		 */
	} else if (argc - optind == 3) {
		args.qgroupid = parse_qgroupid(argv[optind + 1]);
		path = argv[optind + 2];
	} else
		usage(cmd);

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_QGROUP_LIMIT, &args);
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		error("unable to limit requested quota group: %s",
				errno == ENOTCONN ? "quota not enabled"
						: strerror(errno));

		return 1;
	}
	return 0;
}
static DEFINE_SIMPLE_COMMAND(qgroup_limit, "limit");

static const char qgroup_cmd_group_info[] =
"manage quota groups";

static const struct cmd_group qgroup_cmd_group = {
	qgroup_cmd_group_usage, qgroup_cmd_group_info, {
		&cmd_struct_qgroup_assign,
		&cmd_struct_qgroup_remove,
		&cmd_struct_qgroup_create,
		&cmd_struct_qgroup_destroy,
		&cmd_struct_qgroup_show,
		&cmd_struct_qgroup_limit,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(qgroup);
