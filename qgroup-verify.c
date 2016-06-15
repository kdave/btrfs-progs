/*
 * Copyright (C) 2014 SUSE.  All rights reserved.
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
 *
 * Authors: Mark Fasheh <mfasheh@suse.de>
 */

#include <stdio.h>
#include <stdlib.h>
#include <uuid/uuid.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "utils.h"
#include "ulist.h"
#include "rbtree-utils.h"

#include "qgroup-verify.h"

/*#define QGROUP_VERIFY_DEBUG*/
static unsigned long tot_extents_scanned = 0;

static void add_bytes(u64 root_objectid, u64 num_bytes, int exclusive);

struct qgroup_info {
	u64 referenced;
	u64 referenced_compressed;
	u64 exclusive;
	u64 exclusive_compressed;
};

struct qgroup_count {
	u64 qgroupid;
	int subvol_exists;

	struct btrfs_disk_key key;
	struct qgroup_info diskinfo;

	struct qgroup_info info;

	struct rb_node rb_node;
};

static struct counts_tree {
	struct rb_root		root;
	unsigned int		num_groups;
	unsigned int		rescan_running:1;
	unsigned int		qgroup_inconsist:1;
} counts = { .root = RB_ROOT };

static struct rb_root by_bytenr = RB_ROOT;

/*
 * List of interior tree blocks. We walk this list after loading the
 * extent tree to resolve implied refs. For each interior node we'll
 * place a shared ref in the ref tree against each child object. This
 * allows the shared ref resolving code to do the actual work later of
 * finding roots to account against.
 *
 * An implied ref is when a tree block has refs on it that may not
 * exist in any of its child nodes. Even though the refs might not
 * exist further down the tree, the fact that our interior node has a
 * ref means we need to account anything below it to all its roots.
 */
static struct ulist *tree_blocks = NULL;	/* unode->val = bytenr, ->aux
						 * = tree_block pointer */
struct tree_block {
	int			level;
	u64			num_bytes;
};

struct ref {
	u64			bytenr;
	u64			num_bytes;
	u64			parent;
	u64			root;

	struct rb_node		bytenr_node;
};

#ifdef QGROUP_VERIFY_DEBUG
static void print_ref(struct ref *ref)
{
	printf("bytenr: %llu\t\tnum_bytes: %llu\t\t parent: %llu\t\t"
	       "root: %llu\n", ref->bytenr, ref->num_bytes,
	       ref->parent, ref->root);
}

static void print_all_refs(void)
{
	unsigned long count = 0;
	struct ref *ref;
	struct rb_node *node;

	node = rb_first(&by_bytenr);
	while (node) {
		ref = rb_entry(node, struct ref, bytenr_node);

		print_ref(ref);

		count++;
		node = rb_next(node);
	}

	printf("%lu extents scanned with %lu refs in total.\n",
	       tot_extents_scanned, count);
}
#endif

/*
 * Store by bytenr in rbtree
 *
 * The tree is sorted in ascending order by bytenr, then parent, then
 * root. Since full refs have a parent == 0, those will come before
 * shared refs.
 */
static int compare_ref(struct ref *orig, u64 bytenr, u64 root, u64 parent)
{
	if (bytenr < orig->bytenr)
		return -1;
	if (bytenr > orig->bytenr)
		return 1;

	if (parent < orig->parent)
		return -1;
	if (parent > orig->parent)
		return 1;

	if (root < orig->root)
		return -1;
	if (root > orig->root)
		return 1;

	return 0;
}

/*
 * insert a new ref into the tree.  returns the existing ref entry
 * if one is already there.
 */
static struct ref *insert_ref(struct ref *ref)
{
	int ret;
	struct rb_node **p = &by_bytenr.rb_node;
	struct rb_node *parent = NULL;
	struct ref *curr;

	while (*p) {
		parent = *p;
		curr = rb_entry(parent, struct ref, bytenr_node);

		ret = compare_ref(curr, ref->bytenr, ref->root, ref->parent);
		if (ret < 0)
			p = &(*p)->rb_left;
		else if (ret > 0)
			p = &(*p)->rb_right;
		else
			return curr;
	}

	rb_link_node(&ref->bytenr_node, parent, p);
	rb_insert_color(&ref->bytenr_node, &by_bytenr);
	return ref;
}

