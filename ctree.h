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

#ifndef __BTRFS__
#define __BTRFS__

#include "list.h"
#include "kerncompat.h"
#include "radix-tree.h"
#include "extent-cache.h"

struct btrfs_trans_handle;

#define BTRFS_MAGIC "_B2RfS_M"

#define BTRFS_ROOT_TREE_OBJECTID 1ULL
#define BTRFS_EXTENT_TREE_OBJECTID 2ULL
#define BTRFS_FS_TREE_OBJECTID 3ULL
#define BTRFS_ROOT_TREE_DIR_OBJECTID 4ULL
#define BTRFS_FIRST_FREE_OBJECTID 5ULL

/*
 * we can actually store much bigger names, but lets not confuse the rest
 * of linux
 */
#define BTRFS_NAME_LEN 255

/* 32 bytes in various csum fields */
#define BTRFS_CSUM_SIZE 32
/* four bytes for CRC32 */
#define BTRFS_CRC32_SIZE 4

#define BTRFS_FT_UNKNOWN	0
#define BTRFS_FT_REG_FILE	1
#define BTRFS_FT_DIR		2
#define BTRFS_FT_CHRDEV		3
#define BTRFS_FT_BLKDEV		4
#define BTRFS_FT_FIFO		5
#define BTRFS_FT_SOCK		6
#define BTRFS_FT_SYMLINK	7
#define BTRFS_FT_XATTR		8
#define BTRFS_FT_MAX		9

/*
 * the key defines the order in the tree, and so it also defines (optimal)
 * block layout.  objectid corresonds to the inode number.  The flags
 * tells us things about the object, and is a kind of stream selector.
 * so for a given inode, keys with flags of 1 might refer to the inode
 * data, flags of 2 may point to file data in the btree and flags == 3
 * may point to extents.
 *
 * offset is the starting byte offset for this key in the stream.
 *
 * btrfs_disk_key is in disk byte order.  struct btrfs_key is always
 * in cpu native order.  Otherwise they are identical and their sizes
 * should be the same (ie both packed)
 */
struct btrfs_disk_key {
	__le64 objectid;
	u8 type;
	__le64 offset;
} __attribute__ ((__packed__));

struct btrfs_key {
	u64 objectid;
	u8 type;
	u64 offset;
} __attribute__ ((__packed__));

/*
 * every tree block (leaf or node) starts with this header.
 */
struct btrfs_header {
	u8 csum[BTRFS_CSUM_SIZE];
	u8 fsid[16]; /* FS specific uuid */
	__le64 bytenr; /* which block this node is supposed to live in */
	__le64 generation;
	__le64 owner;
	__le32 nritems;
	__le16 flags;
	u8 level;
} __attribute__ ((__packed__));

#define BTRFS_MAX_LEVEL 8
#define BTRFS_NODEPTRS_PER_BLOCK(r) (((r)->nodesize - \
			        sizeof(struct btrfs_header)) / \
			       (sizeof(struct btrfs_disk_key) + sizeof(u64)))
#define __BTRFS_LEAF_DATA_SIZE(bs) ((bs) - sizeof(struct btrfs_header))
#define BTRFS_LEAF_DATA_SIZE(r) (__BTRFS_LEAF_DATA_SIZE(r->leafsize))

struct btrfs_buffer;
/*
 * the super block basically lists the main trees of the FS
 * it currently lacks any block count etc etc
 */
struct btrfs_super_block {
	u8 csum[BTRFS_CSUM_SIZE];
	/* the first 3 fields must match struct btrfs_header */
	u8 fsid[16];    /* FS specific uuid */
	__le64 bytenr; /* this block number */
	__le64 magic;
	__le64 generation;
	__le64 root;
	__le64 total_bytes;
	__le64 bytes_used;
	__le64 root_dir_objectid;
	__le32 sectorsize;
	__le32 nodesize;
	__le32 leafsize;
	u8 root_level;
} __attribute__ ((__packed__));

/*
 * A leaf is full of items. offset and size tell us where to find
 * the item in the leaf (relative to the start of the data area)
 */
