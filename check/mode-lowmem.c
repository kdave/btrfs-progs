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
#include <sys/stat.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "kernel-lib/rbtree.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/ulist.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/backref.h"
#include "kernel-shared/volumes.h"
#include "common/messages.h"
#include "common/internal.h"
#include "common/utils.h"
#include "common/device-utils.h"
#include "check/repair.h"
#include "check/mode-common.h"
#include "check/mode-lowmem.h"

static u64 last_allocated_chunk;
static u64 total_used = 0;

static int calc_extent_flag(struct btrfs_root *root, struct extent_buffer *eb,
			    u64 *flags_ret)
{
	struct btrfs_root *extent_root;
	struct btrfs_root_item *ri = &root->root_item;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;
	struct btrfs_path *path = NULL;
	unsigned long ptr;
	unsigned long end;
	u64 flags;
	u64 owner = 0;
	u64 offset;
	int slot;
	int type;
	int ret = 0;

	/*
	 * Except file/reloc tree, we can not have FULL BACKREF MODE
	 */
	if (root->objectid < BTRFS_FIRST_FREE_OBJECTID)
		goto normal;

	/* root node */
	if (eb->start == btrfs_root_bytenr(ri))
		goto normal;

	if (btrfs_header_flag(eb, BTRFS_HEADER_FLAG_RELOC))
		goto full_backref;

	owner = btrfs_header_owner(eb);
	if (owner == root->objectid)
		goto normal;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = btrfs_header_bytenr(eb);
	key.type = (u8)-1;
	key.offset = (u64)-1;

	extent_root = btrfs_extent_root(gfs_info, key.objectid);
	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret <= 0) {
		ret = -EIO;
		goto out;
	}

	if (ret > 0) {
		ret = btrfs_previous_extent_item(extent_root, path,
						 key.objectid);
		if (ret)
			goto full_backref;

	}
	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

	eb = path->nodes[0];
	slot = path->slots[0];
	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);

	flags = btrfs_extent_flags(eb, ei);
	if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)
		goto full_backref;

	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + btrfs_item_size(eb, slot);

	if (key.type == BTRFS_EXTENT_ITEM_KEY)
		ptr += sizeof(struct btrfs_tree_block_info);

next:
	/* Reached extent item ends normally */
	if (ptr == end)
		goto full_backref;

	/* Beyond extent item end, wrong item size */
	if (ptr > end) {
		error("extent item at bytenr %llu slot %d has wrong size",
			eb->start, slot);
		goto full_backref;
	}

	iref = (struct btrfs_extent_inline_ref *)ptr;
	offset = btrfs_extent_inline_ref_offset(eb, iref);
	type = btrfs_extent_inline_ref_type(eb, iref);

	if (type == BTRFS_TREE_BLOCK_REF_KEY && offset == owner)
		goto normal;
	ptr += btrfs_extent_inline_ref_size(type);
	goto next;

normal:
	*flags_ret &= ~BTRFS_BLOCK_FLAG_FULL_BACKREF;
	goto out;

full_backref:
	*flags_ret |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
out:
	btrfs_free_path(path);
	return ret;
}

/*
 * for a tree node or leaf, if it's shared, indeed we don't need to iterate it
 * in every fs or file tree check. Here we find its all root ids, and only check
 * it in the fs or file tree which has the smallest root id.
 */
static int need_check(struct btrfs_root *root, struct ulist *roots)
{
	struct rb_node *node;
	struct ulist_node *u;

	/*
	 * @roots can be empty if it belongs to tree reloc tree
	 * In that case, we should always check the leaf, as we can't use
	 * the tree owner to ensure some other root will check it.
	 */
	if (roots->nnodes == 1 || roots->nnodes == 0)
		return 1;

	node = rb_first(&roots->root);
	u = rb_entry(node, struct ulist_node, rb_node);
	/*
	 * current root id is not smallest, we skip it and let it be checked
	 * in the fs or file tree who hash the smallest root id.
	 */
	if (root->objectid != u->val)
		return 0;

	return 1;
}

/*
 * for a tree node or leaf, we record its reference count, so later if we still
 * process this node or leaf, don't need to compute its reference count again.
 *
 * @bytenr  if @bytenr == (u64)-1, only update nrefs->full_backref[level]
 */
static int update_nodes_refs(struct btrfs_root *root, u64 bytenr,
			     struct extent_buffer *eb, struct node_refs *nrefs,
			     u64 level, int check_all)
{
	struct ulist *roots;
	u64 refs = 0;
	u64 flags = 0;
	int root_level = btrfs_header_level(root->node);
	int check;
	int ret;

	if (nrefs->bytenr[level] == bytenr)
		return 0;

	if (bytenr != (u64)-1) {
		/* the return value of this function seems a mistake */
		ret = btrfs_lookup_extent_info(NULL, gfs_info, bytenr,
					       level, 1, &refs, &flags);
		/* temporary fix */
		if (ret < 0 && !check_all)
			return ret;

		nrefs->bytenr[level] = bytenr;
		nrefs->refs[level] = refs;
		nrefs->full_backref[level] = 0;
		nrefs->checked[level] = 0;

		if (refs > 1) {
			ret = btrfs_find_all_roots(NULL, gfs_info, bytenr,
						   0, &roots);
			if (ret)
				return -EIO;

			check = need_check(root, roots);
			ulist_free(roots);
			nrefs->need_check[level] = check;
		} else {
			if (!check_all) {
				nrefs->need_check[level] = 1;
			} else {
				if (level == root_level) {
					nrefs->need_check[level] = 1;
				} else {
					/*
					 * The node refs may have not been
					 * updated if upper needs checking (the
					 * lowest root_objectid) the node can
					 * be checked.
					 */
					nrefs->need_check[level] =
						nrefs->need_check[level + 1];
				}
			}
		}
	}

	if (check_all && eb) {
		calc_extent_flag(root, eb, &flags);
		if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)
			nrefs->full_backref[level] = 1;
	}

	return 0;
}

/*
 * Mark all extents unfree in the block group. And set @block_group->cached
 * according to @cache.
 */
static int modify_block_group_cache(struct btrfs_block_group *block_group, int cache)
{
	struct extent_io_tree *free_space_cache = &gfs_info->free_space_cache;
	u64 start = block_group->start;
	u64 end = start + block_group->length;

	if (cache && !block_group->cached) {
		block_group->cached = 1;
		clear_extent_dirty(free_space_cache, start, end - 1);
	}

	if (!cache && block_group->cached) {
		block_group->cached = 0;
		clear_extent_dirty(free_space_cache, start, end - 1);
	}
	return 0;
}

/*
 * Modify block groups which have @flags unfree in free space cache.
 *
 * @cache: if 0, clear block groups cache state;
 *         not 0, mark blocks groups cached.
 */
static int modify_block_groups_cache(u64 flags, int cache)
{
	struct btrfs_root *root = btrfs_block_group_root(gfs_info);
	struct btrfs_key key;
	struct btrfs_path path;
	struct btrfs_block_group *bg_cache;
	struct btrfs_block_group_item *bi;
	struct btrfs_block_group_item bg_item;
	struct extent_buffer *eb;
	int slot;
	int ret;

	key.objectid = 0;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = 0;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		errno = -ret;
		error("fail to search block groups due to %m");
		goto out;
	}

	while (1) {
		eb = path.nodes[0];
		slot = path.slots[0];
		btrfs_item_key_to_cpu(eb, &key, slot);
		bg_cache = btrfs_lookup_block_group(gfs_info, key.objectid);
		if (!bg_cache) {
			ret = -ENOENT;
			goto out;
		}

		bi = btrfs_item_ptr(eb, slot, struct btrfs_block_group_item);
		read_extent_buffer(eb, &bg_item, (unsigned long)bi,
				   sizeof(bg_item));
		if (btrfs_stack_block_group_flags(&bg_item) & flags)
			modify_block_group_cache(bg_cache, cache);

		ret = btrfs_next_item(root, &path);
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

static int mark_block_groups_full(u64 flags)
{
	return modify_block_groups_cache(flags, 1);
}

static int clear_block_groups_full(u64 flags)
{
	return modify_block_groups_cache(flags, 0);
}

static int create_chunk_and_block_group(u64 flags, u64 *start, u64 *nbytes)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root = btrfs_block_group_root(gfs_info);
	int ret;

	if ((flags & BTRFS_BLOCK_GROUP_TYPE_MASK) == 0)
		return -EINVAL;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}
	ret = btrfs_alloc_chunk(trans, gfs_info, start, nbytes, flags);
	if (ret) {
		errno = -ret;
		error("fail to allocate new chunk %m");
		goto out;
	}
	ret = btrfs_make_block_group(trans, gfs_info, 0, flags, *start,
				     *nbytes);
	if (ret) {
		errno = -ret;
		error("fail to make block group for chunk %llu %llu %m",
		      *start, *nbytes);
		goto out;
	}
out:
	btrfs_commit_transaction(trans, root);
	return ret;
}

static int force_cow_in_new_chunk(u64 *start_ret)
{
	struct btrfs_block_group *bg;
	u64 start;
	u64 nbytes;
	u64 alloc_profile;
	u64 flags;
	int ret;

	alloc_profile = (gfs_info->avail_metadata_alloc_bits &
			 gfs_info->metadata_alloc_profile);
	flags = BTRFS_BLOCK_GROUP_METADATA | alloc_profile;
	if (btrfs_fs_incompat(gfs_info, MIXED_GROUPS))
		flags |= BTRFS_BLOCK_GROUP_DATA;

	ret = create_chunk_and_block_group(flags, &start, &nbytes);
	if (ret)
		goto err;
	printf("Created new chunk [%llu %llu]\n", start, nbytes);

	flags = BTRFS_BLOCK_GROUP_METADATA;
	/* Mark all metadata block groups cached and full in free space*/
	ret = mark_block_groups_full(flags);
	if (ret)
		goto clear_bgs_full;

	bg = btrfs_lookup_block_group(gfs_info, start);
	if (!bg) {
		ret = -ENOENT;
		error("fail to look up block group %llu %llu", start, nbytes);
		goto clear_bgs_full;
	}

	/* Clear block group cache just allocated */
	ret = modify_block_group_cache(bg, 0);
	if (ret)
		goto clear_bgs_full;
	if (start_ret)
		*start_ret = start;
	return 0;

clear_bgs_full:
	clear_block_groups_full(flags);
err:
	return ret;
}

/*
 * Returns 0 means not almost full.
 * Returns >0 means almost full.
 * Returns <0 means fatal error.
 */
static int is_chunk_almost_full(u64 start)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root *root = btrfs_block_group_root(gfs_info);
	struct btrfs_block_group_item *bi;
	struct btrfs_block_group_item bg_item;
	struct extent_buffer *eb;
	u64 used;
	u64 total;
	u64 min_free;
	int ret;
	int slot;

	key.objectid = start;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = (u64)-1;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (!ret)
		ret = -EIO;
	if (ret < 0)
		goto out;
	ret = btrfs_previous_item(root, &path, start,
				  BTRFS_BLOCK_GROUP_ITEM_KEY);
	if (ret) {
		error("failed to find block group %llu", start);
		ret = -ENOENT;
		goto out;
	}

	eb = path.nodes[0];
	slot = path.slots[0];
	btrfs_item_key_to_cpu(eb, &key, slot);
	if (key.objectid != start) {
		ret = -ENOENT;
		goto out;
	}

	total = key.offset;
	bi = btrfs_item_ptr(eb, slot, struct btrfs_block_group_item);
	read_extent_buffer(eb, &bg_item, (unsigned long)bi, sizeof(bg_item));
	used = btrfs_stack_block_group_used(&bg_item);

	/*
	 * if the free space in the chunk is less than %10 of total,
	 * or not not enough for CoW once, we think the chunk is almost full.
	 */
	min_free = max_t(u64, (BTRFS_MAX_LEVEL + 1) * gfs_info->nodesize,
			 div_factor(total, 1));

	if ((total - used) > min_free)
		ret = 0;
	else
		ret = 1;
out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Returns <0 for error.
 * Returns 0 for success.
 */
static int try_to_force_cow_in_new_chunk(u64 old_start, u64 *new_start)
{
	int ret;

	if (old_start) {
		ret = is_chunk_almost_full(old_start);
		if (ret <= 0)
			return ret;
	}
	ret = force_cow_in_new_chunk(new_start);
	return ret;
}

static int avoid_extents_overwrite(void)
{
	int ret;
	int mixed = btrfs_fs_incompat(gfs_info, MIXED_GROUPS);

	if (gfs_info->excluded_extents)
		return 0;

	if (last_allocated_chunk != (u64)-1) {
		ret = try_to_force_cow_in_new_chunk(last_allocated_chunk,
				&last_allocated_chunk);
		if (!ret)
			goto out;
		/*
		 * If failed, do not try to allocate chunk again in
		 * next call.
		 * If there is no space left to allocate, try to exclude all
		 * metadata blocks. Mixed filesystem is unsupported.
		 */
		last_allocated_chunk = (u64)-1;
		if (ret != -ENOSPC || mixed)
			goto out;
	}

	printf(
	"Try to exclude all metadata blocks and extents, it may be slow\n");
	ret = exclude_metadata_blocks();
out:
	if (ret) {
		errno = -ret;
		error("failed to avoid extents overwrite %m");
	}
	return ret;
}

static int end_avoid_extents_overwrite(void)
{
	int ret = 0;

	cleanup_excluded_extents();
	if (last_allocated_chunk)
		ret = clear_block_groups_full(BTRFS_BLOCK_GROUP_METADATA);
	return ret;
}

/*
 * Delete the item @path point to. A wrapper of btrfs_del_item().
 *
 * If deleted successfully, @path will point to the previous item of the
 * deleted item.
 */
static int delete_item(struct btrfs_root *root, struct btrfs_path *path)
{
	struct btrfs_key key;
	struct btrfs_trans_handle *trans;
	int ret = 0;

	ret = avoid_extents_overwrite();
	if (ret)
		return ret;
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		goto out;
	}
	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret) {
		ret = -ENOENT;
		goto out;
	}

	ret = btrfs_del_item(trans, root, path);
	if (ret)
		goto out;

	if (path->slots[0] == 0)
		btrfs_prev_leaf(root, path);
	else
		path->slots[0]--;
out:
	btrfs_commit_transaction(trans, root);
	if (ret)
		error("failed to delete root %llu item[%llu, %u, %llu]",
		      root->objectid, key.objectid, key.type, key.offset);
	else
		printf("Deleted root %llu item[%llu, %u, %llu]\n",
		       root->objectid, key.objectid, key.type, key.offset);
	return ret;
}

/*
 * Wrapper function for btrfs_fix_block_accounting().
 *
 * Returns 0     on success.
 * Returns != 0  on error.
 */
static int repair_block_accounting(void)
{
	struct btrfs_trans_handle *trans = NULL;
	int ret;

	trans = btrfs_start_transaction(gfs_info->tree_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	ret = btrfs_fix_block_accounting(trans);
	btrfs_commit_transaction(trans, gfs_info->tree_root);
	return ret;
}

/*
 * This function only handles BACKREF_MISSING,
 * If corresponding extent item exists, increase the ref, else insert an extent
 * item and backref.
 *
 * Returns error bits after repair.
 */
static int repair_tree_block_ref(struct btrfs_root *root,
				 struct extent_buffer *node,
				 struct node_refs *nrefs, int level, int err)
{
	struct btrfs_trans_handle *trans = NULL;
	struct btrfs_root *extent_root;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	struct btrfs_tree_block_info *bi;
	struct btrfs_key key;
	struct extent_buffer *eb;
	u32 size = sizeof(*ei);
	u32 node_size = gfs_info->nodesize;
	int insert_extent = 0;
	int skinny_metadata = btrfs_fs_incompat(gfs_info, SKINNY_METADATA);
	int root_level = btrfs_header_level(root->node);
	int generation;
	int ret;
	u64 owner;
	u64 bytenr;
	u64 flags = BTRFS_EXTENT_FLAG_TREE_BLOCK;
	u64 parent = 0;

	if ((err & BACKREF_MISSING) == 0)
		return err;

	WARN_ON(level > BTRFS_MAX_LEVEL);
	WARN_ON(level < 0);

	btrfs_init_path(&path);
	bytenr = btrfs_header_bytenr(node);
	owner = btrfs_header_owner(node);
	generation = btrfs_header_generation(node);

	key.objectid = bytenr;
	key.type = (u8)-1;
	key.offset = (u64)-1;

	/* Search for the extent item */
	extent_root = btrfs_extent_root(gfs_info, bytenr);
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret <= 0) {
		ret = -EIO;
		goto out;
	}

	ret = btrfs_previous_extent_item(extent_root, &path, bytenr);
	if (ret)
		insert_extent = 1;

	/* calculate if the extent item flag is full backref or not */
	if (nrefs->full_backref[level] != 0)
		flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;

	ret = avoid_extents_overwrite();
	if (ret)
		goto out;
	trans = btrfs_start_transaction(extent_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		trans = NULL;
		goto out;
	}
	/* insert an extent item */
	if (insert_extent) {
		struct btrfs_disk_key copy_key;

		generation = btrfs_header_generation(node);

		if (level < root_level && nrefs->full_backref[level + 1] &&
		    owner != root->objectid) {
			flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
		}

		key.objectid = bytenr;
		if (!skinny_metadata) {
			key.type = BTRFS_EXTENT_ITEM_KEY;
			key.offset = node_size;
			size += sizeof(*bi);
		} else {
			key.type = BTRFS_METADATA_ITEM_KEY;
			key.offset = level;
		}

		btrfs_release_path(&path);
		ret = btrfs_insert_empty_item(trans, extent_root, &path, &key,
					      size);
		if (ret)
			goto out;

		eb = path.nodes[0];
		ei = btrfs_item_ptr(eb, path.slots[0], struct btrfs_extent_item);

		btrfs_set_extent_refs(eb, ei, 0);
		btrfs_set_extent_generation(eb, ei, generation);
		btrfs_set_extent_flags(eb, ei, flags);

		if (!skinny_metadata) {
			bi = (struct btrfs_tree_block_info *)(ei + 1);
			memset_extent_buffer(eb, 0, (unsigned long)bi,
					     sizeof(*bi));
			btrfs_set_disk_key_objectid(&copy_key, root->objectid);
			btrfs_set_disk_key_type(&copy_key, 0);
			btrfs_set_disk_key_offset(&copy_key, 0);

			btrfs_set_tree_block_level(eb, bi, level);
			btrfs_set_tree_block_key(eb, bi, &copy_key);
		}
		btrfs_mark_buffer_dirty(eb);
		printf("Added an extent item [%llu %u]\n", bytenr, node_size);
		btrfs_update_block_group(trans, bytenr, node_size, 1, 0);

		nrefs->refs[level] = 0;
		nrefs->full_backref[level] =
			flags & BTRFS_BLOCK_FLAG_FULL_BACKREF;
		btrfs_release_path(&path);
	}

	if (level < root_level && nrefs->full_backref[level + 1] &&
	    owner != root->objectid)
		parent = nrefs->bytenr[level + 1];

	/* increase the ref */
	ret = btrfs_inc_extent_ref(trans, extent_root, bytenr, node_size,
			parent, root->objectid, level, 0);

	nrefs->refs[level]++;
out:
	if (trans)
		btrfs_commit_transaction(trans, extent_root);
	btrfs_release_path(&path);
	if (ret) {
		errno = -ret;
		error(
	"failed to repair tree block ref start %llu root %llu due to %m",
		      bytenr, root->objectid);
	} else {
		printf("Added one tree block ref start %llu %s %llu\n",
		       bytenr, parent ? "parent" : "root",
		       parent ? parent : root->objectid);
		err &= ~BACKREF_MISSING;
	}

	return err;
}

/*
 * Update global fs information.
 */
static void account_bytes(struct btrfs_root *root, struct btrfs_path *path,
			 int level)
{
	u32 free_nrs;
	struct extent_buffer *eb = path->nodes[level];

	total_btree_bytes += eb->len;
	if (fs_root_objectid(root->objectid))
		total_fs_tree_bytes += eb->len;
	if (btrfs_header_owner(eb) == BTRFS_EXTENT_TREE_OBJECTID)
		total_extent_tree_bytes += eb->len;

	if (level == 0) {
		btree_space_waste += btrfs_leaf_free_space(eb);
	} else {
		free_nrs = (BTRFS_NODEPTRS_PER_BLOCK(gfs_info) -
			    btrfs_header_nritems(eb));
		btree_space_waste += free_nrs * sizeof(struct btrfs_key_ptr);
	}
}

