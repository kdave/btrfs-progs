/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
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

#include "kernel-lib/bitops.h"
#include "kernel-lib/sizes.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/tree-checker.h"
#include "kernel-shared/volumes.h"
#include "crypto/crc32c.h"
#include "common/internal.h"
#include "common/messages.h"
#include "common/utils.h"
#include "check/repair.h"

static int split_node(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_path *path, int level);
static int split_leaf(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		      const struct btrfs_key *ins_key, struct btrfs_path *path,
		      int data_size, int extend);
static int push_node_left(struct btrfs_trans_handle *trans,
			  struct extent_buffer *dst,
			  struct extent_buffer *src, int empty);
static int balance_node_right(struct btrfs_trans_handle *trans,
			      struct extent_buffer *dst_buf,
			      struct extent_buffer *src_buf);

static const struct btrfs_csums {
	u16		size;
	const char	name[10];
	const char	driver[12];
} btrfs_csums[] = {
	[BTRFS_CSUM_TYPE_CRC32] = { .size = 4, .name = "crc32c" },
	[BTRFS_CSUM_TYPE_XXHASH] = { .size = 8, .name = "xxhash64" },
	[BTRFS_CSUM_TYPE_SHA256] = { .size = 32, .name = "sha256" },
	[BTRFS_CSUM_TYPE_BLAKE2] = { .size = 32, .name = "blake2b",
				     .driver = "blake2b-256" },
};

/*
 * The leaf data grows from end-to-front in the node.  this returns the address
 * of the start of the last item, which is the stop of the leaf data stack.
 */
static unsigned int leaf_data_end(const struct extent_buffer *leaf)
{
	u32 nr = btrfs_header_nritems(leaf);

	if (nr == 0)
		return BTRFS_LEAF_DATA_SIZE(leaf->fs_info);
	return btrfs_item_offset(leaf, nr - 1);
}

/*
 * Move data in a @leaf (using memmove, safe for overlapping ranges).
 *
 * @leaf:	leaf that we're doing a memmove on
 * @dst_offset:	item data offset we're moving to
 * @src_offset:	item data offset were' moving from
 * @len:	length of the data we're moving
 *
 * Wrapper around memmove_extent_buffer() that takes into account the header on
 * the leaf.  The btrfs_item offset's start directly after the header, so we
 * have to adjust any offsets to account for the header in the leaf.  This
 * handles that math to simplify the callers.
 */
static inline void memmove_leaf_data(const struct extent_buffer *leaf,
				     unsigned long dst_offset,
				     unsigned long src_offset,
				     unsigned long len)
{
	memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, 0) + dst_offset,
			      btrfs_item_nr_offset(leaf, 0) + src_offset, len);
}

/*
 * Copy item data from @src into @dst at the given @offset.
 *
 * @dst:	destination leaf that we're copying into
 * @src:	source leaf that we're copying from
 * @dst_offset:	item data offset we're copying to
 * @src_offset:	item data offset were' copying from
 * @len:	length of the data we're copying
 *
 * Wrapper around copy_extent_buffer() that takes into account the header on
 * the leaf.  The btrfs_item offset's start directly after the header, so we
 * have to adjust any offsets to account for the header in the leaf.  This
 * handles that math to simplify the callers.
 */
static inline void copy_leaf_data(const struct extent_buffer *dst,
				  const struct extent_buffer *src,
				  unsigned long dst_offset,
				  unsigned long src_offset, unsigned long len)
{
	copy_extent_buffer(dst, src, btrfs_item_nr_offset(dst, 0) + dst_offset,
			   btrfs_item_nr_offset(src, 0) + src_offset, len);
}

/*
 * Move items in a @leaf (using memmove).
 *
 * @dst:	destination leaf for the items
 * @dst_item:	the item nr we're copying into
 * @src_item:	the item nr we're copying from
 * @nr_items:	the number of items to copy
 *
 * Wrapper around memmove_extent_buffer() that does the math to get the
 * appropriate offsets into the leaf from the item numbers.
 */
static inline void memmove_leaf_items(const struct extent_buffer *leaf,
				      int dst_item, int src_item, int nr_items)
{
	memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, dst_item),
			      btrfs_item_nr_offset(leaf, src_item),
			      nr_items * sizeof(struct btrfs_item));
}

/*
 * Copy items from @src into @dst at the given @offset.
 *
 * @dst:	destination leaf for the items
 * @src:	source leaf for the items
 * @dst_item:	the item nr we're copying into
 * @src_item:	the item nr we're copying from
 * @nr_items:	the number of items to copy
 *
 * Wrapper around copy_extent_buffer() that does the math to get the
 * appropriate offsets into the leaf from the item numbers.
 */
static inline void copy_leaf_items(const struct extent_buffer *dst,
				   const struct extent_buffer *src,
				   int dst_item, int src_item, int nr_items)
{
	copy_extent_buffer(dst, src, btrfs_item_nr_offset(dst, dst_item),
			      btrfs_item_nr_offset(src, src_item),
			      nr_items * sizeof(struct btrfs_item));
}

int btrfs_super_csum_size(const struct btrfs_super_block *sb)
{
	const u16 csum_type = btrfs_super_csum_type(sb);

	/* csum type is validated at mount time */
	return btrfs_csums[csum_type].size;
}

const char *btrfs_super_csum_name(u16 csum_type)
{
	/* csum type is validated at mount time */
	return btrfs_csums[csum_type].name;
}

/*
 * Return driver name if defined, otherwise the name that's also a valid driver
 * name
 */
const char *btrfs_super_csum_driver(u16 csum_type)
{
	/* csum type is validated at mount time */
	return btrfs_csums[csum_type].driver[0] ?
		btrfs_csums[csum_type].driver :
		btrfs_csums[csum_type].name;
}

size_t __attribute_const__ btrfs_get_num_csums(void)
{
	return ARRAY_SIZE(btrfs_csums);
}

u16 btrfs_csum_type_size(u16 csum_type)
{
	return btrfs_csums[csum_type].size;
}

u64 btrfs_name_hash(const char *name, int len)
{
	return crc32c((u32)~1, name, len);
}

/*
 * Figure the key offset of an extended inode ref
 */
u64 btrfs_extref_hash(u64 parent_objectid, const char *name, int len)
{
	return (u64)crc32c(parent_objectid, name, len);
}

inline void btrfs_init_path(struct btrfs_path *p)
{
	memset(p, 0, sizeof(*p));
}

struct btrfs_path *btrfs_alloc_path(void)
{
	might_sleep();

	return kzalloc(sizeof(struct btrfs_path), GFP_NOFS);
}

/* this also releases the path */
void btrfs_free_path(struct btrfs_path *p)
{
	if (!p)
		return;
	btrfs_release_path(p);
	kfree(p);
}

/*
 * path release drops references on the extent buffers in the path
 * and it drops any locks held by this path
 *
 * It is safe to call this on paths that no locks or extent buffers held.
 */
noinline void btrfs_release_path(struct btrfs_path *p)
{
	int i;

	for (i = 0; i < BTRFS_MAX_LEVEL; i++) {
		p->slots[i] = 0;
		if (!p->nodes[i])
			continue;
		if (p->locks[i]) {
			btrfs_tree_unlock_rw(p->nodes[i], p->locks[i]);
			p->locks[i] = 0;
		}
		free_extent_buffer(p->nodes[i]);
		p->nodes[i] = NULL;
	}
	memset(p, 0, sizeof(*p));
}

/*
 * We want the transaction abort to print stack trace only for errors where the
 * cause could be a bug, eg. due to ENOSPC, and not for common errors that are
 * caused by external factors.
 */
bool __cold abort_should_print_stack(int errno)
{
	switch (errno) {
	case -EIO:
	case -EROFS:
	case -ENOMEM:
		return false;
	}
	return true;
}

static void add_root_to_dirty_list(struct btrfs_root *root)
{
	if (test_bit(BTRFS_ROOT_TRACK_DIRTY, &root->state) &&
	    list_empty(&root->dirty_list)) {
		list_add(&root->dirty_list,
			 &root->fs_info->dirty_cowonly_roots);
	}
}

static void root_add_used(struct btrfs_root *root, u32 size)
{
        btrfs_set_root_used(&root->root_item,
                            btrfs_root_used(&root->root_item) + size);
}

static void root_sub_used(struct btrfs_root *root, u32 size)
{
        btrfs_set_root_used(&root->root_item,
                            btrfs_root_used(&root->root_item) - size);
}

int btrfs_copy_root(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root,
		      struct extent_buffer *buf,
		      struct extent_buffer **cow_ret, u64 new_root_objectid)
{
	struct extent_buffer *cow;
	int ret = 0;
	int level;
	struct btrfs_root *new_root;
	struct btrfs_disk_key disk_key;

	new_root = kmalloc(sizeof(*new_root), GFP_NOFS);
	if (!new_root)
		return -ENOMEM;

	memcpy(new_root, root, sizeof(*new_root));
	new_root->root_key.objectid = new_root_objectid;

	WARN_ON(test_bit(BTRFS_ROOT_SHAREABLE, &root->state) &&
		trans->transid != root->fs_info->running_transaction->transid);
	WARN_ON(test_bit(BTRFS_ROOT_SHAREABLE, &root->state) &&
		trans->transid != root->last_trans);

	level = btrfs_header_level(buf);
	if (level == 0)
		btrfs_item_key(buf, &disk_key, 0);
	else
		btrfs_node_key(buf, &disk_key, 0);

	cow = btrfs_alloc_tree_block(trans, new_root, buf->len,
				     new_root_objectid, &disk_key,
				     level, buf->start, 0,
				     BTRFS_NESTING_NORMAL);
	if (IS_ERR(cow)) {
		kfree(new_root);
		return PTR_ERR(cow);
	}

	copy_extent_buffer_full(cow, buf);
	btrfs_set_header_bytenr(cow, cow->start);
	btrfs_set_header_generation(cow, trans->transid);
	btrfs_set_header_backref_rev(cow, BTRFS_MIXED_BACKREF_REV);
	btrfs_clear_header_flag(cow, BTRFS_HEADER_FLAG_WRITTEN |
				     BTRFS_HEADER_FLAG_RELOC);
	if (new_root_objectid == BTRFS_TREE_RELOC_OBJECTID)
		btrfs_set_header_flag(cow, BTRFS_HEADER_FLAG_RELOC);
	else
		btrfs_set_header_owner(cow, new_root_objectid);

	write_extent_buffer_fsid(cow, root->fs_info->fs_devices->metadata_uuid);

	WARN_ON(btrfs_header_generation(buf) > trans->transid);
	ret = btrfs_inc_ref(trans, new_root, cow, 0);
	kfree(new_root);

	if (ret)
		return ret;

	btrfs_mark_buffer_dirty(cow);
	*cow_ret = cow;
	return 0;
}

/*
 * Create a new tree root, with root objectid set to @objectid.
 *
 * NOTE: Doesn't support tree with non-zero offset, like data reloc tree.
 */
int btrfs_create_root(struct btrfs_trans_handle *trans,
		      struct btrfs_fs_info *fs_info, u64 objectid)
{
	struct extent_buffer *node;
	struct btrfs_root *new_root;
	struct btrfs_disk_key disk_key;
	struct btrfs_key location;
	struct btrfs_root_item root_item = { 0 };
	int ret;

	new_root = malloc(sizeof(*new_root));
	if (!new_root)
		return -ENOMEM;

	btrfs_setup_root(new_root, fs_info, objectid);
	if (!is_fstree(objectid))
		set_bit(BTRFS_ROOT_TRACK_DIRTY, &new_root->state);
	add_root_to_dirty_list(new_root);

	new_root->objectid = objectid;
	new_root->root_key.objectid = objectid;
	new_root->root_key.type = BTRFS_ROOT_ITEM_KEY;
	new_root->root_key.offset = 0;

	node = btrfs_alloc_tree_block(trans, new_root, fs_info->nodesize,
				      objectid, &disk_key, 0, 0, 0,
				      BTRFS_NESTING_NORMAL);
	if (IS_ERR(node)) {
		ret = PTR_ERR(node);
		error("failed to create root node for tree %llu: %d (%m)",
		      objectid, ret);
		return ret;
	}
	new_root->node = node;

