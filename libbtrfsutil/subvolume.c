/*
 * Copyright (C) 2018 Facebook
 *
 * This file is part of libbtrfsutil.
 *
 * libbtrfsutil is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libbtrfsutil is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libbtrfsutil.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <linux/magic.h>

#include "btrfsutil_internal.h"

/*
 * This intentionally duplicates btrfs_util_is_subvolume_fd() instead of opening
 * a file descriptor and calling it, because fstat() and fstatfs() don't accept
 * file descriptors opened with O_PATH on old kernels (before v3.6 and before
 * v3.12, respectively), but stat() and statfs() can be called on a path that
 * the user doesn't have read or write permissions to.
 */
PUBLIC enum btrfs_util_error btrfs_util_is_subvolume(const char *path)
{
	struct statfs sfs;
	struct stat st;
	int ret;

	ret = statfs(path, &sfs);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_STATFS_FAILED;

	if (sfs.f_type != BTRFS_SUPER_MAGIC) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_NOT_BTRFS;
	}

	ret = stat(path, &st);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_STAT_FAILED;

	if (st.st_ino != BTRFS_FIRST_FREE_OBJECTID || !S_ISDIR(st.st_mode)) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_NOT_SUBVOLUME;
	}

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_is_subvolume_fd(int fd)
{
	struct statfs sfs;
	struct stat st;
	int ret;

	ret = fstatfs(fd, &sfs);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_STATFS_FAILED;

	if (sfs.f_type != BTRFS_SUPER_MAGIC) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_NOT_BTRFS;
	}

	ret = fstat(fd, &st);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_STAT_FAILED;

	if (st.st_ino != BTRFS_FIRST_FREE_OBJECTID || !S_ISDIR(st.st_mode)) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_NOT_SUBVOLUME;
	}

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_id(const char *path,
						     uint64_t *id_ret)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_subvolume_id_fd(fd, id_ret);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_subvolume_id_fd(int fd,
							uint64_t *id_ret)
{
	struct btrfs_ioctl_ino_lookup_args args = {
		.treeid = 0,
		.objectid = BTRFS_FIRST_FREE_OBJECTID,
	};
	int ret;

	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &args);
	if (ret == -1) {
		close(fd);
		return BTRFS_UTIL_ERROR_INO_LOOKUP_FAILED;
	}

	*id_ret = args.treeid;

	return BTRFS_UTIL_OK;
}

static enum btrfs_util_error openat_parent_and_name(int dirfd, const char *path,
						    char *name, size_t name_len,
						    int *fd)
{
	char *tmp_path, *slash, *dirname, *basename;
	size_t len;

	/* Ignore trailing slashes. */
	len = strlen(path);
	while (len > 1 && path[len - 1] == '/')
		len--;

	tmp_path = malloc(len + 1);
	if (!tmp_path)
		return BTRFS_UTIL_ERROR_NO_MEMORY;
	memcpy(tmp_path, path, len);
	tmp_path[len] = '\0';

	slash = memrchr(tmp_path, '/', len);
	if (slash == tmp_path) {
		dirname = "/";
		basename = tmp_path + 1;
	} else if (slash) {
		*slash = '\0';
		dirname = tmp_path;
		basename = slash + 1;
	} else {
		dirname = ".";
		basename = tmp_path;
	}

	len = strlen(basename);
	if (len >= name_len) {
		free(tmp_path);
		errno = ENAMETOOLONG;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}
	memcpy(name, basename, len);
	name[len] = '\0';

	*fd = openat(dirfd, dirname, O_RDONLY | O_DIRECTORY);
	if (*fd == -1) {
		free(tmp_path);
		return BTRFS_UTIL_ERROR_OPEN_FAILED;
	}

	free(tmp_path);
	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_create_subvolume(const char *path,
							 int flags,
							 uint64_t *async_transid,
							 struct btrfs_util_qgroup_inherit *qgroup_inherit)
{
	char name[BTRFS_SUBVOL_NAME_MAX + 1];
	enum btrfs_util_error err;
	int parent_fd;

	err = openat_parent_and_name(AT_FDCWD, path, name, sizeof(name),
				     &parent_fd);
	if (err)
		return err;

	err = btrfs_util_create_subvolume_fd(parent_fd, name, flags,
					    async_transid, qgroup_inherit);
	SAVE_ERRNO_AND_CLOSE(parent_fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_create_subvolume_fd(int parent_fd,
							    const char *name,
							    int flags,
							    uint64_t *async_transid,
							    struct btrfs_util_qgroup_inherit *qgroup_inherit)
{
	struct btrfs_ioctl_vol_args_v2 args = {};
	size_t len;
	int ret;

	if (flags) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}

	if (async_transid)
		args.flags |= BTRFS_SUBVOL_CREATE_ASYNC;
	if (qgroup_inherit) {
		args.flags |= BTRFS_SUBVOL_QGROUP_INHERIT;
		args.qgroup_inherit = (struct btrfs_qgroup_inherit *)qgroup_inherit;
		args.size = (sizeof(*args.qgroup_inherit) +
			     args.qgroup_inherit->num_qgroups *
			     sizeof(args.qgroup_inherit->qgroups[0]));
	}

	len = strlen(name);
	if (len >= sizeof(args.name)) {
		errno = ENAMETOOLONG;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}
	memcpy(args.name, name, len);
	args.name[len] = '\0';

	ret = ioctl(parent_fd, BTRFS_IOC_SUBVOL_CREATE_V2, &args);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_SUBVOL_CREATE_FAILED;

	if (async_transid)
		*async_transid = args.transid;

	return BTRFS_UTIL_OK;
}