/*
 * Find the @index according by @ino and name.
 * Notice:time efficiency is O(N)
 *
 * @root:	the root of the fs/file tree
 * @index_ret:	the index as return value
 * @namebuf:	the name to match
 * @name_len:	the length of name to match
 * @file_type:	the file_type of INODE_ITEM to match
 *
 * Returns 0 if found and *@index_ret will be modified with right value
 * Returns< 0 not found and *@index_ret will be (u64)-1
 */
static int find_dir_index(struct btrfs_root *root, u64 dirid, u64 location_id,
			  u64 *index_ret, char *namebuf, u32 name_len,
			  u8 file_type)
{
	struct btrfs_path path;
	struct extent_buffer *node;
	struct btrfs_dir_item *di;
	struct btrfs_key key;
	struct btrfs_key location;
	char name[BTRFS_NAME_LEN] = {0};

	u32 total;
	u32 cur = 0;
	u32 len;
	u32 data_len;
	u8 filetype;
	int slot;
	int ret;

	ASSERT(index_ret);

	/* search from the last index */
	key.objectid = dirid;
	key.offset = (u64)-1;
	key.type = BTRFS_DIR_INDEX_KEY;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;

loop:
	ret = btrfs_previous_item(root, &path, dirid, BTRFS_DIR_INDEX_KEY);
	if (ret) {
		ret = -ENOENT;
		*index_ret = (64)-1;
		goto out;
	}
	/* Check whether inode_id/filetype/name match */
	node = path.nodes[0];
	slot = path.slots[0];
	di = btrfs_item_ptr(node, slot, struct btrfs_dir_item);
	total = btrfs_item_size(node, slot);
	while (cur < total) {
		ret = -ENOENT;
		len = btrfs_dir_name_len(node, di);
		data_len = btrfs_dir_data_len(node, di);

		btrfs_dir_item_key_to_cpu(node, di, &location);
		if (location.objectid != location_id ||
		    location.type != BTRFS_INODE_ITEM_KEY ||
		    location.offset != 0)
			goto next;

		filetype = btrfs_dir_type(node, di);
		if (file_type != filetype)
			goto next;

		if (len > BTRFS_NAME_LEN)
			len = BTRFS_NAME_LEN;

		read_extent_buffer(node, name, (unsigned long)(di + 1), len);
		if (len != name_len || strncmp(namebuf, name, len))
			goto next;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		*index_ret = key.offset;
		ret = 0;
		goto out;
next:
		len += sizeof(*di) + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}
	goto loop;

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Find DIR_ITEM/DIR_INDEX for the given key and check it with the specified
 * INODE_REF/INODE_EXTREF match.
 *
 * @root:	the root of the fs/file tree
 * @key:	the key of the DIR_ITEM/DIR_INDEX, key->offset will be right
 *              value while find index
 * @location_key: location key of the struct btrfs_dir_item to match
 * @name:	the name to match
 * @namelen:	the length of name
 * @file_type:	the type of file to math
 *
 * Return 0 if no error occurred.
 * Return DIR_ITEM_MISSING/DIR_INDEX_MISSING if couldn't find
 * DIR_ITEM/DIR_INDEX
 * Return DIR_ITEM_MISMATCH/DIR_INDEX_MISMATCH if INODE_REF/INODE_EXTREF
 * and DIR_ITEM/DIR_INDEX mismatch
 */
static int find_dir_item(struct btrfs_root *root, struct btrfs_key *key,
			 struct btrfs_key *location_key, char *name,
			 u32 namelen, u8 file_type)
{
	struct btrfs_path path;
	struct extent_buffer *node;
	struct btrfs_dir_item *di;
	struct btrfs_key location;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 data_len;
	u8 filetype;
	int slot;
	int ret;

	/* get the index by traversing all index */
	if (key->type == BTRFS_DIR_INDEX_KEY && key->offset == (u64)-1) {
		ret = find_dir_index(root, key->objectid,
				     location_key->objectid, &key->offset,
				     name, namelen, file_type);
		if (ret)
			ret = DIR_INDEX_MISSING;
		return ret;
	}

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, key, &path, 0, 0);
	if (ret) {
		ret = key->type == BTRFS_DIR_ITEM_KEY ? DIR_ITEM_MISSING :
			DIR_INDEX_MISSING;
		goto out;
	}

	/* Check whether inode_id/filetype/name match */
	node = path.nodes[0];
	slot = path.slots[0];
	di = btrfs_item_ptr(node, slot, struct btrfs_dir_item);
	total = btrfs_item_size(node, slot);
	while (cur < total) {
		ret = key->type == BTRFS_DIR_ITEM_KEY ?
			DIR_ITEM_MISMATCH : DIR_INDEX_MISMATCH;

		len = btrfs_dir_name_len(node, di);
		data_len = btrfs_dir_data_len(node, di);

		btrfs_dir_item_key_to_cpu(node, di, &location);
		if (location.objectid != location_key->objectid ||
		    location.type != location_key->type ||
		    location.offset != location_key->offset)
			goto next;

		filetype = btrfs_dir_type(node, di);
		if (file_type != filetype)
			goto next;

		if (len > BTRFS_NAME_LEN) {
			len = BTRFS_NAME_LEN;
			warning("root %llu %s[%llu %llu] name too long %u, trimmed",
			root->objectid,
			key->type == BTRFS_DIR_ITEM_KEY ?
			"DIR_ITEM" : "DIR_INDEX",
			key->objectid, key->offset, len);
		}
		read_extent_buffer(node, namebuf, (unsigned long)(di + 1),
				   len);
		if (len != namelen || strncmp(namebuf, name, len))
			goto next;

		ret = 0;
		goto out;
next:
		len += sizeof(*di) + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * The ternary means dir item, dir index and relative inode ref.
 * The function handles errs: INODE_MISSING, DIR_INDEX_MISSING
 * DIR_INDEX_MISMATCH, DIR_ITEM_MISSING, DIR_ITEM_MISMATCH by the follow
 * strategy:
 * If two of three is missing or mismatched, delete the existing one.
 * If one of three is missing or mismatched, add the missing one.
 *
 * returns 0 means success.
 * returns not 0 means on error;
 */
static int repair_ternary_lowmem(struct btrfs_root *root, u64 dir_ino, u64 ino,
			  u64 index, char *name, int name_len, u8 filetype,
			  int err)
{
	struct btrfs_trans_handle *trans;
	int stage = 0;
	int ret = 0;

	/*
	 * stage shall be one of following valild values:
	 *	0: Fine, nothing to do.
	 *	1: One of three is wrong, so add missing one.
	 *	2: Two of three is wrong, so delete existed one.
	 */
	if (err & (DIR_INDEX_MISMATCH | DIR_INDEX_MISSING))
		stage++;
	if (err & (DIR_ITEM_MISMATCH | DIR_ITEM_MISSING))
		stage++;
	if (err & (INODE_REF_MISSING))
		stage++;

	/* stage must be smllarer than 3 */
	ASSERT(stage < 3);

	trans = btrfs_start_transaction(root, 1);
	if (stage == 2) {
		ret = btrfs_unlink(trans, root, ino, dir_ino, index, name,
				   name_len, 0);
		goto out;
	}
	if (stage == 1) {
		ret = btrfs_unlink(trans, root, ino, dir_ino, index, name,
				name_len, 0);
		if (ret)
			goto out;
		ret = btrfs_add_link(trans, root, ino, dir_ino, name, name_len,
			       filetype, &index, 1, 1);
		goto out;
	}
out:
	btrfs_commit_transaction(trans, root);

	if (ret)
		error("fail to repair inode %llu name %s filetype %u",
		      ino, name, filetype);
	else
		printf("%s ref/dir_item of inode %llu name %s filetype %u\n",
		       stage == 2 ? "Delete" : "Add",
		       ino, name, filetype);

	return ret;
}

/*
 * Prints inode ref error message
 */
static void print_inode_ref_err(struct btrfs_root *root, struct btrfs_key *key,
				u64 index, const char *namebuf, int name_len,
				u8 filetype, int err)
{
	if (!err)
		return;

	/* root dir error */
	if (key->objectid == BTRFS_FIRST_FREE_OBJECTID) {
		error(
	"root %llu root dir shouldn't have INODE REF[%llu %llu] name %s",
		      root->objectid, key->objectid, key->offset, namebuf);
		return;
	}

	/* normal error */
	if (err & (DIR_ITEM_MISMATCH | DIR_ITEM_MISSING))
		error("root %llu DIR ITEM[%llu %llu] %s name %s filetype %u",
		      root->objectid, key->offset,
		      btrfs_name_hash(namebuf, name_len),
		      err & DIR_ITEM_MISMATCH ? "mismatch" : "missing",
		      namebuf, filetype);
	if (err & (DIR_INDEX_MISMATCH | DIR_INDEX_MISSING))
		error("root %llu DIR INDEX[%llu %llu] %s name %s filetype %u",
		      root->objectid, key->offset, index,
		      err & DIR_ITEM_MISMATCH ? "mismatch" : "missing",
		      namebuf, filetype);
}

/*
 * Traverse the given INODE_REF and call find_dir_item() to find related
 * DIR_ITEM/DIR_INDEX.
 *
 * @root:	the root of the fs/file tree
 * @ref_key:	the key of the INODE_REF
 * @path        the path provides node and slot
 * @refs:	the count of INODE_REF
 * @mode:	the st_mode of INODE_ITEM
 * @name_ret:   returns with the first ref's name
 * @name_len_ret:    len of the name_ret
 *
 * Return 0 if no error occurred.
 */
static int check_inode_ref(struct btrfs_root *root, struct btrfs_key *ref_key,
			   struct btrfs_path *path, char *name_ret,
			   u32 *namelen_ret, u64 *refs_ret, int mode)
{
	struct btrfs_key key;
	struct btrfs_key location;
	struct btrfs_inode_ref *ref;
	struct extent_buffer *node;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	int ret;
	int err = 0;
	int tmp_err;
	int slot;
	int need_research = 0;
	u64 refs;

begin:
	err = 0;
	cur = 0;
	refs = *refs_ret;

	/* since after repair, path and the dir item may be changed */
	if (need_research) {
		need_research = 0;
		btrfs_release_path(path);
		ret = btrfs_search_slot(NULL, root, ref_key, path, 0, 0);
		/*
		 * The item was deleted, let the path point to the last checked
		 * item.
		 */
		if (ret > 0) {
			if (path->slots[0] == 0)
				btrfs_prev_leaf(root, path);
			else
				path->slots[0]--;
		}
		if (ret)
			goto out;
	}

	location.objectid = ref_key->objectid;
	location.type = BTRFS_INODE_ITEM_KEY;
	location.offset = 0;
	node = path->nodes[0];
	slot = path->slots[0];

	memset(namebuf, 0, sizeof(namebuf) / sizeof(*namebuf));
	ref = btrfs_item_ptr(node, slot, struct btrfs_inode_ref);
	total = btrfs_item_size(node, slot);

next:
	/* Update inode ref count */
	refs++;
	tmp_err = 0;
	index = btrfs_inode_ref_index(node, ref);
	name_len = btrfs_inode_ref_name_len(node, ref);

	if (name_len <= BTRFS_NAME_LEN) {
		len = name_len;
	} else {
		len = BTRFS_NAME_LEN;
		warning("root %llu INODE_REF[%llu %llu] name too long",
			root->objectid, ref_key->objectid, ref_key->offset);
	}

	read_extent_buffer(node, namebuf, (unsigned long)(ref + 1), len);

	/* copy the first name found to name_ret */
	if (refs == 1 && name_ret) {
		memcpy(name_ret, namebuf, len);
		*namelen_ret = len;
	}

	/* Check root dir ref */
	if (ref_key->objectid == BTRFS_FIRST_FREE_OBJECTID) {
		if (index != 0 || len != strlen("..") ||
		    strncmp("..", namebuf, len) ||
		    ref_key->offset != BTRFS_FIRST_FREE_OBJECTID) {
			/* set err bits then repair will delete the ref */
			err |= DIR_INDEX_MISSING;
			err |= DIR_ITEM_MISSING;
		}
		goto end;
	}

	/* Find related DIR_INDEX */
	key.objectid = ref_key->offset;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = index;
	tmp_err |= find_dir_item(root, &key, &location, namebuf, len,
			    imode_to_type(mode));

	/* Find related dir_item */
	key.objectid = ref_key->offset;
	key.type = BTRFS_DIR_ITEM_KEY;
	key.offset = btrfs_name_hash(namebuf, len);
	tmp_err |= find_dir_item(root, &key, &location, namebuf, len,
			    imode_to_type(mode));
end:
	if (tmp_err && opt_check_repair) {
		ret = repair_ternary_lowmem(root, ref_key->offset,
					    ref_key->objectid, index, namebuf,
					    name_len, imode_to_type(mode),
					    tmp_err);
		if (!ret) {
			need_research = 1;
			goto begin;
		}
	}
	print_inode_ref_err(root, ref_key, index, namebuf, name_len,
			    imode_to_type(mode), tmp_err);
	err |= tmp_err;
	len = sizeof(*ref) + name_len;
	ref = (struct btrfs_inode_ref *)((char *)ref + len);
	cur += len;
	if (cur < total)
		goto next;

out:
	*refs_ret = refs;
	return err;
}

/*
 * Traverse the given INODE_EXTREF and call find_dir_item() to find related
 * DIR_ITEM/DIR_INDEX.
 *
 * @root:	the root of the fs/file tree
 * @ref_key:	the key of the INODE_EXTREF
 * @refs:	the count of INODE_EXTREF
 * @mode:	the st_mode of INODE_ITEM
 *
 * Return 0 if no error occurred.
 */
static int check_inode_extref(struct btrfs_root *root,
			      struct btrfs_key *ref_key,
			      struct extent_buffer *node, int slot, u64 *refs,
			      int mode)
{
	struct btrfs_key key;
	struct btrfs_key location;
	struct btrfs_inode_extref *extref;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	u64 parent;
	int ret;
	int err = 0;

	location.objectid = ref_key->objectid;
	location.type = BTRFS_INODE_ITEM_KEY;
	location.offset = 0;

	extref = btrfs_item_ptr(node, slot, struct btrfs_inode_extref);
	total = btrfs_item_size(node, slot);

next:
	/* update inode ref count */
	(*refs)++;
	name_len = btrfs_inode_extref_name_len(node, extref);
	index = btrfs_inode_extref_index(node, extref);
	parent = btrfs_inode_extref_parent(node, extref);
	if (name_len <= BTRFS_NAME_LEN) {
		len = name_len;
	} else {
		len = BTRFS_NAME_LEN;
		warning("root %llu INODE_EXTREF[%llu %llu] name too long",
			root->objectid, ref_key->objectid, ref_key->offset);
	}
	read_extent_buffer(node, namebuf, (unsigned long)(extref + 1), len);

	/* Check root dir ref name */
	if (index == 0 && strncmp(namebuf, "..", name_len)) {
		error("root %llu INODE_EXTREF[%llu %llu] ROOT_DIR name shouldn't be %s",
		      root->objectid, ref_key->objectid, ref_key->offset,
		      namebuf);
		err |= ROOT_DIR_ERROR;
	}

	/* find related dir_index */
	key.objectid = parent;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = index;
	ret = find_dir_item(root, &key, &location, namebuf, len, mode);
	err |= ret;

	/* find related dir_item */
	key.objectid = parent;
	key.type = BTRFS_DIR_ITEM_KEY;
	key.offset = btrfs_name_hash(namebuf, len);
	ret = find_dir_item(root, &key, &location, namebuf, len, mode);
	err |= ret;

	len = sizeof(*extref) + name_len;
	extref = (struct btrfs_inode_extref *)((char *)extref + len);
	cur += len;

	if (cur < total)
		goto next;

	return err;
}

/*
 * Find INODE_REF/INODE_EXTREF for the given key and check it with the specified
 * DIR_ITEM/DIR_INDEX match.
 * Return with @index_ret.
 *
 * @root:	the root of the fs/file tree
 * @key:	the key of the INODE_REF/INODE_EXTREF
 * @name:	the name in the INODE_REF/INODE_EXTREF
 * @namelen:	the length of name in the INODE_REF/INODE_EXTREF
 * @index_ret:	the index in the INODE_REF/INODE_EXTREF,
 *              value (64)-1 means do not check index
 *
 * Return 0 if no error occurred.
 * Return >0 for error bitmap
 */
static int find_inode_ref(struct btrfs_root *root, struct btrfs_key *key,
			  char *name, int namelen, u64 *index_ret)

{
	struct btrfs_path path;
	struct btrfs_inode_ref *ref;
	struct btrfs_inode_extref *extref;
	struct extent_buffer *node;
	char ref_namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 ref_namelen;
	u64 ref_index;
	u64 parent;
	u64 dir_id;
	int slot;
	int ret;

	ASSERT(index_ret);

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, key, &path, 0, 0);
	if (ret) {
		ret = INODE_REF_MISSING;
		goto extref;
	}

	node = path.nodes[0];
	slot = path.slots[0];

	ref = btrfs_item_ptr(node, slot, struct btrfs_inode_ref);
	total = btrfs_item_size(node, slot);

	/* Iterate all entry of INODE_REF */
	while (cur < total) {
		ret = INODE_REF_MISSING;

		ref_namelen = btrfs_inode_ref_name_len(node, ref);
		ref_index = btrfs_inode_ref_index(node, ref);
		if (*index_ret != (u64)-1 && *index_ret != ref_index)
			goto next_ref;

		if (cur + sizeof(*ref) + ref_namelen > total ||
		    ref_namelen > BTRFS_NAME_LEN) {
			warning("root %llu INODE %s[%llu %llu] name too long",
				root->objectid,
				key->type == BTRFS_INODE_REF_KEY ?
					"REF" : "EXTREF",
				key->objectid, key->offset);

			if (cur + sizeof(*ref) > total)
				break;
			len = min_t(u32, total - cur - sizeof(*ref),
				    BTRFS_NAME_LEN);
		} else {
			len = ref_namelen;
		}

		read_extent_buffer(node, ref_namebuf, (unsigned long)(ref + 1),
				   len);

		if (len != namelen || strncmp(ref_namebuf, name, len))
			goto next_ref;

		*index_ret = ref_index;
		ret = 0;
		goto out;
next_ref:
		len = sizeof(*ref) + ref_namelen;
		ref = (struct btrfs_inode_ref *)((char *)ref + len);
		cur += len;
	}

extref:

	/* Skip if not support EXTENDED_IREF feature */
	if (!btrfs_fs_incompat(gfs_info, EXTENDED_IREF))
		goto out;

	btrfs_release_path(&path);
	btrfs_init_path(&path);

	dir_id = key->offset;
	key->type = BTRFS_INODE_EXTREF_KEY;
	key->offset = btrfs_extref_hash(dir_id, name, namelen);

	ret = btrfs_search_slot(NULL, root, key, &path, 0, 0);
	if (ret) {
		ret = INODE_REF_MISSING;
		goto out;
	}

	node = path.nodes[0];
	slot = path.slots[0];

	extref = btrfs_item_ptr(node, slot, struct btrfs_inode_extref);
	cur = 0;
	total = btrfs_item_size(node, slot);

	/* Iterate all entry of INODE_EXTREF */
	while (cur < total) {
		ret = INODE_REF_MISSING;

		ref_namelen = btrfs_inode_extref_name_len(node, extref);
		ref_index = btrfs_inode_extref_index(node, extref);
		parent = btrfs_inode_extref_parent(node, extref);
		if (*index_ret != (u64)-1 && *index_ret != ref_index)
			goto next_extref;

		if (parent != dir_id)
			goto next_extref;

		if (ref_namelen <= BTRFS_NAME_LEN) {
			len = ref_namelen;
		} else {
			len = BTRFS_NAME_LEN;
			warning("root %llu INODE %s[%llu %llu] name too long",
				root->objectid,
				key->type == BTRFS_INODE_REF_KEY ?
					"REF" : "EXTREF",
				key->objectid, key->offset);
		}
		read_extent_buffer(node, ref_namebuf,
				   (unsigned long)(extref + 1), len);

		if (len != namelen || strncmp(ref_namebuf, name, len))
			goto next_extref;

		*index_ret = ref_index;
		ret = 0;
		goto out;

next_extref:
		len = sizeof(*extref) + ref_namelen;
		extref = (struct btrfs_inode_extref *)((char *)extref + len);
		cur += len;

	}
out:
	btrfs_release_path(&path);
	return ret;
}

static int create_inode_item_lowmem(struct btrfs_trans_handle *trans,
				    struct btrfs_root *root, u64 ino,
				    u8 filetype)
{
	u32 mode = (filetype == BTRFS_FT_DIR ? S_IFDIR : S_IFREG) | 0755;

	return insert_inode_item(trans, root, ino, 0, 0, 0, mode);
}

/*
 * Insert the missing inode item.
 *
 * Returns 0 means success.
 * Returns <0 means error.
 */
static int repair_inode_item_missing(struct btrfs_root *root, u64 ino,
				     u8 filetype)
{
	struct btrfs_key key;
	struct btrfs_trans_handle *trans;
	struct btrfs_path path;
	int ret;

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	btrfs_init_path(&path);
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = -EIO;
		goto out;
	}

	ret = btrfs_search_slot(trans, root, &key, &path, 1, 1);
	if (ret < 0 || !ret)
		goto fail;

	/* insert inode item */
	create_inode_item_lowmem(trans, root, ino, filetype);
	ret = 0;
fail:
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error("failed to repair root %llu INODE ITEM[%llu] missing",
		      root->objectid, ino);
	btrfs_release_path(&path);
	return ret;
}

