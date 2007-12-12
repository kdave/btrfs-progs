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
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"

static int print_dir_item(struct btrfs_item *item,
			  struct btrfs_dir_item *di)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	total = btrfs_item_size(item);
	while(cur < total) {
		printf("\t\tdir index %llu type %u\n",
		     (unsigned long long)btrfs_disk_key_objectid(&di->location),
		     btrfs_dir_type(di));
		printf("\t\tname: %.*s\n",
		       btrfs_dir_name_len(di),(char *)(di + 1));
		if (btrfs_dir_data_len(di))
			printf("\t\tdata: %.*s\n", btrfs_dir_data_len(di),
			       (char *)((char *)(di + 1) + btrfs_dir_name_len(di)));
		len = sizeof(*di) + btrfs_dir_name_len(di) + btrfs_dir_data_len(di);
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}
	return 0;
}

static int print_inode_ref_item(struct btrfs_item *item,
				struct btrfs_inode_ref *ref)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	total = btrfs_item_size(item);
	while(cur < total) {
		len = btrfs_inode_ref_name_len(ref);
		printf("\t\tinode ref name: %.*s\n", len, (char *)(ref + 1));
		len += sizeof(*ref);
		ref = (struct btrfs_inode_ref *)((char *)ref + len);
		cur += len;
	}
	return 0;
}

