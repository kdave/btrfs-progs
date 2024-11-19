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
#include <stddef.h>
#include "kernel-shared/accessors.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/free-space-tree.h"
#include "kernel-shared/transaction.h"
#include "common/extent-tree-utils.h"

/*
 * Search in extent tree to found next meta/data extent. Caller needs to check
 * for no-hole or skinny metadata features.
 */
int btrfs_next_extent_item(struct btrfs_root *root, struct btrfs_path *path,
			   u64 max_objectid)
{
	struct btrfs_key found_key;
	int ret;

	while (1) {
		ret = btrfs_next_item(root, path);
		if (ret)
			return ret;
		btrfs_item_key_to_cpu(path->nodes[0], &found_key, path->slots[0]);
		if (found_key.objectid > max_objectid)
			return 1;
		if (found_key.type == BTRFS_EXTENT_ITEM_KEY ||
		    found_key.type == BTRFS_METADATA_ITEM_KEY)
		return 0;
	}
}
