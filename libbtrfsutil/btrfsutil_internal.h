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

#ifndef BTRFS_UTIL_INTERNAL_H
#define BTRFS_UTIL_INTERNAL_H

#include <asm/byteorder.h>

#include "btrfsutil.h"
#include "btrfs.h"
#include "btrfs_tree.h"

#define PUBLIC __attribute__((visibility("default")))

#define le16_to_cpu __le16_to_cpu
#define le32_to_cpu __le32_to_cpu
#define le64_to_cpu __le64_to_cpu

#define SAVE_ERRNO_AND_CLOSE(fd) do {	\
	int saved_errno = errno;	\
					\
	close(fd);			\
	errno = saved_errno;		\
} while (0)

#endif /* BTRFS_UTIL_INTERNAL_H */
