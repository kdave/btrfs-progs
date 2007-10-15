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

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include "kerncompat.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"

static u64 bytes_used = 0;
static u64 total_csum_bytes = 0;
static u64 total_btree_bytes = 0;
static u64 btree_space_waste = 0;
static u64 data_bytes_allocated = 0;
static u64 data_bytes_referenced = 0;

struct extent_record {
	struct cache_extent cache;
	struct btrfs_disk_key parent_key;
	u64 start;
	u64 nr;
	u64 owner;
	u32 refs;
	u32 extent_item_refs;
	int checked;
};

struct block_info {
	u64 start;
	u32 size;
};

static int check_node(struct btrfs_root *root,
		      struct btrfs_disk_key *parent_key,
		      struct btrfs_node *node)
{
	int i;
	u32 nritems = btrfs_header_nritems(&node->header);

	if (nritems == 0 || nritems > BTRFS_NODEPTRS_PER_BLOCK(root))
		return 1;
	if (parent_key->type) {
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
		fprintf(stderr, "leaf is not a leaf %llu\n",
		       (unsigned long long)btrfs_header_bytenr(&leaf->header));
		return 1;
	}
	if (btrfs_leaf_free_space(root, leaf) < 0) {
		fprintf(stderr, "leaf free space incorrect %llu %d\n",
			(unsigned long long)btrfs_header_bytenr(&leaf->header),
			btrfs_leaf_free_space(root, leaf));
		return 1;
	}

	if (nritems == 0)
		return 0;

	if (parent_key->type && memcmp(parent_key, &leaf->items[0].key,
					sizeof(struct btrfs_disk_key))) {
		fprintf(stderr, "leaf parent key incorrect %llu\n",
		       (unsigned long long)btrfs_header_bytenr(&leaf->header));
		return 1;
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

static int maybe_free_extent_rec(struct cache_tree *extent_cache,
				 struct extent_record *rec)
{
	if (rec->checked && rec->extent_item_refs == rec->refs &&
	    rec->refs > 0) {
		remove_cache_extent(extent_cache, &rec->cache);
		free(rec);
	}
	return 0;
}

static int check_block(struct btrfs_root *root,
		       struct cache_tree *extent_cache,
		       struct btrfs_buffer *buf)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int ret = 1;

	cache = find_cache_extent(extent_cache, buf->bytenr, buf->size);
	if (!cache)
		return 1;
	rec = container_of(cache, struct extent_record, cache);
	if (btrfs_is_leaf(&buf->node)) {
		ret = check_leaf(root, &rec->parent_key, &buf->leaf);
	} else {
		ret = check_node(root, &rec->parent_key, &buf->node);
	}
	rec->checked = 1;
	if (!ret)
		maybe_free_extent_rec(extent_cache, rec);
	return ret;
}

static int add_extent_rec(struct cache_tree *extent_cache,
			  struct btrfs_disk_key *parent_key,
			  u64 ref, u64 start,
			  u64 nr, u64 owner,
			  u32 extent_item_refs, int inc_ref, int set_checked)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int ret = 0;

	cache = find_cache_extent(extent_cache, start, nr);
	if (cache) {
		rec = container_of(cache, struct extent_record, cache);
		if (inc_ref)
			rec->refs++;
		if (start != rec->start) {
			fprintf(stderr, "warning, start mismatch %llu %llu\n",
				(unsigned long long)rec->start,
				(unsigned long long)start);
			ret = 1;
		}
		if (extent_item_refs) {
			if (rec->extent_item_refs) {
				fprintf(stderr, "block %llu rec extent_item_refs %u, passed %u\n", (unsigned long long)start, rec->extent_item_refs, extent_item_refs);
			}
			rec->extent_item_refs = extent_item_refs;
		}
		if (set_checked)
			rec->checked = 1;
		maybe_free_extent_rec(extent_cache, rec);
		return ret;
	}
	rec = malloc(sizeof(*rec));
	if (start == 0)
		extent_item_refs = 0;
	rec->start = start;
	rec->nr = nr;
	rec->owner = owner;
	rec->checked = 0;

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

