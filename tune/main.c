/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
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

#include "kerncompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <uuid/uuid.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/extent_io.h"
#include "common/defs.h"
#include "common/utils.h"
#include "common/extent-cache.h"
#include "common/open-utils.h"
#include "common/parse-utils.h"
#include "common/device-scan.h"
#include "common/messages.h"
#include "common/string-utils.h"
#include "common/help.h"
#include "common/box.h"
#include "ioctl.h"

static char *device;
static int force = 0;

static int update_seeding_flag(struct btrfs_root *root, int set_flag)
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

/*
 * Return 0 for no unfinished fsid change.
 * Return >0 for unfinished fsid change, and restore unfinished fsid/
 * chunk_tree_id into fsid_ret/chunk_id_ret.
 */
static int check_unfinished_fsid_change(struct btrfs_fs_info *fs_info,
					uuid_t fsid_ret, uuid_t chunk_id_ret)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	u64 flags = btrfs_super_flags(fs_info->super_copy);

	if (flags & (BTRFS_SUPER_FLAG_CHANGING_FSID |
		     BTRFS_SUPER_FLAG_CHANGING_FSID_V2)) {
		memcpy(fsid_ret, fs_info->super_copy->fsid, BTRFS_FSID_SIZE);
		read_extent_buffer(tree_root->node, chunk_id_ret,
				btrfs_header_chunk_tree_uuid(tree_root->node),
				BTRFS_UUID_SIZE);
		return 1;
	}
	return 0;
}

static int set_metadata_uuid(struct btrfs_root *root, const char *uuid_string)
{
	struct btrfs_super_block *disk_super;
	uuid_t new_fsid, unused1, unused2;
	struct btrfs_trans_handle *trans;
	bool new_uuid = true;
	u64 incompat_flags;
	bool uuid_changed;
	u64 super_flags;
	int ret;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_flags(disk_super);
	incompat_flags = btrfs_super_incompat_flags(disk_super);
	uuid_changed = incompat_flags & BTRFS_FEATURE_INCOMPAT_METADATA_UUID;

	if (super_flags & BTRFS_SUPER_FLAG_SEEDING) {
		error("cannot set metadata UUID on a seed device");
		return 1;
	}

	if (check_unfinished_fsid_change(root->fs_info, unused1, unused2)) {
		error("UUID rewrite in progress, cannot change fsid");
		return 1;
	}

	if (uuid_string)
		uuid_parse(uuid_string, new_fsid);
	else
		uuid_generate(new_fsid);

	new_uuid = (memcmp(new_fsid, disk_super->fsid, BTRFS_FSID_SIZE) != 0);

	/* Step 1 sets the in progress flag */
	trans = btrfs_start_transaction(root, 1);
	super_flags |= BTRFS_SUPER_FLAG_CHANGING_FSID_V2;
	btrfs_set_super_flags(disk_super, super_flags);
	ret = btrfs_commit_transaction(trans, root);
	if (ret < 0)
		return ret;

	if (new_uuid && uuid_changed && memcmp(disk_super->metadata_uuid,
					       new_fsid, BTRFS_FSID_SIZE) == 0) {
		/*
		 * Changing fsid to be the same as metadata uuid, so just
		 * disable the flag
		 */
		memcpy(disk_super->fsid, &new_fsid, BTRFS_FSID_SIZE);
		incompat_flags &= ~BTRFS_FEATURE_INCOMPAT_METADATA_UUID;
		btrfs_set_super_incompat_flags(disk_super, incompat_flags);
		memset(disk_super->metadata_uuid, 0, BTRFS_FSID_SIZE);
	} else if (new_uuid && uuid_changed && memcmp(disk_super->metadata_uuid,
						new_fsid, BTRFS_FSID_SIZE)) {
		/*
		 * Changing fsid on an already changed FS, in this case we
		 * only change the fsid and don't touch metadata uuid as it
		 * has already the correct value
		 */
		memcpy(disk_super->fsid, &new_fsid, BTRFS_FSID_SIZE);
	} else if (new_uuid && !uuid_changed) {
		/*
		 * First time changing the fsid, copy the fsid to metadata_uuid
		 */
		incompat_flags |= BTRFS_FEATURE_INCOMPAT_METADATA_UUID;
		btrfs_set_super_incompat_flags(disk_super, incompat_flags);
		memcpy(disk_super->metadata_uuid, disk_super->fsid,
		       BTRFS_FSID_SIZE);
		memcpy(disk_super->fsid, &new_fsid, BTRFS_FSID_SIZE);
	} else {
		/* Setting the same fsid as current, do nothing */
		return 0;
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
		/* printf("CSUM: start %llu\n", eb->start); */
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
		/* fixme */
		printf("cannot start transaction\n");
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

static int rewrite_checksums(struct btrfs_root *root, int csum_type)
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
	printf("Set superblock flag CHANGING_CSUM\n");
	trans = btrfs_start_transaction(root, 1);
	super_flags |= BTRFS_SUPER_FLAG_CHANGING_CSUM;
	btrfs_set_super_flags(disk_super, super_flags);
	ret = btrfs_commit_transaction(trans, root);
	if (ret < 0)
		return ret;

	/* Change extents first */
	printf("Change fsid in extents\n");
	ret = change_extents_csum(fs_info, csum_type);
	if (ret < 0) {
		error("failed to change csum of metadata: %d", ret);
		goto out;
	}

	/* Then devices */
	printf("Change csum in chunk tree\n");
	ret = change_devices_csum(fs_info->chunk_root, csum_type);
	if (ret < 0) {
		error("failed to change UUID of devices: %d", ret);
		goto out;
	}

	/* DATA */
	printf("Change csum of data blocks\n");
	ret = fill_csum_tree_from_extent(fs_info);
	if (ret < 0)
		goto out;

	/* Last, change fsid in super */
	ret = write_all_supers(fs_info);
	if (ret < 0)
		goto out;

	/* All checksums done, drop the flag, super block csum will get updated */
	printf("Clear superblock flag CHANGING_CSUM\n");
	super_flags = btrfs_super_flags(fs_info->super_copy);
	super_flags &= ~BTRFS_SUPER_FLAG_CHANGING_CSUM;
	btrfs_set_super_flags(fs_info->super_copy, super_flags);
	btrfs_set_super_csum_type(disk_super, csum_type);
	ret = write_all_supers(fs_info);
	printf("Checksum change finished\n");
out:
	/* check errors */

	return ret;
}

static int set_super_incompat_flags(struct btrfs_root *root, u64 flags)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_super_block *disk_super;
	u64 super_flags;
	int ret;

	disk_super = root->fs_info->super_copy;
	super_flags = btrfs_super_incompat_flags(disk_super);
	super_flags |= flags;
	trans = btrfs_start_transaction(root, 1);
	BUG_ON(IS_ERR(trans));
	btrfs_set_super_incompat_flags(disk_super, super_flags);
	ret = btrfs_commit_transaction(trans, root);

	return ret;
}

