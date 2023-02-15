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
#include <string.h>
#include <uuid/uuid.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/volumes.h"
#include "common/defs.h"
#include "common/messages.h"
#include "ioctl.h"

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

static int change_extent_tree_uuid(struct btrfs_fs_info *fs_info, uuid_t new_fsid)
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

static int change_chunk_tree_uuid(struct btrfs_root *root, uuid_t new_fsid)
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

static int change_fsid_done(struct btrfs_fs_info *fs_info)
{
	u64 flags = btrfs_super_flags(fs_info->super_copy);

	flags &= ~BTRFS_SUPER_FLAG_CHANGING_FSID;
	btrfs_set_super_flags(fs_info->super_copy, flags);

	return write_all_supers(fs_info);
}

/*
 * Return 0 for no unfinished fsid change.
 * Return >0 for unfinished fsid change, and restore unfinished fsid/
 * chunk_tree_id into fsid_ret/chunk_id_ret.
 */
int check_unfinished_fsid_change(struct btrfs_fs_info *fs_info,
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

/*
 * Change fsid of a given fs.
 *
 * If new_fsid_str is not given, use a random generated UUID.
 * Caller should check new_fsid_str is valid
 */
int change_uuid(struct btrfs_fs_info *fs_info, const char *new_fsid_str)
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
	pr_verbose(LOG_DEFAULT, "Current fsid: %s\n", uuid_buf);

	uuid_unparse(new_fsid, uuid_buf);
	pr_verbose(LOG_DEFAULT, "New fsid: %s\n", uuid_buf);
	/* Now we can begin fsid change */
	pr_verbose(LOG_DEFAULT, "Set superblock flag CHANGING_FSID\n");
	ret = change_fsid_prepare(fs_info, new_fsid);
	if (ret < 0)
		goto out;

	/* Change extents first */
	pr_verbose(LOG_DEFAULT, "Change fsid in extent tree\n");
	ret = change_extent_tree_uuid(fs_info, new_fsid);
	if (ret < 0) {
		error("failed to change UUID of metadata: %d", ret);
		goto out;
	}

	/* Then devices */
	pr_verbose(LOG_DEFAULT, "Change fsid in chunk tree\n");
	ret = change_chunk_tree_uuid(fs_info->chunk_root, new_fsid);
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
	pr_verbose(LOG_DEFAULT, "Clear superblock flag CHANGING_FSID\n");
	ret = change_fsid_done(fs_info);
	fs_info->new_chunk_tree_uuid = NULL;
	pr_verbose(LOG_DEFAULT, "Fsid change finished\n");
out:
	return ret;
}

