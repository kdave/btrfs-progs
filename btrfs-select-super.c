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

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"
#include "utils.h"

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfs-select-super -s number dev\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

int main(int ac, char **av)
{
	struct btrfs_root *root;
	int ret;
	u64 num = 0;
	u64 bytenr = 0;

	while(1) {
		int c;
		c = getopt(ac, av, "s:");
		if (c < 0)
			break;
		switch(c) {
			case 's':
				num = arg_strtou64(optarg);
				if (num >= BTRFS_SUPER_MIRROR_MAX) {
					fprintf(stderr,
						"ERROR: super mirror should be less than: %d\n",
						BTRFS_SUPER_MIRROR_MAX);
					exit(1);
				}
				bytenr = btrfs_sb_offset(((int)num));
				break;
			default:
				print_usage();
		}
	}
	ac = ac - optind;

	if (ac != 1)
		print_usage();

	if (bytenr == 0) {
		fprintf(stderr, "Please select the super copy with -s\n");
		print_usage();
	}

	radix_tree_init();

	if((ret = check_mounted(av[optind])) < 0) {
		fprintf(stderr, "Could not check mount status: %s\n", strerror(-ret));
		return ret;
	} else if(ret) {
		fprintf(stderr, "%s is currently mounted. Aborting.\n", av[optind]);
		return -EBUSY;
	}

	root = open_ctree(av[optind], bytenr, 1);

	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		return 1;
	}

	/* make the super writing code think we've read the first super */
	root->fs_info->super_bytenr = BTRFS_SUPER_INFO_OFFSET;
	ret = write_all_supers(root);

	/* we don't close the ctree or anything, because we don't want a real
	 * transaction commit.  We just want the super copy we pulled off the
	 * disk to overwrite all the other copies
	 */ 
	printf("using SB copy %d, bytenr %llu\n", num,
	       (unsigned long long)bytenr);
	return ret;
}