struct btrfs_item {
	struct btrfs_disk_key key;
	__le32 offset;
	__le32 size;
} __attribute__ ((__packed__));

/*
 * leaves have an item area and a data area:
 * [item0, item1....itemN] [free space] [dataN...data1, data0]
 *
 * The data is separate from the items to get the keys closer together
 * during searches.
 */
struct btrfs_leaf {
	struct btrfs_header header;
	struct btrfs_item items[];
} __attribute__ ((__packed__));

/*
 * all non-leaf blocks are nodes, they hold only keys and pointers to
 * other blocks
 */
struct btrfs_key_ptr {
	struct btrfs_disk_key key;
	__le64 blockptr;
} __attribute__ ((__packed__));

struct btrfs_node {
	struct btrfs_header header;
	struct btrfs_key_ptr ptrs[];
} __attribute__ ((__packed__));

/*
 * btrfs_paths remember the path taken from the root down to the leaf.
 * level 0 is always the leaf, and nodes[1...BTRFS_MAX_LEVEL] will point
 * to any other levels that are present.
 *
 * The slots array records the index of the item or block pointer
 * used while walking the tree.
 */
struct btrfs_path {
	struct btrfs_buffer *nodes[BTRFS_MAX_LEVEL];
	int slots[BTRFS_MAX_LEVEL];
};

/*
 * items in the extent btree are used to record the objectid of the
 * owner of the block and the number of references
 */
struct btrfs_extent_item {
	__le32 refs;
	__le64 owner;
} __attribute__ ((__packed__));

struct btrfs_inode_timespec {
	__le64 sec;
	__le32 nsec;
} __attribute__ ((__packed__));

/*
 * there is no padding here on purpose.  If you want to extent the inode,
 * make a new item type
 */
struct btrfs_inode_item {
	__le64 generation;
	__le64 size;
	__le64 nblocks;
	__le64 block_group;
	__le32 nlink;
	__le32 uid;
	__le32 gid;
	__le32 mode;
	__le32 rdev;
	__le16 flags;
	__le16 compat_flags;
	struct btrfs_inode_timespec atime;
	struct btrfs_inode_timespec ctime;
	struct btrfs_inode_timespec mtime;
	struct btrfs_inode_timespec otime;
} __attribute__ ((__packed__));

/* inline data is just a blob of bytes */
struct btrfs_inline_data_item {
	u8 data;
} __attribute__ ((__packed__));

struct btrfs_dir_item {
	struct btrfs_disk_key location;
	__le16 data_len;
	__le16 name_len;
	u8 type;
} __attribute__ ((__packed__));

struct btrfs_root_item {
	struct btrfs_inode_item inode;
	__le64 root_dirid;
	__le64 bytenr;
	__le64 byte_limit;
	__le64 bytes_used;
	__le32 flags;
	__le32 refs;
	struct btrfs_disk_key drop_progress;
	u8 drop_level;
	u8 level;
} __attribute__ ((__packed__));

#define BTRFS_FILE_EXTENT_REG 0
#define BTRFS_FILE_EXTENT_INLINE 1

struct btrfs_file_extent_item {
	__le64 generation;
	u8 type;
	/*
	 * disk space consumed by the extent, checksum blocks are included
	 * in these numbers
	 */
	__le64 disk_bytenr;
	__le64 disk_num_bytes;
	/*
	 * the logical offset in file blocks (no csums)
	 * this extent record is for.  This allows a file extent to point
	 * into the middle of an existing extent on disk, sharing it
	 * between two snapshots (useful if some bytes in the middle of the
	 * extent have changed
	 */
	__le64 offset;
	/*
	 * the logical number of file blocks (no csums included)
	 */
	__le64 num_bytes;
} __attribute__ ((__packed__));

struct btrfs_csum_item {
	u8 csum[BTRFS_CSUM_SIZE];
} __attribute__ ((__packed__));

/* tag for the radix tree of block groups in ram */
#define BTRFS_BLOCK_GROUP_DIRTY 0
#define BTRFS_BLOCK_GROUP_SIZE (256 * 1024 * 1024)


