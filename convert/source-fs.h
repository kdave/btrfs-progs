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

#ifndef __BTRFS_CONVERT_SOURCE_FS_H__
#define __BTRFS_CONVERT_SOURCE_FS_H__

#include "kerncompat.h"
#include <sys/types.h>
#include <pthread.h>

struct btrfs_convert_context;
struct btrfs_inode_item;
struct btrfs_root;
struct btrfs_trans_handle;
struct task_info;

#define CONV_IMAGE_SUBVOL_OBJECTID BTRFS_FIRST_FREE_OBJECTID

extern const struct simple_range btrfs_reserved_ranges[3];

const struct simple_range *intersect_with_reserved(u64 bytenr, u64 num_bytes);

struct task_ctx {
	pthread_mutex_t mutex;
	u64 max_copy_inodes;
	u64 cur_copy_inodes;
	struct task_info *info;
};

#define SOURCE_FS_NAME_LEN	(16)

#define CONVERT_FLAG_DATACSUM		(1U << 0)
#define CONVERT_FLAG_INLINE_DATA	(1U << 1)
#define CONVERT_FLAG_XATTR		(1U << 2)
#define CONVERT_FLAG_COPY_LABEL		(1U << 3)
#define CONVERT_FLAG_SET_LABEL		(1U << 4)
#define CONVERT_FLAG_COPY_FSID		(1U << 5)

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

static inline size_t acl_ea_size(int count)
{
	return sizeof(acl_ea_header) + count * sizeof(acl_ea_entry);
}

int ext2_acl_count(size_t size);

#ifndef MKDEV
#define MINORBITS	20
#define MKDEV(ma, mi)	(((ma) << MINORBITS) | (mi))
#endif

dev_t decode_dev(u32 dev);

struct btrfs_convert_operations {
	const char name[SOURCE_FS_NAME_LEN];
	int (*open_fs)(struct btrfs_convert_context *cctx, const char *devname);
	int (*read_used_space)(struct btrfs_convert_context *cctx);
	int (*copy_inodes)(struct btrfs_convert_context *cctx,
			 struct btrfs_root *root, u32 covert_flags,
			 struct task_ctx *p);
	void (*close_fs)(struct btrfs_convert_context *cctx);
	int (*check_state)(struct btrfs_convert_context *cctx);
};

struct blk_iterate_data {
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root;
	struct btrfs_root *convert_root;
	struct btrfs_inode_item *inode;
	u64 convert_ino;
	u64 objectid;
	u64 first_block;
	u64 disk_block;
	u64 num_blocks;
	u64 boundary;
	int checksum;
	int errcode;
};

void init_convert_context(struct btrfs_convert_context *cctx);
void clean_convert_context(struct btrfs_convert_context *cctx);
int block_iterate_proc(u64 disk_block, u64 file_block,
		              struct blk_iterate_data *idata);
void init_blk_iterate_data(struct blk_iterate_data *data,
				  struct btrfs_trans_handle *trans,
				  struct btrfs_root *root,
				  struct btrfs_inode_item *inode,
				  u64 objectid, int checksum);
int convert_insert_dirent(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 const char *name, size_t name_len,
				 u64 dir, u64 objectid,
				 u8 file_type, u64 index_cnt,
				 struct btrfs_inode_item *inode);
int read_disk_extent(struct btrfs_root *root, u64 bytenr,
		            u32 num_bytes, char *buffer);
int record_file_blocks(struct blk_iterate_data *data,
			      u64 file_block, u64 disk_block, u64 num_blocks);

#endif