static int change_buffer_header_uuid(struct extent_buffer *eb, uuid_t new_fsid)
{
	struct btrfs_fs_info *fs_info = eb->fs_info;
	int same_fsid = 1;
	int same_chunk_tree_uuid = 1;
	int ret;

	same_fsid = !memcmp_extent_buffer(eb, new_fsid, btrfs_header_fsid(),
					  BTRFS_FSID_SIZE);
	same_chunk_tree_uuid =
		!memcmp_extent_buffer(eb, fs_info->new_chunk_tree_uuid,
				btrfs_header_chunk_tree_uuid(eb),
				BTRFS_UUID_SIZE);
	if (same_fsid && same_chunk_tree_uuid)
		return 0;
	if (!same_fsid)
		write_extent_buffer(eb, new_fsid, btrfs_header_fsid(),
				    BTRFS_FSID_SIZE);
	if (!same_chunk_tree_uuid)
		write_extent_buffer(eb, fs_info->new_chunk_tree_uuid,
				    btrfs_header_chunk_tree_uuid(eb),
				    BTRFS_UUID_SIZE);
	ret = write_tree_block(NULL, fs_info, eb);

	return ret;
}

static int change_extents_uuid(struct btrfs_fs_info *fs_info, uuid_t new_fsid)
{
	struct btrfs_root *root = btrfs_extent_root(fs_info, 0);
	struct btrfs_path path;
	struct btrfs_key key = {0, 0, 0};
	int ret = 0;

	btrfs_init_path(&path);
	/*
	 * Here we don't use transaction as it will takes a lot of reserve
	 * space, and that will make a near-full btrfs unable to change uuid
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
		ret = change_buffer_header_uuid(eb, new_fsid);
		free_extent_buffer(eb);
		if (ret < 0) {
			error("failed to change uuid of tree block: %llu",
				bytenr);
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

static int change_device_uuid(struct extent_buffer *eb, int slot,
			      uuid_t new_fsid)
{
	struct btrfs_dev_item *di;
	struct btrfs_fs_info *fs_info = eb->fs_info;
	int ret = 0;

	di = btrfs_item_ptr(eb, slot, struct btrfs_dev_item);
	if (!memcmp_extent_buffer(eb, new_fsid,
				  (unsigned long)btrfs_device_fsid(di),
				  BTRFS_FSID_SIZE))
		return ret;

	write_extent_buffer(eb, new_fsid, (unsigned long)btrfs_device_fsid(di),
			    BTRFS_FSID_SIZE);
	ret = write_tree_block(NULL, fs_info, eb);

	return ret;
}

static int change_devices_uuid(struct btrfs_root *root, uuid_t new_fsid)
{
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
		ret = change_device_uuid(path.nodes[0], path.slots[0],
					 new_fsid);
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

static int change_fsid_prepare(struct btrfs_fs_info *fs_info, uuid_t new_fsid)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	u64 flags = btrfs_super_flags(fs_info->super_copy);
	int ret = 0;

	flags |= BTRFS_SUPER_FLAG_CHANGING_FSID;
	btrfs_set_super_flags(fs_info->super_copy, flags);

	memcpy(fs_info->super_copy->fsid, new_fsid, BTRFS_FSID_SIZE);
	ret = write_all_supers(fs_info);
	if (ret < 0)
		return ret;

	/* Also need to change the metadatauuid of the fs info */
	memcpy(fs_info->fs_devices->metadata_uuid, new_fsid, BTRFS_FSID_SIZE);

	/* also restore new chunk_tree_id into tree_root for restore */
	write_extent_buffer(tree_root->node, fs_info->new_chunk_tree_uuid,
			    btrfs_header_chunk_tree_uuid(tree_root->node),
			    BTRFS_UUID_SIZE);
	return write_tree_block(NULL, fs_info, tree_root->node);
}

