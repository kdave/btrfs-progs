#include <stdio.h>
#include <stdlib.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "bit-radix.h"

u64 blocks_used = 0;
struct extent_record {
	u64 start;
	u64 nr;
	u64 owner;
	u32 refs;
	u8 type;
};

static int add_extent_rec(struct radix_tree_root *extent_radix,
			  u64 ref, u64 start, u64 nr, u64 owner, u8 type)
{
	struct extent_record *rec;
	int ret = 0;
	rec = radix_tree_lookup(extent_radix, start);
	if (rec) {
		rec->refs++;
		if (owner != rec->owner) {
			fprintf(stderr, "warning, owner mismatch %Lu\n", start);
			ret = 1;
		}
		if (start != rec->start) {
			fprintf(stderr, "warning, start mismatch %Lu %Lu\n",
				rec->start, start);
			ret = 1;
		}
		if (type != rec->type) {
			fprintf(stderr, "type mismatch block %Lu %d %d\n",
				start, type, type);
			ret = 1;
		}
		return ret;
	}
	rec = malloc(sizeof(*rec));
	rec->start = start;
	rec->nr = nr;
	rec->owner = owner;
	rec->type = type;
	ret = radix_tree_insert(extent_radix, start, rec);
	BUG_ON(ret);
	blocks_used += nr;
	return ret;
}

static int add_pending(struct radix_tree_root *pending,
		       struct radix_tree_root *seen, u64 blocknr)
{
	if (test_radix_bit(seen, blocknr))
		return -EEXIST;
	set_radix_bit(pending, blocknr);
	set_radix_bit(seen, blocknr);
	return 0;
}

static int run_next_block(struct btrfs_root *root,
			  u64 *last,
			  struct radix_tree_root *pending,
			  struct radix_tree_root *seen,
			  struct radix_tree_root *extent_radix)
{
	struct btrfs_buffer *buf;
	u64 blocknr;
	int ret;
	int i;
	int nritems;
	struct btrfs_leaf *leaf;
	struct btrfs_node *node;
	unsigned long bits;

	ret = find_first_radix_bit(pending, &bits, *last, 1);
	if (ret == 0) {
		ret = find_first_radix_bit(pending, &bits, 0, 1);
		if (ret == 0)
			return 1;
	}
	*last = bits;
	blocknr = bits;
	clear_radix_bit(pending, blocknr);
	buf = read_tree_block(root, blocknr);
	nritems = btrfs_header_nritems(&buf->node.header);
	if (btrfs_is_leaf(&buf->node)) {
		leaf = &buf->leaf;
		for (i = 0; i < nritems; i++) {
			struct btrfs_file_extent_item *fi;
			if (btrfs_disk_key_type(&leaf->items[i].key) !=
			    BTRFS_EXTENT_DATA_KEY)
				continue;
			fi = btrfs_item_ptr(leaf, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(fi) !=
			    BTRFS_FILE_EXTENT_REG)
				continue;
			ret = add_extent_rec(extent_radix, blocknr,
				   btrfs_file_extent_disk_blocknr(fi),
				   btrfs_file_extent_disk_num_blocks(fi),
			           btrfs_disk_key_objectid(&leaf->items[i].key),
				   BTRFS_EXTENT_FILE);
			BUG_ON(ret);
		}
	} else {
		node = &buf->node;
		for (i = 0; i < nritems; i++) {
			u64 ptr = btrfs_node_blockptr(node, i);
			ret = add_extent_rec(extent_radix, blocknr, ptr, 1,
					     btrfs_header_owner(&node->header),
					     BTRFS_EXTENT_TREE);
			BUG_ON(ret);
			add_pending(pending, seen, ptr);
		}
	}
	btrfs_block_release(root, buf);
	return 0;
}

static int add_root_to_pending(struct btrfs_root *root,
			       struct radix_tree_root *extent_radix,
			       struct radix_tree_root *pending,
			       struct radix_tree_root *seen)
{
	add_pending(pending, seen, root->node->blocknr);
	add_extent_rec(extent_radix, 0, root->node->blocknr, 1,
		       btrfs_header_owner(&root->node->node.header),
		       BTRFS_EXTENT_TREE);
	return 0;
}
int main(int ac, char **av) {
	struct btrfs_super_block super;
	struct btrfs_root *root;
	struct radix_tree_root extent_radix;
	struct radix_tree_root seen;
	struct radix_tree_root pending;
	int ret;
	u64 last = 0;

	radix_tree_init();

	INIT_RADIX_TREE(&extent_radix, GFP_NOFS);
	init_bit_radix(&seen);
	init_bit_radix(&pending);

	root = open_ctree(av[1], &super);
	add_root_to_pending(root, &extent_radix, &pending, &seen);
	add_root_to_pending(root->fs_info->tree_root,&extent_radix,
			    &pending, &seen);
	add_root_to_pending(root->fs_info->dev_root, &extent_radix,
			    &pending, &seen);
	add_root_to_pending(root->fs_info->extent_root, &extent_radix,
			    &pending, &seen);
	while(1) {
		ret = run_next_block(root, &last, &pending,
				     &seen, &extent_radix);
		if (ret != 0)
			break;
	}
	close_ctree(root, &super);
	printf("found %Lu blocks used\n", blocks_used);
	return 0;
}