/*
 * A wrapper for delete_corrupted_dir_item(), with support part like
 * start/commit transaction.
 */
static int lowmem_delete_corrupted_dir_item(struct btrfs_root *root,
					    struct btrfs_key *di_key,
					    char *namebuf, u32 name_len)
{
	struct btrfs_trans_handle *trans;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	ret = delete_corrupted_dir_item(trans, root, di_key, namebuf, name_len);
	if (ret < 0) {
		btrfs_abort_transaction(trans, ret);
	} else {
		ret = btrfs_commit_transaction(trans, root);
		if (ret < 0) {
			errno = -ret;
			error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		}
	}
	return ret;
}

static int try_repair_imode(struct btrfs_root *root, u64 ino)
{
	struct btrfs_inode_item *iitem;
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;
	btrfs_init_path(&path);

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0)
		goto out;
	iitem = btrfs_item_ptr(path.nodes[0], path.slots[0],
			       struct btrfs_inode_item);
	if (!is_valid_imode(btrfs_inode_mode(path.nodes[0], iitem))) {
		ret = repair_imode_common(root, &path);
	} else {
		ret = -ENOTTY;
	}
out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Call repair_inode_item_missing and repair_ternary_lowmem to repair
 *
 * Returns error after repair
 */
static int repair_dir_item(struct btrfs_root *root, struct btrfs_key *di_key,
			   u64 ino, u64 index, u8 filetype, char *namebuf,
			   u32 name_len, int err)
{
	u64 dirid = di_key->objectid;
	int ret;

	if (err & (DIR_ITEM_HASH_MISMATCH)) {
		ret = lowmem_delete_corrupted_dir_item(root, di_key, namebuf,
						       name_len);
		if (!ret)
			err &= ~(DIR_ITEM_HASH_MISMATCH);
	}
	if (err & INODE_ITEM_MISSING) {
		ret = repair_inode_item_missing(root, ino, filetype);
		if (!ret)
			err &= ~(INODE_ITEM_MISMATCH | INODE_ITEM_MISSING);
	}

	if (err & INODE_ITEM_MISMATCH) {
		/*
		 * INODE_ITEM mismatch can be caused by bad imode, so check if
		 * it's a bad imode, then repair if possible.
		 */
		ret = try_repair_imode(root, ino);
		if (!ret)
			err &= ~INODE_ITEM_MISMATCH;
	}

	if (err & ~(INODE_ITEM_MISMATCH | INODE_ITEM_MISSING)) {
		ret = repair_ternary_lowmem(root, dirid, ino, index, namebuf,
					    name_len, filetype, err);
		if (!ret) {
			err &= ~(DIR_INDEX_MISMATCH | DIR_INDEX_MISSING);
			err &= ~(DIR_ITEM_MISMATCH | DIR_ITEM_MISSING);
			err &= ~(INODE_REF_MISSING);
		}
	}
	return err;
}

static void print_dir_item_err(struct btrfs_root *root, struct btrfs_key *key,
			       u64 ino, u64 index, const char *namebuf,
			       int name_len, u8 filetype, int err)
{
	if (err & (DIR_ITEM_MISMATCH | DIR_ITEM_MISSING)) {
		error("root %llu DIR ITEM[%llu %llu] name %s filetype %d %s",
		      root->objectid, key->objectid, key->offset, namebuf,
		      filetype,
		      err & DIR_ITEM_MISMATCH ? "mismath" : "missing");
	}

	if (err & (DIR_INDEX_MISMATCH | DIR_INDEX_MISSING)) {
		error("root %llu DIR INDEX[%llu %llu] name %s filetype %d %s",
		      root->objectid, key->objectid, index, namebuf, filetype,
		      err & DIR_ITEM_MISMATCH ? "mismath" : "missing");
	}

	if (err & (INODE_ITEM_MISSING | INODE_ITEM_MISMATCH)) {
		error(
		"root %llu INODE_ITEM[%llu] index %llu name %s filetype %d %s",
		      root->objectid, ino, index, namebuf, filetype,
		      err & INODE_ITEM_MISMATCH ? "mismath" : "missing");
	}

	if (err & INODE_REF_MISSING)
		error(
		"root %llu INODE REF[%llu, %llu] name %s filetype %u missing",
		      root->objectid, ino, key->objectid, namebuf, filetype);

}

/*
 * Traverse the given DIR_ITEM/DIR_INDEX and check related INODE_ITEM and
 * call find_inode_ref() to check related INODE_REF/INODE_EXTREF.
 *
 * @root:	the root of the fs/file tree
 * @key:	the key of the INODE_REF/INODE_EXTREF
 * @path:       the path
 * @size:	the st_size of the INODE_ITEM
 *
 * Return 0 if no error occurred.
 * Return DIR_COUNT_AGAIN if the isize of the inode should be recalculated.
 */
static int check_dir_item(struct btrfs_root *root, struct btrfs_key *di_key,
			  struct btrfs_path *path, u64 *size)
{
	struct btrfs_dir_item *di;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key location;
	struct extent_buffer *node;
	int slot;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u32 data_len;
	u8 filetype;
	u32 mode = 0;
	u64 index;
	int ret;
	int err;
	int tmp_err;
	int need_research = 0;

begin:
	err = 0;
	cur = 0;

	/* since after repair, path and the dir item may be changed */
	if (need_research) {
		need_research = 0;
		err |= DIR_COUNT_AGAIN;
		btrfs_release_path(path);
		ret = btrfs_search_slot(NULL, root, di_key, path, 0, 0);
		/* the item was deleted, let path point the last checked item */
		if (ret > 0) {
			if (path->slots[0] == 0)
				btrfs_prev_leaf(root, path);
			else
				path->slots[0]--;
		}
		if (ret)
			goto out;
	}

	node = path->nodes[0];
	slot = path->slots[0];

	di = btrfs_item_ptr(node, slot, struct btrfs_dir_item);
	total = btrfs_item_size(node, slot);
	memset(namebuf, 0, sizeof(namebuf) / sizeof(*namebuf));

	while (cur < total) {
		/*
		 * For DIR_ITEM set index to (u64)-1, so that find_inode_ref
		 * ignore index check.
		 */
		if (di_key->type == BTRFS_DIR_INDEX_KEY)
			index = di_key->offset;
		else
			index = (u64)-1;

		data_len = btrfs_dir_data_len(node, di);
		tmp_err = 0;
		if (data_len)
			error("root %llu %s[%llu %llu] data_len shouldn't be %u",
			      root->objectid,
	      di_key->type == BTRFS_DIR_ITEM_KEY ? "DIR_ITEM" : "DIR_INDEX",
			      di_key->objectid, di_key->offset, data_len);

		name_len = btrfs_dir_name_len(node, di);
		if (name_len <= BTRFS_NAME_LEN) {
			len = name_len;
		} else {
			len = BTRFS_NAME_LEN;
			warning("root %llu %s[%llu %llu] name too long",
				root->objectid,
		di_key->type == BTRFS_DIR_ITEM_KEY ? "DIR_ITEM" : "DIR_INDEX",
				di_key->objectid, di_key->offset);
		}
		(*size) += name_len;
		read_extent_buffer(node, namebuf, (unsigned long)(di + 1),
				   len);
		filetype = btrfs_dir_type(node, di);

		if (di_key->type == BTRFS_DIR_ITEM_KEY &&
		    di_key->offset != btrfs_name_hash(namebuf, len)) {
			error("root %llu DIR_ITEM[%llu %llu] name %s namelen %u filetype %u mismatch with its hash, wanted %llu have %llu",
			root->objectid, di_key->objectid, di_key->offset,
			namebuf, len, filetype, di_key->offset,
			btrfs_name_hash(namebuf, len));
			tmp_err |= DIR_ITEM_HASH_MISMATCH;
			goto next;
		}

		btrfs_dir_item_key_to_cpu(node, di, &location);
		/* Ignore related ROOT_ITEM check */
		if (location.type == BTRFS_ROOT_ITEM_KEY)
			goto next;

		btrfs_release_path(path);
		/* Check relative INODE_ITEM(existence/filetype) */
		ret = btrfs_search_slot(NULL, root, &location, path, 0, 0);
		if (ret) {
			tmp_err |= INODE_ITEM_MISSING;
			goto next;
		}

		ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_inode_item);
		mode = btrfs_inode_mode(path->nodes[0], ii);
		if (imode_to_type(mode) != filetype) {
			tmp_err |= INODE_ITEM_MISMATCH;
			goto next;
		}

		/* Check relative INODE_REF/INODE_EXTREF */
		key.objectid = location.objectid;
		key.type = BTRFS_INODE_REF_KEY;
		key.offset = di_key->objectid;
		tmp_err |= find_inode_ref(root, &key, namebuf, len, &index);

		/* check relative INDEX/ITEM */
		key.objectid = di_key->objectid;
		if (key.type == BTRFS_DIR_ITEM_KEY) {
			key.type = BTRFS_DIR_INDEX_KEY;
			key.offset = index;
		} else {
			key.type = BTRFS_DIR_ITEM_KEY;
			key.offset = btrfs_name_hash(namebuf, name_len);
		}

		tmp_err |= find_dir_item(root, &key, &location, namebuf,
					 name_len, filetype);
		/* find_dir_item may find index */
		if (key.type == BTRFS_DIR_INDEX_KEY)
			index = key.offset;
next:

		if (tmp_err && opt_check_repair) {
			ret = repair_dir_item(root, di_key,
					      location.objectid, index,
					      imode_to_type(mode), namebuf,
					      name_len, tmp_err);
			if (ret != tmp_err) {
				need_research = 1;
				goto begin;
			}
		}
		btrfs_release_path(path);
		print_dir_item_err(root, di_key, location.objectid, index,
				   namebuf, name_len, filetype, tmp_err);
		err |= tmp_err;
		len = sizeof(*di) + name_len + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;

		if (di_key->type == BTRFS_DIR_INDEX_KEY && cur < total) {
			error("root %llu DIR_INDEX[%llu %llu] should contain only one entry",
			      root->objectid, di_key->objectid,
			      di_key->offset);
			break;
		}
	}
out:
	/* research path */
	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, di_key, path, 0, 0);
	if (ret)
		err |= ret > 0 ? -ENOENT : ret;
	return err;
}

/*
 * Wrapper function of btrfs_punch_hole.
 *
 * @path:	The path holder, will point to the same key after hole punching.
 *
 * Returns 0 means success.
 * Returns not 0 means error.
 */
static int punch_extent_hole(struct btrfs_root *root, struct btrfs_path *path,
			     u64 ino, u64 start, u64 len)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	int ret;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	ret = btrfs_punch_hole(trans, root, ino, start, len);
	if (ret) {
		error("failed to add hole [%llu, %llu] in inode [%llu]",
		      start, len, ino);
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	printf("Add a hole [%llu, %llu] in inode [%llu]\n", start, len, ino);
	btrfs_commit_transaction(trans, root);

	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret > 0)
		ret = -ENOENT;
	return ret;
}

static int repair_inline_ram_bytes(struct btrfs_root *root,
				   struct btrfs_path *path, u64 *ram_bytes_ret)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	u32 on_disk_data_len;
	int ret;
	int recover_ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		return ret;
	}
	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	/* Not really possible */
	if (ret > 0) {
		ret = -ENOENT;
		btrfs_release_path(path);
		goto recover;
	}
	if (ret < 0)
		goto recover;

	on_disk_data_len = btrfs_file_extent_inline_item_len(path->nodes[0],
							     path->slots[0]);

	fi = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_file_extent_item);
	if (btrfs_file_extent_type(path->nodes[0], fi) !=
			BTRFS_FILE_EXTENT_INLINE ||
	    btrfs_file_extent_compression(path->nodes[0], fi) !=
			BTRFS_COMPRESS_NONE)
		return -EINVAL;
	btrfs_set_file_extent_ram_bytes(path->nodes[0], fi, on_disk_data_len);
	btrfs_mark_buffer_dirty(path->nodes[0]);

	ret = btrfs_commit_transaction(trans, root);
	if (!ret) {
		printf(
	"Successfully repaired inline ram_bytes for root %llu ino %llu\n",
			root->objectid, key.objectid);
		*ram_bytes_ret = on_disk_data_len;
	}
	return ret;

recover:
	/*
	 * COW search failed, mostly due to the extra COW work (extent
	 * allocation, etc).  Since we have a good path from before, readonly
	 * search should still work, or later checks will fail due to empty
	 * path.
	 */
	recover_ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);

	/* This really shouldn't happen, or we have a big problem */
	ASSERT(recover_ret == 0);
	return ret;
}

static int check_file_extent_inline(struct btrfs_root *root,
				    struct btrfs_path *path, u64 *size,
				    u64 *end)
{
	u32 max_inline_extent_size = min_t(u32, gfs_info->sectorsize - 1,
				BTRFS_MAX_INLINE_DATA_SIZE(gfs_info));
	struct extent_buffer *node = path->nodes[0];
	struct btrfs_file_extent_item *fi;
	struct btrfs_key fkey;
	u64 extent_num_bytes;
	u32 item_inline_len;
	int ret;
	int compressed = 0;
	int err = 0;

	fi = btrfs_item_ptr(node, path->slots[0], struct btrfs_file_extent_item);
	item_inline_len = btrfs_file_extent_inline_item_len(node, path->slots[0]);
	extent_num_bytes = btrfs_file_extent_ram_bytes(node, fi);
	compressed = btrfs_file_extent_compression(node, fi);
	btrfs_item_key_to_cpu(node, &fkey, path->slots[0]);

	if (extent_num_bytes == 0) {
		error(
"root %llu EXTENT_DATA[%llu %llu] has empty inline extent",
				root->objectid, fkey.objectid, fkey.offset);
		err |= FILE_EXTENT_ERROR;
	}

	if (compressed) {
		if (extent_num_bytes > gfs_info->sectorsize) {
			error(
"root %llu EXTENT_DATA[%llu %llu] too large inline extent ram size, have %llu, max: %u",
				root->objectid, fkey.objectid, fkey.offset,
				extent_num_bytes, gfs_info->sectorsize - 1);
			err |= FILE_EXTENT_ERROR;
		}

		if (item_inline_len > max_inline_extent_size) {
			error(
"root %llu EXTENT_DATA[%llu %llu] too large inline extent on-disk size, have %u, max: %u",
				root->objectid, fkey.objectid, fkey.offset,
				item_inline_len, max_inline_extent_size);
			err |= FILE_EXTENT_ERROR;
		}
	} else {
		if (extent_num_bytes > max_inline_extent_size) {
			error(
"root %llu EXTENT_DATA[%llu %llu] too large inline extent size, have %llu, max: %u",
				root->objectid, fkey.objectid, fkey.offset,
				extent_num_bytes, max_inline_extent_size);
			err |= FILE_EXTENT_ERROR;
		}

		if (extent_num_bytes != item_inline_len) {
			error(
"root %llu EXTENT_DATA[%llu %llu] wrong inline size, have: %llu, expected: %u",
				root->objectid, fkey.objectid, fkey.offset,
				extent_num_bytes, item_inline_len);
			if (opt_check_repair) {
				ret = repair_inline_ram_bytes(root, path,
							      &extent_num_bytes);
				if (ret)
					err |= FILE_EXTENT_ERROR;
			} else {
				err |= FILE_EXTENT_ERROR;
			}
		}
	}
	*end += extent_num_bytes;
	*size += extent_num_bytes;

	return err;
}

/*
 * Check file extent datasum/hole, update the size of the file extents,
 * check and update the last offset of the file extent.
 *
 * @root:	the root of fs/file tree.
 * @nodatasum:	INODE_NODATASUM feature.
 * @size:	the sum of all EXTENT_DATA items size for this inode.
 * @end:	the offset of the last extent.
 *
 * Return 0 if no error occurred.
 */
static int check_file_extent(struct btrfs_root *root, struct btrfs_path *path,
			     unsigned int nodatasum, u64 isize, u64 *size,
			     u64 *end)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_key fkey;
	struct extent_buffer *node = path->nodes[0];
	u64 disk_bytenr;
	u64 disk_num_bytes;
	u64 extent_num_bytes;
	u64 extent_offset;
	u64 csum_found;		/* In byte size, sectorsize aligned */
	u64 search_start;	/* Logical range start we search for csum */
	u64 search_len;		/* Logical range len we search for csum */
	u64 gen;
	u64 super_gen;
	unsigned int extent_type;
	unsigned int is_hole;
	int slot = path->slots[0];
	int compressed = 0;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(node, &fkey, slot);
	fi = btrfs_item_ptr(node, slot, struct btrfs_file_extent_item);
	extent_type = btrfs_file_extent_type(node, fi);

	/* Check extent type */
	if (extent_type != BTRFS_FILE_EXTENT_REG &&
	    extent_type != BTRFS_FILE_EXTENT_PREALLOC &&
	    extent_type != BTRFS_FILE_EXTENT_INLINE) {
		err |= FILE_EXTENT_ERROR;
		error("root %llu EXTENT_DATA[%llu %llu] type bad",
		      root->objectid, fkey.objectid, fkey.offset);
		return err;
	}

	/* Check inline extent */
	if (extent_type == BTRFS_FILE_EXTENT_INLINE)
		return check_file_extent_inline(root, path, size, end);

	/* Check REG_EXTENT/PREALLOC_EXTENT */
	gen = btrfs_file_extent_generation(node, fi);
	disk_bytenr = btrfs_file_extent_disk_bytenr(node, fi);
	disk_num_bytes = btrfs_file_extent_disk_num_bytes(node, fi);
	extent_num_bytes = btrfs_file_extent_num_bytes(node, fi);
	extent_offset = btrfs_file_extent_offset(node, fi);
	compressed = btrfs_file_extent_compression(node, fi);
	is_hole = (disk_bytenr == 0) && (disk_num_bytes == 0);
	super_gen = btrfs_super_generation(gfs_info->super_copy);

	if (gen > super_gen + 1) {
		error(
		"invalid file extent generation, have %llu expect (0, %llu]",
			gen, super_gen + 1);
		err |= INVALID_GENERATION;
	}

	/*
	 * Check EXTENT_DATA csum
	 *
	 * For plain (uncompressed) extent, we should only check the range
	 * we're referring to, as it's possible that part of prealloc extent
	 * has been written, and has csum:
	 *
	 * |<--- Original large preallocated extent A ---->|
	 * |<- Prealloc File Extent ->|<- Regular Extent ->|
	 *	No csum				Has csum
	 *
	 * For compressed extent, we should check the whole range.
	 */
	if (!compressed) {
		search_start = disk_bytenr + extent_offset;
		search_len = extent_num_bytes;
	} else {
		search_start = disk_bytenr;
		search_len = disk_num_bytes;
	}
	ret = count_csum_range(search_start, search_len, &csum_found);
	if (csum_found > 0 && nodatasum) {
		err |= ODD_CSUM_ITEM;
		error("root %llu EXTENT_DATA[%llu %llu] nodatasum shouldn't have datasum",
		      root->objectid, fkey.objectid, fkey.offset);
	} else if (extent_type == BTRFS_FILE_EXTENT_REG && !nodatasum &&
		   !is_hole && (ret < 0 || csum_found < search_len)) {
		err |= CSUM_ITEM_MISSING;
		error("root %llu EXTENT_DATA[%llu %llu] csum missing, have: %llu, expected: %llu",
		      root->objectid, fkey.objectid, fkey.offset,
		      csum_found, search_len);
	} else if (extent_type == BTRFS_FILE_EXTENT_PREALLOC &&
		   csum_found > 0) {
		ret = check_prealloc_extent_written(disk_bytenr, disk_num_bytes);
		if (ret < 0)
			return ret;
		if (ret == 0) {
			err |= ODD_CSUM_ITEM;
			error(
"root %llu EXTENT_DATA[%llu %llu] prealloc shouldn't have csum, but has: %llu",
			      root->objectid, fkey.objectid, fkey.offset,
			      csum_found);
		}
	}

	/*
	 * Extra check for compressed extents:
	 * Btrfs doesn't allow NODATASUM and compressed extent co-exist, thus
	 * all compressed extents should have a checksum.
	 */
	if (compressed && csum_found < search_len) {
		error(
"root %llu EXTENT_DATA[%llu %llu] compressed extent must have csum, but only %llu bytes have, expect %llu",
		      root->objectid, fkey.objectid, fkey.offset, csum_found,
		      search_len);
		err |= CSUM_ITEM_MISSING;
	}
	if (compressed && nodatasum) {
		error(
"root %llu EXTENT_DATA[%llu %llu] is compressed, but inode flag doesn't allow it",
		      root->objectid, fkey.objectid, fkey.offset);
		err |= FILE_EXTENT_ERROR;
	}

	/* Check EXTENT_DATA hole */
	if (!no_holes && (fkey.offset < isize) && (*end != fkey.offset)) {
		if (opt_check_repair)
			ret = punch_extent_hole(root, path, fkey.objectid,
						*end, fkey.offset - *end);
		if (!opt_check_repair || ret) {
			err |= FILE_EXTENT_ERROR;
			error(
"root %llu EXTENT_DATA[%llu %llu] gap exists, expected: EXTENT_DATA[%llu %llu]",
				root->objectid, fkey.objectid, fkey.offset,
				fkey.objectid, *end);
		}
	}

	/*
	 * Don't update extent end beyond rounded up isize. As holes
	 * after isize is not considered as missing holes.
	 */
	*end = min(round_up(isize, gfs_info->sectorsize),
		   fkey.offset + extent_num_bytes);
	if (!is_hole)
		*size += extent_num_bytes;

	return err;
}

