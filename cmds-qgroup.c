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

#include "ctree.h"
#include "ioctl.h"

#include "commands.h"
#include "qgroup.h"
#include "utils.h"

static const char * const qgroup_cmd_group_usage[] = {
	"btrfs qgroup <command> [options] <path>",
	NULL
};

static int qgroup_assign(int assign, int argc, char **argv)
{
	int ret = 0;
	int fd;
	int e;
	char *path = argv[3];
	struct btrfs_ioctl_qgroup_assign_args args;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 4))
		return -1;

	memset(&args, 0, sizeof(args));
	args.assign = assign;
	args.src = parse_qgroupid(argv[1]);
	args.dst = parse_qgroupid(argv[2]);

	/*
	 * FIXME src should accept subvol path
	 */
	if ((args.src >> 48) >= (args.dst >> 48)) {
		fprintf(stderr, "ERROR: bad relation requested '%s'\n", path);
		return 1;
	}
	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}

	ret = ioctl(fd, BTRFS_IOC_QGROUP_ASSIGN, &args);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		fprintf(stderr, "ERROR: unable to assign quota group: %s\n",
			strerror(e));
		return 1;
	}
	return 0;
}

static int qgroup_create(int create, int argc, char **argv)
{
	int ret = 0;
	int fd;
	int e;
	char *path = argv[2];
	struct btrfs_ioctl_qgroup_create_args args;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 3))
		return -1;

	memset(&args, 0, sizeof(args));
	args.create = create;
	args.qgroupid = parse_qgroupid(argv[1]);

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}

	ret = ioctl(fd, BTRFS_IOC_QGROUP_CREATE, &args);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		fprintf(stderr, "ERROR: unable to %s quota group: %s\n",
			create ? "create":"destroy", strerror(e));
		return 1;
	}
	return 0;
}

static int parse_limit(const char *p, unsigned long long *s)
{
	char *endptr;
	unsigned long long size;

	if (strcasecmp(p, "none") == 0) {
		*s = 0;
		return 1;
	}
	size = strtoull(p, &endptr, 10);
	switch (*endptr) {
	case 'T':
	case 't':
		size *= 1024;
		/* fallthrough */
	case 'G':
	case 'g':
		size *= 1024;
		/* fallthrough */
	case 'M':
	case 'm':
		size *= 1024;
		/* fallthrough */
	case 'K':
	case 'k':
		size *= 1024;
		++endptr;
		break;
	case 0:
		break;
	default:
		return 0;
	}

	if (*endptr)
		return 0;

	*s = size;

	return 1;
}

static const char * const cmd_qgroup_assign_usage[] = {
	"btrfs qgroup assign <src> <dst> <path>",
	"Enable subvolume qgroup support for a filesystem.",
	NULL
};

static int cmd_qgroup_assign(int argc, char **argv)
{
	int ret = qgroup_assign(1, argc, argv);
	if (ret < 0)
		usage(cmd_qgroup_assign_usage);
	return ret;
}

static const char * const cmd_qgroup_remove_usage[] = {
	"btrfs qgroup remove <src> <dst> <path>",
	"Remove a subvol from a quota group.",
	NULL
};

static int cmd_qgroup_remove(int argc, char **argv)
{
	int ret = qgroup_assign(0, argc, argv);
	if (ret < 0)
		usage(cmd_qgroup_remove_usage);
	return ret;
}

static const char * const cmd_qgroup_create_usage[] = {
	"btrfs qgroup create <qgroupid> <path>",
	"Create a subvolume quota group.",
	NULL
};

static int cmd_qgroup_create(int argc, char **argv)
{
	int ret = qgroup_create(1, argc, argv);
	if (ret < 0)
		usage(cmd_qgroup_create_usage);
	return ret;
}

static const char * const cmd_qgroup_destroy_usage[] = {
	"btrfs qgroup destroy <qgroupid> <path>",
	"Destroy a subvolume quota group.",
	NULL
};

static int cmd_qgroup_destroy(int argc, char **argv)
{
	int ret = qgroup_create(0, argc, argv);
	if (ret < 0)
		usage(cmd_qgroup_destroy_usage);
	return ret;
}

static const char * const cmd_qgroup_show_usage[] = {
	"btrfs qgroup show -pcreFf "
	"[--sort=qgroupid,rfer,excl,max_rfer,max_excl] <path>",
	"Show subvolume quota groups.",
	"-p             print parent qgroup id",
	"-c             print child qgroup id",
	"-r             print limit of referenced size of qgroup",
	"-e             print limit of exclusive size of qgroup",
	"-F             list all qgroups which impact the given path",
	"               (including ancestral qgroups)",
	"-f             list all qgroups which impact the given path",
	"               (excluding ancestral qgroups)",
	"--raw          raw numbers in bytes",
	"--human-readable",
	"               human firendly numbers in given base, 1024 by default",
	"--iec          use 1024 as a base (KiB, MiB, GiB, TiB)",
	"--si           use 1000 as a base (kB, MB, GB, TB)",
	"--kbytes       show sizes in KiB, or kB with --si",
	"--mbytes       show sizes in MiB, or MB with --si",
	"--gbytes       show sizes in GiB, or GB with --si",
	"--tbytes       show sizes in TiB, or TB with --si",
	"--sort=qgroupid,rfer,excl,max_rfer,max_excl",
	"               list qgroups in order of qgroupid,"
	"rfer,max_rfer or max_excl",
	"               you can use '+' or '-' in front of each item.",
	"               (+:ascending, -:descending, ascending default)",
	NULL
};

