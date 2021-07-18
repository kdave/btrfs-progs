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

/*
 * Defines and function declarations for original mode check.
 */

#ifndef __BTRFS_CHECK_MODE_ORIGINAL_H__
#define __BTRFS_CHECK_MODE_ORIGINAL_H__

#include "common/rbtree-utils.h"

struct extent_backref {
	struct rb_node node;
	unsigned int is_data:1;
	unsigned int found_extent_tree:1;
	unsigned int full_backref:1;
	unsigned int found_ref:1;
	unsigned int broken:1;
};

static inline struct extent_backref* rb_node_to_extent_backref(struct rb_node *node)
{
	return rb_entry(node, struct extent_backref, node);
}

struct data_backref {
	struct extent_backref node;
	union {
		u64 parent;
		u64 root;
	};
	u64 owner;
	u64 offset;
	u64 disk_bytenr;
	u64 bytes;
	u64 ram_bytes;
	u32 num_refs;
	u32 found_ref;
};

static inline struct data_backref* to_data_backref(struct extent_backref *back)
{
	return container_of(back, struct data_backref, node);
}

struct tree_backref {
	struct extent_backref node;
	union {
		u64 parent;
		u64 root;
	};
};

static inline struct tree_backref* to_tree_backref(struct extent_backref *back)
{
	return container_of(back, struct tree_backref, node);
}

/* Explicit initialization for extent_record::flag_block_full_backref */
enum { FLAG_UNSET = 2 };

struct extent_record {
	struct list_head backrefs;
	struct list_head dups;
	struct rb_root backref_tree;
	struct list_head list;
	struct cache_extent cache;
	struct btrfs_disk_key parent_key;
	u64 start;
	u64 max_size;
	u64 nr;
	u64 refs;
	u64 extent_item_refs;
	u64 generation;
	u64 parent_generation;
	u64 info_objectid;
	u32 num_duplicates;
	u8 info_level;
	unsigned int flag_block_full_backref:2;
	unsigned int found_rec:1;
	unsigned int content_checked:1;
	unsigned int owner_ref_checked:1;
	unsigned int is_root:1;
	unsigned int metadata:1;
	unsigned int bad_full_backref:1;
	unsigned int crossing_stripes:1;
	unsigned int wrong_chunk_type:1;
};

static inline struct extent_record* to_extent_record(struct list_head *entry)
{
	return container_of(entry, struct extent_record, list);
}

struct inode_backref {
	struct list_head list;
	unsigned int found_dir_item:1;
	unsigned int found_dir_index:1;
	unsigned int found_inode_ref:1;
	u8 filetype;
	u8 ref_type;
	int errors;
	u64 dir;
	u64 index;
	u16 namelen;
	char name[0];
};

static inline struct inode_backref* to_inode_backref(struct list_head *entry)
{
	return list_entry(entry, struct inode_backref, list);
}

struct root_item_record {
	struct list_head list;
	u64 objectid;
	u64 bytenr;
	u64 last_snapshot;
	u8 level;
	u8 drop_level;
	struct btrfs_key drop_key;
};

#define REF_ERR_NO_DIR_ITEM		(1 << 0)
#define REF_ERR_NO_DIR_INDEX		(1 << 1)
#define REF_ERR_NO_INODE_REF		(1 << 2)
#define REF_ERR_DUP_DIR_ITEM		(1 << 3)
#define REF_ERR_DUP_DIR_INDEX		(1 << 4)
#define REF_ERR_DUP_INODE_REF		(1 << 5)
#define REF_ERR_INDEX_UNMATCH		(1 << 6)
#define REF_ERR_FILETYPE_UNMATCH	(1 << 7)
#define REF_ERR_NAME_TOO_LONG		(1 << 8) // 100
#define REF_ERR_NO_ROOT_REF		(1 << 9)
#define REF_ERR_NO_ROOT_BACKREF		(1 << 10)
#define REF_ERR_DUP_ROOT_REF		(1 << 11)
#define REF_ERR_DUP_ROOT_BACKREF	(1 << 12)

struct file_extent_hole {
	struct rb_node node;
	u64 start;
	u64 len;
};

struct unaligned_extent_rec_t {
	struct list_head list;

	u64 objectid;
	u64 owner;
	u64 offset;

	u64 bytenr;
};