static int __count_dir_isize(struct btrfs_root *root, u64 ino, int type,
		u64 *size_ret)
{
	struct btrfs_key key;
	struct btrfs_path path;
	u32 len;
	struct btrfs_dir_item *di;
	int ret;
	int cur = 0;
	int total = 0;

	ASSERT(size_ret);
	*size_ret = 0;

	key.objectid = ino;
	key.type = type;
	key.offset = (u64)-1;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		ret = -EIO;
		goto out;
	}
	/* if found, go to spacial case */
	if (ret == 0)
		goto special_case;

loop:
	ret = btrfs_previous_item(root, &path, ino, type);

	if (ret) {
		ret = 0;
		goto out;
	}

special_case:
	di = btrfs_item_ptr(path.nodes[0], path.slots[0], struct btrfs_dir_item);
	cur = 0;
	total = btrfs_item_size(path.nodes[0], path.slots[0]);

	while (cur < total) {
		len = btrfs_dir_name_len(path.nodes[0], di);
		if (len > BTRFS_NAME_LEN)
			len = BTRFS_NAME_LEN;
		*size_ret += len;

		len += btrfs_dir_data_len(path.nodes[0], di);
		len += sizeof(*di);
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}
	goto loop;

out:
	btrfs_release_path(&path);
	return ret;
}

static int count_dir_isize(struct btrfs_root *root, u64 ino, u64 *size)
{
	u64 item_size;
	u64 index_size;
	int ret;

	ASSERT(size);
	ret = __count_dir_isize(root, ino, BTRFS_DIR_ITEM_KEY, &item_size);
	if (ret)
		goto out;

	ret = __count_dir_isize(root, ino, BTRFS_DIR_INDEX_KEY, &index_size);
	if (ret)
		goto out;

	*size = item_size + index_size;

out:
	if (ret)
		error("failed to count root %llu INODE[%llu] root size",
		      root->objectid, ino);
	return ret;
}

/*
 * Set inode item nbytes to @nbytes
 *
 * Returns  0     on success
 * Returns  != 0  on error
 */
static int repair_inode_nbytes_lowmem(struct btrfs_root *root,
				      struct btrfs_path *path,
				      u64 ino, u64 nbytes)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key research_key;
	int err = 0;
	int ret;

	btrfs_item_key_to_cpu(path->nodes[0], &research_key, path->slots[0]);

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		err |= ret;
		goto out;
	}

	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret) {
		err |= ret;
		goto fail;
	}

	ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_nbytes(path->nodes[0], ii, nbytes);
	btrfs_mark_buffer_dirty(path->nodes[0]);
fail:
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error("failed to set nbytes in inode %llu root %llu",
		      ino, root->root_key.objectid);
	else
		printf("Set nbytes in inode item %llu root %llu to %llu\n", ino,
		       root->root_key.objectid, nbytes);

	/* research path */
	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, &research_key, path, 0, 0);
	err |= ret;

	return err;
}

/*
 * Set directory inode isize to @isize.
 *
 * Returns 0     on success.
 * Returns != 0  on error.
 */
static int repair_dir_isize_lowmem(struct btrfs_root *root,
				   struct btrfs_path *path,
				   u64 ino, u64 isize)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key research_key;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(path->nodes[0], &research_key, path->slots[0]);

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		err |= ret;
		goto out;
	}

	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret) {
		err |= ret;
		goto fail;
	}

	ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_size(path->nodes[0], ii, isize);
	btrfs_mark_buffer_dirty(path->nodes[0]);
fail:
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error("failed to set isize in inode %llu root %llu",
		      ino, root->root_key.objectid);
	else
		printf("Set isize in inode %llu root %llu to %llu\n",
		       ino, root->root_key.objectid, isize);

	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, &research_key, path, 0, 0);
	err |= ret;

	return err;
}

/*
 * Wrapper function for btrfs_add_orphan_item().
 *
 * Returns 0     on success.
 * Returns != 0  on error.
 */
static int repair_inode_orphan_item_lowmem(struct btrfs_root *root,
					   struct btrfs_path *path, u64 ino)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key research_key;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(path->nodes[0], &research_key, path->slots[0]);

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		err |= ret;
		goto out;
	}

	btrfs_release_path(path);
	ret = btrfs_add_orphan_item(trans, root, path, ino);
	err |= ret;
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error("failed to add inode %llu as orphan item root %llu",
		      ino, root->root_key.objectid);
	else
		printf("Added inode %llu as orphan item root %llu\n",
		       ino, root->root_key.objectid);

	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, root, &research_key, path, 0, 0);
	err |= ret;

	return err;
}

/* Set inode_item nlink to @ref_count.
 * If @ref_count == 0, move it to "lost+found" and increase @ref_count.
 *
 * Returns 0 on success
 */
static int repair_inode_nlinks_lowmem(struct btrfs_root *root,
				      struct btrfs_path *path, u64 ino,
				      const char *name, u32 namelen,
				      u64 ref_count, u8 filetype, u64 *nlink)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key old_key;
	char namebuf[BTRFS_NAME_LEN] = {0};
	int name_len;
	int ret;
	int ret2;

	/* save the key */
	btrfs_item_key_to_cpu(path->nodes[0], &old_key, path->slots[0]);

	if (name && namelen) {
		ASSERT(namelen <= BTRFS_NAME_LEN);
		memcpy(namebuf, name, namelen);
		name_len = namelen;
	} else {
		sprintf(namebuf, "%llu", ino);
		name_len = count_digits(ino);
		printf("Can't find file name for inode %llu, use %s instead\n",
		       ino, namebuf);
	}

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto out;
	}

	btrfs_release_path(path);
	/* if refs is 0, put it into lostfound */
	if (ref_count == 0) {
		ret = link_inode_to_lostfound(trans, root, path, ino, namebuf,
					      name_len, filetype, &ref_count);
		if (ret)
			goto fail;
	}

	/* reset inode_item's nlink to ref_count */
	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	btrfs_release_path(path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret)
		goto fail;

	ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_nlink(path->nodes[0], ii, ref_count);
	btrfs_mark_buffer_dirty(path->nodes[0]);

	if (nlink)
		*nlink = ref_count;
fail:
	btrfs_commit_transaction(trans, root);
out:
	if (ret)
		error(
	"fail to repair nlink of inode %llu root %llu name %s filetype %u",
		       root->objectid, ino, namebuf, filetype);
	else
		printf("Fixed nlink of inode %llu root %llu name %s filetype %u\n",
		       root->objectid, ino, namebuf, filetype);

	/* research */
	btrfs_release_path(path);
	ret2 = btrfs_search_slot(NULL, root, &old_key, path, 0, 0);
	if (ret2 < 0)
		return ret |= ret2;
	return ret;
}

static bool has_orphan_item(struct btrfs_root *root, u64 ino)
{
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;

	btrfs_init_path(&path);
	key.objectid = BTRFS_ORPHAN_OBJECTID;
	key.type = BTRFS_ORPHAN_ITEM_KEY;
	key.offset = ino;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	btrfs_release_path(&path);
	if (ret == 0)
		return true;
	return false;
}

static int repair_inode_gen_lowmem(struct btrfs_root *root,
				   struct btrfs_path *path)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	u64 transid;
	int ret;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "inode gen repair: %m");
		return ret;
	}
	transid = trans->transid;
	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	ASSERT(key.type == BTRFS_INODE_ITEM_KEY);

	btrfs_release_path(path);

	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0) {
		ret = -ENOENT;
		error("no inode item found for ino %llu", key.objectid);
		goto error;
	}
	if (ret < 0) {
		errno = -ret;
		error("failed to find inode item for ino %llu: %m", key.objectid);
		goto error;
	}
	ii = btrfs_item_ptr(path->nodes[0], path->slots[0],
			    struct btrfs_inode_item);
	btrfs_set_inode_generation(path->nodes[0], ii, trans->transid);
	btrfs_set_inode_transid(path->nodes[0], ii, trans->transid);
	btrfs_mark_buffer_dirty(path->nodes[0]);
	ret = btrfs_commit_transaction(trans, root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		goto error;
	}
	printf("resetting inode generation/transid to %llu for ino %llu\n",
		transid, key.objectid);
	return ret;

error:
	btrfs_abort_transaction(trans, ret);
	return ret;
}

/*
 * Check INODE_ITEM and related ITEMs (the same inode number)
 * 1. check link count
 * 2. check inode ref/extref
 * 3. check dir item/index
 *
 * Return 0 if no error occurred.
 * Return >0 for error or hit the traversal is done(by error bitmap)
 */
static int check_inode_item(struct btrfs_root *root, struct btrfs_path *path)
{
	struct extent_buffer *node;
	struct btrfs_inode_item *ii;
	struct btrfs_key key;
	struct btrfs_key last_key;
	struct btrfs_super_block *super = gfs_info->super_copy;
	u64 inode_id;
	u32 mode;
	u64 flags;
	u64 nlink;
	u64 nbytes;
	u64 isize;
	u64 size = 0;
	u64 refs = 0;
	u64 extent_end = 0;
	u64 extent_size = 0;
	u64 generation;
	u64 transid;
	u64 gen_uplimit;
	unsigned int dir;
	unsigned int nodatasum;
	bool is_orphan = false;
	int slot;
	int ret;
	int err = 0;
	char namebuf[BTRFS_NAME_LEN] = {0};
	u32 name_len = 0;

	node = path->nodes[0];
	slot = path->slots[0];

	btrfs_item_key_to_cpu(node, &key, slot);
	inode_id = key.objectid;

	if (inode_id == BTRFS_ORPHAN_OBJECTID) {
		ret = btrfs_next_item(root, path);
		if (ret > 0)
			err |= LAST_ITEM;
		return err;
	}

	is_orphan = has_orphan_item(root, inode_id);
	ii = btrfs_item_ptr(node, slot, struct btrfs_inode_item);
	isize = btrfs_inode_size(node, ii);
	nbytes = btrfs_inode_nbytes(node, ii);
	mode = btrfs_inode_mode(node, ii);
	flags = btrfs_inode_flags(node, ii);
	dir = imode_to_type(mode) == BTRFS_FT_DIR;
	nlink = btrfs_inode_nlink(node, ii);
	generation = btrfs_inode_generation(node, ii);
	transid = btrfs_inode_transid(node, ii);
	nodatasum = btrfs_inode_flags(node, ii) & BTRFS_INODE_NODATASUM;

	if (!is_valid_imode(mode)) {
		error("invalid imode mode bits: 0%o", mode);
		if (opt_check_repair) {
			ret = repair_imode_common(root, path);
			if (ret < 0)
				err |= INODE_MODE_ERROR;
		} else {
			err |= INODE_MODE_ERROR;
		}
	}

	if (btrfs_super_log_root(super) != 0 &&
	    root->objectid == BTRFS_TREE_LOG_OBJECTID)
		gen_uplimit = btrfs_super_generation(super) + 1;
	else
		gen_uplimit = btrfs_super_generation(super);

	if (generation > gen_uplimit || transid > gen_uplimit) {
		error(
"invalid inode generation %llu or transid %llu for ino %llu, expect [0, %llu)",
		      generation, transid, inode_id, gen_uplimit);
		if (opt_check_repair) {
			ret = repair_inode_gen_lowmem(root, path);
			if (ret < 0)
				err |= INVALID_GENERATION;
		} else {
			err |= INVALID_GENERATION;
		}

	}
	if (S_ISLNK(mode) &&
	    flags & (BTRFS_INODE_IMMUTABLE | BTRFS_INODE_APPEND)) {
		err |= INODE_FLAGS_ERROR;
		error(
"symlinks must never have immutable/append flags set, root %llu inode item %llu flags %llu may be corrupted",
		      root->objectid, inode_id, flags);
	}

	while (1) {
		btrfs_item_key_to_cpu(path->nodes[0], &last_key, path->slots[0]);
		ret = btrfs_next_item(root, path);

		/*
		 * New leaf, we need to check it and see if it's valid, if not
		 * we need to bail otherwise we could end up stuck.
		 */
		if (path->slots[0] == 0 &&
		    btrfs_check_leaf(gfs_info, NULL, path->nodes[0]))
			ret = -EIO;

		if (ret < 0) {
			err |= FATAL_ERROR;
			goto out;
		} else if (ret > 0) {
			err |= LAST_ITEM;
			goto out;
		}

		node = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);
		if (key.objectid != inode_id)
			goto out;

		switch (key.type) {
		case BTRFS_INODE_REF_KEY:
			ret = check_inode_ref(root, &key, path, namebuf,
					      &name_len, &refs, mode);
			err |= ret;
			break;
		case BTRFS_INODE_EXTREF_KEY:
		{
			bool ext_ref = btrfs_fs_incompat(gfs_info,
							 EXTENDED_IREF);
			if (key.type == BTRFS_INODE_EXTREF_KEY && !ext_ref)
				warning("root %llu EXTREF[%llu %llu] isn't supported",
					root->objectid, key.objectid,
					key.offset);
			ret = check_inode_extref(root, &key, node, slot, &refs,
						 mode);
			err |= ret;
			break;
		}
		case BTRFS_DIR_ITEM_KEY:
		case BTRFS_DIR_INDEX_KEY:
			if (!dir) {
				warning("root %llu INODE[%llu] mode %u shouldn't have DIR_INDEX[%llu %llu]",
					root->objectid,	inode_id,
					imode_to_type(mode), key.objectid,
					key.offset);
			}
			if (is_orphan && key.type == BTRFS_DIR_INDEX_KEY)
				break;
			ret = check_dir_item(root, &key, path, &size);
			err |= ret;
			break;
		case BTRFS_EXTENT_DATA_KEY:
			if (dir) {
				warning("root %llu DIR INODE[%llu] shouldn't EXTENT_DATA[%llu %llu]",
					root->objectid, inode_id, key.objectid,
					key.offset);
			}
			ret = check_file_extent(root, path, nodatasum, isize,
						&extent_size, &extent_end);
			err |= ret;
			break;
		case BTRFS_XATTR_ITEM_KEY:
			break;
		default:
			error("ITEM[%llu %u %llu] UNKNOWN TYPE",
			      key.objectid, key.type, key.offset);
		}
	}

out:
	if (err & LAST_ITEM) {
		btrfs_release_path(path);
		ret = btrfs_search_slot(NULL, root, &last_key, path, 0, 0);
		if (ret)
			return err;
	}

	/* verify INODE_ITEM nlink/isize/nbytes */
	if (dir) {
		if (opt_check_repair && (err & DIR_COUNT_AGAIN)) {
			err &= ~DIR_COUNT_AGAIN;
			count_dir_isize(root, inode_id, &size);
		}

		if ((nlink != 1 || refs != 1) && opt_check_repair) {
			ret = repair_inode_nlinks_lowmem(root, path, inode_id,
				namebuf, name_len, refs, imode_to_type(mode),
				&nlink);
		}

		if (nlink > 1) {
			err |= LINK_COUNT_ERROR;
			error("root %llu DIR INODE[%llu] shouldn't have more than one link(%llu)",
			      root->objectid, inode_id, nlink);
		}

		/*
		 * Just a warning, as dir inode nbytes is just an
		 * instructive value.
		 */
		if (!IS_ALIGNED(nbytes, gfs_info->nodesize)) {
			warning("root %llu DIR INODE[%llu] nbytes should be aligned to %u",
				root->objectid, inode_id,
				gfs_info->nodesize);
		}

		if (isize != size && !is_orphan) {
			if (opt_check_repair)
				ret = repair_dir_isize_lowmem(root, path,
							      inode_id, size);
			if (!opt_check_repair || ret) {
				err |= ISIZE_ERROR;
				error(
		"root %llu DIR INODE [%llu] size %llu not equal to %llu",
				      root->objectid, inode_id, isize, size);
			}
		}
	} else {
		if (nlink != refs) {
			if (opt_check_repair)
				ret = repair_inode_nlinks_lowmem(root, path,
					 inode_id, namebuf, name_len, refs,
					 imode_to_type(mode), &nlink);
			if (!opt_check_repair || ret) {
				err |= LINK_COUNT_ERROR;
				error(
		"root %llu INODE[%llu] nlink(%llu) not equal to inode_refs(%llu)",
				      root->objectid, inode_id, nlink, refs);
			}
		} else if (!nlink && !is_orphan) {
			if (opt_check_repair)
				ret = repair_inode_orphan_item_lowmem(root,
							      path, inode_id);
			if (!opt_check_repair || ret) {
				err |= ORPHAN_ITEM;
				error("root %llu INODE[%llu] is orphan item",
				      root->objectid, inode_id);
			}
		}

		/*
		 * For orphan inode, updating nbytes/size is just a waste of
		 * time, so skip such repair and don't report them as error.
		 */
		if (nbytes != extent_size && !is_orphan) {
			if (opt_check_repair) {
				ret = repair_inode_nbytes_lowmem(root, path,
							 inode_id, extent_size);
				if (!ret)
					nbytes = extent_size;
			}
			if (!opt_check_repair || ret) {
				err |= NBYTES_ERROR;
				error(
	"root %llu INODE[%llu] nbytes %llu not equal to extent_size %llu",
				      root->objectid, inode_id, nbytes,
				      extent_size);
			}
		}

		if (!nbytes && !no_holes && extent_end < isize) {
			if (opt_check_repair)
				ret = punch_extent_hole(root, path, inode_id,
						extent_end, isize - extent_end);
			if (!opt_check_repair || ret) {
				err |= NBYTES_ERROR;
				error(
	"root %llu INODE[%llu] size %llu should have a file extent hole",
				      root->objectid, inode_id, isize);
			}
		}
	}

	if (err & LAST_ITEM)
		btrfs_next_item(root, path);
	return err;
}

/*
 * Returns >0  Found error, not fatal, should continue
 * Returns <0  Fatal error, must exit the whole check
 * Returns 0   No errors found
 */
static int process_one_leaf(struct btrfs_root *root, struct btrfs_path *path,
			    struct node_refs *nrefs, int *level)
{
	struct extent_buffer *cur = path->nodes[0];
	struct btrfs_key key;
	u64 cur_bytenr;
	u32 nritems;
	u64 first_ino = 0;
	int root_level = btrfs_header_level(root->node);
	int i;
	int ret = 0; /* Final return value */
	int err = 0; /* Positive error bitmap */

	cur_bytenr = cur->start;

	/* skip to first inode item or the first inode number change */
	nritems = btrfs_header_nritems(cur);
	for (i = 0; i < nritems; i++) {
		btrfs_item_key_to_cpu(cur, &key, i);
		if (i == 0)
			first_ino = key.objectid;
		if (key.type == BTRFS_INODE_ITEM_KEY ||
		    (first_ino && first_ino != key.objectid))
			break;
	}
	if (i == nritems) {
		path->slots[0] = nritems;
		return 0;
	}
	path->slots[0] = i;

again:
	err |= check_inode_item(root, path);

	/* modify cur since check_inode_item may change path */
	cur = path->nodes[0];

	if (err & LAST_ITEM || err & FATAL_ERROR)
		goto out;

	/* still have inode items in this leaf */
	if (cur->start == cur_bytenr)
		goto again;

