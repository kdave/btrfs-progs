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

static int change_tree_csum(struct btrfs_trans_handle *trans, struct btrfs_root *root,
			    int csum_type)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct btrfs_path path;
	struct btrfs_key key = {0, 0, 0};
	int ret = 0;
	int level;

	btrfs_init_path(&path);
	/* No transaction, all in-place */
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;

	while (1) {
		level = 1;
		while (path.nodes[level]) {
			/* Caching can make double writes */
			if (!btrfs_header_flag(path.nodes[level], BTRFS_HEADER_FLAG_CSUM_NEW)) {
				ret = write_tree_block(NULL, fs_info, path.nodes[level]);
				if (ret < 0)
					goto out;
				btrfs_set_header_flag(path.nodes[level],
						BTRFS_HEADER_FLAG_CSUM_NEW);
			}
			level++;
		}
		ret = write_tree_block(NULL, fs_info, path.nodes[0]);
		if (ret < 0)
			goto out;
		ret = btrfs_next_leaf(root, &path);
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

static struct btrfs_csum_item *lookup_tmp_csum(struct btrfs_trans_handle *trans,
		  struct btrfs_path *path, u64 bytenr, int cow)
{
	int ret;
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *csum_root = fs_info->csum_tree_tmp;
	struct btrfs_key file_key;
	struct btrfs_key found_key;
	struct btrfs_csum_item *item;
	struct extent_buffer *leaf;
	u64 csum_offset = 0;
	u16 csum_type = fs_info->csum_type;
	u16 csum_size = fs_info->csum_size;
	int csums_in_item;

	file_key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	file_key.offset = bytenr;
	file_key.type = BTRFS_EXTENT_CSUM_KEY;
	ret = btrfs_search_slot(trans, csum_root, &file_key, path, 0, cow);
	if (ret < 0)
		goto fail;
	leaf = path->nodes[0];

	if (leaf->fs_info->force_csum_type != -1) {
		csum_type = fs_info->force_csum_type;
		csum_size = btrfs_csum_type_size(csum_type);
	}

	if (ret > 0) {
		ret = 1;
		if (path->slots[0] == 0)
			goto fail;
		path->slots[0]--;
		btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
		if (found_key.type != BTRFS_EXTENT_CSUM_KEY)
			goto fail;

		csum_offset = (bytenr - found_key.offset) / fs_info->sectorsize;
		csums_in_item = btrfs_item_size(leaf, path->slots[0]);
		csums_in_item /= csum_size;

		if (csum_offset >= csums_in_item) {
			ret = -EFBIG;
			goto fail;
		}
	}
	item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_csum_item);
	item = (struct btrfs_csum_item *)((unsigned char *)item +
					  csum_offset * csum_size);
	return item;
fail:
	if (ret > 0)
		ret = -ENOENT;
	return ERR_PTR(ret);
}

#define MAX_CSUM_ITEMS(r, size) ((((BTRFS_LEAF_DATA_SIZE(r->fs_info) - \
			       sizeof(struct btrfs_item) * 2) / \
			       size) - 1))