/*
 * Partial search, returns the first ref with matching bytenr. Caller
 * can walk forward from there.
 *
 * Leftmost refs will be full refs - this is used to our advantage
 * when resolving roots.
 */
static struct ref *find_ref_bytenr(u64 bytenr)
{
	struct rb_node *n = by_bytenr.rb_node;
	struct ref *ref;

	while (n) {
		ref = rb_entry(n, struct ref, bytenr_node);

		if (bytenr < ref->bytenr)
			n = n->rb_left;
		else if (bytenr > ref->bytenr)
			n = n->rb_right;
		else {
			/* Walk to the left to find the first item */
			struct rb_node *node_left = rb_prev(&ref->bytenr_node);
			struct ref *ref_left;

			while (node_left) {
				ref_left = rb_entry(node_left, struct ref,
						    bytenr_node);
				if (ref_left->bytenr != ref->bytenr)
					break;
				ref = ref_left;
				node_left = rb_prev(node_left);
			}
			return ref;
		}
	}
	return NULL;
}

static struct ref *find_ref(u64 bytenr, u64 root, u64 parent)
{
	struct rb_node *n = by_bytenr.rb_node;
	struct ref *ref;
	int ret;

	while (n) {
		ref = rb_entry(n, struct ref, bytenr_node);

		ret = compare_ref(ref, bytenr, root, parent);
		if (ret < 0)
			n = n->rb_left;
		else if (ret > 0)
			n = n->rb_right;
		else
			return ref;
	}
	return NULL;
}

static struct ref *alloc_ref(u64 bytenr, u64 root, u64 parent, u64 num_bytes)
{
	struct ref *ref = find_ref(bytenr, root, parent);

	BUG_ON(parent && root);

	if (ref == NULL) {
		ref = calloc(1, sizeof(*ref));
		if (ref) {
			ref->bytenr = bytenr;
			ref->root = root;
			ref->parent = parent;
			ref->num_bytes = num_bytes;

			insert_ref(ref);
		}
	}
	return ref;
}

static void free_ref_node(struct rb_node *node)
{
	struct ref *ref = rb_entry(node, struct ref, bytenr_node);
	free(ref);
}

FREE_RB_BASED_TREE(ref, free_ref_node);

/*
 * Resolves all the possible roots for the ref at parent.
 */
static void find_parent_roots(struct ulist *roots, u64 parent)
{
	struct ref *ref;
	struct rb_node *node;

	/*
	 * Search the rbtree for the first ref with bytenr == parent.
	 * Walk forward so long as bytenr == parent, adding resolved root ids.
	 * For each unresolved root, we recurse
	 */
	ref = find_ref_bytenr(parent);
	node = &ref->bytenr_node;
	BUG_ON(ref == NULL);
	BUG_ON(ref->bytenr != parent);

	{
		/*
		 * Random sanity check, are we actually getting the
		 * leftmost node?
		 */
		struct rb_node *prev_node = rb_prev(&ref->bytenr_node);
		struct ref *prev;
		if (prev_node) {
			prev = rb_entry(prev_node, struct ref, bytenr_node);
			BUG_ON(prev->bytenr == parent);
		}
	}

	do {
		if (ref->root)
			ulist_add(roots, ref->root, 0, 0);
		else
			find_parent_roots(roots, ref->parent);

		node = rb_next(node);
		if (node)
			ref = rb_entry(node, struct ref, bytenr_node);
	} while (node && ref->bytenr == parent);
}

static void print_subvol_info(u64 subvolid, u64 bytenr, u64 num_bytes,
			      struct ulist *roots);
/*
 * Account each ref. Walk the refs, for each set of refs in a
 * given bytenr:
 *
 * - add the roots for direct refs to the ref roots ulist
 *
 * - resolve all possible roots for shared refs, insert each
 *   of those into ref_roots ulist (this is a recursive process)
 *
 * - Walk ref_roots ulist, adding extent bytes to each qgroup count that
 *    cooresponds to a found root.
 */