	rec->cache.start = start;
	rec->cache.size = nr;
	ret = insert_existing_cache_extent(extent_cache, &rec->cache);
	BUG_ON(ret);
	bytes_used += nr;
	if (set_checked)
		rec->checked = 1;
	return ret;
}

static int add_pending(struct cache_tree *pending,
		       struct cache_tree *seen, u64 bytenr, u32 size)
{
	int ret;
	ret = insert_cache_extent(seen, bytenr, size);
	if (ret)
		return ret;
	insert_cache_extent(pending, bytenr, size);
	return 0;
}
static int pick_next_pending(struct cache_tree *pending,
			struct cache_tree *reada,
			struct cache_tree *nodes,
			u64 last, struct block_info *bits, int bits_nr,
			int *reada_bits)
{
	unsigned long node_start = last;
	struct cache_extent *cache;
	int ret;

	cache = find_first_cache_extent(reada, 0);
	if (cache) {
		bits[0].start = cache->start;
		bits[1].size = cache->size;
		*reada_bits = 1;
		return 1;
	}
	*reada_bits = 0;
	if (node_start > 32768)
		node_start -= 32768;

	cache = find_first_cache_extent(nodes, node_start);
	if (!cache)
		cache = find_first_cache_extent(nodes, 0);

	if (!cache) {
		 cache = find_first_cache_extent(pending, 0);
		 if (!cache)
			 return 0;
		 ret = 0;
		 do {
			 bits[ret].start = cache->start;
			 bits[ret].size = cache->size;
			 cache = next_cache_extent(cache);
			 ret++;
		 } while (cache && ret < bits_nr);
		 return ret;
	}

	ret = 0;
	do {
		bits[ret].start = cache->start;
		bits[ret].size = cache->size;
		cache = next_cache_extent(cache);
		ret++;
	} while (cache && ret < bits_nr);

	if (bits_nr - ret > 8) {
		u64 lookup = bits[0].start + bits[0].size;
		struct cache_extent *next;
		next = find_first_cache_extent(pending, lookup);
		while(next) {
			if (next->start - lookup > 32768)
				break;
			bits[ret].start = next->start;
			bits[ret].size = next->size;
			lookup = next->start + next->size;
			ret++;
			if (ret == bits_nr)
				break;
			next = next_cache_extent(next);
			if (!next)
				break;
		}
	}
	return ret;
}
static struct btrfs_buffer reada_buf;

static int run_next_block(struct btrfs_root *root,
			  struct block_info *bits,
			  int bits_nr,
			  u64 *last,
			  struct cache_tree *pending,
			  struct cache_tree *seen,
			  struct cache_tree *reada,
			  struct cache_tree *nodes,
			  struct cache_tree *extent_cache)
{
	struct btrfs_buffer *buf;
	u64 bytenr;
	u32 size;
	int ret;
	int i;
	int nritems;
	struct btrfs_leaf *leaf;
	struct btrfs_node *node;
	struct btrfs_disk_key *disk_key;
	struct cache_extent *cache;
	int reada_bits;

	u64 last_block = 0;
	ret = pick_next_pending(pending, reada, nodes, *last, bits,
				bits_nr, &reada_bits);
	if (ret == 0) {
		return 1;
	}
	if (!reada_bits) {
		for(i = 0; i < ret; i++) {
			u64 offset;
			insert_cache_extent(reada, bits[i].start,
					    bits[i].size);
			btrfs_map_bh_to_logical(root, &reada_buf,
						bits[i].start);
			offset = reada_buf.dev_bytenr;
			last_block = bits[i].start;
			readahead(reada_buf.fd, offset, bits[i].size);
		}
	}
	*last = bits[0].start;
	bytenr = bits[0].start;
	size = bits[0].size;

	cache = find_cache_extent(pending, bytenr, size);
	if (cache) {
		remove_cache_extent(pending, cache);
		free(cache);
	}
	cache = find_cache_extent(reada, bytenr, size);
	if (cache) {
		remove_cache_extent(reada, cache);
		free(cache);
	}
	cache = find_cache_extent(nodes, bytenr, size);
	if (cache) {
		remove_cache_extent(nodes, cache);
		free(cache);
	}

