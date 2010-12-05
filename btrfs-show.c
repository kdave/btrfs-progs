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
#include "version.h"

static int uuid_search(struct btrfs_fs_devices *fs_devices, char *search)
{
	struct list_head *cur;
	struct btrfs_device *device;

	list_for_each(cur, &fs_devices->devices) {
		device = list_entry(cur, struct btrfs_device, dev_list);
		if ((device->label && strcmp(device->label, search) == 0) ||
		    strcmp(device->name, search) == 0)
			return 1;
	}
	return 0;
}

static void print_one_uuid(struct btrfs_fs_devices *fs_devices)
{
	char uuidbuf[37];
	struct list_head *cur;
	struct btrfs_device *device;
	char *super_bytes_used;
	u64 devs_found = 0;
	u64 total;

	uuid_unparse(fs_devices->fsid, uuidbuf);
	device = list_entry(fs_devices->devices.next, struct btrfs_device,
			    dev_list);
	if (device->label && device->label[0])
		printf("Label: %s ", device->label);
	else
		printf("Label: none ");

	super_bytes_used = pretty_sizes(device->super_bytes_used);

	total = device->total_devs;
	printf(" uuid: %s\n\tTotal devices %llu FS bytes used %s\n", uuidbuf,
	       (unsigned long long)total, super_bytes_used);

	free(super_bytes_used);

	list_for_each(cur, &fs_devices->devices) {
		char *total_bytes;
		char *bytes_used;
		device = list_entry(cur, struct btrfs_device, dev_list);
		total_bytes = pretty_sizes(device->total_bytes);
		bytes_used = pretty_sizes(device->bytes_used);
		printf("\tdevid %4llu size %s used %s path %s\n",
		       (unsigned long long)device->devid,
		       total_bytes, bytes_used, device->name);
		free(total_bytes);
		free(bytes_used);
		devs_found++;
	}
	if (devs_found < total) {
		printf("\t*** Some devices missing\n");
	}
	printf("\n");
}

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfs-show [search label or device]\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

static struct option long_options[] = {
	/* { "byte-count", 1, NULL, 'b' }, */
	{ 0, 0, 0, 0}
};

int main(int ac, char **av)
{
	struct list_head *all_uuids;
	struct btrfs_fs_devices *fs_devices;
	struct list_head *cur_uuid;
	char *search = NULL;
	int ret;
	int option_index = 0;

	printf( "**\n"
		"** WARNING: this program is considered deprecated\n"
		"** Please consider to switch to the btrfs utility\n"
		"**\n");

	while(1) {
		int c;
		c = getopt_long(ac, av, "", long_options,
				&option_index);
		if (c < 0)
			break;
		switch(c) {
			default:
				print_usage();
		}
	}
	ac = ac - optind;
	if (ac != 0) {
		search = av[optind];
	}

	ret = btrfs_scan_one_dir("/dev", 0);
	if (ret)
		fprintf(stderr, "error %d while scanning\n", ret);

	all_uuids = btrfs_scanned_uuids();
	list_for_each(cur_uuid, all_uuids) {
		fs_devices = list_entry(cur_uuid, struct btrfs_fs_devices,
					list);
		if (search && uuid_search(fs_devices, search) == 0)
			continue;
		print_one_uuid(fs_devices);
	}
	printf("%s\n", BTRFS_BUILD_VERSION);
	return 0;
}