static void account_all_refs(int do_qgroups, u64 search_subvol)
{
	int exclusive;
	struct ref *ref;
	struct rb_node *node;
	u64 bytenr, num_bytes;
	struct ulist *roots = ulist_alloc(0);
	struct ulist_iterator uiter;
	struct ulist_node *unode;

	node = rb_first(&by_bytenr);
	while (node) {
		ulist_reinit(roots);

		ref = rb_entry(node, struct ref, bytenr_node);
		/*
		 * Walk forward through the list of refs for this
		 * bytenr, adding roots to our ulist. If it's a full
		 * ref, then we have the easy case. Otherwise we need
		 * to search for roots.
		 */
		bytenr = ref->bytenr;
		num_bytes = ref->num_bytes;
		do {
			BUG_ON(ref->bytenr != bytenr);
			BUG_ON(ref->num_bytes != num_bytes);
			if (ref->root)
				ulist_add(roots, ref->root, 0, 0);
			else
				find_parent_roots(roots, ref->parent);

			/*
			 * When we leave this inner loop, node is set
			 * to next in our tree and will be turned into
			 * a ref object up top
			 */
			node = rb_next(node);
			if (node)
				ref = rb_entry(node, struct ref, bytenr_node);
		} while (node && ref->bytenr == bytenr);

		/*
		 * Now that we have all roots, we can properly account
		 * this extent against the corresponding qgroups.
		 */
		if (roots->nnodes == 1)
			exclusive = 1;
		else
			exclusive = 0;

		if (search_subvol)
			print_subvol_info(search_subvol, bytenr, num_bytes,
					  roots);

		ULIST_ITER_INIT(&uiter);
		while ((unode = ulist_next(roots, &uiter))) {
			BUG_ON(unode->val == 0ULL);
			/* We only want to account fs trees */
			if (is_fstree(unode->val) && do_qgroups)
				add_bytes(unode->val, num_bytes, exclusive);
		}
	}

	ulist_free(roots);
}

static u64 resolve_one_root(u64 bytenr)
{
	struct ref *ref = find_ref_bytenr(bytenr);

	BUG_ON(ref == NULL);

	if (ref->root)
		return ref->root;
	return resolve_one_root(ref->parent);
}

static inline struct tree_block *unode_tree_block(struct ulist_node *unode)
{
	return u64_to_ptr(unode->aux);
}
static inline u64 unode_bytenr(struct ulist_node *unode)
{
	return unode->val;
}

static int alloc_tree_block(u64 bytenr, u64 num_bytes, int level)
{
	struct tree_block *block = calloc(1, sizeof(*block));

	if (block) {
		block->num_bytes = num_bytes;
		block->level = level;
		if (ulist_add(tree_blocks, bytenr, ptr_to_u64(block), 0) >= 0)
			return 0;
		free(block);
	}
	return -ENOMEM;
}

static void free_tree_blocks(void)
{
	struct ulist_iterator uiter;
	struct ulist_node *unode;

	if (!tree_blocks)
		return;

	ULIST_ITER_INIT(&uiter);
	while ((unode = ulist_next(tree_blocks, &uiter)))
		free(unode_tree_block(unode));
	ulist_free(tree_blocks);	
	tree_blocks = NULL;
}

#ifdef QGROUP_VERIFY_DEBUG
static void print_tree_block(u64 bytenr, struct tree_block *block)
{
	struct ref *ref;
	struct rb_node *node;

	printf("tree block: %llu\t\tlevel: %d\n", (unsigned long long)bytenr,
	       block->level);

	ref = find_ref_bytenr(bytenr);
	node = &ref->bytenr_node;
	do {
		print_ref(ref);
		node = rb_next(node);
		if (node)
			ref = rb_entry(node, struct ref, bytenr_node);
	} while (node && ref->bytenr == bytenr);

	printf("\n");
}

static void print_all_tree_blocks(void)
{
	struct ulist_iterator uiter;
	struct ulist_node *unode;

	if (!tree_blocks)
		return;

	printf("Listing all found interior tree nodes:\n");

	ULIST_ITER_INIT(&uiter);
	while ((unode = ulist_next(tree_blocks, &uiter)))
		print_tree_block(unode_bytenr(unode), unode_tree_block(unode));
}
#endif

