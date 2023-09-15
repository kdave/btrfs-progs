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
#include <stdbool.h>
#include <string.h>
#include <uuid/uuid.h>
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/volumes.h"
#include "common/messages.h"
#include "tune/tune.h"

/*
 * Return 0 for no unfinished metadata_uuid change.
 * Return >0 for unfinished metadata_uuid change, and restore unfinished
 * fsid/metadata_uuid into fsid_ret/metadata_uuid_ret.
 */
static int check_unfinished_metadata_uuid(struct btrfs_fs_info *fs_info,
					  uuid_t fsid_ret,
					  uuid_t metadata_uuid_ret)
{
	struct btrfs_root *tree_root = fs_info->tree_root;

	if (fs_info->fs_devices->inconsistent_super) {
		memcpy(fsid_ret, fs_info->super_copy->fsid, BTRFS_FSID_SIZE);
		read_extent_buffer(tree_root->node, metadata_uuid_ret,
				   btrfs_header_chunk_tree_uuid(tree_root->node),
				   BTRFS_UUID_SIZE);
		return 1;
	}
	return 0;
}

int set_metadata_uuid(struct btrfs_root *root, const char *new_fsid_string)
{
	struct btrfs_super_block *disk_super;
	uuid_t fsid, metadata_uuid;
	struct btrfs_trans_handle *trans;
	bool new_fsid = true;
	u64 incompat_flags;
	bool fsid_changed;
	u64 super_flags;
	int ret;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_flags(disk_super);
	incompat_flags = btrfs_super_incompat_flags(disk_super);
	fsid_changed = incompat_flags & BTRFS_FEATURE_INCOMPAT_METADATA_UUID;

	if (super_flags & BTRFS_SUPER_FLAG_SEEDING) {
		error("cannot set metadata UUID on a seed device");
		return 1;
	}

	if (check_unfinished_metadata_uuid(root->fs_info, fsid, metadata_uuid)) {
		if (new_fsid_string) {
			uuid_t tmp;

			uuid_parse(new_fsid_string, tmp);
			if (memcmp(tmp, fsid, BTRFS_FSID_SIZE) != 0) {
				error(
		"new fsid %s is not the same with unfinished fsid change",
				      new_fsid_string);
				return -EINVAL;
			}
		}
	} else {
		if (new_fsid_string)
			uuid_parse(new_fsid_string, fsid);
		else
			uuid_generate(fsid);
	}

	new_fsid = (memcmp(fsid, disk_super->fsid, BTRFS_FSID_SIZE) != 0);

	/* Step 1 sets the in progress flag */
	trans = btrfs_start_transaction(root, 1);
	super_flags |= BTRFS_SUPER_FLAG_CHANGING_FSID_V2;
	btrfs_set_super_flags(disk_super, super_flags);
	ret = btrfs_commit_transaction(trans, root);
	if (ret < 0)
		return ret;

	if (new_fsid && fsid_changed && memcmp(disk_super->metadata_uuid,
					       fsid, BTRFS_FSID_SIZE) == 0) {
		/*
		 * Changing fsid to be the same as metadata uuid, so just
		 * disable the flag
		 */
		memcpy(disk_super->fsid, &fsid, BTRFS_FSID_SIZE);
		incompat_flags &= ~BTRFS_FEATURE_INCOMPAT_METADATA_UUID;
		btrfs_set_super_incompat_flags(disk_super, incompat_flags);
		memset(disk_super->metadata_uuid, 0, BTRFS_FSID_SIZE);
	} else if (new_fsid && fsid_changed && memcmp(disk_super->metadata_uuid,
						      fsid, BTRFS_FSID_SIZE)) {
		/*
		 * Changing fsid on an already changed FS, in this case we
		 * only change the fsid and don't touch metadata uuid as it
		 * has already the correct value
		 */
		memcpy(disk_super->fsid, &fsid, BTRFS_FSID_SIZE);
	} else if (new_fsid && !fsid_changed) {
		/*
		 * First time changing the fsid, copy the fsid to metadata_uuid
		 */
		incompat_flags |= BTRFS_FEATURE_INCOMPAT_METADATA_UUID;
		btrfs_set_super_incompat_flags(disk_super, incompat_flags);
		memcpy(disk_super->metadata_uuid, disk_super->fsid,
		       BTRFS_FSID_SIZE);
		memcpy(disk_super->fsid, &fsid, BTRFS_FSID_SIZE);
	}

	trans = btrfs_start_transaction(root, 1);

	/*
	 * Step 2 is to write the metadata_uuid, set the incompat flag and
	 * clear the in progress flag
	 */
	super_flags &= ~BTRFS_SUPER_FLAG_CHANGING_FSID_V2;
	btrfs_set_super_flags(disk_super, super_flags);

	/* Then actually copy the metadata uuid and set the incompat bit */

	return btrfs_commit_transaction(trans, root);
}
