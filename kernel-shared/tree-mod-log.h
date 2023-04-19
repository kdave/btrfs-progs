/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_TREE_MOD_LOG_H
#define BTRFS_TREE_MOD_LOG_H

#include "kernel-shared/ctree.h"

/* Represents a tree mod log user. */
struct btrfs_seq_list {
	struct list_head list;
	u64 seq;
};

#define BTRFS_SEQ_LIST_INIT(name) { .list = LIST_HEAD_INIT((name).list), .seq = 0 }
#define BTRFS_SEQ_LAST            ((u64)-1)

enum btrfs_mod_log_op {
	BTRFS_MOD_LOG_KEY_REPLACE,
	BTRFS_MOD_LOG_KEY_ADD,
	BTRFS_MOD_LOG_KEY_REMOVE,
	BTRFS_MOD_LOG_KEY_REMOVE_WHILE_FREEING,
	BTRFS_MOD_LOG_KEY_REMOVE_WHILE_MOVING,
	BTRFS_MOD_LOG_MOVE_KEYS,
	BTRFS_MOD_LOG_ROOT_REPLACE,
};

static inline u64 btrfs_get_tree_mod_seq(struct btrfs_fs_info *fs_info,
					 struct btrfs_seq_list *elem)
{
	return 0;
}

static inline void btrfs_put_tree_mod_seq(struct btrfs_fs_info *fs_info,
					  struct btrfs_seq_list *elem)
{
}

static inline int btrfs_tree_mod_log_insert_root(struct extent_buffer *old_root,
						 struct extent_buffer *new_root,
						 bool log_removal)
{
	return 0;
}

static inline int btrfs_tree_mod_log_insert_key(struct extent_buffer *eb, int slot,
						enum btrfs_mod_log_op op)
{
	return 0;
}

static inline int btrfs_tree_mod_log_free_eb(struct extent_buffer *eb)
{
	return 0;
}

static inline struct extent_buffer *btrfs_tree_mod_log_rewind(struct btrfs_fs_info *fs_info,
							      struct btrfs_path *path,
							      struct extent_buffer *eb,
							      u64 time_seq)
{
	return NULL;
}

static inline struct extent_buffer *btrfs_get_old_root(struct btrfs_root *root,
						       u64 time_seq)
{
	return NULL;
}

static inline int btrfs_old_root_level(struct btrfs_root *root, u64 time_seq)
{
	return btrfs_header_level(root->node);
}

static inline int btrfs_tree_mod_log_eb_copy(struct extent_buffer *dst,
					     struct extent_buffer *src,
					     unsigned long dst_offset,
					     unsigned long src_offset,
					     int nr_items)
{
	return 0;
}

static inline int btrfs_tree_mod_log_insert_move(struct extent_buffer *eb,
						 int dst_slot, int src_slot,
						 int nr_items)
{
	return 0;
}

static inline u64 btrfs_tree_mod_log_lowest_seq(struct btrfs_fs_info *fs_info)
{
	return 0;
}

#endif