	/*
	 * we have switched to another leaf, above nodes may
	 * have changed, here walk down the path, if a node
	 * or leaf is shared, check whether we can skip this
	 * node or leaf.
	 */
	for (i = root_level; i >= 0; i--) {
		if (path->nodes[i]->start == nrefs->bytenr[i])
			continue;

		ret = update_nodes_refs(root, path->nodes[i]->start,
				path->nodes[i], nrefs, i, 0);
		if (ret)
			goto out;

		if (!nrefs->need_check[i]) {
			*level += 1;
			break;
		}
	}

	for (i = 0; i < *level; i++) {
		free_extent_buffer(path->nodes[i]);
		path->nodes[i] = NULL;
	}
out:
	err &= ~LAST_ITEM;
	if (err && !ret)
		ret = err;
	return ret;
}

/*
 * @level           if @level == -1 means extent data item
 *                  else normal treeblock.
 */
static int should_check_extent_strictly(struct btrfs_root *root,
					struct node_refs *nrefs, int level)
{
	int root_level = btrfs_header_level(root->node);

	if (level > root_level || level < -1)
		return 1;
	if (level == root_level)
		return 1;
	/*
	 * if the upper node is marked full backref, it should contain shared
	 * backref of the parent (except owner == root->objectid).
	 */
	while (++level <= root_level)
		if (nrefs->refs[level] > 1)
			return 0;

	return 1;
}

static int check_extent_inline_ref(struct extent_buffer *eb,
		   struct btrfs_key *key, struct btrfs_extent_inline_ref *iref)
{
	int ret;
	u8 type = btrfs_extent_inline_ref_type(eb, iref);

	switch (type) {
	case BTRFS_TREE_BLOCK_REF_KEY:
	case BTRFS_EXTENT_DATA_REF_KEY:
	case BTRFS_SHARED_BLOCK_REF_KEY:
	case BTRFS_SHARED_DATA_REF_KEY:
		ret = 0;
		break;
	default:
		error("extent[%llu %u %llu] has unknown ref type: %d",
		      key->objectid, key->type, key->offset, type);
		ret = UNKNOWN_TYPE;
		break;
	}

	return ret;
}

/*
 * Check backrefs of a tree block given by @bytenr or @eb.
 *
 * @root:	the root containing the @bytenr or @eb
 * @eb:		tree block extent buffer, can be NULL
 * @bytenr:	bytenr of the tree block to search
 * @level:	tree level of the tree block
 * @owner:	owner of the tree block
 *
 * Return >0 for any error found and output error message
 * Return 0 for no error found
 */
static int check_tree_block_ref(struct btrfs_root *root,
				struct extent_buffer *eb, u64 bytenr,
				int level, u64 owner, struct node_refs *nrefs)
{
	struct btrfs_key key;
	struct btrfs_root *extent_root;
	struct btrfs_path path;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct extent_buffer *leaf;
	unsigned long end;
	unsigned long ptr;
	int slot;
	int skinny_level;
	int root_level = btrfs_header_level(root->node);
	int type;
	u32 nodesize = gfs_info->nodesize;
	u32 item_size;
	u64 offset;
	int found_ref = 0;
	int err = 0;
	int ret;
	int strict = 1;
	int parent = 0;

	btrfs_init_path(&path);
	key.objectid = bytenr;
	if (btrfs_fs_incompat(gfs_info, SKINNY_METADATA))
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

	/* Search for the backref in extent tree */
	extent_root = btrfs_extent_root(gfs_info, bytenr);
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0) {
		err |= BACKREF_MISSING;
		goto out;
	}
	ret = btrfs_previous_extent_item(extent_root, &path, bytenr);
	if (ret) {
		err |= BACKREF_MISSING;
		goto out;
	}

	leaf = path.nodes[0];
	slot = path.slots[0];
	btrfs_item_key_to_cpu(leaf, &key, slot);

	ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);

	if (key.type == BTRFS_METADATA_ITEM_KEY) {
		skinny_level = (int)key.offset;
		iref = (struct btrfs_extent_inline_ref *)(ei + 1);
	} else {
		struct btrfs_tree_block_info *info;

		info = (struct btrfs_tree_block_info *)(ei + 1);
		skinny_level = btrfs_tree_block_level(leaf, info);
		iref = (struct btrfs_extent_inline_ref *)(info + 1);
	}


	if (eb) {
		u64 header_gen;
		u64 extent_gen;

		/*
		 * Due to the feature of shared tree blocks, if the upper node
		 * is a fs root or shared node, the extent of checked node may
		 * not be updated until the next CoW.
		 */
		if (nrefs)
			strict = should_check_extent_strictly(root, nrefs,
					level);
		if (!(btrfs_extent_flags(leaf, ei) &
		      BTRFS_EXTENT_FLAG_TREE_BLOCK)) {
			error(
		"extent[%llu %u] backref type mismatch, missing bit: %llx",
				key.objectid, nodesize,
				BTRFS_EXTENT_FLAG_TREE_BLOCK);
			err = BACKREF_MISMATCH;
		}
		header_gen = btrfs_header_generation(eb);
		extent_gen = btrfs_extent_generation(leaf, ei);
		if (header_gen != extent_gen) {
			error(
	"extent[%llu %u] backref generation mismatch, wanted: %llu, have: %llu",
				key.objectid, nodesize, header_gen,
				extent_gen);
			err = BACKREF_MISMATCH;
		}
		if (level != skinny_level) {
			error(
			"extent[%llu %u] level mismatch, wanted: %u, have: %u",
				key.objectid, nodesize, level, skinny_level);
			err = BACKREF_MISMATCH;
		}
		if (!is_fstree(owner) && btrfs_extent_refs(leaf, ei) != 1) {
			error(
			"extent[%llu %u] is referred by other roots than %llu",
				key.objectid, nodesize, root->objectid);
			err = BACKREF_MISMATCH;
		}
	}

	/*
	 * Iterate the extent/metadata item to find the exact backref
	 */
	item_size = btrfs_item_size(leaf, slot);
	ptr = (unsigned long)iref;
	end = (unsigned long)ei + item_size;

	while (ptr < end) {
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		offset = btrfs_extent_inline_ref_offset(leaf, iref);

		ret = check_extent_inline_ref(leaf, &key, iref);
		if (ret) {
			err |= ret;
			break;
		}
		if (type == BTRFS_TREE_BLOCK_REF_KEY) {
			if (offset == root->objectid)
				found_ref = 1;
			if (!strict && owner == offset)
				found_ref = 1;
		} else if (type == BTRFS_SHARED_BLOCK_REF_KEY) {
			/*
			 * Backref of tree reloc root points to itself, no need
			 * to check backref any more.
			 *
			 * This may be an error of loop backref, but extent tree
			 * checker should have already handled it.
			 * Here we only need to avoid infinite iteration.
			 */
			if (offset == bytenr) {
				found_ref = 1;
			} else {
				/*
				 * Check if the backref points to valid
				 * referencer
				 */
				found_ref = !check_tree_block_ref(root, NULL,
						offset, level + 1, owner, NULL);
			}
		}

		if (found_ref)
			break;
		ptr += btrfs_extent_inline_ref_size(type);
	}

	/*
	 * Inlined extent item doesn't have what we need, check
	 * TREE_BLOCK_REF_KEY
	 */
	if (!found_ref) {
		btrfs_release_path(&path);
		key.objectid = bytenr;
		key.type = BTRFS_TREE_BLOCK_REF_KEY;
		key.offset = root->objectid;

		ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
		if (!ret)
			found_ref = 1;
	}
	/*
	 * Finally check SHARED BLOCK REF, any found will be good
	 * Here we're not doing comprehensive extent backref checking,
	 * only need to ensure there is some extent referring to this
	 * tree block.
	 */
	if (!found_ref) {
		btrfs_release_path(&path);
		key.objectid = bytenr;
		key.type = BTRFS_SHARED_BLOCK_REF_KEY;
		key.offset = (u64)-1;

		ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
		if (ret < 0) {
			err |= BACKREF_MISSING;
			goto out;
		}
		ret = btrfs_previous_extent_item(extent_root, &path, bytenr);
		if (ret) {
			err |= BACKREF_MISSING;
			goto out;
		}
		found_ref = 1;
	}
	if (!found_ref)
		err |= BACKREF_MISSING;
out:
	btrfs_release_path(&path);
	if (nrefs && strict &&
	    level < root_level && nrefs->full_backref[level + 1])
		parent = nrefs->bytenr[level + 1];
	if (eb && (err & BACKREF_MISSING))
		error(
	"extent[%llu %u] backref lost (owner: %llu, level: %u) %s %llu",
		      bytenr, nodesize, owner, level,
		      parent ? "parent" : "root",
		      parent ? parent : root->objectid);
	return err;
}

/*
 * If @err contains BYTES_UNALIGNED then delete the extent data item.
 * If @err contains BACKREF_MISSING then add extent of the
 * file_extent_data_item.
 *
 * Returns error bits after reapir.
 */
static int repair_extent_data_item(struct btrfs_root *root,
				   struct btrfs_path *pathp,
				   struct node_refs *nrefs,
				   int err)
{
	struct btrfs_trans_handle *trans = NULL;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key fi_key;
	struct btrfs_key key;
	struct btrfs_extent_item *ei;
	struct btrfs_path path;
	struct btrfs_root *extent_root;
	struct extent_buffer *eb;
	u64 size;
	u64 disk_bytenr;
	u64 num_bytes;
	u64 parent;
	u64 offset;
	u64 extent_offset;
	u64 file_offset;
	int generation;
	int slot;
	int need_insert = 0;
	int ret = 0;

	eb = pathp->nodes[0];
	slot = pathp->slots[0];
	btrfs_item_key_to_cpu(eb, &fi_key, slot);
	fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);

	if (btrfs_file_extent_type(eb, fi) == BTRFS_FILE_EXTENT_INLINE ||
	    btrfs_file_extent_disk_bytenr(eb, fi) == 0)
		return err;

	file_offset = fi_key.offset;
	generation = btrfs_file_extent_generation(eb, fi);
	disk_bytenr = btrfs_file_extent_disk_bytenr(eb, fi);
	num_bytes = btrfs_file_extent_disk_num_bytes(eb, fi);
	extent_offset = btrfs_file_extent_offset(eb, fi);
	offset = file_offset - extent_offset;

	if (nrefs->full_backref[0])
		parent = btrfs_header_bytenr(eb);
	else
		parent = 0;

	if (err & BYTES_UNALIGNED) {
		ret = delete_item(root, pathp);
		if (!ret)
			err = 0;
		goto out_no_release;
	}

	/* now repair only adds backref */
	if ((err & BACKREF_MISSING) == 0)
		return err;

	/* search extent item */
	key.objectid = disk_bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = num_bytes;

	btrfs_init_path(&path);
	extent_root = btrfs_extent_root(gfs_info, key.objectid);
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0) {
		ret = -EIO;
		goto out;
	}
	need_insert = ret;

	ret = avoid_extents_overwrite();
	if (ret)
		goto out;
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		trans = NULL;
		goto out;
	}
	/* insert an extent item */
	if (need_insert) {
		key.objectid = disk_bytenr;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = num_bytes;
		size = sizeof(*ei);

		btrfs_release_path(&path);
		ret = btrfs_insert_empty_item(trans, extent_root, &path, &key,
					      size);
		if (ret)
			goto out;
		eb = path.nodes[0];
		ei = btrfs_item_ptr(eb, path.slots[0], struct btrfs_extent_item);

		btrfs_set_extent_refs(eb, ei, 0);
		btrfs_set_extent_generation(eb, ei, generation);
		btrfs_set_extent_flags(eb, ei, BTRFS_EXTENT_FLAG_DATA);

		btrfs_mark_buffer_dirty(eb);
		ret = btrfs_update_block_group(trans, disk_bytenr, num_bytes,
					       1, 0);
		btrfs_release_path(&path);
	}

	ret = btrfs_inc_extent_ref(trans, root, disk_bytenr, num_bytes, parent,
				   root->objectid,
		   parent ? BTRFS_FIRST_FREE_OBJECTID : fi_key.objectid,
				   offset);
	if (ret) {
		error(
		"failed to increase extent data backref[%llu %llu] root %llu",
		      disk_bytenr, num_bytes, root->objectid);
		goto out;
	} else {
		printf("Add one extent data backref [%llu %llu]\n",
		       disk_bytenr, num_bytes);
	}

	err &= ~BACKREF_MISSING;
out:
	if (trans)
		btrfs_commit_transaction(trans, root);
	btrfs_release_path(&path);
out_no_release:
	if (ret)
		error("can't repair root %llu extent data item[%llu %llu]",
		      root->objectid, disk_bytenr, num_bytes);
	return err;
}

/*
 * Check EXTENT_DATA item, mainly for its dbackref in extent tree
 *
 * Return >0 any error found and output error message
 * Return 0 for no error found
 */
static int check_extent_data_item(struct btrfs_root *root,
				  struct btrfs_path *pathp,
				  struct node_refs *nrefs,  int account_bytes)
{
	struct btrfs_file_extent_item *fi;
	struct extent_buffer *eb = pathp->nodes[0];
	struct btrfs_path path;
	struct btrfs_root *extent_root;
	struct btrfs_key fi_key;
	struct btrfs_key dbref_key;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	u64 owner;
	u64 disk_bytenr;
	u64 disk_num_bytes;
	u64 extent_num_bytes;
	u64 extent_flags;
	u64 offset;
	u32 item_size;
	unsigned long end;
	unsigned long ptr;
	int type;
	int found_dbackref = 0;
	int slot = pathp->slots[0];
	int err = 0;
	int ret;
	int strict;

	btrfs_item_key_to_cpu(eb, &fi_key, slot);
	fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);

	/* Nothing to check for hole and inline data extents */
	if (btrfs_file_extent_type(eb, fi) == BTRFS_FILE_EXTENT_INLINE ||
	    btrfs_file_extent_disk_bytenr(eb, fi) == 0)
		return 0;

	disk_bytenr = btrfs_file_extent_disk_bytenr(eb, fi);
	disk_num_bytes = btrfs_file_extent_disk_num_bytes(eb, fi);
	extent_num_bytes = btrfs_file_extent_num_bytes(eb, fi);
	offset = btrfs_file_extent_offset(eb, fi);

	/* Check unaligned disk_bytenr, disk_num_bytes and num_bytes */
	if (!IS_ALIGNED(disk_bytenr, gfs_info->sectorsize)) {
		error(
"file extent [%llu, %llu] has unaligned disk bytenr: %llu, should be aligned to %u",
			fi_key.objectid, fi_key.offset, disk_bytenr,
			gfs_info->sectorsize);
		err |= BYTES_UNALIGNED;
	}
	if (!IS_ALIGNED(disk_num_bytes, gfs_info->sectorsize)) {
		error(
"file extent [%llu, %llu] has unaligned disk num bytes: %llu, should be aligned to %u",
			fi_key.objectid, fi_key.offset, disk_num_bytes,
			gfs_info->sectorsize);
		err |= BYTES_UNALIGNED;
	} else if (account_bytes) {
		data_bytes_allocated += disk_num_bytes;
	}
	if (!IS_ALIGNED(extent_num_bytes, gfs_info->sectorsize)) {
		error(
"file extent [%llu, %llu] has unaligned num bytes: %llu, should be aligned to %u",
			fi_key.objectid, fi_key.offset, extent_num_bytes,
			gfs_info->sectorsize);
		err |= BYTES_UNALIGNED;
	} else if (account_bytes) {
		data_bytes_referenced += extent_num_bytes;
	}
	owner = btrfs_header_owner(eb);

	/* Check the extent item of the file extent in extent tree */
	btrfs_init_path(&path);
	dbref_key.objectid = btrfs_file_extent_disk_bytenr(eb, fi);
	dbref_key.type = BTRFS_EXTENT_ITEM_KEY;
	dbref_key.offset = btrfs_file_extent_disk_num_bytes(eb, fi);

	extent_root = btrfs_extent_root(gfs_info, dbref_key.objectid);
	ret = btrfs_search_slot(NULL, extent_root, &dbref_key, &path, 0, 0);
	if (ret)
		goto out;

	leaf = path.nodes[0];
	slot = path.slots[0];
	ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);

	extent_flags = btrfs_extent_flags(leaf, ei);

	if (!(extent_flags & BTRFS_EXTENT_FLAG_DATA)) {
		error(
"file extent[%llu %llu] root %llu owner %llu backref type mismatch, wanted bit: %llx",
			fi_key.objectid, fi_key.offset, root->objectid, owner,
			BTRFS_EXTENT_FLAG_DATA);
		err |= BACKREF_MISMATCH;
	}

	/* Check data backref inside that extent item */
	item_size = btrfs_item_size(leaf, path.slots[0]);
	iref = (struct btrfs_extent_inline_ref *)(ei + 1);
	ptr = (unsigned long)iref;
	end = (unsigned long)ei + item_size;
	strict = should_check_extent_strictly(root, nrefs, -1);

	while (ptr < end) {
		u64 ref_root;
		u64 ref_objectid;
		u64 ref_offset;
		bool match = false;

		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		dref = (struct btrfs_extent_data_ref *)(&iref->offset);

		ret = check_extent_inline_ref(leaf, &dbref_key, iref);
		if (ret) {
			err |= ret;
			break;
		}
		if (type == BTRFS_EXTENT_DATA_REF_KEY) {
			ref_root = btrfs_extent_data_ref_root(leaf, dref);
			ref_objectid = btrfs_extent_data_ref_objectid(leaf,
								      dref);
			ref_offset = btrfs_extent_data_ref_offset(leaf, dref);

			if (ref_objectid == fi_key.objectid &&
			    ref_offset == fi_key.offset - offset)
				match = true;
			if (ref_root == root->objectid && match)
				found_dbackref = 1;
			else if (!strict && owner == ref_root && match)
				found_dbackref = 1;
		} else if (type == BTRFS_SHARED_DATA_REF_KEY) {
			found_dbackref = !check_tree_block_ref(root, NULL,
				btrfs_extent_inline_ref_offset(leaf, iref),
				0, owner, NULL);
		}

		if (found_dbackref)
			break;
		ptr += btrfs_extent_inline_ref_size(type);
	}

	if (!found_dbackref) {
		btrfs_release_path(&path);

		/* Didn't find inlined data backref, try EXTENT_DATA_REF_KEY */
		dbref_key.objectid = btrfs_file_extent_disk_bytenr(eb, fi);
		dbref_key.type = BTRFS_EXTENT_DATA_REF_KEY;
		dbref_key.offset = hash_extent_data_ref(owner, fi_key.objectid,
							fi_key.offset - offset);

		extent_root = btrfs_extent_root(gfs_info, dbref_key.objectid);
		ret = btrfs_search_slot(NULL, extent_root, &dbref_key, &path, 0,
					0);
		if (!ret) {
			found_dbackref = 1;
			goto out;
		}

		btrfs_release_path(&path);

		/*
		 * Neither inlined nor EXTENT_DATA_REF found, try
		 * SHARED_DATA_REF as last chance.
		 */
		dbref_key.objectid = disk_bytenr;
		dbref_key.type = BTRFS_SHARED_DATA_REF_KEY;
		dbref_key.offset = eb->start;

		ret = btrfs_search_slot(NULL, extent_root, &dbref_key, &path, 0,
					0);
		if (!ret) {
			found_dbackref = 1;
			goto out;
		}
	}

out:
	if (!found_dbackref)
		err |= BACKREF_MISSING;
	btrfs_release_path(&path);
	if (err & BACKREF_MISSING) {
		error(
		"file extent[%llu %llu] root %llu owner %llu backref lost",
			fi_key.objectid, fi_key.offset, root->objectid, owner);
	}
	return err;
}

/*
 * Check a block group item with its referener (chunk) and its used space
 * with extent/metadata item
 */