static int cmd_qgroup_show(int argc, char **argv)
{
	char *path;
	int ret = 0;
	int fd;
	int e;
	DIR *dirstream = NULL;
	u64 qgroupid;
	int filter_flag = 0;
	unsigned unit_mode = UNITS_DEFAULT;

	struct btrfs_qgroup_comparer_set *comparer_set;
	struct btrfs_qgroup_filter_set *filter_set;
	filter_set = btrfs_qgroup_alloc_filter_set();
	comparer_set = btrfs_qgroup_alloc_comparer_set();

	optind = 1;
	while (1) {
		int c;
		int option_index = 0;
		static const struct option long_options[] = {
			{"sort", 1, NULL, 'S'},
			{"raw", no_argument, NULL, GETOPT_VAL_RAW},
			{"kbytes", no_argument, NULL, GETOPT_VAL_KBYTES},
			{"mbytes", no_argument, NULL, GETOPT_VAL_MBYTES},
			{"gbytes", no_argument, NULL, GETOPT_VAL_GBYTES},
			{"tbytes", no_argument, NULL, GETOPT_VAL_TBYTES},
			{"si", no_argument, NULL, GETOPT_VAL_SI},
			{"iec", no_argument, NULL, GETOPT_VAL_IEC},
			{ "human-readable", no_argument, NULL,
				GETOPT_VAL_HUMAN_READABLE},
			{ NULL, 0, NULL, 0 }
		};
		c = getopt_long(argc, argv, "pcreFf",
				long_options, &option_index);

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
		case 'S':
			ret = btrfs_qgroup_parse_sort_string(optarg,
							     &comparer_set);
			if (ret)
				usage(cmd_qgroup_show_usage);
			break;
		case GETOPT_VAL_RAW:
			unit_mode = UNITS_RAW;
			break;
		case GETOPT_VAL_KBYTES:
			units_set_base(&unit_mode, UNITS_KBYTES);
			break;
		case GETOPT_VAL_MBYTES:
			units_set_base(&unit_mode, UNITS_MBYTES);
			break;
		case GETOPT_VAL_GBYTES:
			units_set_base(&unit_mode, UNITS_GBYTES);
			break;
		case GETOPT_VAL_TBYTES:
			units_set_base(&unit_mode, UNITS_TBYTES);
			break;
		case GETOPT_VAL_SI:
			units_set_mode(&unit_mode, UNITS_DECIMAL);
			break;
		case GETOPT_VAL_IEC:
			units_set_mode(&unit_mode, UNITS_BINARY);
			break;
		case GETOPT_VAL_HUMAN_READABLE:
			unit_mode = UNITS_HUMAN_BINARY;
			break;
		default:
			usage(cmd_qgroup_show_usage);
		}
	}
	btrfs_qgroup_setup_units(unit_mode);

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_qgroup_show_usage);

	path = argv[optind];
	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}

	if (filter_flag) {
		qgroupid = btrfs_get_path_rootid(fd);
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
	e = errno;
	close_file_or_dir(fd, dirstream);
	if (ret < 0)
		fprintf(stderr, "ERROR: can't list qgroups: %s\n",
				strerror(e));

	return !!ret;
}

static const char * const cmd_qgroup_limit_usage[] = {
	"btrfs qgroup limit [options] <size>|none [<qgroupid>] <path>",
	"Limit the size of a subvolume quota group.",
	"",
	"-c   limit amount of data after compression. This is the default,",
	"     it is currently not possible to turn off this option.",
	"-e   limit space exclusively assigned to this qgroup",
	NULL
};

static int cmd_qgroup_limit(int argc, char **argv)
{
	int ret = 0;
	int fd;
	int e;
	char *path = NULL;
	struct btrfs_ioctl_qgroup_limit_args args;
	unsigned long long size;
	int compressed = 0;
	int exclusive = 0;
	DIR *dirstream = NULL;

	optind = 1;
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
			usage(cmd_qgroup_limit_usage);
		}
	}

	if (check_argc_min(argc - optind, 2))
		usage(cmd_qgroup_limit_usage);

	if (!parse_limit(argv[optind], &size)) {
		fprintf(stderr, "Invalid size argument given\n");
		return 1;
	}

	memset(&args, 0, sizeof(args));
	if (size) {
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
	}

	if (argc - optind == 2) {
		args.qgroupid = 0;
		path = argv[optind + 1];
		ret = test_issubvolume(path);
		if (ret < 0) {
			fprintf(stderr, "ERROR: error accessing '%s'\n", path);
			return 1;
		}
		if (!ret) {
			fprintf(stderr, "ERROR: '%s' is not a subvolume\n",
				path);
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
		usage(cmd_qgroup_limit_usage);

	fd = open_file_or_dir(path, &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 1;
	}

	ret = ioctl(fd, BTRFS_IOC_QGROUP_LIMIT, &args);
	e = errno;
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		fprintf(stderr, "ERROR: unable to limit requested quota group: "
			"%s\n", strerror(e));
		return 1;
	}
	return 0;
}

const struct cmd_group qgroup_cmd_group = {
	qgroup_cmd_group_usage, NULL, {
		{ "assign", cmd_qgroup_assign, cmd_qgroup_assign_usage,
		   NULL, 0 },
		{ "remove", cmd_qgroup_remove, cmd_qgroup_remove_usage,
		   NULL, 0 },
		{ "create", cmd_qgroup_create, cmd_qgroup_create_usage,
		   NULL, 0 },
		{ "destroy", cmd_qgroup_destroy, cmd_qgroup_destroy_usage,
		   NULL, 0 },
		{ "show", cmd_qgroup_show, cmd_qgroup_show_usage,
		   NULL, 0 },
		{ "limit", cmd_qgroup_limit, cmd_qgroup_limit_usage,
		   NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_qgroup(int argc, char **argv)
{
	return handle_command_group(&qgroup_cmd_group, argc, argv);
}
