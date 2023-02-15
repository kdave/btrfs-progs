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
#include <errno.h>
#include <stdio.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/transaction.h"
#include "common/messages.h"
#include "common/extent-cache.h"

/* After this many block groups we need to commit transaction. */
#define BLOCK_GROUP_BATCH	64

int convert_to_bg_tree(struct btrfs_fs_info *fs_info)
{
	struct btrfs_super_block *sb = fs_info->super_copy;
	struct btrfs_trans_handle *trans;
	struct cache_extent *ce;
	int converted_bgs = 0;
	int ret;

	trans = btrfs_start_transaction(fs_info->tree_root, 2);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	/* Set NO_HOLES feature */
	btrfs_set_super_incompat_flags(sb, btrfs_super_incompat_flags(sb) |
				       BTRFS_FEATURE_INCOMPAT_NO_HOLES);

	/* We're resuming from previous run. */
	if (btrfs_super_flags(sb) & BTRFS_SUPER_FLAG_CHANGING_BG_TREE)
		goto iterate_bgs;

	ret = btrfs_create_root(trans, fs_info,
				BTRFS_BLOCK_GROUP_TREE_OBJECTID);
	if (ret < 0) {
		error("failed to create block group root: %d", ret);
		goto error;
	}
	btrfs_set_super_flags(sb,
			btrfs_super_flags(sb) |
			BTRFS_SUPER_FLAG_CHANGING_BG_TREE);
	fs_info->last_converted_bg_bytenr = (u64)-1;

	/* Now commit the transaction to make above changes to reach disks. */
	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret < 0) {
		error_msg(ERROR_MSG_COMMIT_TRANS, "new bg root: %d", ret);
		goto error;
	}
	trans = btrfs_start_transaction(fs_info->tree_root, 2);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

iterate_bgs:
	if (fs_info->last_converted_bg_bytenr == (u64)-1) {
		ce = last_cache_extent(&fs_info->mapping_tree.cache_tree);
	} else {
		ce = search_cache_extent(&fs_info->mapping_tree.cache_tree,
					 fs_info->last_converted_bg_bytenr);
		if (!ce) {
			error("failed to find block group for bytenr %llu",
			      fs_info->last_converted_bg_bytenr);
			ret = -ENOENT;
			goto error;
		}
		ce = prev_cache_extent(ce);
		if (!ce) {
			error("no more block groups before bytenr %llu",
			      fs_info->last_converted_bg_bytenr);
			ret = -ENOENT;
			goto error;
		}
	}

	/* Now convert each block */
	while (ce) {
		struct cache_extent *prev = prev_cache_extent(ce);
		u64 bytenr = ce->start;

		ret = btrfs_convert_one_bg(trans, bytenr);
		if (ret < 0)
			goto error;
		converted_bgs++;
		ce = prev;

		if (converted_bgs % BLOCK_GROUP_BATCH == 0) {
			ret = btrfs_commit_transaction(trans,
							fs_info->tree_root);
			if (ret < 0) {
				errno = -ret;
				error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
				return ret;
			}
			trans = btrfs_start_transaction(fs_info->tree_root, 2);
			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				errno = -ret;
				error_msg(ERROR_MSG_START_TRANS, "%m");
				return ret;
			}
		}
	}
	/*
	 * All bgs converted, remove the CHANGING_BG flag and set the compat ro
	 * flag.
	 */
	fs_info->last_converted_bg_bytenr = 0;
	btrfs_set_super_flags(sb,
		btrfs_super_flags(sb) &
		~BTRFS_SUPER_FLAG_CHANGING_BG_TREE);
	btrfs_set_super_compat_ro_flags(sb,
			btrfs_super_compat_ro_flags(sb) |
			BTRFS_FEATURE_COMPAT_RO_BLOCK_GROUP_TREE);
	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "final transaction: %m");
		return ret;
	}
	pr_verbose(LOG_DEFAULT, "Converted the filesystem to block group tree feature\n");
	return 0;
error:
	btrfs_abort_transaction(trans, ret);
	return ret;
}


