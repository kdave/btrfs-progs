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
#include <stdbool.h>
#include <stddef.h>
#include <reiserfs/misc.h>
#include <reiserfs/io.h>
#include <reiserfs/reiserfs_lib.h>
#include <reiserfs/reiserfs_fs.h>
#include "convert/source-fs.h"

struct btrfs_inode_item;
struct btrfs_root;
struct btrfs_trans_handle;

#define REISERFS_ACL_VERSION	0x0001

#define OID_OFFSET (BTRFS_FIRST_FREE_OBJECTID - REISERFS_ROOT_OBJECTID)

struct reiserfs_convert_info {
	bool copy_attrs;
	struct reiserfs_key privroot_key;
	struct reiserfs_key xattr_key;

	/* only set during copy_inodes */
	struct task_ctx *progress;

	/* used to track hardlinks */
	unsigned used_slots;
	unsigned alloced_slots;
	u64 *objectids;
};

struct reiserfs_blk_iterate_data {
	struct blk_iterate_data blk_data;
	char *inline_data;
	u64 inline_offset;
	u32 inline_length;
};

struct reiserfs_dirent_data {
	u64 index;
	u32 convert_flags;
	struct btrfs_inode_item *inode;
	struct btrfs_root *root;
};

struct reiserfs_xattr_data {
	struct btrfs_root *root;
	struct btrfs_trans_handle *trans;
	u64 target_oid;
	const char *name;
	size_t namelen;
	void *body;
	size_t len;
};

#endif	/* BTRFSCONVERT_REISERFS */

#endif
