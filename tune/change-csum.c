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

static int delete_csum_items(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root = btrfs_csum_root(fs_info, 0);
	struct btrfs_path path;
	struct btrfs_key key;
	int nr;
	int ret;

	btrfs_init_path(&path);

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.type = BTRFS_EXTENT_CSUM_KEY;
	key.offset = 0;

	while (1) {
		ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
		if (ret < 0)
			goto out;

		nr = btrfs_header_nritems(path.nodes[0]);
		if (!nr)
			break;

		path.slots[0] = 0;
		ret = btrfs_del_items(trans, root, &path, 0, nr);
		if (ret)
			goto out;

		btrfs_release_path(&path);
	}

	ret = 0;
out:
	btrfs_release_path(&path);
	return ret;
}

static int change_extents_csum(struct btrfs_fs_info *fs_info, int csum_type)
{
	struct btrfs_root *root = btrfs_extent_root(fs_info, 0);
	struct btrfs_path path;
	struct btrfs_key key = {0, 0, 0};
	int ret = 0;

	btrfs_init_path(&path);
	/*
	 * Here we don't use transaction as it will takes a lot of reserve
	 * space, and that will make a near-full btrfs unable to change csums
	 */
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;

	while (1) {
		struct btrfs_extent_item *ei;
		struct extent_buffer *eb;
		u64 flags;
		u64 bytenr;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY)
			goto next;
		ei = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_extent_item);
		flags = btrfs_extent_flags(path.nodes[0], ei);
		if (!(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK))
			goto next;

		bytenr = key.objectid;
		eb = read_tree_block(fs_info, bytenr, 0);
		if (IS_ERR(eb)) {
			error("failed to read tree block: %llu", bytenr);
			ret = PTR_ERR(eb);
			goto out;
		}
		/* Only rewrite block */
		ret = write_tree_block(NULL, fs_info, eb);
		free_extent_buffer(eb);
		if (ret < 0) {
			error("failed to change csum of tree block: %llu", bytenr);
			goto out;
		}
next:
		ret = btrfs_next_item(root, &path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}

out:
	btrfs_release_path(&path);
	return ret;
}

static int change_devices_csum(struct btrfs_root *root, int csum_type)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_path path;
	struct btrfs_key key = {0, 0, 0};
	int ret = 0;

	btrfs_init_path(&path);
	/* No transaction again */
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;

	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.type != BTRFS_DEV_ITEM_KEY ||
		    key.objectid != BTRFS_DEV_ITEMS_OBJECTID)
			goto next;
		/* Only rewrite block */
		ret = write_tree_block(NULL, fs_info, path.nodes[0]);
		if (ret < 0)
			goto out;
next:
		ret = btrfs_next_item(root, &path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}
out:
	btrfs_release_path(&path);
	return ret;
}

static int populate_csum(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *fs_info, char *buf, u64 start,
			 u64 len)
{
	u64 offset = 0;
	u64 sectorsize;
	int ret = 0;

	while (offset < len) {
		sectorsize = fs_info->sectorsize;
		ret = read_data_from_disk(fs_info, buf, start + offset,
					  &sectorsize, 0);
		if (ret)
			break;
		ret = btrfs_csum_file_block(trans, start + len, start + offset,
				buf, sectorsize);
		if (ret)
			break;
		offset += sectorsize;
	}
	return ret;
}

static int fill_csum_tree_from_extent(struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *extent_root = btrfs_extent_root(fs_info, 0);
	struct btrfs_trans_handle *trans;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	struct extent_buffer *leaf;
	char *buf;
	struct btrfs_key key;
	int ret;

	trans = btrfs_start_transaction(extent_root, 1);
	if (trans == NULL) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return -EINVAL;
	}

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}

	buf = malloc(fs_info->sectorsize);
	if (!buf) {
		btrfs_release_path(&path);
		return -ENOMEM;
	}

	ret = delete_csum_items(trans, fs_info);
	if (ret) {
		error("unable to delete all checksum items: %d", ret);
		return -EIO;
	}

	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(extent_root, &path);
			if (ret < 0)
				break;
			if (ret) {
				ret = 0;
				break;
			}
		}
		leaf = path.nodes[0];

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}

		ei = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_extent_item);
		if (!(btrfs_extent_flags(leaf, ei) & BTRFS_EXTENT_FLAG_DATA)) {
			path.slots[0]++;
			continue;
		}

		ret = populate_csum(trans, fs_info, buf, key.objectid, key.offset);
		if (ret)
			break;
		path.slots[0]++;
	}

	btrfs_release_path(&path);
	free(buf);

	/* dont' commit if thre's error */
	ret = btrfs_commit_transaction(trans, extent_root);

	return ret;
}

int rewrite_checksums(struct btrfs_root *root, int csum_type)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_super_block *disk_super;
	struct btrfs_trans_handle *trans;
	u64 super_flags;
	int ret;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_flags(disk_super);

	/* FIXME: Sanity checks */
	if (0) {
		error("UUID rewrite in progress, cannot change fsid");
		return 1;
	}

	fs_info->force_csum_type = csum_type;

	/* Step 1 sets the in progress flag, no other change to the sb */
	pr_verbose(LOG_DEFAULT, "Set superblock flag CHANGING_CSUM\n");
	trans = btrfs_start_transaction(root, 1);
	super_flags |= BTRFS_SUPER_FLAG_CHANGING_CSUM;
	btrfs_set_super_flags(disk_super, super_flags);
	ret = btrfs_commit_transaction(trans, root);
	if (ret < 0)
		return ret;

	/* Change extents first */
	pr_verbose(LOG_DEFAULT, "Change fsid in extents\n");
	ret = change_extents_csum(fs_info, csum_type);
	if (ret < 0) {
		error("failed to change csum of metadata: %d", ret);
		goto out;
	}

	/* Then devices */
	pr_verbose(LOG_DEFAULT, "Change csum in chunk tree\n");
	ret = change_devices_csum(fs_info->chunk_root, csum_type);
	if (ret < 0) {
		error("failed to change UUID of devices: %d", ret);
		goto out;
	}

	/* DATA */
	pr_verbose(LOG_DEFAULT, "Change csum of data blocks\n");
	ret = fill_csum_tree_from_extent(fs_info);
	if (ret < 0)
		goto out;

	/* Last, change fsid in super */
	ret = write_all_supers(fs_info);
	if (ret < 0)
		goto out;

	/* All checksums done, drop the flag, super block csum will get updated */
	pr_verbose(LOG_DEFAULT, "Clear superblock flag CHANGING_CSUM\n");
	super_flags = btrfs_super_flags(fs_info->super_copy);
	super_flags &= ~BTRFS_SUPER_FLAG_CHANGING_CSUM;
	btrfs_set_super_flags(fs_info->super_copy, super_flags);
	btrfs_set_super_csum_type(disk_super, csum_type);
	ret = write_all_supers(fs_info);
	pr_verbose(LOG_DEFAULT, "Checksum change finished\n");
out:
	/* check errors */

	return ret;
}