	memset_extent_buffer(node, 0, 0, sizeof(struct btrfs_header));
	btrfs_set_header_bytenr(node, node->start);
	btrfs_set_header_generation(node, trans->transid);
	btrfs_set_header_backref_rev(node, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(node, objectid);
	write_extent_buffer_fsid(node, fs_info->fs_devices->metadata_uuid);
	write_extent_buffer_chunk_tree_uuid(node, fs_info->chunk_tree_uuid);
	btrfs_set_header_nritems(node, 0);
	btrfs_set_header_level(node, 0);
	ret = btrfs_inc_ref(trans, new_root, node, 0);
	if (ret < 0)
		goto free;

	/*
	 * Special tree roots may need to modify pointers in @fs_info
	 * Only quota is supported yet.
	 */
	switch (objectid) {
	case BTRFS_QUOTA_TREE_OBJECTID:
		if (fs_info->quota_root) {
			error("quota root already exists");
			ret = -EEXIST;
			goto free;
		}
		fs_info->quota_root = new_root;
		fs_info->quota_enabled = 1;
		break;
	case BTRFS_BLOCK_GROUP_TREE_OBJECTID:
		if (fs_info->block_group_root) {
			error("bg root already exists");
			ret = -EEXIST;
			goto free;
		}
		fs_info->block_group_root = new_root;
		break;

	/*
	 * Essential trees can't be created by this function, yet.
	 * As we expect such skeleton exists, or a lot of functions like
	 * btrfs_alloc_tree_block() doesn't work at all
	 */
	case BTRFS_ROOT_TREE_OBJECTID:
	case BTRFS_EXTENT_TREE_OBJECTID:
	case BTRFS_CHUNK_TREE_OBJECTID:
	case BTRFS_FS_TREE_OBJECTID:
		ret = -EEXIST;
		goto free;
	default:
		/* Subvolume trees don't need special handling */
		if (is_fstree(objectid))
			break;
		/* Other special trees are not supported yet */
		ret = -ENOTTY;
		goto free;
	}
	btrfs_mark_buffer_dirty(node);
	btrfs_set_root_bytenr(&root_item, btrfs_header_bytenr(node));
	btrfs_set_root_level(&root_item, 0);
	btrfs_set_root_generation(&root_item, trans->transid);
	btrfs_set_root_dirid(&root_item, 0);
	btrfs_set_root_refs(&root_item, 1);
	btrfs_set_root_used(&root_item, fs_info->nodesize);
	location.objectid = objectid;
	location.type = BTRFS_ROOT_ITEM_KEY;
	location.offset = 0;

	ret = btrfs_insert_root(trans, fs_info->tree_root, &location, &root_item);
	if (ret < 0)
		goto free;
	return ret;

free:
	free_extent_buffer(node);
	free(new_root);
	return ret;
}

/*
 * check if the tree block can be shared by multiple trees
 */
static int btrfs_block_can_be_shared(struct btrfs_root *root,
			             struct extent_buffer *buf)
{
	/*
	 * Tree blocks not in shareable trees and tree roots are never shared.
	 * If a block was allocated after the last snapshot and the block was
	 * not allocated by tree relocation, we know the block is not shared.
	 */
	if (test_bit(BTRFS_ROOT_SHAREABLE, &root->state) &&
	    buf != root->node && buf != root->commit_root &&
	    (btrfs_header_generation(buf) <=
	     btrfs_root_last_snapshot(&root->root_item) ||
	     btrfs_header_flag(buf, BTRFS_HEADER_FLAG_RELOC)))
		return 1;

	return 0;
}

static noinline int update_ref_for_cow(struct btrfs_trans_handle *trans,
				       struct btrfs_root *root,
				       struct extent_buffer *buf,
				       struct extent_buffer *cow)
{
	u64 refs;
	u64 owner;
	u64 flags;
	u64 new_flags = 0;
	int ret;

	/*
	 * Backrefs update rules:
	 *
	 * Always use full backrefs for extent pointers in tree block
	 * allocated by tree relocation.
	 *
	 * If a shared tree block is no longer referenced by its owner
	 * tree (btrfs_header_owner(buf) == root->root_key.objectid),
	 * use full backrefs for extent pointers in tree block.
	 *
	 * If a tree block is been relocating
	 * (root->root_key.objectid == BTRFS_TREE_RELOC_OBJECTID),
	 * use full backrefs for extent pointers in tree block.
	 * The reason for this is some operations (such as drop tree)
	 * are only allowed for blocks use full backrefs.
	 */

	if (btrfs_block_can_be_shared(root, buf)) {
		ret = btrfs_lookup_extent_info(trans, trans->fs_info,
					       buf->start,
					       btrfs_header_level(buf), 1,
					       &refs, &flags);
		BUG_ON(ret);
		BUG_ON(refs == 0);
	} else {
		refs = 1;
		if (root->root_key.objectid == BTRFS_TREE_RELOC_OBJECTID ||
		    btrfs_header_backref_rev(buf) < BTRFS_MIXED_BACKREF_REV)
			flags = BTRFS_BLOCK_FLAG_FULL_BACKREF;
		else
			flags = 0;
	}

	owner = btrfs_header_owner(buf);
	BUG_ON(!(flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) &&
	       owner == BTRFS_TREE_RELOC_OBJECTID);

	if (refs > 1) {
		if ((owner == root->root_key.objectid ||
		     root->root_key.objectid == BTRFS_TREE_RELOC_OBJECTID) &&
		    !(flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)) {
			ret = btrfs_inc_ref(trans, root, buf, 1);
			BUG_ON(ret);

			if (root->root_key.objectid ==
			    BTRFS_TREE_RELOC_OBJECTID) {
				ret = btrfs_dec_ref(trans, root, buf, 0);
				BUG_ON(ret);
				ret = btrfs_inc_ref(trans, root, cow, 1);
				BUG_ON(ret);
			}
			new_flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
		} else {

			if (root->root_key.objectid ==
			    BTRFS_TREE_RELOC_OBJECTID)
				ret = btrfs_inc_ref(trans, root, cow, 1);
			else
				ret = btrfs_inc_ref(trans, root, cow, 0);
			BUG_ON(ret);
		}
		if (new_flags != 0) {
			ret = btrfs_set_disk_extent_flags(trans, buf, new_flags);
			BUG_ON(ret);
		}
	} else {
		if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
			if (root->root_key.objectid ==
			    BTRFS_TREE_RELOC_OBJECTID)
				ret = btrfs_inc_ref(trans, root, cow, 1);
			else
				ret = btrfs_inc_ref(trans, root, cow, 0);
			BUG_ON(ret);
			ret = btrfs_dec_ref(trans, root, buf, 1);
			BUG_ON(ret);
		}
		btrfs_clear_buffer_dirty(trans, buf);
	}
	return 0;
}

/*
 * does the dirty work in cow of a single block.  The parent block (if
 * supplied) is updated to point to the new cow copy.  The new buffer is marked
 * dirty and returned locked.  If you modify the block it needs to be marked
 * dirty again.
 *
 * search_start -- an allocation hint for the new block
 *
 * empty_size -- a hint that you plan on doing more cow.  This is the size in
 * bytes the allocator should try to find free next to the block it returns.
 * This is just a hint and may be ignored by the allocator.
 */
static noinline int __btrfs_cow_block(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     struct extent_buffer *buf,
			     struct extent_buffer *parent, int parent_slot,
			     struct extent_buffer **cow_ret,
			     u64 search_start, u64 empty_size)
{
	struct extent_buffer *cow;
	struct btrfs_disk_key disk_key;
	int level;

	WARN_ON(test_bit(BTRFS_ROOT_SHAREABLE, &root->state) &&
		trans->transid != root->fs_info->running_transaction->transid);
	WARN_ON(test_bit(BTRFS_ROOT_SHAREABLE, &root->state) &&
		trans->transid != root->last_trans);

	level = btrfs_header_level(buf);

	if (level == 0)
		btrfs_item_key(buf, &disk_key, 0);
	else
		btrfs_node_key(buf, &disk_key, 0);

	cow = btrfs_alloc_tree_block(trans, root, buf->len,
				     root->root_key.objectid, &disk_key,
				     level, search_start, empty_size,
				     BTRFS_NESTING_NORMAL);
	if (IS_ERR(cow))
		return PTR_ERR(cow);

	copy_extent_buffer_full(cow, buf);
	btrfs_set_header_bytenr(cow, cow->start);
	btrfs_set_header_generation(cow, trans->transid);
	btrfs_set_header_backref_rev(cow, BTRFS_MIXED_BACKREF_REV);
	btrfs_clear_header_flag(cow, BTRFS_HEADER_FLAG_WRITTEN |
				     BTRFS_HEADER_FLAG_RELOC);
	if (root->root_key.objectid == BTRFS_TREE_RELOC_OBJECTID)
		btrfs_set_header_flag(cow, BTRFS_HEADER_FLAG_RELOC);
	else
		btrfs_set_header_owner(cow, root->root_key.objectid);

	write_extent_buffer_fsid(cow, root->fs_info->fs_devices->metadata_uuid);

	WARN_ON(!(buf->flags & EXTENT_BUFFER_BAD_TRANSID) &&
		btrfs_header_generation(buf) > trans->transid);

	update_ref_for_cow(trans, root, buf, cow);

	if (buf == root->node) {
		root->node = cow;
		extent_buffer_get(cow);

		btrfs_free_extent(trans, buf->start, buf->len, 0,
				  root->root_key.objectid, level, 0);
		free_extent_buffer(buf);
		add_root_to_dirty_list(root);
	} else {
		btrfs_set_node_blockptr(parent, parent_slot,
					cow->start);
		WARN_ON(trans->transid == 0);
		btrfs_set_node_ptr_generation(parent, parent_slot,
					      trans->transid);
		btrfs_mark_buffer_dirty(parent);
		WARN_ON(btrfs_header_generation(parent) != trans->transid);

		btrfs_free_extent(trans, buf->start, buf->len, 0,
				  root->root_key.objectid, level, 0);
	}
	if (!list_empty(&buf->recow)) {
		list_del_init(&buf->recow);
		free_extent_buffer(buf);
	}
	free_extent_buffer(buf);
	btrfs_mark_buffer_dirty(cow);
	*cow_ret = cow;
	return 0;
}

static inline int should_cow_block(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct extent_buffer *buf)
{
	if (btrfs_header_generation(buf) == trans->transid &&
	    !btrfs_header_flag(buf, BTRFS_HEADER_FLAG_WRITTEN) &&
	    !(root->root_key.objectid != BTRFS_TREE_RELOC_OBJECTID &&
	      btrfs_header_flag(buf, BTRFS_HEADER_FLAG_RELOC)))
		return 0;
	return 1;
}

int btrfs_cow_block(struct btrfs_trans_handle *trans,
		    struct btrfs_root *root, struct extent_buffer *buf,
		    struct extent_buffer *parent, int parent_slot,
		    struct extent_buffer **cow_ret)
{
	u64 search_start;
	int ret;
	/*
	if (trans->transaction != root->fs_info->running_transaction) {
		printk(KERN_CRIT "trans %llu running %llu\n", trans->transid,
		       root->fs_info->running_transaction->transid);
		WARN_ON(1);
	}
	*/
	if (trans->transid != root->fs_info->generation) {
		printk(KERN_CRIT "trans %llu running %llu\n",
			(unsigned long long)trans->transid,
			(unsigned long long)root->fs_info->generation);
		WARN_ON(1);
	}
	if (!should_cow_block(trans, root, buf)) {
		*cow_ret = buf;
		return 0;
	}

	search_start = buf->start & ~((u64)SZ_1G - 1);
	ret = __btrfs_cow_block(trans, root, buf, parent,
				 parent_slot, cow_ret, search_start, 0);
	return ret;
}

/*
 * helper function for defrag to decide if two blocks pointed to by a
 * node are actually close by
 */
static __attribute__((unused)) int close_blocks(u64 blocknr, u64 other, u32 blocksize)
{
	if (blocknr < other && other - (blocknr + blocksize) < 32768)
		return 1;
	if (blocknr > other && blocknr - (other + blocksize) < 32768)
		return 1;
	return 0;
}


/*
 * same as comp_keys only with two btrfs_key's
 */
int __pure btrfs_comp_cpu_keys(const struct btrfs_key *k1, const struct btrfs_key *k2)
{
	if (k1->objectid > k2->objectid)
		return 1;
	if (k1->objectid < k2->objectid)
		return -1;
	if (k1->type > k2->type)
		return 1;
	if (k1->type < k2->type)
		return -1;
	if (k1->offset > k2->offset)
		return 1;
	if (k1->offset < k2->offset)
		return -1;
	return 0;
}

/*
 * compare two keys in a memcmp fashion
 */
static int btrfs_comp_keys(struct btrfs_disk_key *disk,
		const struct btrfs_key *k2)
{
	struct btrfs_key k1;

	btrfs_disk_key_to_cpu(&k1, disk);
	return btrfs_comp_cpu_keys(&k1, k2);
}

static int noinline check_block(struct btrfs_fs_info *fs_info,
				struct btrfs_path *path, int level)
{
	enum btrfs_tree_block_status ret;

	if (path->skip_check_block)
		return 0;
	if (level == 0)
		ret = __btrfs_check_leaf(path->nodes[0]);
	else
		ret = __btrfs_check_node(path->nodes[level]);
	if (ret == BTRFS_TREE_BLOCK_CLEAN)
		return 0;
	return -EIO;
}

/*
 * search for key in the extent_buffer.  The items start at offset p,
 * and they are item_size apart.  There are 'max' items in p.
 *
 * the slot in the array is returned via slot, and it points to
 * the place where you would insert key if it is not found in
 * the array.
 *
 * slot may point to max if the key is bigger than all of the keys
 */
static int generic_bin_search(struct extent_buffer *eb, unsigned long p,
			      int item_size, const struct btrfs_key *key,
			      int max, int *slot)
{
	int low = 0;
	int high = max;
	int mid;
	int ret;
	unsigned long offset;
	struct btrfs_disk_key *tmp;

	while(low < high) {
		mid = (low + high) / 2;
		offset = p + mid * item_size;

		tmp = (struct btrfs_disk_key *)(eb->data + offset);
		ret = btrfs_comp_keys(tmp, key);

		if (ret < 0)
			low = mid + 1;
		else if (ret > 0)
			high = mid;
		else {
			*slot = mid;
			return 0;
		}
	}
	*slot = low;
	return 1;
}

