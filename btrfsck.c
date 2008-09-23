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
#include "list.h"
#include "version.h"

static u64 bytes_used = 0;
static u64 total_csum_bytes = 0;
static u64 total_btree_bytes = 0;
static u64 btree_space_waste = 0;
static u64 data_bytes_allocated = 0;
static u64 data_bytes_referenced = 0;

struct extent_backref {
	struct list_head list;
	u64 parent;
	u64 root;
	u64 generation;
	u64 owner;
	u64 offset;
	u32 num_refs;
	u32 found_ref;
	int found_extent_tree;
};

struct extent_record {
	struct list_head backrefs;
	struct cache_extent cache;
	struct btrfs_disk_key parent_key;
	u64 start;
	u64 nr;
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
		      struct extent_buffer *buf)
{
	int i;
	struct btrfs_key cpukey;
	struct btrfs_disk_key key;
	u32 nritems = btrfs_header_nritems(buf);

	if (nritems == 0 || nritems > BTRFS_NODEPTRS_PER_BLOCK(root))
		return 1;
	if (parent_key->type) {
		btrfs_node_key(buf, &key, 0);
		if (memcmp(parent_key, &key, sizeof(key)))
			return 1;
	}
	for (i = 0; nritems > 1 && i < nritems - 2; i++) {
		btrfs_node_key(buf, &key, i);
		btrfs_node_key_to_cpu(buf, &cpukey, i + 1);
		if (btrfs_comp_keys(&key, &cpukey) >= 0)
			return 1;
	}
	return 0;
}

static int check_leaf(struct btrfs_root *root,
		      struct btrfs_disk_key *parent_key,
		      struct extent_buffer *buf)
{
	int i;
	struct btrfs_key cpukey;
	struct btrfs_disk_key key;
	u32 nritems = btrfs_header_nritems(buf);

	if (btrfs_header_level(buf) != 0) {
		fprintf(stderr, "leaf is not a leaf %llu\n",
		       (unsigned long long)btrfs_header_bytenr(buf));
		return 1;
	}
	if (btrfs_leaf_free_space(root, buf) < 0) {
		fprintf(stderr, "leaf free space incorrect %llu %d\n",
			(unsigned long long)btrfs_header_bytenr(buf),
			btrfs_leaf_free_space(root, buf));
		return 1;
	}

	if (nritems == 0)
		return 0;

	btrfs_item_key(buf, &key, 0);
	if (parent_key->type && memcmp(parent_key, &key, sizeof(key))) {
		fprintf(stderr, "leaf parent key incorrect %llu\n",
		       (unsigned long long)btrfs_header_bytenr(buf));
		return 1;
	}
	for (i = 0; nritems > 1 && i < nritems - 2; i++) {
		btrfs_item_key(buf, &key, i);
		btrfs_item_key_to_cpu(buf, &cpukey, i + 1);
		if (btrfs_comp_keys(&key, &cpukey) >= 0) {
			fprintf(stderr, "bad key ordering %d %d\n", i, i+1);
			return 1;
		}
		if (btrfs_item_offset_nr(buf, i) !=
			btrfs_item_end_nr(buf, i + 1)) {
			fprintf(stderr, "incorrect offsets %u %u\n",
				btrfs_item_offset_nr(buf, i),
				btrfs_item_end_nr(buf, i + 1));
			return 1;
		}
		if (i == 0 && btrfs_item_end_nr(buf, i) !=
		    BTRFS_LEAF_DATA_SIZE(root)) {
			fprintf(stderr, "bad item end %u wanted %u\n",
				btrfs_item_end_nr(buf, i),
				(unsigned)BTRFS_LEAF_DATA_SIZE(root));
			return 1;
		}
	}
	return 0;
}

