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
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"
#include "common/messages.h"
#include "common/path-utils.h"
#include "common/device-scan.h"
#include "common/open-utils.h"

/*
 * Check if a file is used (directly or indirectly via a loop device) by a
 * device in fs_devices
 */
static int blk_file_in_dev_list(struct btrfs_fs_devices* fs_devices,
		const char* file)
{
	int ret;
	struct btrfs_device *device;

	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		if((ret = is_same_loop_file(device->name, file)))
			return ret;
	}

	return 0;
}

int check_mounted_where(int fd, const char *file, char *where, int size,
			struct btrfs_fs_devices **fs_dev_ret, unsigned sbflags)
{
	int ret;
	u64 total_devs = 1;
	int is_btrfs;
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

			ret = is_same_loop_file(file, mnt->mnt_fsname);
		}

		if(ret < 0)
			goto out_mntloop_err;
		else if(ret)
			break;
	}

	/* Did we find an entry in mnt table? */
	if (mnt && size && where) {
		strncpy(where, mnt->mnt_dir, size);
		where[size-1] = 0;
	}
	if (fs_dev_ret)
		*fs_dev_ret = fs_devices_mnt;

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

	ret =  check_mounted_where(fd, file, NULL, 0, NULL, SBREAD_DEFAULT);
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

	ret = check_mounted_where(fd, dev, mp, mp_size, NULL, SBREAD_DEFAULT);
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
 * Given a pathname, return a filehandle to:
 * 	the original pathname or,
 * 	if the pathname is a mounted btrfs device, to its mountpoint.
 *
 * On error, return -1, errno should be set.
 */
int open_path_or_dev_mnt(const char *path, DIR **dirstream, int verbose)
{
	char mp[PATH_MAX];
	int ret;

	if (path_is_block_device(path)) {
		ret = get_btrfs_mount(path, mp, sizeof(mp));
		if (ret < 0) {
			/* not a mounted btrfs dev */
			error_on(verbose, "'%s' is not a mounted btrfs device",
				 path);
			errno = EINVAL;
			return -1;
		}
		ret = open_file_or_dir(mp, dirstream);
		error_on(verbose && ret < 0, "can't access '%s': %m",
			 path);
	} else {
		ret = btrfs_open_dir(path, dirstream, 1);
	}

	return ret;
}

/*
 * Do the following checks before calling open_file_or_dir():
 * 1: path is in a btrfs filesystem
 * 2: path is a directory if dir_only is 1
 */
int btrfs_open(const char *path, DIR **dirstream, int verbose, int dir_only)
{
	struct statfs stfs;
	struct stat st;
	int ret;

	if (stat(path, &st) != 0) {
		error_on(verbose, "cannot access '%s': %m", path);
		return -1;
	}

	if (dir_only && !S_ISDIR(st.st_mode)) {
		error_on(verbose, "not a directory: %s", path);
		return -3;
	}

	if (statfs(path, &stfs) != 0) {
		error_on(verbose, "cannot access '%s': %m", path);
		return -1;
	}

	if (stfs.f_type != BTRFS_SUPER_MAGIC) {
		error_on(verbose, "not a btrfs filesystem: %s", path);
		return -2;
	}

	ret = open_file_or_dir(path, dirstream);
	if (ret < 0) {
		error_on(verbose, "cannot access '%s': %m", path);
	}

	return ret;
}

int btrfs_open_dir(const char *path, DIR **dirstream, int verbose)
{
	return btrfs_open(path, dirstream, verbose, 1);
}

int btrfs_open_file_or_dir(const char *path, DIR **dirstream, int verbose)
{
	return btrfs_open(path, dirstream, verbose, 0);
}

int open_file_or_dir3(const char *fname, DIR **dirstream, int open_flags)
{
	int ret;
	struct stat st;
	int fd;

	ret = stat(fname, &st);
	if (ret < 0) {
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		*dirstream = opendir(fname);
		if (!*dirstream)
			return -1;
		fd = dirfd(*dirstream);
	} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
		fd = open(fname, open_flags);
	} else {
		/*
		 * we set this on purpose, in case the caller output
		 * strerror(errno) as success
		 */
		errno = EINVAL;
		return -1;
	}
	if (fd < 0) {
		fd = -1;
		if (*dirstream) {
			closedir(*dirstream);
			*dirstream = NULL;
		}
	}
	return fd;
}

int open_file_or_dir(const char *fname, DIR **dirstream)
{
	return open_file_or_dir3(fname, dirstream, O_RDWR);
}

void close_file_or_dir(int fd, DIR *dirstream)
{
	int old_errno;

	old_errno = errno;
	if (dirstream) {
		closedir(dirstream);
	} else if (fd >= 0) {
		close(fd);
	}

	errno = old_errno;
}


