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

#include "kerncompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"
#include "common/utils.h"
#include "common/help.h"
#include "common/open-utils.h"
#include "common/messages.h"
#include "common/string-utils.h"

static void print_usage(void)
{
	printf("usage: btrfs-select-super -s number dev\n");
	printf("\t-s super   copy of superblock to overwrite the primary one (values: 1, 2)\n");
	exit(1);
}

int main(int argc, char **argv)
{
	struct btrfs_root *root;
	int ret;
	u64 num = 0;
	u64 bytenr = 0;

	while(1) {
		int c;
		c = getopt(argc, argv, "s:");
		if (c < 0)
			break;
		switch(c) {
			case 's':
				num = arg_strtou64(optarg);
				if (num >= BTRFS_SUPER_MIRROR_MAX) {
					error("super mirror should be less than: %d",
						BTRFS_SUPER_MIRROR_MAX);
					exit(1);
				}
				bytenr = btrfs_sb_offset(((int)num));
				break;
			default:
				print_usage();
		}
	}
	set_argv0(argv);
	if (check_argc_exact(argc - optind, 1))
		return 1;

	if (bytenr == 0) {
		error("please select the super copy with -s");
		print_usage();
	}

	if((ret = check_mounted(argv[optind])) < 0) {
		errno = -ret;
		error("cannot check mount status: %m");
		return ret;
	} else if(ret) {
		error("%s is currently mounted, aborting", argv[optind]);
		return -EBUSY;
	}

	root = open_ctree(argv[optind], bytenr, 1);

	if (!root) {
		error("open ctree failed");
		return 1;
	}

	/* make the super writing code think we've read the first super */
	root->fs_info->super_bytenr = BTRFS_SUPER_INFO_OFFSET;
	ret = write_all_supers(root->fs_info);

	/* we don't close the ctree or anything, because we don't want a real
	 * transaction commit.  We just want the super copy we pulled off the
	 * disk to overwrite all the other copies
	 */
	printf("using SB copy %llu, bytenr %llu\n", num, bytenr);
	close_ctree(root);
	btrfs_close_all_devices();
	return ret;
}