/*
 * simple bin_search frontend that does the right thing for
 * leaves vs nodes
 */
int btrfs_bin_search(struct extent_buffer *eb, int first_slot,
		     const struct btrfs_key *key, int *slot)
{
	if (btrfs_header_level(eb) == 0)
		return generic_bin_search(eb,
					  offsetof(struct btrfs_leaf, items),
					  sizeof(struct btrfs_item),
					  key, btrfs_header_nritems(eb),
					  slot);
	else
		return generic_bin_search(eb,
					  offsetof(struct btrfs_node, ptrs),
					  sizeof(struct btrfs_key_ptr),
					  key, btrfs_header_nritems(eb),
					  slot);
}

struct extent_buffer *btrfs_read_node_slot(struct extent_buffer *parent,
					   int slot)
{
	struct btrfs_fs_info *fs_info = parent->fs_info;
	struct extent_buffer *ret;
	int level = btrfs_header_level(parent);

	if (slot < 0)
		return NULL;
	if (slot >= btrfs_header_nritems(parent))
		return NULL;

	if (level == 0)
		return NULL;

	ret = read_tree_block(fs_info, btrfs_node_blockptr(parent, slot),
			      btrfs_header_owner(parent),
			      btrfs_node_ptr_generation(parent, slot),
			      level - 1, NULL);
	if (!extent_buffer_uptodate(ret))
		return ERR_PTR(-EIO);

	if (btrfs_header_level(ret) != level - 1) {
		error(
"child eb corrupted: parent bytenr=%llu item=%d parent level=%d child bytenr=%llu child level=%d",
		      btrfs_header_bytenr(parent), slot, btrfs_header_level(parent),
		      btrfs_header_bytenr(ret), btrfs_header_level(ret));
		free_extent_buffer(ret);
		return ERR_PTR(-EIO);
	}
	return ret;
}

/*
 * node level balancing, used to make sure nodes are in proper order for
 * item deletion.  We balance from the top down, so we have to make sure
 * that a deletion won't leave an node completely empty later on.
 */
static noinline int balance_level(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root,
			 struct btrfs_path *path, int level)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *right = NULL;
	struct extent_buffer *mid;
	struct extent_buffer *left = NULL;
	struct extent_buffer *parent = NULL;
	int ret = 0;
	int wret;
	int pslot;
	int orig_slot = path->slots[level];
	u64 orig_ptr;

	if (level == 0)
		return 0;

	mid = path->nodes[level];
	WARN_ON(btrfs_header_generation(mid) != trans->transid);

	orig_ptr = btrfs_node_blockptr(mid, orig_slot);

	if (level < BTRFS_MAX_LEVEL - 1) {
		parent = path->nodes[level + 1];
		pslot = path->slots[level + 1];
	}

	/*
	 * deal with the case where there is only one pointer in the root
	 * by promoting the node below to a root
	 */
	if (!parent) {
		struct extent_buffer *child;

		if (btrfs_header_nritems(mid) != 1)
			return 0;

		/* promote the child to a root */
		child = btrfs_read_node_slot(mid, 0);
		BUG_ON(!extent_buffer_uptodate(child));
		ret = btrfs_cow_block(trans, root, child, mid, 0, &child);
		BUG_ON(ret);

		root->node = child;
		add_root_to_dirty_list(root);
		path->nodes[level] = NULL;
		btrfs_clear_buffer_dirty(trans, mid);
		/* once for the path */
		free_extent_buffer(mid);

		root_sub_used(root, mid->len);

		ret = btrfs_free_extent(trans, mid->start, mid->len, 0,
					root->root_key.objectid, level, 0);
		/* once for the root ptr */
		free_extent_buffer(mid);
		return ret;
	}
	if (btrfs_header_nritems(mid) >
	    BTRFS_NODEPTRS_PER_BLOCK(fs_info) / 4)
		return 0;

	left = btrfs_read_node_slot(parent, pslot - 1);
	if (extent_buffer_uptodate(left)) {
		wret = btrfs_cow_block(trans, root, left,
				       parent, pslot - 1, &left);
		if (wret) {
			ret = wret;
			goto enospc;
		}
	}
	right = btrfs_read_node_slot(parent, pslot + 1);
	if (extent_buffer_uptodate(right)) {
		wret = btrfs_cow_block(trans, root, right,
				       parent, pslot + 1, &right);
		if (wret) {
			ret = wret;
			goto enospc;
		}
	}

	/* first, try to make some room in the middle buffer */
	if (left) {
		orig_slot += btrfs_header_nritems(left);
		wret = push_node_left(trans, left, mid, 1);
		if (wret < 0)
			ret = wret;
	}

	/*
	 * then try to empty the right most buffer into the middle
	 */
	if (right) {
		wret = push_node_left(trans, mid, right, 1);
		if (wret < 0 && wret != -ENOSPC)
			ret = wret;
		if (btrfs_header_nritems(right) == 0) {
			u64 bytenr = right->start;
			u32 blocksize = right->len;

			btrfs_clear_buffer_dirty(trans, right);
			free_extent_buffer(right);
			right = NULL;
			wret = btrfs_del_ptr(root, path, level + 1, pslot + 1);
			if (wret)
				ret = wret;

			root_sub_used(root, blocksize);
			wret = btrfs_free_extent(trans, bytenr, blocksize, 0,
						 root->root_key.objectid, level,
						 0);
			if (wret)
				ret = wret;
		} else {
			struct btrfs_disk_key right_key;
			btrfs_node_key(right, &right_key, 0);
			btrfs_set_node_key(parent, &right_key, pslot + 1);
			btrfs_mark_buffer_dirty(parent);
		}
	}
	if (btrfs_header_nritems(mid) == 1) {
		/*
		 * we're not allowed to leave a node with one item in the
		 * tree during a delete.  A deletion from lower in the tree
		 * could try to delete the only pointer in this node.
		 * So, pull some keys from the left.
		 * There has to be a left pointer at this point because
		 * otherwise we would have pulled some pointers from the
		 * right
		 */
		BUG_ON(!left);
		wret = balance_node_right(trans, mid, left);
		if (wret < 0) {
			ret = wret;
			goto enospc;
		}
		if (wret == 1) {
			wret = push_node_left(trans, left, mid, 1);
			if (wret < 0)
				ret = wret;
		}
		BUG_ON(wret == 1);
	}
	if (btrfs_header_nritems(mid) == 0) {
		/* we've managed to empty the middle node, drop it */
		u64 bytenr = mid->start;
		u32 blocksize = mid->len;
		btrfs_clear_buffer_dirty(trans, mid);
		free_extent_buffer(mid);
		mid = NULL;
		wret = btrfs_del_ptr(root, path, level + 1, pslot);
		if (wret)
			ret = wret;

		root_sub_used(root, blocksize);
		wret = btrfs_free_extent(trans, bytenr, blocksize, 0,
					 root->root_key.objectid, level, 0);
		if (wret)
			ret = wret;
	} else {
		/* update the parent key to reflect our changes */
		struct btrfs_disk_key mid_key;
		btrfs_node_key(mid, &mid_key, 0);
		btrfs_set_node_key(parent, &mid_key, pslot);
		btrfs_mark_buffer_dirty(parent);
	}

	/* update the path */
	if (left) {
		if (btrfs_header_nritems(left) > orig_slot) {
			extent_buffer_get(left);
			path->nodes[level] = left;
			path->slots[level + 1] -= 1;
			path->slots[level] = orig_slot;
			if (mid)
				free_extent_buffer(mid);
		} else {
			orig_slot -= btrfs_header_nritems(left);
			path->slots[level] = orig_slot;
		}
	}
	/* double check we haven't messed things up */
	check_block(root->fs_info, path, level);
	if (orig_ptr !=
	    btrfs_node_blockptr(path->nodes[level], path->slots[level]))
		BUG();
enospc:
	if (right)
		free_extent_buffer(right);
	if (left)
		free_extent_buffer(left);
	return ret;
}

/* Node balancing for insertion.  Here we only split or push nodes around
 * when they are completely full.  This is also done top down, so we
 * have to be pessimistic.
 */
static noinline int push_nodes_for_insert(struct btrfs_trans_handle *trans,
					  struct btrfs_root *root,
					  struct btrfs_path *path, int level)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct extent_buffer *right = NULL;
	struct extent_buffer *mid;
	struct extent_buffer *left = NULL;
	struct extent_buffer *parent = NULL;
	int ret = 0;
	int wret;
	int pslot;
	int orig_slot = path->slots[level];

	if (level == 0)
		return 1;

	mid = path->nodes[level];
	WARN_ON(btrfs_header_generation(mid) != trans->transid);

	if (level < BTRFS_MAX_LEVEL - 1) {
		parent = path->nodes[level + 1];
		pslot = path->slots[level + 1];
	}

	if (!parent)
		return 1;

	left = btrfs_read_node_slot(parent, pslot - 1);

	/* first, try to make some room in the middle buffer */
	if (extent_buffer_uptodate(left)) {
		u32 left_nr;
		left_nr = btrfs_header_nritems(left);
		if (left_nr >= BTRFS_NODEPTRS_PER_BLOCK(fs_info) - 1) {
			wret = 1;
		} else {
			ret = btrfs_cow_block(trans, root, left, parent,
					      pslot - 1, &left);
			if (ret)
				wret = 1;
			else {
				wret = push_node_left(trans, left, mid, 0);
			}
		}
		if (wret < 0)
			ret = wret;
		if (wret == 0) {
			struct btrfs_disk_key disk_key;
			orig_slot += left_nr;
			btrfs_node_key(mid, &disk_key, 0);
			btrfs_set_node_key(parent, &disk_key, pslot);
			btrfs_mark_buffer_dirty(parent);
			if (btrfs_header_nritems(left) > orig_slot) {
				path->nodes[level] = left;
				path->slots[level + 1] -= 1;
				path->slots[level] = orig_slot;
				free_extent_buffer(mid);
			} else {
				orig_slot -=
					btrfs_header_nritems(left);
				path->slots[level] = orig_slot;
				free_extent_buffer(left);
			}
			return 0;
		}
		free_extent_buffer(left);
	}
	right= btrfs_read_node_slot(parent, pslot + 1);

	/*
	 * then try to empty the right most buffer into the middle
	 */
	if (extent_buffer_uptodate(right)) {
		u32 right_nr;
		right_nr = btrfs_header_nritems(right);
		if (right_nr >= BTRFS_NODEPTRS_PER_BLOCK(root->fs_info) - 1) {
			wret = 1;
		} else {
			ret = btrfs_cow_block(trans, root, right,
					      parent, pslot + 1,
					      &right);
			if (ret)
				wret = 1;
			else {
				wret = balance_node_right(trans, right, mid);
			}
		}
		if (wret < 0)
			ret = wret;
		if (wret == 0) {
			struct btrfs_disk_key disk_key;

			btrfs_node_key(right, &disk_key, 0);
			btrfs_set_node_key(parent, &disk_key, pslot + 1);
			btrfs_mark_buffer_dirty(parent);

			if (btrfs_header_nritems(mid) <= orig_slot) {
				path->nodes[level] = right;
				path->slots[level + 1] += 1;
				path->slots[level] = orig_slot -
					btrfs_header_nritems(mid);
				free_extent_buffer(mid);
			} else {
				free_extent_buffer(right);
			}
			return 0;
		}
		free_extent_buffer(right);
	}
	return 1;
}

/*
 * readahead one full node of leaves, finding things that are close
 * to the block in 'slot', and triggering ra on them.
 */
static void reada_for_search(struct btrfs_fs_info *fs_info,
			     struct btrfs_path *path,
			     int level, int slot, u64 objectid)
{
	struct extent_buffer *node;
	struct btrfs_disk_key disk_key;
	u32 nritems;
	u64 search;
	u64 lowest_read;
	u64 highest_read;
	u64 nread = 0;
	int direction = path->reada;
	struct extent_buffer *eb;
	u32 nr;
	u32 nscan = 0;

	if (level != 1)
		return;

	if (!path->nodes[level])
		return;

	node = path->nodes[level];
	search = btrfs_node_blockptr(node, slot);
	eb = btrfs_find_tree_block(fs_info, search, fs_info->nodesize);
	if (eb) {
		free_extent_buffer(eb);
		return;
	}

	highest_read = search;
	lowest_read = search;

	nritems = btrfs_header_nritems(node);
	nr = slot;
	while(1) {
		if (direction < 0) {
			if (nr == 0)
				break;
			nr--;
		} else if (direction > 0) {
			nr++;
			if (nr >= nritems)
				break;
		}
		if (path->reada < 0 && objectid) {
			btrfs_node_key(node, &disk_key, nr);
			if (btrfs_disk_key_objectid(&disk_key) != objectid)
				break;
		}
		search = btrfs_node_blockptr(node, nr);
		if ((search >= lowest_read && search <= highest_read) ||
		    (search < lowest_read && lowest_read - search <= 32768) ||
		    (search > highest_read && search - highest_read <= 32768)) {
			readahead_tree_block(fs_info, search,
				     btrfs_node_ptr_generation(node, nr));
			nread += fs_info->nodesize;
		}
		nscan++;
		if (path->reada < 2 && (nread > SZ_256K || nscan > 32))
			break;
		if(nread > SZ_1M || nscan > 128)
			break;

		if (search < lowest_read)
			lowest_read = search;
		if (search > highest_read)
			highest_read = search;
	}
}

