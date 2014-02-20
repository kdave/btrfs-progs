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
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "kerncompat.h"
#include "ioctl.h"
#include "utils.h"
#include "ctree.h"
#include "send-utils.h"

#include "commands.h"
#include "btrfs-list.h"

static const char * const inspect_cmd_group_usage[] = {
	"btrfs inspect-internal <command> <args>",
	NULL
};

static int __ino_to_path_fd(u64 inum, int fd, int verbose, const char *prepend)
{
	int ret;
	int i;
	struct btrfs_ioctl_ino_path_args ipa;
	struct btrfs_data_container *fspath;

	fspath = malloc(4096);
	if (!fspath)
		return -ENOMEM;

	memset(fspath, 0, sizeof(*fspath));
	ipa.inum = inum;
	ipa.size = 4096;
	ipa.fspath = (uintptr_t)fspath;

	ret = ioctl(fd, BTRFS_IOC_INO_PATHS, &ipa);
	if (ret) {
		printf("ioctl ret=%d, error: %s\n", ret, strerror(errno));
		goto out;
	}

	if (verbose)
		printf("ioctl ret=%d, bytes_left=%lu, bytes_missing=%lu, "
			"cnt=%d, missed=%d\n", ret,
			(unsigned long)fspath->bytes_left,
			(unsigned long)fspath->bytes_missing,
			fspath->elem_cnt, fspath->elem_missed);

	for (i = 0; i < fspath->elem_cnt; ++i) {
		u64 ptr;
		char *str;
		ptr = (u64)(unsigned long)fspath->val;
		ptr += fspath->val[i];
		str = (char *)(unsigned long)ptr;
		if (prepend)
			printf("%s/%s\n", prepend, str);
		else
			printf("%s\n", str);
	}

out:
	free(fspath);
	return !!ret;
}

static const char * const cmd_inode_resolve_usage[] = {
	"btrfs inspect-internal inode-resolve [-v] <inode> <path>",
	"Get file system paths for the given inode",
	"",
	"-v   verbose mode",
	NULL
};

static int cmd_inode_resolve(int argc, char **argv)
{
	int fd;
	int verbose = 0;
	int ret;
	DIR *dirstream = NULL;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "v");
		if (c < 0)
			break;

		switch (c) {
		case 'v':
			verbose = 1;
			break;
		default:
			usage(cmd_inode_resolve_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_inode_resolve_usage);

	fd = open_file_or_dir(argv[optind+1], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind+1]);
		return 1;
	}

	ret = __ino_to_path_fd(arg_strtou64(argv[optind]), fd, verbose,
			       argv[optind+1]);
	close_file_or_dir(fd, dirstream);
	return !!ret;

}

static const char * const cmd_logical_resolve_usage[] = {
	"btrfs inspect-internal logical-resolve [-Pv] [-s bufsize] <logical> <path>",
	"Get file system paths for the given logical address",
	"-P          skip the path resolving and print the inodes instead",
	"-v          verbose mode",
	"-s bufsize  set inode container's size. This is used to increase inode",
	"            container's size in case it is not enough to read all the ",
	"            resolved results. The max value one can set is 64k",
	NULL
};

