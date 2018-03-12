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

#include <time.h>
#include "ctree.h"
#include "internal.h"
#include "messages.h"
#include "transaction.h"
#include "utils.h"
#include "disk-io.h"
#include "repair.h"
#include "free-space-cache.h"
#include "free-space-tree.h"
#include "volumes.h"
#include "task-utils.h"
#include "check/mode-common.h"

/*
 * Clear log tree
 *
 * NOTE: log tree extent items is not handled here, fsck repair code should
 * remove these extent items.
 */
int zero_log_tree(struct btrfs_root *root)
{
	struct btrfs_trans_handle *trans;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		return ret;
	}
	btrfs_set_super_log_root(root->fs_info->super_copy, 0);
	btrfs_set_super_log_root_level(root->fs_info->super_copy, 0);
	ret = btrfs_commit_transaction(trans, root);
	return ret;
}

static int pin_down_tree_blocks(struct btrfs_fs_info *fs_info,
				struct extent_buffer *eb, int tree_root)
{
	struct extent_buffer *tmp;
	struct btrfs_root_item *ri;
	struct btrfs_key key;
	u64 bytenr;
	int level = btrfs_header_level(eb);
	int nritems;
	int ret;
	int i;

	/*
	 * If we have pinned this block before, don't pin it again.
	 * This can not only avoid forever loop with broken filesystem
	 * but also give us some speedups.
	 */
	if (test_range_bit(&fs_info->pinned_extents, eb->start,
			   eb->start + eb->len - 1, EXTENT_DIRTY, 0))
		return 0;

	btrfs_pin_extent(fs_info, eb->start, eb->len);

	nritems = btrfs_header_nritems(eb);
	for (i = 0; i < nritems; i++) {
		if (level == 0) {
			btrfs_item_key_to_cpu(eb, &key, i);
			if (key.type != BTRFS_ROOT_ITEM_KEY)
				continue;
			/* Skip the extent root and reloc roots */
			if (key.objectid == BTRFS_EXTENT_TREE_OBJECTID ||
			    key.objectid == BTRFS_TREE_RELOC_OBJECTID ||
			    key.objectid == BTRFS_DATA_RELOC_TREE_OBJECTID)
				continue;
			ri = btrfs_item_ptr(eb, i, struct btrfs_root_item);
			bytenr = btrfs_disk_root_bytenr(eb, ri);

			/*
			 * If at any point we start needing the real root we
			 * will have to build a stump root for the root we are
			 * in, but for now this doesn't actually use the root so
			 * just pass in extent_root.
			 */
			tmp = read_tree_block(fs_info, bytenr, 0);
			if (!extent_buffer_uptodate(tmp)) {
				fprintf(stderr, "Error reading root block\n");
				return -EIO;
			}
			ret = pin_down_tree_blocks(fs_info, tmp, 0);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		} else {
			bytenr = btrfs_node_blockptr(eb, i);

			/* If we aren't the tree root don't read the block */
			if (level == 1 && !tree_root) {
				btrfs_pin_extent(fs_info, bytenr,
						fs_info->nodesize);
				continue;
			}

			tmp = read_tree_block(fs_info, bytenr, 0);
			if (!extent_buffer_uptodate(tmp)) {
				fprintf(stderr, "Error reading tree block\n");
				return -EIO;
			}
			ret = pin_down_tree_blocks(fs_info, tmp, tree_root);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int pin_metadata_blocks(struct btrfs_fs_info *fs_info)
{
	int ret;

	ret = pin_down_tree_blocks(fs_info, fs_info->chunk_root->node, 0);
	if (ret)
		return ret;

	return pin_down_tree_blocks(fs_info, fs_info->tree_root->node, 1);
}

static int reset_block_groups(struct btrfs_fs_info *fs_info)
{
	struct btrfs_block_group_cache *cache;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_chunk *chunk;
	struct btrfs_key key;
	int ret;
	u64 start;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, fs_info->chunk_root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}

	/*
	 * We do this in case the block groups were screwed up and had alloc
	 * bits that aren't actually set on the chunks.  This happens with
	 * restored images every time and could happen in real life I guess.
	 */
	fs_info->avail_data_alloc_bits = 0;
	fs_info->avail_metadata_alloc_bits = 0;
	fs_info->avail_system_alloc_bits = 0;

	/* First we need to create the in-memory block groups */
	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(fs_info->chunk_root, &path);
			if (ret < 0) {
				btrfs_release_path(&path);
				return ret;
			}
			if (ret) {
				ret = 0;
				break;
			}
		}
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_CHUNK_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}

		chunk = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_chunk);
		btrfs_add_block_group(fs_info, 0,
				      btrfs_chunk_type(leaf, chunk), key.offset,
				      btrfs_chunk_length(leaf, chunk));
		set_extent_dirty(&fs_info->free_space_cache, key.offset,
				 key.offset + btrfs_chunk_length(leaf, chunk));
		path.slots[0]++;
	}
	start = 0;
	while (1) {
		cache = btrfs_lookup_first_block_group(fs_info, start);
		if (!cache)
			break;
		cache->cached = 1;
		start = cache->key.objectid + cache->key.offset;
	}

	btrfs_release_path(&path);
	return 0;
}

static void record_root_in_trans(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root)
{
	if (root->last_trans != trans->transid) {
		root->track_dirty = 1;
		root->last_trans = trans->transid;
		root->commit_root = root->node;
		extent_buffer_get(root->node);
	}
}

static int reset_balance(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *root = fs_info->tree_root;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int del_slot, del_nr = 0;
	int ret;
	int found = 0;

	btrfs_init_path(&path);
	key.objectid = BTRFS_BALANCE_OBJECTID;
	key.type = BTRFS_BALANCE_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
	if (ret) {
		if (ret > 0)
			ret = 0;
		if (!ret)
			goto reinit_data_reloc;
		else
			goto out;
	}

	ret = btrfs_del_item(trans, root, &path);
	if (ret)
		goto out;
	btrfs_release_path(&path);

	key.objectid = BTRFS_TREE_RELOC_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
	if (ret < 0)
		goto out;
	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			if (!found)
				break;

			if (del_nr) {
				ret = btrfs_del_items(trans, root, &path,
						      del_slot, del_nr);
				del_nr = 0;
				if (ret)
					goto out;
			}
			key.offset++;
			btrfs_release_path(&path);

			found = 0;
			ret = btrfs_search_slot(trans, root, &key, &path,
						-1, 1);
			if (ret < 0)
				goto out;
			continue;
		}
		found = 1;
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid > BTRFS_TREE_RELOC_OBJECTID)
			break;
		if (key.objectid != BTRFS_TREE_RELOC_OBJECTID) {
			path.slots[0]++;
			continue;
		}
		if (!del_nr) {
			del_slot = path.slots[0];
			del_nr = 1;
		} else {
			del_nr++;
		}
		path.slots[0]++;
	}

	if (del_nr) {
		ret = btrfs_del_items(trans, root, &path, del_slot, del_nr);
		if (ret)
			goto out;
	}
	btrfs_release_path(&path);