int btrfs_find_item(struct btrfs_root *fs_root, struct btrfs_path *found_path,
		u64 iobjectid, u64 ioff, u8 key_type,
		struct btrfs_key *found_key)
{
	int ret;
	struct btrfs_key key;
	struct extent_buffer *eb;
	struct btrfs_path *path;

	key.type = key_type;
	key.objectid = iobjectid;
	key.offset = ioff;

	if (found_path == NULL) {
		path = btrfs_alloc_path();
		if (!path)
			return -ENOMEM;
	} else
		path = found_path;

	ret = btrfs_search_slot(NULL, fs_root, &key, path, 0, 0);
	if ((ret < 0) || (found_key == NULL))
		goto out;

	eb = path->nodes[0];
	if (ret && path->slots[0] >= btrfs_header_nritems(eb)) {
		ret = btrfs_next_leaf(fs_root, path);
		if (ret)
			goto out;
		eb = path->nodes[0];
	}

	btrfs_item_key_to_cpu(eb, found_key, path->slots[0]);
	if (found_key->type != key.type ||
			found_key->objectid != key.objectid) {
		ret = 1;
		goto out;
	}

out:
	if (path != found_path)
		btrfs_free_path(path);
	return ret;
}

/*
 * look for key in the tree.  path is filled in with nodes along the way
 * if key is found, we return zero and you can find the item in the leaf
 * level of the path (level 0)
 *
 * If the key isn't found, the path points to the slot where it should
 * be inserted, and 1 is returned.  If there are other errors during the
 * search a negative error number is returned.
 *
 * if ins_len > 0, nodes and leaves will be split as we walk down the
 * tree.  if ins_len < 0, nodes will be merged as we walk down the tree (if
 * possible)
 */
int btrfs_search_slot(struct btrfs_trans_handle *trans,
		struct btrfs_root *root, const struct btrfs_key *key,
		struct btrfs_path *p, int ins_len, int cow)
{
	struct extent_buffer *b;
	int slot;
	int ret;
	int level;
	int should_reada = p->reada;
	struct btrfs_fs_info *fs_info = root->fs_info;
	u8 lowest_level = 0;

	lowest_level = p->lowest_level;
	WARN_ON(lowest_level && ins_len > 0);
	WARN_ON(p->nodes[0] != NULL);
again:
	b = root->node;
	extent_buffer_get(b);
	while (b) {
		level = btrfs_header_level(b);
		if (cow) {
			int wret;
			wret = btrfs_cow_block(trans, root, b,
					       p->nodes[level + 1],
					       p->slots[level + 1],
					       &b);
			if (wret) {
				free_extent_buffer(b);
				return wret;
			}
		}
		BUG_ON(!cow && ins_len);
		if (level != btrfs_header_level(b))
			WARN_ON(1);
		level = btrfs_header_level(b);
		p->nodes[level] = b;
		ret = check_block(fs_info, p, level);
		if (ret)
			return -1;
		ret = btrfs_bin_search(b, 0, key, &slot);
		if (level != 0) {
			if (ret && slot > 0)
				slot -= 1;
			p->slots[level] = slot;
			if ((p->search_for_split || ins_len > 0) &&
			    btrfs_header_nritems(b) >=
			    BTRFS_NODEPTRS_PER_BLOCK(fs_info) - 3) {
				int sret = split_node(trans, root, p, level);
				BUG_ON(sret > 0);
				if (sret)
					return sret;
				b = p->nodes[level];
				slot = p->slots[level];
			} else if (ins_len < 0) {
				int sret = balance_level(trans, root, p,
							 level);
				if (sret)
					return sret;
				b = p->nodes[level];
				if (!b) {
					btrfs_release_path(p);
					goto again;
				}
				slot = p->slots[level];
				BUG_ON(btrfs_header_nritems(b) == 1);
			}
			/* this is only true while dropping a snapshot */
			if (level == lowest_level)
				break;

			if (should_reada)
				reada_for_search(fs_info, p, level, slot,
						 key->objectid);

			b = btrfs_read_node_slot(b, slot);
			if (!extent_buffer_uptodate(b))
				return -EIO;
		} else {
			p->slots[level] = slot;
			if (ins_len > 0 &&
			    ins_len > btrfs_leaf_free_space(b)) {
				int sret = split_leaf(trans, root, key,
						      p, ins_len, ret == 0);
				BUG_ON(sret > 0);
				if (sret)
					return sret;
			}
			return ret;
		}
	}
	return 1;
}

/*
 * Helper to use instead of search slot if no exact match is needed but
 * instead the next or previous item should be returned.
 * When find_higher is true, the next higher item is returned, the next lower
 * otherwise.
 * When return_any and find_higher are both true, and no higher item is found,
 * return the next lower instead.
 * When return_any is true and find_higher is false, and no lower item is found,
 * return the next higher instead.
 * It returns 0 if any item is found, 1 if none is found (tree empty), and
 * < 0 on error
 */
int btrfs_search_slot_for_read(struct btrfs_root *root,
                               const struct btrfs_key *key,
                               struct btrfs_path *p, int find_higher,
                               int return_any)
{
        int ret;
        struct extent_buffer *leaf;

again:
        ret = btrfs_search_slot(NULL, root, key, p, 0, 0);
        if (ret <= 0)
                return ret;
        /*
	 * A return value of 1 means the path is at the position where the item
	 * should be inserted. Normally this is the next bigger item, but in
	 * case the previous item is the last in a leaf, path points to the
	 * first free slot in the previous leaf, i.e. at an invalid item.
         */
        leaf = p->nodes[0];

        if (find_higher) {
                if (p->slots[0] >= btrfs_header_nritems(leaf)) {
                        ret = btrfs_next_leaf(root, p);
                        if (ret <= 0)
                                return ret;
                        if (!return_any)
                                return 1;
                        /*
			 * No higher item found, return the next lower instead
                         */
                        return_any = 0;
                        find_higher = 0;
                        btrfs_release_path(p);
                        goto again;
                }
        } else {
                if (p->slots[0] == 0) {
                        ret = btrfs_prev_leaf(root, p);
                        if (ret < 0)
                                return ret;
                        if (!ret) {
                                leaf = p->nodes[0];
                                if (p->slots[0] == btrfs_header_nritems(leaf))
                                        p->slots[0]--;
                                return 0;
                        }
                        if (!return_any)
                                return 1;
                        /*
			 * No lower item found, return the next higher instead
                         */
                        return_any = 0;
                        find_higher = 1;
                        btrfs_release_path(p);
                        goto again;
                } else {
                        --p->slots[0];
                }
        }
        return 0;
}

/*
 * adjust the pointers going up the tree, starting at level
 * making sure the right key of each node is points to 'key'.
 * This is used after shifting pointers to the left, so it stops
 * fixing up pointers when a given leaf/node is not in slot 0 of the
 * higher levels
 */
void btrfs_fixup_low_keys( struct btrfs_path *path, struct btrfs_disk_key *key,
		int level)
{
	int i;
	struct extent_buffer *t;

	for (i = level; i < BTRFS_MAX_LEVEL; i++) {
		int tslot = path->slots[i];
		if (!path->nodes[i])
			break;
		t = path->nodes[i];
		btrfs_set_node_key(t, key, tslot);
		btrfs_mark_buffer_dirty(path->nodes[i]);
		if (tslot != 0)
			break;
	}
}

/*
 * update item key.
 *
 * This function isn't completely safe. It's the caller's responsibility
 * that the new key won't break the order
 */
int btrfs_set_item_key_safe(struct btrfs_root *root, struct btrfs_path *path,
			    struct btrfs_key *new_key)
{
	struct btrfs_disk_key disk_key;
	struct extent_buffer *eb;
	int slot;

	eb = path->nodes[0];
	slot = path->slots[0];
	if (slot > 0) {
		btrfs_item_key(eb, &disk_key, slot - 1);
		if (btrfs_comp_keys(&disk_key, new_key) >= 0)
			return -1;
	}
	if (slot < btrfs_header_nritems(eb) - 1) {
		btrfs_item_key(eb, &disk_key, slot + 1);
		if (btrfs_comp_keys(&disk_key, new_key) <= 0)
			return -1;
	}

	btrfs_cpu_key_to_disk(&disk_key, new_key);
	btrfs_set_item_key(eb, &disk_key, slot);
	btrfs_mark_buffer_dirty(eb);
	if (slot == 0)
		btrfs_fixup_low_keys(path, &disk_key, 1);
	return 0;
}

/*
 * update an item key without the safety checks.  This is meant to be called by
 * fsck only.
 */
void btrfs_set_item_key_unsafe(struct btrfs_root *root,
			       struct btrfs_path *path,
			       struct btrfs_key *new_key)
{
	struct btrfs_disk_key disk_key;
	struct extent_buffer *eb;
	int slot;

	eb = path->nodes[0];
	slot = path->slots[0];

	btrfs_cpu_key_to_disk(&disk_key, new_key);
	btrfs_set_item_key(eb, &disk_key, slot);
	btrfs_mark_buffer_dirty(eb);
	if (slot == 0)
		btrfs_fixup_low_keys(path, &disk_key, 1);
}

/*
 * try to push data from one node into the next node left in the
 * tree.
 *
 * returns 0 if some ptrs were pushed left, < 0 if there was some horrible
 * error, and > 0 if there was no room in the left hand block.
 */
static int push_node_left(struct btrfs_trans_handle *trans,
			  struct extent_buffer *dst,
			  struct extent_buffer *src, int empty)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	int push_items = 0;
	int src_nritems;
	int dst_nritems;
	int ret = 0;

	src_nritems = btrfs_header_nritems(src);
	dst_nritems = btrfs_header_nritems(dst);
	push_items = BTRFS_NODEPTRS_PER_BLOCK(fs_info) - dst_nritems;
	WARN_ON(btrfs_header_generation(src) != trans->transid);
	WARN_ON(btrfs_header_generation(dst) != trans->transid);

	if (!empty && src_nritems <= 8)
		return 1;

	if (push_items <= 0) {
		return 1;
	}

	if (empty) {
		push_items = min(src_nritems, push_items);
		if (push_items < src_nritems) {
			/* leave at least 8 pointers in the node if
			 * we aren't going to empty it
			 */
			if (src_nritems - push_items < 8) {
				if (push_items <= 8)
					return 1;
				push_items -= 8;
			}
		}
	} else
		push_items = min(src_nritems - 8, push_items);

	copy_extent_buffer(dst, src,
			   btrfs_node_key_ptr_offset(dst, dst_nritems),
			   btrfs_node_key_ptr_offset(src, 0),
		           push_items * sizeof(struct btrfs_key_ptr));

	if (push_items < src_nritems) {
		memmove_extent_buffer(src, btrfs_node_key_ptr_offset(src, 0),
				      btrfs_node_key_ptr_offset(src, push_items),
				      (src_nritems - push_items) *
				      sizeof(struct btrfs_key_ptr));
	}
	btrfs_set_header_nritems(src, src_nritems - push_items);
	btrfs_set_header_nritems(dst, dst_nritems + push_items);
	btrfs_mark_buffer_dirty(src);
	btrfs_mark_buffer_dirty(dst);

	return ret;
}

/*
 * try to push data from one node into the next node right in the
 * tree.
 *
 * returns 0 if some ptrs were pushed, < 0 if there was some horrible
 * error, and > 0 if there was no room in the right hand block.
 *
 * this will  only push up to 1/2 the contents of the left node over
 */
static int balance_node_right(struct btrfs_trans_handle *trans,
			      struct extent_buffer *dst,
			      struct extent_buffer *src)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	int push_items = 0;
	int max_push;
	int src_nritems;
	int dst_nritems;
	int ret = 0;

	WARN_ON(btrfs_header_generation(src) != trans->transid);
	WARN_ON(btrfs_header_generation(dst) != trans->transid);

	src_nritems = btrfs_header_nritems(src);
	dst_nritems = btrfs_header_nritems(dst);
	push_items = BTRFS_NODEPTRS_PER_BLOCK(fs_info) - dst_nritems;
	if (push_items <= 0) {
		return 1;
	}

	if (src_nritems < 4) {
		return 1;
	}

	max_push = src_nritems / 2 + 1;
	/* don't try to empty the node */
	if (max_push >= src_nritems) {
		return 1;
	}

	if (max_push < push_items)
		push_items = max_push;

	memmove_extent_buffer(dst, btrfs_node_key_ptr_offset(dst, push_items),
				      btrfs_node_key_ptr_offset(dst, 0),
				      (dst_nritems) *
				      sizeof(struct btrfs_key_ptr));

	copy_extent_buffer(dst, src,
			   btrfs_node_key_ptr_offset(dst, 0),
			   btrfs_node_key_ptr_offset(src, src_nritems - push_items),
		           push_items * sizeof(struct btrfs_key_ptr));

	btrfs_set_header_nritems(src, src_nritems - push_items);
	btrfs_set_header_nritems(dst, dst_nritems + push_items);

	btrfs_mark_buffer_dirty(src);
	btrfs_mark_buffer_dirty(dst);

	return ret;
}