static int change_fsid_done(struct btrfs_fs_info *fs_info)
{
	u64 flags = btrfs_super_flags(fs_info->super_copy);

	flags &= ~BTRFS_SUPER_FLAG_CHANGING_FSID;
	btrfs_set_super_flags(fs_info->super_copy, flags);

	return write_all_supers(fs_info);
}


/*
 * Change fsid of a given fs.
 *
 * If new_fsid_str is not given, use a random generated UUID.
 * Caller should check new_fsid_str is valid
 */
static int change_uuid(struct btrfs_fs_info *fs_info, const char *new_fsid_str)
{
	uuid_t new_fsid;
	uuid_t new_chunk_id;
	uuid_t old_fsid;
	char uuid_buf[BTRFS_UUID_UNPARSED_SIZE];
	int ret = 0;

	if (check_unfinished_fsid_change(fs_info, new_fsid, new_chunk_id)) {
		if (new_fsid_str) {
			uuid_t tmp;

			uuid_parse(new_fsid_str, tmp);
			if (memcmp(tmp, new_fsid, BTRFS_FSID_SIZE)) {
				error(
		"new fsid %s is not the same with unfinished fsid change",
					new_fsid_str);
				return -EINVAL;
			}
		}
	} else {
		if (new_fsid_str)
			uuid_parse(new_fsid_str, new_fsid);
		else
			uuid_generate(new_fsid);

		uuid_generate(new_chunk_id);
	}
	fs_info->new_chunk_tree_uuid = new_chunk_id;

	memcpy(old_fsid, (const char*)fs_info->fs_devices->fsid, BTRFS_UUID_SIZE);
	uuid_unparse(old_fsid, uuid_buf);
	printf("Current fsid: %s\n", uuid_buf);

	uuid_unparse(new_fsid, uuid_buf);
	printf("New fsid: %s\n", uuid_buf);
	/* Now we can begin fsid change */
	printf("Set superblock flag CHANGING_FSID\n");
	ret = change_fsid_prepare(fs_info, new_fsid);
	if (ret < 0)
		goto out;

	/* Change extents first */
	printf("Change fsid in extents\n");
	ret = change_extents_uuid(fs_info, new_fsid);
	if (ret < 0) {
		error("failed to change UUID of metadata: %d", ret);
		goto out;
	}

	/* Then devices */
	printf("Change fsid on devices\n");
	ret = change_devices_uuid(fs_info->chunk_root, new_fsid);
	if (ret < 0) {
		error("failed to change UUID of devices: %d", ret);
		goto out;
	}

	/* Last, change fsid in super */
	memcpy(fs_info->fs_devices->fsid, new_fsid, BTRFS_FSID_SIZE);
	memcpy(fs_info->super_copy->fsid, new_fsid, BTRFS_FSID_SIZE);
	ret = write_all_supers(fs_info);
	if (ret < 0)
		goto out;

	/* Now fsid change is done */
	printf("Clear superblock flag CHANGING_FSID\n");
	ret = change_fsid_done(fs_info);
	fs_info->new_chunk_tree_uuid = NULL;
	printf("Fsid change finished\n");
out:
	return ret;
}

