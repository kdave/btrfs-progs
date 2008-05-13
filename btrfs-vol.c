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

#define _GNU_SOURCE
#ifndef __CHECKER__
#include <sys/ioctl.h>
#include <sys/mount.h>
#include "ioctl.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <uuid/uuid.h>
#include "kerncompat.h"
#include "ctree.h"
#include "transaction.h"
#include "utils.h"
#include "volumes.h"

#ifdef __CHECKER__
#define BLKGETSIZE64 0
#define BTRFS_IOC_SNAP_CREATE 0
#define BTRFS_IOC_ADD_DEV 0
#define BTRFS_IOC_RM_DEV 0
#define BTRFS_VOL_NAME_MAX 255
struct btrfs_ioctl_vol_args { char name[BTRFS_VOL_NAME_MAX]; };
static inline int ioctl(int fd, int define, void *arg) { return 0; }
#endif

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfs-vol [options] mount_point\n");
	fprintf(stderr, "\t-a device add one device\n");
	fprintf(stderr, "\t-b balance chunks across all devices\n");
	fprintf(stderr, "\t-r device remove one device\n");
	exit(1);
}

static struct option long_options[] = {
	/* { "byte-count", 1, NULL, 'b' }, */
	{ "add", 1, NULL, 'a' },
	{ "balance", 0, NULL, 'b' },
	{ "remove", 1, NULL, 'r' },
	{ 0, 0, 0, 0}
};

int main(int ac, char **av)
{
	struct stat st;
	char *device = NULL;
	char *mnt = NULL;
	int ret;
	int option_index = 0;
	int cmd = 0;
	int fd;
	int devfd = 0;
	DIR *dirstream;
	struct btrfs_ioctl_vol_args args;
	u64 dev_block_count = 0;

	while(1) {
		int c;
		c = getopt_long(ac, av, "a:br:", long_options,
				&option_index);
		if (c < 0)
			break;
		switch(c) {
			case 'a':
				device = strdup(optarg);
				cmd = BTRFS_IOC_ADD_DEV;
				break;
			case 'b':
				cmd = BTRFS_IOC_BALANCE;
				break;
			case 'r':
				device = strdup(optarg);
				cmd = BTRFS_IOC_RM_DEV;
				break;
			default:
				print_usage();
		}
	}
	ac = ac - optind;
	if (ac == 0)
		print_usage();
	mnt = av[optind];

	if (device && strcmp(device, "missing") == 0 &&
	    cmd == BTRFS_IOC_RM_DEV) {
		fprintf(stderr, "removing missing devices from %s\n", mnt);
	} else if (device) {
		devfd = open(device, O_RDWR);
		if (!devfd) {
			fprintf(stderr, "Unable to open device %s\n", device);
		}
		ret = fstat(devfd, &st);
		if (ret) {
			fprintf(stderr, "Unable to stat %s\n", device);
			exit(1);
		}
		if (!S_ISBLK(st.st_mode)) {
			fprintf(stderr, "%s is not a block device\n", device);
			exit(1);
		}
	}
	dirstream = opendir(mnt);
	if (!dirstream) {
		fprintf(stderr, "Unable to open directory %s\n", mnt);
		exit(1);
	}
	if (cmd == BTRFS_IOC_ADD_DEV) {
		ret = btrfs_prepare_device(devfd, device, 1, &dev_block_count);
		if (ret) {
			fprintf(stderr, "Unable to init %s\n", device);
			exit(1);
		}
	}
	fd = dirfd(dirstream);
	if (device)
		strcpy(args.name, device);
	else
		args.name[0] = '\0';

	ret = ioctl(fd, cmd, &args);
	printf("ioctl returns %d\n", ret);
	return 0;
}