static int all_backpointers_checked(struct extent_record *rec, int print_errs)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *back;
	u32 found = 0;
	int err = 0;

	while(cur != &rec->backrefs) {
		back = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (!back->found_extent_tree) {
			err = 1;
			if (!print_errs)
				goto out;
			fprintf(stderr, "Backref %llu parent %llu"
				" [%llu %llu %llu %llu %lu]"
				" not found in extent tree\n",
				(unsigned long long)rec->start,
				(unsigned long long)back->parent,
				(unsigned long long)back->root,
				(unsigned long long)back->generation,
				(unsigned long long)back->owner,
				(unsigned long long)back->offset,
				(unsigned long)back->num_refs);
		}
		if (!back->found_ref) {
			err = 1;
			if (!print_errs)
				goto out;
			fprintf(stderr, "Backref %llu parent %llu"
				" [%llu %llu %llu %llu %lu]"
				" not referenced\n",
				(unsigned long long)rec->start,
				(unsigned long long)back->parent,
				(unsigned long long)back->root,
				(unsigned long long)back->generation,
				(unsigned long long)back->owner,
				(unsigned long long)back->offset,
				(unsigned long)back->num_refs);
		}
		if (back->found_ref != back->num_refs) {
			err = 1;
			if (!print_errs)
				goto out;
			fprintf(stderr, "Incorrect local backref count "
				"on %llu parent %llu found %u wanted %u\n",
				(unsigned long long)rec->start,
				(unsigned long long)back->parent,
				back->found_ref, back->num_refs);
		}
		found += back->found_ref;
	}
	if (found != rec->refs) {
		err = 1;
		if (!print_errs)
			goto out;
		fprintf(stderr, "Incorrect global backref count "
			"on %llu found %u wanted %u\n",
			(unsigned long long)rec->start,
			found, rec->refs);
	}
out:
	return err;
}

static int free_all_backrefs(struct extent_record *rec)
{
	struct extent_backref *back;
	struct list_head *cur;
	while (!list_empty(&rec->backrefs)) {
		cur = rec->backrefs.next;
		back = list_entry(cur, struct extent_backref, list);
		list_del(cur);
		free(back);
	}
	return 0;
}

static int maybe_free_extent_rec(struct cache_tree *extent_cache,
				 struct extent_record *rec)
{
	if (rec->checked && rec->extent_item_refs == rec->refs &&
	    rec->refs > 0 && !all_backpointers_checked(rec, 0)) {
		remove_cache_extent(extent_cache, &rec->cache);
		free_all_backrefs(rec);
		free(rec);
	}
	return 0;
}

static int check_block(struct btrfs_root *root,
		       struct cache_tree *extent_cache,
		       struct extent_buffer *buf)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int ret = 1;

	cache = find_cache_extent(extent_cache, buf->start, buf->len);
	if (!cache)
		return 1;
	rec = container_of(cache, struct extent_record, cache);
	if (btrfs_is_leaf(buf)) {
		ret = check_leaf(root, &rec->parent_key, buf);
	} else {
		ret = check_node(root, &rec->parent_key, buf);
	}
	rec->checked = 1;
	if (!ret)
		maybe_free_extent_rec(extent_cache, rec);
	return ret;
}

static struct extent_backref *find_backref(struct extent_record *rec,
					   u64 parent, u64 root, u64 gen)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *back;

	while(cur != &rec->backrefs) {
		back = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (back->parent != parent)
			continue;
		if (back->root != root || back->generation != gen)
			continue;
		return back;
	}
	return NULL;
}

static struct extent_backref *alloc_backref(struct extent_record *rec,
					    u64 parent, u64 root, u64 gen,
					    u64 owner, u64 owner_offset)
{
	struct extent_backref *ref = malloc(sizeof(*ref));
	ref->parent = parent;
	ref->root = root;
	ref->generation = gen;
	ref->owner = owner;
	ref->offset = owner_offset;
	ref->num_refs = 0;
	ref->found_extent_tree = 0;
	ref->found_ref = 0;
	list_add_tail(&ref->list, &rec->backrefs);
	return ref;
}