/*
 * helper function to insert a new root level in the tree.
 * A new node is allocated, and a single item is inserted to
 * point to the existing root
 *
 * returns zero on success or < 0 on failure.
 */
static int noinline insert_new_root(struct btrfs_trans_handle *trans,
			   struct btrfs_root *root,
			   struct btrfs_path *path, int level)
{
	u64 lower_gen;
	struct extent_buffer *lower;
	struct extent_buffer *c;
	struct extent_buffer *old;
	struct btrfs_disk_key lower_key;

	BUG_ON(path->nodes[level]);
	BUG_ON(path->nodes[level-1] != root->node);

	lower = path->nodes[level-1];
	if (level == 1)
		btrfs_item_key(lower, &lower_key, 0);
	else
		btrfs_node_key(lower, &lower_key, 0);

	c = btrfs_alloc_tree_block(trans, root, root->fs_info->nodesize,
				   root->root_key.objectid, &lower_key,
				   level, root->node->start, 0,
				   BTRFS_NESTING_NORMAL);

	if (IS_ERR(c))
		return PTR_ERR(c);

	memset_extent_buffer(c, 0, 0, sizeof(struct btrfs_header));
	btrfs_set_header_nritems(c, 1);
	btrfs_set_header_level(c, level);
	btrfs_set_header_bytenr(c, c->start);
	btrfs_set_header_generation(c, trans->transid);
	btrfs_set_header_backref_rev(c, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(c, root->root_key.objectid);

	root_add_used(root, root->fs_info->nodesize);

	write_extent_buffer_fsid(c, root->fs_info->fs_devices->metadata_uuid);
	write_extent_buffer_chunk_tree_uuid(c, root->fs_info->chunk_tree_uuid);
	btrfs_set_node_key(c, &lower_key, 0);
	btrfs_set_node_blockptr(c, 0, lower->start);
	lower_gen = btrfs_header_generation(lower);
	WARN_ON(lower_gen != trans->transid);

	btrfs_set_node_ptr_generation(c, 0, lower_gen);

	btrfs_mark_buffer_dirty(c);

	old = root->node;
	root->node = c;

	/* the super has an extra ref to root->node */
	free_extent_buffer(old);

	add_root_to_dirty_list(root);
	extent_buffer_get(c);
	path->nodes[level] = c;
	path->slots[level] = 0;
	return 0;
}

/*
 * worker function to insert a single pointer in a node.
 * the node should have enough room for the pointer already
 *
 * slot and level indicate where you want the key to go, and
 * blocknr is the block the key points to.
 *
 * returns zero on success and < 0 on any error
 */
static int insert_ptr(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_path *path, struct btrfs_disk_key
		      *key, u64 bytenr, int slot, int level)
{
	struct extent_buffer *lower;
	int nritems;

	BUG_ON(!path->nodes[level]);
	lower = path->nodes[level];
	nritems = btrfs_header_nritems(lower);
	if (slot > nritems)
		BUG();
	if (nritems == BTRFS_NODEPTRS_PER_BLOCK(root->fs_info))
		BUG();
	if (slot < nritems) {
		/* shift the items */
		memmove_extent_buffer(lower,
			      btrfs_node_key_ptr_offset(lower, slot + 1),
			      btrfs_node_key_ptr_offset(lower, slot),
			      (nritems - slot) * sizeof(struct btrfs_key_ptr));
	}
	btrfs_set_node_key(lower, key, slot);
	btrfs_set_node_blockptr(lower, slot, bytenr);
	WARN_ON(trans->transid == 0);
	btrfs_set_node_ptr_generation(lower, slot, trans->transid);
	btrfs_set_header_nritems(lower, nritems + 1);
	btrfs_mark_buffer_dirty(lower);
	return 0;
}

/*
 * split the node at the specified level in path in two.
 * The path is corrected to point to the appropriate node after the split
 *
 * Before splitting this tries to make some room in the node by pushing
 * left and right, if either one works, it returns right away.
 *
 * returns 0 on success and < 0 on failure
 */
static int split_node(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_path *path, int level)
{
	struct extent_buffer *c;
	struct extent_buffer *split;
	struct btrfs_disk_key disk_key;
	int mid;
	int ret;
	int wret;
	u32 c_nritems;

	c = path->nodes[level];
	WARN_ON(btrfs_header_generation(c) != trans->transid);
	if (c == root->node) {
		/* trying to split the root, lets make a new one */
		ret = insert_new_root(trans, root, path, level + 1);
		if (ret)
			return ret;
	} else {
		ret = push_nodes_for_insert(trans, root, path, level);
		c = path->nodes[level];
		if (!ret && btrfs_header_nritems(c) <
		    BTRFS_NODEPTRS_PER_BLOCK(root->fs_info) - 3)
			return 0;
		if (ret < 0)
			return ret;
	}

	c_nritems = btrfs_header_nritems(c);
	mid = (c_nritems + 1) / 2;
	btrfs_node_key(c, &disk_key, mid);

	split = btrfs_alloc_tree_block(trans, root, root->fs_info->nodesize,
					root->root_key.objectid,
					&disk_key, level, c->start, 0,
					BTRFS_NESTING_NORMAL);
	if (IS_ERR(split))
		return PTR_ERR(split);

	memset_extent_buffer(split, 0, 0, sizeof(struct btrfs_header));
	btrfs_set_header_level(split, btrfs_header_level(c));
	btrfs_set_header_bytenr(split, split->start);
	btrfs_set_header_generation(split, trans->transid);
	btrfs_set_header_backref_rev(split, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(split, root->root_key.objectid);
	write_extent_buffer_fsid(split, root->fs_info->fs_devices->metadata_uuid);
	write_extent_buffer_chunk_tree_uuid(split, root->fs_info->chunk_tree_uuid);

	root_add_used(root, root->fs_info->nodesize);

	copy_extent_buffer(split, c,
			   btrfs_node_key_ptr_offset(split, 0),
			   btrfs_node_key_ptr_offset(c, mid),
			   (c_nritems - mid) * sizeof(struct btrfs_key_ptr));
	btrfs_set_header_nritems(split, c_nritems - mid);
	btrfs_set_header_nritems(c, mid);
	ret = 0;

	btrfs_mark_buffer_dirty(c);
	btrfs_mark_buffer_dirty(split);

	wret = insert_ptr(trans, root, path, &disk_key, split->start,
			  path->slots[level + 1] + 1,
			  level + 1);
	if (wret)
		ret = wret;

	if (path->slots[level] >= mid) {
		path->slots[level] -= mid;
		free_extent_buffer(c);
		path->nodes[level] = split;
		path->slots[level + 1] += 1;
	} else {
		free_extent_buffer(split);
	}
	return ret;
}

/*
 * how many bytes are required to store the items in a leaf.  start
 * and nr indicate which items in the leaf to check.  This totals up the
 * space used both by the item structs and the item data
 */
static int leaf_space_used(struct extent_buffer *l, int start, int nr)
{
	int data_len;
	int nritems = btrfs_header_nritems(l);
	int end = min(nritems, start + nr) - 1;

	if (!nr)
		return 0;
	data_len = btrfs_item_data_end(l, start);
	data_len = data_len - btrfs_item_offset(l, end);
	data_len += sizeof(struct btrfs_item) * nr;
	WARN_ON(data_len < 0);
	return data_len;
}

/*
 * The space between the end of the leaf items and
 * the start of the leaf data.  IOW, how much room
 * the leaf has left for both items and data
 */
int btrfs_leaf_free_space(struct extent_buffer *leaf)
{
	int nritems = btrfs_header_nritems(leaf);
	u32 leaf_data_size;
	int ret;

	BUG_ON(!leaf->fs_info);
	BUG_ON(leaf->fs_info->nodesize != leaf->len);
	leaf_data_size = BTRFS_LEAF_DATA_SIZE(leaf->fs_info);
	ret = leaf_data_size - leaf_space_used(leaf, 0 ,nritems);
	if (ret < 0) {
		printk("leaf free space ret %d, leaf data size %u, used %d nritems %d\n",
		       ret, leaf_data_size, leaf_space_used(leaf, 0, nritems),
		       nritems);
	}
	return ret;
}

/*
 * push some data in the path leaf to the right, trying to free up at
 * least data_size bytes.  returns zero if the push worked, nonzero otherwise
 *
 * returns 1 if the push failed because the other node didn't have enough
 * room, 0 if everything worked out and < 0 if there were major errors.
 */
static int push_leaf_right(struct btrfs_trans_handle *trans, struct btrfs_root
			   *root, struct btrfs_path *path, int data_size,
			   int empty)
{
	struct extent_buffer *left = path->nodes[0];
	struct extent_buffer *right;
	struct extent_buffer *upper;
	struct btrfs_disk_key disk_key;
	int slot;
	u32 i;
	int free_space;
	int push_space = 0;
	int push_items = 0;
	u32 left_nritems;
	u32 nr;
	u32 right_nritems;
	u32 data_end;
	u32 this_item_size;
	int ret;

	slot = path->slots[1];
	if (!path->nodes[1]) {
		return 1;
	}
	upper = path->nodes[1];
	if (slot >= btrfs_header_nritems(upper) - 1)
		return 1;

	right = btrfs_read_node_slot(upper, slot + 1);
	if (!extent_buffer_uptodate(right)) {
		if (IS_ERR(right))
			return PTR_ERR(right);
		return -EIO;
	}
	free_space = btrfs_leaf_free_space(right);
	if (free_space < data_size) {
		free_extent_buffer(right);
		return 1;
	}

	/* cow and double check */
	ret = btrfs_cow_block(trans, root, right, upper,
			      slot + 1, &right);
	if (ret) {
		free_extent_buffer(right);
		return 1;
	}
	free_space = btrfs_leaf_free_space(right);
	if (free_space < data_size) {
		free_extent_buffer(right);
		return 1;
	}

	left_nritems = btrfs_header_nritems(left);
	if (left_nritems == 0) {
		free_extent_buffer(right);
		return 1;
	}

	if (empty)
		nr = 0;
	else
		nr = 1;

	i = left_nritems - 1;
	while (i >= nr) {
		if (path->slots[0] == i)
			push_space += data_size + sizeof(struct btrfs_item);

		this_item_size = btrfs_item_size(left, i);
		if (this_item_size + sizeof(struct btrfs_item) + push_space > free_space)
			break;
		push_items++;
		push_space += this_item_size + sizeof(struct btrfs_item);
		if (i == 0)
			break;
		i--;
	}

	if (push_items == 0) {
		free_extent_buffer(right);
		return 1;
	}

	if (!empty && push_items == left_nritems)
		WARN_ON(1);

	/* push left to right */
	right_nritems = btrfs_header_nritems(right);

	push_space = btrfs_item_data_end(left, left_nritems - push_items);
	push_space -= leaf_data_end(left);

	/* make room in the right data area */
	data_end = leaf_data_end(right);
	memmove_extent_buffer(right,
			      btrfs_item_nr_offset(right, 0) + data_end - push_space,
			      btrfs_item_nr_offset(right, 0) + data_end,
			      BTRFS_LEAF_DATA_SIZE(root->fs_info) - data_end);

	/* copy from the left data area */
	copy_extent_buffer(right, left, btrfs_item_nr_offset(right, 0) +
		     BTRFS_LEAF_DATA_SIZE(root->fs_info) - push_space,
		     btrfs_item_nr_offset(left, 0) + leaf_data_end(left), push_space);

	memmove_extent_buffer(right, btrfs_item_nr_offset(right, push_items),
			      btrfs_item_nr_offset(right, 0),
			      right_nritems * sizeof(struct btrfs_item));

	/* copy the items from left to right */
	copy_extent_buffer(right, left, btrfs_item_nr_offset(right, 0),
		   btrfs_item_nr_offset(left, left_nritems - push_items),
		   push_items * sizeof(struct btrfs_item));

	/* update the item pointers */
	right_nritems += push_items;
	btrfs_set_header_nritems(right, right_nritems);
	push_space = BTRFS_LEAF_DATA_SIZE(root->fs_info);
	for (i = 0; i < right_nritems; i++) {
		push_space -= btrfs_item_size(right, i);
		btrfs_set_item_offset(right, i, push_space);
	}

	left_nritems -= push_items;
	btrfs_set_header_nritems(left, left_nritems);

	if (left_nritems)
		btrfs_mark_buffer_dirty(left);
	btrfs_mark_buffer_dirty(right);

	btrfs_item_key(right, &disk_key, 0);
	btrfs_set_node_key(upper, &disk_key, slot + 1);
	btrfs_mark_buffer_dirty(upper);

	/* then fixup the leaf pointer in the path */
	if (path->slots[0] >= left_nritems) {
		path->slots[0] -= left_nritems;
		free_extent_buffer(path->nodes[0]);
		path->nodes[0] = right;
		path->slots[1] += 1;
	} else {
		free_extent_buffer(right);
	}
	return 0;
}
/*
 * push some data in the path leaf to the left, trying to free up at
 * least data_size bytes.  returns zero if the push worked, nonzero otherwise
 */
static int push_leaf_left(struct btrfs_trans_handle *trans, struct btrfs_root
			  *root, struct btrfs_path *path, int data_size,
			  int empty)
{
	struct btrfs_disk_key disk_key;
	struct extent_buffer *right = path->nodes[0];
	struct extent_buffer *left;
	int slot;
	int i;
	int free_space;
	int push_space = 0;
	int push_items = 0;
	u32 old_left_nritems;
	u32 right_nritems;
	u32 nr;
	int ret = 0;
	u32 this_item_size;
	u32 old_left_item_size;

	slot = path->slots[1];
	if (slot == 0)
		return 1;
	if (!path->nodes[1])
		return 1;

	right_nritems = btrfs_header_nritems(right);
	if (right_nritems == 0) {
		return 1;
	}

	left = btrfs_read_node_slot(path->nodes[1], slot - 1);
	free_space = btrfs_leaf_free_space(left);
	if (free_space < data_size) {
		free_extent_buffer(left);
		return 1;
	}

	/* cow and double check */
	ret = btrfs_cow_block(trans, root, left,
			      path->nodes[1], slot - 1, &left);
	if (ret) {
		/* we hit -ENOSPC, but it isn't fatal here */
		free_extent_buffer(left);
		return 1;
	}

	free_space = btrfs_leaf_free_space(left);
	if (free_space < data_size) {
		free_extent_buffer(left);
		return 1;
	}

	if (empty)
		nr = right_nritems;
	else
		nr = right_nritems - 1;

	for (i = 0; i < nr; i++) {
		if (path->slots[0] == i)
			push_space += data_size + sizeof(struct btrfs_item);

		this_item_size = btrfs_item_size(right, i);
		if (this_item_size + sizeof(struct btrfs_item) + push_space > free_space)
			break;

		push_items++;
		push_space += this_item_size + sizeof(struct btrfs_item);
	}

	if (push_items == 0) {
		free_extent_buffer(left);
		return 1;
	}
	if (!empty && push_items == btrfs_header_nritems(right))
		WARN_ON(1);

	/* push data from right to left */
	copy_extent_buffer(left, right,
			   btrfs_item_nr_offset(left, btrfs_header_nritems(left)),
			   btrfs_item_nr_offset(right, 0),
			   push_items * sizeof(struct btrfs_item));

	push_space = BTRFS_LEAF_DATA_SIZE(root->fs_info) -
		     btrfs_item_offset(right, push_items -1);

	copy_extent_buffer(left, right, btrfs_item_nr_offset(left, 0) +
		     leaf_data_end(left) - push_space,
		     btrfs_item_nr_offset(right, 0) +
		     btrfs_item_offset(right, push_items - 1),
		     push_space);
	old_left_nritems = btrfs_header_nritems(left);
	BUG_ON(old_left_nritems == 0);

	old_left_item_size = btrfs_item_offset(left, old_left_nritems - 1);
	for (i = old_left_nritems; i < old_left_nritems + push_items; i++) {
		u32 ioff;

		ioff = btrfs_item_offset(left, i);
		btrfs_set_item_offset(left, i,
		      ioff - (BTRFS_LEAF_DATA_SIZE(root->fs_info) -
			      old_left_item_size));
	}
	btrfs_set_header_nritems(left, old_left_nritems + push_items);

	/* fixup right node */
	if (push_items > right_nritems) {
		printk("push items %d nr %u\n", push_items, right_nritems);
		WARN_ON(1);
	}

	if (push_items < right_nritems) {
		push_space = btrfs_item_offset(right, push_items - 1) -
						  leaf_data_end(right);
		memmove_extent_buffer(right, btrfs_item_nr_offset(right, 0) +
				      BTRFS_LEAF_DATA_SIZE(root->fs_info) -
				      push_space,
				      btrfs_item_nr_offset(right, 0) +
				      leaf_data_end(right), push_space);

		memmove_extent_buffer(right, btrfs_item_nr_offset(right, 0),
			      btrfs_item_nr_offset(right, push_items),
			     (btrfs_header_nritems(right) - push_items) *
			     sizeof(struct btrfs_item));
	}
	right_nritems -= push_items;
	btrfs_set_header_nritems(right, right_nritems);
	push_space = BTRFS_LEAF_DATA_SIZE(root->fs_info);
	for (i = 0; i < right_nritems; i++) {
		push_space = push_space - btrfs_item_size(right, i);
		btrfs_set_item_offset(right, i, push_space);
	}

	btrfs_mark_buffer_dirty(left);
	if (right_nritems)
		btrfs_mark_buffer_dirty(right);

	btrfs_item_key(right, &disk_key, 0);
	btrfs_fixup_low_keys(path, &disk_key, 1);

	/* then fixup the leaf pointer in the path */
	if (path->slots[0] < push_items) {
		path->slots[0] += old_left_nritems;
		free_extent_buffer(path->nodes[0]);
		path->nodes[0] = left;
		path->slots[1] -= 1;
	} else {
		free_extent_buffer(left);
		path->slots[0] -= push_items;
	}
	BUG_ON(path->slots[0] < 0);
	return ret;
}

/*
 * split the path's leaf in two, making sure there is at least data_size
 * available for the resulting leaf level of the path.
 *
 * returns 0 if all went well and < 0 on failure.
 */
static noinline int copy_for_split(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root,
			       struct btrfs_path *path,
			       struct extent_buffer *l,
			       struct extent_buffer *right,
			       int slot, int mid, int nritems)
{
	int data_copy_size;
	int rt_data_off;
	int i;
	int ret = 0;
	int wret;
	struct btrfs_disk_key disk_key;

	nritems = nritems - mid;
	btrfs_set_header_nritems(right, nritems);
	data_copy_size = btrfs_item_data_end(l, mid) - leaf_data_end(l);

	copy_extent_buffer(right, l, btrfs_item_nr_offset(right, 0),
			   btrfs_item_nr_offset(l, mid),
			   nritems * sizeof(struct btrfs_item));

	copy_extent_buffer(right, l,
		     btrfs_item_nr_offset(right, 0) +
		     BTRFS_LEAF_DATA_SIZE(root->fs_info) - data_copy_size,
			 btrfs_item_nr_offset(l, 0) + leaf_data_end(l), data_copy_size);

	rt_data_off = BTRFS_LEAF_DATA_SIZE(root->fs_info) -
		      btrfs_item_data_end(l, mid);

	for (i = 0; i < nritems; i++) {
		u32 ioff = btrfs_item_offset(right, i);
		btrfs_set_item_offset(right, i, ioff + rt_data_off);
	}

	btrfs_set_header_nritems(l, mid);
	ret = 0;
	btrfs_item_key(right, &disk_key, 0);
	wret = insert_ptr(trans, root, path, &disk_key, right->start,
			  path->slots[1] + 1, 1);
	if (wret)
		ret = wret;

	btrfs_mark_buffer_dirty(right);
	btrfs_mark_buffer_dirty(l);
	BUG_ON(path->slots[0] != slot);

	if (mid <= slot) {
		free_extent_buffer(path->nodes[0]);
		path->nodes[0] = right;
		path->slots[0] -= mid;
		path->slots[1] += 1;
	} else {
		free_extent_buffer(right);
	}

	BUG_ON(path->slots[0] < 0);

	return ret;
}

/*
 * split the path's leaf in two, making sure there is at least data_size
 * available for the resulting leaf level of the path.
 *
 * returns 0 if all went well and < 0 on failure.
 */
static noinline int split_leaf(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root,
			       const struct btrfs_key *ins_key,
			       struct btrfs_path *path, int data_size,
			       int extend)
{
	struct btrfs_disk_key disk_key;
	struct extent_buffer *l;
	u32 nritems;
	int mid;
	int slot;
	struct extent_buffer *right;
	int ret = 0;
	int wret;
	int split;
	int num_doubles = 0;

	l = path->nodes[0];
	slot = path->slots[0];
	if (extend && data_size + btrfs_item_size(l, slot) +
	    sizeof(struct btrfs_item) > BTRFS_LEAF_DATA_SIZE(root->fs_info))
		return -EOVERFLOW;

	/* first try to make some room by pushing left and right */
	if (data_size && ins_key->type != BTRFS_DIR_ITEM_KEY) {
		wret = push_leaf_right(trans, root, path, data_size, 0);
		if (wret < 0)
			return wret;
		if (wret) {
			wret = push_leaf_left(trans, root, path, data_size, 0);
			if (wret < 0)
				return wret;
		}
		l = path->nodes[0];

		/* did the pushes work? */
		if (btrfs_leaf_free_space(l) >= data_size)
			return 0;
	}

	if (!path->nodes[1]) {
		ret = insert_new_root(trans, root, path, 1);
		if (ret)
			return ret;
	}
again:
	split = 1;
	l = path->nodes[0];
	slot = path->slots[0];
	nritems = btrfs_header_nritems(l);
	mid = (nritems + 1) / 2;

	if (mid <= slot) {
		if (nritems == 1 ||
		    leaf_space_used(l, mid, nritems - mid) + data_size >
			BTRFS_LEAF_DATA_SIZE(root->fs_info)) {
			if (slot >= nritems) {
				split = 0;
			} else {
				mid = slot;
				if (mid != nritems &&
				    leaf_space_used(l, mid, nritems - mid) +
				    data_size >
				    BTRFS_LEAF_DATA_SIZE(root->fs_info)) {
					split = 2;
				}
			}
		}
	} else {
		if (leaf_space_used(l, 0, mid) + data_size >
			BTRFS_LEAF_DATA_SIZE(root->fs_info)) {
			if (!extend && data_size && slot == 0) {
				split = 0;
			} else if ((extend || !data_size) && slot == 0) {
				mid = 1;
			} else {
				mid = slot;
				if (mid != nritems &&
				    leaf_space_used(l, mid, nritems - mid) +
				    data_size >
				    BTRFS_LEAF_DATA_SIZE(root->fs_info)) {
					split = 2 ;
				}
			}
		}
	}

	if (split == 0)
		btrfs_cpu_key_to_disk(&disk_key, ins_key);
	else
		btrfs_item_key(l, &disk_key, mid);

	right = btrfs_alloc_tree_block(trans, root, root->fs_info->nodesize,
					root->root_key.objectid,
					&disk_key, 0, l->start, 0,
					BTRFS_NESTING_NORMAL);
	if (IS_ERR(right)) {
		BUG_ON(1);
		return PTR_ERR(right);
	}

	memset_extent_buffer(right, 0, 0, sizeof(struct btrfs_header));
	btrfs_set_header_bytenr(right, right->start);
	btrfs_set_header_generation(right, trans->transid);
	btrfs_set_header_backref_rev(right, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(right, root->root_key.objectid);
	btrfs_set_header_level(right, 0);
	write_extent_buffer_fsid(right, root->fs_info->fs_devices->metadata_uuid);
	write_extent_buffer_chunk_tree_uuid(right, root->fs_info->chunk_tree_uuid);

	root_add_used(root, root->fs_info->nodesize);

	if (split == 0) {
		if (mid <= slot) {
			btrfs_set_header_nritems(right, 0);
			wret = insert_ptr(trans, root, path,
					  &disk_key, right->start,
					  path->slots[1] + 1, 1);
			if (wret)
				ret = wret;

			free_extent_buffer(path->nodes[0]);
			path->nodes[0] = right;
			path->slots[0] = 0;
			path->slots[1] += 1;
		} else {
			btrfs_set_header_nritems(right, 0);
			wret = insert_ptr(trans, root, path,
					  &disk_key,
					  right->start,
					  path->slots[1], 1);
			if (wret)
				ret = wret;
			free_extent_buffer(path->nodes[0]);
			path->nodes[0] = right;
			path->slots[0] = 0;
			if (path->slots[1] == 0)
				btrfs_fixup_low_keys(path, &disk_key, 1);
		}
		btrfs_mark_buffer_dirty(right);
		return ret;
	}

	ret = copy_for_split(trans, root, path, l, right, slot, mid, nritems);
	BUG_ON(ret);

	if (split == 2) {
		BUG_ON(num_doubles != 0);
		num_doubles++;
		goto again;
	}

	return ret;
}

/*
 * This function splits a single item into two items,
 * giving 'new_key' to the new item and splitting the
 * old one at split_offset (from the start of the item).
 *
 * The path may be released by this operation.  After
 * the split, the path is pointing to the old item.  The
 * new item is going to be in the same node as the old one.
 *
 * Note, the item being split must be smaller enough to live alone on
 * a tree block with room for one extra struct btrfs_item
 *
 * This allows us to split the item in place, keeping a lock on the
 * leaf the entire time.
 */
int btrfs_split_item(struct btrfs_trans_handle *trans,
		     struct btrfs_root *root,
		     struct btrfs_path *path,
		     struct btrfs_key *new_key,
		     unsigned long split_offset)
{
	u32 item_size;
	struct extent_buffer *leaf;
	struct btrfs_key orig_key;
	int ret = 0;
	int slot;
	u32 nritems;
	u32 orig_offset;
	struct btrfs_disk_key disk_key;
	char *buf;

	leaf = path->nodes[0];
	btrfs_item_key_to_cpu(leaf, &orig_key, path->slots[0]);
	if (btrfs_leaf_free_space(leaf) >=
	    sizeof(struct btrfs_item))
		goto split;

	item_size = btrfs_item_size(leaf, path->slots[0]);
	btrfs_release_path(path);

	path->search_for_split = 1;

	ret = btrfs_search_slot(trans, root, &orig_key, path, 0, 1);
	path->search_for_split = 0;

	/* if our item isn't there or got smaller, return now */
	if (ret != 0 || item_size != btrfs_item_size(path->nodes[0],
							path->slots[0])) {
		return -EAGAIN;
	}

	ret = split_leaf(trans, root, &orig_key, path, 0, 0);
	BUG_ON(ret);

	BUG_ON(btrfs_leaf_free_space(leaf) < sizeof(struct btrfs_item));
	leaf = path->nodes[0];

split:
	orig_offset = btrfs_item_offset(leaf, path->slots[0]);
	item_size = btrfs_item_size(leaf, path->slots[0]);


	buf = kmalloc(item_size, GFP_NOFS);
	BUG_ON(!buf);
	read_extent_buffer(leaf, buf, btrfs_item_ptr_offset(leaf,
			    path->slots[0]), item_size);
	slot = path->slots[0] + 1;
	leaf = path->nodes[0];

	nritems = btrfs_header_nritems(leaf);

	if (slot < nritems) {
		/* shift the items */
		memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, slot + 1),
			      btrfs_item_nr_offset(leaf, slot),
			      (nritems - slot) * sizeof(struct btrfs_item));

	}

	btrfs_cpu_key_to_disk(&disk_key, new_key);
	btrfs_set_item_key(leaf, &disk_key, slot);

	btrfs_set_item_offset(leaf, slot, orig_offset);
	btrfs_set_item_size(leaf, slot, item_size - split_offset);

	btrfs_set_item_offset(leaf, path->slots[0],
				 orig_offset + item_size - split_offset);
	btrfs_set_item_size(leaf, path->slots[0], split_offset);

	btrfs_set_header_nritems(leaf, nritems + 1);

	/* write the data for the start of the original item */
	write_extent_buffer(leaf, buf,
			    btrfs_item_ptr_offset(leaf, path->slots[0]),
			    split_offset);

	/* write the data for the new item */
	write_extent_buffer(leaf, buf + split_offset,
			    btrfs_item_ptr_offset(leaf, slot),
			    item_size - split_offset);
	btrfs_mark_buffer_dirty(leaf);

	ret = 0;
	if (btrfs_leaf_free_space(leaf) < 0) {
		btrfs_print_leaf(leaf, BTRFS_PRINT_TREE_DEFAULT);
		BUG();
	}
	kfree(buf);
	return ret;
}

int btrfs_truncate_item(struct btrfs_path *path, u32 new_size, int from_end)
{
	int ret = 0;
	int slot;
	struct extent_buffer *leaf;
	u32 nritems;
	unsigned int data_end;
	unsigned int old_data_start;
	unsigned int old_size;
	unsigned int size_diff;
	int i;

	leaf = path->nodes[0];
	slot = path->slots[0];

	old_size = btrfs_item_size(leaf, slot);
	if (old_size == new_size)
		return 0;

	nritems = btrfs_header_nritems(leaf);
	data_end = leaf_data_end(leaf);

	old_data_start = btrfs_item_offset(leaf, slot);

	size_diff = old_size - new_size;

	BUG_ON(slot < 0);
	BUG_ON(slot >= nritems);

	/*
	 * item0..itemN ... dataN.offset..dataN.size .. data0.size
	 */
	/* first correct the data pointers */
	for (i = slot; i < nritems; i++) {
		u32 ioff;
		ioff = btrfs_item_offset(leaf, i);
		btrfs_set_item_offset(leaf, i, ioff + size_diff);
	}

	/* shift the data */
	if (from_end) {
		memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, 0) +
			      data_end + size_diff, btrfs_item_nr_offset(leaf, 0) +
			      data_end, old_data_start + new_size - data_end);
	} else {
		struct btrfs_disk_key disk_key;
		u64 offset;

		btrfs_item_key(leaf, &disk_key, slot);

		if (btrfs_disk_key_type(&disk_key) == BTRFS_EXTENT_DATA_KEY) {
			unsigned long ptr;
			struct btrfs_file_extent_item *fi;

			fi = btrfs_item_ptr(leaf, slot,
					    struct btrfs_file_extent_item);
			fi = (struct btrfs_file_extent_item *)(
			     (unsigned long)fi - size_diff);

			if (btrfs_file_extent_type(leaf, fi) ==
			    BTRFS_FILE_EXTENT_INLINE) {
				ptr = btrfs_item_ptr_offset(leaf, slot);
				memmove_extent_buffer(leaf, ptr,
				        (unsigned long)fi,
				        offsetof(struct btrfs_file_extent_item,
						 disk_bytenr));
			}
		}

		memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, 0) +
			      data_end + size_diff, btrfs_item_nr_offset(leaf, 0) +
			      data_end, old_data_start - data_end);

		offset = btrfs_disk_key_offset(&disk_key);
		btrfs_set_disk_key_offset(&disk_key, offset + size_diff);
		btrfs_set_item_key(leaf, &disk_key, slot);
		if (slot == 0)
			btrfs_fixup_low_keys(path, &disk_key, 1);
	}

	btrfs_set_item_size(leaf, slot, new_size);
	btrfs_mark_buffer_dirty(leaf);

	ret = 0;
	if (btrfs_leaf_free_space(leaf) < 0) {
		btrfs_print_leaf(leaf, BTRFS_PRINT_TREE_DEFAULT);
		BUG();
	}
	return ret;
}

