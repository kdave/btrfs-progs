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

#ifndef BTRFS_UTIL_H
#define BTRFS_UTIL_H

#define BTRFS_UTIL_VERSION_MAJOR 1
#define BTRFS_UTIL_VERSION_MINOR 0
#define BTRFS_UTIL_VERSION_PATCH 0

#ifdef __cplusplus
extern "C" {
#endif

/**
 * enum btrfs_util_error - libbtrfsutil error codes.
 *
 * All functions in libbtrfsutil that can return an error return this type and
 * set errno.
 */
enum btrfs_util_error {
	BTRFS_UTIL_OK,
	BTRFS_UTIL_ERROR_STOP_ITERATION,
	BTRFS_UTIL_ERROR_NO_MEMORY,
	BTRFS_UTIL_ERROR_INVALID_ARGUMENT,
	BTRFS_UTIL_ERROR_NOT_BTRFS,
	BTRFS_UTIL_ERROR_NOT_SUBVOLUME,
	BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND,
	BTRFS_UTIL_ERROR_OPEN_FAILED,
	BTRFS_UTIL_ERROR_RMDIR_FAILED,
	BTRFS_UTIL_ERROR_UNLINK_FAILED,
	BTRFS_UTIL_ERROR_STAT_FAILED,
	BTRFS_UTIL_ERROR_STATFS_FAILED,
	BTRFS_UTIL_ERROR_SEARCH_FAILED,
	BTRFS_UTIL_ERROR_INO_LOOKUP_FAILED,
	BTRFS_UTIL_ERROR_SUBVOL_GETFLAGS_FAILED,
	BTRFS_UTIL_ERROR_SUBVOL_SETFLAGS_FAILED,
	BTRFS_UTIL_ERROR_SUBVOL_CREATE_FAILED,
	BTRFS_UTIL_ERROR_SNAP_CREATE_FAILED,
	BTRFS_UTIL_ERROR_SNAP_DESTROY_FAILED,
	BTRFS_UTIL_ERROR_DEFAULT_SUBVOL_FAILED,
	BTRFS_UTIL_ERROR_SYNC_FAILED,
	BTRFS_UTIL_ERROR_START_SYNC_FAILED,
	BTRFS_UTIL_ERROR_WAIT_SYNC_FAILED,
};

/**
 * btrfs_util_strerror() - Convert a libtrfsutil error code to a string
 * description.
 * @err: The error to convert.
 *
 * Return: Error description.
 */
const char *btrfs_util_strerror(enum btrfs_util_error err);

#ifdef __cplusplus
}
#endif

#endif /* BTRFS_UTIL_H */
