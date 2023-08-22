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
#include <pthread.h>
#include "kernel-lib/rbtree.h"
#include "kernel-lib/list.h"
#include "kernel-lib/sizes.h"
#include "kernel-shared/ctree.h"
#include "image/sanitize.h"

#define IMAGE_BLOCK_SIZE		SZ_1K
#define IMAGE_BLOCK_MASK		(IMAGE_BLOCK_SIZE - 1)

#define ITEMS_PER_CLUSTER ((IMAGE_BLOCK_SIZE - sizeof(struct meta_cluster)) / \
			   sizeof(struct meta_cluster_item))

#define COMPRESS_NONE		0
#define COMPRESS_ZLIB		1

#define MAX_WORKER_THREADS	(32)

struct dump_version {
	u64 magic_cpu;
	int version;
	int max_pending_size;
	unsigned int extra_sb_flags:1;
};

extern const struct dump_version dump_versions[];
const extern struct dump_version *current_version;

struct async_work {
	struct list_head list;
	struct list_head ordered;
	u64 start;
	u64 size;
	u8 *buffer;
	size_t bufsize;
	int error;
};

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

struct metadump_struct {
	struct btrfs_root *root;
	FILE *out;

	pthread_t threads[MAX_WORKER_THREADS];
	size_t num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct rb_root name_tree;

	struct extent_io_tree seen;

	struct list_head list;
	struct list_head ordered;
	size_t num_items;
	size_t num_ready;

	u64 pending_start;
	u64 pending_size;

	int compress_level;
	int done;
	int data;
	enum sanitize_mode sanitize_names;

	int error;

	union {
		struct meta_cluster cluster;
		char meta_cluster_bytes[IMAGE_BLOCK_SIZE];
	};
};

struct mdrestore_struct {
	FILE *in;
	FILE *out;

	pthread_t threads[MAX_WORKER_THREADS];
	size_t num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	/*
	 * Records system chunk ranges, so restore can use this to determine
	 * if an item is in chunk tree range.
	 */
	struct cache_tree sys_chunks;
	struct rb_root chunk_tree;
	struct rb_root physical_tree;
	struct list_head list;
	struct list_head overlapping_chunks;
	struct btrfs_super_block *original_super;
	size_t num_items;
	u32 nodesize;
	u64 devid;
	u64 alloced_chunks;
	u64 last_physical_offset;
	/* An quicker checker for if a item is in sys chunk range */
	u64 sys_chunk_end;
	u8 uuid[BTRFS_UUID_SIZE];
	u8 fsid[BTRFS_FSID_SIZE];

	int compress_method;
	int done;
	int error;
	int old_restore;
	int fixup_offset;
	int multi_devices;
	int clear_space_cache;
	struct btrfs_fs_info *info;
};

int create_metadump(const char *input, FILE *out, int num_threads, int
		    compress_level, enum sanitize_mode sanitize, int
		    walk_trees, bool dump_data);
int restore_metadump(const char *input, FILE *out, int old_restore,
		     int num_threads, int fixup_offset, const char *target,
		     int multi_devices);

#endif