static int add_refs_for_leaf_items(struct extent_buffer *eb, u64 ref_parent)
{
	int nr, i;
	int extent_type;
	u64 bytenr, num_bytes;
	struct btrfs_key key;
	struct btrfs_disk_key disk_key;
	struct btrfs_file_extent_item *fi;

	nr = btrfs_header_nritems(eb);
	for (i = 0; i < nr; i++) {
		btrfs_item_key(eb, &disk_key, i);
		btrfs_disk_key_to_cpu(&key, &disk_key);

		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;

		fi = btrfs_item_ptr(eb, i, struct btrfs_file_extent_item);
		/* filter out: inline, disk_bytenr == 0, compressed?
		 * not if we can avoid it */
		extent_type = btrfs_file_extent_type(eb, fi);

		if (extent_type == BTRFS_FILE_EXTENT_INLINE)
			continue;

		bytenr = btrfs_file_extent_disk_bytenr(eb, fi);
		if (!bytenr)
			continue;

		num_bytes = btrfs_file_extent_disk_num_bytes(eb, fi);
		if (alloc_ref(bytenr, 0, ref_parent, num_bytes) == NULL)
			return ENOMEM;
	}

	return 0;
}

static int travel_tree(struct btrfs_fs_info *info, struct btrfs_root *root,
		       u64 bytenr, u64 num_bytes, u64 ref_parent)
{
	int ret, nr, i;
	struct extent_buffer *eb;
	u64 new_bytenr;
	u64 new_num_bytes;

//	printf("travel_tree: bytenr: %llu\tnum_bytes: %llu\tref_parent: %llu\n",
//	       bytenr, num_bytes, ref_parent);

	eb = read_tree_block(root, bytenr, num_bytes, 0);
	if (!extent_buffer_uptodate(eb))
		return -EIO;

	ret = 0;
	/* Don't add a ref for our starting tree block to itself */
	if (bytenr != ref_parent) {
		if (alloc_ref(bytenr, 0, ref_parent, num_bytes) == NULL)
			return ENOMEM;
	}

	if (btrfs_is_leaf(eb)) {
		ret = add_refs_for_leaf_items(eb, ref_parent);
		goto out;
	}

	/*
	 * Interior nodes are tuples of (key, bytenr) where key is the
	 * leftmost key in the tree block pointed to by bytenr. We
	 * don't have to care about key here, just follow the bytenr
	 * pointer.
	 */
	nr = btrfs_header_nritems(eb);
	for (i = 0; i < nr; i++) {
		new_bytenr = btrfs_node_blockptr(eb, i);
		new_num_bytes = root->nodesize;

		ret = travel_tree(info, root, new_bytenr, new_num_bytes,
				  ref_parent);
	}

out:
	free_extent_buffer(eb);
	return ret;
}

static int add_refs_for_implied(struct btrfs_fs_info *info, u64 bytenr,
				struct tree_block *block)
{
	int ret;
	u64 root_id = resolve_one_root(bytenr);
	struct btrfs_root *root;
	struct btrfs_key key;

	key.objectid = root_id;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	/*
	 * XXX: Don't free the root object as we don't know whether it
	 * came off our fs_info struct or not.
	 */
	root = btrfs_read_fs_root(info, &key);
	if (!root || IS_ERR(root))
		return ENOENT;

	ret = travel_tree(info, root, bytenr, block->num_bytes, bytenr);
	if (ret)
		return ret;

	return 0;
}

/*
 * Place shared refs in the ref tree for each child of an interior tree node.
 */
static int map_implied_refs(struct btrfs_fs_info *info)
{
	int ret = 0;
	struct ulist_iterator uiter;
	struct ulist_node *unode;

	ULIST_ITER_INIT(&uiter);
	while ((unode = ulist_next(tree_blocks, &uiter))) {
		ret = add_refs_for_implied(info, unode_bytenr(unode),
					   unode_tree_block(unode));
		if (ret)
			goto out;
	}
out:
	return ret;
}

/*
 * insert a new root into the tree.  returns the existing root entry
 * if one is already there.  qgroupid is used
 * as the key
 */
static int insert_count(struct qgroup_count *qc)
{
	struct rb_node **p = &counts.root.rb_node;
	struct rb_node *parent = NULL;
	struct qgroup_count *curr;

	while (*p) {
		parent = *p;
		curr = rb_entry(parent, struct qgroup_count, rb_node);

		if (qc->qgroupid < curr->qgroupid)
			p = &(*p)->rb_left;
		else if (qc->qgroupid > curr->qgroupid)
			p = &(*p)->rb_right;
		else
			return EEXIST;
	}
	counts.num_groups++;
	rb_link_node(&qc->rb_node, parent, p);
	rb_insert_color(&qc->rb_node, &counts.root);
	return 0;
}