reinit_data_reloc:
	key.objectid = BTRFS_DATA_RELOC_TREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root)) {
		fprintf(stderr, "Error reading data reloc tree\n");
		ret = PTR_ERR(root);
		goto out;
	}
	record_root_in_trans(trans, root);
	ret = btrfs_fsck_reinit_root(trans, root, 0);
	if (ret)
		goto out;
	ret = btrfs_make_root_dir(trans, root, BTRFS_FIRST_FREE_OBJECTID);
out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Using fs and other trees to rebuild extent tree.
 */
int reinit_extent_tree(struct btrfs_trans_handle *trans,
		       struct btrfs_fs_info *fs_info)
{
	u64 start = 0;
	int ret;

	/*
	 * The only reason we don't do this is because right now we're just
	 * walking the trees we find and pinning down their bytes, we don't look
	 * at any of the leaves.  In order to do mixed groups we'd have to check
	 * the leaves of any fs roots and pin down the bytes for any file
	 * extents we find.  Not hard but why do it if we don't have to?
	 */
	if (btrfs_fs_incompat(fs_info, MIXED_GROUPS)) {
		fprintf(stderr, "We don't support re-initing the extent tree "
			"for mixed block groups yet, please notify a btrfs "
			"developer you want to do this so they can add this "
			"functionality.\n");
		return -EINVAL;
	}

	/*
	 * first we need to walk all of the trees except the extent tree and pin
	 * down the bytes that are in use so we don't overwrite any existing
	 * metadata.
	 */
	ret = pin_metadata_blocks(fs_info);
	if (ret) {
		fprintf(stderr, "error pinning down used bytes\n");
		return ret;
	}

	/*
	 * Need to drop all the block groups since we're going to recreate all
	 * of them again.
	 */
	btrfs_free_block_groups(fs_info);
	ret = reset_block_groups(fs_info);
	if (ret) {
		fprintf(stderr, "error resetting the block groups\n");
		return ret;
	}

	/* Ok we can allocate now, reinit the extent root */
	ret = btrfs_fsck_reinit_root(trans, fs_info->extent_root, 0);
	if (ret) {
		fprintf(stderr, "extent root initialization failed\n");
		/*
		 * When the transaction code is updated we should end the
		 * transaction, but for now progs only knows about commit so
		 * just return an error.
		 */
		return ret;
	}

	/*
	 * Now we have all the in-memory block groups setup so we can make
	 * allocations properly, and the metadata we care about is safe since we
	 * pinned all of it above.
	 */
	while (1) {
		struct btrfs_block_group_cache *cache;

		cache = btrfs_lookup_first_block_group(fs_info, start);
		if (!cache)
			break;
		start = cache->key.objectid + cache->key.offset;
		ret = btrfs_insert_item(trans, fs_info->extent_root,
					&cache->key, &cache->item,
					sizeof(cache->item));
		if (ret) {
			fprintf(stderr, "Error adding block group\n");
			return ret;
		}
		btrfs_extent_post_op(trans, fs_info->extent_root);
	}

	ret = reset_balance(trans, fs_info);
	if (ret)
		fprintf(stderr, "error resetting the pending balance\n");

	return ret;
}

/*
 * Re-init one tree.
 *
 * NOTE: extent items for this tree is not handled here, fsck repair code should
 * remove these extent items.
 */
int btrfs_fsck_reinit_root(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root, int overwrite)
{
	struct extent_buffer *c;
	struct extent_buffer *old = root->node;
	int level;
	int ret;
	struct btrfs_disk_key disk_key = {0,0,0};

	level = 0;

	if (overwrite) {
		c = old;
		extent_buffer_get(c);
		goto init;
	}
	c = btrfs_alloc_free_block(trans, root,
				   root->fs_info->nodesize,
				   root->root_key.objectid,
				   &disk_key, level, 0, 0);
	if (IS_ERR(c)) {
		c = old;
		extent_buffer_get(c);
		overwrite = 1;
	}
init:
	memset_extent_buffer(c, 0, 0, sizeof(struct btrfs_header));
	btrfs_set_header_level(c, level);
	btrfs_set_header_bytenr(c, c->start);
	btrfs_set_header_generation(c, trans->transid);
	btrfs_set_header_backref_rev(c, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(c, root->root_key.objectid);

	write_extent_buffer(c, root->fs_info->fsid,
			    btrfs_header_fsid(), BTRFS_FSID_SIZE);

	write_extent_buffer(c, root->fs_info->chunk_tree_uuid,
			    btrfs_header_chunk_tree_uuid(c),
			    BTRFS_UUID_SIZE);

	btrfs_mark_buffer_dirty(c);
	/*
	 * this case can happen in the following case:
	 *
	 * 1.overwrite previous root.
	 *
	 * 2.reinit reloc data root, this is because we skip pin
	 * down reloc data tree before which means we can allocate
	 * same block bytenr here.
	 */
	if (old->start == c->start) {
		btrfs_set_root_generation(&root->root_item,
					  trans->transid);
		root->root_item.level = btrfs_header_level(root->node);
		ret = btrfs_update_root(trans, root->fs_info->tree_root,
					&root->root_key, &root->root_item);
		if (ret) {
			free_extent_buffer(c);
			return ret;
		}
	}
	free_extent_buffer(old);
	root->node = c;
	add_root_to_dirty_list(root);
	return 0;
}

static int populate_csum(struct btrfs_trans_handle *trans,
			 struct btrfs_root *csum_root, char *buf, u64 start,
			 u64 len)
{
	struct btrfs_fs_info *fs_info = csum_root->fs_info;
	u64 offset = 0;
	u64 sectorsize;
	int ret = 0;

	while (offset < len) {
		sectorsize = fs_info->sectorsize;
		ret = read_extent_data(fs_info, buf, start + offset,
				       &sectorsize, 0);
		if (ret)
			break;
		ret = btrfs_csum_file_block(trans, csum_root, start + len,
					    start + offset, buf, sectorsize);
		if (ret)
			break;
		offset += sectorsize;
	}
	return ret;
}

static int fill_csum_tree_from_one_fs_root(struct btrfs_trans_handle *trans,
				      struct btrfs_root *csum_root,
				      struct btrfs_root *cur_root)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *node;
	struct btrfs_file_extent_item *fi;
	char *buf = NULL;
	u64 start = 0;
	u64 len = 0;
	int slot = 0;
	int ret = 0;

	buf = malloc(cur_root->fs_info->sectorsize);
	if (!buf)
		return -ENOMEM;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.offset = 0;
	key.type = 0;
	ret = btrfs_search_slot(NULL, cur_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	/* Iterate all regular file extents and fill its csum */
	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

		if (key.type != BTRFS_EXTENT_DATA_KEY)
			goto next;
		node = path.nodes[0];
		slot = path.slots[0];
		fi = btrfs_item_ptr(node, slot, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(node, fi) != BTRFS_FILE_EXTENT_REG)
			goto next;
		start = btrfs_file_extent_disk_bytenr(node, fi);
		len = btrfs_file_extent_disk_num_bytes(node, fi);

		ret = populate_csum(trans, csum_root, buf, start, len);
		if (ret == -EEXIST)
			ret = 0;
		if (ret < 0)
			goto out;
next:
		/*
		 * TODO: if next leaf is corrupted, jump to nearest next valid
		 * leaf.
		 */
		ret = btrfs_next_item(cur_root, &path);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = 0;
			goto out;
		}
	}

