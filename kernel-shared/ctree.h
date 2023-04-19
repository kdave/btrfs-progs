/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
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

#ifndef __BTRFS_CTREE_H__
#define __BTRFS_CTREE_H__

#include <stdbool.h>

#include "kernel-lib/list.h"
#include "kerncompat.h"
#include "common/extent-cache.h"
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/extent-io-tree.h"
#include "kernel-shared/locking.h"

struct btrfs_root;
struct btrfs_trans_handle;
struct btrfs_free_space_ctl;

/*
 * Fake signature for an unfinalized filesystem, which only has barebone tree
 * structures (normally 6 near empty trees, on SINGLE meta/sys temporary chunks)
 *
 * ascii !BHRfS_M, no null
 */
#define BTRFS_MAGIC_TEMPORARY 0x4D5F536652484221ULL

#define BTRFS_MAX_MIRRORS 3

struct btrfs_mapping_tree {
	struct cache_tree cache_tree;
};

static inline unsigned long btrfs_chunk_item_size(int num_stripes)
{
	BUG_ON(num_stripes == 0);
	return sizeof(struct btrfs_chunk) +
		sizeof(struct btrfs_stripe) * (num_stripes - 1);
}

/* Temporary flag not on-disk for blocks that have changed csum already */
#define BTRFS_HEADER_FLAG_CSUM_NEW		(1ULL << 16)
#define BTRFS_SUPER_FLAG_CHANGING_CSUM		(1ULL << 37)

/*
 * The fs is undergoing block group tree feature change.
 * If no BLOCK_GROUP_TREE compat ro flag, it's changing from regular
 * bg item in extent tree to new bg tree.
 */
#define BTRFS_SUPER_FLAG_CHANGING_BG_TREE	(1ULL << 38)

static inline u32 __BTRFS_LEAF_DATA_SIZE(u32 nodesize)
{
	return nodesize - sizeof(struct btrfs_header);
}

#define BTRFS_LEAF_DATA_SIZE(fs_info) (fs_info->leaf_data_size)

#define BTRFS_SUPER_INFO_OFFSET			(65536)
#define BTRFS_SUPER_INFO_SIZE			(4096)

/*
 * The FREE_SPACE_TREE and FREE_SPACE_TREE_VALID compat_ro bits must not be
 * added here until read-write support for the free space tree is implemented in
 * btrfs-progs.
 */
#define BTRFS_FEATURE_COMPAT_RO_SUPP			\
	(BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE |	\
	 BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID | \
	 BTRFS_FEATURE_COMPAT_RO_VERITY |		\
	 BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE)

#if EXPERIMENTAL
#define BTRFS_FEATURE_INCOMPAT_SUPP			\
	(BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF |		\
	 BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL |	\
	 BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO |		\
	 BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD |		\
	 BTRFS_FEATURE_INCOMPAT_BIG_METADATA |		\
	 BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF |		\
	 BTRFS_FEATURE_INCOMPAT_RAID56 |		\
	 BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS |		\
	 BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA |	\
	 BTRFS_FEATURE_INCOMPAT_NO_HOLES |		\
	 BTRFS_FEATURE_INCOMPAT_RAID1C34 |		\
	 BTRFS_FEATURE_INCOMPAT_METADATA_UUID |		\
	 BTRFS_FEATURE_INCOMPAT_ZONED |			\
	 BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2)
#else
#define BTRFS_FEATURE_INCOMPAT_SUPP			\
	(BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF |		\
	 BTRFS_FEATURE_INCOMPAT_DEFAULT_SUBVOL |	\
	 BTRFS_FEATURE_INCOMPAT_COMPRESS_LZO |		\
	 BTRFS_FEATURE_INCOMPAT_COMPRESS_ZSTD |		\
	 BTRFS_FEATURE_INCOMPAT_BIG_METADATA |		\
	 BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF |		\
	 BTRFS_FEATURE_INCOMPAT_RAID56 |		\
	 BTRFS_FEATURE_INCOMPAT_MIXED_GROUPS |		\
	 BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA |	\
	 BTRFS_FEATURE_INCOMPAT_NO_HOLES |		\
	 BTRFS_FEATURE_INCOMPAT_RAID1C34 |		\
	 BTRFS_FEATURE_INCOMPAT_METADATA_UUID |		\
	 BTRFS_FEATURE_INCOMPAT_ZONED)
#endif

/*
 * btrfs_paths remember the path taken from the root down to the leaf.
 * level 0 is always the leaf, and nodes[1...BTRFS_MAX_LEVEL] will point
 * to any other levels that are present.
 *
 * The slots array records the index of the item or block pointer
 * used while walking the tree.
 */
enum { READA_NONE = 0, READA_BACK, READA_FORWARD };
struct btrfs_path {
	struct extent_buffer *nodes[BTRFS_MAX_LEVEL];
	int slots[BTRFS_MAX_LEVEL];
#if 0
	/* The kernel locking scheme is not done in userspace. */
	int locks[BTRFS_MAX_LEVEL];
#endif
	signed char reada;
	/* keep some upper locks as we walk down */
	u8 lowest_level;

	/*
	 * set by btrfs_split_item, tells search_slot to keep all locks
	 * and to force calls to keep space in the nodes
	 */
	u8 search_for_split;
	u8 skip_check_block;
};

#define BTRFS_MAX_EXTENT_ITEM_SIZE(r) \
			((BTRFS_LEAF_DATA_SIZE(r->fs_info) >> 4) - \
					sizeof(struct btrfs_item))
#define BTRFS_MAX_EXTENT_SIZE		128UL * 1024 * 1024

