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

#include "kerncompat.h"
#include <stdlib.h>
#include "kernel-lib/rbtree.h"
#include "kernel-lib/bitops.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/delayed-ref.h"
#include "kernel-shared/zoned.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/locking.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "common/messages.h"

/*
 * The metadata reservation code is completely different from the kernel:
 *
 * - No need to support reclaim
 * - No support for transaction join
 *
 * This is due to the fact that btrfs-progs is only single threaded, thus it
 * always starts a transaction, does some tree operations, and commits the
 * transaction.
 *
 * So here we only need to make sure we have enough metadata space, and there
 * will be no metadata over-commit (allowing extra metadata operations as long
 * as there is unallocated space).
 *
 * The only extra step we can really do to increase metadata space is to allocate
 * new metadata chunks.
 */

static unsigned int calc_insert_metadata_size(const struct btrfs_fs_info *fs_info,
					      unsigned int num_items)
{
	return fs_info->nodesize * BTRFS_MAX_LEVEL * num_items * 2;
}

static bool meta_has_enough_space(struct btrfs_fs_info *fs_info,
				  u64 profile, unsigned int size)
{
	struct btrfs_space_info *sinfo;

	profile &= BTRFS_BLOCK_GROUP_TYPE_MASK;

	/*
	 * The fs is temporary (still during mkfs), do not check free space
	 * as we don't have all meta/sys chunks setup.
	 */
	if (btrfs_super_magic(fs_info->super_copy) != BTRFS_MAGIC)
		return true;

	/*
	 * The fs is under extent tree rebuilding, do not do any free space check
	 * as they are not reliable.
	 */
	if (fs_info->rebuilding_extent_tree)
		return true;

	sinfo = btrfs_find_space_info(fs_info, profile);
	if (!sinfo) {
		error("unable to find block group for profile 0x%llx", profile);
		return false;
	}

	if (sinfo->bytes_used + sinfo->bytes_pinned + sinfo->bytes_reserved +
	    size < sinfo->total_bytes)
		return true;
	return false;
}

static struct btrfs_trans_handle *alloc_trans_handle(struct btrfs_root *root,
						     unsigned int num_items)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *h;

	h = kzalloc(sizeof(*h), GFP_NOFS);
	if (!h)
		return ERR_PTR(-ENOMEM);

	h->fs_info = fs_info;
	fs_info->running_transaction = h;
	fs_info->generation++;
	h->transid = fs_info->generation;
	h->blocks_reserved = num_items;
	h->reinit_extent_tree = false;
	h->allocating_chunk = 0;
	root->last_trans = h->transid;
	root->commit_root = root->node;
	extent_buffer_get(root->node);
	INIT_LIST_HEAD(&h->dirty_bgs);

	return h;
}

struct btrfs_trans_handle *btrfs_start_transaction(struct btrfs_root *root,
						   unsigned int num_items)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *h;
	unsigned int rsv_bytes;
	bool need_retry = false;
	u64 profile;

	if (root->root_key.objectid == BTRFS_CHUNK_TREE_OBJECTID)
		profile = BTRFS_BLOCK_GROUP_SYSTEM |
			  (fs_info->avail_system_alloc_bits &
			   fs_info->system_alloc_profile);
	else
		profile = BTRFS_BLOCK_GROUP_METADATA |
			  (fs_info->avail_metadata_alloc_bits &
			   fs_info->metadata_alloc_profile);

	if (fs_info->transaction_aborted)
		return ERR_PTR(-EROFS);

	if (root->commit_root) {
		error("commit_root already set when starting transaction");
		return ERR_PTR(-EINVAL);
	}
	if (fs_info->running_transaction) {
		error("attempt to start transaction over already running one");
		return ERR_PTR(-EINVAL);
	}

	/*
	 * For those call sites, they are mostly delete items, in that case
	 * just change it to 1.
	 */
	if (num_items == 0)
		num_items = 1;

	rsv_bytes = calc_insert_metadata_size(fs_info, num_items);

	/*
	 * We should not have so many items that it's larger than one metadata
	 * chunk.
	 */
	if (rsv_bytes > SZ_1G) {
		error("too much metadata space required: num_items %u reserved bytes %u",
		      num_items, rsv_bytes);
		return ERR_PTR(-EINVAL);
	}

	if (!meta_has_enough_space(fs_info, profile, rsv_bytes))
		need_retry = true;

	h = alloc_trans_handle(root, num_items);
	if (IS_ERR(h))
		return ERR_PTR(PTR_ERR(h));

	if (need_retry) {
		int ret;

		ret = btrfs_try_chunk_alloc(h, fs_info, rsv_bytes, profile);
		if (ret < 0) {
			btrfs_abort_transaction(h, ret);
			errno = -ret;
			error("failed to allocate new chunk: %m");
			return ERR_PTR(ret);
		}
		ret = btrfs_commit_transaction(h, root);
		if (ret < 0) {
			errno = -ret;
			error("failed to commit transaction for the new chunk: %m");
			return ERR_PTR(ret);
		}
		if (!meta_has_enough_space(fs_info, profile, rsv_bytes)) {
			errno = -ENOSPC;
			error("failed to start transaction: %m");
			return ERR_PTR(-ENOSPC);
		}

		h = alloc_trans_handle(root, num_items);
	}
	return h;
}