void btrfs_print_leaf(struct btrfs_root *root, struct btrfs_leaf *l)
{
	int i;
	u32 nr = btrfs_header_nritems(&l->header);
	struct btrfs_item *item;
	struct btrfs_extent_item *ei;
	struct btrfs_root_item *ri;
	struct btrfs_dir_item *di;
	struct btrfs_inode_item *ii;
	struct btrfs_file_extent_item *fi;
	struct btrfs_csum_item *ci;
	struct btrfs_block_group_item *bi;
	struct btrfs_extent_ref *ref;
	struct btrfs_inode_ref *iref;
	u32 type;

	printf("leaf %llu ptrs %d free space %d generation %llu owner %llu\n",
		(unsigned long long)btrfs_header_bytenr(&l->header), nr,
		btrfs_leaf_free_space(root, l),
		(unsigned long long)btrfs_header_generation(&l->header),
		(unsigned long long)btrfs_header_owner(&l->header));
	fflush(stdout);
	for (i = 0 ; i < nr ; i++) {
		item = l->items + i;
		type = btrfs_disk_key_type(&item->key);
		printf("\titem %d key (%llu %x %llu) itemoff %d itemsize %d\n",
			i,
			(unsigned long long)btrfs_disk_key_objectid(&item->key),
			btrfs_disk_key_type(&item->key),
			(unsigned long long)btrfs_disk_key_offset(&item->key),
			btrfs_item_offset(item),
			btrfs_item_size(item));
		switch (type) {
		case BTRFS_INODE_ITEM_KEY:
			ii = btrfs_item_ptr(l, i, struct btrfs_inode_item);
			printf("\t\tinode generation %llu size %llu block group %llu mode %o links %u\n",
			       (unsigned long long)btrfs_inode_generation(ii),
			       (unsigned long long)btrfs_inode_size(ii),
			       (unsigned long long)btrfs_inode_block_group(ii),
			       btrfs_inode_mode(ii),
			       btrfs_inode_nlink(ii));
			break;
		case BTRFS_INODE_REF_KEY:
			iref = btrfs_item_ptr(l, i, struct btrfs_inode_ref);
			print_inode_ref_item(l->items + i, iref);
			break;
		case BTRFS_DIR_ITEM_KEY:
			di = btrfs_item_ptr(l, i, struct btrfs_dir_item);
			print_dir_item(l->items + i, di);
			break;
		case BTRFS_XATTR_ITEM_KEY:
		case BTRFS_DIR_INDEX_KEY:
			di = btrfs_item_ptr(l, i, struct btrfs_dir_item);
			print_dir_item(l->items + i, di);
			break;
		case BTRFS_ROOT_ITEM_KEY:
			ri = btrfs_item_ptr(l, i, struct btrfs_root_item);
			printf("\t\troot data bytenr %llu level %d dirid %llu refs %u\n",
				(unsigned long long)btrfs_root_bytenr(ri),
				ri->level,
				(unsigned long long)btrfs_root_dirid(ri),
				btrfs_root_refs(ri));
			if (1 || btrfs_root_refs(ri) == 0) {
				struct btrfs_key drop_key;
				btrfs_disk_key_to_cpu(&drop_key,
						      &ri->drop_progress);
				printf("\t\tdrop key %Lu %x %Lu level %d\n",
				       (unsigned long long)drop_key.objectid,
				       drop_key.type,
				       (unsigned long long)drop_key.offset,
				       ri->drop_level);
			}
			break;
		case BTRFS_EXTENT_ITEM_KEY:
			ei = btrfs_item_ptr(l, i, struct btrfs_extent_item);
			printf("\t\textent data refs %u\n",
				btrfs_extent_refs(ei));
			break;
		case BTRFS_EXTENT_REF_KEY:
			ref = btrfs_item_ptr(l, i, struct btrfs_extent_ref);
			printf("\t\textent back ref root %llu gen %llu "
			       "owner %llu offset %llu\n",
			       (unsigned long long)btrfs_ref_root(ref),
			       (unsigned long long)btrfs_ref_generation(ref),
			       (unsigned long long)btrfs_ref_objectid(ref),
			       (unsigned long long)btrfs_ref_offset(ref));
			break;
		case BTRFS_CSUM_ITEM_KEY:
			ci = btrfs_item_ptr(l, i,
					    struct btrfs_csum_item);
			printf("\t\tcsum item\n");
			break;
		case BTRFS_EXTENT_DATA_KEY:
			fi = btrfs_item_ptr(l, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(fi) ==
			    BTRFS_FILE_EXTENT_INLINE) {
				printf("\t\tinline extent data size %u\n",
			           btrfs_file_extent_inline_len(l->items + i));
				break;
			}
			printf("\t\textent data disk byte %llu nr %llu\n",
			       (unsigned long long)btrfs_file_extent_disk_bytenr(fi),
			       (unsigned long long)btrfs_file_extent_disk_num_bytes(fi));
			printf("\t\textent data offset %llu nr %llu\n",
			  (unsigned long long)btrfs_file_extent_offset(fi),
			  (unsigned long long)btrfs_file_extent_num_bytes(fi));
			break;
		case BTRFS_BLOCK_GROUP_ITEM_KEY:
			bi = btrfs_item_ptr(l, i,
					    struct btrfs_block_group_item);
			printf("\t\tblock group used %llu flags %x\n",
			       (unsigned long long)btrfs_block_group_used(bi),
			       bi->flags);
			break;
		case BTRFS_STRING_ITEM_KEY:
			printf("\t\titem data %.*s\n", btrfs_item_size(item),
				btrfs_leaf_data(l) + btrfs_item_offset(item));
			break;
		};
		fflush(stdout);
	}
}
void btrfs_print_tree(struct btrfs_root *root, struct btrfs_buffer *t)
{
	int i;
	u32 nr;
	struct btrfs_node *c;
	u32 size;

	if (!t)
		return;
	c = &t->node;
	nr = btrfs_header_nritems(&c->header);
	if (btrfs_is_leaf(c)) {
		btrfs_print_leaf(root, (struct btrfs_leaf *)c);
		return;
	}
	printf("node %llu level %d ptrs %d free %u generation %llu owner %llu\n",
	       (unsigned long long)t->bytenr,
	        btrfs_header_level(&c->header), nr,
		(u32)BTRFS_NODEPTRS_PER_BLOCK(root) - nr,
		(unsigned long long)btrfs_header_generation(&c->header),
		(unsigned long long)btrfs_header_owner(&c->header));
	fflush(stdout);
	size = btrfs_level_size(root, btrfs_header_level(&c->header) - 1);
	for (i = 0; i < nr; i++) {
		u64 blocknr = btrfs_node_blockptr(c, i);
		printf("\tkey %d (%llu %x %llu) block %llu (%llu) gen %llu\n",
		       i,
		       (unsigned long long)c->ptrs[i].key.objectid,
		       c->ptrs[i].key.type,
		       (unsigned long long)c->ptrs[i].key.offset,
		       (unsigned long long)blocknr,
		       (unsigned long long)blocknr / size,
		       (unsigned long long)btrfs_node_ptr_generation(c, i));
		fflush(stdout);
	}
	for (i = 0; i < nr; i++) {
		struct btrfs_buffer *next_buf = read_tree_block(root,
						btrfs_node_blockptr(c, i),
						size);
		struct btrfs_node *next = &next_buf->node;
		if (btrfs_is_leaf(next) &&
		    btrfs_header_level(&c->header) != 1)
			BUG();
		if (btrfs_header_level(&next->header) !=
			btrfs_header_level(&c->header) - 1)
			BUG();
		btrfs_print_tree(root, next_buf);
		btrfs_block_release(root, next_buf);
	}
}

