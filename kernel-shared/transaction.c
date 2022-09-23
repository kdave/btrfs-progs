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
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/delayed-ref.h"
#include "kernel-shared/zoned.h"
#include "common/messages.h"

struct btrfs_trans_handle* btrfs_start_transaction(struct btrfs_root *root,
		int num_blocks)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_trans_handle *h;

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

	h = kzalloc(sizeof(*h), GFP_NOFS);
	if (!h)
		return ERR_PTR(-ENOMEM);

	h->fs_info = fs_info;
	fs_info->running_transaction = h;
	fs_info->generation++;
	h->transid = fs_info->generation;
	h->blocks_reserved = num_blocks;
	h->reinit_extent_tree = false;
	h->allocating_chunk = 0;
	root->last_trans = h->transid;
	root->commit_root = root->node;
	extent_buffer_get(root->node);
	INIT_LIST_HEAD(&h->dirty_bgs);

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
	ret = btrfs_cow_block(trans, fs_info->tree_root, eb, NULL, 0, &eb);
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
		ret = update_cowonly_root(trans, root);
		free_extent_buffer(root->commit_root);
		root->commit_root = NULL;
		if (ret < 0)
			return ret;
	}

	return 0;
}

int __commit_transaction(struct btrfs_trans_handle *trans,
				struct btrfs_root *root)
{
	u64 start;
	u64 end;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *eb;
	struct extent_io_tree *tree = &fs_info->extent_cache;
	int ret;

	while(1) {
again:
		ret = find_first_extent_bit(tree, 0, &start, &end,
					    EXTENT_DIRTY);
		if (ret)
			break;

		if (btrfs_redirty_extent_buffer_for_zoned(fs_info, start, end))
			goto again;

		while(start <= end) {
			eb = find_first_extent_buffer(tree, start);
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
			clear_extent_buffer_dirty(eb);
			free_extent_buffer(eb);
		}
	}
	return 0;
cleanup:
	/*
	 * Mark all remaining dirty ebs clean, as they have no chance to be written
	 * back anymore.
	 */
	while (1) {
		int find_ret;

		find_ret = find_first_extent_bit(tree, 0, &start, &end, EXTENT_DIRTY);

		if (find_ret)
			break;

		while (start <= end) {
			eb = find_first_extent_buffer(tree, start);
			BUG_ON(!eb || eb->start != start);
			start += eb->len;
			clear_extent_buffer_dirty(eb);
			free_extent_buffer(eb);
		}
	}
	return ret;
}

int btrfs_commit_transaction(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root)
{
	u64 transid = trans->transid;
	int ret = 0;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_space_info *sinfo;

	if (trans->fs_info->transaction_aborted)
		return -EROFS;
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
	btrfs_abort_transaction(trans, ret);
	btrfs_destroy_delayed_refs(trans);
	free(trans);
	return ret;
}

void btrfs_abort_transaction(struct btrfs_trans_handle *trans, int error)
{
	trans->fs_info->transaction_aborted = error;
}