static int add_extent_rec(struct cache_tree *extent_cache,
			  struct btrfs_disk_key *parent_key,
			  u64 ref, u64 start, u64 nr,
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
		if (rec->nr == 1)
			rec->nr = nr;

		if (start != rec->start) {
			fprintf(stderr, "warning, start mismatch %llu %llu\n",
				(unsigned long long)rec->start,
				(unsigned long long)start);
			ret = 1;
		}
		if (extent_item_refs) {
			if (rec->extent_item_refs) {
				fprintf(stderr, "block %llu rec "
					"extent_item_refs %u, passed %u\n",
					(unsigned long long)start,
					rec->extent_item_refs,
					extent_item_refs);
			}
			rec->extent_item_refs = extent_item_refs;
		}
		if (set_checked)
			rec->checked = 1;

		if (parent_key)
			memcpy(&rec->parent_key, parent_key,
			       sizeof(*parent_key));

		maybe_free_extent_rec(extent_cache, rec);
		return ret;
	}
	rec = malloc(sizeof(*rec));
	if (start == 0)
		extent_item_refs = 0;
	rec->start = start;
	rec->nr = nr;
	rec->checked = 0;
	INIT_LIST_HEAD(&rec->backrefs);

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

static int add_backref(struct cache_tree *extent_cache, u64 bytenr,
		       u64 parent, u64 root, u64 gen, u64 owner,
		       u64 owner_offset, u32 num_refs, int found_ref)
{
	struct extent_record *rec;
	struct extent_backref *back;
	struct cache_extent *cache;

	cache = find_cache_extent(extent_cache, bytenr, 1);
	if (!cache) {
		add_extent_rec(extent_cache, NULL, 0, bytenr, 1, 0, 0, 0);
		cache = find_cache_extent(extent_cache, bytenr, 1);
		if (!cache)
			abort();
	}

	rec = container_of(cache, struct extent_record, cache);
	if (rec->start != bytenr) {
		abort();
	}
	back = find_backref(rec, parent, root, gen);
	if (!back)
		back = alloc_backref(rec, parent, root, gen, owner,
				     owner_offset);

	if (found_ref) {
		if (back->found_ref > 0 &&
		    back->owner < BTRFS_FIRST_FREE_OBJECTID) {
			fprintf(stderr, "Extent back ref already exists "
				"for %llu parent %llu root %llu gen %llu "
				"owner %llu offset %llu num_refs %lu\n",
				(unsigned long long)parent,
				(unsigned long long)bytenr,
				(unsigned long long)root,
				(unsigned long long)gen,
				(unsigned long long)owner,
				(unsigned long long)owner_offset,
				(unsigned long)num_refs);
		}
		BUG_ON(num_refs != 1);
		back->found_ref += 1;
	} else {
		if (back->found_extent_tree) {
			fprintf(stderr, "Extent back ref already exists "
				"for %llu parent %llu root %llu gen %llu "
				"owner %llu offset %llu num_refs %lu\n",
				(unsigned long long)parent,
				(unsigned long long)bytenr,
				(unsigned long long)root,
				(unsigned long long)gen,
				(unsigned long long)owner,
				(unsigned long long)owner_offset,
				(unsigned long)num_refs);
		}
		back->num_refs = num_refs;
		back->found_extent_tree = 1;
	}
	return 0;
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
	struct extent_buffer *buf;
	u64 bytenr;
	u32 size;
	int ret;
	int i;
	int nritems;
	struct btrfs_extent_ref *ref;
	struct btrfs_disk_key disk_key;
	struct cache_extent *cache;
	int reada_bits;

	ret = pick_next_pending(pending, reada, nodes, *last, bits,
				bits_nr, &reada_bits);
	if (ret == 0) {
		return 1;
	}
	if (!reada_bits) {
		for(i = 0; i < ret; i++) {
			insert_cache_extent(reada, bits[i].start,
					    bits[i].size);

			/* fixme, get the parent transid */
			readahead_tree_block(root, bits[i].start,
					     bits[i].size, 0);
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

	/* fixme, get the real parent transid */
	buf = read_tree_block(root, bytenr, size, 0);
	nritems = btrfs_header_nritems(buf);
	ret = check_block(root, extent_cache, buf);
	if (ret) {
		fprintf(stderr, "bad block %llu\n",
			(unsigned long long)bytenr);
	}
	if (btrfs_is_leaf(buf)) {
		btree_space_waste += btrfs_leaf_free_space(root, buf);
		for (i = 0; i < nritems; i++) {
			struct btrfs_file_extent_item *fi;
			btrfs_item_key(buf, &disk_key, i);
			if (btrfs_disk_key_type(&disk_key) ==
			    BTRFS_EXTENT_ITEM_KEY) {
				struct btrfs_key found;
				struct btrfs_extent_item *ei;
				btrfs_disk_key_to_cpu(&found, &disk_key);
				ei = btrfs_item_ptr(buf, i,
						    struct btrfs_extent_item);
				add_extent_rec(extent_cache, NULL, 0,
					       found.objectid,
					       found.offset,
					       btrfs_extent_refs(buf, ei),
					       0, 0);
				continue;
			}
			if (btrfs_disk_key_type(&disk_key) ==
			    BTRFS_CSUM_ITEM_KEY) {
				total_csum_bytes +=
					btrfs_item_size_nr(buf, i);
				continue;
			}
			if (btrfs_disk_key_type(&disk_key) ==
			    BTRFS_BLOCK_GROUP_ITEM_KEY) {
				struct btrfs_block_group_item *bi;
				bi = btrfs_item_ptr(buf, i,
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
			if (btrfs_disk_key_type(&disk_key) ==
			    BTRFS_EXTENT_REF_KEY) {
				ref = btrfs_item_ptr(buf, i,
						     struct btrfs_extent_ref);
				add_backref(extent_cache,
					btrfs_disk_key_objectid(&disk_key),
					btrfs_disk_key_offset(&disk_key),
					btrfs_ref_root(buf, ref),
					btrfs_ref_generation(buf, ref),
					btrfs_ref_objectid(buf, ref),
					btrfs_ref_offset(buf, ref),
					btrfs_ref_num_refs(buf, ref), 0);
				continue;
			}
			if (btrfs_disk_key_type(&disk_key) !=
			    BTRFS_EXTENT_DATA_KEY)
				continue;
			fi = btrfs_item_ptr(buf, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(buf, fi) !=
			    BTRFS_FILE_EXTENT_REG)
				continue;
			if (btrfs_file_extent_disk_bytenr(buf, fi) == 0)
				continue;

			data_bytes_allocated +=
				btrfs_file_extent_disk_num_bytes(buf, fi);
			if (data_bytes_allocated < root->sectorsize) {
				abort();
			}
			data_bytes_referenced +=
				btrfs_file_extent_num_bytes(buf, fi);
			ret = add_extent_rec(extent_cache, NULL, bytenr,
				   btrfs_file_extent_disk_bytenr(buf, fi),
				   btrfs_file_extent_disk_num_bytes(buf, fi),
				   0, 1, 1);
			add_backref(extent_cache,
				    btrfs_file_extent_disk_bytenr(buf, fi),
				    buf->start,
				    btrfs_header_owner(buf),
				    btrfs_header_generation(buf),
				    btrfs_disk_key_objectid(&disk_key),
				    btrfs_disk_key_offset(&disk_key), 1, 1);
			BUG_ON(ret);
		}
	} else {
		int level;
		level = btrfs_header_level(buf);
		for (i = 0; i < nritems; i++) {
			u64 ptr = btrfs_node_blockptr(buf, i);
			u32 size = btrfs_level_size(root, level - 1);
			btrfs_node_key(buf, &disk_key, i);
			ret = add_extent_rec(extent_cache,
					     &disk_key,
					     bytenr, ptr, size,
					     0, 1, 0);
			BUG_ON(ret);

			add_backref(extent_cache, ptr, buf->start,
				btrfs_header_owner(buf),
				btrfs_header_generation(buf),
				level - 1, 0, 1, 1);

			if (level > 1) {
				add_pending(nodes, seen, ptr, size);
			} else {
				add_pending(pending, seen, ptr, size);
			}
		}
		btree_space_waste += (BTRFS_NODEPTRS_PER_BLOCK(root) -
				      nritems) * sizeof(struct btrfs_key_ptr);
	}
	total_btree_bytes += buf->len;
	free_extent_buffer(buf);
	return 0;
}

static int add_root_to_pending(struct extent_buffer *buf,
			       struct block_info *bits,
			       int bits_nr,
			       struct cache_tree *extent_cache,
			       struct cache_tree *pending,
			       struct cache_tree *seen,
			       struct cache_tree *reada,
			       struct cache_tree *nodes, u64 root_objectid)
{
	if (btrfs_header_level(buf) > 0)
		add_pending(nodes, seen, buf->start, buf->len);
	else
		add_pending(pending, seen, buf->start, buf->len);
	add_extent_rec(extent_cache, NULL, 0, buf->start, buf->len,
		       0, 1, 0);

	add_backref(extent_cache, buf->start, buf->start, root_objectid,
		    btrfs_header_generation(buf),
		    btrfs_header_level(buf), 0, 1, 1);
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
		if (all_backpointers_checked(rec, 1)) {
			fprintf(stderr, "backpointer mismatch on [%llu %llu]\n",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);

			err = 1;
		}
		remove_cache_extent(extent_cache, cache);
		free_all_backrefs(rec);
		free(rec);
	}
	return err;
}

void print_usage(void) {
	fprintf(stderr, "usage: btrfsck dev\n");
	fprintf(stderr, "%s\n", BTRFS_BUILD_VERSION);
	exit(1);
}

int main(int ac, char **av) {
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
	struct extent_buffer *leaf;
	int slot;
	struct btrfs_root_item ri;

	if (ac < 2)
		print_usage();

	radix_tree_init();
	cache_tree_init(&extent_cache);
	cache_tree_init(&seen);
	cache_tree_init(&pending);
	cache_tree_init(&nodes);
	cache_tree_init(&reada);

	root = open_ctree(av[1], 0, 0);

	bits_nr = 1024;
	bits = malloc(bits_nr * sizeof(struct block_info));
	if (!bits) {
		perror("malloc");
		exit(1);
	}

	add_root_to_pending(root->fs_info->tree_root->node, bits, bits_nr,
			    &extent_cache, &pending, &seen, &reada, &nodes,
			    root->fs_info->tree_root->root_key.objectid);

	add_root_to_pending(root->fs_info->chunk_root->node, bits, bits_nr,
			    &extent_cache, &pending, &seen, &reada, &nodes,
			    root->fs_info->chunk_root->root_key.objectid);

	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	btrfs_set_key_type(&key, BTRFS_ROOT_ITEM_KEY);
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root,
					&key, &path, 0, 0);
	BUG_ON(ret < 0);
	while(1) {
		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret != 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (btrfs_key_type(&found_key) == BTRFS_ROOT_ITEM_KEY) {
			unsigned long offset;
			struct extent_buffer *buf;

			offset = btrfs_item_ptr_offset(leaf, path.slots[0]);
			read_extent_buffer(leaf, &ri, offset, sizeof(ri));
			buf = read_tree_block(root->fs_info->tree_root,
					      btrfs_root_bytenr(&ri),
					      btrfs_level_size(root,
					       btrfs_root_level(&ri)), 0);
			add_root_to_pending(buf, bits, bits_nr, &extent_cache,
					    &pending, &seen, &reada, &nodes,
					    found_key.objectid);
			free_extent_buffer(buf);
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
	close_ctree(root);
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
	printf("%s\n", BTRFS_BUILD_VERSION);
	return ret;
}