out:
	btrfs_release_path(&path);
	free(buf);
	return ret;
}

static int fill_csum_tree_from_fs(struct btrfs_trans_handle *trans,
				  struct btrfs_root *csum_root)
{
	struct btrfs_fs_info *fs_info = csum_root->fs_info;
	struct btrfs_path path;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *cur_root;
	struct extent_buffer *node;
	struct btrfs_key key;
	int slot = 0;
	int ret = 0;

	btrfs_init_path(&path);
	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.offset = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	while (1) {
		node = path.nodes[0];
		slot = path.slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);
		if (key.objectid > BTRFS_LAST_FREE_OBJECTID)
			goto out;
		if (key.type != BTRFS_ROOT_ITEM_KEY)
			goto next;
		if (!is_fstree(key.objectid))
			goto next;
		key.offset = (u64)-1;

		cur_root = btrfs_read_fs_root(fs_info, &key);
		if (IS_ERR(cur_root) || !cur_root) {
			fprintf(stderr, "Fail to read fs/subvol tree: %lld\n",
				key.objectid);
			goto out;
		}
		ret = fill_csum_tree_from_one_fs_root(trans, csum_root,
				cur_root);
		if (ret < 0)
			goto out;
next:
		ret = btrfs_next_item(tree_root, &path);
		if (ret > 0) {
			ret = 0;
			goto out;
		}
		if (ret < 0)
			goto out;
	}

out:
	btrfs_release_path(&path);
	return ret;
}

static int fill_csum_tree_from_extent(struct btrfs_trans_handle *trans,
				      struct btrfs_root *csum_root)
{
	struct btrfs_root *extent_root = csum_root->fs_info->extent_root;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	struct extent_buffer *leaf;
	char *buf;
	struct btrfs_key key;
	int ret;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}

	buf = malloc(csum_root->fs_info->sectorsize);
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

		ei = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_extent_item);
		if (!(btrfs_extent_flags(leaf, ei) &
		      BTRFS_EXTENT_FLAG_DATA)) {
			path.slots[0]++;
			continue;
		}

		ret = populate_csum(trans, csum_root, buf, key.objectid,
				    key.offset);
		if (ret)
			break;
		path.slots[0]++;
	}

	btrfs_release_path(&path);
	free(buf);
	return ret;
}

/*
 * Recalculate the csum and put it into the csum tree.
 *
 * Extent tree init will wipe out all the extent info, so in that case, we
 * can't depend on extent tree, but use fs tree.  If search_fs_tree is set, we
 * will use fs/subvol trees to init the csum tree.
 */
int fill_csum_tree(struct btrfs_trans_handle *trans,
		   struct btrfs_root *csum_root, int search_fs_tree)
{
	if (search_fs_tree)
		return fill_csum_tree_from_fs(trans, csum_root);
	else
		return fill_csum_tree_from_extent(trans, csum_root);
}

static void free_roots_info_cache(void)
{
	if (!roots_info_cache)
		return;

	while (!cache_tree_empty(roots_info_cache)) {
		struct cache_extent *entry;
		struct root_item_info *rii;

		entry = first_cache_extent(roots_info_cache);
		if (!entry)
			break;
		remove_cache_extent(roots_info_cache, entry);
		rii = container_of(entry, struct root_item_info, cache_extent);
		free(rii);
	}

	free(roots_info_cache);
	roots_info_cache = NULL;
}

static int build_roots_info_cache(struct btrfs_fs_info *info)
{
	int ret = 0;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_path path;

	if (!roots_info_cache) {
		roots_info_cache = malloc(sizeof(*roots_info_cache));
		if (!roots_info_cache)
			return -ENOMEM;
		cache_tree_init(roots_info_cache);
	}

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, info->extent_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	leaf = path.nodes[0];

	while (1) {
		struct btrfs_key found_key;
		struct btrfs_extent_item *ei;
		struct btrfs_extent_inline_ref *iref;
		unsigned long item_end;
		int slot = path.slots[0];
		int type;
		u64 flags;
		u64 root_id;
		u8 level;
		struct cache_extent *entry;
		struct root_item_info *rii;

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(info->extent_root, &path);
			if (ret < 0) {
				break;
			} else if (ret) {
				ret = 0;
				break;
			}
			leaf = path.nodes[0];
			slot = path.slots[0];
		}

		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);

		if (found_key.type != BTRFS_EXTENT_ITEM_KEY &&
		    found_key.type != BTRFS_METADATA_ITEM_KEY)
			goto next;

		ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
		flags = btrfs_extent_flags(leaf, ei);
		item_end = (unsigned long)ei + btrfs_item_size_nr(leaf, slot);

		if (found_key.type == BTRFS_EXTENT_ITEM_KEY &&
		    !(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK))
			goto next;

		if (found_key.type == BTRFS_METADATA_ITEM_KEY) {
			iref = (struct btrfs_extent_inline_ref *)(ei + 1);
			level = found_key.offset;
		} else {
			struct btrfs_tree_block_info *binfo;

			binfo = (struct btrfs_tree_block_info *)(ei + 1);
			iref = (struct btrfs_extent_inline_ref *)(binfo + 1);
			level = btrfs_tree_block_level(leaf, binfo);
		}

		/*
		 * It's a valid extent/metadata item that has no inline ref,
		 * but SHARED_BLOCK_REF or other shared references.
		 * So we need to do extra check to avoid reading beyond leaf
		 * boudnary.
		 */
		if ((unsigned long)iref >= item_end)
			goto next;

		/*
		 * For a root extent, it must be of the following type and the
		 * first (and only one) iref in the item.
		 */
		type = btrfs_extent_inline_ref_type(leaf, iref);
		if (type != BTRFS_TREE_BLOCK_REF_KEY)
			goto next;

		root_id = btrfs_extent_inline_ref_offset(leaf, iref);
		entry = lookup_cache_extent(roots_info_cache, root_id, 1);
		if (!entry) {
			rii = malloc(sizeof(struct root_item_info));
			if (!rii) {
				ret = -ENOMEM;
				goto out;
			}
			rii->cache_extent.start = root_id;
			rii->cache_extent.size = 1;
			rii->level = (u8)-1;
			entry = &rii->cache_extent;
			ret = insert_cache_extent(roots_info_cache, entry);
			ASSERT(ret == 0);
		} else {
			rii = container_of(entry, struct root_item_info,
					   cache_extent);
		}

		ASSERT(rii->cache_extent.start == root_id);
		ASSERT(rii->cache_extent.size == 1);

		if (level > rii->level || rii->level == (u8)-1) {
			rii->level = level;
			rii->bytenr = found_key.objectid;
			rii->gen = btrfs_extent_generation(leaf, ei);
			rii->node_count = 1;
		} else if (level == rii->level) {
			rii->node_count++;
		}