enum btrfs_tree_block_status {
	BTRFS_TREE_BLOCK_CLEAN,
	BTRFS_TREE_BLOCK_INVALID_NRITEMS,
	BTRFS_TREE_BLOCK_INVALID_PARENT_KEY,
	BTRFS_TREE_BLOCK_BAD_KEY_ORDER,
	BTRFS_TREE_BLOCK_INVALID_LEVEL,
	BTRFS_TREE_BLOCK_INVALID_FREE_SPACE,
	BTRFS_TREE_BLOCK_INVALID_OFFSETS,
	BTRFS_TREE_BLOCK_INVALID_BLOCKPTR,
};

/*
 * We don't want to overwrite 1M at the beginning of device, even though
 * there is our 1st superblock at 64k. Some possible reasons:
 *  - the first 64k blank is useful for some boot loader/manager
 *  - the first 1M could be scratched by buggy partitioner or somesuch
 */
#define BTRFS_BLOCK_RESERVED_1M_FOR_SUPER	((u64)1 * 1024 * 1024)

enum btrfs_raid_types {
	BTRFS_RAID_RAID10,
	BTRFS_RAID_RAID1,
	BTRFS_RAID_DUP,
	BTRFS_RAID_RAID0,
	BTRFS_RAID_SINGLE,
	BTRFS_RAID_RAID5,
	BTRFS_RAID_RAID6,
	BTRFS_RAID_RAID1C3,
	BTRFS_RAID_RAID1C4,
	BTRFS_NR_RAID_TYPES
};

/*
 * GLOBAL_RSV does not exist as a on-disk block group type and is used
 * internally for exporting info about global block reserve from space infos
 */
#define BTRFS_SPACE_INFO_GLOBAL_RSV    (1ULL << 49)

#define BTRFS_QGROUP_LEVEL_SHIFT		48

static inline u64 btrfs_qgroup_subvolid(u64 qgroupid)
{
	return qgroupid & ((1ULL << BTRFS_QGROUP_LEVEL_SHIFT) - 1);
}

struct btrfs_space_info {
	u64 flags;
	u64 total_bytes;
	/*
	 * Space already used.
	 * Only accounting space in current extent tree, thus delayed ref
	 * won't be accounted here.
	 */
	u64 bytes_used;

	/*
	 * Space being pinned down.
	 * So extent allocator will not try to allocate space from them.
	 *
	 * For cases like extents being freed in current transaction, or
	 * manually pinned bytes for re-initializing certain trees.
	 */
	u64 bytes_pinned;

	/*
	 * Space being reserved.
	 * Space has already being reserved but not yet reach extent tree.
	 *
	 * New tree blocks allocated in current transaction goes here.
	 */
	u64 bytes_reserved;
	int full;
	struct list_head list;
};

struct btrfs_block_group {
	struct btrfs_space_info *space_info;
	struct btrfs_free_space_ctl *free_space_ctl;
	u64 start;
	u64 length;
	u64 used;
	u64 bytes_super;
	u64 pinned;
	u64 flags;
	int cached;
	int ro;
	/*
	 * If the free space extent count exceeds this number, convert the block
	 * group to bitmaps.
	 */
	u32 bitmap_high_thresh;
	/*
	 * If the free space extent count drops below this number, convert the
	 * block group back to extents.
	 */
	u32 bitmap_low_thresh;

	/* Block group cache stuff */
	struct rb_node cache_node;

	/* For dirty block groups */
	struct list_head dirty_list;

	/*
	 * Allocation offset for the block group to implement sequential
	 * allocation. This is used only with ZONED mode enabled.
	 */
	u64 alloc_offset;
	u64 write_offset;

	u64 global_root_id;
};

struct btrfs_device;
struct btrfs_fs_devices;
struct btrfs_fs_info {
	u8 chunk_tree_uuid[BTRFS_UUID_SIZE];
	u8 *new_chunk_tree_uuid;
	struct btrfs_root *fs_root;
	struct btrfs_root *tree_root;
	struct btrfs_root *chunk_root;
	struct btrfs_root *dev_root;
	struct btrfs_root *quota_root;
	struct btrfs_root *uuid_root;
	struct btrfs_root *block_group_root;

	struct rb_root global_roots_tree;
	struct rb_root fs_root_tree;

	/* the log root tree is a directory of all the other log roots */
	struct btrfs_root *log_root_tree;

	/* When switching csums */
	struct btrfs_root *csum_tree_tmp;

	struct cache_tree extent_cache;
	u64 max_cache_size;
	u64 cache_size;
	struct list_head lru;

	struct extent_io_tree dirty_buffers;
	struct extent_io_tree free_space_cache;
	struct extent_io_tree pinned_extents;
	struct extent_io_tree extent_ins;
	struct extent_io_tree *excluded_extents;

	struct rb_root block_group_cache_tree;
	/* logical->physical extent mapping */
	struct btrfs_mapping_tree mapping_tree;

	u64 generation;
	u64 last_trans_committed;

	u64 avail_data_alloc_bits;
	u64 avail_metadata_alloc_bits;
	u64 avail_system_alloc_bits;
	u64 data_alloc_profile;
	u64 metadata_alloc_profile;
	u64 system_alloc_profile;

	struct btrfs_trans_handle *running_transaction;
	struct btrfs_super_block *super_copy;

	u64 super_bytenr;
	u64 total_pinned;
	u64 nr_global_roots;

	struct list_head dirty_cowonly_roots;
	struct list_head recow_ebs;

	struct btrfs_fs_devices *fs_devices;
	struct list_head space_info;

