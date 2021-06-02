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

#ifndef __PRINT_TREE_H__
#define __PRINT_TREE_H__

enum {
	/* Depth-first search, nodes and leaves can be interleaved */
	BTRFS_PRINT_TREE_DFS		= (1 << 0),
	/* Breadth-first search, first nodes, then leaves */
	BTRFS_PRINT_TREE_BFS		= (1 << 1),
	/* Follow to child nodes */
	BTRFS_PRINT_TREE_FOLLOW		= (1 << 2),
	/* Print checksum of node/leaf */
	BTRFS_PRINT_TREE_CSUM_HEADERS	= (1 << 3),
	/* Print checksums in checksum items */
	BTRFS_PRINT_TREE_CSUM_ITEMS	= (1 << 4),
	BTRFS_PRINT_TREE_DEFAULT = BTRFS_PRINT_TREE_BFS,
};

void btrfs_print_tree(struct extent_buffer *eb, unsigned int mode);
void btrfs_print_leaf(struct extent_buffer *eb, unsigned int mode);

void btrfs_print_key(struct btrfs_disk_key *disk_key);
void print_chunk_item(struct extent_buffer *eb, struct btrfs_chunk *chunk);
void print_extent_item(struct extent_buffer *eb, int slot, int metadata);
void print_objectid(FILE *stream, u64 objectid, u8 type);
void print_key_type(FILE *stream, u64 objectid, u8 type);
void btrfs_print_superblock(struct btrfs_super_block *sb, int full);

#endif
