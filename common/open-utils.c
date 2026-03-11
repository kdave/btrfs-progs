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
#include <sys/stat.h>
#include <sys/statfs.h>
#include <linux/magic.h>
#include <unistd.h>
#include <fcntl.h>
#include <mntent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "kernel-lib/list.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"
#include "common/messages.h"
#include "common/path-utils.h"
#include "common/device-scan.h"
#include "common/string-utils.h"
#include "common/open-utils.h"

/*
 * Check if a file is used  by a device in fs_devices
 */
static int blk_file_in_dev_list(struct btrfs_fs_devices* fs_devices,
		const char* file)
{
	int ret;
	struct btrfs_device *device;

	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		if((ret = is_same_blk_file(device->name, file)))
			return ret;
	}

	return 0;
}

int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_dev_ret, unsigned sbflags,
			bool noscan)
{
	int ret;
	u64 total_devs = 1;
	bool is_btrfs;
	struct btrfs_fs_devices *fs_devices_mnt = NULL;
	FILE *f;
	struct mntent *mnt;

	/* scan the initial device */
	ret = btrfs_scan_one_device(fd, file, &fs_devices_mnt,
		    &total_devs, BTRFS_SUPER_INFO_OFFSET, sbflags);
	is_btrfs = (ret >= 0);

	/* scan other devices */
	if (is_btrfs && total_devs > 1) {
		ret = btrfs_scan_devices(0);
		if (ret)
			return ret;
	}

	/* iterate over the list of currently mounted filesystems */
	if ((f = setmntent ("/proc/self/mounts", "r")) == NULL)
		return -errno;

	while ((mnt = getmntent (f)) != NULL) {
		if(is_btrfs) {
			if(strcmp(mnt->mnt_type, "btrfs") != 0)
				continue;

			ret = blk_file_in_dev_list(fs_devices_mnt, mnt->mnt_fsname);
		} else {
			/* ignore entries in the mount table that are not
			   associated with a file*/
			if((ret = path_is_reg_or_block_device(mnt->mnt_fsname)) < 0)
				goto out_mntloop_err;
			else if(!ret)
				continue;

			ret = is_same_blk_file(file, mnt->mnt_fsname);
		}

		if(ret < 0)
			goto out_mntloop_err;
		else if(ret)
			break;
	}

	/* Did we find an entry in mnt table? */
	if (mnt && size && where)
		strncpy_null(where, mnt->mnt_dir, size);

	if (fs_dev_ret)
		*fs_dev_ret = fs_devices_mnt;
	else if (noscan)
		btrfs_close_all_devices();

	ret = (mnt != NULL);

out_mntloop_err:
	endmntent (f);

	return ret;
}

/*
 * returns 1 if the device was mounted, < 0 on error or 0 if everything
 * is safe to continue.
 */
int check_mounted(const char* file)
{
	int fd;
	int ret;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		error("mount check: cannot open %s: %m", file);
		return -errno;
	}

	ret =  check_mounted_where(fd, file, NULL, 0, NULL, SBREAD_DEFAULT, false);
	close(fd);

	return ret;
}

/*
 * Find the mount point for a mounted device.
 * On success, returns 0 with mountpoint in *mp.
 * On failure, returns -errno (not mounted yields -EINVAL)
 * Is noisy on failures, expects to be given a mounted device.
 */
int get_btrfs_mount(const char *dev, char *mp, size_t mp_size)
{
	int ret;
	int fd = -1;

	ret = path_is_block_device(dev);
	if (ret <= 0) {
		if (!ret) {
			error("not a block device: %s", dev);
			ret = -EINVAL;
		} else {
			errno = -ret;
			error("cannot check %s: %m", dev);
		}
		goto out;
	}

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		error("cannot open %s: %m", dev);
		goto out;
	}

	ret = check_mounted_where(fd, dev, mp, mp_size, NULL, SBREAD_DEFAULT, false);
	if (!ret) {
		ret = -EINVAL;
	} else { /* mounted, all good */
		ret = 0;
	}
out:
	if (fd != -1)
		close(fd);
	return ret;
}

/*
 * Open the given path and check if it's a btrfs filesystem.
 *
 * Return the file descriptor or -errno.
 */
int btrfs_open_path(const char *path, bool read_write, bool dir_only)
{
	struct statfs stfs;
	struct stat st;
	int ret;

	if (stat(path, &st) < 0) {
		error("cannot access '%s': %m", path);
		return -errno;
	}

	if (dir_only && !S_ISDIR(st.st_mode)) {
		error("not a directory: %s", path);
		return -ENOTDIR;
	}

	if (statfs(path, &stfs) < 0) {
		error("cannot access '%s': %m", path);
		return -errno;
	}

	if (stfs.f_type != BTRFS_SUPER_MAGIC) {
		error("not a btrfs filesystem: %s", path);
		return -EINVAL;
	}

	if (S_ISDIR(st.st_mode) || !read_write)
		ret = open(path, O_RDONLY);
	else
		ret = open(path, O_RDWR);

	if (ret < 0) {
		error("cannot access '%s': %m", path);
		ret = -errno;
	}

	return ret;
}

int btrfs_open_file_or_dir(const char *path)
{
	return btrfs_open_path(path, true, false);
}

/* Open the path for write and check that it's a directory. */
int btrfs_open_dir(const char *path)
{
	return btrfs_open_path(path, true, true);
}

/*
 * Given a path, return a file descriptor to the original path name or, if the
 * pathname is a mounted btrfs device, to its mountpoint.
 *
 * Return the file descriptor or -errno.
 */
int btrfs_open_mnt(const char *path)
{
	char mp[PATH_MAX];
	int ret;

	if (path_is_block_device(path)) {
		ret = get_btrfs_mount(path, mp, sizeof(mp));
		if (ret < 0) {
			error("'%s' is not a mounted btrfs device", path);
			return -EINVAL;
		}
		ret = btrfs_open_dir(mp);
	} else {
		ret = btrfs_open_dir(path);
	}

	return ret;
}
