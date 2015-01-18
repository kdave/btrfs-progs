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

static void print_usage(void) __attribute__((noreturn));
static void print_usage(void)
{
	fprintf(stderr, "usage: btrfs-zero-log dev\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

int main(int ac, char **av)
{
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;
	int ret;

	set_argv0(av);
	if (check_argc_exact(ac, 2))
		print_usage();

	radix_tree_init();

	if((ret = check_mounted(av[1])) < 0) {
		fprintf(stderr, "Could not check mount status: %s\n", strerror(-ret));
		goto out;
	} else if(ret) {
		fprintf(stderr, "%s is currently mounted. Aborting.\n", av[1]);
		ret = -EBUSY;
		goto out;
	}

	root = open_ctree(av[1], 0, OPEN_CTREE_WRITES | OPEN_CTREE_PARTIAL);

	if (root == NULL)
		return 1;

	trans = btrfs_start_transaction(root, 1);
	btrfs_set_super_log_root(root->fs_info->super_copy, 0);
	btrfs_set_super_log_root_level(root->fs_info->super_copy, 0);
	btrfs_commit_transaction(trans, root);
	close_ctree(root);
	printf("Log root zero'ed\n");
out:
	return !!ret;
}
