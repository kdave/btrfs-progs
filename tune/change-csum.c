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
#include <stdlib.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/transaction.h"
#include "common/messages.h"
#include "common/internal.h"
#include "tune/tune.h"

static int check_csum_change_requreiment(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *dev_root = fs_info->dev_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int ret;

	if (btrfs_super_log_root(fs_info->super_copy)) {
		error("dirty log tree detected, please replay the log or zero it.");
		return -EINVAL;
	}
	if (btrfs_fs_incompat(fs_info, EXTENT_TREE_V2)) {
		error("no csum change support for extent-tree-v2 feature yet.");
		return -EOPNOTSUPP;
	}
	if (btrfs_super_flags(fs_info->super_copy) &
	    (BTRFS_SUPER_FLAG_CHANGING_DATA_CSUM |
	     BTRFS_SUPER_FLAG_CHANGING_META_CSUM)) {
		error("resume from half converted status is not yet supported");
		return -EOPNOTSUPP;
	}
	key.objectid = BTRFS_BALANCE_OBJECTID;
	key.type = BTRFS_TEMPORARY_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	btrfs_release_path(&path);
	if (ret < 0) {
		errno = -ret;
		error("failed to check the balance status: %m");
		return ret;
	}
	if (ret == 0) {
		error("running balance detected, please finish or cancel it.");
		return -EINVAL;
	}

	key.objectid = 0;
	key.type = BTRFS_DEV_REPLACE_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, dev_root, &key, &path, 0, 0);
	btrfs_release_path(&path);
	if (ret < 0) {
		errno = -ret;
		error("failed to check the dev-reaplce status: %m");
		return ret;
	}
	if (ret == 0) {
		error("running dev-replace detected, please finish or cancel it.");
		return -EINVAL;
	}
	return 0;
}

int btrfs_change_csum_type(struct btrfs_fs_info *fs_info, u16 new_csum_type)
{
	int ret;

	/* Phase 0, check conflicting features. */
	ret = check_csum_change_requreiment(fs_info);
	if (ret < 0)
		return ret;

	/*
	 * Phase 1, generate new data csums.
	 *
	 * The new data csums would have a different key objectid, and there
	 * will be a temporary item in root tree to indicate the new checksum
	 * algo.
	 */

	/* Phase 2, delete the old data csums. */

	/* Phase 3, change the new csum key objectid */

	/*
	 * Phase 4, change the csums for metadata.
	 *
	 * This has to be done in-place, as we don't have a good method
	 * like relocation in progs.
	 * Thus we have to support reading a tree block with either csum.
	 */
	return -EOPNOTSUPP;
}