static int check_block_group_item(struct extent_buffer *eb, int slot)
{
	struct btrfs_root *extent_root;
	struct btrfs_root *chunk_root = gfs_info->chunk_root;
	struct btrfs_block_group_item *bi;
	struct btrfs_block_group_item bg_item;
	struct btrfs_path path;
	struct btrfs_key bg_key;
	struct btrfs_key chunk_key;
	struct btrfs_key extent_key;
	struct btrfs_chunk *chunk;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	u32 nodesize = btrfs_super_nodesize(gfs_info->super_copy);
	u64 flags;
	u64 bg_flags;
	u64 used;
	u64 total = 0;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(eb, &bg_key, slot);
	bi = btrfs_item_ptr(eb, slot, struct btrfs_block_group_item);
	read_extent_buffer(eb, &bg_item, (unsigned long)bi, sizeof(bg_item));
	used = btrfs_stack_block_group_used(&bg_item);
	bg_flags = btrfs_stack_block_group_flags(&bg_item);

	chunk_key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	chunk_key.type = BTRFS_CHUNK_ITEM_KEY;
	chunk_key.offset = bg_key.objectid;

	btrfs_init_path(&path);
	/* Search for the referencer chunk */
	ret = btrfs_search_slot(NULL, chunk_root, &chunk_key, &path, 0, 0);
	if (ret) {
		error(
		"block group[%llu %llu] did not find the related chunk item",
			bg_key.objectid, bg_key.offset);
		err |= REFERENCER_MISSING;
	} else {
		chunk = btrfs_item_ptr(path.nodes[0], path.slots[0],
					struct btrfs_chunk);
		if (btrfs_chunk_length(path.nodes[0], chunk) !=
						bg_key.offset) {
			error(
	"block group[%llu %llu] related chunk item length does not match",
				bg_key.objectid, bg_key.offset);
			err |= REFERENCER_MISMATCH;
		}
	}
	btrfs_release_path(&path);

	/* Search from the block group bytenr */
	extent_key.objectid = bg_key.objectid;
	extent_key.type = 0;
	extent_key.offset = 0;

	btrfs_init_path(&path);
	extent_root = btrfs_extent_root(gfs_info, extent_key.objectid);
	ret = btrfs_search_slot(NULL, extent_root, &extent_key, &path, 0, 0);
	if (ret < 0)
		goto out;

	/* Iterate extent tree to account used space */
	while (1) {
		leaf = path.nodes[0];

		/* Search slot can point to the last item beyond leaf nritems */
		if (path.slots[0] >= btrfs_header_nritems(leaf))
			goto next;

		btrfs_item_key_to_cpu(leaf, &extent_key, path.slots[0]);
		if (extent_key.objectid >= bg_key.objectid + bg_key.offset)
			break;

		if (extent_key.type != BTRFS_METADATA_ITEM_KEY &&
		    extent_key.type != BTRFS_EXTENT_ITEM_KEY)
			goto next;
		if (extent_key.objectid < bg_key.objectid)
			goto next;

		if (extent_key.type == BTRFS_METADATA_ITEM_KEY)
			total += nodesize;
		else
			total += extent_key.offset;

		ei = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_extent_item);
		flags = btrfs_extent_flags(leaf, ei);
		if (flags & BTRFS_EXTENT_FLAG_DATA) {
			if (!(bg_flags & BTRFS_BLOCK_GROUP_DATA)) {
				error(
			"bad extent[%llu, %llu) type mismatch with chunk",
				      extent_key.objectid,
				      extent_key.objectid + extent_key.offset);
				err |= CHUNK_TYPE_MISMATCH;
			}
		} else if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
			if (!(bg_flags & (BTRFS_BLOCK_GROUP_SYSTEM |
				    BTRFS_BLOCK_GROUP_METADATA))) {
				error(
			"bad extent[%llu, %llu) type mismatch with chunk",
					extent_key.objectid,
					extent_key.objectid + nodesize);
				err |= CHUNK_TYPE_MISMATCH;
			}
		}
next:
		ret = btrfs_next_item(extent_root, &path);
		if (ret)
			break;
	}

out:
	btrfs_release_path(&path);

	total_used += used;

	if (total != used) {
		error(
		"block group[%llu %llu] used %llu but extent items used %llu",
			bg_key.objectid, bg_key.offset, used, total);
		err |= BG_ACCOUNTING_ERROR;
	}
	return err;
}

/*
 * Get real tree block level for the case like shared block
 * Return >= 0 as tree level
 * Return <0 for error
 */
static int query_tree_block_level(u64 bytenr)
{
	struct btrfs_root *extent_root;
	struct extent_buffer *eb;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_extent_item *ei;
	u64 flags;
	u64 transid;
	u8 backref_level;
	u8 header_level;
	int ret;

	/* Search extent tree for extent generation and level */
	key.objectid = bytenr;
	key.type = BTRFS_METADATA_ITEM_KEY;
	key.offset = (u64)-1;

	btrfs_init_path(&path);

	extent_root = btrfs_extent_root(gfs_info, bytenr);
	ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
	if (ret < 0)
		goto release_out;
	ret = btrfs_previous_extent_item(extent_root, &path, bytenr);
	if (ret < 0)
		goto release_out;
	if (ret > 0) {
		ret = -ENOENT;
		goto release_out;
	}

	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	ei = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_extent_item);
	flags = btrfs_extent_flags(path.nodes[0], ei);
	if (!(flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)) {
		ret = -ENOENT;
		goto release_out;
	}

	/* Get transid for later read_tree_block() check */
	transid = btrfs_extent_generation(path.nodes[0], ei);

	/* Get backref level as one source */
	if (key.type == BTRFS_METADATA_ITEM_KEY) {
		backref_level = key.offset;
	} else {
		struct btrfs_tree_block_info *info;

		info = (struct btrfs_tree_block_info *)(ei + 1);
		backref_level = btrfs_tree_block_level(path.nodes[0], info);
	}
	btrfs_release_path(&path);

	/* Get level from tree block as an alternative source */
	eb = read_tree_block(gfs_info, bytenr, transid);
	if (!extent_buffer_uptodate(eb)) {
		free_extent_buffer(eb);
		return -EIO;
	}
	header_level = btrfs_header_level(eb);
	free_extent_buffer(eb);

	if (header_level != backref_level)
		return -EIO;
	return header_level;

release_out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Check if a tree block backref is valid (points to a valid tree block)
 * if level == -1, level will be resolved
 * Return >0 for any error found and print error message
 */
static int check_tree_block_backref(u64 root_id, u64 bytenr, int level)
{
	struct btrfs_root *root;
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *eb;
	struct extent_buffer *node;
	u32 nodesize = btrfs_super_nodesize(gfs_info->super_copy);
	int err = 0;
	int ret;

	/* Query level for level == -1 special case */
	if (level == -1)
		level = query_tree_block_level(bytenr);
	if (level < 0) {
		err |= REFERENCER_MISSING;
		goto out;
	}

	key.objectid = root_id;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	root = btrfs_read_fs_root(gfs_info, &key);
	if (IS_ERR(root)) {
		err |= REFERENCER_MISSING;
		goto out;
	}

	/* Read out the tree block to get item/node key */
	eb = read_tree_block(gfs_info, bytenr, 0);
	if (!extent_buffer_uptodate(eb)) {
		err |= REFERENCER_MISSING;
		free_extent_buffer(eb);
		goto out;
	}

	/* Empty tree, no need to check key */
	if (!btrfs_header_nritems(eb) && !level) {
		free_extent_buffer(eb);
		goto out;
	}

	if (level)
		btrfs_node_key_to_cpu(eb, &key, 0);
	else
		btrfs_item_key_to_cpu(eb, &key, 0);

	free_extent_buffer(eb);

	btrfs_init_path(&path);
	path.lowest_level = level;
	/* Search with the first key, to ensure we can reach it */
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		err |= REFERENCER_MISSING;
		goto release_out;
	}

	node = path.nodes[level];
	if (btrfs_header_bytenr(node) != bytenr) {
		error(
	"extent [%llu %d] referencer bytenr mismatch, wanted: %llu, have: %llu",
			bytenr, nodesize, bytenr,
			btrfs_header_bytenr(node));
		err |= REFERENCER_MISMATCH;
	}
	if (btrfs_header_level(node) != level) {
		error(
	"extent [%llu %d] referencer level mismatch, wanted: %d, have: %d",
			bytenr, nodesize, level,
			btrfs_header_level(node));
		err |= REFERENCER_MISMATCH;
	}

release_out:
	btrfs_release_path(&path);
out:
	if (err & REFERENCER_MISSING) {
		if (level < 0)
			error("extent [%llu %d] lost referencer (owner: %llu)",
				bytenr, nodesize, root_id);
		else
			error(
		"extent [%llu %d] lost referencer (owner: %llu, level: %u)",
				bytenr, nodesize, root_id, level);
	}

	return err;
}

/*
 * Check if tree block @eb is tree reloc root.
 * Return 0 if it's not or any problem happens
 * Return 1 if it's a tree reloc root
 */
static int is_tree_reloc_root(struct extent_buffer *eb)
{
	struct btrfs_root *tree_reloc_root;
	struct btrfs_key key;
	u64 bytenr = btrfs_header_bytenr(eb);
	u64 owner = btrfs_header_owner(eb);
	int ret = 0;

	key.objectid = BTRFS_TREE_RELOC_OBJECTID;
	key.offset = owner;
	key.type = BTRFS_ROOT_ITEM_KEY;

	tree_reloc_root = btrfs_read_fs_root_no_cache(gfs_info, &key);
	if (IS_ERR(tree_reloc_root))
		return 0;

	if (bytenr == btrfs_header_bytenr(tree_reloc_root->node))
		ret = 1;
	btrfs_free_fs_root(tree_reloc_root);
	return ret;
}

/*
 * Check referencer for shared block backref
 * If level == -1, this function will resolve the level.
 */
static int check_shared_block_backref(u64 parent, u64 bytenr, int level)
{
	struct extent_buffer *eb;
	u32 nr;
	int found_parent = 0;
	int i;

	eb = read_tree_block(gfs_info, parent, 0);
	if (!extent_buffer_uptodate(eb))
		goto out;

	if (level == -1)
		level = query_tree_block_level(bytenr);
	if (level < 0)
		goto out;

	/* It's possible it's a tree reloc root */
	if (parent == bytenr) {
		if (is_tree_reloc_root(eb))
			found_parent = 1;
		goto out;
	}

	if (level + 1 != btrfs_header_level(eb))
		goto out;

	nr = btrfs_header_nritems(eb);
	for (i = 0; i < nr; i++) {
		if (bytenr == btrfs_node_blockptr(eb, i)) {
			found_parent = 1;
			break;
		}
	}
out:
	free_extent_buffer(eb);
	if (!found_parent) {
		error(
	"shared extent[%llu %u] lost its parent (parent: %llu, level: %u)",
			bytenr, gfs_info->nodesize, parent, level);
		return REFERENCER_MISSING;
	}
	return 0;
}

/*
 * Check referencer for normal (inlined) data ref
 * If len == 0, it will be resolved by searching in extent tree
 */
static int check_extent_data_backref(u64 root_id, u64 objectid, u64 offset,
				     u64 bytenr, u64 len, u32 count)
{
	struct btrfs_root *root;
	struct btrfs_root *extent_root;
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	u32 found_count = 0;
	int slot;
	int ret = 0;

	if (!len) {
		key.objectid = bytenr;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = (u64)-1;

		btrfs_init_path(&path);
		extent_root = btrfs_extent_root(gfs_info, bytenr);
		ret = btrfs_search_slot(NULL, extent_root, &key, &path, 0, 0);
		if (ret < 0)
			goto out;
		ret = btrfs_previous_extent_item(extent_root, &path, bytenr);
		if (ret)
			goto out;
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.objectid != bytenr ||
		    key.type != BTRFS_EXTENT_ITEM_KEY)
			goto out;
		len = key.offset;
		btrfs_release_path(&path);
	}
	key.objectid = root_id;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	btrfs_init_path(&path);

	root = btrfs_read_fs_root(gfs_info, &key);
	if (IS_ERR(root))
		goto out;

	key.objectid = objectid;
	key.type = BTRFS_EXTENT_DATA_KEY;
	/*
	 * It can be nasty as data backref offset is
	 * file offset - file extent offset, which is smaller or
	 * equal to original backref offset.  The only special case is
	 * overflow.  So we need to special check and do further search.
	 */
	key.offset = offset & (1ULL << 63) ? 0 : offset;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;

	/*
	 * Search afterwards to get correct one
	 * NOTE: As we must do a comprehensive check on the data backref to
	 * make sure the dref count also matches, we must iterate all file
	 * extents for that inode.
	 */
	while (1) {
		leaf = path.nodes[0];
		slot = path.slots[0];

		if (slot >= btrfs_header_nritems(leaf) ||
		    btrfs_header_owner(leaf) != root_id)
			goto next;
		/*
		 * For tree blocks have been relocated, data backref are
		 * shared instead of keyed. Do not account it.
		 */
		if (btrfs_header_flag(leaf, BTRFS_HEADER_FLAG_RELOC)) {
			/*
			 * skip the leaf to speed up.
			 */
			slot = btrfs_header_nritems(leaf);
			goto next;
		}

		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid != objectid ||
		    key.type != BTRFS_EXTENT_DATA_KEY)
			break;
		fi = btrfs_item_ptr(leaf, slot, struct btrfs_file_extent_item);
		/*
		 * Except normal disk bytenr and disk num bytes, we still
		 * need to do extra check on dbackref offset as
		 * dbackref offset = file_offset - file_extent_offset
		 *
		 * Also, we must check the leaf owner.
		 * In case of shared tree blocks (snapshots) we can inherit
		 * leaves from source snapshot.
		 * In that case, reference from source snapshot should not
		 * count.
		 */
		if (btrfs_file_extent_disk_bytenr(leaf, fi) == bytenr &&
		    btrfs_file_extent_disk_num_bytes(leaf, fi) == len &&
		    (u64)(key.offset - btrfs_file_extent_offset(leaf, fi)) ==
		    offset && btrfs_header_owner(leaf) == root_id)
			found_count++;

next:
		ret = btrfs_next_item(root, &path);
		if (ret)
			break;
	}
out:
	btrfs_release_path(&path);
	if (found_count != count) {
		error(
"extent[%llu, %llu] referencer count mismatch (root: %llu, owner: %llu, offset: %llu) wanted: %u, have: %u",
			bytenr, len, root_id, objectid, offset, count,
			found_count);
		return REFERENCER_MISSING;
	}
	return 0;
}

/*
 * Check if the referencer of a shared data backref exists
 */
static int check_shared_data_backref(u64 parent, u64 bytenr)
{
	struct extent_buffer *eb;
	struct btrfs_key key;
	struct btrfs_file_extent_item *fi;
	u32 nr;
	int found_parent = 0;
	int i;

	eb = read_tree_block(gfs_info, parent, 0);
	if (!extent_buffer_uptodate(eb))
		goto out;

	nr = btrfs_header_nritems(eb);
	for (i = 0; i < nr; i++) {
		btrfs_item_key_to_cpu(eb, &key, i);
		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;

		fi = btrfs_item_ptr(eb, i, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(eb, fi) == BTRFS_FILE_EXTENT_INLINE)
			continue;

		if (btrfs_file_extent_disk_bytenr(eb, fi) == bytenr) {
			found_parent = 1;
			break;
		}
	}

out:
	free_extent_buffer(eb);
	if (!found_parent) {
		error("shared extent %llu referencer lost (parent: %llu)",
			bytenr, parent);
		return REFERENCER_MISSING;
	}
	return 0;
}

/*
 * Only delete backref if REFERENCER_MISSING or REFERENCER_MISMATCH.
 *
 * Returns <0   error
 * Returns >0   the backref was deleted but extent still exists
 * Returns =0   the whole extent item was deleted
 */
static int repair_extent_item(struct btrfs_path *path, u64 bytenr, u64
			      num_bytes, u64 parent, u64 root_objectid, u64
			      owner, u64 offset)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key old_key;
	struct btrfs_root *extent_root = btrfs_extent_root(gfs_info, bytenr);
	int ret;

	btrfs_item_key_to_cpu(path->nodes[0], &old_key, path->slots[0]);

	ret = avoid_extents_overwrite();
	if (ret)
		return ret;

	trans = btrfs_start_transaction(extent_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		goto out;
	}
	/* delete the backref */
	ret = btrfs_free_extent(trans, gfs_info->fs_root, bytenr,
			num_bytes, parent, root_objectid, owner, offset);
	if (!ret)
		printf("Delete backref in extent [%llu %llu]\n",
		       bytenr, num_bytes);
	else {
		error("fail to delete backref in extent [%llu %llu]",
		      bytenr, num_bytes);
		btrfs_abort_transaction(trans, ret);
		goto out;
	}
	btrfs_commit_transaction(trans, extent_root);

	btrfs_release_path(path);
	ret = btrfs_search_slot(NULL, extent_root, &old_key, path, 0, 0);
	if (ret > 0) {
		/* odd, there must be one block group before at least */
		if (path->slots[0] == 0) {
			ret = -EUCLEAN;
			goto out;
		}
		/*
		 * btrfs_free_extent() has deleted the extent item,
		 * let path point to last checked item.
		 */
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0]))
			path->slots[0] = btrfs_header_nritems(path->nodes[0]) - 1;
		else
			path->slots[0]--;

		ret = 0;
	} else if (ret == 0) {
		ret = 1;
	}

out:
	return ret;
}

/*
 * Reset generation for extent item specified by @path.
 * Will try to grab the proper generation number from other sources, but if
 * it fails, then use current transid as fallback.
 *
 * Returns < 0 for error.
 * Return 0 if the generation is reset.
 */