static int update_cowonly_root(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root)
{
	int ret;
	u64 old_root_bytenr;
	struct btrfs_root *tree_root = root->fs_info->tree_root;

	while(1) {
		old_root_bytenr = btrfs_root_bytenr(&root->root_item);
		if (old_root_bytenr == root->node->start)
			break;
		btrfs_set_root_bytenr(&root->root_item,
				       root->node->start);
		btrfs_set_root_generation(&root->root_item,
					  trans->transid);
		root->root_item.level = btrfs_header_level(root->node);
		ret = btrfs_update_root(trans, tree_root,
					&root->root_key,
					&root->root_item);
		if (ret < 0)
			return ret;
		ret = btrfs_write_dirty_block_groups(trans);
		if (ret)
			return ret;
	}
	return 0;
}

int commit_tree_roots(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root;
	struct list_head *next;
	struct extent_buffer *eb;
	int ret;

	if (fs_info->readonly)
		return 0;

	eb = fs_info->tree_root->node;
	extent_buffer_get(eb);
	ret = btrfs_cow_block(trans, fs_info->tree_root, eb, NULL, 0, &eb,
			      BTRFS_NESTING_NORMAL);
	free_extent_buffer(eb);
	if (ret)
		return ret;

	/*
	 * If the above CoW is the first one to dirty the current tree_root,
	 * delayed refs for it won't be run until after this function has
	 * finished executing, meaning we won't process the extent tree root,
	 * which will have been added to ->dirty_cowonly_roots.  So run
	 * delayed refs here as well.
	 */
	ret = btrfs_run_delayed_refs(trans, -1);
	if (ret)
		return ret;

	while(!list_empty(&fs_info->dirty_cowonly_roots)) {
		next = fs_info->dirty_cowonly_roots.next;
		list_del_init(next);
		root = list_entry(next, struct btrfs_root, dirty_list);
		clear_bit(BTRFS_ROOT_DIRTY, &root->state);
		ret = update_cowonly_root(trans, root);
		free_extent_buffer(root->commit_root);
		root->commit_root = NULL;
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void clean_dirty_buffers(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct extent_io_tree *tree = &fs_info->dirty_buffers;
	struct extent_buffer *eb;
	u64 start, end;

	while (find_first_extent_bit(tree, 0, &start, &end, EXTENT_DIRTY,
				     NULL) == 0) {
		while (start <= end) {
			eb = find_first_extent_buffer(fs_info, start);
			BUG_ON(!eb || eb->start != start);
			start += eb->len;
			btrfs_clear_buffer_dirty(trans, eb);
			free_extent_buffer(eb);
		}
	}
}

int __commit_transaction(struct btrfs_trans_handle *trans,
				struct btrfs_root *root)
{
	u64 start;
	u64 end;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *eb;
	struct extent_io_tree *tree = &fs_info->dirty_buffers;
	int ret;

	while(1) {
again:
		ret = find_first_extent_bit(tree, 0, &start, &end,
					    EXTENT_DIRTY, NULL);
		if (ret)
			break;

		if (btrfs_redirty_extent_buffer_for_zoned(fs_info, start, end))
			goto again;

		while(start <= end) {
			eb = find_first_extent_buffer(fs_info, start);
			BUG_ON(!eb || eb->start != start);
			ret = write_tree_block(trans, fs_info, eb);
			if (ret < 0) {
				free_extent_buffer(eb);
				errno = -ret;
				error("failed to write tree block %llu: %m",
				      eb->start);
				goto cleanup;
			}
			start += eb->len;
			btrfs_clear_buffer_dirty(trans, eb);
			free_extent_buffer(eb);
		}
	}
	return 0;
cleanup:
	/*
	 * Mark all remaining dirty ebs clean, as they have no chance to be written
	 * back anymore.
	 */
	clean_dirty_buffers(trans);
	return ret;
}

void btrfs_cleanup_aborted_transaction(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans = fs_info->running_transaction;
	int error = fs_info->transaction_aborted;

	if (!error || !trans)
		return;

	btrfs_abort_transaction(trans, error);
	clean_dirty_buffers(trans);
	btrfs_destroy_delayed_refs(trans);
	kfree(trans);
	fs_info->running_transaction = NULL;
}

int btrfs_commit_transaction(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root)
{
	u64 transid = trans->transid;
	int ret = 0;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_space_info *sinfo;

	if (trans->fs_info->transaction_aborted) {
		ret = -EROFS;
		goto error;
	}

	/*
	 * Flush all accumulated delayed refs so that root-tree updates are
	 * consistent
	 */
	ret = btrfs_run_delayed_refs(trans, -1);
	if (ret < 0)
		goto error;

	if (root->commit_root == root->node)
		goto commit_tree;
	if (root == root->fs_info->tree_root)
		goto commit_tree;
	if (root == root->fs_info->chunk_root)
		goto commit_tree;
	if (root == root->fs_info->block_group_root)
		goto commit_tree;

	free_extent_buffer(root->commit_root);
	root->commit_root = NULL;

	btrfs_set_root_bytenr(&root->root_item, root->node->start);
	btrfs_set_root_generation(&root->root_item, trans->transid);
	root->root_item.level = btrfs_header_level(root->node);
	ret = btrfs_update_root(trans, root->fs_info->tree_root,
				&root->root_key, &root->root_item);
	if (ret < 0)
		goto error;

commit_tree:
	ret = commit_tree_roots(trans, fs_info);
	if (ret < 0)
		goto error;

	/*
	 * btrfs_write_dirty_block_groups() can cause COW thus new delayed
	 * tree refs, while run such delayed tree refs can dirty block groups
	 * again, we need to exhause both dirty blocks and delayed refs
	 */
	while (!RB_EMPTY_ROOT(&trans->delayed_refs.href_root) ||
	       !list_empty(&trans->dirty_bgs)) {
		ret = btrfs_write_dirty_block_groups(trans);
		if (ret < 0)
			goto error;
		ret = btrfs_run_delayed_refs(trans, -1);
		if (ret < 0)
			goto error;
	}
	ret = __commit_transaction(trans, root);
	if (ret < 0)
		goto error;

	/* There should be no pending delayed refs now */
	if (!RB_EMPTY_ROOT(&trans->delayed_refs.href_root)) {
		error("uncommitted delayed refs detected");
		goto error;
	}
	ret = write_ctree_super(trans);
	btrfs_finish_extent_commit(trans);
	kfree(trans);
	free_extent_buffer(root->commit_root);
	root->commit_root = NULL;
	fs_info->running_transaction = NULL;
	fs_info->last_trans_committed = transid;
	list_for_each_entry(sinfo, &fs_info->space_info, list) {
		if (sinfo->bytes_reserved) {
			warning(
	"reserved space leaked, transid=%llu flag=0x%llx bytes_reserved=%llu",
				transid, sinfo->flags, sinfo->bytes_reserved);
		}
	}
	return ret;
error:
	btrfs_cleanup_aborted_transaction(fs_info);
	return ret;
}

void btrfs_abort_transaction(struct btrfs_trans_handle *trans, int error)
{
	trans->fs_info->transaction_aborted = error;
}