#define I_ERR_NO_INODE_ITEM		(1 << 0)
#define I_ERR_NO_ORPHAN_ITEM		(1 << 1)
#define I_ERR_DUP_INODE_ITEM		(1 << 2)
#define I_ERR_DUP_DIR_INDEX		(1 << 3)
#define I_ERR_ODD_DIR_ITEM		(1 << 4)
#define I_ERR_ODD_FILE_EXTENT		(1 << 5)
#define I_ERR_BAD_FILE_EXTENT		(1 << 6)
#define I_ERR_FILE_EXTENT_OVERLAP	(1 << 7)
#define I_ERR_FILE_EXTENT_DISCOUNT	(1 << 8) // 100
#define I_ERR_DIR_ISIZE_WRONG		(1 << 9)
#define I_ERR_FILE_NBYTES_WRONG		(1 << 10) // 400
#define I_ERR_ODD_CSUM_ITEM		(1 << 11)
#define I_ERR_SOME_CSUM_MISSING		(1 << 12)
#define I_ERR_LINK_COUNT_WRONG		(1 << 13)
#define I_ERR_UNALIGNED_EXTENT_REC	(1 << 14)
#define I_ERR_FILE_EXTENT_TOO_LARGE	(1 << 15)
#define I_ERR_ODD_INODE_FLAGS		(1 << 16)
#define I_ERR_INLINE_RAM_BYTES_WRONG	(1 << 17)
#define I_ERR_MISMATCH_DIR_HASH		(1 << 18)
#define I_ERR_INVALID_IMODE		(1 << 19)
#define I_ERR_INVALID_GEN		(1 << 20)
#define I_ERR_INVALID_NLINK		(1 << 21)

struct inode_record {
	struct list_head backrefs;
	unsigned int checked:1;
	unsigned int merging:1;
	unsigned int found_inode_item:1;
	unsigned int found_dir_item:1;
	unsigned int found_file_extent:1;
	unsigned int found_csum_item:1;
	unsigned int some_csum_missing:1;
	unsigned int nodatasum:1;
	int errors;

	struct list_head unaligned_extent_recs;

	u64 ino;
	u32 nlink;
	u32 imode;
	u64 isize;
	u64 nbytes;

	u32 found_link;
	u64 found_size;
	u64 extent_start;
	u64 extent_end;
	struct rb_root holes;
	struct list_head mismatch_dir_hash;

	u32 refs;
};

/*
 * To record one dir_item with mismatch hash.
 *
 * Since the hash is incorrect, we must record the hash (key).
 */
struct mismatch_dir_hash_record {
	struct list_head list;
	struct btrfs_key key;
	int namelen;
	/* namebuf follows here */
};

struct root_backref {
	struct list_head list;
	unsigned int found_dir_item:1;
	unsigned int found_dir_index:1;
	unsigned int found_back_ref:1;
	unsigned int found_forward_ref:1;
	unsigned int reachable:1;
	int errors;
	u64 ref_root;
	u64 dir;
	u64 index;
	u16 namelen;
	char name[0];
};

static inline struct root_backref* to_root_backref(struct list_head *entry)
{
	return list_entry(entry, struct root_backref, list);
}

struct root_record {
	struct list_head backrefs;
	struct cache_extent cache;
	unsigned int found_root_item:1;
	u64 objectid;
	u32 found_ref;
};

struct ptr_node {
	struct cache_extent cache;
	void *data;
};

struct shared_node {
	struct cache_extent cache;
	struct cache_tree root_cache;
	struct cache_tree inode_cache;
	struct inode_record *current;
	u32 refs;
};

struct block_info {
	u64 start;
	u32 size;
};

struct walk_control {
	struct cache_tree shared;
	struct shared_node *nodes[BTRFS_MAX_LEVEL];
	int active_node;
	int root_level;
};

struct bad_item {
	struct btrfs_key key;
	u64 root_id;
	struct list_head list;
};

struct extent_entry {
	u64 bytenr;
	u64 bytes;
	int count;
	int broken;
	struct list_head list;
};

struct root_item_info {
	/* level of the root */
	u8 level;
	/* number of nodes at this level, must be 1 for a root */
	int node_count;
	u64 bytenr;
	u64 gen;
	struct cache_extent cache_extent;
};

#endif
