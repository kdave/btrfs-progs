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

static const char * const qgroup_cmd_group_usage[] = {
	"btrfs qgroup <command> [options] <path>",
	NULL
};

static u64 parse_qgroupid(char *p)
{
	char *s = strchr(p, '/');
	u64 level;
	u64 id;

	if (!s)
		return atoll(p);
	level = atoll(p);
	id = atoll(s + 1);

	return (level << 48) | id;
}

static int qgroup_assign(int assign, int argc, char **argv)
{
	int ret = 0;
	int fd;
	int e;
	char *path = argv[3];
	struct btrfs_ioctl_qgroup_assign_args args;

	if (check_argc_exact(argc, 4))
		return -1;

	memset(&args, 0, sizeof(args));
	args.assign = assign;
	args.src = parse_qgroupid(argv[1]);
	args.dst = parse_qgroupid(argv[2]);

	/*
	 * FIXME src should accept subvol path
	 */
	if (args.src >= args.dst) {
		fprintf(stderr, "ERROR: bad relation requested '%s'\n", path);
		return 12;
	}
	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 12;
	}

	ret = ioctl(fd, BTRFS_IOC_QGROUP_ASSIGN, &args);
	e = errno;
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "ERROR: unable to assign quota group: %s\n",
			strerror(e));
		return 30;
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

	if (check_argc_exact(argc, 3))
		return -1;

	memset(&args, 0, sizeof(args));
	args.create = create;
	args.qgroupid = parse_qgroupid(argv[1]);

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 12;
	}

	ret = ioctl(fd, BTRFS_IOC_QGROUP_CREATE, &args);
	e = errno;
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "ERROR: unable to create quota group: %s\n",
			strerror(e));
		return 30;
	}
	return 0;
}

void print_qgroup_info(u64 objectid, struct btrfs_qgroup_info_item *info)
{
	printf("%llu/%llu %lld %lld\n", objectid >> 48,
		objectid & ((1ll << 48) - 1),
		btrfs_stack_qgroup_info_referenced(info),
		btrfs_stack_qgroup_info_exclusive(info));
}

int list_qgroups(int fd)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	unsigned long off = 0;
	unsigned int i;
	int e;
	struct btrfs_qgroup_info_item *info;

	memset(&args, 0, sizeof(args));

	/* search in the quota tree */
	sk->tree_id = BTRFS_QUOTA_TREE_OBJECTID;

	/*
	 * set the min and max to backref keys.  The search will
	 * only send back this type of key now.
	 */
	sk->max_type = BTRFS_QGROUP_INFO_KEY;
	sk->min_type = BTRFS_QGROUP_INFO_KEY;
	sk->max_objectid = 0;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;

	/* just a big number, doesn't matter much */
	sk->nr_items = 4096;

	while (1) {
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		e = errno;
		if (ret < 0) {
			fprintf(stderr,
				"ERROR: can't perform the search - %s\n",
				strerror(e));
			return ret;
		}
		/* the ioctl returns the number of item it found in nr_items */
		if (sk->nr_items == 0)
			break;

		off = 0;

		/*
		 * for each item, pull the key out of the header and then
		 * read the root_ref item it contains
		 */
		for (i = 0; i < sk->nr_items; i++) {
			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);
			off += sizeof(*sh);

			if (sh->objectid != 0)
				goto done;

			if (sh->type != BTRFS_QGROUP_INFO_KEY)
				goto done;

			info = (struct btrfs_qgroup_info_item *)
					(args.buf + off);
			print_qgroup_info(sh->offset, info);

			off += sh->len;

			/*
			 * record the mins in sk so we can make sure the
			 * next search doesn't repeat this root
			 */
			sk->min_offset = sh->offset;
		}
		sk->nr_items = 4096;
		/*
		 * this iteration is done, step forward one qgroup for the next
		 * ioctl
		 */
		if (sk->min_offset < (u64)-1)
			sk->min_offset++;
		else
			break;
	}

done:
	return ret;
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
	case 'G':
	case 'g':
		size *= 1024;
	case 'M':
	case 'm':
		size *= 1024;
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
	"btrfs qgroup show <path>",
	"Show all subvolume quota groups.",
	NULL
};

static int cmd_qgroup_show(int argc, char **argv)
{
	int ret = 0;
	int fd;
	char *path = argv[1];

	if (check_argc_exact(argc, 2))
		usage(cmd_qgroup_show_usage);

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 12;
	}

	ret = list_qgroups(fd);
	if (ret < 0) {
		fprintf(stderr, "ERROR: can't list qgroups\n");
		return 30;
	}

	close(fd);

	return ret;
}

static const char * const cmd_qgroup_limit_usage[] = {
	"btrfs qgroup limit [options] <size>|none [<qgroupid>] <path>",
	"Limit the size of a subvolume quota group.",
	"",
	"-c   limit amount of data after compression",
	"-e   limit space exclusively assigned to this qgroup",
	NULL
};

static int cmd_qgroup_limit(int argc, char **argv)
{
	int ret = 0;
	int fd;
	int e;
	char *path;
	struct btrfs_ioctl_qgroup_limit_args args;
	unsigned long long size;
	int compressed = 0;
	int exclusive = 0;

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
	args.qgroupid = parse_qgroupid(argv[optind + 1]);
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

	if (args.qgroupid == 0) {
		if (check_argc_exact(argc - optind, 2))
			usage(cmd_qgroup_limit_usage);
		path = argv[optind + 1];
		ret = test_issubvolume(path);
		if (ret < 0) {
			fprintf(stderr, "ERROR: error accessing '%s'\n", path);
			return 12;
		}
		if (!ret) {
			fprintf(stderr, "ERROR: '%s' is not a subvolume\n",
				path);
			return 13;
		}
		/*
		 * keep qgroupid at 0, this indicates that the subvolume the
		 * fd refers to is to be limited
		 */
	} else {
		if (check_argc_exact(argc - optind, 3))
			usage(cmd_qgroup_limit_usage);
		path = argv[optind + 2];
	}

	fd = open_file_or_dir(path);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", path);
		return 12;
	}

	ret = ioctl(fd, BTRFS_IOC_QGROUP_LIMIT, &args);
	e = errno;
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "ERROR: unable to limit requested quota group: "
			"%s\n", strerror(e));
		return 30;
	}
	return 0;
}

const struct cmd_group qgroup_cmd_group = {
	qgroup_cmd_group_usage, NULL, {
		{ "assign", cmd_qgroup_assign, cmd_qgroup_assign_usage, 0, 0 },
		{ "remove", cmd_qgroup_remove, cmd_qgroup_remove_usage, 0, 0 },
		{ "create", cmd_qgroup_create, cmd_qgroup_create_usage, 0, 0 },
		{ "destroy", cmd_qgroup_destroy,
		  cmd_qgroup_destroy_usage, 0, 0 },
		{ "show", cmd_qgroup_show, cmd_qgroup_show_usage, 0, 0 },
		{ "limit", cmd_qgroup_limit, cmd_qgroup_limit_usage, 0, 0 },
		{ 0, 0, 0, 0, 0 }
	}
};

int cmd_qgroup(int argc, char **argv)
{
	return handle_command_group(&qgroup_cmd_group, argc, argv);
}
