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

#ifndef BTRFS_UTIL_INTERNAL_H
#define BTRFS_UTIL_INTERNAL_H

#include <asm/byteorder.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "btrfsutil.h"
#include "btrfs.h"
#include "btrfs_tree.h"

#define PUBLIC __attribute__((visibility("default")))
#define LIBBTRFSUTIL_ALIAS(orig)	__attribute__((alias(#orig)))

#define le16_to_cpu __le16_to_cpu
#define le32_to_cpu __le32_to_cpu
#define le64_to_cpu __le64_to_cpu

#define SAVE_ERRNO_AND_CLOSE(fd) do {	\
	int saved_errno = errno;	\
					\
	close(fd);			\
	errno = saved_errno;		\
} while (0)

struct __unagligned_u16 { __le16 x; } __attribute__((__packed__));
struct __unagligned_u32 { __le32 x; } __attribute__((__packed__));
struct __unagligned_u64 { __le64 x; } __attribute__((__packed__));

static inline __u16 get_unaligned_le16(const void *ptr)
{
	return le16_to_cpu(((const struct __unagligned_u16 *)(ptr))->x);
}

static inline __u32 get_unaligned_le32(const void *ptr)
{
	return le32_to_cpu(((const struct __unagligned_u32 *)(ptr))->x);
}

static inline __u64 get_unaligned_le64(const void *ptr)
{
	return le64_to_cpu(((const struct __unagligned_u64 *)(ptr))->x);
}

/*
 * Accessors of search header that is commonly mapped to a byte buffer so the
 * alignment is not guraranteed
 */
static inline __u64 btrfs_search_header_transid(const struct btrfs_ioctl_search_header *sh)
{
	__u64 tmp;
	memcpy(&tmp, &sh->transid, sizeof(__u64));
	return tmp;
}

static inline __u64 btrfs_search_header_objectid(const struct btrfs_ioctl_search_header *sh)
{
	__u64 tmp;
	memcpy(&tmp, &sh->objectid, sizeof(__u64));
	return tmp;
}

static inline __u64 btrfs_search_header_offset(const struct btrfs_ioctl_search_header *sh)
{
	__u64 tmp;
	memcpy(&tmp, &sh->offset, sizeof(__u64));
	return tmp;
}

static inline __u32 btrfs_search_header_type(const struct btrfs_ioctl_search_header *sh)
{
	__u32 tmp;
	memcpy(&tmp, &sh->type, sizeof(__u32));
	return tmp;
}

static inline __u32 btrfs_search_header_len(const struct btrfs_ioctl_search_header *sh)
{
	__u32 tmp;
	memcpy(&tmp, &sh->len, sizeof(__u32));
	return tmp;
}

#endif /* BTRFS_UTIL_INTERNAL_H */