	unsigned int system_allocs:1;
	unsigned int readonly:1;
	unsigned int on_restoring:1;
	unsigned int is_chunk_recover:1;
	unsigned int quota_enabled:1;
	unsigned int suppress_check_block_errors:1;
	unsigned int ignore_fsid_mismatch:1;
	/* Don't verify checksums at all */
	unsigned int skip_csum_check:1;
	unsigned int ignore_chunk_tree_error:1;
	unsigned int avoid_meta_chunk_alloc:1;
	unsigned int avoid_sys_chunk_alloc:1;
	unsigned int finalize_on_close:1;
	unsigned int hide_names:1;
	unsigned int allow_transid_mismatch:1;

	int transaction_aborted;
	int force_csum_type;

	int (*free_extent_hook)(u64 bytenr, u64 num_bytes, u64 parent,
				u64 root_objectid, u64 owner, u64 offset,
				int refs_to_drop);
	struct cache_tree *fsck_extent_cache;
	struct cache_tree *corrupt_blocks;

	/*
	 * For converting to/from bg tree feature, this records the bytenr
	 * of the last processed block group item.
	 *
	 * Any new block group item after this bytenr is using the target
	 * block group item format. (e.g. if converting to bg tree, bg item
	 * after this bytenr should go into block group tree).
	 *
	 * Thus the number should decrease as our convert progress goes.
	 */
	u64 last_converted_bg_bytenr;

	/* Cached block sizes */
	u32 nodesize;
	u32 sectorsize;
	u32 stripesize;
	u32 leaf_data_size;
	u16 csum_type;
	u16 csum_size;

	/*
	 * Zone size > 0 when in ZONED mode, otherwise it's used for a check
	 * if the mode is enabled
	 */
	union {
		u64 zone_size;
		u64 zoned;
	};

	struct super_block *sb;
};

static inline bool btrfs_is_zoned(const struct btrfs_fs_info *fs_info)
{
	return fs_info->zoned != 0;
}

/*
 * The state of btrfs root
 */
enum {
	/*
	 * btrfs_record_root_in_trans is a multi-step process, and it can race
	 * with the balancing code.   But the race is very small, and only the
	 * first time the root is added to each transaction.  So IN_TRANS_SETUP
	 * is used to tell us when more checks are required
	 */
	BTRFS_ROOT_IN_TRANS_SETUP,

	/*
	 * Set if tree blocks of this root can be shared by other roots.
	 * Only subvolume trees and their reloc trees have this bit set.
	 * Conflicts with TRACK_DIRTY bit.
	 *
	 * This affects two things:
	 *
	 * - How balance works
	 *   For shareable roots, we need to use reloc tree and do path
	 *   replacement for balance, and need various pre/post hooks for
	 *   snapshot creation to handle them.
	 *
	 *   While for non-shareable trees, we just simply do a tree search
	 *   with COW.
	 *
	 * - How dirty roots are tracked
	 *   For shareable roots, btrfs_record_root_in_trans() is needed to
	 *   track them, while non-subvolume roots have TRACK_DIRTY bit, they
	 *   don't need to set this manually.
	 */
	BTRFS_ROOT_SHAREABLE,
	BTRFS_ROOT_TRACK_DIRTY,
	BTRFS_ROOT_IN_RADIX,
	BTRFS_ROOT_ORPHAN_ITEM_INSERTED,
	BTRFS_ROOT_DEFRAG_RUNNING,
	BTRFS_ROOT_FORCE_COW,
	BTRFS_ROOT_MULTI_LOG_TASKS,
	BTRFS_ROOT_DIRTY,
	BTRFS_ROOT_DELETING,

	/*
	 * Reloc tree is orphan, only kept here for qgroup delayed subtree scan
	 *
	 * Set for the subvolume tree owning the reloc tree.
	 */
	BTRFS_ROOT_DEAD_RELOC_TREE,
	/* Mark dead root stored on device whose cleanup needs to be resumed */
	BTRFS_ROOT_DEAD_TREE,
	/* The root has a log tree. Used for subvolume roots and the tree root. */
	BTRFS_ROOT_HAS_LOG_TREE,
	/* Qgroup flushing is in progress */
	BTRFS_ROOT_QGROUP_FLUSHING,
	/* We started the orphan cleanup for this root. */
	BTRFS_ROOT_ORPHAN_CLEANUP,
	/* This root has a drop operation that was started previously. */
	BTRFS_ROOT_UNFINISHED_DROP,
	/* This reloc root needs to have its buffers lockdep class reset. */
	BTRFS_ROOT_RESET_LOCKDEP_CLASS,
};

/*
 * in ram representation of the tree.  extent_root is used for all allocations
 * and for the extent tree extent_root root.
 */
struct btrfs_root {
	struct extent_buffer *node;
	struct extent_buffer *commit_root;
	struct btrfs_root_item root_item;
	struct btrfs_key root_key;
	struct btrfs_fs_info *fs_info;
	u64 objectid;
	u64 last_trans;

	unsigned long state;

	u32 type;
	u64 last_inode_alloc;

	struct list_head unaligned_extent_recs;

	/* the dirty list is only used by non-reference counted roots */
	struct list_head dirty_list;
	struct rb_node rb_node;
};

static inline u32 BTRFS_MAX_ITEM_SIZE(const struct btrfs_fs_info *info)
{
	return BTRFS_LEAF_DATA_SIZE(info) - sizeof(struct btrfs_item);
}

static inline u32 BTRFS_NODEPTRS_PER_BLOCK(const struct btrfs_fs_info *info)
{
	return BTRFS_LEAF_DATA_SIZE(info) / sizeof(struct btrfs_key_ptr);
}