#define BTRFS_BLOCK_GROUP_DATA 1
struct btrfs_block_group_item {
	__le64 used;
	u8 flags;
} __attribute__ ((__packed__));

struct btrfs_block_group_cache {
	struct cache_extent cache;
	struct btrfs_key key;
	struct btrfs_block_group_item item;
	int dirty;
};

struct btrfs_fs_info {
	struct btrfs_root *fs_root;
	struct btrfs_root *extent_root;
	struct btrfs_root *tree_root;
	struct btrfs_key last_insert;
	struct cache_tree extent_cache;
	struct cache_tree block_group_cache;
	struct cache_tree pending_tree;
	struct cache_tree pinned_tree;
	struct cache_tree del_pending;
	struct list_head trans;
	struct list_head cache;
	u64 last_inode_alloc;
	u64 last_inode_alloc_dirid;
	u64 generation;
	int cache_size;
	int fp;
	struct btrfs_trans_handle *running_transaction;
	struct btrfs_super_block *disk_super;
};

/*
 * in ram representation of the tree.  extent_root is used for all allocations
 * and for the extent tree extent_root root.
 */
struct btrfs_root {
	struct btrfs_buffer *node;
	struct btrfs_buffer *commit_root;
	struct btrfs_root_item root_item;
	struct btrfs_key root_key;
	struct btrfs_fs_info *fs_info;

	/* data allocations are done in sectorsize units */
	u32 sectorsize;

	/* node allocations are done in nodesize units */
	u32 nodesize;

	/* leaf allocations are done in leafsize units */
	u32 leafsize;

	int ref_cows;
	u32 type;
};

/* the lower bits in the key flags defines the item type */
#define BTRFS_KEY_TYPE_MAX	256
#define BTRFS_KEY_TYPE_SHIFT	24
#define BTRFS_KEY_TYPE_MASK	(((u32)BTRFS_KEY_TYPE_MAX - 1) << \
				  BTRFS_KEY_TYPE_SHIFT)

/*
 * inode items have the data typically returned from stat and store other
 * info about object characteristics.  There is one for every file and dir in
 * the FS
 */
#define BTRFS_INODE_ITEM_KEY		1
#define BTRFS_XATTR_ITEM_KEY            2

/* reserve 3-15 close to the inode for later flexibility */

/*
 * dir items are the name -> inode pointers in a directory.  There is one
 * for every name in a directory.
 */
#define BTRFS_DIR_ITEM_KEY	16
#define BTRFS_DIR_INDEX_KEY	17
/*
 * extent data is for file data
 */
#define BTRFS_EXTENT_DATA_KEY	18
/*
 * csum items have the checksums for data in the extents
 */
#define BTRFS_CSUM_ITEM_KEY	19

/* reserve 20-31 for other file stuff */

/*
 * root items point to tree roots.  There are typically in the root
 * tree used by the super block to find all the other trees
 */
#define BTRFS_ROOT_ITEM_KEY	32
/*
 * extent items are in the extent map tree.  These record which blocks
 * are used, and how many references there are to each block
 */
#define BTRFS_EXTENT_ITEM_KEY	33

/*
 * block groups give us hints into the extent allocation trees.  Which
 * blocks are free etc etc
 */
#define BTRFS_BLOCK_GROUP_ITEM_KEY 34

/*
 * string items are for debugging.  They just store a short string of
 * data in the FS
 */
#define BTRFS_STRING_ITEM_KEY	253


static inline u64 btrfs_block_group_used(struct btrfs_block_group_item *bi)
{
	return le64_to_cpu(bi->used);
}

static inline void btrfs_set_block_group_used(struct
						   btrfs_block_group_item *bi,
						   u64 val)
{
	bi->used = cpu_to_le64(val);
}

static inline u64 btrfs_inode_generation(struct btrfs_inode_item *i)
{
	return le64_to_cpu(i->generation);
}

static inline void btrfs_set_inode_generation(struct btrfs_inode_item *i,
					      u64 val)
{
	i->generation = cpu_to_le64(val);
}