/* After this many block groups we need to commit transaction. */
#define BLOCK_GROUP_BATCH	64

static int convert_to_bg_tree(struct btrfs_fs_info *fs_info)
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
			error("no more block group before bytenr %llu",
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
	printf("Converted the filesystem to block group tree feature\n");
	return 0;
error:
	btrfs_abort_transaction(trans, ret);
	return ret;
}

static void print_usage(void)
{
	printf("usage: btrfstune [options] device\n");
	printf("Tune settings of filesystem features on an unmounted device\n\n");
	printf("Options:\n");
	printf("  change feature status:\n");
	printf("\t-r          enable extended inode refs (mkfs: extref, for hardlink limits)\n");
	printf("\t-x          enable skinny metadata extent refs (mkfs: skinny-metadata)\n");
	printf("\t-n          enable no-holes feature (mkfs: no-holes, more efficient sparse file representation)\n");
	printf("\t-S <0|1>    set/unset seeding status of a device\n");
	printf("  uuid changes:\n");
	printf("\t-u          rewrite fsid, use a random one\n");
	printf("\t-U UUID     rewrite fsid to UUID\n");
	printf("\t-m          change fsid in metadata_uuid to a random UUID\n");
	printf("\t            (incompat change, more lightweight than -u|-U)\n");
	printf("\t-M UUID     change fsid in metadata_uuid to UUID\n");
	printf("  general:\n");
	printf("\t-f          allow dangerous operations, make sure that you are aware of the dangers\n");
	printf("\t--help      print this help\n");
#if EXPERIMENTAL
	printf("\nEXPERIMENTAL FEATURES:\n");
	printf("  checksum changes:\n");
	printf("\t--csum CSUM	switch checksum for data and metadata to CSUM\n");
	printf("\t-b          enable block group tree (mkfs: block-group-tree, for less mount time)\n");
#endif
}