static struct qgroup_count *find_count(u64 qgroupid)
{
	struct rb_node *n = counts.root.rb_node;
	struct qgroup_count *count;

	while (n) {
		count = rb_entry(n, struct qgroup_count, rb_node);

		if (qgroupid < count->qgroupid)
			n = n->rb_left;
		else if (qgroupid > count->qgroupid)
			n = n->rb_right;
		else
			return count;
	}
	return NULL;
}

static struct qgroup_count *alloc_count(struct btrfs_disk_key *key,
					struct extent_buffer *leaf,
					struct btrfs_qgroup_info_item *disk)
{
	struct qgroup_count *c = calloc(1, sizeof(*c));
	struct qgroup_info *item;

	if (c) {
		c->qgroupid = btrfs_disk_key_offset(key);
		c->key = *key;

		item = &c->diskinfo;
		item->referenced = btrfs_qgroup_info_referenced(leaf, disk);
		item->referenced_compressed =
			btrfs_qgroup_info_referenced_compressed(leaf, disk);
		item->exclusive = btrfs_qgroup_info_exclusive(leaf, disk);
		item->exclusive_compressed =
			btrfs_qgroup_info_exclusive_compressed(leaf, disk);

		if (insert_count(c)) {
			free(c);
			c = NULL;
		}
	}
	return c;
}

static void add_bytes(u64 root_objectid, u64 num_bytes, int exclusive)
{
	struct qgroup_count *count = find_count(root_objectid);
	struct qgroup_info *qg;

	BUG_ON(num_bytes < 4096); /* Random sanity check. */

	if (!count)
		return;

	qg = &count->info;

	qg->referenced += num_bytes;
	/*
	 * count of compressed bytes is unimplemented, so we do the
	 * same as kernel.
	 */
	qg->referenced_compressed += num_bytes;

	if (exclusive) {
		qg->exclusive += num_bytes;
		qg->exclusive_compressed += num_bytes;
	}
}

static void read_qgroup_status(struct btrfs_path *path,
			      struct counts_tree *counts)
{
	struct btrfs_qgroup_status_item *status_item;
	u64 flags;

	status_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				     struct btrfs_qgroup_status_item);
	flags = btrfs_qgroup_status_flags(path->nodes[0], status_item);
	/*
	 * Since qgroup_inconsist/rescan_running is just one bit,
	 * assign value directly won't work.
	 */
	counts->qgroup_inconsist = !!(flags &
			BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT);
	counts->rescan_running = !!(flags & BTRFS_QGROUP_STATUS_FLAG_RESCAN);
}

static int load_quota_info(struct btrfs_fs_info *info)
{
	int ret;
	struct btrfs_root *root = info->quota_root;
	struct btrfs_root *tmproot;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key root_key;
	struct btrfs_disk_key disk_key;
	struct extent_buffer *leaf;
	struct btrfs_qgroup_info_item *item;
	struct qgroup_count *count;
	int i, nr;

	btrfs_init_path(&path);

	key.offset = 0;
	key.objectid = 0;
	key.type = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "ERROR: Couldn't search slot: %d\n", ret);
		goto out;
	}

	while (1) {
		leaf = path.nodes[0];

		nr = btrfs_header_nritems(leaf);
		for(i = 0; i < nr; i++) {
			btrfs_item_key(leaf, &disk_key, i);
			btrfs_disk_key_to_cpu(&key, &disk_key);

			if (key.type == BTRFS_QGROUP_STATUS_KEY) {
				read_qgroup_status(&path, &counts);
				continue;
			}
			if (key.type == BTRFS_QGROUP_RELATION_KEY)
				printf("Ignoring qgroup relation key %llu\n",
				       key.objectid);

			/*
			 * Ignore: BTRFS_QGROUP_LIMIT_KEY,
			 * 	   BTRFS_QGROUP_RELATION_KEY
			 */
			if (key.type != BTRFS_QGROUP_INFO_KEY)
				continue;

			item = btrfs_item_ptr(leaf, i,
					      struct btrfs_qgroup_info_item);

			count = alloc_count(&disk_key, leaf, item);
			if (!count) {
				ret = ENOMEM;
				fprintf(stderr, "ERROR: out of memory\n");
				goto out;
			}

			root_key.objectid = key.offset;
			root_key.type = BTRFS_ROOT_ITEM_KEY;
			root_key.offset = (u64)-1;
			tmproot = btrfs_read_fs_root_no_cache(info, &root_key);
			if (tmproot && !IS_ERR(tmproot)) {
				count->subvol_exists = 1;
				btrfs_free_fs_root(tmproot);
			}
		}

		ret = btrfs_next_leaf(root, &path);
		if (ret != 0)
			break;
	}

	ret = 0;
	btrfs_release_path(&path);