static inline u64 btrfs_inode_size(struct btrfs_inode_item *i)
{
	return le64_to_cpu(i->size);
}

static inline void btrfs_set_inode_size(struct btrfs_inode_item *i, u64 val)
{
	i->size = cpu_to_le64(val);
}

static inline u64 btrfs_inode_nblocks(struct btrfs_inode_item *i)
{
	return le64_to_cpu(i->nblocks);
}

static inline void btrfs_set_inode_nblocks(struct btrfs_inode_item *i, u64 val)
{
	i->nblocks = cpu_to_le64(val);
}

static inline u64 btrfs_inode_block_group(struct btrfs_inode_item *i)
{
	return le64_to_cpu(i->block_group);
}

static inline void btrfs_set_inode_block_group(struct btrfs_inode_item *i,
						u64 val)
{
	i->block_group = cpu_to_le64(val);
}

static inline u32 btrfs_inode_nlink(struct btrfs_inode_item *i)
{
	return le32_to_cpu(i->nlink);
}

static inline void btrfs_set_inode_nlink(struct btrfs_inode_item *i, u32 val)
{
	i->nlink = cpu_to_le32(val);
}

static inline u32 btrfs_inode_uid(struct btrfs_inode_item *i)
{
	return le32_to_cpu(i->uid);
}

static inline void btrfs_set_inode_uid(struct btrfs_inode_item *i, u32 val)
{
	i->uid = cpu_to_le32(val);
}

static inline u32 btrfs_inode_gid(struct btrfs_inode_item *i)
{
	return le32_to_cpu(i->gid);
}

static inline void btrfs_set_inode_gid(struct btrfs_inode_item *i, u32 val)
{
	i->gid = cpu_to_le32(val);
}

static inline u32 btrfs_inode_mode(struct btrfs_inode_item *i)
{
	return le32_to_cpu(i->mode);
}

static inline void btrfs_set_inode_mode(struct btrfs_inode_item *i, u32 val)
{
	i->mode = cpu_to_le32(val);
}

static inline u32 btrfs_inode_rdev(struct btrfs_inode_item *i)
{
	return le32_to_cpu(i->rdev);
}

static inline void btrfs_set_inode_rdev(struct btrfs_inode_item *i, u32 val)
{
	i->rdev = cpu_to_le32(val);
}

static inline u16 btrfs_inode_flags(struct btrfs_inode_item *i)
{
	return le16_to_cpu(i->flags);
}

static inline void btrfs_set_inode_flags(struct btrfs_inode_item *i, u16 val)
{
	i->flags = cpu_to_le16(val);
}

static inline u16 btrfs_inode_compat_flags(struct btrfs_inode_item *i)
{
	return le16_to_cpu(i->compat_flags);
}

static inline void btrfs_set_inode_compat_flags(struct btrfs_inode_item *i,
						u16 val)
{
	i->compat_flags = cpu_to_le16(val);
}

static inline u64 btrfs_timespec_sec(struct btrfs_inode_timespec *ts)
{
	return le64_to_cpu(ts->sec);
}

static inline void btrfs_set_timespec_sec(struct btrfs_inode_timespec *ts,
					  u64 val)
{
	ts->sec = cpu_to_le64(val);
}

static inline u32 btrfs_timespec_nsec(struct btrfs_inode_timespec *ts)
{
	return le32_to_cpu(ts->nsec);
}

static inline void btrfs_set_timespec_nsec(struct btrfs_inode_timespec *ts,
					  u32 val)
{
	ts->nsec = cpu_to_le32(val);
}

static inline u32 btrfs_extent_refs(struct btrfs_extent_item *ei)
{
	return le32_to_cpu(ei->refs);
}

static inline void btrfs_set_extent_refs(struct btrfs_extent_item *ei, u32 val)
{
	ei->refs = cpu_to_le32(val);
}

static inline u64 btrfs_extent_owner(struct btrfs_extent_item *ei)
{
	return le64_to_cpu(ei->owner);
}

static inline void btrfs_set_extent_owner(struct btrfs_extent_item *ei, u64 val)
{
	ei->owner = cpu_to_le64(val);
}