static inline u32 BTRFS_NODEPTRS_PER_EXTENT_BUFFER(const struct extent_buffer *eb)
{
	BUG_ON(!eb->fs_info);
	BUG_ON(eb->fs_info->nodesize != eb->len);
	return BTRFS_LEAF_DATA_SIZE(eb->fs_info) / sizeof(struct btrfs_key_ptr);
}

static inline u32 BTRFS_MAX_XATTR_SIZE(const struct btrfs_fs_info *info)
{
	return BTRFS_MAX_ITEM_SIZE(info) - sizeof(struct btrfs_dir_item);
}

/*
 * inode items have the data typically returned from stat and store other
 * info about object characteristics.  There is one for every file and dir in
 * the FS
 */
#define BTRFS_INODE_ITEM_KEY		1
#define BTRFS_INODE_REF_KEY		12
#define BTRFS_INODE_EXTREF_KEY		13
#define BTRFS_XATTR_ITEM_KEY		24

#define BTRFS_VERITY_DESC_ITEM_KEY	36
#define BTRFS_VERITY_MERKLE_ITEM_KEY	37

#define BTRFS_ORPHAN_ITEM_KEY		48

#define BTRFS_DIR_LOG_ITEM_KEY  60
#define BTRFS_DIR_LOG_INDEX_KEY 72
/*
 * dir items are the name -> inode pointers in a directory.  There is one
 * for every name in a directory.
 */
#define BTRFS_DIR_ITEM_KEY	84
#define BTRFS_DIR_INDEX_KEY	96

/*
 * extent data is for file data
 */
#define BTRFS_EXTENT_DATA_KEY	108

/*
 * csum items have the checksums for data in the extents
 */
#define BTRFS_CSUM_ITEM_KEY	120
/*
 * extent csums are stored in a separate tree and hold csums for
 * an entire extent on disk.
 */
#define BTRFS_EXTENT_CSUM_KEY	128

/*
 * root items point to tree roots.  There are typically in the root
 * tree used by the super block to find all the other trees
 */
#define BTRFS_ROOT_ITEM_KEY	132

/*
 * root backrefs tie subvols and snapshots to the directory entries that
 * reference them
 */
#define BTRFS_ROOT_BACKREF_KEY	144

/*
 * root refs make a fast index for listing all of the snapshots and
 * subvolumes referenced by a given root.  They point directly to the
 * directory item in the root that references the subvol
 */
#define BTRFS_ROOT_REF_KEY	156

/*
 * extent items are in the extent map tree.  These record which blocks
 * are used, and how many references there are to each block
 */
#define BTRFS_EXTENT_ITEM_KEY	168

/*
 * The same as the BTRFS_EXTENT_ITEM_KEY, except it's metadata we already know
 * the length, so we save the level in key->offset instead of the length.
 */
#define BTRFS_METADATA_ITEM_KEY	169

#define BTRFS_TREE_BLOCK_REF_KEY	176

#define BTRFS_EXTENT_DATA_REF_KEY	178

/* old style extent backrefs */
#define BTRFS_EXTENT_REF_V0_KEY		180

#define BTRFS_SHARED_BLOCK_REF_KEY	182

#define BTRFS_SHARED_DATA_REF_KEY	184


/*
 * block groups give us hints into the extent allocation trees.  Which
 * blocks are free etc etc
 */
#define BTRFS_BLOCK_GROUP_ITEM_KEY 192

/*
 * Every block group is represented in the free space tree by a free space info
 * item, which stores some accounting information. It is keyed on
 * (block_group_start, FREE_SPACE_INFO, block_group_length).
 */
#define BTRFS_FREE_SPACE_INFO_KEY 198

/*
 * A free space extent tracks an extent of space that is free in a block group.
 * It is keyed on (start, FREE_SPACE_EXTENT, length).
 */
#define BTRFS_FREE_SPACE_EXTENT_KEY 199

/*
 * When a block group becomes very fragmented, we convert it to use bitmaps
 * instead of extents. A free space bitmap is keyed on
 * (start, FREE_SPACE_BITMAP, length); the corresponding item is a bitmap with
 * (length / sectorsize) bits.
 */
#define BTRFS_FREE_SPACE_BITMAP_KEY 200

#define BTRFS_DEV_EXTENT_KEY	204
#define BTRFS_DEV_ITEM_KEY	216
#define BTRFS_CHUNK_ITEM_KEY	228

#define BTRFS_BALANCE_ITEM_KEY	248

/*
 * quota groups
 */
#define BTRFS_QGROUP_STATUS_KEY		240
#define BTRFS_QGROUP_INFO_KEY		242
#define BTRFS_QGROUP_LIMIT_KEY		244
#define BTRFS_QGROUP_RELATION_KEY	246

/*
 * Obsolete name, see BTRFS_TEMPORARY_ITEM_KEY.
 */
#define BTRFS_BALANCE_ITEM_KEY	248

/*
 * The key type for tree items that are stored persistently, but do not need to
 * exist for extended period of time. The items can exist in any tree.
 *
 * [subtype, BTRFS_TEMPORARY_ITEM_KEY, data]
 *
 * Existing items:
 *
 * - balance status item (objectid -4)
 *   (BTRFS_BALANCE_OBJECTID, BTRFS_TEMPORARY_ITEM_KEY, 0)
 *
 * - second csum tree for conversion (objecitd
 */
#define BTRFS_TEMPORARY_ITEM_KEY	248

/*
 * Temporary value
 *
 * root tree pointer of checksum tree with new checksum type
 */
