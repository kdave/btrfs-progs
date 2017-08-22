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

#ifndef __BTRFS_CONVERT_SOURCE_REISERFS_H__
#define __BTRFS_CONVERT_SOURCE_REISERFS_H__

#if BTRFSCONVERT_REISERFS

#include "kerncompat.h"
#include <reiserfs/misc.h>
#include <reiserfs/io.h>
#include <reiserfs/reiserfs_lib.h>
#include <reiserfs/reiserfs_fs.h>
#include <linux/kdev_t.h>
#include "convert/source-fs.h"

#define REISERFS_ACL_VERSION	0x0001

/* 23.2.5 acl_tag_t values */

#define ACL_UNDEFINED_TAG       (0x00)
#define ACL_USER_OBJ            (0x01)
#define ACL_USER                (0x02)
#define ACL_GROUP_OBJ           (0x04)
#define ACL_GROUP               (0x08)
#define ACL_MASK                (0x10)
#define ACL_OTHER               (0x20)

/* 23.2.7 ACL qualifier constants */

#define ACL_UNDEFINED_ID        ((id_t)-1)

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
	__le32		e_id;
} ext2_acl_entry;

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
} ext2_acl_entry_short;

typedef struct {
	__le32		a_version;
} ext2_acl_header;

#define ACL_EA_VERSION		0x0002

typedef struct {
	__le16		e_tag;
	__le16		e_perm;
	__le32		e_id;
} acl_ea_entry;

typedef struct {
	__le32		a_version;
	acl_ea_entry	a_entries[0];
} acl_ea_header;

static inline int ext2_acl_count(size_t size)
{
	ssize_t s;
	size -= sizeof(ext2_acl_header);
	s = size - 4 * sizeof(ext2_acl_entry_short);
	if (s < 0) {
		if (size % sizeof(ext2_acl_entry_short))
			return -1;
		return size / sizeof(ext2_acl_entry_short);
	} else {
		if (s % sizeof(ext2_acl_entry))
			return -1;
		return s / sizeof(ext2_acl_entry) + 4;
	}
}


static inline size_t acl_ea_size(int count)
{
	return sizeof(acl_ea_header) + count * sizeof(acl_ea_entry);
}

static inline dev_t new_decode_dev(u32 dev)
{
	unsigned major = (dev & 0xfff00) >> 8;
	unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);

	return MKDEV(major, minor);
}

#endif	/* BTRFSCONVERT_REISERFS */

#endif