static int csum_file_block(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *fs_info,
			   u64 alloc_end, u64 bytenr, char *data, size_t len)
{
	struct btrfs_root *csum_root = fs_info->csum_tree_tmp;
	int ret = 0;
	struct btrfs_key file_key;
	struct btrfs_key found_key;
	u64 next_offset = (u64)-1;
	int found_next = 0;
	struct btrfs_path *path;
	struct btrfs_csum_item *item;
	struct extent_buffer *leaf = NULL;
	u64 csum_offset;
	u8 csum_result[BTRFS_CSUM_SIZE];
	u32 sectorsize = fs_info->sectorsize;
	u32 nritems;
	u32 ins_size;
	u16 csum_size;
	u16 csum_type;

	if (fs_info->force_csum_type != -1)
		return -EINVAL;

	csum_type = fs_info->force_csum_type;
	csum_size = btrfs_csum_type_size(csum_type);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	file_key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	file_key.type = BTRFS_EXTENT_CSUM_KEY;
	file_key.offset = bytenr;

	item = lookup_tmp_csum(trans, path, bytenr, 1);
	if (!IS_ERR(item)) {
		leaf = path->nodes[0];
		ret = 0;
		goto found;
	}
	ret = PTR_ERR(item);
	if (ret == -EFBIG) {
		u32 item_size;

		/* We found one, but it isn't big enough yet */
		leaf = path->nodes[0];
		item_size = btrfs_item_size(leaf, path->slots[0]);
		if ((item_size / csum_size) >= MAX_CSUM_ITEMS(csum_root, csum_size)) {
			/* Already at max size, make a new one */
			goto insert;
		}
	} else {
		int slot = path->slots[0] + 1;

		/* We didn't find a csum item, insert one */
		nritems = btrfs_header_nritems(path->nodes[0]);
		if (path->slots[0] >= nritems - 1) {
			ret = btrfs_next_leaf(csum_root, path);
			if (ret == 1)
				found_next = 1;
			if (ret != 0)
				goto insert;
			slot = 0;
		}
		btrfs_item_key_to_cpu(path->nodes[0], &found_key, slot);
		if (found_key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
		    found_key.type != BTRFS_EXTENT_CSUM_KEY) {
			found_next = 1;
			goto insert;
		}
		next_offset = found_key.offset;
		found_next = 1;
		goto insert;
	}

	/*
	 * At this point, we know the tree has an item, but it isn't big
	 * enough yet to put our csum in.  Grow it.
	 */
	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, csum_root, &file_key, path, csum_size, 1);
	if (ret < 0)
		goto fail;
	if (ret == 0)
		BUG();
	if (path->slots[0] == 0)
		goto insert;
	path->slots[0]--;
	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
	csum_offset = (file_key.offset - found_key.offset) / sectorsize;
	if (found_key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
	    found_key.type != BTRFS_EXTENT_CSUM_KEY ||
	    csum_offset >= MAX_CSUM_ITEMS(csum_root, csum_size)) {
		goto insert;
	}
	if (csum_offset >= btrfs_item_size(leaf, path->slots[0]) / csum_size) {
		u32 diff = (csum_offset + 1) * csum_size;

		diff = diff - btrfs_item_size(leaf, path->slots[0]);
		if (diff != csum_size)
			goto insert;
		ret = btrfs_extend_item(csum_root, path, diff);
		BUG_ON(ret);
		goto csum;
	}

insert:
	btrfs_release_path(path);
	csum_offset = 0;
	if (found_next) {
		u64 tmp = min(alloc_end, next_offset);
		tmp -= file_key.offset;
		tmp /= sectorsize;
		tmp = max((u64)1, tmp);
		tmp = min(tmp, (u64)MAX_CSUM_ITEMS(csum_root, csum_size));
		ins_size = csum_size * tmp;
	} else {
		ins_size = csum_size;
	}
	ret = btrfs_insert_empty_item(trans, csum_root, path, &file_key, ins_size);
	if (ret < 0)
		goto fail;
	if (ret != 0) {
		WARN_ON(1);
		goto fail;
	}
csum:
	leaf = path->nodes[0];
	item = btrfs_item_ptr(leaf, path->slots[0], struct btrfs_csum_item);
	ret = 0;
	item = (struct btrfs_csum_item *)((unsigned char *)item +
					  csum_offset * csum_size);
found:
	btrfs_csum_data(fs_info, csum_type, (u8 *)data, csum_result, len);
	write_extent_buffer(leaf, csum_result, (unsigned long)item, csum_size);
	btrfs_mark_buffer_dirty(path->nodes[0]);
