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

#ifndef __BTRFS_IMAGE_METADUMP_H__
#define __BTRFS_IMAGE_METADUMP_H__

#include "kerncompat.h"
#include "kernel-lib/rbtree.h"
#include "kernel-lib/list.h"
#include "kernel-shared/ctree.h"

#define BLOCK_SIZE		SZ_1K
#define BLOCK_MASK		(BLOCK_SIZE - 1)

#define ITEMS_PER_CLUSTER ((BLOCK_SIZE - sizeof(struct meta_cluster)) / \
			   sizeof(struct meta_cluster_item))

#define COMPRESS_NONE		0
#define COMPRESS_ZLIB		1

struct dump_version {
	u64 magic_cpu;
	int version;
	int max_pending_size;
	unsigned int extra_sb_flags:1;
};

extern const struct dump_version dump_versions[];
const extern struct dump_version *current_version;

struct meta_cluster_item {
	__le64 bytenr;
	__le32 size;
} __attribute__ ((__packed__));

struct meta_cluster_header {
	__le64 magic;
	__le64 bytenr;
	__le32 nritems;
	u8 compress;
} __attribute__ ((__packed__));

/* cluster header + index items + buffers */
struct meta_cluster {
	struct meta_cluster_header header;
	struct meta_cluster_item items[];
} __attribute__ ((__packed__));

struct fs_chunk {
	u64 logical;
	u64 physical;
	/*
	 * physical_dup only store additional physical for BTRFS_BLOCK_GROUP_DUP
	 * currently restore only support single and DUP
	 * TODO: modify this structure and the function related to this
	 * structure for support RAID*
	 */
	u64 physical_dup;
	u64 bytes;
	struct rb_node l;
	struct rb_node p;
	struct list_head list;
};

#endif