static int repair_extent_item_generation(struct btrfs_path *path)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	struct btrfs_extent_item *ei;
	struct btrfs_root *extent_root;
	u64 new_gen = 0;;
	int ret;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	ASSERT(key.type == BTRFS_METADATA_ITEM_KEY ||
	       key.type == BTRFS_EXTENT_ITEM_KEY);

	get_extent_item_generation(key.objectid, &new_gen);
	ret = avoid_extents_overwrite();
	if (ret)
		return ret;
	btrfs_release_path(path);
	extent_root = btrfs_extent_root(gfs_info, key.objectid);
	trans = btrfs_start_transaction(extent_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}
	ret = btrfs_search_slot(trans, extent_root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0) {
		errno = -ret;
		error("failed to locate extent item for %llu: %m", key.objectid);
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	if (!new_gen)
		new_gen = trans->transid;
	ei = btrfs_item_ptr(path->nodes[0], path->slots[0], struct btrfs_extent_item);
	btrfs_set_extent_generation(path->nodes[0], ei, new_gen);
	ret = btrfs_commit_transaction(trans, extent_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	printf("Reset extent item (%llu) generation to %llu\n",
		key.objectid, new_gen);
	return ret;
}

/*
 * This function will check a given extent item, including its backref and
 * itself (like crossing stripe boundary and type)
 *
 * Since we don't use extent_record anymore, introduce new error bit
 */
static int check_extent_item(struct btrfs_path *path)
{
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	struct extent_buffer *eb = path->nodes[0];
	unsigned long ptr;
	int slot = path->slots[0];
	int type;
	u32 nodesize = btrfs_super_nodesize(gfs_info->super_copy);
	u32 item_size = btrfs_item_size(eb, slot);
	u64 flags;
	u64 offset;
	u64 parent;
	u64 num_bytes;
	u64 root_objectid;
	u64 gen;
	u64 owner;
	u64 owner_offset;
	u64 super_gen;
	int metadata = 0;
	/* To handle corrupted values in skinny backref */
	u64 level;
	struct btrfs_key key;
	int ret;
	int err = 0;
	int tmp_err = 0;
	u32 ptr_offset;

	btrfs_item_key_to_cpu(eb, &key, slot);
	if (key.type == BTRFS_EXTENT_ITEM_KEY) {
		bytes_used += key.offset;
		num_bytes = key.offset;
	} else {
		bytes_used += nodesize;
		num_bytes = nodesize;
	}

	if (item_size < sizeof(*ei)) {
		/*
		 * COMPAT_EXTENT_TREE_V0 case, but it's already a super
		 * old thing when on disk format is still un-determined.
		 * No need to care about it anymore
		 */
		error("unsupported COMPAT_EXTENT_TREE_V0 detected");
		return -ENOTTY;
	}

	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
	flags = btrfs_extent_flags(eb, ei);
	gen = btrfs_extent_generation(eb, ei);
	super_gen = btrfs_super_generation(gfs_info->super_copy);
	if (gen > super_gen + 1) {
		error(
		"invalid generation for extent %llu, have %llu expect (0, %llu]",
			key.objectid, gen, super_gen + 1);
		tmp_err |= INVALID_GENERATION;
	}

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
		metadata = 1;
	if (metadata && check_crossing_stripes(gfs_info, key.objectid,
					       eb->len)) {
		error("bad metadata [%llu, %llu) crossing stripe boundary",
		      key.objectid, key.objectid + nodesize);
		err |= CROSSING_STRIPE_BOUNDARY;
	}
	if (metadata)
		btrfs_check_subpage_eb_alignment(gfs_info, key.objectid, nodesize);

	ptr = (unsigned long)(ei + 1);

	if (metadata && key.type == BTRFS_EXTENT_ITEM_KEY) {
		/* Old EXTENT_ITEM metadata */
		struct btrfs_tree_block_info *info;

		info = (struct btrfs_tree_block_info *)ptr;
		level = btrfs_tree_block_level(eb, info);
		ptr += sizeof(struct btrfs_tree_block_info);
	} else {
		/* New METADATA_ITEM */
		level = key.offset;
	}

	if (metadata && level >= BTRFS_MAX_LEVEL) {
		error(
		"tree block %llu has bad backref level, has %llu expect [0, %u]",
		      key.objectid, level, BTRFS_MAX_LEVEL - 1);
		err |= BACKREF_MISMATCH;
		/* This is a critical error, exit right now */
		goto out;
	}

	ptr_offset = ptr - (unsigned long)ei;

next:
	/* Reached extent item end normally */
	if (ptr_offset == item_size)
		goto out;

	/* Beyond extent item end, wrong item size */
	if (ptr_offset > item_size) {
		err |= ITEM_SIZE_MISMATCH;
		error("extent item at bytenr %llu slot %d has wrong size",
			eb->start, slot);
		goto out;
	}

	ptr = (unsigned long)ei + ptr_offset;
	parent = 0;
	root_objectid = 0;
	owner = 0;
	owner_offset = 0;
	/* Now check every backref in this extent item */
	iref = (struct btrfs_extent_inline_ref *)ptr;
	type = btrfs_extent_inline_ref_type(eb, iref);
	offset = btrfs_extent_inline_ref_offset(eb, iref);
	switch (type) {
	case BTRFS_TREE_BLOCK_REF_KEY:
		root_objectid = offset;
		owner = level;
		tmp_err |= check_tree_block_backref(offset, key.objectid, level);
		break;
	case BTRFS_SHARED_BLOCK_REF_KEY:
		parent = offset;
		tmp_err |= check_shared_block_backref(offset, key.objectid, level);
		break;
	case BTRFS_EXTENT_DATA_REF_KEY:
		dref = (struct btrfs_extent_data_ref *)(&iref->offset);
		root_objectid = btrfs_extent_data_ref_root(eb, dref);
		owner = btrfs_extent_data_ref_objectid(eb, dref);
		owner_offset = btrfs_extent_data_ref_offset(eb, dref);
		tmp_err |= check_extent_data_backref(root_objectid,
			    owner, owner_offset, key.objectid, key.offset,
			    btrfs_extent_data_ref_count(eb, dref));
		break;
	case BTRFS_SHARED_DATA_REF_KEY:
		parent = offset;
		tmp_err |= check_shared_data_backref(offset, key.objectid);
		break;
	default:
		error("extent[%llu %d %llu] has unknown ref type: %d",
			key.objectid, key.type, key.offset, type);
		err |= UNKNOWN_TYPE;

		goto out;
	}

	if ((tmp_err & (REFERENCER_MISSING | REFERENCER_MISMATCH))
	    && opt_check_repair) {
		ret = repair_extent_item(path, key.objectid, num_bytes, parent,
					 root_objectid, owner, owner_offset);
		if (ret < 0) {
			err |= tmp_err;
			err |= FATAL_ERROR;
			goto out;
		} else if (ret == 0) {
			err = 0;
			goto out;
		} else if (ret > 0) {
			/*
			 * The error has been repaired which means the
			 * extent item is still existed with other backrefs,
			 * go to check next.
			 */
			tmp_err &= ~REFERENCER_MISSING;
			tmp_err &= ~REFERENCER_MISMATCH;
			err |= tmp_err;
			eb = path->nodes[0];
			slot = path->slots[0];
			ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
			item_size = btrfs_item_size(eb, slot);
			goto next;
		}
	}
	if ((tmp_err & INVALID_GENERATION) && opt_check_repair){
		ret = repair_extent_item_generation(path);
		if (ret < 0) {
			err |= tmp_err;
			err |= FATAL_ERROR;
			goto out;
		}
		/* Error has been repaired */
		tmp_err &= ~INVALID_GENERATION;
		err |= tmp_err;
		eb = path->nodes[0];
		slot = path->slots[0];
		ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
		item_size = btrfs_item_size(eb, slot);
		ptr_offset += btrfs_extent_inline_ref_size(type);
		goto next;
	}

	err |= tmp_err;
	ptr_offset += btrfs_extent_inline_ref_size(type);
	goto next;

out:
	return err;
}

/*
 * Check if a dev extent item is referred correctly by its chunk
 */
static int check_dev_extent_item(struct extent_buffer *eb, int slot)
{
	struct btrfs_root *chunk_root = gfs_info->chunk_root;
	struct btrfs_dev_extent *ptr;
	struct btrfs_path path;
	struct btrfs_key chunk_key;
	struct btrfs_key devext_key;
	struct btrfs_chunk *chunk;
	struct extent_buffer *l;
	int num_stripes;
	u64 length;
	int i;
	int found_chunk = 0;
	int ret;

	btrfs_item_key_to_cpu(eb, &devext_key, slot);
	ptr = btrfs_item_ptr(eb, slot, struct btrfs_dev_extent);
	length = btrfs_dev_extent_length(eb, ptr);

	chunk_key.objectid = btrfs_dev_extent_chunk_objectid(eb, ptr);
	chunk_key.type = BTRFS_CHUNK_ITEM_KEY;
	chunk_key.offset = btrfs_dev_extent_chunk_offset(eb, ptr);

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, chunk_root, &chunk_key, &path, 0, 0);
	if (ret)
		goto out;

	l = path.nodes[0];
	chunk = btrfs_item_ptr(l, path.slots[0], struct btrfs_chunk);
	ret = btrfs_check_chunk_valid(gfs_info, l, chunk, path.slots[0],
				      chunk_key.offset);
	if (ret < 0)
		goto out;

	if (btrfs_stripe_length(gfs_info, l, chunk) != length)
		goto out;

	num_stripes = btrfs_chunk_num_stripes(l, chunk);
	for (i = 0; i < num_stripes; i++) {
		u64 devid = btrfs_stripe_devid_nr(l, chunk, i);
		u64 offset = btrfs_stripe_offset_nr(l, chunk, i);

		if (devid == devext_key.objectid &&
		    offset == devext_key.offset) {
			found_chunk = 1;
			break;
		}
	}
out:
	btrfs_release_path(&path);
	if (!found_chunk) {
		error(
		"device extent[%llu, %llu, %llu] did not find the related chunk",
			devext_key.objectid, devext_key.offset, length);
		return REFERENCER_MISSING;
	}
	return 0;
}

/*
 * Check if the used space is correct with the dev item
 */
static int check_dev_item(struct extent_buffer *eb, int slot,
			  u64 *bytes_used_expected)
{
	struct btrfs_root *dev_root = gfs_info->dev_root;
	struct btrfs_dev_item *dev_item;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_dev_extent *ptr;
	struct btrfs_device *dev;
	struct stat st;
	u64 block_dev_size;
	u64 total_bytes;
	u64 dev_id;
	u64 used;
	u64 total = 0;
	u64 prev_devid = 0;
	u64 prev_dev_ext_end = 0;
	int ret;

	dev_item = btrfs_item_ptr(eb, slot, struct btrfs_dev_item);
	dev_id = btrfs_device_id(eb, dev_item);
	used = btrfs_device_bytes_used(eb, dev_item);
	total_bytes = btrfs_device_total_bytes(eb, dev_item);

	if (used > total_bytes) {
		error(
		"device %llu has incorrect used bytes %llu > total bytes %llu",
			dev_id, used, total_bytes);
		return ACCOUNTING_MISMATCH;
	}
	key.objectid = dev_id;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = 0;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, dev_root, &key, &path, 0, 0);
	if (ret < 0) {
		btrfs_item_key_to_cpu(eb, &key, slot);
		error("cannot find any related dev extent for dev[%llu, %u, %llu]",
			key.objectid, key.type, key.offset);
		btrfs_release_path(&path);
		return REFERENCER_MISSING;
	}

	/*
	 * Iterate dev_extents to calculate the used space of a device
	 *
	 * Also make sure no dev extents overlap and end beyond device boundary
	 */
	while (1) {
		u64 devid;
		u64 physical_offset;
		u64 physical_len;

		if (path.slots[0] >= btrfs_header_nritems(path.nodes[0]))
			goto next;

		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.objectid > dev_id)
			break;
		if (key.type != BTRFS_DEV_EXTENT_KEY || key.objectid != dev_id)
			goto next;

		ptr = btrfs_item_ptr(path.nodes[0], path.slots[0],
				     struct btrfs_dev_extent);
		devid = key.objectid;
		physical_offset = key.offset;
		physical_len = btrfs_dev_extent_length(path.nodes[0], ptr);

		if (prev_devid == devid && physical_offset < prev_dev_ext_end) {
			error(
"dev extent devid %llu offset %llu len %llu overlap with previous dev extent end %llu",
			      devid, physical_offset, physical_len,
			      prev_dev_ext_end);
			btrfs_release_path(&path);
			return ACCOUNTING_MISMATCH;
		}
		if (physical_offset + physical_len > total_bytes) {
			error(
"dev extent devid %llu offset %llu len %llu is beyond device boundary %llu",
			      devid, physical_offset, physical_len,
			      total_bytes);
			btrfs_release_path(&path);
			return ACCOUNTING_MISMATCH;
		}
		prev_devid = devid;
		prev_dev_ext_end = physical_offset + physical_len;
		total += physical_len;
next:
		ret = btrfs_next_item(dev_root, &path);
		if (ret)
			break;
	}
	btrfs_release_path(&path);

	*bytes_used_expected = total;
	if (used != total) {
		btrfs_item_key_to_cpu(eb, &key, slot);
		error(
"Dev extent's total-byte %llu is not equal to bytes-used %llu in dev[%llu, %u, %llu]",
			total, used, BTRFS_ROOT_TREE_OBJECTID,
			BTRFS_DEV_EXTENT_KEY, dev_id);
		return ACCOUNTING_MISMATCH;
	}
	check_dev_size_alignment(dev_id, total_bytes, gfs_info->sectorsize);

	dev = btrfs_find_device_by_devid(gfs_info->fs_devices, dev_id, 0);
	if (!dev || dev->fd < 0)
		return 0;

	ret = fstat(dev->fd, &st);
	if (ret < 0) {
		warning(
		"unable to open devid %llu, skipping its block device size check",
			dev->devid);
		return 0;
	}
	block_dev_size = device_get_partition_size_fd_stat(dev->fd, &st);
	if (block_dev_size < total_bytes) {
		error(
"block device size is smaller than total_bytes in device item, has %llu expect >= %llu",
		      block_dev_size, total_bytes);
		return ACCOUNTING_MISMATCH;
	}
	return 0;
}

/*
 * Find the block group item with @bytenr, @len and @type
 *
 * Return 0 if found.
 * Return -ENOENT if not found.
 * Return <0 for fatal error.
 */
static int find_block_group_item(struct btrfs_path *path, u64 bytenr, u64 len,
				 u64 type)
{
	struct btrfs_root *root = btrfs_block_group_root(gfs_info);
	struct btrfs_block_group_item bgi;
	struct btrfs_key key;
	int ret;

	key.objectid = bytenr;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = len;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		return ret;
	if (ret > 0) {
		ret = -ENOENT;
		error("chunk [%llu %llu) doesn't have related block group item",
		      bytenr, bytenr + len);
		goto out;
	}
	read_extent_buffer(path->nodes[0], &bgi,
			btrfs_item_ptr_offset(path->nodes[0], path->slots[0]),
			sizeof(bgi));
	if (btrfs_stack_block_group_flags(&bgi) != type) {
		error(
"chunk [%llu %llu) type mismatch with block group, block group has 0x%llx chunk has %llx",
		      bytenr, bytenr + len, btrfs_stack_block_group_flags(&bgi),
		      type);
		ret = -EUCLEAN;
	}

out:
	btrfs_release_path(path);
	return ret;
}

/*
 * Check a chunk item.
 * Including checking all referred dev_extents and block group
 */
static int check_chunk_item(struct extent_buffer *eb, int slot)
{
	struct btrfs_root *dev_root = gfs_info->dev_root;
	struct btrfs_path path;
	struct btrfs_key chunk_key;
	struct btrfs_key devext_key;
	struct btrfs_chunk *chunk;
	struct extent_buffer *leaf;
	struct btrfs_dev_extent *ptr;
	u64 length;
	u64 chunk_end;
	u64 stripe_len;
	u64 type;
	int num_stripes;
	u64 offset;
	u64 objectid;
	int i;
	int ret;
	int err = 0;

	btrfs_item_key_to_cpu(eb, &chunk_key, slot);
	chunk = btrfs_item_ptr(eb, slot, struct btrfs_chunk);
	length = btrfs_chunk_length(eb, chunk);
	chunk_end = chunk_key.offset + length;
	ret = btrfs_check_chunk_valid(gfs_info, eb, chunk, slot,
				      chunk_key.offset);
	if (ret < 0) {
		error("chunk[%llu %llu) is invalid", chunk_key.offset,
			chunk_end);
		err |= BYTES_UNALIGNED | UNKNOWN_TYPE;
		goto out;
	}
	type = btrfs_chunk_type(eb, chunk);

	btrfs_init_path(&path);
	ret = find_block_group_item(&path, chunk_key.offset, length, type);
	if (ret < 0)
		err |= REFERENCER_MISSING;

	num_stripes = btrfs_chunk_num_stripes(eb, chunk);
	stripe_len = btrfs_stripe_length(gfs_info, eb, chunk);
	for (i = 0; i < num_stripes; i++) {
		btrfs_release_path(&path);
		btrfs_init_path(&path);
		devext_key.objectid = btrfs_stripe_devid_nr(eb, chunk, i);
		devext_key.type = BTRFS_DEV_EXTENT_KEY;
		devext_key.offset = btrfs_stripe_offset_nr(eb, chunk, i);

		ret = btrfs_search_slot(NULL, dev_root, &devext_key, &path,
					0, 0);
		if (ret)
			goto not_match_dev;

		leaf = path.nodes[0];
		ptr = btrfs_item_ptr(leaf, path.slots[0],
				     struct btrfs_dev_extent);
		objectid = btrfs_dev_extent_chunk_objectid(leaf, ptr);
		offset = btrfs_dev_extent_chunk_offset(leaf, ptr);
		if (objectid != chunk_key.objectid ||
		    offset != chunk_key.offset ||
		    btrfs_dev_extent_length(leaf, ptr) != stripe_len)
			goto not_match_dev;
		continue;
not_match_dev:
		err |= BACKREF_MISSING;
		error(
		"chunk[%llu %llu) stripe %d did not find the related dev extent",
			chunk_key.objectid, chunk_end, i);
		continue;
	}
	btrfs_release_path(&path);
out:
	return err;
}

/*
 * Add block group item to the extent tree if @err contains REFERENCER_MISSING.
 * FIXME: We still need to repair error of dev_item.
 *
 * Returns error after repair.
 */
static int repair_chunk_item(struct btrfs_root *chunk_root,
			     struct btrfs_path *path, int err)
{
	struct btrfs_chunk *chunk;
	struct btrfs_key chunk_key;
	struct extent_buffer *eb = path->nodes[0];
	struct btrfs_root *extent_root;
	struct btrfs_trans_handle *trans;
	u64 length;
	int slot = path->slots[0];
	u64 type;
	int ret = 0;

	btrfs_item_key_to_cpu(eb, &chunk_key, slot);
	if (chunk_key.type != BTRFS_CHUNK_ITEM_KEY)
		return err;
	extent_root = btrfs_extent_root(gfs_info, chunk_key.offset);
	chunk = btrfs_item_ptr(eb, slot, struct btrfs_chunk);
	type = btrfs_chunk_type(path->nodes[0], chunk);
	length = btrfs_chunk_length(eb, chunk);

	/* now repair only adds block group */
	if ((err & REFERENCER_MISSING) == 0)
		return err;

	ret = avoid_extents_overwrite();
	if (ret)
		return ret;

	trans = btrfs_start_transaction(extent_root, 1);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	ret = btrfs_make_block_group(trans, gfs_info, 0, type,
				     chunk_key.offset, length);
	if (ret) {
		error("fail to add block group item [%llu %llu]",
		      chunk_key.offset, length);
	} else {
		err &= ~REFERENCER_MISSING;
		printf("Added block group item[%llu %llu]\n", chunk_key.offset,
		       length);
	}

	btrfs_commit_transaction(trans, extent_root);
	if (ret)
		error("fail to repair item(s) related to chunk item [%llu %llu]",
		      chunk_key.objectid, chunk_key.offset);
	return err;
}

/*
 * Main entry function to check known items and update related accounting info
 */
static int check_leaf_items(struct btrfs_root *root, struct btrfs_path *path,
			    struct node_refs *nrefs, int account_bytes)
{
	u64 bytes_used_expected = (u64)-1;
	struct btrfs_key key;
	struct extent_buffer *eb;
	int slot;
	int type;
	struct btrfs_extent_data_ref *dref;
	int ret = 0;
	int err = 0;

again:
	eb = path->nodes[0];
	slot = path->slots[0];
	if (slot >= btrfs_header_nritems(eb)) {
		if (slot == 0) {
			error("empty leaf [%llu %u] root %llu", eb->start,
				gfs_info->nodesize, root->objectid);
			err |= EIO;
		}
		goto out;
	}

	btrfs_item_key_to_cpu(eb, &key, slot);
	type = key.type;

	switch (type) {
	case BTRFS_EXTENT_DATA_KEY:
		ret = check_extent_data_item(root, path, nrefs, account_bytes);
		if (opt_check_repair && ret)
			ret = repair_extent_data_item(root, path, nrefs, ret);
		err |= ret;
		break;
	case BTRFS_BLOCK_GROUP_ITEM_KEY:
		ret = check_block_group_item(eb, slot);
		if (opt_check_repair &&
		    ret & REFERENCER_MISSING)
			ret = delete_item(root, path);
		err |= ret;
		break;
	case BTRFS_DEV_ITEM_KEY:
		ret = check_dev_item(eb, slot, &bytes_used_expected);
		if (opt_check_repair && (ret & ACCOUNTING_MISMATCH) &&
		    bytes_used_expected != (u64)-1) {
			ret = repair_dev_item_bytes_used(root->fs_info,
					key.offset, bytes_used_expected);
			if (ret < 0)
				ret = ACCOUNTING_MISMATCH;
		}
		err |= ret;
		break;
	case BTRFS_CHUNK_ITEM_KEY:
		ret = check_chunk_item(eb, slot);
		if (opt_check_repair && ret)
			ret = repair_chunk_item(root, path, ret);
		err |= ret;
		break;
	case BTRFS_DEV_EXTENT_KEY:
		ret = check_dev_extent_item(eb, slot);
		err |= ret;
		break;
	case BTRFS_EXTENT_ITEM_KEY:
	case BTRFS_METADATA_ITEM_KEY:
		ret = check_extent_item(path);
		err |= ret;
		break;
	case BTRFS_EXTENT_CSUM_KEY:
		total_csum_bytes += btrfs_item_size(eb, slot);
		err |= ret;
		break;
	case BTRFS_TREE_BLOCK_REF_KEY:
		ret = check_tree_block_backref(key.offset, key.objectid, -1);
		if (opt_check_repair &&
		    ret & (REFERENCER_MISMATCH | REFERENCER_MISSING))
			ret = delete_item(root, path);
		err |= ret;
		break;
	case BTRFS_EXTENT_DATA_REF_KEY:
		dref = btrfs_item_ptr(eb, slot, struct btrfs_extent_data_ref);
		ret = check_extent_data_backref(
				btrfs_extent_data_ref_root(eb, dref),
				btrfs_extent_data_ref_objectid(eb, dref),
				btrfs_extent_data_ref_offset(eb, dref),
				key.objectid, 0,
				btrfs_extent_data_ref_count(eb, dref));
		if (opt_check_repair &&
		    ret & (REFERENCER_MISMATCH | REFERENCER_MISSING))
			ret = delete_item(root, path);
		err |= ret;
		break;
	case BTRFS_SHARED_BLOCK_REF_KEY:
		ret = check_shared_block_backref(key.offset, key.objectid, -1);
		if (opt_check_repair &&
		    ret & (REFERENCER_MISMATCH | REFERENCER_MISSING))
			ret = delete_item(root, path);
		err |= ret;
		break;
	case BTRFS_SHARED_DATA_REF_KEY:
		ret = check_shared_data_backref(key.offset, key.objectid);
		if (opt_check_repair &&
		    ret & (REFERENCER_MISMATCH | REFERENCER_MISSING))
			ret = delete_item(root, path);
		err |= ret;
		break;
	default:
		break;
	}

	++path->slots[0];
	goto again;
out:
	return err;
}

/*
 * @trans      just for lowmem repair mode
 * @check all  if not 0 then check all tree block backrefs and items
 *             0 then just check relationship of items in fs tree(s)
 *
 * Returns >0  Found error, should continue
 * Returns <0  Fatal error, must exit the whole check
 * Returns 0   No errors found
 */