static inline u64 btrfs_node_blockptr(struct btrfs_node *n, int nr)
{
	return le64_to_cpu(n->ptrs[nr].blockptr);
}


static inline void btrfs_set_node_blockptr(struct btrfs_node *n, int nr,
					   u64 val)
{
	n->ptrs[nr].blockptr = cpu_to_le64(val);
}

static inline u32 btrfs_item_offset(struct btrfs_item *item)
{
	return le32_to_cpu(item->offset);
}

static inline void btrfs_set_item_offset(struct btrfs_item *item, u32 val)
{
	item->offset = cpu_to_le32(val);
}

static inline u32 btrfs_item_end(struct btrfs_item *item)
{
	return le32_to_cpu(item->offset) + le32_to_cpu(item->size);
}

static inline u32 btrfs_item_size(struct btrfs_item *item)
{
	return le32_to_cpu(item->size);
}

static inline void btrfs_set_item_size(struct btrfs_item *item, u32 val)
{
	item->size = cpu_to_le32(val);
}

static inline u8 btrfs_dir_type(struct btrfs_dir_item *d)
{
	return d->type;
}

static inline void btrfs_set_dir_type(struct btrfs_dir_item *d, u8 val)
{
	d->type = val;
}

static inline u16 btrfs_dir_name_len(struct btrfs_dir_item *d)
{
	return le16_to_cpu(d->name_len);
}

static inline void btrfs_set_dir_name_len(struct btrfs_dir_item *d, u16 val)
{
	d->name_len = cpu_to_le16(val);
}

static inline u16 btrfs_dir_data_len(struct btrfs_dir_item *d)
{
	return le16_to_cpu(d->data_len);
}

static inline void btrfs_set_dir_data_len(struct btrfs_dir_item *d, u16 val)
{
	d->data_len = cpu_to_le16(val);
}

static inline void btrfs_disk_key_to_cpu(struct btrfs_key *cpu,
					 struct btrfs_disk_key *disk)
{
	cpu->offset = le64_to_cpu(disk->offset);
	cpu->type = le32_to_cpu(disk->type);
	cpu->objectid = le64_to_cpu(disk->objectid);
}

static inline void btrfs_cpu_key_to_disk(struct btrfs_disk_key *disk,
					 struct btrfs_key *cpu)
{
	disk->offset = cpu_to_le64(cpu->offset);
	disk->type = cpu_to_le32(cpu->type);
	disk->objectid = cpu_to_le64(cpu->objectid);
}

static inline u64 btrfs_disk_key_objectid(struct btrfs_disk_key *disk)
{
	return le64_to_cpu(disk->objectid);
}

static inline void btrfs_set_disk_key_objectid(struct btrfs_disk_key *disk,
					       u64 val)
{
	disk->objectid = cpu_to_le64(val);
}

static inline u64 btrfs_disk_key_offset(struct btrfs_disk_key *disk)
{
	return le64_to_cpu(disk->offset);
}

static inline void btrfs_set_disk_key_offset(struct btrfs_disk_key *disk,
					     u64 val)
{
	disk->offset = cpu_to_le64(val);
}

static inline u8 btrfs_disk_key_type(struct btrfs_disk_key *key)
{
	return key->type;
}

static inline void btrfs_set_disk_key_type(struct btrfs_disk_key *key, u8 val)
{
	key->type = val;
}

static inline u32 btrfs_key_type(struct btrfs_key *key)
{
	return key->type;
}

static inline void btrfs_set_key_type(struct btrfs_key *key, u32 val)
{
	key->type = val;
}

static inline u64 btrfs_header_bytenr(struct btrfs_header *h)
{
	return le64_to_cpu(h->bytenr);
}

static inline void btrfs_set_header_bytenr(struct btrfs_header *h, u64 bytenr)
{
	h->bytenr = cpu_to_le64(bytenr);
}

static inline u64 btrfs_header_generation(struct btrfs_header *h)
{
	return le64_to_cpu(h->generation);
}