next:
		path.slots[0]++;
	}

out:
	btrfs_release_path(&path);

	return ret;
}

static int maybe_repair_root_item(struct btrfs_path *path,
				  const struct btrfs_key *root_key,
				  const int read_only_mode)
{
	const u64 root_id = root_key->objectid;
	struct cache_extent *entry;
	struct root_item_info *rii;
	struct btrfs_root_item ri;
	unsigned long offset;

	entry = lookup_cache_extent(roots_info_cache, root_id, 1);
	if (!entry) {
		fprintf(stderr,
			"Error: could not find extent items for root %llu\n",
			root_key->objectid);
		return -ENOENT;
	}

	rii = container_of(entry, struct root_item_info, cache_extent);
	ASSERT(rii->cache_extent.start == root_id);
	ASSERT(rii->cache_extent.size == 1);

	if (rii->node_count != 1) {
		fprintf(stderr,
			"Error: could not find btree root extent for root %llu\n",
			root_id);
		return -ENOENT;
	}

	offset = btrfs_item_ptr_offset(path->nodes[0], path->slots[0]);
	read_extent_buffer(path->nodes[0], &ri, offset, sizeof(ri));

	if (btrfs_root_bytenr(&ri) != rii->bytenr ||
	    btrfs_root_level(&ri) != rii->level ||
	    btrfs_root_generation(&ri) != rii->gen) {

		/*
		 * If we're in repair mode but our caller told us to not update
		 * the root item, i.e. just check if it needs to be updated, don't
		 * print this message, since the caller will call us again shortly
		 * for the same root item without read only mode (the caller will
		 * open a transaction first).
		 */
		if (!(read_only_mode && repair))
			fprintf(stderr,
				"%sroot item for root %llu,"
				" current bytenr %llu, current gen %llu, current level %u,"
				" new bytenr %llu, new gen %llu, new level %u\n",
				(read_only_mode ? "" : "fixing "),
				root_id,
				btrfs_root_bytenr(&ri), btrfs_root_generation(&ri),
				btrfs_root_level(&ri),
				rii->bytenr, rii->gen, rii->level);

		if (btrfs_root_generation(&ri) > rii->gen) {
			fprintf(stderr,
				"root %llu has a root item with a more recent gen (%llu) compared to the found root node (%llu)\n",
				root_id, btrfs_root_generation(&ri), rii->gen);
			return -EINVAL;
		}

		if (!read_only_mode) {
			btrfs_set_root_bytenr(&ri, rii->bytenr);
			btrfs_set_root_level(&ri, rii->level);
			btrfs_set_root_generation(&ri, rii->gen);
			write_extent_buffer(path->nodes[0], &ri,
					    offset, sizeof(ri));
		}

		return 1;
	}

	return 0;
}

static int clear_free_space_cache(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_block_group_cache *bg_cache;
	u64 current = 0;
	int ret = 0;

	/* Clear all free space cache inodes and its extent data */
	while (1) {
		bg_cache = btrfs_lookup_first_block_group(fs_info, current);
		if (!bg_cache)
			break;
		ret = btrfs_clear_free_space_cache(fs_info, bg_cache);
		if (ret < 0)
			return ret;
		current = bg_cache->key.objectid + bg_cache->key.offset;
	}

	/* Don't forget to set cache_generation to -1 */
	trans = btrfs_start_transaction(fs_info->tree_root, 0);
	if (IS_ERR(trans)) {
		error("failed to update super block cache generation");
		return PTR_ERR(trans);
	}
	btrfs_set_super_cache_generation(fs_info->super_copy, (u64)-1);
	btrfs_commit_transaction(trans, fs_info->tree_root);

	return ret;
}

/*
 * Wrapper to clear free space cache or free space tree.
 */
int do_clear_free_space_cache(struct btrfs_fs_info *fs_info,
		int clear_version)
{
	int ret = 0;

	if (clear_version == 1) {
		if (btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE)) {
			error(
		"free space cache v2 detected, use --clear-space-cache v2");
			ret = 1;
			goto close_out;
		}
		printf("Clearing free space cache\n");
		ret = clear_free_space_cache(fs_info);
		if (ret) {
			error("failed to clear free space cache");
			ret = 1;
		} else {
			printf("Free space cache cleared\n");
		}
	} else if (clear_version == 2) {
		if (!btrfs_fs_compat_ro(fs_info, FREE_SPACE_TREE)) {
			printf("no free space cache v2 to clear\n");
			ret = 0;
			goto close_out;
		}
		printf("Clear free space cache v2\n");
		ret = btrfs_clear_free_space_tree(fs_info);
		if (ret) {
			error("failed to clear free space cache v2: %d", ret);
			ret = 1;
		} else {
			printf("free space cache v2 cleared\n");
		}
	}
close_out:
	return ret;
}

/*
 * Unlike device size alignment check above, some super total_bytes check
 * failure can lead to mount failure for newer kernel.
 *
 * So this function will return the error for a fatal super total_bytes problem.
 */
bool is_super_size_valid(struct btrfs_fs_info *fs_info)
{
	struct btrfs_device *dev;
	struct list_head *dev_list = &fs_info->fs_devices->devices;
	u64 total_bytes = 0;
	u64 super_bytes = btrfs_super_total_bytes(fs_info->super_copy);

	list_for_each_entry(dev, dev_list, dev_list)
		total_bytes += dev->total_bytes;

	/* Important check, which can cause unmountable fs */
	if (super_bytes < total_bytes) {
		error("super total bytes %llu smaller than real device(s) size %llu",
			super_bytes, total_bytes);
		error("mounting this fs may fail for newer kernels");
		error("this can be fixed by 'btrfs rescue fix-device-size'");
		return false;
	}

	/*
	 * Optional check, just to make everything aligned and match with each
	 * other.
	 *
	 * For a btrfs-image restored fs, we don't need to check it anyway.
	 */
	if (btrfs_super_flags(fs_info->super_copy) &
	    (BTRFS_SUPER_FLAG_METADUMP | BTRFS_SUPER_FLAG_METADUMP_V2))
		return true;
	if (!IS_ALIGNED(super_bytes, fs_info->sectorsize) ||
	    !IS_ALIGNED(total_bytes, fs_info->sectorsize) ||
	    super_bytes != total_bytes) {
		warning("minor unaligned/mismatch device size detected");
		warning(
		"recommended to use 'btrfs rescue fix-device-size' to fix it");
	}
	return true;
}

