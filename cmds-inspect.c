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
#include <sys/ioctl.h>
#include <errno.h>

#include "kerncompat.h"
#include "ioctl.h"

#include "btrfs_cmds.h"

/* btrfs-list.c */
char *path_for_root(int fd, u64 root);

static int __ino_to_path_fd(u64 inum, int fd, int verbose, const char *prepend)
{
	int ret;
	int i;
	struct btrfs_ioctl_ino_path_args ipa;
	struct btrfs_data_container *fspath;

	fspath = malloc(4096);
	if (!fspath)
		return 1;

	ipa.inum = inum;
	ipa.size = 4096;
	ipa.fspath = (u64)fspath;

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
		char **str = (char **)fspath->val;
		str[i] += (unsigned long)fspath->val;
		if (prepend)
			printf("%s/%s\n", prepend, str[i]);
		else
			printf("%s\n", str[i]);
	}

out:
	free(fspath);
	return ret;
}

int do_ino_to_path(int nargs, char **argv)
{
	int fd;
	int verbose = 0;

	optind = 1;
	while (1) {
		int c = getopt(nargs, argv, "v");
		if (c < 0)
			break;
		switch (c) {
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "invalid arguments for ipath\n");
			return 1;
		}
	}
	if (nargs - optind != 2) {
		fprintf(stderr, "invalid arguments for ipath\n");
		return 1;
	}

	fd = open_file_or_dir(argv[optind+1]);
	if (fd < 0) {
		fprintf(stderr, "ERROR: can't access '%s'\n", argv[optind+1]);
		return 12;
	}

	return __ino_to_path_fd(atoll(argv[optind]), fd, verbose,
				argv[optind+1]);
}

int do_logical_to_ino(int nargs, char **argv)
{
	int ret;
	int fd;
	int i;
	int verbose = 0;
	int getpath = 1;
	int bytes_left;
	struct btrfs_ioctl_logical_ino_args loi;
	struct btrfs_data_container *inodes;
	char full_path[4096];
	char *path_ptr;

	optind = 1;
	while (1) {
		int c = getopt(nargs, argv, "Pv");
		if (c < 0)
			break;
		switch (c) {
		case 'P':
			getpath = 0;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "invalid arguments for ipath\n");
			return 1;
		}
	}
	if (nargs - optind != 2) {
		fprintf(stderr, "invalid arguments for ipath\n");
		return 1;
	}

	inodes = malloc(4096);
	if (!inodes)
		return 1;

	loi.logical = atoll(argv[optind]);
	loi.size = 4096;
	loi.inodes = (u64)inodes;

	fd = open_file_or_dir(argv[optind+1]);
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
		printf("ioctl ret=%d, bytes_left=%lu, bytes_missing=%lu, "
			"cnt=%d, missed=%d\n", ret,
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

		if (getpath) {
			name = path_for_root(fd, root);
			if (IS_ERR(name))
				return PTR_ERR(name);
			if (!name) {
				path_ptr[-1] = '\0';
				path_fd = fd;
			} else {
				path_ptr[-1] = '/';
				ret = snprintf(path_ptr, bytes_left, "%s",
						name);
				BUG_ON(ret >= bytes_left);
				free(name);
				path_fd = open_file_or_dir(full_path);
				if (path_fd < 0) {
					fprintf(stderr, "ERROR: can't access "
						"'%s'\n", full_path);
					goto out;
				}
			}
			__ino_to_path_fd(inum, path_fd, verbose, full_path);
		} else {
			printf("inode %llu offset %llu root %llu\n", inum,
				offset, root);
		}
	}

out:
	free(inodes);
	return ret;
}