#define BTRFS_CSUM_TREE_TMP_OBJECTID	13ULL

/*
 * Obsolete name, see BTRFS_PERSISTENT_ITEM_KEY
 */
#define BTRFS_DEV_STATS_KEY		249

/*
 * The key type for tree items that are stored persistently and usually exist
 * for a long period, eg. filesystem lifetime. The item kinds can be status
 * information, stats or preference values. The item can exist in any tree.
 *
 * [subtype, BTRFS_PERSISTENT_ITEM_KEY, data]
 *
 * Existing items:
 *
 * - device statistics, store IO stats in the device tree, one key for all
 *   stats
 *   (BTRFS_DEV_STATS_OBJECTID, BTRFS_DEV_STATS_KEY, 0)
 */
#define BTRFS_PERSISTENT_ITEM_KEY	249

/*
 * Persistently stores the device replace state in the device tree.
 * The key is built like this: (0, BTRFS_DEV_REPLACE_KEY, 0).
 */
#define BTRFS_DEV_REPLACE_KEY	250

/*
 * Stores items that allow to quickly map UUIDs to something else.
 * These items are part of the filesystem UUID tree.
 * The key is built like this:
 * (UUID_upper_64_bits, BTRFS_UUID_KEY*, UUID_lower_64_bits).
 */
#if BTRFS_UUID_SIZE != 16
#error "UUID items require BTRFS_UUID_SIZE == 16!"
#endif
#define BTRFS_UUID_KEY_SUBVOL	251	/* for UUIDs assigned to subvols */
#define BTRFS_UUID_KEY_RECEIVED_SUBVOL	252	/* for UUIDs assigned to
						 * received subvols */

/*
 * string items are for debugging.  They just store a short string of
 * data in the FS
 */
#define BTRFS_STRING_ITEM_KEY	253

static inline unsigned long btrfs_header_fsid(void)
{
	return offsetof(struct btrfs_header, fsid);
}

static inline unsigned long btrfs_header_chunk_tree_uuid(struct extent_buffer *eb)
{
	return offsetof(struct btrfs_header, chunk_tree_uuid);
}

static inline struct btrfs_timespec *
btrfs_inode_atime(struct btrfs_inode_item *inode_item)
{
	unsigned long ptr = (unsigned long)inode_item;
	ptr += offsetof(struct btrfs_inode_item, atime);
	return (struct btrfs_timespec *)ptr;
}

static inline struct btrfs_timespec *
btrfs_inode_mtime(struct btrfs_inode_item *inode_item)
{
	unsigned long ptr = (unsigned long)inode_item;
	ptr += offsetof(struct btrfs_inode_item, mtime);
	return (struct btrfs_timespec *)ptr;
}

static inline struct btrfs_timespec *
btrfs_inode_ctime(struct btrfs_inode_item *inode_item)
{
	unsigned long ptr = (unsigned long)inode_item;
	ptr += offsetof(struct btrfs_inode_item, ctime);
	return (struct btrfs_timespec *)ptr;
}

static inline struct btrfs_timespec *
btrfs_inode_otime(struct btrfs_inode_item *inode_item)
{
	unsigned long ptr = (unsigned long)inode_item;
	ptr += offsetof(struct btrfs_inode_item, otime);
	return (struct btrfs_timespec *)ptr;
}

static inline struct btrfs_timespec* btrfs_root_ctime(
		struct btrfs_root_item *root_item)
{
	unsigned long ptr = (unsigned long)root_item;
	ptr += offsetof(struct btrfs_root_item, ctime);
	return (struct btrfs_timespec *)ptr;
}

static inline struct btrfs_timespec* btrfs_root_otime(
		struct btrfs_root_item *root_item)
{
	unsigned long ptr = (unsigned long)root_item;
	ptr += offsetof(struct btrfs_root_item, otime);
	return (struct btrfs_timespec *)ptr;
}

static inline struct btrfs_timespec* btrfs_root_stime(
		struct btrfs_root_item *root_item)
{
	unsigned long ptr = (unsigned long)root_item;
	ptr += offsetof(struct btrfs_root_item, stime);
	return (struct btrfs_timespec *)ptr;
}

static inline struct btrfs_timespec* btrfs_root_rtime(
		struct btrfs_root_item *root_item)
{
	unsigned long ptr = (unsigned long)root_item;
	ptr += offsetof(struct btrfs_root_item, rtime);
	return (struct btrfs_timespec *)ptr;
}

static inline u8 *btrfs_dev_extent_chunk_tree_uuid(struct btrfs_dev_extent *dev)
{
	unsigned long ptr = offsetof(struct btrfs_dev_extent, chunk_tree_uuid);
       return (u8 *)((unsigned long)dev + ptr);
}

static inline u64 btrfs_dev_stats_value(const struct extent_buffer *eb,
					const struct btrfs_dev_stats_item *ptr,
					int index)
{
	u64 val;

	read_extent_buffer(eb, &val,
			   offsetof(struct btrfs_dev_stats_item, values) +
			   ((unsigned long)ptr) + (index * sizeof(u64)),
			   sizeof(val));
	return val;
}

/* struct btrfs_ioctl_search_header */
static inline u64 btrfs_search_header_transid(struct btrfs_ioctl_search_header *sh)
{
	return get_unaligned_64(&sh->transid);
}

static inline u64 btrfs_search_header_objectid(struct btrfs_ioctl_search_header *sh)
{
	return get_unaligned_64(&sh->objectid);
}

static inline u64 btrfs_search_header_offset(struct btrfs_ioctl_search_header *sh)
{
	return get_unaligned_64(&sh->offset);
}

