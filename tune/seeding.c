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
#include "kernel-shared/ctree.h"
#include "kernel-shared/transaction.h"
#include "common/messages.h"

int update_seeding_flag(struct btrfs_root *root, const char *device, int set_flag, int force)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *disk_super;
	u64 super_flags;
	int ret;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_flags(disk_super);
	if (set_flag) {
		if (super_flags & BTRFS_SUPER_FLAG_SEEDING) {
			if (force)
				return 0;
			else
				warning("seeding flag is already set on %s",
						device);
			return 1;
		}
		if (btrfs_super_log_root(disk_super)) {
			error("filesystem with dirty log detected, not setting seed flag");
			return 1;
		}
		super_flags |= BTRFS_SUPER_FLAG_SEEDING;
	} else {
		if (!(super_flags & BTRFS_SUPER_FLAG_SEEDING)) {
			warning("seeding flag is not set on %s", device);
			return 1;
		}
		super_flags &= ~BTRFS_SUPER_FLAG_SEEDING;
		warning("seeding flag cleared on %s", device);
	}

	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	btrfs_set_super_flags(disk_super, super_flags);
	ret = btrfs_commit_transaction(trans, root);

	return ret;
}