int btrfs_extend_item(struct btrfs_root *root, struct btrfs_path *path,
		      u32 data_size)
{
	int ret = 0;
	int slot;
	struct extent_buffer *leaf;
	u32 nritems;
	unsigned int data_end;
	unsigned int old_data;
	unsigned int old_size;
	int i;

	leaf = path->nodes[0];

	nritems = btrfs_header_nritems(leaf);
	data_end = leaf_data_end(leaf);

	if (btrfs_leaf_free_space(leaf) < data_size) {
		btrfs_print_leaf(leaf, BTRFS_PRINT_TREE_DEFAULT);
		BUG();
	}
	slot = path->slots[0];
	old_data = btrfs_item_data_end(leaf, slot);

	BUG_ON(slot < 0);
	if (slot >= nritems) {
		btrfs_print_leaf(leaf, BTRFS_PRINT_TREE_DEFAULT);
		printk("slot %d too large, nritems %u\n", slot, nritems);
		BUG_ON(1);
	}

	/*
	 * item0..itemN ... dataN.offset..dataN.size .. data0.size
	 */
	/* first correct the data pointers */
	for (i = slot; i < nritems; i++) {
		u32 ioff;
		ioff = btrfs_item_offset(leaf, i);
		btrfs_set_item_offset(leaf, i, ioff - data_size);
	}

	/* shift the data */
	memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, 0) +
		      data_end - data_size, btrfs_item_nr_offset(leaf, 0) +
		      data_end, old_data - data_end);

	data_end = old_data;
	old_size = btrfs_item_size(leaf, slot);
	btrfs_set_item_size(leaf, slot, old_size + data_size);
	btrfs_mark_buffer_dirty(leaf);

	ret = 0;
	if (btrfs_leaf_free_space(leaf) < 0) {
		btrfs_print_leaf(leaf, BTRFS_PRINT_TREE_DEFAULT);
		BUG();
	}
	return ret;
}