out:
	return ret;
}

static int add_inline_refs(struct btrfs_fs_info *info,
			   struct extent_buffer *ei_leaf, int slot,
			   u64 bytenr, u64 num_bytes, int meta_item)
{
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	u64 flags, root_obj, offset, parent;
	u32 item_size = btrfs_item_size_nr(ei_leaf, slot);
	int type;
	unsigned long end;
	unsigned long ptr;

	ei = btrfs_item_ptr(ei_leaf, slot, struct btrfs_extent_item);
	flags = btrfs_extent_flags(ei_leaf, ei);

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK && !meta_item) {
		struct btrfs_tree_block_info *tbinfo;
		tbinfo = (struct btrfs_tree_block_info *)(ei + 1);
		iref = (struct btrfs_extent_inline_ref *)(tbinfo + 1);
	} else {
		iref = (struct btrfs_extent_inline_ref *)(ei + 1);
	}

	ptr = (unsigned long)iref;
	end = (unsigned long)ei + item_size;
	while (ptr < end) {
		iref = (struct btrfs_extent_inline_ref *)ptr;

		parent = root_obj = 0;
		offset = btrfs_extent_inline_ref_offset(ei_leaf, iref);
		type = btrfs_extent_inline_ref_type(ei_leaf, iref);
		switch (type) {
		case BTRFS_TREE_BLOCK_REF_KEY:
			root_obj = offset;
			break;
		case BTRFS_EXTENT_DATA_REF_KEY:
			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			root_obj = btrfs_extent_data_ref_root(ei_leaf, dref);
			break;
		case BTRFS_SHARED_DATA_REF_KEY:
		case BTRFS_SHARED_BLOCK_REF_KEY:
			parent = offset;
			break;
		default:
			return 1;
		}

		if (alloc_ref(bytenr, root_obj, parent, num_bytes) == NULL)
			return ENOMEM;

		ptr += btrfs_extent_inline_ref_size(type);
	}

	return 0;
}

static int add_keyed_ref(struct btrfs_fs_info *info,
			 struct btrfs_key *key,
			 struct extent_buffer *leaf, int slot,
			 u64 bytenr, u64 num_bytes)
{
	u64 root_obj = 0, parent = 0;
	struct btrfs_extent_data_ref *dref;

	switch(key->type) {
	case BTRFS_TREE_BLOCK_REF_KEY:
		root_obj = key->offset;
		break;
	case BTRFS_EXTENT_DATA_REF_KEY:
		dref = btrfs_item_ptr(leaf, slot, struct btrfs_extent_data_ref);
		root_obj = btrfs_extent_data_ref_root(leaf, dref);
		break;
	case BTRFS_SHARED_DATA_REF_KEY:
	case BTRFS_SHARED_BLOCK_REF_KEY:
		parent = key->offset;
		break;
	default:
		return 1;
	}

	if (alloc_ref(bytenr, root_obj, parent, num_bytes) == NULL)
		return ENOMEM;

	return 0;
}

/*
 * return value of 0 indicates leaf or not meta data. The code that
 * calls this does not need to make a distinction between the two as
 * it is only concerned with intermediate blocks which will always
 * have level > 0.
 */
static int get_tree_block_level(struct btrfs_key *key,
				struct extent_buffer *ei_leaf,
				int slot)
{
	int level = 0;
	int meta_key = key->type == BTRFS_METADATA_ITEM_KEY;
	u64 flags;
	struct btrfs_extent_item *ei;

	ei = btrfs_item_ptr(ei_leaf, slot, struct btrfs_extent_item);
	flags = btrfs_extent_flags(ei_leaf, ei);

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK && !meta_key) {
		struct btrfs_tree_block_info *tbinfo;
		tbinfo = (struct btrfs_tree_block_info *)(ei + 1);
		level = btrfs_tree_block_level(ei_leaf, tbinfo);
	} else if (meta_key) {
		/* skinny metadata */
		level = (int)key->offset;
	}
	return level;
}

