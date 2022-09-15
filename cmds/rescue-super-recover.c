/*
 * Copyright (C) 2013 FUJITSU LIMITED.  All rights reserved.
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

#include "kerncompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include "kernel-lib/list.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "common/utils.h"
#include "cmds/rescue.h"

struct btrfs_recover_superblock {
	struct btrfs_fs_devices *fs_devices;

	struct list_head good_supers;
	struct list_head bad_supers;

	u64 max_generation;
};

struct super_block_record {
	struct list_head list;

	char *device_name;
	struct btrfs_super_block sb;

	u64 bytenr;
};

static
void init_recover_superblock(struct btrfs_recover_superblock *recover)
{
	INIT_LIST_HEAD(&recover->good_supers);
	INIT_LIST_HEAD(&recover->bad_supers);

	recover->fs_devices = NULL;
	recover->max_generation = 0;
}

static
void free_recover_superblock(struct btrfs_recover_superblock *recover)
{
	struct super_block_record *record;

	if (!recover->fs_devices)
		return;

	while (!list_empty(&recover->good_supers)) {
		record = list_entry(recover->good_supers.next,
				struct super_block_record, list);
		list_del_init(&record->list);
		free(record->device_name);
		free(record);
	}

	while (!list_empty(&recover->bad_supers)) {
		record = list_entry(recover->bad_supers.next,
				struct super_block_record, list);
		list_del_init(&record->list);
		free(record->device_name);
		free(record);
	}
}

static int add_superblock_record(struct btrfs_super_block *sb, char *fname,
			u64 bytenr, struct list_head *head)
{
	struct super_block_record *record;

	record = malloc(sizeof(struct super_block_record));
	if (!record)
		return -ENOMEM;

	record->device_name = strdup(fname);
	if (!record->device_name) {
		free(record);
		return -ENOMEM;
	}
	memcpy(&record->sb, sb, sizeof(*sb));
	record->bytenr = bytenr;
	list_add_tail(&record->list, head);

	return 0;
}

static int
read_dev_supers(char *filename, struct btrfs_recover_superblock *recover)
{
	int i, ret, fd;
	u64 max_gen, bytenr;
	struct btrfs_super_block sb;

	/* just ignore errno that were set in btrfs_scan_fs_devices() */
	errno = 0;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -errno;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);

		ret = btrfs_read_dev_super(fd, &sb, bytenr, SBREAD_DEFAULT);
		if (!ret) {
			ret = add_superblock_record(&sb, filename, bytenr,
							&recover->good_supers);
			if (ret)
				goto out;
			max_gen = btrfs_super_generation(&sb);
			if (max_gen > recover->max_generation)
				recover->max_generation = max_gen;
		} else if (ret != -ENOENT){
			/*
			 * Skip superblock which doesn't exist, only adds
			 * really corrupted superblock
			 */
			ret = add_superblock_record(&sb, filename, bytenr,
						&recover->bad_supers);
			if (ret)
				goto out;
		}
	}
	ret = 0;
out:
	close(fd);
	return ret;
}

static int read_fs_supers(struct btrfs_recover_superblock *recover)
{
	struct super_block_record *record;
	struct super_block_record *next_record;
	struct btrfs_device *dev;
	int ret;
	u64 gen;

	list_for_each_entry(dev, &recover->fs_devices->devices,
				dev_list) {
		ret = read_dev_supers(dev->name, recover);
		if (ret)
			return ret;
	}
	list_for_each_entry_safe(record, next_record,
			&recover->good_supers, list) {
		gen = btrfs_super_generation(&record->sb);
		if (gen < recover->max_generation)
			list_move_tail(&record->list, &recover->bad_supers);
	}

	return 0;
}

static void print_super_info(struct super_block_record *record)
{
	printf("\t\tdevice name = %s\n", record->device_name);
	printf("\t\tsuperblock bytenr = %llu\n", record->bytenr);
}

static void print_all_supers(struct btrfs_recover_superblock *recover)
{
	struct super_block_record *record;

	printf("\t[All good supers]:\n");
	list_for_each_entry(record, &recover->good_supers, list) {
		print_super_info(record);
		printf("\n");
	}

	printf("\t[All bad supers]:\n");
	list_for_each_entry(record, &recover->bad_supers, list) {
		print_super_info(record);
		printf("\n");
	}
	printf("\n");
}

static void recover_err_str(int ret)
{
	switch (ret) {
	case 0:
		printf("All supers are valid, no need to recover\n");
		break;
	case 1:
		printf("Usage or syntax errors\n");
		break;
	case 2:
		printf("Recovered bad superblocks successful\n");
		break;
	case 3:
		printf("Failed to recover bad superblocks\n");
		break;
	case 4:
		printf("Aborted to recover bad superblocks\n");
		break;
	default:
		printf("Unknown recover result\n");
		break;
	}
}

int btrfs_recover_superblocks(const char *dname, int yes)
{
	int fd, ret;
	struct btrfs_recover_superblock recover;
	struct super_block_record *record;
	struct btrfs_root *root = NULL;

	fd = open(dname, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s error\n", dname);
		return 1;
	}
	init_recover_superblock(&recover);

	ret = btrfs_scan_fs_devices(fd, dname, &recover.fs_devices, 0,
			SBREAD_RECOVER, 0);
	close(fd);
	if (ret) {
		ret = 1;
		goto no_recover;
	}

	if (bconf.verbose > BTRFS_BCONF_QUIET)
		print_all_devices(&recover.fs_devices->devices);

	ret = read_fs_supers(&recover);
	if (ret) {
		ret = 1;
		goto no_recover;
	}
	if (bconf.verbose > BTRFS_BCONF_QUIET) {
		printf("Before Recovering:\n");
		print_all_supers(&recover);
	}

	if (list_empty(&recover.bad_supers))
		goto no_recover;

	if (!yes) {
		ret = ask_user("Make sure this is a btrfs disk otherwise the tool will destroy other fs, Are you sure?");
		if (!ret) {
			ret = 4;
			goto no_recover;
		}
	}
	record = list_first_entry(&recover.good_supers,
				  struct super_block_record, list);

	root = open_ctree(record->device_name, record->bytenr,
			  OPEN_CTREE_RECOVER_SUPER | OPEN_CTREE_WRITES);
	if (!root) {
		ret = 3;
		goto no_recover;
	}
	/* reset super_bytenr in order that we will rewrite all supers */
	root->fs_info->super_bytenr = BTRFS_SUPER_INFO_OFFSET;
	ret = write_all_supers(root->fs_info);
	if (!ret)
		ret = 2;
	else
		ret = 3;

	close_ctree(root);
no_recover:
	recover_err_str(ret);
	free_recover_superblock(&recover);
	return ret;
}