	buf = read_tree_block(root, bytenr, size);
	nritems = btrfs_header_nritems(&buf->node.header);
	ret = check_block(root, extent_cache, buf);
	if (ret) {
		fprintf(stderr, "bad block %llu\n",
			(unsigned long long)bytenr);
	}
	if (btrfs_is_leaf(&buf->node)) {
		leaf = &buf->leaf;
		btree_space_waste += btrfs_leaf_free_space(root, leaf);
		for (i = 0; i < nritems; i++) {
			struct btrfs_file_extent_item *fi;
			disk_key = &leaf->items[i].key;
			if (btrfs_disk_key_type(disk_key) ==
			    BTRFS_EXTENT_ITEM_KEY) {
				struct btrfs_key found;
				struct btrfs_extent_item *ei;
				btrfs_disk_key_to_cpu(&found,
						      &leaf->items[i].key);
				ei = btrfs_item_ptr(leaf, i,
						    struct btrfs_extent_item);
				add_extent_rec(extent_cache, NULL, 0,
					       found.objectid,
					       found.offset,
					       btrfs_extent_owner(ei),
					       btrfs_extent_refs(ei), 0, 0);
				continue;
			}
			if (btrfs_disk_key_type(disk_key) ==
			    BTRFS_CSUM_ITEM_KEY) {
				total_csum_bytes +=
					btrfs_item_size(leaf->items + i);
				continue;
			}
			if (btrfs_disk_key_type(disk_key) ==
			    BTRFS_BLOCK_GROUP_ITEM_KEY) {
				struct btrfs_block_group_item *bi;
				bi = btrfs_item_ptr(leaf, i,
					    struct btrfs_block_group_item);
#if 0
				fprintf(stderr,"block group %Lu %Lu used %Lu ",
					btrfs_disk_key_objectid(disk_key),
					btrfs_disk_key_offset(disk_key),
					btrfs_block_group_used(bi));
				fprintf(stderr, "flags %x\n", bi->flags);
#endif
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
			if (btrfs_file_extent_disk_bytenr(fi) == 0)
				continue;

			data_bytes_allocated +=
				btrfs_file_extent_disk_num_bytes(fi);
			data_bytes_referenced +=
				btrfs_file_extent_num_bytes(fi);
			ret = add_extent_rec(extent_cache, NULL, bytenr,
				   btrfs_file_extent_disk_bytenr(fi),
				   btrfs_file_extent_disk_num_bytes(fi),
			           btrfs_disk_key_objectid(&leaf->items[i].key),
				   0, 1, 1);
			BUG_ON(ret);
		}
	} else {
		int level;
		node = &buf->node;
		level = btrfs_header_level(&node->header);
		for (i = 0; i < nritems; i++) {
			u64 ptr = btrfs_node_blockptr(node, i);
			u32 size = btrfs_level_size(root, level - 1);
			ret = add_extent_rec(extent_cache,
					     &node->ptrs[i].key,
					     bytenr, ptr, size,
					     btrfs_header_owner(&node->header),
					     0, 1, 0);
			BUG_ON(ret);
			if (level > 1) {
				add_pending(nodes, seen, ptr, size);
			} else {
				add_pending(pending, seen, ptr, size);
			}
		}
		btree_space_waste += (BTRFS_NODEPTRS_PER_BLOCK(root) -
				      nritems) * sizeof(struct btrfs_key_ptr);
	}
	total_btree_bytes += buf->size;
	btrfs_block_release(root, buf);
	return 0;
}

static int add_root_to_pending(struct btrfs_buffer *buf,
			       struct block_info *bits,
			       int bits_nr,
			       struct cache_tree *extent_cache,
			       struct cache_tree *pending,
			       struct cache_tree *seen,
			       struct cache_tree *reada,
			       struct cache_tree *nodes)
{
	if (btrfs_header_level(&buf->node.header) > 0)
		add_pending(nodes, seen, buf->bytenr, buf->size);
	else
		add_pending(pending, seen, buf->bytenr, buf->size);
	add_extent_rec(extent_cache, NULL, 0, buf->bytenr, buf->size,
		       btrfs_header_owner(&buf->node.header), 0, 1, 0);
	return 0;
}

int check_extent_refs(struct btrfs_root *root,
		      struct cache_tree *extent_cache)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int err = 0;