/*
 * A regression introduced in the 3.17 kernel (more specifically in 3.17-rc2),
 * caused read-only snapshots to be corrupted if they were created at a moment
 * when the source subvolume/snapshot had orphan items. The issue was that the
 * on-disk root items became incorrect, referring to the pre orphan cleanup root
 * node instead of the post orphan cleanup root node.
 * So this function, and its callees, just detects and fixes those cases. Even
 * though the regression was for read-only snapshots, this function applies to
 * any snapshot/subvolume root.
 * This must be run before any other repair code - not doing it so, makes other
 * repair code delete or modify backrefs in the extent tree for example, which
 * will result in an inconsistent fs after repairing the root items.
 */
int repair_root_items(struct btrfs_fs_info *info)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_trans_handle *trans = NULL;
	int ret = 0;
	int bad_roots = 0;
	int need_trans = 0;

	btrfs_init_path(&path);

	ret = build_roots_info_cache(info);
	if (ret)
		goto out;

	key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = 0;

again:
	/*
	 * Avoid opening and committing transactions if a leaf doesn't have
	 * any root items that need to be fixed, so that we avoid rotating
	 * backup roots unnecessarily.
	 */
	if (need_trans) {
		trans = btrfs_start_transaction(info->tree_root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			goto out;
		}
	}

	ret = btrfs_search_slot(trans, info->tree_root, &key, &path,
				0, trans ? 1 : 0);
	if (ret < 0)
		goto out;
	leaf = path.nodes[0];

	while (1) {
		struct btrfs_key found_key;

		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			int no_more_keys = find_next_key(&path, &key);

			btrfs_release_path(&path);
			if (trans) {
				ret = btrfs_commit_transaction(trans,
							       info->tree_root);
				trans = NULL;
				if (ret < 0)
					goto out;
			}
			need_trans = 0;
			if (no_more_keys)
				break;
			goto again;
		}

		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);

		if (found_key.type != BTRFS_ROOT_ITEM_KEY)
			goto next;
		if (found_key.objectid == BTRFS_TREE_RELOC_OBJECTID)
			goto next;

		ret = maybe_repair_root_item(&path, &found_key, trans ? 0 : 1);
		if (ret < 0)
			goto out;
		if (ret) {
			if (!trans && repair) {
				need_trans = 1;
				key = found_key;
				btrfs_release_path(&path);
				goto again;
			}
			bad_roots++;
		}
next:
		path.slots[0]++;
	}
	ret = 0;
out:
	free_roots_info_cache();
	btrfs_release_path(&path);
	if (trans)
		btrfs_commit_transaction(trans, info->tree_root);
	if (ret < 0)
		return ret;

	return bad_roots;
}

static int check_cache_range(struct btrfs_root *root,
			     struct btrfs_block_group_cache *cache,
			     u64 offset, u64 bytes)
{
	struct btrfs_free_space *entry;
	u64 *logical;
	u64 bytenr;
	int stripe_len;
	int i, nr, ret;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = btrfs_rmap_block(root->fs_info,
				       cache->key.objectid, bytenr, 0,
				       &logical, &nr, &stripe_len);
		if (ret)
			return ret;

		while (nr--) {
			if (logical[nr] + stripe_len <= offset)
				continue;
			if (offset + bytes <= logical[nr])
				continue;
			if (logical[nr] == offset) {
				if (stripe_len >= bytes) {
					free(logical);
					return 0;
				}
				bytes -= stripe_len;
				offset += stripe_len;
			} else if (logical[nr] < offset) {
				if (logical[nr] + stripe_len >=
				    offset + bytes) {
					free(logical);
					return 0;
				}
				bytes = (offset + bytes) -
					(logical[nr] + stripe_len);
				offset = logical[nr] + stripe_len;
			} else {
				/*
				 * Could be tricky, the super may land in the
				 * middle of the area we're checking.  First
				 * check the easiest case, it's at the end.
				 */
				if (logical[nr] + stripe_len >=
				    bytes + offset) {
					bytes = logical[nr] - offset;
					continue;
				}

				/* Check the left side */
				ret = check_cache_range(root, cache,
							offset,
							logical[nr] - offset);
				if (ret) {
					free(logical);
					return ret;
				}

				/* Now we continue with the right side */
				bytes = (offset + bytes) -
					(logical[nr] + stripe_len);
				offset = logical[nr] + stripe_len;
			}
		}

		free(logical);
	}

	entry = btrfs_find_free_space(cache->free_space_ctl, offset, bytes);
	if (!entry) {
		fprintf(stderr, "there is no free space entry for %llu-%llu\n",
			offset, offset+bytes);
		return -EINVAL;
	}

	if (entry->offset != offset) {
		fprintf(stderr, "wanted offset %llu, found %llu\n", offset,
			entry->offset);
		return -EINVAL;
	}

	if (entry->bytes != bytes) {
		fprintf(stderr, "wanted bytes %llu, found %llu for off %llu\n",
			bytes, entry->bytes, offset);
		return -EINVAL;
	}

	unlink_free_space(cache->free_space_ctl, entry);
	free(entry);
	return 0;
}

static int verify_space_cache(struct btrfs_root *root,
			      struct btrfs_block_group_cache *cache)
{
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 last;
	int ret = 0;

	root = root->fs_info->extent_root;

	last = max_t(u64, cache->key.objectid, BTRFS_SUPER_INFO_OFFSET);

	btrfs_init_path(&path);
	key.objectid = last;
	key.offset = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	ret = 0;
	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0)
				goto out;
			if (ret > 0) {
				ret = 0;
				break;
			}
		}
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid >= cache->key.offset + cache->key.objectid)
			break;
		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}

		if (last == key.objectid) {
			if (key.type == BTRFS_EXTENT_ITEM_KEY)
				last = key.objectid + key.offset;
			else
				last = key.objectid + root->fs_info->nodesize;
			path.slots[0]++;
			continue;
		}

		ret = check_cache_range(root, cache, last,
					key.objectid - last);
		if (ret)
			break;
		if (key.type == BTRFS_EXTENT_ITEM_KEY)
			last = key.objectid + key.offset;
		else
			last = key.objectid + root->fs_info->nodesize;
		path.slots[0]++;
	}

	if (last < cache->key.objectid + cache->key.offset)
		ret = check_cache_range(root, cache, last,
					cache->key.objectid +
					cache->key.offset - last);

out:
	btrfs_release_path(&path);

	if (!ret &&
	    !RB_EMPTY_ROOT(&cache->free_space_ctl->free_space_offset)) {
		fprintf(stderr, "There are still entries left in the space "
			"cache\n");
		ret = -EINVAL;
	}

	return ret;
}

