/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
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

#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <stdlib.h>
#include "kerncompat.h"
#include "ctree.h"
#include "transaction.h"
#include "utils.h"
#include "version.h"

#ifdef __CHECKER__
#define BLKGETSIZE64 0
#define BTRFS_IOC_SNAP_CREATE 0
#define BTRFS_VOL_NAME_MAX 255
struct btrfs_ioctl_vol_args { char name[BTRFS_VOL_NAME_MAX]; };
static inline int ioctl(int fd, int define, void *arg) { return 0; }
#endif

static void print_usage(void)
{
	printf("usage: btrfsctl [ -d file|dir] [ -s snap_name subvol|tree ]\n");
	printf("                [-r size] [-A device] [-a] [-c] [-D dir .]\n");
	printf("\t-d filename: defragments one file\n");
	printf("\t-d directory: defragments the entire Btree\n");
	printf("\t-s snap_name dir: creates a new snapshot of dir\n");
	printf("\t-S subvol_name dir: creates a new subvolume\n");
	printf("\t-r [+-]size[gkm]: resize the FS by size amount\n");
	printf("\t-A device: scans the device file for a Btrfs filesystem\n");
	printf("\t-a: scans all devices for Btrfs filesystems\n");
	printf("\t-c: forces a single FS sync\n");
	printf("\t-D: delete snapshot\n");
	printf("\t-m [tree id] directory: set the default mounted subvolume"
	       " to the [tree id] or the directory\n");
	printf("%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

static int open_file_or_dir(const char *fname)
{
	int ret;
	struct stat st;
	DIR *dirstream;
	int fd;

	ret = stat(fname, &st);
	if (ret < 0) {
		perror("stat:");
		exit(1);
	}
	if (S_ISDIR(st.st_mode)) {
		dirstream = opendir(fname);
		if (!dirstream) {
			perror("opendir");
			exit(1);
		}
		fd = dirfd(dirstream);
	} else {
		fd = open(fname, O_RDWR);
	}
	if (fd < 0) {
		perror("open");
		exit(1);
	}
	return fd;
}
int main(int ac, char **av)
{
	char *fname = NULL;
	char *snap_location = NULL;
	int snap_fd = 0;
	int fd;
	int ret;
	struct btrfs_ioctl_vol_args args;
	char *name = NULL;
	int i;
	unsigned long command = 0;
	int len;
	char *pos;
	char *fullpath;
	u64 objectid = 0;

	printf( "**\n"
		"** WARNING: this program is considered deprecated\n"
		"** Please consider to switch to the btrfs utility\n"
		"**\n");
	
	if (ac == 2 && strcmp(av[1], "-a") == 0) {
		fprintf(stderr, "Scanning for Btrfs filesystems\n");
		btrfs_scan_one_dir("/dev", 1);
		exit(0);
	}
	for (i = 1; i < ac; i++) {
		if (strcmp(av[i], "-s") == 0) {
			if (i + 1 >= ac - 1) {
				fprintf(stderr, "-s requires an arg");
				print_usage();
			}
			fullpath = av[i + 1];

			snap_location = strdup(fullpath);
			snap_location = dirname(snap_location);

			snap_fd = open_file_or_dir(snap_location);

			name = strdup(fullpath);
			name = basename(name);
			len = strlen(name);

			if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
				fprintf(stderr,
				     "snapshot name zero length or too long\n");
				exit(1);
			}
			if (strchr(name, '/')) {
				fprintf(stderr,
					"error: / not allowed in names\n");
				exit(1);
			}
			command = BTRFS_IOC_SNAP_CREATE;
		} else if (strcmp(av[i], "-S") == 0) {
			if (i + 1 >= ac - 1) {
				fprintf(stderr, "-S requires an arg");
				print_usage();
			}
			name = av[i + 1];
			len = strlen(name);
			if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
				fprintf(stderr,
				     "snapshot name zero length or too long\n");
				exit(1);
			}
			if (strchr(name, '/')) {
				fprintf(stderr,
					"error: / not allowed in names\n");
				exit(1);
			}
			command = BTRFS_IOC_SUBVOL_CREATE;
		} else if (strcmp(av[i], "-d") == 0) {
			if (i >= ac - 1) {
				fprintf(stderr, "-d requires an arg\n");
				print_usage();
			}
			command = BTRFS_IOC_DEFRAG;
		} else if (strcmp(av[i], "-D") == 0) {
			if (i >= ac - 1) {
				fprintf(stderr, "-D requires an arg\n");
				print_usage();
			}
			command = BTRFS_IOC_SNAP_DESTROY;
			name = av[i + 1];
			len = strlen(name);
			pos = strchr(name, '/');
			if (pos) {
				if (*(pos + 1) == '\0')
					*(pos) = '\0';
				else {
					fprintf(stderr,
						"error: / not allowed in names\n");
					exit(1);
				}
			}
			if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
				fprintf(stderr, "-D size too long\n");
				exit(1);
			}
		} else if (strcmp(av[i], "-A") == 0) {
			if (i >= ac - 1) {
				fprintf(stderr, "-A requires an arg\n");
				print_usage();
			}
			command = BTRFS_IOC_SCAN_DEV;
		} else if (strcmp(av[i], "-r") == 0) {
			if (i >= ac - 1) {
				fprintf(stderr, "-r requires an arg\n");
				print_usage();
			}
			name = av[i + 1];
			len = strlen(name);
			if (len == 0 || len >= BTRFS_VOL_NAME_MAX) {
				fprintf(stderr, "-r size too long\n");
				exit(1);
			}
			command = BTRFS_IOC_RESIZE;
		} else if (strcmp(av[i], "-c") == 0) {
			command = BTRFS_IOC_SYNC;
		} else if (strcmp(av[i], "-m") == 0) {
			command = BTRFS_IOC_DEFAULT_SUBVOL;
			if (i == ac - 3) {
				objectid = (unsigned long long)
					    strtoll(av[i + 1], NULL, 0);
				if (errno == ERANGE) {
					fprintf(stderr, "invalid tree id\n");
					exit(1);
				}
			}
		}
	}
	if (command == 0) {
		fprintf(stderr, "no valid commands given\n");
		print_usage();
		exit(1);
	}
	fname = av[ac - 1];

	if (command == BTRFS_IOC_SCAN_DEV) {
		fd = open("/dev/btrfs-control", O_RDWR);
		if (fd < 0) {
			perror("failed to open /dev/btrfs-control");
			exit(1);
		}
		name = fname;
	 } else {
		fd = open_file_or_dir(fname);
	 }

	if (name) {
                strncpy(args.name, name, BTRFS_PATH_NAME_MAX + 1);
                args.name[BTRFS_PATH_NAME_MAX] = 0;
	} else
		args.name[0] = '\0';

	if (command == BTRFS_IOC_SNAP_CREATE) {
		args.fd = fd;
		ret = ioctl(snap_fd, command, &args);
	} else if (command == BTRFS_IOC_DEFAULT_SUBVOL) {
		printf("objectid is %llu\n", (unsigned long long)objectid);
		ret = ioctl(fd, command, &objectid);
	} else
		ret = ioctl(fd, command, &args);
	if (ret < 0) {
		perror("ioctl:");
		exit(1);
	}
	if (ret == 0) {
		printf("operation complete\n");
	} else {
		printf("ioctl failed with error %d\n", ret);
	}
	printf("%s\n", BTRFS_BUILD_VERSION);
	if (ret)
		exit(1);

	return 0;
}