fail:
	btrfs_free_path(path);
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
		ret = csum_file_block(trans, fs_info, start + len, start + offset,
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

int rewrite_checksums(struct btrfs_fs_info *fs_info, int csum_type)
{
	struct btrfs_root *root;
	struct btrfs_super_block *disk_super;
	struct btrfs_trans_handle *trans;
	struct btrfs_path path;
	struct btrfs_key key;
	u64 super_flags;
	int ret;

	disk_super = fs_info->super_copy;
	super_flags = btrfs_super_flags(disk_super);

	/* FIXME: Sanity checks */
	if (0) {
		error("UUID rewrite in progress, cannot change csum");
		return 1;
	}

	pr_verbose(LOG_DEFAULT, "Change csum from %s to %s\n",
			btrfs_super_csum_name(fs_info->csum_type),
			btrfs_super_csum_name(csum_type));

	fs_info->force_csum_type = csum_type;
	root = fs_info->tree_root;

	/* Step 1 sets the in progress flag, no other change to the sb */
	pr_verbose(LOG_DEFAULT, "Set superblock flag CHANGING_CSUM\n");
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	btrfs_init_path(&path);
	key.objectid = BTRFS_CSUM_TREE_TMP_OBJECTID;
	key.type = BTRFS_TEMPORARY_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

	if (ret == 1) {
		struct item {
			u64 offset;
			u64 generation;
			u16 csum_type;
			/*
			 * - generation when last synced
			 * - must recheck the whole tree anyway in case the fs
			 *   was mounted between and there are some extents missing
			 */
		} item[1];

		ret = btrfs_create_root(trans, fs_info, BTRFS_CSUM_TREE_TMP_OBJECTID);
		if (ret < 0) {
			return ret;
		} else {
			item->offset = btrfs_header_bytenr(fs_info->csum_tree_tmp->node);
			item->generation = btrfs_super_generation(fs_info->super_copy);
			item->csum_type = csum_type;
			ret = btrfs_insert_item(trans, fs_info->tree_root, &key, item,
						sizeof(*item));
			if (ret < 0)
				return ret;
		}
	} else {
		error("updating existing tmp csum root not implemented");
		exit(1);
	}

	super_flags |= BTRFS_SUPER_FLAG_CHANGING_CSUM;
	btrfs_set_super_flags(disk_super, super_flags);
	/* Change csum type here */
	btrfs_set_super_csum_type(disk_super, csum_type);
	ret = btrfs_commit_transaction(trans, root);
	if (ret < 0)
		return ret;
	btrfs_release_path(&path);

	struct {
		struct btrfs_root *root;
		const char *name;
		u64 objectid;
		bool p;
		bool g;
	} trees[] = {
		{ .p = true, .root = fs_info->tree_root, .name = "root tree" },
		{ .p = true, .root = fs_info->chunk_root, .name = "chunk tree" },
		{ .p = true, .root = fs_info->dev_root, .name = "dev tree" },
		{ .p = true, .root = fs_info->uuid_root, .name = "uuid tree" },
		{ .p = true, .root = fs_info->quota_root, .name = "quota tree" },
		{ .p = true, .root = fs_info->block_group_root, .name = "block group tree" },
		{ .g = true, .objectid = BTRFS_EXTENT_TREE_OBJECTID, .name = "extent tree" },
		{ .g = true, .objectid = BTRFS_CSUM_TREE_OBJECTID, .name = "csum tree" },
		{ .g = true, .objectid = BTRFS_FREE_SPACE_TREE_OBJECTID, .name = "free space tree" },
		{ .p = true, .root = fs_info->csum_tree_tmp, .name = "csum tmp tree" },
		{ .objectid = BTRFS_DATA_RELOC_TREE_OBJECTID, .name = "data reloc tree" },
		{ .objectid = BTRFS_FS_TREE_OBJECTID, .name = "fs tree" },
		/* TODO: iterate all fs trees */
		/* TODO: crashes if trees not present */
		/* { .objectid = BTRFS_TREE_LOG_OBJECTID, .name = "tree log tree" }, */
		/* { .objectid = BTRFS_TREE_RELOC_OBJECTID, .name = "tree reloc tree" }, */
		/* { .objectid = BTRFS_BLOCK_GROUP_TREE_OBJECTID, .name = "block group tree" }, */
	};

	for (int i = 0; i < ARRAY_SIZE(trees); i++) {
		pr_verbose(LOG_DEFAULT, "Change csum in %s\n", trees[i].name);
		if (trees[i].p) {
			root = trees[i].root;
			if (!root)
				continue;
		} else if (trees[i].g) {
			key.objectid = trees[i].objectid;
			key.type = BTRFS_ROOT_ITEM_KEY;
			key.offset = 0;
			root = btrfs_global_root(fs_info, &key);
			if (!root)
				continue;
		} else {
			key.objectid = trees[i].objectid;
			key.type = BTRFS_ROOT_ITEM_KEY;
			key.offset = (u64)-1;
			root = btrfs_read_fs_root_no_cache(fs_info, &key);
			if (!root)
				continue;
		}
		ret = change_tree_csum(trans, root, csum_type);
		if (ret < 0) {
			error("failed to change csum of %s: %d", trees[i].name, ret);
			goto out;
		}
	}

	/* DATA */
	pr_verbose(LOG_DEFAULT, "Change csum of data blocks\n");
	ret = fill_csum_tree_from_extent(fs_info);
	if (ret < 0)
		goto out;

	/* TODO: sync last status of old csum tree */
	/* TODO: delete old csum tree */

	/* Last, change csum in super */
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