static inline u32 btrfs_search_header_type(struct btrfs_ioctl_search_header *sh)
{
	return get_unaligned_32(&sh->type);
}

static inline u32 btrfs_search_header_len(struct btrfs_ioctl_search_header *sh)
{
	return get_unaligned_32(&sh->len);
}

#define btrfs_fs_incompat(fs_info, opt) \
	__btrfs_fs_incompat((fs_info), BTRFS_FEATURE_INCOMPAT_##opt)

static inline bool __btrfs_fs_incompat(struct btrfs_fs_info *fs_info, u64 flag)
{
	struct btrfs_super_block *disk_super;
	disk_super = fs_info->super_copy;
	return !!(btrfs_super_incompat_flags(disk_super) & flag);
}

#define btrfs_fs_compat_ro(fs_info, opt) \
	__btrfs_fs_compat_ro((fs_info), BTRFS_FEATURE_COMPAT_RO_##opt)

static inline int __btrfs_fs_compat_ro(struct btrfs_fs_info *fs_info, u64 flag)
{
	struct btrfs_super_block *disk_super;
	disk_super = fs_info->super_copy;
	return !!(btrfs_super_compat_ro_flags(disk_super) & flag);
}

u64 btrfs_name_hash(const char *name, int len);
u64 btrfs_extref_hash(u64 parent_objectid, const char *name, int len);

/* extent-tree.c */
int btrfs_reserve_extent(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 u64 num_bytes, u64 empty_size,
			 u64 hint_byte, u64 search_end,
			 struct btrfs_key *ins, bool is_data);
int btrfs_fix_block_accounting(struct btrfs_trans_handle *trans);
void btrfs_pin_extent(struct btrfs_fs_info *fs_info, u64 bytenr, u64 num_bytes);
void btrfs_unpin_extent(struct btrfs_fs_info *fs_info,
			u64 bytenr, u64 num_bytes);
struct btrfs_block_group *btrfs_lookup_block_group(struct btrfs_fs_info *info,
						   u64 bytenr);
struct btrfs_block_group *btrfs_lookup_first_block_group(struct
						       btrfs_fs_info *info,
						       u64 bytenr);
struct extent_buffer *btrfs_alloc_tree_block(struct btrfs_trans_handle *trans,
					struct btrfs_root *root,
					u32 blocksize, u64 root_objectid,
					struct btrfs_disk_key *key, int level,
					u64 hint, u64 empty_size,
					enum btrfs_lock_nesting nest);
int btrfs_lookup_extent_info(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info, u64 bytenr,
			     u64 offset, int metadata, u64 *refs, u64 *flags);
int btrfs_set_disk_extent_flags(struct btrfs_trans_handle *trans, u64 bytenr,
				int level, u64 flags);
int btrfs_inc_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct extent_buffer *buf, int record_parent);
int btrfs_dec_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct extent_buffer *buf, int record_parent);
int btrfs_free_tree_block(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root,
			  struct extent_buffer *buf,
			  u64 parent, int last_ref);
int btrfs_free_extent(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root,
		      u64 bytenr, u64 num_bytes, u64 parent,
		      u64 root_objectid, u64 owner, u64 offset);
void btrfs_finish_extent_commit(struct btrfs_trans_handle *trans);
int btrfs_inc_extent_ref(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 u64 bytenr, u64 num_bytes, u64 parent,
			 u64 root_objectid, u64 owner, u64 offset);
int btrfs_update_extent_ref(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root, u64 bytenr,
			    u64 orig_parent, u64 parent,
			    u64 root_objectid, u64 ref_generation,
			    u64 owner_objectid);
int btrfs_write_dirty_block_groups(struct btrfs_trans_handle *trans);
int update_space_info(struct btrfs_fs_info *info, u64 flags,
		      u64 total_bytes, u64 bytes_used,
		      struct btrfs_space_info **space_info);
int btrfs_free_block_groups(struct btrfs_fs_info *info);
int btrfs_read_block_groups(struct btrfs_fs_info *info);
struct btrfs_block_group *
btrfs_add_block_group(struct btrfs_fs_info *fs_info, u64 bytes_used, u64 type,
		      u64 chunk_offset, u64 size);
int btrfs_make_block_group(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *fs_info, u64 bytes_used,
			   u64 type, u64 chunk_offset, u64 size);
int btrfs_make_block_groups(struct btrfs_trans_handle *trans,
			    struct btrfs_fs_info *fs_info);
int btrfs_update_block_group(struct btrfs_trans_handle *trans, u64 bytenr,
			     u64 num, int alloc, int mark_free);
int btrfs_record_file_extent(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *inode,
			      u64 file_pos, u64 disk_bytenr,
			      u64 num_bytes);
int btrfs_remove_block_group(struct btrfs_trans_handle *trans,
			     u64 bytenr, u64 len);
void free_excluded_extents(struct btrfs_fs_info *fs_info,
			   struct btrfs_block_group *cache);
int exclude_super_stripes(struct btrfs_fs_info *fs_info,
			  struct btrfs_block_group *cache);
u64 add_new_free_space(struct btrfs_block_group *block_group,
		       struct btrfs_fs_info *info, u64 start, u64 end);
u64 hash_extent_data_ref(u64 root_objectid, u64 owner, u64 offset);
int btrfs_convert_one_bg(struct btrfs_trans_handle *trans, u64 bytenr);

/* ctree.c */
int btrfs_comp_cpu_keys(const struct btrfs_key *k1, const struct btrfs_key *k2);
int btrfs_del_ptr(struct btrfs_root *root, struct btrfs_path *path,
		int level, int slot);
