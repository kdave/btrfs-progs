/*
 * Copyright (C) 2008 Morey Roof.   All rights reserved.
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
#endif /* __CHECKER__ */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <ctype.h>
#include "kerncompat.h"
#include "ctree.h"
#include "utils.h"
#include "version.h"
#include "disk-io.h"
#include "transaction.h"

#define MOUNTED                        1
#define UNMOUNTED                      2
#define GET_LABEL                      3
#define SET_LABEL                      4

static void change_label_unmounted(char *dev, char *nLabel)
{
       struct btrfs_root *root;
       struct btrfs_trans_handle *trans;

       /* Open the super_block at the default location
        * and as read-write.
        */
       root = open_ctree(dev, 0, 1);
       if (!root) /* errors are printed by open_ctree() */
         return;

       trans = btrfs_start_transaction(root, 1);
       strncpy(root->fs_info->super_copy.label, nLabel, BTRFS_LABEL_SIZE);
       root->fs_info->super_copy.label[BTRFS_LABEL_SIZE-1] = 0;
       btrfs_commit_transaction(trans, root);

       /* Now we close it since we are done. */
       close_ctree(root);
}

int get_label_unmounted(char *dev)
{
       struct btrfs_root *root;

       /* Open the super_block at the default location
        * and as read-only.
        */
       root = open_ctree(dev, 0, 0);

       if(!root)
         return -1;

       fprintf(stdout, "%s\n", root->fs_info->super_copy.label);

       /* Now we close it since we are done. */
       close_ctree(root);
       return 0;
}

int get_label(char *btrfs_dev)
{

	int ret;
	ret = check_mounted(btrfs_dev);
	if (ret < 0)
	{
	       fprintf(stderr, "FATAL: error checking %s mount status\n", btrfs_dev);
	       return -1;
	}

	if(ret != 0)
	{
	       fprintf(stderr, "FATAL: the filesystem has to be unmounted\n");
	       return -2;
	}
	ret = get_label_unmounted(btrfs_dev);
	return ret;
}


int set_label(char *btrfs_dev, char *nLabel)
{

	int ret;
	ret = check_mounted(btrfs_dev);
	if (ret < 0)
	{
	       fprintf(stderr, "FATAL: error checking %s mount status\n", btrfs_dev);
	       return -1;
	}

	if(ret != 0)
	{
	       fprintf(stderr, "FATAL: the filesystem has to be unmounted\n");
	       return -2;
	}
	change_label_unmounted(btrfs_dev, nLabel);
	return 0;
}