/*
 * Walk the extent tree, allocating a ref item for every ref and
 * storing it in the bytenr tree.
 */
static int scan_extents(struct btrfs_fs_info *info,
			u64 start, u64 end)
{
	int ret, i, nr, level;
	struct btrfs_root *root = info->extent_root;
	struct btrfs_key key;
	struct btrfs_path path;
	struct btrfs_disk_key disk_key;
	struct extent_buffer *leaf;
	u64 bytenr = 0, num_bytes = 0;

	btrfs_init_path(&path);

	key.objectid = start;
	key.type = 0;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "ERROR: Couldn't search slot: %d\n", ret);
		goto out;
	}
	path.reada = 1;

	while (1) {
		leaf = path.nodes[0];

		nr = btrfs_header_nritems(leaf);
		for(i = 0; i < nr; i++) {
			btrfs_item_key(leaf, &disk_key, i);
			btrfs_disk_key_to_cpu(&key, &disk_key);

			if (key.objectid < start)
				continue;

			if (key.objectid > end)
				goto done;

			if (key.type == BTRFS_EXTENT_ITEM_KEY ||
			    key.type == BTRFS_METADATA_ITEM_KEY) {
				int meta = 0;

				tot_extents_scanned++;

				bytenr = key.objectid;
				num_bytes = key.offset;
				if (key.type == BTRFS_METADATA_ITEM_KEY) {
					num_bytes = info->extent_root->nodesize;
					meta = 1;
				}

				ret = add_inline_refs(info, leaf, i, bytenr,
						      num_bytes, meta);
				if (ret)
					goto out;

				level = get_tree_block_level(&key, leaf, i);
				if (level) {
					if (alloc_tree_block(bytenr, num_bytes,
							     level))
						return ENOMEM;
				}

				continue;
			}

			if (key.type > BTRFS_SHARED_DATA_REF_KEY)
				continue;
			if (key.type < BTRFS_TREE_BLOCK_REF_KEY)
				continue;

			/*
			 * Keyed refs should come after their extent
			 * item in the tree. As a result, the value of
			 * bytenr and num_bytes should be unchanged
			 * from the above block that catches the
			 * original extent item.
			 */
			BUG_ON(key.objectid != bytenr);

			ret = add_keyed_ref(info, &key, leaf, i, bytenr,
					    num_bytes);
			if (ret)
				goto out;
		}

		ret = btrfs_next_leaf(root, &path);
		if (ret != 0) {
			if (ret < 0) {
				fprintf(stderr,
					"ERROR: Next leaf failed: %d\n", ret);
				goto out;
			}
			break;
		}
	}
done:
	ret = 0;
out:
	btrfs_release_path(&path);

	return ret;
}

static void print_fields(u64 bytes, u64 bytes_compressed, char *prefix,
			 char *type)
{
	printf("%s\t\t%s %llu %s compressed %llu\n",
	       prefix, type, (unsigned long long)bytes, type,
	       (unsigned long long)bytes_compressed);
}

static void print_fields_signed(long long bytes,
				long long bytes_compressed,
				char *prefix, char *type)
{
	printf("%s\t\t%s %lld %s compressed %lld\n",
	       prefix, type, bytes, type, bytes_compressed);
}

static int report_qgroup_difference(struct qgroup_count *count, int verbose)
{
	int is_different;
	struct qgroup_info *info = &count->info;
	struct qgroup_info *disk = &count->diskinfo;
	long long excl_diff = info->exclusive - disk->exclusive;
	long long ref_diff = info->referenced - disk->referenced;

	is_different = excl_diff || ref_diff;

	if (verbose || (is_different && count->subvol_exists)) {
		printf("Counts for qgroup id: %llu %s\n",
		       (unsigned long long)count->qgroupid,
		       is_different ? "are different" : "");

		print_fields(info->referenced, info->referenced_compressed,
			     "our:", "referenced");
		print_fields(disk->referenced, disk->referenced_compressed,
			     "disk:", "referenced");
		if (ref_diff)
			print_fields_signed(ref_diff, ref_diff,
					    "diff:", "referenced");
		print_fields(info->exclusive, info->exclusive_compressed,
			     "our:", "exclusive");
		print_fields(disk->exclusive, disk->exclusive_compressed,
			     "disk:", "exclusive");
		if (excl_diff)
			print_fields_signed(excl_diff, excl_diff,
					    "diff:", "exclusive");
	}
	return (is_different && count->subvol_exists);
}