enum btrfs_tree_block_status btrfs_check_node(struct extent_buffer *buf);
enum btrfs_tree_block_status btrfs_check_leaf(struct extent_buffer *buf);
void reada_for_search(struct btrfs_fs_info *fs_info, struct btrfs_path *path,
		      int level, int slot, u64 objectid);
struct extent_buffer *read_node_slot(struct btrfs_fs_info *fs_info,
				   struct extent_buffer *parent, int slot);
int btrfs_previous_item(struct btrfs_root *root,
			struct btrfs_path *path, u64 min_objectid,
			int type);
int btrfs_previous_extent_item(struct btrfs_root *root,
			struct btrfs_path *path, u64 min_objectid);
int btrfs_next_extent_item(struct btrfs_root *root,
			struct btrfs_path *path, u64 max_objectid);
int btrfs_cow_block(struct btrfs_trans_handle *trans,
		    struct btrfs_root *root, struct extent_buffer *buf,
		    struct extent_buffer *parent, int parent_slot,
		    struct extent_buffer **cow_ret);
int __btrfs_cow_block(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     struct extent_buffer *buf,
			     struct extent_buffer *parent, int parent_slot,
			     struct extent_buffer **cow_ret,
			     u64 search_start, u64 empty_size);
int btrfs_copy_root(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root,
		      struct extent_buffer *buf,
		      struct extent_buffer **cow_ret, u64 new_root_objectid);
int btrfs_create_root(struct btrfs_trans_handle *trans,
		      struct btrfs_fs_info *fs_info, u64 objectid);
int btrfs_extend_item(struct btrfs_root *root, struct btrfs_path *path,
		u32 data_size);
int btrfs_truncate_item(struct btrfs_path *path, u32 new_size, int from_end);
int btrfs_split_item(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root,
		     struct btrfs_path *path,
		     struct btrfs_key *new_key,
		     unsigned long split_offset);
int btrfs_search_slot(struct btrfs_trans_handle *trans,
		struct btrfs_root *root, const struct btrfs_key *key,
		struct btrfs_path *p, int ins_len, int cow);
int btrfs_search_slot_for_read(struct btrfs_root *root,
                               const struct btrfs_key *key,
                               struct btrfs_path *p, int find_higher,
                               int return_any);
int btrfs_bin_search(struct extent_buffer *eb, const struct btrfs_key *key,
		     int *slot);
int btrfs_find_item(struct btrfs_root *fs_root, struct btrfs_path *found_path,
		u64 iobjectid, u64 ioff, u8 key_type,
		struct btrfs_key *found_key);
void btrfs_release_path(struct btrfs_path *p);
void add_root_to_dirty_list(struct btrfs_root *root);
struct btrfs_path *btrfs_alloc_path(void);
void btrfs_free_path(struct btrfs_path *p);
void btrfs_init_path(struct btrfs_path *p);
int btrfs_del_items(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		   struct btrfs_path *path, int slot, int nr);

static inline int btrfs_del_item(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path)
{
	return btrfs_del_items(trans, root, path, path->slots[0], 1);
}

int btrfs_insert_item(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_key *key, void *data, u32 data_size);
int btrfs_insert_empty_items(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     struct btrfs_path *path,
			     struct btrfs_key *cpu_key, u32 *data_size, int nr);

static inline int btrfs_insert_empty_item(struct btrfs_trans_handle *trans,
					  struct btrfs_root *root,
					  struct btrfs_path *path,
					  struct btrfs_key *key,
					  u32 data_size)
{
	return btrfs_insert_empty_items(trans, root, path, key, &data_size, 1);
}

int btrfs_next_sibling_tree_block(struct btrfs_fs_info *fs_info,
				  struct btrfs_path *path);

/*
 * Walk up the tree as far as necessary to find the next leaf.
 *
 * returns 0 if it found something or 1 if there are no greater leaves.
 * returns < 0 on io errors.
 */
static inline int btrfs_next_leaf(struct btrfs_root *root,
				  struct btrfs_path *path)
{
	path->lowest_level = 0;
	return btrfs_next_sibling_tree_block(root->fs_info, path);
}

static inline int btrfs_next_item(struct btrfs_root *root,
				  struct btrfs_path *p)
{
	++p->slots[0];
	if (p->slots[0] >= btrfs_header_nritems(p->nodes[0])) {
		int ret;
		ret = btrfs_next_leaf(root, p);
		/*
		 * Revert the increased slot, or the path may point to
		 * an invalid item.
		 */
		if (ret)
			p->slots[0]--;
		return ret;
	}
	return 0;
}

int btrfs_prev_leaf(struct btrfs_root *root, struct btrfs_path *path);
int btrfs_leaf_free_space(struct extent_buffer *leaf);
void btrfs_fixup_low_keys(struct btrfs_path *path, struct btrfs_disk_key *key,
		int level);
int btrfs_set_item_key_safe(struct btrfs_root *root, struct btrfs_path *path,
			    struct btrfs_key *new_key);
void btrfs_set_item_key_unsafe(struct btrfs_root *root,
			       struct btrfs_path *path,
			       struct btrfs_key *new_key);

u16 btrfs_super_csum_size(const struct btrfs_super_block *s);
const char *btrfs_super_csum_name(u16 csum_type);
u16 btrfs_csum_type_size(u16 csum_type);
size_t btrfs_super_num_csums(void);

/* root-item.c */
int btrfs_add_root_ref(struct btrfs_trans_handle *trans,
		       struct btrfs_root *tree_root,
		       u64 root_id, u8 type, u64 ref_id,
		       u64 dirid, u64 sequence,
		       const char *name, int name_len);