/*
 * Check if space cache is valid.
 *
 * Handles both free space cache and free space tree
 */
int check_space_cache(struct btrfs_root *root)
{
	struct btrfs_block_group_cache *cache;
	u64 start = BTRFS_SUPER_INFO_OFFSET + BTRFS_SUPER_INFO_SIZE;
	int ret;
	int error = 0;

	if (btrfs_super_cache_generation(root->fs_info->super_copy) != -1ULL &&
	    btrfs_super_generation(root->fs_info->super_copy) !=
	    btrfs_super_cache_generation(root->fs_info->super_copy)) {
		printf("cache and super generation don't match, space cache "
		       "will be invalidated\n");
		return 0;
	}

	if (ctx.progress_enabled) {
		ctx.tp = TASK_FREE_SPACE;
		task_start(ctx.info);
	}

	while (1) {
		cache = btrfs_lookup_first_block_group(root->fs_info, start);
		if (!cache)
			break;

		start = cache->key.objectid + cache->key.offset;
		if (!cache->free_space_ctl) {
			if (btrfs_init_free_space_ctl(cache,
						root->fs_info->sectorsize)) {
				ret = -ENOMEM;
				break;
			}
		} else {
			btrfs_remove_free_space_cache(cache);
		}

		if (btrfs_fs_compat_ro(root->fs_info, FREE_SPACE_TREE)) {
			ret = exclude_super_stripes(root, cache);
			if (ret) {
				fprintf(stderr, "could not exclude super stripes: %s\n",
					strerror(-ret));
				error++;
				continue;
			}
			ret = load_free_space_tree(root->fs_info, cache);
			free_excluded_extents(root, cache);
			if (ret < 0) {
				fprintf(stderr, "could not load free space tree: %s\n",
					strerror(-ret));
				error++;
				continue;
			}
			error += ret;
		} else {
			ret = load_free_space_cache(root->fs_info, cache);
			if (ret < 0)
				error++;
			if (ret <= 0)
				continue;
		}

		ret = verify_space_cache(root, cache);
		if (ret) {
			fprintf(stderr, "cache appears valid but isn't %llu\n",
				cache->key.objectid);
			error++;
		}
	}

	task_stop(ctx.info);

	return error ? -EINVAL : 0;
}

static int check_extent_csums(struct btrfs_root *root, u64 bytenr,
			u64 num_bytes, unsigned long leaf_offset,
			struct extent_buffer *eb)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 offset = 0;
	u16 csum_size = btrfs_super_csum_size(fs_info->super_copy);
	char *data;
	unsigned long csum_offset;
	u32 csum;
	u32 csum_expected;
	u64 read_len;
	u64 data_checked = 0;
	u64 tmp;
	int ret = 0;
	int mirror;
	int num_copies;

	if (num_bytes % fs_info->sectorsize)
		return -EINVAL;

	data = malloc(num_bytes);
	if (!data)
		return -ENOMEM;

	while (offset < num_bytes) {
		mirror = 0;
again:
		read_len = num_bytes - offset;
		/* read as much space once a time */
		ret = read_extent_data(fs_info, data + offset,
				bytenr + offset, &read_len, mirror);
		if (ret)
			goto out;
		data_checked = 0;
		/* verify every 4k data's checksum */
		while (data_checked < read_len) {
			csum = ~(u32)0;
			tmp = offset + data_checked;

			csum = btrfs_csum_data((char *)data + tmp,
					       csum, fs_info->sectorsize);
			btrfs_csum_final(csum, (u8 *)&csum);

			csum_offset = leaf_offset +
				 tmp / fs_info->sectorsize * csum_size;
			read_extent_buffer(eb, (char *)&csum_expected,
					   csum_offset, csum_size);
			/* try another mirror */
			if (csum != csum_expected) {
				fprintf(stderr, "mirror %d bytenr %llu csum %u expected csum %u\n",
						mirror, bytenr + tmp,
						csum, csum_expected);
				num_copies = btrfs_num_copies(root->fs_info,
						bytenr, num_bytes);
				if (mirror < num_copies - 1) {
					mirror += 1;
					goto again;
				}
			}
			data_checked += fs_info->sectorsize;
		}
		offset += read_len;
	}
out:
	free(data);
	return ret;
}

static int check_extent_exists(struct btrfs_root *root, u64 bytenr,
			       u64 num_bytes)
{
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int ret;

	btrfs_init_path(&path);
	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

again:
	ret = btrfs_search_slot(NULL, root->fs_info->extent_root, &key, &path,
				0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error looking up extent record %d\n", ret);
		btrfs_release_path(&path);
		return ret;
	} else if (ret) {
		if (path.slots[0] > 0) {
			path.slots[0]--;
		} else {
			ret = btrfs_prev_leaf(root, &path);
			if (ret < 0) {
				goto out;
			} else if (ret > 0) {
				ret = 0;
				goto out;
			}
		}
	}

	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

	/*
	 * Block group items come before extent items if they have the same
	 * bytenr, so walk back one more just in case.  Dear future traveller,
	 * first congrats on mastering time travel.  Now if it's not too much
	 * trouble could you go back to 2006 and tell Chris to make the
	 * BLOCK_GROUP_ITEM_KEY (and BTRFS_*_REF_KEY) lower than the
	 * EXTENT_ITEM_KEY please?
	 */
	while (key.type > BTRFS_EXTENT_ITEM_KEY) {
		if (path.slots[0] > 0) {
			path.slots[0]--;
		} else {
			ret = btrfs_prev_leaf(root, &path);
			if (ret < 0) {
				goto out;
			} else if (ret > 0) {
				ret = 0;
				goto out;
			}
		}
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	}

	while (num_bytes) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf "
					"%d\n", ret);
				btrfs_release_path(&path);
				return ret;
			} else if (ret) {
				break;
			}
		}
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}
		if (key.objectid + key.offset < bytenr) {
			path.slots[0]++;
			continue;
		}
		if (key.objectid > bytenr + num_bytes)
			break;

		if (key.objectid == bytenr) {
			if (key.offset >= num_bytes) {
				num_bytes = 0;
				break;
			}
			num_bytes -= key.offset;
			bytenr += key.offset;
		} else if (key.objectid < bytenr) {
			if (key.objectid + key.offset >= bytenr + num_bytes) {
				num_bytes = 0;
				break;
			}
			num_bytes = (bytenr + num_bytes) -
				(key.objectid + key.offset);
			bytenr = key.objectid + key.offset;
		} else {
			if (key.objectid + key.offset < bytenr + num_bytes) {
				u64 new_start = key.objectid + key.offset;
				u64 new_bytes = bytenr + num_bytes - new_start;

				/*
				 * Weird case, the extent is in the middle of
				 * our range, we'll have to search one side
				 * and then the other.  Not sure if this happens
				 * in real life, but no harm in coding it up
				 * anyway just in case.
				 */
				btrfs_release_path(&path);
				ret = check_extent_exists(root, new_start,
							  new_bytes);
				if (ret) {
					fprintf(stderr, "Right section didn't "
						"have a record\n");
					break;
				}
				num_bytes = key.objectid - bytenr;
				goto again;
			}
			num_bytes = key.objectid - bytenr;
		}
		path.slots[0]++;
	}
	ret = 0;

