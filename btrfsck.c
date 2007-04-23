#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "bit-radix.h"

u64 blocks_used = 0;
struct extent_record {
	struct btrfs_disk_key parent_key;
	struct btrfs_disk_key node_key;
	u64 start;
	u64 nr;
	u64 owner;
	u32 refs;
	u32 extent_item_refs;
	u8 type;
};

static int check_node(struct btrfs_root *root,
		      struct btrfs_disk_key *parent_key,
		      struct btrfs_node *node)
{
	int i;
	u32 nritems = btrfs_header_nritems(&node->header);

	if (nritems == 0 || nritems > BTRFS_NODEPTRS_PER_BLOCK(root))
		return 1;
	if (parent_key->flags) {
		if (memcmp(parent_key, &node->ptrs[0].key,
			      sizeof(struct btrfs_disk_key)))
			return 1;
	}
	for (i = 0; nritems > 1 && i < nritems - 2; i++) {
		struct btrfs_key cpukey;
		btrfs_disk_key_to_cpu(&cpukey, &node->ptrs[i + 1].key);
		if (btrfs_comp_keys(&node->ptrs[i].key, &cpukey) >= 0)
			return 1;
	}
	return 0;
}

static int check_leaf(struct btrfs_root *root,
		      struct btrfs_disk_key *parent_key,
		      struct btrfs_leaf *leaf)
{
	int i;
	u32 nritems = btrfs_header_nritems(&leaf->header);

	if (btrfs_header_level(&leaf->header) != 0) {
		fprintf(stderr, "leaf is not a leaf %Lu\n",
			btrfs_header_blocknr(&leaf->header));
		return 1;
	}
	if (btrfs_leaf_free_space(root, leaf) < 0) {
		fprintf(stderr, "leaf free space incorrect %Lu %d\n",
			btrfs_header_blocknr(&leaf->header),
			btrfs_leaf_free_space(root, leaf));
		return 1;
	}

	if (nritems == 0)
		return 0;

	if (parent_key->flags) {
		if (memcmp(parent_key, &leaf->items[0].key,
		       sizeof(struct btrfs_disk_key))) {
			fprintf(stderr, "leaf parent key incorrect %Lu\n",
				btrfs_header_blocknr(&leaf->header));
			return 1;
		}
	}
	for (i = 0; nritems > 1 && i < nritems - 2; i++) {
		struct btrfs_key cpukey;
		btrfs_disk_key_to_cpu(&cpukey, &leaf->items[i + 1].key);
		if (btrfs_comp_keys(&leaf->items[i].key,
		                 &cpukey) >= 0)
			return 1;
		if (btrfs_item_offset(leaf->items + i) !=
			btrfs_item_end(leaf->items + i + 1))
			return 1;
		if (i == 0) {
			if (btrfs_item_offset(leaf->items + i) +
			       btrfs_item_size(leaf->items + i) !=
			       BTRFS_LEAF_DATA_SIZE(root))
				return 1;
		}
	}
	return 0;
}

static int check_block(struct btrfs_root *root,
		       struct radix_tree_root *extent_radix,
		       struct btrfs_buffer *buf)
{
	struct extent_record *rec;

	rec = radix_tree_lookup(extent_radix, buf->blocknr);
	if (!rec)
		return 1;
	if (btrfs_is_leaf(&buf->node)) {
		return check_leaf(root, &rec->parent_key, &buf->leaf);
	} else {
		return check_node(root, &rec->parent_key, &buf->node);
	}
	return 1;
}

