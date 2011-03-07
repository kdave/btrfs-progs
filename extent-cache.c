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
#include <stdio.h>
#include <stdlib.h>
#include "kerncompat.h"
#include "extent-cache.h"

void cache_tree_init(struct cache_tree *tree)
{
	tree->root.rb_node = NULL;
}

static struct rb_node *tree_insert(struct rb_root *root, u64 offset,
				   u64 size, struct rb_node *node)
{
	struct rb_node ** p = &root->rb_node;
	struct rb_node * parent = NULL;
	struct cache_extent *entry;

	while(*p) {
		parent = *p;
		entry = rb_entry(parent, struct cache_extent, rb_node);

		if (offset + size <= entry->start)
			p = &(*p)->rb_left;
		else if (offset >= entry->start + entry->size)
			p = &(*p)->rb_right;
		else
			return parent;
	}

	entry = rb_entry(parent, struct cache_extent, rb_node);
	rb_link_node(node, parent, p);
	rb_insert_color(node, root);
	return NULL;
}

static struct rb_node *__tree_search(struct rb_root *root, u64 offset,
				     u64 size, struct rb_node **prev_ret)
{
	struct rb_node * n = root->rb_node;
	struct rb_node *prev = NULL;
	struct cache_extent *entry;
	struct cache_extent *prev_entry = NULL;

	while(n) {
		entry = rb_entry(n, struct cache_extent, rb_node);
		prev = n;
		prev_entry = entry;

		if (offset + size <= entry->start)
			n = n->rb_left;
		else if (offset >= entry->start + entry->size)
			n = n->rb_right;
		else
			return n;
	}
	if (!prev_ret)
		return NULL;

	while(prev && offset >= prev_entry->start + prev_entry->size) {
		prev = rb_next(prev);
		prev_entry = rb_entry(prev, struct cache_extent, rb_node);
	}
	*prev_ret = prev;
	return NULL;
}

struct cache_extent *alloc_cache_extent(u64 start, u64 size)
{
	struct cache_extent *pe = malloc(sizeof(*pe));

	if (!pe)
		return pe;
	pe->start = start;
	pe->size = size;
	return pe;
}

int insert_existing_cache_extent(struct cache_tree *tree,
				 struct cache_extent *pe)
{
	struct rb_node *found;

	found = tree_insert(&tree->root, pe->start, pe->size, &pe->rb_node);
	if (found)
		return -EEXIST;

	return 0;
}

int insert_cache_extent(struct cache_tree *tree, u64 start, u64 size)
{
	struct cache_extent *pe = alloc_cache_extent(start, size);
	int ret;
	ret = insert_existing_cache_extent(tree, pe);
	if (ret)
		free(pe);
	return ret;
}

struct cache_extent *find_cache_extent(struct cache_tree *tree,
					   u64 start, u64 size)
{
	struct rb_node *prev;
	struct rb_node *ret;
	struct cache_extent *entry;
	ret = __tree_search(&tree->root, start, size, &prev);
	if (!ret)
		return NULL;

	entry = rb_entry(ret, struct cache_extent, rb_node);
	return entry;
}

struct cache_extent *find_first_cache_extent(struct cache_tree *tree,
						 u64 start)
{
	struct rb_node *prev;
	struct rb_node *ret;
	struct cache_extent *entry;

	ret = __tree_search(&tree->root, start, 1, &prev);
	if (!ret)
		ret = prev;
	if (!ret)
		return NULL;
	entry = rb_entry(ret, struct cache_extent, rb_node);
	return entry;
}

struct cache_extent *prev_cache_extent(struct cache_extent *pe)
{
	struct rb_node *node = rb_prev(&pe->rb_node);

	if (!node)
		return NULL;
	return rb_entry(node, struct cache_extent, rb_node);
}

struct cache_extent *next_cache_extent(struct cache_extent *pe)
{
	struct rb_node *node = rb_next(&pe->rb_node);

	if (!node)
		return NULL;
	return rb_entry(node, struct cache_extent, rb_node);
}

void remove_cache_extent(struct cache_tree *tree,
				 struct cache_extent *pe)
{
	rb_erase(&pe->rb_node, &tree->root);
}

