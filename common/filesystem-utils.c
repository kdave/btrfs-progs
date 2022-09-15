/*
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
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "common/filesystem-utils.h"
#include "common/messages.h"
#include "common/open-utils.h"
#include "common/path-utils.h"

/*
 * For a given:
 * - file or directory return the containing tree root id
 * - subvolume return its own tree id
 * - BTRFS_EMPTY_SUBVOL_DIR_OBJECTID (directory with ino == 2) the result is
 *   undefined and function returns -1
 */
int lookup_path_rootid(int fd, u64 *rootid)
{
	struct btrfs_ioctl_ino_lookup_args args;
	int ret;

	memset(&args, 0, sizeof(args));
	args.treeid = 0;
	args.objectid = BTRFS_FIRST_FREE_OBJECTID;

	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &args);
	if (ret < 0)
		return -errno;

	*rootid = args.treeid;

	return 0;
}

/*
 * Checks to make sure that the label matches our requirements.
 * Returns:
       0    if everything is safe and usable
      -1    if the label is too long
 */
static int check_label(const char *input)
{
       int len = strlen(input);

       if (len > BTRFS_LABEL_SIZE - 1) {
		error("label %s is too long (max %d)", input,
				BTRFS_LABEL_SIZE - 1);
               return -1;
       }

       return 0;
}

static int set_label_unmounted(const char *dev, const char *label)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	int ret;

	ret = check_mounted(dev);
	if (ret < 0) {
	       error("checking mount status of %s failed: %d", dev, ret);
	       return -1;
	}
	if (ret > 0) {
		error("device %s is mounted, use mount point", dev);
		return -1;
	}

	/* Open the super_block at the default location
	 * and as read-write.
	 */
	root = open_ctree(dev, 0, OPEN_CTREE_WRITES);
	if (!root) /* errors are printed by open_ctree() */
		return -1;

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	__strncpy_null(root->fs_info->super_copy->label, label, BTRFS_LABEL_SIZE - 1);

	btrfs_commit_transaction(trans, root);

	/* Now we close it since we are done. */
	close_ctree(root);
	return 0;
}

static int set_label_mounted(const char *mount_path, const char *labelp)
{
	int fd;
	char label[BTRFS_LABEL_SIZE];

	fd = open(mount_path, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		error("unable to access %s: %m", mount_path);
		return -1;
	}

	memset(label, 0, sizeof(label));
	__strncpy_null(label, labelp, BTRFS_LABEL_SIZE - 1);
	if (ioctl(fd, BTRFS_IOC_SET_FSLABEL, label) < 0) {
		error("unable to set label of %s: %m", mount_path);
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

int get_label_unmounted(const char *dev, char *label)
{
	struct btrfs_root *root;
	int ret;

	ret = check_mounted(dev);
	if (ret < 0) {
	       error("checking mount status of %s failed: %d", dev, ret);
	       return -1;
	}

	/* Open the super_block at the default location
	 * and as read-only.
	 */
	root = open_ctree(dev, 0, 0);
	if(!root)
		return -1;

	__strncpy_null(label, root->fs_info->super_copy->label,
			BTRFS_LABEL_SIZE - 1);

	/* Now we close it since we are done. */
	close_ctree(root);
	return 0;
}

/*
 * If a partition is mounted, try to get the filesystem label via its
 * mounted path rather than device.  Return the corresponding error
 * the user specified the device path.
 */
int get_label_mounted(const char *mount_path, char *labelp)
{
	char label[BTRFS_LABEL_SIZE];
	int fd;
	int ret;

	fd = open(mount_path, O_RDONLY | O_NOATIME);
	if (fd < 0) {
		error("unable to access %s: %m", mount_path);
		return -1;
	}

	memset(label, '\0', sizeof(label));
	ret = ioctl(fd, BTRFS_IOC_GET_FSLABEL, label);
	if (ret < 0) {
		if (errno != ENOTTY)
			error("unable to get label of %s: %m", mount_path);
		ret = -errno;
		close(fd);
		return ret;
	}

	__strncpy_null(labelp, label, BTRFS_LABEL_SIZE - 1);
	close(fd);
	return 0;
}

int get_label(const char *btrfs_dev, char *label)
{
	int ret;

	ret = path_is_reg_or_block_device(btrfs_dev);
	if (!ret)
		ret = get_label_mounted(btrfs_dev, label);
	else if (ret > 0)
		ret = get_label_unmounted(btrfs_dev, label);

	return ret;
}

int set_label(const char *btrfs_dev, const char *label)
{
	int ret;

	if (check_label(label))
		return -1;

	ret = path_is_reg_or_block_device(btrfs_dev);
	if (!ret)
		ret = set_label_mounted(btrfs_dev, label);
	else if (ret > 0)
		ret = set_label_unmounted(btrfs_dev, label);

	return ret;
}

