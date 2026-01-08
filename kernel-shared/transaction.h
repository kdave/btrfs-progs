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

#ifndef __BTRFS_TRANSACTION_H__
#define __BTRFS_TRANSACTION_H__

#include "kerncompat.h"
#include <sys/stat.h>
#include <stdbool.h>
#include "kernel-lib/list.h"
#include "kernel-shared/delayed-ref.h"
#include "kernel-shared/extent-io-tree.h"
#include "kernel-shared/misc.h"

struct btrfs_fs_info;
struct btrfs_root;

enum btrfs_trans_state {
	TRANS_STATE_RUNNING,
	TRANS_STATE_COMMIT_PREP,
	TRANS_STATE_COMMIT_START,
	TRANS_STATE_COMMIT_DOING,
	TRANS_STATE_UNBLOCKED,
	TRANS_STATE_SUPER_COMMITTED,
	TRANS_STATE_COMPLETED,
	TRANS_STATE_MAX,
};

#define BTRFS_TRANS_HAVE_FREE_BGS	0
#define BTRFS_TRANS_DIRTY_BG_RUN	1
#define BTRFS_TRANS_CACHE_ENOSPC	2

struct btrfs_transaction {
	u64 transid;
	/*
	 * total external writers(USERSPACE/START/ATTACH) in this
	 * transaction, it must be zero before the transaction is
	 * being committed
	 */
	atomic_t num_extwriters;
	/*
	 * total writers in this transaction, it must be zero before the
	 * transaction can end
	 */
	atomic_t num_writers;
	refcount_t use_count;

	unsigned long flags;

	/* Be protected by fs_info->trans_lock when we want to change it. */
	enum btrfs_trans_state state;
	int aborted;
	struct list_head list;
	struct extent_io_tree dirty_pages;
	wait_queue_head_t writer_wait;
	wait_queue_head_t commit_wait;
	struct list_head pending_snapshots;
	struct list_head dev_update_list;
	struct list_head switch_commits;
	struct list_head dirty_bgs;

	/*
	 * There is no explicit lock which protects io_bgs, rather its
	 * consistency is implied by the fact that all the sites which modify
	 * it do so under some form of transaction critical section, namely:
	 *
	 * - btrfs_start_dirty_block_groups - This function can only ever be
	 *   run by one of the transaction committers. Refer to
	 *   BTRFS_TRANS_DIRTY_BG_RUN usage in btrfs_commit_transaction
	 *
	 * - btrfs_write_dirty_blockgroups - this is called by
	 *   commit_cowonly_roots from transaction critical section
	 *   (TRANS_STATE_COMMIT_DOING)
	 *
	 * - btrfs_cleanup_dirty_bgs - called on transaction abort
	 */
	struct list_head io_bgs;
	struct list_head dropped_roots;
	struct extent_io_tree pinned_extents;

	/*
	 * we need to make sure block group deletion doesn't race with
	 * free space cache writeout.  This mutex keeps them from stomping
	 * on each other
	 */
	struct mutex cache_write_mutex;
	spinlock_t dirty_bgs_lock;
	/* Protected by spin lock fs_info->unused_bgs_lock. */
	struct list_head deleted_bgs;
	spinlock_t dropped_roots_lock;
	struct btrfs_delayed_ref_root delayed_refs;
	struct btrfs_fs_info *fs_info;

	/*
	 * Number of ordered extents the transaction must wait for before
	 * committing. These are ordered extents started by a fast fsync.
	 */
	atomic_t pending_ordered;
	wait_queue_head_t pending_wait;
};

enum {
	ENUM_BIT(__TRANS_FREEZABLE),
	ENUM_BIT(__TRANS_START),
	ENUM_BIT(__TRANS_ATTACH),
	ENUM_BIT(__TRANS_JOIN),
	ENUM_BIT(__TRANS_JOIN_NOLOCK),
	ENUM_BIT(__TRANS_DUMMY),
	ENUM_BIT(__TRANS_JOIN_NOSTART),
};

#define TRANS_START		(__TRANS_START | __TRANS_FREEZABLE)
#define TRANS_ATTACH		(__TRANS_ATTACH)
#define TRANS_JOIN		(__TRANS_JOIN | __TRANS_FREEZABLE)
#define TRANS_JOIN_NOLOCK	(__TRANS_JOIN_NOLOCK)
#define TRANS_JOIN_NOSTART	(__TRANS_JOIN_NOSTART)