out:
	if (num_bytes && !ret) {
		fprintf(stderr,
			"there are no extents for csum range %llu-%llu\n",
			bytenr, bytenr+num_bytes);
		ret = 1;
	}

	btrfs_release_path(&path);
	return ret;
}

/*
 * Check csum trees with its data
 */
int check_csums(struct btrfs_root *root)
{
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 offset = 0, num_bytes = 0;
	u16 csum_size = btrfs_super_csum_size(root->fs_info->super_copy);
	int errors = 0;
	int ret;
	u64 data_len;
	unsigned long leaf_offset;

	root = root->fs_info->csum_root;
	if (!extent_buffer_uptodate(root->node)) {
		fprintf(stderr, "No valid csum tree found\n");
		return -ENOENT;
	}

	btrfs_init_path(&path);
	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.type = BTRFS_EXTENT_CSUM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching csum tree %d\n", ret);
		btrfs_release_path(&path);
		return ret;
	}

	if (ret > 0 && path.slots[0])
		path.slots[0]--;
	ret = 0;

	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf "
					"%d\n", ret);
				break;
			}
			if (ret)
				break;
		}
		leaf = path.nodes[0];

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_CSUM_KEY) {
			path.slots[0]++;
			continue;
		}

		data_len = (btrfs_item_size_nr(leaf, path.slots[0]) /
			      csum_size) * root->fs_info->sectorsize;
		if (!check_data_csum)
			goto skip_csum_check;
		leaf_offset = btrfs_item_ptr_offset(leaf, path.slots[0]);
		ret = check_extent_csums(root, key.offset, data_len,
					 leaf_offset, leaf);
		if (ret)
			break;
skip_csum_check:
		if (!num_bytes) {
			offset = key.offset;
		} else if (key.offset != offset + num_bytes) {
			ret = check_extent_exists(root, offset, num_bytes);
			if (ret) {
				fprintf(stderr,
		"csum exists for %llu-%llu but there is no extent record\n",
					offset, offset+num_bytes);
				errors++;
			}
			offset = key.offset;
			num_bytes = 0;
		}
		num_bytes += data_len;
		path.slots[0]++;
	}

	btrfs_release_path(&path);
	return errors;
}

/*
 * Re-CoW extent buffers to fix its transid problem
 */
int recow_extent_buffer(struct btrfs_root *root, struct extent_buffer *eb)
{
	struct btrfs_path path;
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	int ret;

	printf("Recowing metadata block %llu\n", eb->start);
	key.objectid = btrfs_header_owner(eb);
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	root = btrfs_read_fs_root(root->fs_info, &key);
	if (IS_ERR(root)) {
		fprintf(stderr, "Couldn't find owner root %llu\n",
			key.objectid);
		return PTR_ERR(root);
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	btrfs_init_path(&path);
	path.lowest_level = btrfs_header_level(eb);
	if (path.lowest_level)
		btrfs_node_key_to_cpu(eb, &key, 0);
	else
		btrfs_item_key_to_cpu(eb, &key, 0);

	ret = btrfs_search_slot(trans, root, &key, &path, 0, 1);
	btrfs_commit_transaction(trans, root);
	btrfs_release_path(&path);
	return ret;
}

/*
 * Search in csum tree to find how many bytes of range [@start, @start + @len)
 * has the corresponding csum item.
 *
 * @start:	range start
 * @len:	range length
 * @found:	return value of found csum bytes
 *		unit is BYTE.
 */
int count_csum_range(struct btrfs_fs_info *fs_info, u64 start,
		     u64 len, u64 *found)
{
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	int ret;
	size_t size;
	*found = 0;
	u64 csum_end;
	u16 csum_size = btrfs_super_csum_size(fs_info->super_copy);

	btrfs_init_path(&path);

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.offset = start;
	key.type = BTRFS_EXTENT_CSUM_KEY;

	ret = btrfs_search_slot(NULL, fs_info->csum_root,
				&key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0 && path.slots[0] > 0) {
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0] - 1);
		if (key.objectid == BTRFS_EXTENT_CSUM_OBJECTID &&
		    key.type == BTRFS_EXTENT_CSUM_KEY)
			path.slots[0]--;
	}

	while (len > 0) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(fs_info->csum_root, &path);
			if (ret > 0)
				break;
			else if (ret < 0)
				goto out;
			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
		    key.type != BTRFS_EXTENT_CSUM_KEY)
			break;

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.offset >= start + len)
			break;

		if (key.offset > start)
			start = key.offset;

		size = btrfs_item_size_nr(leaf, path.slots[0]);
		csum_end = key.offset + (size / csum_size) *
			   fs_info->sectorsize;
		if (csum_end > start) {
			size = min(csum_end - start, len);
			len -= size;
			start += size;
			*found += size;
		}

		path.slots[0]++;
	}
out:
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;
	return 0;
}

/*
 * Wrapper to insert one inode item into given @root
 * Timestamp will be set to current time.
 *
 * @root:	the root to insert inode item into
 * @ino:	inode number
 * @size:	inode size
 * @nbytes:	nbytes (real used size, without hole)
 * @nlink:	number of links
 * @mode:	file mode, including S_IF* bits
 */
int insert_inode_item(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, u64 ino, u64 size,
		      u64 nbytes, u64 nlink, u32 mode)
{
	struct btrfs_inode_item ii;
	time_t now = time(NULL);
	int ret;

	btrfs_set_stack_inode_size(&ii, size);
	btrfs_set_stack_inode_nbytes(&ii, nbytes);
	btrfs_set_stack_inode_nlink(&ii, nlink);
	btrfs_set_stack_inode_mode(&ii, mode);
	btrfs_set_stack_inode_generation(&ii, trans->transid);
	btrfs_set_stack_timespec_nsec(&ii.atime, 0);
	btrfs_set_stack_timespec_sec(&ii.ctime, now);
	btrfs_set_stack_timespec_nsec(&ii.ctime, 0);
	btrfs_set_stack_timespec_sec(&ii.mtime, now);
	btrfs_set_stack_timespec_nsec(&ii.mtime, 0);
	btrfs_set_stack_timespec_sec(&ii.otime, 0);
	btrfs_set_stack_timespec_nsec(&ii.otime, 0);

	ret = btrfs_insert_inode(trans, root, ino, &ii);
	ASSERT(!ret);

	warning("root %llu inode %llu recreating inode item, this may "
		"be incomplete, please check permissions and content after "
		"the fsck completes.\n", (unsigned long long)root->objectid,
		(unsigned long long)ino);

	return 0;
}