int report_qgroups(int all)
{
	struct rb_node *node;
	struct qgroup_count *c;
	int ret = 0;

	if (counts.rescan_running) {
		if (all) {
			printf(
	"Qgroup rescan is running, qgroup counts difference is expected\n");
		} else {
			printf(
	"Qgroup rescan is running, ignore qgroup check\n");
			return ret;
		}
	}
	if (counts.qgroup_inconsist && !counts.rescan_running)
		fprintf(stderr, "Qgroup is already inconsistent before checking\n");
	node = rb_first(&counts.root);
	while (node) {
		c = rb_entry(node, struct qgroup_count, rb_node);
		ret |= report_qgroup_difference(c, all);
		node = rb_next(node);
	}
	return ret;
}

void free_qgroup_counts(void)
{
	struct rb_node *node;
	struct qgroup_count *c;
	node = rb_first(&counts.root);
	while (node) {
		c = rb_entry(node, struct qgroup_count, rb_node);
		node = rb_next(node);
		rb_erase(&c->rb_node, &counts.root);
		free(c);
	}
}

int qgroup_verify_all(struct btrfs_fs_info *info)
{
	int ret;

	if (!info->quota_enabled)
		return 0;

	tree_blocks = ulist_alloc(0);
	if (!tree_blocks) {
		fprintf(stderr,
			"ERROR: Out of memory while allocating ulist.\n");
		return ENOMEM;
	}

	ret = load_quota_info(info);
	if (ret) {
		fprintf(stderr, "ERROR: Loading qgroups from disk: %d\n", ret);
		goto out;
	}

	/*
	 * Put all extent refs into our rbtree
	 */
	ret = scan_extents(info, 0, ~0ULL);
	if (ret) {
		fprintf(stderr, "ERROR: while scanning extent tree: %d\n", ret);
		goto out;
	}

	ret = map_implied_refs(info);
	if (ret) {
		fprintf(stderr, "ERROR: while mapping refs: %d\n", ret);
		goto out;
	}

	account_all_refs(1, 0);

out:
	/*
	 * Don't free the qgroup count records as they will be walked
	 * later via the print function.
	 */
	free_tree_blocks();
	free_ref_tree(&by_bytenr);
	return ret;
}

static void __print_subvol_info(u64 bytenr, u64 num_bytes, struct ulist *roots)
{
	int n = roots->nnodes;
	struct ulist_iterator uiter;
	struct ulist_node *unode;

	printf("%llu\t%llu\t%d\t", bytenr, num_bytes, n);

	ULIST_ITER_INIT(&uiter);
	while ((unode = ulist_next(roots, &uiter))) {
		printf("%llu ", unode->val);
	}
	printf("\n");
}

static void print_subvol_info(u64 subvolid, u64 bytenr, u64 num_bytes,
			      struct ulist *roots)
{
	struct ulist_iterator uiter;
	struct ulist_node *unode;

	ULIST_ITER_INIT(&uiter);
	while ((unode = ulist_next(roots, &uiter))) {
		BUG_ON(unode->val == 0ULL);
		if (unode->val == subvolid) {
			__print_subvol_info(bytenr, num_bytes, roots);
			return;
		}
	}


}

int print_extent_state(struct btrfs_fs_info *info, u64 subvol)
{
	int ret;

	tree_blocks = ulist_alloc(0);
	if (!tree_blocks) {
		fprintf(stderr,
			"ERROR: Out of memory while allocating ulist.\n");
		return ENOMEM;
	}

	/*
	 * Put all extent refs into our rbtree
	 */
	ret = scan_extents(info, 0, ~0ULL);
	if (ret) {
		fprintf(stderr, "ERROR: while scanning extent tree: %d\n", ret);
		goto out;
	}

	ret = map_implied_refs(info);
	if (ret) {
		fprintf(stderr, "ERROR: while mapping refs: %d\n", ret);
		goto out;
	}

	printf("Offset\t\tLen\tRoot Refs\tRoots\n");
	account_all_refs(0, subvol);

out:
	free_tree_blocks();
	free_ref_tree(&by_bytenr);
	return ret;
}