#define TRANS_EXTWRITERS	(__TRANS_START | __TRANS_ATTACH)

struct btrfs_trans_handle {
	struct btrfs_fs_info *fs_info;
	u64 transid;
	u64 alloc_exclude_start;
	u64 alloc_exclude_nr;
	bool reinit_extent_tree;
	unsigned int allocating_chunk:1;
	u64 delayed_ref_updates;
	unsigned long blocks_reserved;
	unsigned long blocks_used;
	struct btrfs_block_group *block_group;
	struct btrfs_delayed_ref_root delayed_refs;
	struct list_head dirty_bgs;
};

/*
 * The abort status can be changed between calls and is not protected by locks.
 * This accepts btrfs_transaction and btrfs_trans_handle as types. Once it's
 * set to a non-zero value it does not change, so the macro should be in checks
 * but is not necessary for further reads of the value.
 */
#define TRANS_ABORTED(trans)		(unlikely(READ_ONCE((trans)->aborted)))

struct btrfs_pending_snapshot {
	struct dentry *dentry;
	struct inode *dir;
	struct btrfs_root *root;
	struct btrfs_root_item *root_item;
	struct btrfs_root *snap;
	struct btrfs_qgroup_inherit *inherit;
	struct btrfs_path *path;
	/* extra metadata reservation for relocation */
	int error;
	/* Preallocated anonymous block device number */
	dev_t anon_dev;
	bool readonly;
	struct list_head list;
};

bool __cold abort_should_print_stack(int error);

void btrfs_cleanup_aborted_transaction(struct btrfs_fs_info *fs_info);
void btrfs_abort_transaction(struct btrfs_trans_handle *trans, int error);
int btrfs_end_transaction(struct btrfs_trans_handle *trans);
struct btrfs_trans_handle *btrfs_start_transaction(struct btrfs_root *root,
						   unsigned int num_items);
struct btrfs_trans_handle *btrfs_start_transaction_fallback_global_rsv(
					struct btrfs_root *root,
					unsigned int num_items);
struct btrfs_trans_handle *btrfs_join_transaction(struct btrfs_root *root);
struct btrfs_trans_handle *btrfs_join_transaction_spacecache(struct btrfs_root *root);
struct btrfs_trans_handle *btrfs_join_transaction_nostart(struct btrfs_root *root);
struct btrfs_trans_handle *btrfs_attach_transaction(struct btrfs_root *root);
struct btrfs_trans_handle *btrfs_attach_transaction_barrier(
					struct btrfs_root *root);
int btrfs_wait_for_commit(struct btrfs_fs_info *fs_info, u64 transid);

void btrfs_add_dead_root(struct btrfs_root *root);
int btrfs_defrag_root(struct btrfs_root *root);
void btrfs_maybe_wake_unfinished_drop(struct btrfs_fs_info *fs_info);
int btrfs_clean_one_deleted_snapshot(struct btrfs_fs_info *fs_info);
int btrfs_commit_transaction(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root);
void btrfs_commit_transaction_async(struct btrfs_trans_handle *trans);
int btrfs_end_transaction_throttle(struct btrfs_trans_handle *trans);
bool btrfs_should_end_transaction(struct btrfs_trans_handle *trans);
void btrfs_throttle(struct btrfs_fs_info *fs_info);
int btrfs_record_root_in_trans(struct btrfs_trans_handle *trans,
				struct btrfs_root *root);
int btrfs_write_marked_extents(struct btrfs_fs_info *fs_info,
				struct extent_io_tree *dirty_pages, int mark);
int btrfs_wait_tree_log_extents(struct btrfs_root *root, int mark);
int btrfs_transaction_blocked(struct btrfs_fs_info *info);
int btrfs_transaction_in_commit(struct btrfs_fs_info *info);
void btrfs_put_transaction(struct btrfs_transaction *transaction);
void btrfs_add_dropped_root(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root);
void btrfs_trans_release_chunk_metadata(struct btrfs_trans_handle *trans);
void __cold __btrfs_abort_transaction(struct btrfs_trans_handle *trans,
				      const char *function,
				      unsigned int line, int error, bool first_hit);

int __init btrfs_transaction_init(void);
void __cold btrfs_transaction_exit(void);

int __commit_transaction(struct btrfs_trans_handle *trans,
				struct btrfs_root *root);
int commit_tree_roots(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info);

#endif
