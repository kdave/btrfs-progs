/*
 * Copyright (C) 2018 Facebook
 *
 * This file is part of libbtrfsutil.
 *
 * libbtrfsutil is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
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
#include <unistd.h>
#include <sys/ioctl.h>

#include "btrfsutil_internal.h"

PUBLIC enum btrfs_util_error btrfs_util_sync(const char *path)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_sync_fd(fd);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_sync_fd(int fd)
{
	int ret;

	ret = ioctl(fd, BTRFS_IOC_SYNC, NULL);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_SYNC_FAILED;

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_start_sync(const char *path,
						   uint64_t *transid)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_start_sync_fd(fd, transid);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_start_sync_fd(int fd, uint64_t *transid)
{
	int ret;

	ret = ioctl(fd, BTRFS_IOC_START_SYNC, transid);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_START_SYNC_FAILED;

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_wait_sync(const char *path,
						  uint64_t transid)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_wait_sync_fd(fd, transid);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_wait_sync_fd(int fd, uint64_t transid)
{
	int ret;

	ret = ioctl(fd, BTRFS_IOC_WAIT_SYNC, &transid);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_WAIT_SYNC_FAILED;

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_filesystem_set_label_fd(int fd,
								const char *label)
{
	int ret;
	char buf[BTRFS_UTIL_LABEL_SIZE];

	if (strlen(label) >= BTRFS_UTIL_LABEL_SIZE) {
		errno = EINVAL;
		return BTRFS_UTIL_ERROR_INVALID_ARGUMENT;
	}
	memset(buf, 0, sizeof(buf));
	strncpy(buf, label, BTRFS_UTIL_LABEL_SIZE - 1);

	ret = ioctl(fd, BTRFS_IOC_SET_FSLABEL, buf);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_GET_LABEL_FAILED;

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_filesystem_set_label(const char *path,
							     const char *label)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_filesystem_set_label_fd(fd, label);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}

PUBLIC enum btrfs_util_error btrfs_util_filesystem_get_label_fd(int fd,
								char *label)
{
	int ret;

	/* Kernel does not return more than BTRFS_LABEL_SIZE/BTRFS_UTIL_LABEL_SIZE */
	ret = ioctl(fd, BTRFS_IOC_GET_FSLABEL, label);
	if (ret == -1)
		return BTRFS_UTIL_ERROR_GET_LABEL_FAILED;

	return BTRFS_UTIL_OK;
}

PUBLIC enum btrfs_util_error btrfs_util_filesystem_get_label(const char *path,
							     char *label)
{
	enum btrfs_util_error err;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		return BTRFS_UTIL_ERROR_OPEN_FAILED;

	err = btrfs_util_filesystem_get_label_fd(fd, label);
	SAVE_ERRNO_AND_CLOSE(fd);
	return err;
}