int btrfs_insert_root(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_key *key, struct btrfs_root_item
		      *item);
int btrfs_del_root(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		   struct btrfs_key *key);
int btrfs_update_root(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_key *key, struct btrfs_root_item
		      *item);
int btrfs_find_last_root(struct btrfs_root *root, u64 objectid, struct
			 btrfs_root_item *item, struct btrfs_key *key);
/* dir-item.c */
int btrfs_insert_dir_item(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, const char *name, int name_len, u64 dir,
			  struct btrfs_key *location, u8 type, u64 index);
struct btrfs_dir_item *btrfs_lookup_dir_item(struct btrfs_trans_handle *trans,
					     struct btrfs_root *root,
					     struct btrfs_path *path, u64 dir,
					     const char *name, int name_len,
					     int mod);
struct btrfs_dir_item *btrfs_lookup_dir_index_item(struct btrfs_trans_handle *trans,
					struct btrfs_root *root,
					struct btrfs_path *path, u64 dir,
					u64 objectid, const char *name, int name_len,
					int mod);
int btrfs_delete_one_dir_name(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root,
			      struct btrfs_path *path,
			      struct btrfs_dir_item *di);
int btrfs_insert_xattr_item(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root, const char *name,
			    u16 name_len, const void *data, u16 data_len,
			    u64 dir);
struct btrfs_dir_item *btrfs_match_dir_item_name(struct btrfs_root *root,
			      struct btrfs_path *path,
			      const char *name, int name_len);

/* inode-item.c */
int btrfs_insert_inode_ref(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   const char *name, int name_len,
			   u64 inode_objectid, u64 ref_objectid, u64 index);
int btrfs_insert_inode(struct btrfs_trans_handle *trans, struct btrfs_root
		       *root, u64 objectid, struct btrfs_inode_item
		       *inode_item);
int btrfs_lookup_inode(struct btrfs_trans_handle *trans, struct btrfs_root
		       *root, struct btrfs_path *path,
		       struct btrfs_key *location, int mod);
struct btrfs_inode_extref *btrfs_lookup_inode_extref(struct btrfs_trans_handle
		*trans, struct btrfs_path *path, struct btrfs_root *root,
		u64 ino, u64 parent_ino, u64 index, const char *name,
		int namelen, int ins_len);
int btrfs_del_inode_extref(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   const char *name, int name_len,
			   u64 inode_objectid, u64 ref_objectid,
			   u64 *index);
int btrfs_insert_inode_extref(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root,
			      const char *name, int name_len,
			      u64 inode_objectid, u64 ref_objectid, u64 index);
struct btrfs_inode_ref *btrfs_lookup_inode_ref(struct btrfs_trans_handle *trans,
		struct btrfs_root *root, struct btrfs_path *path,
		const char *name, int namelen, u64 ino, u64 parent_ino,
		int ins_len);
int btrfs_del_inode_ref(struct btrfs_trans_handle *trans,
			struct btrfs_root *root, const char *name, int name_len,
			u64 ino, u64 parent_ino, u64 *index);

/* uuid-tree.c, interface for mounted mounted filesystem */
int btrfs_lookup_uuid_subvol_item(int fd, const u8 *uuid, u64 *subvol_id);
int btrfs_lookup_uuid_received_subvol_item(int fd, const u8 *uuid,
					   u64 *subvol_id);

/* uuid-tree.c, interface for unmounte filesystem */
int btrfs_uuid_tree_add(struct btrfs_trans_handle *trans, u8 *uuid, u8 type,
			u64 subvol_id_cpu);
int btrfs_uuid_tree_remove(struct btrfs_trans_handle *trans, u8 *uuid, u8 type,
			   u64 subid);

static inline int is_fstree(u64 rootid)
{
	if (rootid == BTRFS_FS_TREE_OBJECTID ||
	    (signed long long)rootid >= (signed long long)BTRFS_FIRST_FREE_OBJECTID)
		return 1;
	return 0;
}

void btrfs_uuid_to_key(const u8 *uuid, struct btrfs_key *key);

/* inode.c */
int check_dir_conflict(struct btrfs_root *root, char *name, int namelen,
		u64 dir, u64 index);
int btrfs_new_inode(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		u64 ino, u32 mode);
int btrfs_change_inode_flags(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 ino, u64 flags);
int btrfs_add_link(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		   u64 ino, u64 parent_ino, char *name, int namelen,
		   u8 type, u64 *index, int add_backref, int ignore_existed);
int btrfs_unlink(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		 u64 ino, u64 parent_ino, u64 index, const char *name,
		 int namelen, int add_orphan);
int btrfs_add_orphan_item(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root, struct btrfs_path *path,
			  u64 ino);
int btrfs_mkdir(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		char *name, int namelen, u64 parent_ino, u64 *ino, int mode);
struct btrfs_root *btrfs_mksubvol(struct btrfs_root *root, const char *base,
				  u64 root_objectid, bool convert);
int btrfs_find_free_objectid(struct btrfs_trans_handle *trans,
			     struct btrfs_root *fs_root,
			     u64 dirid, u64 *objectid);

/* file.c */
int btrfs_get_extent(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root,
		     struct btrfs_path *path,
		     u64 ino, u64 offset, u64 len, int ins_len);
int btrfs_punch_hole(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root,
		     u64 ino, u64 offset, u64 len);
int btrfs_read_file(struct btrfs_root *root, u64 ino, u64 start, int len,
		    char *dest);

/* extent-tree.c */
int btrfs_run_delayed_refs(struct btrfs_trans_handle *trans, unsigned long nr);

#endif
