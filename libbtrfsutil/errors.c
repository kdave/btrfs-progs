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

#include <stddef.h>

#include "btrfsutil_internal.h"

static const char * const error_messages[] = {
	[BTRFS_UTIL_OK] = "Success",
	[BTRFS_UTIL_ERROR_STOP_ITERATION] = "Stop iteration",
	[BTRFS_UTIL_ERROR_NO_MEMORY] = "Cannot allocate memory",
	[BTRFS_UTIL_ERROR_INVALID_ARGUMENT] = "Invalid argument",
	[BTRFS_UTIL_ERROR_NOT_BTRFS] = "Not a Btrfs filesystem",
	[BTRFS_UTIL_ERROR_NOT_SUBVOLUME] = "Not a Btrfs subvolume",
	[BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND] = "Subvolume not found",
	[BTRFS_UTIL_ERROR_OPEN_FAILED] = "Could not open",
	[BTRFS_UTIL_ERROR_RMDIR_FAILED] = "Could not rmdir",
	[BTRFS_UTIL_ERROR_UNLINK_FAILED] = "Could not unlink",
	[BTRFS_UTIL_ERROR_STAT_FAILED] = "Could not stat",
	[BTRFS_UTIL_ERROR_STATFS_FAILED] = "Could not statfs",
	[BTRFS_UTIL_ERROR_SEARCH_FAILED] = "Could not search B-tree",
	[BTRFS_UTIL_ERROR_INO_LOOKUP_FAILED] = "Could not lookup inode",
	[BTRFS_UTIL_ERROR_SUBVOL_GETFLAGS_FAILED] = "Could not get subvolume flags",
	[BTRFS_UTIL_ERROR_SUBVOL_SETFLAGS_FAILED] = "Could not set subvolume flags",
	[BTRFS_UTIL_ERROR_SUBVOL_CREATE_FAILED] = "Could not create subvolume",
	[BTRFS_UTIL_ERROR_SNAP_CREATE_FAILED] = "Could not create snapshot",
	[BTRFS_UTIL_ERROR_SNAP_DESTROY_FAILED] = "Could not destroy subvolume/snapshot",
	[BTRFS_UTIL_ERROR_DEFAULT_SUBVOL_FAILED] = "Could not set default subvolume",
	[BTRFS_UTIL_ERROR_SYNC_FAILED] = "Could not sync filesystem",
	[BTRFS_UTIL_ERROR_START_SYNC_FAILED] = "Could not start filesystem sync",
	[BTRFS_UTIL_ERROR_WAIT_SYNC_FAILED] = "Could not wait for filesystem sync",
	[BTRFS_UTIL_ERROR_GET_SUBVOL_INFO_FAILED] =
		"Could not get subvolume information with BTRFS_IOC_GET_SUBVOL_INFO",
	[BTRFS_UTIL_ERROR_GET_SUBVOL_ROOTREF_FAILED] =
		"Could not get rootref information with BTRFS_IOC_GET_SUBVOL_ROOTREF",
	[BTRFS_UTIL_ERROR_INO_LOOKUP_USER_FAILED] =
		"Could not resolve subvolume path with BTRFS_IOC_INO_LOOKUP_USER",
	[BTRFS_UTIL_ERROR_FS_INFO_FAILED] =
		"Could not get filesystem information",
};

PUBLIC const char *btrfs_util_strerror(enum btrfs_util_error err)
{
	if (err < 0 || err >= sizeof(error_messages) / sizeof(error_messages[0]))
		return NULL;
	return error_messages[err];
}
