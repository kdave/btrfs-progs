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

#ifndef __BTRFS_CONVERT_SOURCE_EXT2_H__
#define __BTRFS_CONVERT_SOURCE_EXT2_H__

#if BTRFSCONVERT_EXT2

#include "kerncompat.h"
#include <ext2fs/ext2_fs.h>
#include <ext2fs/ext2fs.h>
#include "convert/source-fs.h"

struct btrfs_inode_item;
struct btrfs_root;
struct btrfs_trans_handle;

#define INO_OFFSET (BTRFS_FIRST_FREE_OBJECTID - EXT2_ROOT_INO)

/*
 * Compatibility code for e2fsprogs 1.41 which doesn't support RO compat flag
 * BIGALLOC.
 * Unlike normal RO compat flag, BIGALLOC affects how e2fsprogs check used
 * space, and btrfs-convert heavily relies on it.
 *
 * e2fsprogs 1.42 also introduced the 64-bit API.  Any file system
 * that requires it will have EXT4_FEATURE_INCOMPAT_64BIT set and
 * will fail to open with earlier releases.  We can map it to the
 * older API without risk of corruption.
 */
#ifndef EXT2_FLAG_64BITS
#define EXT2_FLAG_64BITS		(0)
#define ext2fs_get_block_bitmap_range2 ext2fs_get_block_bitmap_range
#define ext2fs_inode_data_blocks2 ext2fs_inode_data_blocks
#define ext2fs_read_ext_attr2 ext2fs_read_ext_attr
#define ext2fs_blocks_count(s)		((s)->s_blocks_count)
#define EXT2FS_CLUSTER_RATIO(fs)	(1)
#define EXT2_CLUSTERS_PER_GROUP(s)	(EXT2_BLOCKS_PER_GROUP(s))
#define EXT2FS_B2C(fs, blk)		(blk)
#endif

/*
 * Following xattr/acl related codes are based on codes in
 * fs/ext3/xattr.c and fs/ext3/acl.c
 */
#define EXT2_XATTR_BHDR(ptr) ((struct ext2_ext_attr_header *)(ptr))
#define EXT2_XATTR_BFIRST(ptr) \
	((struct ext2_ext_attr_entry *)(EXT2_XATTR_BHDR(ptr) + 1))
#define EXT2_XATTR_IHDR(inode) \
	((struct ext2_ext_attr_header *) ((void *)(inode) + \
		EXT2_GOOD_OLD_INODE_SIZE + (inode)->i_extra_isize))
#define EXT2_XATTR_IFIRST(inode) \
	((struct ext2_ext_attr_entry *) ((void *)EXT2_XATTR_IHDR(inode) + \
		sizeof(EXT2_XATTR_IHDR(inode)->h_magic)))

struct dir_iterate_data {
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	struct btrfs_inode_item *inode;
	u64 objectid;
	u64 index_cnt;
	u64 parent;
	int errcode;
};

#define EXT2_ACL_VERSION	0x0001

#endif	/* BTRFSCONVERT_EXT2 */

#endif