/*
 * Given a key and some data, insert an item into the tree.
 * This does all the path init required, making room in the tree if needed.
 */
int btrfs_insert_empty_items(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    struct btrfs_path *path,
			    struct btrfs_key *cpu_key, u32 *data_size,
			    int nr)
{
	struct extent_buffer *leaf;
	int ret = 0;
	int slot;
	int i;
	u32 nritems;
	u32 total_size = 0;
	u32 total_data = 0;
	unsigned int data_end;
	struct btrfs_disk_key disk_key;

	for (i = 0; i < nr; i++) {
		total_data += data_size[i];
	}

	/* create a root if there isn't one */
	if (!root->node)
		BUG();

	total_size = total_data + nr * sizeof(struct btrfs_item);
	ret = btrfs_search_slot(trans, root, cpu_key, path, total_size, 1);
	if (ret == 0) {
		return -EEXIST;
	}
	if (ret < 0)
		goto out;

	leaf = path->nodes[0];

	nritems = btrfs_header_nritems(leaf);
	data_end = leaf_data_end(leaf);

	if (btrfs_leaf_free_space(leaf) < total_size) {
		btrfs_print_leaf(leaf, BTRFS_PRINT_TREE_DEFAULT);
		printk("not enough freespace need %u have %d\n",
		       total_size, btrfs_leaf_free_space(leaf));
		BUG();
	}

	slot = path->slots[0];
	BUG_ON(slot < 0);

	if (slot < nritems) {
		unsigned int old_data = btrfs_item_data_end(leaf, slot);

		if (old_data < data_end) {
			btrfs_print_leaf(leaf, BTRFS_PRINT_TREE_DEFAULT);
			printk("slot %d old_data %u data_end %u\n",
			       slot, old_data, data_end);
			BUG_ON(1);
		}
		/*
		 * item0..itemN ... dataN.offset..dataN.size .. data0.size
		 */
		/* first correct the data pointers */
		for (i = slot; i < nritems; i++) {
			u32 ioff;

			ioff = btrfs_item_offset(leaf, i);
			btrfs_set_item_offset(leaf, i, ioff - total_data);
		}

		/* shift the items */
		memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, slot + nr),
			      btrfs_item_nr_offset(leaf, slot),
			      (nritems - slot) * sizeof(struct btrfs_item));

		/* shift the data */
		memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, 0) +
			      data_end - total_data, btrfs_item_nr_offset(leaf, 0) +
			      data_end, old_data - data_end);
		data_end = old_data;
	}

	/* setup the item for the new data */
	for (i = 0; i < nr; i++) {
		btrfs_cpu_key_to_disk(&disk_key, cpu_key + i);
		btrfs_set_item_key(leaf, &disk_key, slot + i);
		btrfs_set_item_offset(leaf, slot + i, data_end - data_size[i]);
		data_end -= data_size[i];
		btrfs_set_item_size(leaf, slot + i, data_size[i]);
	}
	btrfs_set_header_nritems(leaf, nritems + nr);
	btrfs_mark_buffer_dirty(leaf);

	ret = 0;
	if (slot == 0) {
		btrfs_cpu_key_to_disk(&disk_key, cpu_key);
		btrfs_fixup_low_keys(path, &disk_key, 1);
	}

	if (btrfs_leaf_free_space(leaf) < 0) {
		btrfs_print_leaf(leaf, BTRFS_PRINT_TREE_DEFAULT);
		BUG();
	}

out:
	return ret;
}

/*
 * Given a key and some data, insert an item into the tree.
 * This does all the path init required, making room in the tree if needed.
 */
int btrfs_insert_item(struct btrfs_trans_handle *trans, struct btrfs_root
		      *root, struct btrfs_key *cpu_key, void *data, u32
		      data_size)
{
	int ret = 0;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	unsigned long ptr;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_insert_empty_item(trans, root, path, cpu_key, data_size);
	if (!ret) {
		leaf = path->nodes[0];
		ptr = btrfs_item_ptr_offset(leaf, path->slots[0]);
		write_extent_buffer(leaf, data, ptr, data_size);
		btrfs_mark_buffer_dirty(leaf);
	}
	btrfs_free_path(path);
	return ret;
}

/*
 * delete the pointer from a given node.
 *
 * If the delete empties a node, the node is removed from the tree,
 * continuing all the way the root if required.  The root is converted into
 * a leaf if all the nodes are emptied.
 */
int btrfs_del_ptr(struct btrfs_root *root, struct btrfs_path *path,
		int level, int slot)
{
	struct extent_buffer *parent = path->nodes[level];
	u32 nritems;
	int ret = 0;

	nritems = btrfs_header_nritems(parent);
	if (slot < nritems - 1) {
		/* shift the items */
		memmove_extent_buffer(parent,
			      btrfs_node_key_ptr_offset(parent, slot),
			      btrfs_node_key_ptr_offset(parent, slot + 1),
			      sizeof(struct btrfs_key_ptr) *
			      (nritems - slot - 1));
	}
	nritems--;
	btrfs_set_header_nritems(parent, nritems);
	if (nritems == 0 && parent == root->node) {
		BUG_ON(btrfs_header_level(root->node) != 1);
		/* just turn the root into a leaf and break */
		btrfs_set_header_level(root->node, 0);
	} else if (slot == 0) {
		struct btrfs_disk_key disk_key;

		btrfs_node_key(parent, &disk_key, 0);
		btrfs_fixup_low_keys(path, &disk_key, level + 1);
	}
	btrfs_mark_buffer_dirty(parent);
	return ret;
}

/*
 * a helper function to delete the leaf pointed to by path->slots[1] and
 * path->nodes[1].
 *
 * This deletes the pointer in path->nodes[1] and frees the leaf
 * block extent.  zero is returned if it all worked out, < 0 otherwise.
 *
 * The path must have already been setup for deleting the leaf, including
 * all the proper balancing.  path->nodes[1] must be locked.
 */
static noinline int btrfs_del_leaf(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct btrfs_path *path,
				   struct extent_buffer *leaf)
{
	int ret;