static int walk_down_tree(struct btrfs_root *root, struct btrfs_path *path,
			  int *level, struct node_refs *nrefs, int check_all)
{
	enum btrfs_tree_block_status status;
	u64 bytenr;
	u64 ptr_gen;
	struct extent_buffer *next;
	struct extent_buffer *cur;
	int ret;
	int err = 0;
	int check;
	int account_file_data = 0;

	WARN_ON(*level < 0);
	WARN_ON(*level >= BTRFS_MAX_LEVEL);

	ret = update_nodes_refs(root, btrfs_header_bytenr(path->nodes[*level]),
				path->nodes[*level], nrefs, *level, check_all);
	if (ret < 0)
		return ret;

	while (*level >= 0) {
		WARN_ON(*level < 0);
		WARN_ON(*level >= BTRFS_MAX_LEVEL);
		cur = path->nodes[*level];
		bytenr = btrfs_header_bytenr(cur);
		check = nrefs->need_check[*level];

		if (btrfs_header_level(cur) != *level)
			WARN_ON(1);
	       /*
		* Update bytes accounting and check tree block ref
		* NOTE: Doing accounting and check before checking nritems
		* is necessary because of empty node/leaf.
		*/
		if ((check_all && !nrefs->checked[*level]) ||
		    (!check_all && nrefs->need_check[*level])) {
			ret = check_tree_block_ref(root, cur,
			   btrfs_header_bytenr(cur), btrfs_header_level(cur),
			   btrfs_header_owner(cur), nrefs);

			if (opt_check_repair && ret)
				ret = repair_tree_block_ref(root,
				    path->nodes[*level], nrefs, *level, ret);
			err |= ret;

			if (check_all && nrefs->need_check[*level] &&
				nrefs->refs[*level]) {
				account_bytes(root, path, *level);
				account_file_data = 1;
			}
			nrefs->checked[*level] = 1;
		}

		if (path->slots[*level] >= btrfs_header_nritems(cur))
			break;

		/* Don't forgot to check leaf/node validation */
		if (*level == 0) {
			/* skip duplicate check */
			if (check || !check_all) {
				ret = btrfs_check_leaf(gfs_info, NULL, cur);
				if (ret != BTRFS_TREE_BLOCK_CLEAN) {
					err |= -EIO;
					break;
				}
			}

			ret = 0;
			if (!check_all)
				ret = process_one_leaf(root, path, nrefs, level);
			else
				ret = check_leaf_items(root, path,
					       nrefs, account_file_data);
			err |= ret;
			break;
		}
		if (check || !check_all) {
			ret = btrfs_check_node(gfs_info, NULL, cur);
			if (ret != BTRFS_TREE_BLOCK_CLEAN) {
				err |= -EIO;
				break;
			}
		}

		bytenr = btrfs_node_blockptr(cur, path->slots[*level]);
		ptr_gen = btrfs_node_ptr_generation(cur, path->slots[*level]);

		ret = update_nodes_refs(root, bytenr, NULL, nrefs, *level - 1,
					check_all);
		if (ret < 0)
			break;
		/*
		 * check all trees in check_chunks_and_extent
		 * check shared node once in check_fs_roots
		 */
		if (!check_all && !nrefs->need_check[*level - 1]) {
			path->slots[*level]++;
			continue;
		}

		next = btrfs_find_tree_block(gfs_info, bytenr, gfs_info->nodesize);
		if (!next || !btrfs_buffer_uptodate(next, ptr_gen)) {
			free_extent_buffer(next);
			reada_walk_down(root, cur, path->slots[*level]);
			next = read_tree_block(gfs_info, bytenr, ptr_gen);
			if (!extent_buffer_uptodate(next)) {
				struct btrfs_key node_key;

				btrfs_node_key_to_cpu(path->nodes[*level],
						      &node_key,
						      path->slots[*level]);
				btrfs_add_corrupt_extent_record(gfs_info,
					&node_key, path->nodes[*level]->start,
					gfs_info->nodesize, *level);
				err |= -EIO;
				break;
			}
		}

		ret = check_child_node(cur, path->slots[*level], next);
		err |= ret;
		if (ret < 0)
			break;

		if (btrfs_is_leaf(next))
			status = btrfs_check_leaf(gfs_info, NULL, next);
		else
			status = btrfs_check_node(gfs_info, NULL, next);
		if (status != BTRFS_TREE_BLOCK_CLEAN) {
			free_extent_buffer(next);
			err |= -EIO;
			break;
		}

		*level = *level - 1;
		free_extent_buffer(path->nodes[*level]);
		path->nodes[*level] = next;
		path->slots[*level] = 0;
		account_file_data = 0;

		update_nodes_refs(root, (u64)-1, next, nrefs, *level, check_all);
	}
	return err;
}

static int walk_up_tree(struct btrfs_root *root, struct btrfs_path *path,
			int *level)
{
	int i;
	struct extent_buffer *leaf;

	for (i = *level; i < BTRFS_MAX_LEVEL - 1 && path->nodes[i]; i++) {
		leaf = path->nodes[i];
		if (path->slots[i] + 1 < btrfs_header_nritems(leaf)) {
			path->slots[i]++;
			*level = i;
			return 0;
		}
		free_extent_buffer(path->nodes[*level]);
		path->nodes[*level] = NULL;
		*level = i + 1;
	}
	return 1;
}

/*
 * Insert the missing inode item and inode ref.
 *
 * Normal INODE_ITEM_MISSING and INODE_REF_MISSING are handled in backref * dir.
 * Root dir should be handled specially because root dir is the root of fs.
 *
 * returns err (>0 or 0) after repair
 */
static int repair_fs_first_inode(struct btrfs_root *root, int err)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_key key;
	struct btrfs_path path;
	int filetype = BTRFS_FT_DIR;
	int ret = 0;

	btrfs_init_path(&path);

	if (err & INODE_REF_MISSING) {
		key.objectid = BTRFS_FIRST_FREE_OBJECTID;
		key.type = BTRFS_INODE_REF_KEY;
		key.offset = BTRFS_FIRST_FREE_OBJECTID;

		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			goto out;
		}

		btrfs_release_path(&path);
		ret = btrfs_search_slot(trans, root, &key, &path, 1, 1);
		if (ret)
			goto trans_fail;

		ret = btrfs_insert_inode_ref(trans, root, "..", 2,
					     BTRFS_FIRST_FREE_OBJECTID,
					     BTRFS_FIRST_FREE_OBJECTID, 0);
		if (ret)
			goto trans_fail;

		printf("Add INODE_REF[%llu %llu] name %s\n",
		       BTRFS_FIRST_FREE_OBJECTID, BTRFS_FIRST_FREE_OBJECTID,
		       "..");
		err &= ~INODE_REF_MISSING;
trans_fail:
		if (ret)
			error("fail to insert first inode's ref");
		btrfs_commit_transaction(trans, root);
	}

	if (err & INODE_ITEM_MISSING) {
		ret = repair_inode_item_missing(root,
					BTRFS_FIRST_FREE_OBJECTID, filetype);
		if (ret)
			goto out;
		err &= ~INODE_ITEM_MISSING;
	}
out:
	if (ret)
		error("fail to repair first inode");
	btrfs_release_path(&path);
	return err;
}

/*
 * check first root dir's inode_item and inode_ref
 *
 * returns 0 means no error
 * returns >0 means error
 * returns <0 means fatal error
 */
static int check_fs_first_inode(struct btrfs_root *root)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_inode_item *ii;
	u64 index;
	u32 mode;
	int err = 0;
	int ret;

	key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	/* For root being dropped, we don't need to check first inode */
	if (btrfs_root_refs(&root->root_item) == 0 &&
	    btrfs_disk_key_objectid(&root->root_item.drop_progress) >=
	    BTRFS_FIRST_FREE_OBJECTID)
		return 0;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = 0;
		err |= INODE_ITEM_MISSING;
	} else {
		ii = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_inode_item);
		mode = btrfs_inode_mode(path.nodes[0], ii);
		if (imode_to_type(mode) != BTRFS_FT_DIR)
			err |= INODE_ITEM_MISMATCH;
	}

	/* lookup first inode ref */
	key.offset = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_INODE_REF_KEY;
	/* special index value */
	index = 0;

	ret = find_inode_ref(root, &key, "..", strlen(".."), &index);
	if (ret < 0)
		goto out;
	err |= ret;

out:
	btrfs_release_path(&path);

	if (err && opt_check_repair)
		err = repair_fs_first_inode(root, err);

	if (err & (INODE_ITEM_MISSING | INODE_ITEM_MISMATCH))
		error("root dir INODE_ITEM is %s",
		      err & INODE_ITEM_MISMATCH ? "mismatch" : "missing");
	if (err & INODE_REF_MISSING)
		error("root dir INODE_REF is missing");

	return ret < 0 ? ret : err;
}

/*
 * This function calls walk_down_tree and walk_up_tree to check tree
 * blocks and integrity of fs tree items.
 *
 * @root:         the root of the tree to be checked.
 * @account       if NOT 0 means check the tree (including tree)'s treeblocks.
 *                otherwise means check fs tree(s) items relationship and
 *		  @root MUST be a fs tree root.
 * Returns 0      represents OK.
 * Returns >0     represents error bits.
 */
static int check_btrfs_root(struct btrfs_root *root, int check_all)
{
	struct btrfs_path path;
	struct node_refs nrefs;
	struct btrfs_root_item *root_item = &root->root_item;
	u64 super_generation = btrfs_super_generation(gfs_info->super_copy);
	int ret;
	int level;
	int err = 0;

	memset(&nrefs, 0, sizeof(nrefs));
	if (!check_all) {
		/*
		 * We need to manually check the first inode item (256)
		 * As the following traversal function will only start from
		 * the first inode item in the leaf, if inode item (256) is
		 * missing we will skip it forever.
		 */
		ret = check_fs_first_inode(root);
		if (ret)
			return FATAL_ERROR;
	}


	level = btrfs_header_level(root->node);
	btrfs_init_path(&path);

	if (btrfs_root_generation(root_item) > super_generation + 1) {
		error(
	"invalid root generation for root %llu, have %llu expect (0, %llu)",
		      root->root_key.objectid, btrfs_root_generation(root_item),
		      super_generation + 1);
		err |= INVALID_GENERATION;
		if (opt_check_repair) {
			root->node->flags |= EXTENT_BAD_TRANSID;
			ret = recow_extent_buffer(root, root->node);
			if (!ret) {
				printf("Reset generation for root %llu\n",
					root->root_key.objectid);
				err &= ~INVALID_GENERATION;
			}
		}
	}
	if (btrfs_root_refs(root_item) > 0 ||
	    btrfs_disk_key_objectid(&root_item->drop_progress) == 0) {
		path.nodes[level] = root->node;
		path.slots[level] = 0;
		extent_buffer_get(root->node);
	} else {
		struct btrfs_key key;

		btrfs_disk_key_to_cpu(&key, &root_item->drop_progress);
		level = root_item->drop_level;
		path.lowest_level = level;
		ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
		if (ret < 0)
			goto out;
		ret = 0;
	}

	while (1) {
		g_task_ctx.item_count++;
		ret = walk_down_tree(root, &path, &level, &nrefs, check_all);

		if (ret > 0)
			err |= ret;
		/* if ret is negative, walk shall stop */
		if (ret < 0) {
			ret = err | FATAL_ERROR;
			break;
		}

		ret = walk_up_tree(root, &path, &level);
		if (ret != 0) {
			/* Normal exit, reset ret to err */
			ret = err;
			break;
		}
	}

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Iterate all items in the tree and call check_inode_item() to check.
 *
 * @root:	the root of the tree to be checked.
 *
 * Return 0 if no error found.
 * Return <0 for error.
 */
static int check_fs_root(struct btrfs_root *root)
{
	reset_cached_block_groups();
	return check_btrfs_root(root, 0);
}

/*
 * Find the relative ref for root_ref and root_backref.
 *
 * @root:	the root of the root tree.
 * @ref_key:	the key of the root ref.
 *
 * Return 0 if no error occurred.
 */
static int check_root_ref(struct btrfs_root *root, struct btrfs_key *ref_key,
			  struct extent_buffer *node, int slot)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root_ref *ref;
	struct btrfs_root_ref *backref;
	char ref_name[BTRFS_NAME_LEN] = {0};
	char backref_name[BTRFS_NAME_LEN] = {0};
	u64 ref_dirid;
	u64 ref_seq;
	u32 ref_namelen;
	u64 backref_dirid;
	u64 backref_seq;
	u32 backref_namelen;
	u32 len;
	int ret;
	int err = 0;

	ref = btrfs_item_ptr(node, slot, struct btrfs_root_ref);
	ref_dirid = btrfs_root_ref_dirid(node, ref);
	ref_seq = btrfs_root_ref_sequence(node, ref);
	ref_namelen = btrfs_root_ref_name_len(node, ref);

	if (ref_namelen <= BTRFS_NAME_LEN) {
		len = ref_namelen;
	} else {
		len = BTRFS_NAME_LEN;
		warning("%s[%llu %llu] ref_name too long",
			ref_key->type == BTRFS_ROOT_REF_KEY ?
			"ROOT_REF" : "ROOT_BACKREF", ref_key->objectid,
			ref_key->offset);
	}
	read_extent_buffer(node, ref_name, (unsigned long)(ref + 1), len);

	/* Find relative root_ref */
	key.objectid = ref_key->offset;
	key.type = BTRFS_ROOT_BACKREF_KEY + BTRFS_ROOT_REF_KEY - ref_key->type;
	key.offset = ref_key->objectid;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret) {
		err |= ROOT_REF_MISSING;
		error("%s[%llu %llu] couldn't find relative ref",
		      ref_key->type == BTRFS_ROOT_REF_KEY ?
		      "ROOT_REF" : "ROOT_BACKREF",
		      ref_key->objectid, ref_key->offset);
		goto out;
	}

	backref = btrfs_item_ptr(path.nodes[0], path.slots[0],
				 struct btrfs_root_ref);
	backref_dirid = btrfs_root_ref_dirid(path.nodes[0], backref);
	backref_seq = btrfs_root_ref_sequence(path.nodes[0], backref);
	backref_namelen = btrfs_root_ref_name_len(path.nodes[0], backref);

	if (backref_namelen <= BTRFS_NAME_LEN) {
		len = backref_namelen;
	} else {
		len = BTRFS_NAME_LEN;
		warning("%s[%llu %llu] ref_name too long",
			key.type == BTRFS_ROOT_REF_KEY ?
			"ROOT_REF" : "ROOT_BACKREF",
			key.objectid, key.offset);
	}
	read_extent_buffer(path.nodes[0], backref_name,
			   (unsigned long)(backref + 1), len);

	if (ref_dirid != backref_dirid || ref_seq != backref_seq ||
	    ref_namelen != backref_namelen ||
	    strncmp(ref_name, backref_name, len)) {
		err |= ROOT_REF_MISMATCH;
		error("%s[%llu %llu] mismatch relative ref",
		      ref_key->type == BTRFS_ROOT_REF_KEY ?
		      "ROOT_REF" : "ROOT_BACKREF",
		      ref_key->objectid, ref_key->offset);
	}
out:
	btrfs_release_path(&path);
	return err;
}

/*
 * Check all fs/file tree in low_memory mode.
 *
 * 1. for fs tree root item, call check_fs_root()
 * 2. for fs tree root ref/backref, call check_root_ref()
 *
 * Return 0 if no error occurred.
 */
int check_fs_roots_lowmem(void)
{
	struct btrfs_root *tree_root = gfs_info->tree_root;
	struct btrfs_root *cur_root = NULL;
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *node;
	int slot;
	int ret;
	int err = 0;

	btrfs_init_path(&path);
	key.objectid = BTRFS_FS_TREE_OBJECTID;
	key.offset = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;

	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	if (ret < 0) {
		err = ret;
		goto out;
	} else if (ret > 0) {
		err = -ENOENT;
		goto out;
	}

	while (1) {
		node = path.nodes[0];
		slot = path.slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);
		if (key.objectid > BTRFS_LAST_FREE_OBJECTID)
			goto out;
		if (key.type == BTRFS_INODE_ITEM_KEY &&
		    is_fstree(key.objectid)) {
			ret = check_repair_free_space_inode(&path);
			/* Check if we still have a valid path to continue */
			if (ret < 0 && path.nodes[0]) {
				err |= ret;
				goto next;
			}
			if (ret < 0 && !path.nodes[0])
				goto out;
		}
		if (key.type == BTRFS_ROOT_ITEM_KEY &&
		    fs_root_objectid(key.objectid)) {
			if (key.objectid == BTRFS_TREE_RELOC_OBJECTID) {
				cur_root = btrfs_read_fs_root_no_cache(gfs_info,
								       &key);
			} else {
				key.offset = (u64)-1;
				cur_root = btrfs_read_fs_root(gfs_info, &key);
			}

			if (IS_ERR(cur_root)) {
				error("Fail to read fs/subvol tree: %lld",
				      key.objectid);
				err = -EIO;
				goto next;
			}

			ret = check_fs_root(cur_root);
			err |= ret;

			if (key.objectid == BTRFS_TREE_RELOC_OBJECTID)
				btrfs_free_fs_root(cur_root);
		} else if (key.type == BTRFS_ROOT_REF_KEY ||
				key.type == BTRFS_ROOT_BACKREF_KEY) {
			ret = check_root_ref(tree_root, &key, node, slot);
			err |= ret;
		}
next:
		/*
		 * In repair mode, our path is no longer reliable as CoW can
		 * happen.  We need to reset our path.
		 */
		if (opt_check_repair) {
			btrfs_release_path(&path);
			ret = btrfs_search_slot(NULL, tree_root, &key, &path,
						0, 0);
			if (ret < 0) {
				if (!err)
					err = ret;
				goto out;
			}
			if (ret > 0) {
				/* Key not found, but already at next item */
				if (path.slots[0] <
				    btrfs_header_nritems(path.nodes[0]))
					continue;
				/* falls through to next leaf */
			}
		}
		ret = btrfs_next_item(tree_root, &path);
		if (ret > 0)
			goto out;
		if (ret < 0) {
			err = ret;
			goto out;
		}
	}

out:
	btrfs_release_path(&path);
	return err;
}

/*
 * Low memory usage version check_chunks_and_extents.
 */
int check_chunks_and_extents_lowmem(void)
{
	struct btrfs_path path;
	struct btrfs_key old_key;
	struct btrfs_key key;
	struct btrfs_root *root;
	struct btrfs_root *cur_root;
	int err = 0;
	int ret;

	root = gfs_info->chunk_root;
	ret = check_btrfs_root(root, 1);
	err |= ret;

	root = gfs_info->tree_root;
	ret = check_btrfs_root(root, 1);
	err |= ret;

	btrfs_init_path(&path);
	key.objectid = BTRFS_EXTENT_TREE_OBJECTID;
	key.offset = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;

	ret = btrfs_search_slot(NULL, gfs_info->tree_root, &key, &path, 0, 0);
	if (ret) {
		error("cannot find extent tree in tree_root");
		goto out;
	}

	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.type != BTRFS_ROOT_ITEM_KEY)
			goto next;
		old_key = key;
		key.offset = (u64)-1;

		if (key.objectid == BTRFS_TREE_RELOC_OBJECTID)
			cur_root = btrfs_read_fs_root_no_cache(gfs_info,
					&key);
		else
			cur_root = btrfs_read_fs_root(gfs_info, &key);
		if (IS_ERR(cur_root) || !cur_root) {
			error("failed to read tree: %lld", key.objectid);
			goto next;
		}

		ret = check_btrfs_root(cur_root, 1);
		err |= ret;

		if (key.objectid == BTRFS_TREE_RELOC_OBJECTID)
			btrfs_free_fs_root(cur_root);

		btrfs_release_path(&path);
		ret = btrfs_search_slot(NULL, gfs_info->tree_root,
					&old_key, &path, 0, 0);
		if (ret)
			goto out;
next:
		ret = btrfs_next_item(gfs_info->tree_root, &path);
		if (ret)
			goto out;
	}
out:

	if (total_used != btrfs_super_bytes_used(gfs_info->super_copy)) {
		fprintf(stderr,
			"super bytes_used %llu mismatches actual used %llu\n",
			btrfs_super_bytes_used(gfs_info->super_copy),
			total_used);
		err |= SUPER_BYTES_USED_ERROR;
	}

	if (opt_check_repair) {
		ret = end_avoid_extents_overwrite();
		if (ret < 0)
			ret = FATAL_ERROR;
		err |= ret;

		reset_cached_block_groups();
		/* update block accounting */
		ret = repair_block_accounting();
		if (ret)
			err |= ret;
		else
			err &= ~(BG_ACCOUNTING_ERROR | SUPER_BYTES_USED_ERROR);
	}

	btrfs_release_path(&path);
	return err;
}