static inline void btrfs_set_header_generation(struct btrfs_header *h,
					       u64 val)
{
	h->generation = cpu_to_le64(val);
}

static inline u64 btrfs_header_owner(struct btrfs_header *h)
{
	return le64_to_cpu(h->owner);
}

static inline void btrfs_set_header_owner(struct btrfs_header *h,
					       u64 val)
{
	h->owner = cpu_to_le64(val);
}

static inline u32 btrfs_header_nritems(struct btrfs_header *h)
{
	return le32_to_cpu(h->nritems);
}

static inline void btrfs_set_header_nritems(struct btrfs_header *h, u32 val)
{
	h->nritems = cpu_to_le32(val);
}

static inline u16 btrfs_header_flags(struct btrfs_header *h)
{
	return le16_to_cpu(h->flags);
}

static inline void btrfs_set_header_flags(struct btrfs_header *h, u16 val)
{
	h->flags = cpu_to_le16(val);
}

static inline int btrfs_header_level(struct btrfs_header *h)
{
	return h->level;
}

static inline void btrfs_set_header_level(struct btrfs_header *h, int level)
{
	BUG_ON(level > BTRFS_MAX_LEVEL);
	h->level = level;
}

static inline int btrfs_is_leaf(struct btrfs_node *n)
{
	return (btrfs_header_level(&n->header) == 0);
}

static inline u64 btrfs_root_bytenr(struct btrfs_root_item *item)
{
	return le64_to_cpu(item->bytenr);
}

static inline void btrfs_set_root_bytenr(struct btrfs_root_item *item, u64 val)
{
	item->bytenr = cpu_to_le64(val);
}

static inline u8 btrfs_root_level(struct btrfs_root_item *item)
{
	return item->level;
}

static inline void btrfs_set_root_level(struct btrfs_root_item *item, u8 val)
{
	item->level = val;
}

static inline u64 btrfs_root_dirid(struct btrfs_root_item *item)
{
	return le64_to_cpu(item->root_dirid);
}

static inline void btrfs_set_root_dirid(struct btrfs_root_item *item, u64 val)
{
	item->root_dirid = cpu_to_le64(val);
}

static inline u32 btrfs_root_refs(struct btrfs_root_item *item)
{
	return le32_to_cpu(item->refs);
}

static inline void btrfs_set_root_refs(struct btrfs_root_item *item, u32 val)
{
	item->refs = cpu_to_le32(val);
}

static inline u32 btrfs_root_flags(struct btrfs_root_item *item)
{
	return le32_to_cpu(item->flags);
}

static inline void btrfs_set_root_flags(struct btrfs_root_item *item, u32 val)
{
	item->flags = cpu_to_le32(val);
}

static inline void btrfs_set_root_bytes_used(struct btrfs_root_item *item,
					      u64 val)
{
	item->bytes_used = cpu_to_le64(val);
}

static inline u64 btrfs_root_bytes_used(struct btrfs_root_item *item)
{
	return le64_to_cpu(item->bytes_used);
}

static inline u64 btrfs_super_bytenr(struct btrfs_super_block *s)
{
	return le64_to_cpu(s->bytenr);
}

static inline void btrfs_set_super_bytenr(struct btrfs_super_block *s, u64 val)
{
	s->bytenr = cpu_to_le64(val);
}

static inline u64 btrfs_super_generation(struct btrfs_super_block *s)
{
	return le64_to_cpu(s->generation);
}

static inline void btrfs_set_super_generation(struct btrfs_super_block *s,
					      u64 val)
{
	s->generation = cpu_to_le64(val);
}

static inline u8 btrfs_super_root_level(struct btrfs_super_block *s)
{
	return s->root_level;
}

static inline void btrfs_set_super_root_level(struct btrfs_super_block *s,
					      u8 val)
{
	s->root_level = val;
}

static inline u64 btrfs_super_root(struct btrfs_super_block *s)
{
	return le64_to_cpu(s->root);
}

static inline void btrfs_set_super_root(struct btrfs_super_block *s, u64 val)
{
	s->root = cpu_to_le64(val);
}