int BOX_MAIN(btrfstune)(int argc, char *argv[])
{
	struct btrfs_root *root;
	unsigned ctree_flags = OPEN_CTREE_WRITES;
	int success = 0;
	int total = 0;
	int seeding_flag = 0;
	u64 seeding_value = 0;
	int random_fsid = 0;
	int change_metadata_uuid = 0;
	bool to_bg_tree = false;
	int csum_type = -1;
	char *new_fsid_str = NULL;
	int ret;
	u64 super_flags = 0;
	int fd = -1;

	while(1) {
		enum { GETOPT_VAL_CSUM = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{ "help", no_argument, NULL, GETOPT_VAL_HELP},
#if EXPERIMENTAL
			{ "csum", required_argument, NULL, GETOPT_VAL_CSUM },
#endif
			{ NULL, 0, NULL, 0 }
		};
#if EXPERIMENTAL
		int c = getopt_long(argc, argv, "S:rxfuU:nmM:b", long_options, NULL);
#else
		int c = getopt_long(argc, argv, "S:rxfuU:nmM:", long_options, NULL);
#endif

		if (c < 0)
			break;
		switch(c) {
		case 'b':
			btrfs_warn_experimental("Feature: conversion to block-group-tree");
			to_bg_tree = true;
			break;
		case 'S':
			seeding_flag = 1;
			seeding_value = arg_strtou64(optarg);
			break;
		case 'r':
			super_flags |= BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF;
			break;
		case 'x':
			super_flags |= BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA;
			break;
		case 'n':
			super_flags |= BTRFS_FEATURE_INCOMPAT_NO_HOLES;
			break;
		case 'f':
			force = 1;
			break;
		case 'U':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			new_fsid_str = optarg;
			break;
		case 'u':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			random_fsid = 1;
			break;
		case 'M':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			change_metadata_uuid = 1;
			new_fsid_str = optarg;
			break;
		case 'm':
			ctree_flags |= OPEN_CTREE_IGNORE_FSID_MISMATCH;
			change_metadata_uuid = 1;
			break;
#if EXPERIMENTAL
		case GETOPT_VAL_CSUM:
			btrfs_warn_experimental(
				"Switching checksums is experimental, do not use for valuable data!");
			ctree_flags |= OPEN_CTREE_SKIP_CSUM_CHECK;
			csum_type = parse_csum_type(optarg);
			printf("Switch csum to %s\n",
					btrfs_super_csum_name(csum_type));
			break;
#endif
		case GETOPT_VAL_HELP:
		default:
			print_usage();
			return c != GETOPT_VAL_HELP;
		}
	}

	set_argv0(argv);
	device = argv[optind];
	if (check_argc_exact(argc - optind, 1))
		return 1;

	if (random_fsid && new_fsid_str) {
		error("random fsid can't be used with specified fsid");
		return 1;
	}
	if (!super_flags && !seeding_flag && !(random_fsid || new_fsid_str) &&
	    !change_metadata_uuid && csum_type == -1 && !to_bg_tree) {
		error("at least one option should be specified");
		print_usage();
		return 1;
	}

	if (new_fsid_str) {
		uuid_t tmp;

		ret = uuid_parse(new_fsid_str, tmp);
		if (ret < 0) {
			error("could not parse UUID: %s", new_fsid_str);
			return 1;
		}
		if (!test_uuid_unique(new_fsid_str)) {
			error("fsid %s is not unique", new_fsid_str);
			return 1;
		}
	}

	fd = open(device, O_RDWR);
	if (fd < 0) {
		error("mount check: cannot open %s: %m", device);
		return 1;
	}

	ret = check_mounted_where(fd, device, NULL, 0, NULL,
			SBREAD_IGNORE_FSID_MISMATCH);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status of %s: %m", device);
		close(fd);
		return 1;
	} else if (ret) {
		error("%s is mounted", device);
		close(fd);
		return 1;
	}

	root = open_ctree_fd(fd, device, 0, ctree_flags);

	if (!root) {
		error("open ctree failed");
		return 1;
	}

	if (to_bg_tree) {
		if (btrfs_fs_compat_ro(root->fs_info, BLOCK_GROUP_TREE)) {
			error("the filesystem already has block group tree feature");
			ret = 1;
			goto out;
		}
		if (!btrfs_fs_compat_ro(root->fs_info, FREE_SPACE_TREE_VALID)) {
			error("the filesystem doesn't have space cache v2, needs to be mounted with \"-o space_cache=v2\" first");
			ret = 1;
			goto out;
		}
		ret = convert_to_bg_tree(root->fs_info);
		if (ret < 0) {
			error("failed to convert the filesystem to block group tree feature");
			goto out;
		}
		goto out;
	}
	if (seeding_flag) {
		if (btrfs_fs_incompat(root->fs_info, METADATA_UUID)) {
			error("SEED flag cannot be changed on a metadata-uuid changed fs");
			ret = 1;
			goto out;
		}

		if (!seeding_value && !force) {
			warning(
"this is dangerous, clearing the seeding flag may cause the derived device not to be mountable!");
			ret = ask_user("We are going to clear the seeding flag, are you sure?");
			if (!ret) {
				error("clear seeding flag canceled");
				ret = 1;
				goto out;
			}
		}

		ret = update_seeding_flag(root, seeding_value);
		if (!ret)
			success++;
		total++;
	}

	if (super_flags) {
		ret = set_super_incompat_flags(root, super_flags);
		if (!ret)
			success++;
		total++;
	}

	if (csum_type != -1) {
		/* TODO: check conflicting flags */
		printf("Proceed to switch checksums\n");
		ret = rewrite_checksums(root, csum_type);
	}

	if (change_metadata_uuid) {
		if (seeding_flag) {
			error("not allowed to set both seeding flag and uuid metadata");
			ret = 1;
			goto out;
		}

		if (new_fsid_str)
			ret = set_metadata_uuid(root, new_fsid_str);
		else
			ret = set_metadata_uuid(root, NULL);

		if (!ret)
			success++;
		total++;
	}

	if (random_fsid || (new_fsid_str && !change_metadata_uuid)) {
		if (btrfs_fs_incompat(root->fs_info, METADATA_UUID)) {
			error(
		"Cannot rewrite fsid while METADATA_UUID flag is active. \n"
		"Ensure fsid and metadata_uuid match before retrying.");
			ret = 1;
			goto out;
		}

		if (!force) {
			warning(
"it's recommended to run 'btrfs check --readonly' before this operation.\n"
"\tThe whole operation must finish before the filesystem can be mounted again.\n"
"\tIf cancelled or interrupted, run 'btrfstune -u' to restart.");
			ret = ask_user("We are going to change UUID, are your sure?");
			if (!ret) {
				error("UUID change canceled");
				ret = 1;
				goto out;
			}
		}
		ret = change_uuid(root->fs_info, new_fsid_str);
		if (!ret)
			success++;
		total++;
	}

	if (success == total) {
		ret = 0;
	} else {
		root->fs_info->readonly = 1;
		ret = 1;
		error("btrfstune failed");
	}
out:
	close_ctree(root);
	btrfs_close_all_devices();

	return ret;
}