static int add_extent_rec(struct radix_tree_root *extent_radix,
			  struct btrfs_disk_key *parent_key,
			  u64 ref, u64 start, u64 nr, u64 owner, u8 type,
			  u32 extent_item_refs, int inc_ref)
{
	struct extent_record *rec;
	int ret = 0;
	rec = radix_tree_lookup(extent_radix, start);
	if (rec) {
		if (inc_ref)
			rec->refs++;
		if (owner != rec->owner) {
			fprintf(stderr, "warning, owner mismatch %Lu %Lu %Lu\n",
				start, owner, rec->owner);
			ret = 1;
		}
		if (start != rec->start) {
			fprintf(stderr, "warning, start mismatch %Lu %Lu\n",
				rec->start, start);
			ret = 1;
		}
		if (type != rec->type) {
			fprintf(stderr, "type mismatch block %Lu %d %d\n",
				start, type, rec->type);
			ret = 1;
		}
		if (extent_item_refs)
			rec->extent_item_refs = extent_item_refs;
		return ret;
	}
	rec = malloc(sizeof(*rec));
	if (start == 0)
		extent_item_refs = 0;
	rec->start = start;
	rec->nr = nr;
	rec->owner = owner;
	rec->type = type;

	if (inc_ref)
		rec->refs = 1;
	else
		rec->refs = 0;

	if (extent_item_refs)
		rec->extent_item_refs = extent_item_refs;
	else
		rec->extent_item_refs = 0;

	if (parent_key)
		memcpy(&rec->parent_key, parent_key, sizeof(*parent_key));
	else
		memset(&rec->parent_key, 0, sizeof(*parent_key));

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

static int pick_next_pending(struct radix_tree_root *pending,
			struct radix_tree_root *reada,
			struct radix_tree_root *nodes,
			u64 last, unsigned long *bits, int bits_nr)
{
	unsigned long node_start = last;
	int ret;
	ret = find_first_radix_bit(reada, bits, 0, 1);
	if (ret)
		return ret;
	if (node_start > 8)
		node_start -= 8;
	ret = find_first_radix_bit(nodes, bits, node_start, bits_nr);
	if (!ret)
		ret = find_first_radix_bit(nodes, bits, 0, bits_nr);
	if (ret)
		return ret;
	return find_first_radix_bit(pending, bits, 0, bits_nr);
}

static struct btrfs_buffer reada_buf;

static int run_next_block(struct btrfs_root *root,
			  unsigned long *bits,
			  int bits_nr,
			  u64 *last,
			  struct radix_tree_root *pending,
			  struct radix_tree_root *seen,
			  struct radix_tree_root *reada,
			  struct radix_tree_root *nodes,
			  struct radix_tree_root *extent_radix)
{
	struct btrfs_buffer *buf;
	u64 blocknr;
	int ret;
	int i;
	int nritems;
	struct btrfs_leaf *leaf;
	struct btrfs_node *node;
	u64 last_block = 0;

	ret = pick_next_pending(pending, reada, nodes, *last, bits, bits_nr);
	if (ret == 0) {
		return 1;
	}
	for(i = 0; i < ret; i++) {
		u64 offset;
		if (test_radix_bit(reada, bits[i]))
			continue;
		set_radix_bit(reada, bits[i]);
		btrfs_map_bh_to_logical(root, &reada_buf, bits[i]);
		offset = reada_buf.dev_blocknr * root->blocksize;
		last_block = bits[i];
		readahead(reada_buf.fd, offset, root->blocksize);
	}

	*last = bits[0];
	blocknr = bits[0];
	clear_radix_bit(pending, blocknr);
	clear_radix_bit(reada, blocknr);
	clear_radix_bit(nodes, blocknr);
	buf = read_tree_block(root, blocknr);
	nritems = btrfs_header_nritems(&buf->node.header);
	ret = check_block(root, extent_radix, buf);
	if (ret) {
		fprintf(stderr, "bad block %Lu\n", blocknr);
	}
	if (btrfs_is_leaf(&buf->node)) {
		leaf = &buf->leaf;
		for (i = 0; i < nritems; i++) {
			struct btrfs_file_extent_item *fi;
			if (btrfs_disk_key_type(&leaf->items[i].key) ==
			    BTRFS_EXTENT_ITEM_KEY) {
				struct btrfs_key found;
				struct btrfs_extent_item *ei;
				btrfs_disk_key_to_cpu(&found,
						      &leaf->items[i].key);
				ei = btrfs_item_ptr(leaf, i,
						    struct btrfs_extent_item);
				add_extent_rec(extent_radix, NULL, 0,
					       found.objectid,
					       found.offset,
					       btrfs_extent_owner(ei),
					       btrfs_extent_type(ei),
					       btrfs_extent_refs(ei), 0);
				continue;
			}
			if (btrfs_disk_key_type(&leaf->items[i].key) !=
			    BTRFS_EXTENT_DATA_KEY)
				continue;
			fi = btrfs_item_ptr(leaf, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(fi) !=
			    BTRFS_FILE_EXTENT_REG)
				continue;
			ret = add_extent_rec(extent_radix, NULL, blocknr,
				   btrfs_file_extent_disk_blocknr(fi),
				   btrfs_file_extent_disk_num_blocks(fi),
			           btrfs_disk_key_objectid(&leaf->items[i].key),
				   BTRFS_EXTENT_FILE, 0, 1);
			BUG_ON(ret);
		}
	} else {
		int level;
		node = &buf->node;
		level = btrfs_header_level(&node->header);
		for (i = 0; i < nritems; i++) {
			u64 ptr = btrfs_node_blockptr(node, i);
			ret = add_extent_rec(extent_radix,
					     &node->ptrs[i].key,
					     blocknr, ptr, 1,
					     btrfs_header_owner(&node->header),
					     BTRFS_EXTENT_TREE, 0, 1);
			BUG_ON(ret);
			if (level > 1) {
				add_pending(nodes, seen, ptr);
			} else {
				add_pending(pending, seen, ptr);
			}
		}
	}
	btrfs_block_release(root, buf);
	return 0;
}

static int add_root_to_pending(struct btrfs_root *root,
			       unsigned long *bits,
			       int bits_nr,
			       struct radix_tree_root *extent_radix,
			       struct radix_tree_root *pending,
			       struct radix_tree_root *seen,
			       struct radix_tree_root *reada,
			       struct radix_tree_root *nodes)
{
	add_pending(pending, seen, root->node->blocknr);
	add_extent_rec(extent_radix, NULL, 0, root->node->blocknr, 1,
		       btrfs_header_owner(&root->node->node.header),
		       BTRFS_EXTENT_TREE, 0, 1);
	return 0;
}

int check_extent_refs(struct btrfs_root *root,
		      struct radix_tree_root *extent_radix)
{
	struct extent_record *rec[64];
	int i;
	int ret;
	int err = 0;

	while(1) {
		ret = radix_tree_gang_lookup(extent_radix, (void **)rec, 0,
					     ARRAY_SIZE(rec));
		if (!ret)
			break;
		for (i = 0; i < ret; i++) {
			if (rec[i]->refs != rec[i]->extent_item_refs) {
				fprintf(stderr, "ref mismatch on [%Lu %Lu] ",
					rec[i]->start, rec[i]->nr);
				fprintf(stderr, "extent item %u, found %u\n",
					rec[i]->extent_item_refs,
					rec[i]->refs);
				err = 1;
			}
			radix_tree_delete(extent_radix, rec[i]->start);
			free(rec[i]);
		}
	}
	return err;
}

int main(int ac, char **av) {
	struct btrfs_super_block super;
	struct btrfs_root *root;
	struct radix_tree_root extent_radix;
	struct radix_tree_root seen;
	struct radix_tree_root pending;
	struct radix_tree_root reada;
	struct radix_tree_root nodes;
	int ret;
	u64 last = 0;
	unsigned long *bits;
	int bits_nr;

	radix_tree_init();


	INIT_RADIX_TREE(&extent_radix, GFP_NOFS);
	init_bit_radix(&seen);
	init_bit_radix(&pending);
	init_bit_radix(&reada);
	init_bit_radix(&nodes);

	root = open_ctree(av[1], &super);

	bits_nr = 1024 * 1024 / root->blocksize;
	bits = malloc(bits_nr * sizeof(unsigned long));
	if (!bits) {
		perror("malloc");
		exit(1);
	}

	add_root_to_pending(root, bits, bits_nr, &extent_radix,
			    &pending, &seen, &reada, &nodes);
	add_root_to_pending(root->fs_info->tree_root, bits, bits_nr,
			    &extent_radix, &pending, &seen, &reada, &nodes);
	add_root_to_pending(root->fs_info->dev_root, bits, bits_nr,
			    &extent_radix, &pending, &seen, &reada, &nodes);
	add_root_to_pending(root->fs_info->extent_root, bits, bits_nr,
			    &extent_radix, &pending, &seen, &reada, &nodes);
	while(1) {
		ret = run_next_block(root, bits, bits_nr, &last, &pending,
				     &seen, &reada, &nodes, &extent_radix);
		if (ret != 0)
			break;
	}
	ret = check_extent_refs(root, &extent_radix);
	close_ctree(root, &super);
	printf("found %Lu blocks used err is %d\n", blocks_used, ret);
	return ret;
}