static inline u64 btrfs_super_total_bytes(struct btrfs_super_block *s)
{
	return le64_to_cpu(s->total_bytes);
}

static inline void btrfs_set_super_total_bytes(struct btrfs_super_block *s,
						u64 val)
{
	s->total_bytes = cpu_to_le64(val);
}

static inline u64 btrfs_super_bytes_used(struct btrfs_super_block *s)
{
	return le64_to_cpu(s->bytes_used);
}

static inline void btrfs_set_super_bytes_used(struct btrfs_super_block *s,
						u64 val)
{
	s->bytes_used = cpu_to_le64(val);
}

static inline u32 btrfs_super_sectorsize(struct btrfs_super_block *s)
{
	return le32_to_cpu(s->sectorsize);
}

static inline void btrfs_set_super_sectorsize(struct btrfs_super_block *s,
						u32 val)
{
	s->sectorsize = cpu_to_le32(val);
}

static inline u32 btrfs_super_nodesize(struct btrfs_super_block *s)
{
	return le32_to_cpu(s->nodesize);
}

static inline void btrfs_set_super_nodesize(struct btrfs_super_block *s,
						u32 val)
{
	s->nodesize = cpu_to_le32(val);
}

static inline u32 btrfs_super_leafsize(struct btrfs_super_block *s)
{
	return le32_to_cpu(s->leafsize);
}

static inline void btrfs_set_super_leafsize(struct btrfs_super_block *s,
						u32 val)
{
	s->leafsize = cpu_to_le32(val);
}

static inline u64 btrfs_super_root_dir(struct btrfs_super_block *s)
{
	return le64_to_cpu(s->root_dir_objectid);
}

static inline void btrfs_set_super_root_dir(struct btrfs_super_block *s, u64
					    val)
{
	s->root_dir_objectid = cpu_to_le64(val);
}

static inline u8 *btrfs_leaf_data(struct btrfs_leaf *l)
{
	return (u8 *)l->items;
}

static inline int btrfs_file_extent_type(struct btrfs_file_extent_item *e)
{
	return e->type;
}
static inline void btrfs_set_file_extent_type(struct btrfs_file_extent_item *e,
					      u8 val)
{
	e->type = val;
}

static inline char *btrfs_file_extent_inline_start(struct
						   btrfs_file_extent_item *e)
{
	return (char *)(&e->disk_bytenr);
}

static inline u32 btrfs_file_extent_calc_inline_size(u32 datasize)
{
	return (unsigned long)(&((struct
		  btrfs_file_extent_item *)NULL)->disk_bytenr) + datasize;
}

static inline u32 btrfs_file_extent_inline_len(struct btrfs_item *e)
{
	struct btrfs_file_extent_item *fe = NULL;
	return btrfs_item_size(e) - (unsigned long)(&fe->disk_bytenr);
}

static inline u64 btrfs_file_extent_disk_bytenr(struct btrfs_file_extent_item
						 *e)
{
	return le64_to_cpu(e->disk_bytenr);
}

static inline void btrfs_set_file_extent_disk_bytenr(struct
						      btrfs_file_extent_item
						      *e, u64 val)
{
	e->disk_bytenr = cpu_to_le64(val);
}

static inline u64 btrfs_file_extent_generation(struct btrfs_file_extent_item *e)
{
	return le64_to_cpu(e->generation);
}

static inline void btrfs_set_file_extent_generation(struct
						    btrfs_file_extent_item *e,
						    u64 val)
{
	e->generation = cpu_to_le64(val);
}

static inline u64 btrfs_file_extent_disk_num_bytes(struct
						    btrfs_file_extent_item *e)
{
	return le64_to_cpu(e->disk_num_bytes);
}

static inline void btrfs_set_file_extent_disk_num_bytes(struct
							 btrfs_file_extent_item
							 *e, u64 val)
{
	e->disk_num_bytes = cpu_to_le64(val);
}

static inline u64 btrfs_file_extent_offset(struct btrfs_file_extent_item *e)
{
	return le64_to_cpu(e->offset);
}