	WARN_ON(btrfs_header_generation(leaf) != trans->transid);
	ret = btrfs_del_ptr(root, path, 1, path->slots[1]);
	if (ret)
		return ret;

	root_sub_used(root, leaf->len);

	ret = btrfs_free_extent(trans, leaf->start, leaf->len, 0,
				root->root_key.objectid, 0, 0);
	return ret;
}

/*
 * delete the item at the leaf level in path.  If that empties
 * the leaf, remove it from the tree
 */
int btrfs_del_items(struct btrfs_trans_handle *trans, struct btrfs_root *root,
		    struct btrfs_path *path, int slot, int nr)
{
	struct extent_buffer *leaf;
	int last_off;
	int dsize = 0;
	int ret = 0;
	int wret;
	int i;
	u32 nritems;

	leaf = path->nodes[0];
	last_off = btrfs_item_offset(leaf, slot + nr - 1);

	for (i = 0; i < nr; i++)
		dsize += btrfs_item_size(leaf, slot + i);

	nritems = btrfs_header_nritems(leaf);

	if (slot + nr != nritems) {
		int data_end = leaf_data_end(leaf);

		memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, 0) +
			      data_end + dsize,
			      btrfs_item_nr_offset(leaf, 0) + data_end,
			      last_off - data_end);

		for (i = slot + nr; i < nritems; i++) {
			u32 ioff;

			ioff = btrfs_item_offset(leaf, i);
			btrfs_set_item_offset(leaf, i, ioff + dsize);
		}

		memmove_extent_buffer(leaf, btrfs_item_nr_offset(leaf, slot),
			      btrfs_item_nr_offset(leaf, slot + nr),
			      sizeof(struct btrfs_item) *
			      (nritems - slot - nr));
	}
	btrfs_set_header_nritems(leaf, nritems - nr);
	nritems -= nr;

	/* delete the leaf if we've emptied it */
	if (nritems == 0) {
		if (leaf == root->node) {
			btrfs_set_header_level(leaf, 0);
		} else {
			btrfs_clear_buffer_dirty(trans, leaf);
			wret = btrfs_del_leaf(trans, root, path, leaf);
			BUG_ON(ret);
			if (wret)
				ret = wret;
		}
	} else {
		int used = leaf_space_used(leaf, 0, nritems);
		if (slot == 0) {
			struct btrfs_disk_key disk_key;

			btrfs_item_key(leaf, &disk_key, 0);
			btrfs_fixup_low_keys(path, &disk_key, 1);
		}

		/* delete the leaf if it is mostly empty */
		if (used < BTRFS_LEAF_DATA_SIZE(root->fs_info) / 4) {
			/* push_leaf_left fixes the path.
			 * make sure the path still points to our leaf
			 * for possible call to del_ptr below
			 */
			slot = path->slots[1];
			extent_buffer_get(leaf);

			wret = push_leaf_left(trans, root, path, 1, 1);
			if (wret < 0 && wret != -ENOSPC)
				ret = wret;

			if (path->nodes[0] == leaf &&
			    btrfs_header_nritems(leaf)) {
				wret = push_leaf_right(trans, root, path, 1, 1);
				if (wret < 0 && wret != -ENOSPC)
					ret = wret;
			}

			if (btrfs_header_nritems(leaf) == 0) {
				btrfs_clear_buffer_dirty(trans, leaf);
				path->slots[1] = slot;
				ret = btrfs_del_leaf(trans, root, path, leaf);
				BUG_ON(ret);
				free_extent_buffer(leaf);

			} else {
				btrfs_mark_buffer_dirty(leaf);
				free_extent_buffer(leaf);
			}
		} else {
			btrfs_mark_buffer_dirty(leaf);
		}
	}
	return ret;
}

/*
 * walk up the tree as far as required to find the previous leaf.
 * returns 0 if it found something or 1 if there are no lesser leaves.
 * returns < 0 on io errors.
 */
int btrfs_prev_leaf(struct btrfs_root *root, struct btrfs_path *path)
{
	int slot;
	int level = 1;
	struct extent_buffer *c;
	struct extent_buffer *next = NULL;

	while(level < BTRFS_MAX_LEVEL) {
		if (!path->nodes[level])
			return 1;

		slot = path->slots[level];
		c = path->nodes[level];
		if (slot == 0) {
			level++;
			if (level == BTRFS_MAX_LEVEL)
				return 1;
			continue;
		}
		slot--;

		next = btrfs_read_node_slot(c, slot);
		if (!extent_buffer_uptodate(next)) {
			if (IS_ERR(next))
				return PTR_ERR(next);
			return -EIO;
		}
		break;
	}
	path->slots[level] = slot;
	while(1) {
		level--;
		c = path->nodes[level];
		free_extent_buffer(c);
		slot = btrfs_header_nritems(next);
		if (slot != 0)
			slot--;
		path->nodes[level] = next;
		path->slots[level] = slot;
		if (!level)
			break;
		next = btrfs_read_node_slot(next, slot);
		if (!extent_buffer_uptodate(next)) {
			if (IS_ERR(next))
				return PTR_ERR(next);
			return -EIO;
		}
	}
	return 0;
}

/*
 * Walk up the tree as far as necessary to find the next sibling tree block.
 * More generic version of btrfs_next_leaf(), as it could find sibling nodes
 * if @path->lowest_level is not 0.
 *
 * returns 0 if it found something or 1 if there are no greater leaves.
 * returns < 0 on io errors.
 */
int btrfs_next_sibling_tree_block(struct btrfs_fs_info *fs_info,
				  struct btrfs_path *path)
{
	int slot;
	int level = path->lowest_level + 1;
	struct extent_buffer *c;
	struct extent_buffer *next = NULL;

	BUG_ON(path->lowest_level + 1 >= BTRFS_MAX_LEVEL);
	do {
		if (!path->nodes[level])
			return 1;

		slot = path->slots[level] + 1;
		c = path->nodes[level];
		if (slot >= btrfs_header_nritems(c)) {
			level++;
			if (level == BTRFS_MAX_LEVEL)
				return 1;
			continue;
		}

		if (path->reada)
			reada_for_search(fs_info, path, level, slot, 0);

		next = btrfs_read_node_slot(c, slot);
		if (!extent_buffer_uptodate(next))
			return -EIO;
		break;
	} while (level < BTRFS_MAX_LEVEL);
	path->slots[level] = slot;
	while(1) {
		level--;
		c = path->nodes[level];
		free_extent_buffer(c);
		path->nodes[level] = next;
		path->slots[level] = 0;
		/*
		 * Fsck will happily load corrupt blocks in order to fix them,
		 * so we need an extra check just to make sure this block isn't
		 * marked uptodate but invalid.
		 */
		if (check_block(fs_info, path, level))
			return -EIO;
		if (level == path->lowest_level)
			break;
		if (path->reada)
			reada_for_search(fs_info, path, level, 0, 0);
		next = btrfs_read_node_slot(next, 0);
		if (!extent_buffer_uptodate(next))
			return -EIO;
	}
	return 0;
}

int btrfs_previous_item(struct btrfs_root *root,
			struct btrfs_path *path, u64 min_objectid,
			int type)
{
	struct btrfs_key found_key;
	struct extent_buffer *leaf;
	u32 nritems;
	int ret;

	while(1) {
		if (path->slots[0] == 0) {
			ret = btrfs_prev_leaf(root, path);
			if (ret != 0)
				return ret;
		} else {
			path->slots[0]--;
		}
		leaf = path->nodes[0];
		nritems = btrfs_header_nritems(leaf);
		if (nritems == 0)
			return 1;
		if (path->slots[0] == nritems)
			path->slots[0]--;

		btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
		if (found_key.objectid < min_objectid)
			break;
		if (found_key.type == type)
			return 0;
		if (found_key.objectid == min_objectid &&
		    found_key.type < type)
			break;
	}
	return 1;
}

/*
 * search in extent tree to find a previous Metadata/Data extent item with
 * min objectid.
 *
 * returns 0 if something is found, 1 if nothing was found and < 0 on error
 */
int btrfs_previous_extent_item(struct btrfs_root *root,
			struct btrfs_path *path, u64 min_objectid)
{
	struct btrfs_key found_key;
	struct extent_buffer *leaf;
	u32 nritems;
	int ret;

	while (1) {
		if (path->slots[0] == 0) {
			ret = btrfs_prev_leaf(root, path);
			if (ret != 0)
				return ret;
		} else {
			path->slots[0]--;
		}
		leaf = path->nodes[0];
		nritems = btrfs_header_nritems(leaf);
		if (nritems == 0)
			return 1;
		if (path->slots[0] == nritems)
			path->slots[0]--;

		btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
		if (found_key.objectid < min_objectid)
			break;
		if (found_key.type == BTRFS_EXTENT_ITEM_KEY ||
		    found_key.type == BTRFS_METADATA_ITEM_KEY)
			return 0;
		if (found_key.objectid == min_objectid &&
		    found_key.type < BTRFS_EXTENT_ITEM_KEY)
			break;
	}
	return 1;
}

/*
 * Search in extent tree to found next meta/data extent
 * Caller needs to check for no-hole or skinny metadata features.
 */
int btrfs_next_extent_item(struct btrfs_root *root,
			struct btrfs_path *path, u64 max_objectid)
{
	struct btrfs_key found_key;
	int ret;

	while (1) {
		ret = btrfs_next_item(root, path);
		if (ret)
			return ret;
		btrfs_item_key_to_cpu(path->nodes[0], &found_key,
				      path->slots[0]);
		if (found_key.objectid > max_objectid)
			return 1;
		if (found_key.type == BTRFS_EXTENT_ITEM_KEY ||
		    found_key.type == BTRFS_METADATA_ITEM_KEY)
		return 0;
	}
}

/*
 * Search uuid tree - unmounted
 *
 * return -ENOENT for !found, < 0 for errors, or 0 if an item was found
 */
static int btrfs_uuid_tree_lookup(struct btrfs_root *uuid_root, u8 *uuid,
				  u8 type, u64 subid)
{
	int ret;
	struct btrfs_path *path = NULL;
	struct extent_buffer *eb;
	int slot;
	u32 item_size;
	unsigned long offset;
	struct btrfs_key key;

	if (!uuid_root) {
		ret = -ENOENT;
		goto out;
	}

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	btrfs_uuid_to_key(uuid, &key);
	key.type = type;
	ret = btrfs_search_slot(NULL, uuid_root, &key, path, 0, 0);
	if (ret < 0) {
		goto out;
	} else if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	eb = path->nodes[0];
	slot = path->slots[0];
	item_size = btrfs_item_size(eb, slot);
	offset = btrfs_item_ptr_offset(eb, slot);
	ret = -ENOENT;

	if (!IS_ALIGNED(item_size, sizeof(u64))) {
		warning("uuid item with invalid size %lu!",
			(unsigned long)item_size);
		goto out;
	}
	while (item_size) {
		__le64 data;

		read_extent_buffer(eb, &data, offset, sizeof(data));
		if (le64_to_cpu(data) == subid) {
			ret = 0;
			break;
		}
		offset += sizeof(data);
		item_size -= sizeof(data);
	}

out:
	btrfs_free_path(path);
	return ret;
}

int btrfs_uuid_tree_add(struct btrfs_trans_handle *trans, u8 *uuid, u8 type,
			u64 subvol_id_cpu)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *uuid_root = fs_info->uuid_root;
	int ret;
	struct btrfs_path *path = NULL;
	struct btrfs_key key;
	struct extent_buffer *eb;
	int slot;
	unsigned long offset;
	__le64 subvol_id_le;

	if (!uuid_root) {
		warning("%s: uuid root is not initialized", __func__);
		return -EINVAL;
	}

	ret = btrfs_uuid_tree_lookup(uuid_root, uuid, type, subvol_id_cpu);
	if (ret != -ENOENT)
		return ret;

	key.type = type;
	btrfs_uuid_to_key(uuid, &key);

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	ret = btrfs_insert_empty_item(trans, uuid_root, path, &key,
				      sizeof(subvol_id_le));
	if (ret < 0 && ret != -EEXIST) {
		warning(
		"inserting uuid item failed (0x%016llx, 0x%016llx) type %u: %d",
			(unsigned long long)key.objectid,
			(unsigned long long)key.offset, type, ret);
		goto out;
	}

	if (ret >= 0) {
		/* Add an item for the type for the first time */
		eb = path->nodes[0];
		slot = path->slots[0];
		offset = btrfs_item_ptr_offset(eb, slot);
	} else {
		/*
		 * ret == -EEXIST case, An item with that type already exists.
		 * Extend the item and store the new subvol_id at the end.
		 */
		btrfs_extend_item(uuid_root, path, sizeof(subvol_id_le));
		eb = path->nodes[0];
		slot = path->slots[0];
		offset = btrfs_item_ptr_offset(eb, slot);
		offset += btrfs_item_size(eb, slot) - sizeof(subvol_id_le);
	}

	ret = 0;
	subvol_id_le = cpu_to_le64(subvol_id_cpu);
	write_extent_buffer(eb, &subvol_id_le, offset, sizeof(subvol_id_le));
	btrfs_mark_buffer_dirty(eb);

out:
	btrfs_free_path(path);
	return ret;
}