static int get_highest_inode(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, struct btrfs_path *path,
			     u64 *highest_ino)
{
	struct btrfs_key key, found_key;
	int ret;

	btrfs_init_path(path);
	key.objectid = BTRFS_LAST_FREE_OBJECTID;
	key.offset = -1;
	key.type = BTRFS_INODE_ITEM_KEY;
	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret == 1) {
		btrfs_item_key_to_cpu(path->nodes[0], &found_key,
				path->slots[0] - 1);
		*highest_ino = found_key.objectid;
		ret = 0;
	}
	if (*highest_ino >= BTRFS_LAST_FREE_OBJECTID)
		ret = -EOVERFLOW;
	btrfs_release_path(path);
	return ret;
}

/*
 * Link inode to dir 'lost+found'. Increase @ref_count.
 *
 * Returns 0 means success.
 * Returns <0 means failure.
 */
int link_inode_to_lostfound(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    struct btrfs_path *path,
			    u64 ino, char *namebuf, u32 name_len,
			    u8 filetype, u64 *ref_count)
{
	char *dir_name = "lost+found";
	u64 lost_found_ino;
	int ret;
	u32 mode = 0700;

	btrfs_release_path(path);
	ret = get_highest_inode(trans, root, path, &lost_found_ino);
	if (ret < 0)
		goto out;
	lost_found_ino++;

	ret = btrfs_mkdir(trans, root, dir_name, strlen(dir_name),
			  BTRFS_FIRST_FREE_OBJECTID, &lost_found_ino,
			  mode);
	if (ret < 0) {
		error("failed to create '%s' dir: %s", dir_name, strerror(-ret));
		goto out;
	}
	ret = btrfs_add_link(trans, root, ino, lost_found_ino,
			     namebuf, name_len, filetype, NULL, 1, 0);
	/*
	 * Add ".INO" suffix several times to handle case where
	 * "FILENAME.INO" is already taken by another file.
	 */
	while (ret == -EEXIST) {
		/*
		 * Conflicting file name, add ".INO" as suffix * +1 for '.'
		 */
		if (name_len + count_digits(ino) + 1 > BTRFS_NAME_LEN) {
			ret = -EFBIG;
			goto out;
		}
		snprintf(namebuf + name_len, BTRFS_NAME_LEN - name_len,
			 ".%llu", ino);
		name_len += count_digits(ino) + 1;
		ret = btrfs_add_link(trans, root, ino, lost_found_ino, namebuf,
				     name_len, filetype, NULL, 1, 0);
	}
	if (ret < 0) {
		error("failed to link the inode %llu to %s dir: %s",
		      ino, dir_name, strerror(-ret));
		goto out;
	}

	++*ref_count;
	printf("Moving file '%.*s' to '%s' dir since it has no valid backref\n",
	       name_len, namebuf, dir_name);
out:
	btrfs_release_path(path);
	if (ret)
		error("failed to move file '%.*s' to '%s' dir", name_len,
				namebuf, dir_name);
	return ret;
}

/*
 * Extra (optional) check for dev_item size to report possbile problem on a new
 * kernel.
 */
void check_dev_size_alignment(u64 devid, u64 total_bytes, u32 sectorsize)
{
	if (!IS_ALIGNED(total_bytes, sectorsize)) {
		warning(
"unaligned total_bytes detected for devid %llu, have %llu should be aligned to %u",
			devid, total_bytes, sectorsize);
		warning(
"this is OK for older kernel, but may cause kernel warning for newer kernels");
		warning("this can be fixed by 'btrfs rescue fix-device-size'");
	}
}

void reada_walk_down(struct btrfs_root *root, struct extent_buffer *node,
		     int slot)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 bytenr;
	u64 ptr_gen;
	u32 nritems;
	int i;
	int level;

	level = btrfs_header_level(node);
	if (level != 1)
		return;

	nritems = btrfs_header_nritems(node);
	for (i = slot; i < nritems; i++) {
		bytenr = btrfs_node_blockptr(node, i);
		ptr_gen = btrfs_node_ptr_generation(node, i);
		readahead_tree_block(fs_info, bytenr, ptr_gen);
	}
}

/*
 * Check the child node/leaf by the following condition:
 * 1. the first item key of the node/leaf should be the same with the one
 *    in parent.
 * 2. block in parent node should match the child node/leaf.
 * 3. generation of parent node and child's header should be consistent.
 *
 * Or the child node/leaf pointed by the key in parent is not valid.
 *
 * We hope to check leaf owner too, but since subvol may share leaves,
 * which makes leaf owner check not so strong, key check should be
 * sufficient enough for that case.
 */
int check_child_node(struct extent_buffer *parent, int slot,
		     struct extent_buffer *child)
{
	struct btrfs_key parent_key;
	struct btrfs_key child_key;
	int ret = 0;

	btrfs_node_key_to_cpu(parent, &parent_key, slot);
	if (btrfs_header_level(child) == 0)
		btrfs_item_key_to_cpu(child, &child_key, 0);
	else
		btrfs_node_key_to_cpu(child, &child_key, 0);

	if (memcmp(&parent_key, &child_key, sizeof(parent_key))) {
		ret = -EINVAL;
		fprintf(stderr,
			"Wrong key of child node/leaf, wanted: (%llu, %u, %llu), have: (%llu, %u, %llu)\n",
			parent_key.objectid, parent_key.type, parent_key.offset,
			child_key.objectid, child_key.type, child_key.offset);
	}
	if (btrfs_header_bytenr(child) != btrfs_node_blockptr(parent, slot)) {
		ret = -EINVAL;
		fprintf(stderr, "Wrong block of child node/leaf, wanted: %llu, have: %llu\n",
			btrfs_node_blockptr(parent, slot),
			btrfs_header_bytenr(child));
	}
	if (btrfs_node_ptr_generation(parent, slot) !=
	    btrfs_header_generation(child)) {
		ret = -EINVAL;
		fprintf(stderr, "Wrong generation of child node/leaf, wanted: %llu, have: %llu\n",
			btrfs_header_generation(child),
			btrfs_node_ptr_generation(parent, slot));
	}
	return ret;
}

void reset_cached_block_groups(struct btrfs_fs_info *fs_info)
{
	struct btrfs_block_group_cache *cache;
	u64 start, end;
	int ret;

	while (1) {
		ret = find_first_extent_bit(&fs_info->free_space_cache, 0,
					    &start, &end, EXTENT_DIRTY);
		if (ret)
			break;
		clear_extent_dirty(&fs_info->free_space_cache, start, end);
	}

	start = 0;
	while (1) {
		cache = btrfs_lookup_first_block_group(fs_info, start);
		if (!cache)
			break;
		if (cache->cached)
			cache->cached = 0;
		start = cache->key.objectid + cache->key.offset;
	}
}