static inline void btrfs_set_file_extent_offset(struct btrfs_file_extent_item
						*e, u64 val)
{
	e->offset = cpu_to_le64(val);
}

static inline u64 btrfs_file_extent_num_bytes(struct btrfs_file_extent_item
					       *e)
{
	return le64_to_cpu(e->num_bytes);
}

static inline void btrfs_set_file_extent_num_bytes(struct
						    btrfs_file_extent_item *e,
						    u64 val)
{
	e->num_bytes = cpu_to_le64(val);
}

/* helper function to cast into the data area of the leaf. */
#define btrfs_item_ptr(leaf, slot, type) \
	((type *)(btrfs_leaf_data(leaf) + \
	btrfs_item_offset((leaf)->items + (slot))))

static inline u32 btrfs_level_size(struct btrfs_root *root, int level)
{
	if (level == 0)
		return root->leafsize;
	return root->nodesize;
}
int btrfs_comp_keys(struct btrfs_disk_key *disk, struct btrfs_key *k2);
int btrfs_extend_item(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_path *path, u32 data_size);
struct btrfs_buffer *btrfs_alloc_free_block(struct btrfs_trans_handle *trans,
					    struct btrfs_root *root,
					    u32 blocksize);
int btrfs_inc_ref(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		  struct btrfs_buffer *buf);
int btrfs_free_extent(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, u64 bytenr, u64 num_bytes, int pin);
int btrfs_search_slot(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_key *key, struct btrfs_path *p, int
		      ins_len, int cow);
void btrfs_release_path(struct btrfs_root *root, struct btrfs_path *p);
void btrfs_init_path(struct btrfs_path *p);
int btrfs_del_item(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		   struct btrfs_path *path);
int btrfs_insert_item(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_key *key, void *data, u32 data_size);
int btrfs_insert_empty_item(struct btrfs_trans_handle *trans, struct btrfs_root
			    *root, struct btrfs_path *path, struct btrfs_key
			    *cpu_key, u32 data_size);
int btrfs_next_leaf(struct btrfs_root *root, struct btrfs_path *path);
int btrfs_leaf_free_space(struct btrfs_root *root, struct btrfs_leaf *leaf);
int btrfs_drop_snapshot(struct btrfs_trans_handle *trans, struct btrfs_root
			*root, struct btrfs_buffer *snap);
int btrfs_finish_extent_commit(struct btrfs_trans_handle *trans, struct
			       btrfs_root *root);
int btrfs_del_root(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		   struct btrfs_key *key);
int btrfs_insert_root(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_key *key, struct btrfs_root_item
		      *item);
int btrfs_update_root(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_key *key, struct btrfs_root_item
		      *item);
int btrfs_find_last_root(struct btrfs_root *root, u64 objectid, struct
			 btrfs_root_item *item, struct btrfs_key *key);
int btrfs_insert_dir_item(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, char *name, int name_len, u64 dir,
			  struct btrfs_key *location, u8 type);
int btrfs_lookup_dir_item(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, struct btrfs_path *path, u64 dir, char *name,
			  int name_len, int mod);
int btrfs_match_dir_item_name(struct btrfs_root *root, struct btrfs_path *path,
			      char *name, int name_len);
int btrfs_find_free_objectid(struct btrfs_trans_handle *trans,
			     struct btrfs_root *fs_root,
			     u64 dirid, u64 *objectid);
int btrfs_insert_inode(struct btrfs_trans_handle *trans, struct btrfs_root
		       *root, u64 objectid, struct btrfs_inode_item
		       *inode_item);
int btrfs_lookup_inode(struct btrfs_trans_handle *trans, struct btrfs_root
		       *root, struct btrfs_path *path, u64 objectid, int mod);
int btrfs_write_dirty_block_groups(struct btrfs_trans_handle *trans,
				    struct btrfs_root *root);
int btrfs_free_block_groups(struct btrfs_fs_info *info);
int btrfs_read_block_groups(struct btrfs_root *root);
int btrfs_insert_block_group(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     struct btrfs_key *key,
			     struct btrfs_block_group_item *bi);
#endif
