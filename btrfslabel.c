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

static int set_label_unmounted(const char *dev, const char *label)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	int ret;

	ret = check_mounted(dev);
	if (ret < 0) {
	       fprintf(stderr, "FATAL: error checking %s mount status\n", dev);
	       return -1;
	}
	if (ret > 0) {
		fprintf(stderr, "ERROR: dev %s is mounted, use mount point\n",
			dev);
		return -1;
	}

	/* Open the super_block at the default location
	 * and as read-write.
	 */
	root = open_ctree(dev, 0, 1);
	if (!root) /* errors are printed by open_ctree() */
		return -1;

	trans = btrfs_start_transaction(root, 1);
	strncpy(root->fs_info->super_copy.label, label, BTRFS_LABEL_SIZE);
	root->fs_info->super_copy.label[BTRFS_LABEL_SIZE-1] = 0;
	btrfs_commit_transaction(trans, root);

	/* Now we close it since we are done. */
	close_ctree(root);
	return 0;
}

static int set_label_mounted(const char *mount_path, const char *label)
{
	int fd;

	fd = open(mount_path, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		fprintf(stderr, "ERROR: unable access to '%s'\n", mount_path);
		return -1;
	}

	if (ioctl(fd, BTRFS_IOC_SET_FSLABEL, label) < 0) {
		fprintf(stderr, "ERROR: unable to set label %s\n",
			strerror(errno));
		close(fd);
		return -1;
	}

	return 0;
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


int set_label(char *btrfs_dev, char *label)
{
	return is_existing_blk_or_reg_file(btrfs_dev) ?
		set_label_unmounted(btrfs_dev, label) :
		set_label_mounted(btrfs_dev, label);
}