static int cmd_logical_resolve(int argc, char **argv)
{
	int ret;
	int fd;
	int i;
	int verbose = 0;
	int getpath = 1;
	int bytes_left;
	struct btrfs_ioctl_logical_ino_args loi;
	struct btrfs_data_container *inodes;
	u64 size = 4096;
	char full_path[4096];
	char *path_ptr;
	DIR *dirstream = NULL;

	optind = 1;
	while (1) {
		int c = getopt(argc, argv, "Pvs:");
		if (c < 0)
			break;

		switch (c) {
		case 'P':
			getpath = 0;
			break;
		case 'v':
			verbose = 1;
			break;
		case 's':
			size = arg_strtou64(optarg);
			break;
		default:
			usage(cmd_logical_resolve_usage);
		}
	}

	if (check_argc_exact(argc - optind, 2))
		usage(cmd_logical_resolve_usage);

	size = min(size, (u64)64 * 1024);
	inodes = malloc(size);
	if (!inodes)
		return 1;

	memset(inodes, 0, sizeof(*inodes));
	loi.logical = arg_strtou64(argv[optind]);
	loi.size = size;
	loi.inodes = (uintptr_t)inodes;

	fd = open_file_or_dir(argv[optind+1], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind+1]);
		ret = 12;
		goto out;
	}

	ret = ioctl(fd, BTRFS_IOC_LOGICAL_INO, &loi);
	if (ret) {
		printf("ioctl ret=%d, error: %s\n", ret, strerror(errno));
		goto out;
	}

	if (verbose)
		printf("ioctl ret=%d, total_size=%llu, bytes_left=%lu, "
			"bytes_missing=%lu, cnt=%d, missed=%d\n",
			ret, size,
			(unsigned long)inodes->bytes_left,
			(unsigned long)inodes->bytes_missing,
			inodes->elem_cnt, inodes->elem_missed);

	bytes_left = sizeof(full_path);
	ret = snprintf(full_path, bytes_left, "%s/", argv[optind+1]);
	path_ptr = full_path + ret;
	bytes_left -= ret + 1;
	BUG_ON(bytes_left < 0);

	for (i = 0; i < inodes->elem_cnt; i += 3) {
		u64 inum = inodes->val[i];
		u64 offset = inodes->val[i+1];
		u64 root = inodes->val[i+2];
		int path_fd;
		char *name;
		DIR *dirs = NULL;

		if (getpath) {
			name = btrfs_list_path_for_root(fd, root);
			if (IS_ERR(name)) {
				ret = PTR_ERR(name);
				goto out;
			}
			if (!name) {
				path_ptr[-1] = '\0';
				path_fd = fd;
			} else {
				path_ptr[-1] = '/';
				ret = snprintf(path_ptr, bytes_left, "%s",
						name);
				BUG_ON(ret >= bytes_left);
				free(name);
				path_fd = open_file_or_dir(full_path, &dirs);
				if (path_fd < 0) {
					fprintf(stderr, "ERROR: can't access "
						"'%s'\n", full_path);
					goto out;
				}
			}
			__ino_to_path_fd(inum, path_fd, verbose, full_path);
			if (path_fd != fd)
				close_file_or_dir(path_fd, dirs);
		} else {
			printf("inode %llu offset %llu root %llu\n", inum,
				offset, root);
		}
	}

out:
	close_file_or_dir(fd, dirstream);
	free(inodes);
	return !!ret;
}

static const char * const cmd_subvolid_resolve_usage[] = {
	"btrfs inspect-internal subvolid-resolve <subvolid> <path>",
	"Get file system paths for the given subvolume ID.",
	NULL
};

static int cmd_subvolid_resolve(int argc, char **argv)
{
	int ret;
	int fd = -1;
	u64 subvol_id;
	char path[BTRFS_PATH_NAME_MAX + 1];
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 3))
		usage(cmd_subvolid_resolve_usage);

	fd = open_file_or_dir(argv[2], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[2]);
		ret = -ENOENT;
		goto out;
	}

	subvol_id = arg_strtou64(argv[1]);
	ret = btrfs_subvolid_resolve(fd, path, sizeof(path), subvol_id);

	if (ret) {
		fprintf(stderr,
			"%s: btrfs_subvolid_resolve(subvol_id %llu) failed with ret=%d\n",
			argv[0], (unsigned long long)subvol_id, ret);
		goto out;
	}

	path[BTRFS_PATH_NAME_MAX] = '\0';
	printf("%s\n", path);

out:
	close_file_or_dir(fd, dirstream);
	return ret ? 1 : 0;
}

static const char* const cmd_rootid_usage[] = {
	"btrfs inspect-internal rootid <path>",
	"Get tree ID of the containing subvolume of path.",
	NULL
};

static int cmd_rootid(int argc, char **argv)
{
	int ret;
	int fd = -1;
	u64 rootid;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc, 2))
		usage(cmd_rootid_usage);

	fd = open_file_or_dir(argv[1], &dirstream);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[1]);
		ret = -ENOENT;
		goto out;
	}

	ret = lookup_ino_rootid(fd, &rootid);
	if (ret) {
		fprintf(stderr, "%s: rootid failed with ret=%d\n",
			argv[0], ret);
		goto out;
	}

	printf("%llu\n", (unsigned long long)rootid);
out:
	close_file_or_dir(fd, dirstream);

	return !!ret;
}

const struct cmd_group inspect_cmd_group = {
	inspect_cmd_group_usage, NULL, {
		{ "inode-resolve", cmd_inode_resolve, cmd_inode_resolve_usage,
			NULL, 0 },
		{ "logical-resolve", cmd_logical_resolve,
			cmd_logical_resolve_usage, NULL, 0 },
		{ "subvolid-resolve", cmd_subvolid_resolve,
			cmd_subvolid_resolve_usage, NULL, 0 },
		{ "rootid", cmd_rootid, cmd_rootid_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_inspect(int argc, char **argv)
{
	return handle_command_group(&inspect_cmd_group, argc, argv);
}
