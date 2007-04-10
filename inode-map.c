#include <stdio.h>
#include <stdlib.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"

/*
 * walks the btree of allocated inodes and find a hole.
 */
int btrfs_find_free_objectid(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     u64 dirid, u64 *objectid)
{
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;
	u64 hole_size = 0;
	int slot = 0;
	u64 last_ino;
	int start_found;
	struct btrfs_leaf *l;
	struct btrfs_key search_key;
	u64 search_start = dirid;

	if (root->fs_info->last_inode_alloc_dirid == dirid)
		search_start = root->fs_info->last_inode_alloc;

	if (search_start < BTRFS_FIRST_FREE_OBJECTID)
		search_start = BTRFS_FIRST_FREE_OBJECTID;
	search_key.objectid = search_start;
	search_key.flags = 0;
	search_key.offset = 0;

	btrfs_init_path(&path);
	start_found = 0;
	ret = btrfs_search_slot(trans, root, &search_key, &path, 0, 0);
	if (ret < 0)
		goto error;

	if (path.slots[0] > 0)
		path.slots[0]--;

	while (1) {
		l = &path.nodes[0]->leaf;
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(&l->header)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret == 0)
				continue;
			if (ret < 0)
				goto error;
			if (!start_found) {
				*objectid = search_start;
				start_found = 1;
				goto found;
			}
			*objectid = last_ino > search_start ?
				last_ino : search_start;
			goto found;
		}
		btrfs_disk_key_to_cpu(&key, &l->items[slot].key);
		if (key.objectid >= search_start) {
			if (start_found) {
				if (last_ino < search_start)
					last_ino = search_start;
				hole_size = key.objectid - last_ino;
				if (hole_size > 0) {
					*objectid = last_ino;
					goto found;
				}
			}
		}
		start_found = 1;
		last_ino = key.objectid + 1;
		path.slots[0]++;
	}
	// FIXME -ENOSPC
found:
	root->fs_info->last_inode_alloc = *objectid;
	root->fs_info->last_inode_alloc_dirid = dirid;
	btrfs_release_path(root, &path);
	BUG_ON(*objectid < search_start);
	return 0;
error:
	btrfs_release_path(root, &path);
	return ret;
}