	while(1) {
		cache = find_first_cache_extent(extent_cache, 0);
		if (!cache)
			break;
		rec = container_of(cache, struct extent_record, cache);
		if (rec->refs != rec->extent_item_refs) {
			fprintf(stderr, "ref mismatch on [%llu %llu] ",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);
			fprintf(stderr, "extent item %u, found %u\n",
				rec->extent_item_refs,
				rec->refs);
			err = 1;
		}
		remove_cache_extent(extent_cache, cache);
		free(rec);
	}
	return err;
}

int main(int ac, char **av) {
	struct btrfs_super_block super;
	struct btrfs_root *root;
	struct cache_tree extent_cache;
	struct cache_tree seen;
	struct cache_tree pending;
	struct cache_tree reada;
	struct cache_tree nodes;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	int ret;
	u64 last = 0;
	struct block_info *bits;
	int bits_nr;
	struct btrfs_leaf *leaf;
	int slot;
	struct btrfs_root_item *ri;

	radix_tree_init();
	cache_tree_init(&extent_cache);
	cache_tree_init(&seen);
	cache_tree_init(&pending);
	cache_tree_init(&nodes);
	cache_tree_init(&reada);

	root = open_ctree(av[1], &super);

	bits_nr = 1024;
	bits = malloc(bits_nr * sizeof(struct block_info));
	if (!bits) {
		perror("malloc");
		exit(1);
	}

	add_root_to_pending(root->fs_info->tree_root->node, bits, bits_nr,
			    &extent_cache, &pending, &seen, &reada, &nodes);

	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	btrfs_set_key_type(&key, BTRFS_ROOT_ITEM_KEY);
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root,
					&key, &path, 0, 0);
	BUG_ON(ret < 0);
	while(1) {
		leaf = &path.nodes[0]->leaf;
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(&leaf->header)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret != 0)
				break;
			leaf = &path.nodes[0]->leaf;
			slot = path.slots[0];
		}
		btrfs_disk_key_to_cpu(&found_key,
				      &leaf->items[path.slots[0]].key);
		if (btrfs_key_type(&found_key) == BTRFS_ROOT_ITEM_KEY) {
			struct btrfs_buffer *buf;
			ri = btrfs_item_ptr(leaf, path.slots[0],
					    struct btrfs_root_item);
			buf = read_tree_block(root->fs_info->tree_root,
					      btrfs_root_bytenr(ri),
					      btrfs_level_size(root,
						       btrfs_root_level(ri)));
			add_root_to_pending(buf, bits, bits_nr, &extent_cache,
					    &pending, &seen, &reada, &nodes);
			btrfs_block_release(root->fs_info->tree_root, buf);
		}
		path.slots[0]++;
	}
	btrfs_release_path(root, &path);
	while(1) {
		ret = run_next_block(root, bits, bits_nr, &last, &pending,
				     &seen, &reada, &nodes, &extent_cache);
		if (ret != 0)
			break;
	}
	ret = check_extent_refs(root, &extent_cache);
	close_ctree(root, &super);
	printf("found %llu bytes used err is %d\n",
	       (unsigned long long)bytes_used, ret);
	printf("total csum bytes: %llu\n",(unsigned long long)total_csum_bytes);
	printf("total tree bytes: %llu\n",
	       (unsigned long long)total_btree_bytes);
	printf("btree space waste bytes: %llu\n",
	       (unsigned long long)btree_space_waste);
	printf("file data blocks allocated: %llu\n referenced %llu\n",
		(unsigned long long)data_bytes_allocated,
		(unsigned long long)data_bytes_referenced);
	return ret;
}
