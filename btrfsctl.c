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
#include "kerncompat.h"

#ifdef __CHECKER__
#define BLKGETSIZE64 0
#define BTRFS_IOC_SNAP_CREATE 0
#define BTRFS_IOC_ADD_DISK 0
#define BTRFS_VOL_NAME_MAX 255
struct btrfs_ioctl_vol_args { char name[BTRFS_VOL_NAME_MAX]; };
static inline int ioctl(int fd, int define, void *arg) { return 0; }
#endif

void print_usage(void)
{
	printf("usage: btrfsctl [ -s name ] [-d] [-r size] file_or_dir\n");
	printf("\t-d filename defragments one file\n");
	printf("\t-d directory defragments the entire Btree\n");
	printf("\t-s snap_name existing_subvol creates a new snapshot\n");
	printf("\t-s snap_name tree_root creates a new subvolume\n");
	printf("\t-r [+-]size[gkm] resize the FS\n");
	printf("\t-a device scans the device for a Btrfs filesystem\n");
	exit(1);
}

int main(int ac, char **av)
{
	char *fname;
	int fd;
	int ret;
	struct btrfs_ioctl_vol_args args;
	char *name = NULL;
	int i;
	struct stat st;
	DIR *dirstream;
	unsigned long command = 0;
	int len;

	for (i = 1; i < ac - 1; i++) {
		if (strcmp(av[i], "-s") == 0) {
			if (i + 1 >= ac - 1) {
				fprintf(stderr, "-s requires an arg");
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
			command = BTRFS_IOC_SNAP_CREATE;
		} else if (strcmp(av[i], "-d") == 0) {
			if (i >= ac - 1) {
				fprintf(stderr, "-d requires an arg\n");
				print_usage();
			}
			command = BTRFS_IOC_DEFRAG;
		} else if (strcmp(av[i], "-a") == 0) {
			if (i >= ac - 1) {
				fprintf(stderr, "-a requires an arg\n");
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
		}
	}
	if (command == 0) {
		fprintf(stderr, "no valid commands given\n");
		exit(1);
	}
	fname = av[ac - 1];
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
	} else if (command == BTRFS_IOC_SCAN_DEV) {
		fd = open("/dev/btrfs-control", O_RDWR);
		printf("scanning %s command %lu\n", fname, BTRFS_IOC_SCAN_DEV);
		name = fname;
	} else {
		fd = open(fname, O_RDWR);
	}
	if (fd < 0) {
		perror("open");
		exit(1);
	}
	if (name)
		strcpy(args.name, name);
	else
		args.name[0] = '\0';
	ret = ioctl(fd, command, &args);
	printf("ioctl returns %d\n", ret);
	return 0;
}

