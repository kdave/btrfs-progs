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
#include <uuid/uuid.h>
#include "kerncompat.h"
#include "radix-tree.h"
#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"

static int print_dir_item(struct extent_buffer *eb, struct btrfs_item *item,
			  struct btrfs_dir_item *di)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u32 data_len;
	char namebuf[BTRFS_NAME_LEN];
	struct btrfs_disk_key location;

	total = btrfs_item_size(eb, item);
	while(cur < total) {
		btrfs_dir_item_key(eb, di, &location);
		printf("\t\tlocation ");
		btrfs_print_key(&location);
		printf(" type %u\n", btrfs_dir_type(eb, di));
		name_len = btrfs_dir_name_len(eb, di);
		data_len = btrfs_dir_data_len(eb, di);
		len = (name_len <= sizeof(namebuf))? name_len: sizeof(namebuf);
		read_extent_buffer(eb, namebuf, (unsigned long)(di + 1), len);
		printf("\t\tnamelen %u datalen %u name: %.*s\n",
		       name_len, data_len, len, namebuf);
		len = sizeof(*di) + name_len + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}
	return 0;
}

static int print_inode_ref_item(struct extent_buffer *eb, struct btrfs_item *item,
				struct btrfs_inode_ref *ref)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	char namebuf[BTRFS_NAME_LEN];
	total = btrfs_item_size(eb, item);
	while(cur < total) {
		name_len = btrfs_inode_ref_name_len(eb, ref);
		index = btrfs_inode_ref_index(eb, ref);
		len = (name_len <= sizeof(namebuf))? name_len: sizeof(namebuf);
		read_extent_buffer(eb, namebuf, (unsigned long)(ref + 1), len);
		printf("\t\tinode ref index %llu namelen %u name: %.*s\n",
		       (unsigned long long)index, name_len, len, namebuf);
		len = sizeof(*ref) + name_len;
		ref = (struct btrfs_inode_ref *)((char *)ref + len);
		cur += len;
	}
	return 0;
}

static void print_chunk(struct extent_buffer *eb, struct btrfs_chunk *chunk)
{
	int num_stripes = btrfs_chunk_num_stripes(eb, chunk);
	int i;
	printf("\t\tchunk length %llu owner %llu type %llu num_stripes %d\n",
	       (unsigned long long)btrfs_chunk_length(eb, chunk),
	       (unsigned long long)btrfs_chunk_owner(eb, chunk),
	       (unsigned long long)btrfs_chunk_type(eb, chunk),
	       num_stripes);
	for (i = 0 ; i < num_stripes ; i++) {
		printf("\t\t\tstripe %d devid %llu offset %llu\n", i,
		      (unsigned long long)btrfs_stripe_devid_nr(eb, chunk, i),
		      (unsigned long long)btrfs_stripe_offset_nr(eb, chunk, i));
	}
}
static void print_dev_item(struct extent_buffer *eb,
			   struct btrfs_dev_item *dev_item)
{
	printf("\t\tdev item devid %llu "
	       "total_bytes %llu bytes used %Lu\n",
	       (unsigned long long)btrfs_device_id(eb, dev_item),
	       (unsigned long long)btrfs_device_total_bytes(eb, dev_item),
	       (unsigned long long)btrfs_device_bytes_used(eb, dev_item));
}

static void print_uuids(struct extent_buffer *eb)
{
	char fs_uuid[37];
	char chunk_uuid[37];
	u8 disk_uuid[BTRFS_UUID_SIZE];

	read_extent_buffer(eb, disk_uuid, (unsigned long)btrfs_header_fsid(eb),
			   BTRFS_FSID_SIZE);

	fs_uuid[36] = '\0';
	uuid_unparse(disk_uuid, fs_uuid);

	read_extent_buffer(eb, disk_uuid,
			   (unsigned long)btrfs_header_chunk_tree_uuid(eb),
			   BTRFS_UUID_SIZE);

	chunk_uuid[36] = '\0';
	uuid_unparse(disk_uuid, chunk_uuid);
	printf("fs uuid %s\nchunk uuid %s\n", fs_uuid, chunk_uuid);
}

static void print_file_extent_item(struct extent_buffer *eb,
				   struct btrfs_item *item,
				   struct btrfs_file_extent_item *fi)
{
	int extent_type = btrfs_file_extent_type(eb, fi);

	if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
		printf("\t\tinline extent data size %u "
		       "ram %u compress %d\n",
		  btrfs_file_extent_inline_item_len(eb, item),
		  btrfs_file_extent_inline_len(eb, fi),
		  btrfs_file_extent_compression(eb, fi));
		return;
	}
	if (extent_type == BTRFS_FILE_EXTENT_PREALLOC) {
		printf("\t\tprealloc data disk byte %llu nr %llu\n",
		  (unsigned long long)btrfs_file_extent_disk_bytenr(eb, fi),
		  (unsigned long long)btrfs_file_extent_disk_num_bytes(eb, fi));
		printf("\t\tprealloc data offset %llu nr %llu\n",
		  (unsigned long long)btrfs_file_extent_offset(eb, fi),
		  (unsigned long long)btrfs_file_extent_num_bytes(eb, fi));
		return;
	}
	printf("\t\textent data disk byte %llu nr %llu\n",
		(unsigned long long)btrfs_file_extent_disk_bytenr(eb, fi),
		(unsigned long long)btrfs_file_extent_disk_num_bytes(eb, fi));
	printf("\t\textent data offset %llu nr %llu ram %llu\n",
		(unsigned long long)btrfs_file_extent_offset(eb, fi),
		(unsigned long long)btrfs_file_extent_num_bytes(eb, fi),
		(unsigned long long)btrfs_file_extent_ram_bytes(eb, fi));
	printf("\t\textent compression %d\n",
	       btrfs_file_extent_compression(eb, fi));
}

static void print_extent_item(struct extent_buffer *eb, int slot)
{
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	struct btrfs_shared_data_ref *sref;
	struct btrfs_disk_key key;
	unsigned long end;
	unsigned long ptr;
	int type;
	u32 item_size = btrfs_item_size_nr(eb, slot);
	u64 flags;
	u64 offset;

	if (item_size < sizeof(*ei)) {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
		struct btrfs_extent_item_v0 *ei0;
		BUG_ON(item_size != sizeof(*ei0));
		ei0 = btrfs_item_ptr(eb, slot, struct btrfs_extent_item_v0);
		printf("\t\textent refs %u\n",
		       btrfs_extent_refs_v0(eb, ei0));
		return;
#else
		BUG();
#endif
	}

	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
	flags = btrfs_extent_flags(eb, ei);

	printf("\t\textent refs %llu gen %llu flags %llu\n",
	       (unsigned long long)btrfs_extent_refs(eb, ei),
	       (unsigned long long)btrfs_extent_generation(eb, ei),
	       (unsigned long long)flags);

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		struct btrfs_tree_block_info *info;
		info = (struct btrfs_tree_block_info *)(ei + 1);
		btrfs_tree_block_key(eb, info, &key);
		printf("\t\ttree block key (%llu %x %llu) level %d\n",
		       (unsigned long long)btrfs_disk_key_objectid(&key),
		       key.type,
		       (unsigned long long)btrfs_disk_key_offset(&key),
		       btrfs_tree_block_level(eb, info));
		iref = (struct btrfs_extent_inline_ref *)(info + 1);
	} else {
		iref = (struct btrfs_extent_inline_ref *)(ei + 1);
	}

	ptr = (unsigned long)iref;
	end = (unsigned long)ei + item_size;
	while (ptr < end) {
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(eb, iref);
		offset = btrfs_extent_inline_ref_offset(eb, iref);
		switch (type) {
		case BTRFS_TREE_BLOCK_REF_KEY:
			printf("\t\ttree block backref root %llu\n",
			       (unsigned long long)offset);
			break;
		case BTRFS_SHARED_BLOCK_REF_KEY:
			printf("\t\tshared block backref parent %llu\n",
			       (unsigned long long)offset);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY:
			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			printf("\t\textent data backref root %llu "
			       "objectid %llu offset %llu count %u\n",
			       (unsigned long long)btrfs_extent_data_ref_root(eb, dref),
			       (unsigned long long)btrfs_extent_data_ref_objectid(eb, dref),
			       (unsigned long long)btrfs_extent_data_ref_offset(eb, dref),
			       btrfs_extent_data_ref_count(eb, dref));
			break;
		case BTRFS_SHARED_DATA_REF_KEY:
			sref = (struct btrfs_shared_data_ref *)(iref + 1);
			printf("\t\tshared data backref parent %llu count %u\n",
			       (unsigned long long)offset,
			       btrfs_shared_data_ref_count(eb, sref));
			break;
		default:
			BUG();
		}
		ptr += btrfs_extent_inline_ref_size(type);
	}
	WARN_ON(ptr > end);
}

#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
static void print_extent_ref_v0(struct extent_buffer *eb, int slot)
{
	struct btrfs_extent_ref_v0 *ref0;

	ref0 = btrfs_item_ptr(eb, slot, struct btrfs_extent_ref_v0);
	printf("\t\textent back ref root %llu gen %llu "
		"owner %llu num_refs %lu\n",
		(unsigned long long)btrfs_ref_root_v0(eb, ref0),
		(unsigned long long)btrfs_ref_generation_v0(eb, ref0),
		(unsigned long long)btrfs_ref_objectid_v0(eb, ref0),
		(unsigned long)btrfs_ref_count_v0(eb, ref0));
}
#endif

static void print_root_ref(struct extent_buffer *leaf, int slot, char *tag)
{
	struct btrfs_root_ref *ref;
	char namebuf[BTRFS_NAME_LEN];
	int namelen;

	ref = btrfs_item_ptr(leaf, slot, struct btrfs_root_ref);
	namelen = btrfs_root_ref_name_len(leaf, ref);
	read_extent_buffer(leaf, namebuf, (unsigned long)(ref + 1), namelen);
	printf("\t\troot %s key dirid %llu sequence %llu name %.*s\n", tag,
	       (unsigned long long)btrfs_root_ref_dirid(leaf, ref),
	       (unsigned long long)btrfs_root_ref_sequence(leaf, ref),
	       namelen, namebuf);
}

static void print_key_type(u8 type)
{
	switch (type) {
	case BTRFS_INODE_ITEM_KEY:
		printf("INODE_ITEM");
		break;
	case BTRFS_INODE_REF_KEY:
		printf("INODE_REF");
		break;
	case BTRFS_DIR_ITEM_KEY:
		printf("DIR_ITEM");
		break;
	case BTRFS_DIR_INDEX_KEY:
		printf("DIR_INDEX");
		break;
	case BTRFS_DIR_LOG_ITEM_KEY:
		printf("DIR_LOG_ITEM");
		break;
	case BTRFS_DIR_LOG_INDEX_KEY:
		printf("DIR_LOG_INDEX");
		break;
	case BTRFS_XATTR_ITEM_KEY:
		printf("XATTR_ITEM");
		break;
	case BTRFS_ORPHAN_ITEM_KEY:
		printf("ORPHAN_ITEM");
		break;
	case BTRFS_ROOT_ITEM_KEY:
		printf("ROOT_ITEM");
		break;
	case BTRFS_ROOT_REF_KEY:
		printf("ROOT_REF");
		break;
	case BTRFS_ROOT_BACKREF_KEY:
		printf("ROOT_BACKREF");
		break;
	case BTRFS_EXTENT_ITEM_KEY:
		printf("EXTENT_ITEM");
		break;
	case BTRFS_TREE_BLOCK_REF_KEY:
		printf("TREE_BLOCK_REF");
		break;
	case BTRFS_SHARED_BLOCK_REF_KEY:
		printf("SHARED_BLOCK_REF");
		break;
	case BTRFS_EXTENT_DATA_REF_KEY:
		printf("EXTENT_DATA_REF");
		break;
	case BTRFS_SHARED_DATA_REF_KEY:
		printf("SHARED_DATA_REF");
		break;
	case BTRFS_EXTENT_REF_V0_KEY:
		printf("EXTENT_REF_V0");
		break;
	case BTRFS_CSUM_ITEM_KEY:
		printf("CSUM_ITEM");
		break;
	case BTRFS_EXTENT_CSUM_KEY:
		printf("EXTENT_CSUM");
		break;
	case BTRFS_EXTENT_DATA_KEY:
		printf("EXTENT_DATA");
		break;
	case BTRFS_BLOCK_GROUP_ITEM_KEY:
		printf("BLOCK_GROUP_ITEM");
		break;
	case BTRFS_CHUNK_ITEM_KEY:
		printf("CHUNK_ITEM");
		break;
	case BTRFS_DEV_ITEM_KEY:
		printf("DEV_ITEM");
		break;
	case BTRFS_DEV_EXTENT_KEY:
		printf("DEV_EXTENT");
		break;
	case BTRFS_STRING_ITEM_KEY:
		printf("STRING_ITEM");
		break;
	default:
		printf("UNKNOWN");
	};
}

static void print_objectid(unsigned long long objectid, u8 type)
{
	if (type == BTRFS_DEV_EXTENT_KEY) {
		printf("%llu", objectid); /* device id */
		return;
	}

	switch (objectid) {
	case BTRFS_ROOT_TREE_OBJECTID:
		if (type == BTRFS_DEV_ITEM_KEY)
			printf("DEV_ITEMS");
		else
			printf("ROOT_TREE");
		break;
	case BTRFS_EXTENT_TREE_OBJECTID:
		printf("EXTENT_TREE");
		break;
	case BTRFS_CHUNK_TREE_OBJECTID:
		printf("CHUNK_TREE");
		break;
	case BTRFS_DEV_TREE_OBJECTID:
		printf("DEV_TREE");
		break;
	case BTRFS_FS_TREE_OBJECTID:
		printf("FS_TREE");
		break;
	case BTRFS_ROOT_TREE_DIR_OBJECTID:
		printf("ROOT_TREE_DIR");
		break;
	case BTRFS_CSUM_TREE_OBJECTID:
		printf("CSUM_TREE");
		break;
	case BTRFS_ORPHAN_OBJECTID:
		printf("ORPHAN");
		break;
	case BTRFS_TREE_LOG_OBJECTID:
		printf("TREE_LOG");
		break;
	case BTRFS_TREE_LOG_FIXUP_OBJECTID:
		printf("LOG_FIXUP");
		break;
	case BTRFS_TREE_RELOC_OBJECTID:
		printf("TREE_RELOC");
		break;
	case BTRFS_DATA_RELOC_TREE_OBJECTID:
		printf("DATA_RELOC_TREE");
		break;
	case BTRFS_EXTENT_CSUM_OBJECTID:
		printf("EXTENT_CSUM");
		break;
	case BTRFS_MULTIPLE_OBJECTIDS:
		printf("MULTIPLE");
		break;
	case BTRFS_FIRST_CHUNK_TREE_OBJECTID:
		printf("FIRST_CHUNK_TREE");
		break;
	default:
		printf("%llu", objectid);
	}
}

void btrfs_print_key(struct btrfs_disk_key *disk_key)
{
	u8 type;
	printf("key (");
	type = btrfs_disk_key_type(disk_key);
	print_objectid((unsigned long long)btrfs_disk_key_objectid(disk_key),
		type);
	printf(" ");
	print_key_type(type);
	printf(" %llu)", (unsigned long long)btrfs_disk_key_offset(disk_key));
}

void btrfs_print_leaf(struct btrfs_root *root, struct extent_buffer *l)
{
	int i;
	char *str;
	struct btrfs_item *item;
	struct btrfs_root_item *ri;
	struct btrfs_dir_item *di;
	struct btrfs_inode_item *ii;
	struct btrfs_file_extent_item *fi;
	struct btrfs_csum_item *ci;
	struct btrfs_block_group_item *bi;
	struct btrfs_extent_data_ref *dref;
	struct btrfs_shared_data_ref *sref;
	struct btrfs_inode_ref *iref;
	struct btrfs_dev_extent *dev_extent;
	struct btrfs_disk_key disk_key;
	struct btrfs_root_item root_item;
	struct btrfs_block_group_item bg_item;
	struct btrfs_dir_log_item *dlog;
	u32 nr = btrfs_header_nritems(l);
	u32 type;

	printf("leaf %llu items %d free space %d generation %llu owner %llu\n",
		(unsigned long long)btrfs_header_bytenr(l), nr,
		btrfs_leaf_free_space(root, l),
		(unsigned long long)btrfs_header_generation(l),
		(unsigned long long)btrfs_header_owner(l));
	print_uuids(l);
	fflush(stdout);
	for (i = 0 ; i < nr ; i++) {
		item = btrfs_item_nr(l, i);
		btrfs_item_key(l, &disk_key, i);
		type = btrfs_disk_key_type(&disk_key);
		printf("\titem %d ", i);
		btrfs_print_key(&disk_key);
		printf(" itemoff %d itemsize %d\n",
			btrfs_item_offset(l, item),
			btrfs_item_size(l, item));
		switch (type) {
		case BTRFS_INODE_ITEM_KEY:
			ii = btrfs_item_ptr(l, i, struct btrfs_inode_item);
			printf("\t\tinode generation %llu size %llu block group %llu mode %o links %u\n",
			       (unsigned long long)btrfs_inode_generation(l, ii),
			       (unsigned long long)btrfs_inode_size(l, ii),
			       (unsigned long long)btrfs_inode_block_group(l,ii),
			       btrfs_inode_mode(l, ii),
			       btrfs_inode_nlink(l, ii));
			break;
		case BTRFS_INODE_REF_KEY:
			iref = btrfs_item_ptr(l, i, struct btrfs_inode_ref);
			print_inode_ref_item(l, item, iref);
			break;
		case BTRFS_DIR_ITEM_KEY:
		case BTRFS_DIR_INDEX_KEY:
		case BTRFS_XATTR_ITEM_KEY:
			di = btrfs_item_ptr(l, i, struct btrfs_dir_item);
			print_dir_item(l, item, di);
			break;
		case BTRFS_DIR_LOG_INDEX_KEY:
		case BTRFS_DIR_LOG_ITEM_KEY:
			dlog = btrfs_item_ptr(l, i, struct btrfs_dir_log_item);
			printf("\t\tdir log end %Lu\n",
			       btrfs_dir_log_end(l, dlog));
		       break;
		case BTRFS_ORPHAN_ITEM_KEY:
			printf("\t\torphan item\n");
			break;
		case BTRFS_ROOT_ITEM_KEY:
			ri = btrfs_item_ptr(l, i, struct btrfs_root_item);
			read_extent_buffer(l, &root_item, (unsigned long)ri, sizeof(root_item));
			printf("\t\troot data bytenr %llu level %d dirid %llu refs %u\n",
				(unsigned long long)btrfs_root_bytenr(&root_item),
				btrfs_root_level(&root_item),
				(unsigned long long)btrfs_root_dirid(&root_item),
				btrfs_root_refs(&root_item));
			if (btrfs_root_refs(&root_item) == 0) {
				struct btrfs_key drop_key;
				btrfs_disk_key_to_cpu(&drop_key,
						      &root_item.drop_progress);
				printf("\t\tdrop ");
				btrfs_print_key(&root_item.drop_progress);
				printf(" level %d\n", root_item.drop_level);
			}
			break;
		case BTRFS_ROOT_REF_KEY:
			print_root_ref(l, i, "ref");
			break;
		case BTRFS_ROOT_BACKREF_KEY:
			print_root_ref(l, i, "backref");
			break;
		case BTRFS_EXTENT_ITEM_KEY:
			print_extent_item(l, i);
			break;
		case BTRFS_TREE_BLOCK_REF_KEY:
			printf("\t\ttree block backref\n");
			break;
		case BTRFS_SHARED_BLOCK_REF_KEY:
			printf("\t\tshared block backref\n");
			break;
		case BTRFS_EXTENT_DATA_REF_KEY:
			dref = btrfs_item_ptr(l, i, struct btrfs_extent_data_ref);
			printf("\t\textent data backref root %llu "
			       "objectid %llu offset %llu count %u\n",
			       (unsigned long long)btrfs_extent_data_ref_root(l, dref),
			       (unsigned long long)btrfs_extent_data_ref_objectid(l, dref),
			       (unsigned long long)btrfs_extent_data_ref_offset(l, dref),
			       btrfs_extent_data_ref_count(l, dref));
			break;
		case BTRFS_SHARED_DATA_REF_KEY:
			sref = btrfs_item_ptr(l, i, struct btrfs_shared_data_ref);
			printf("\t\tshared data backref count %u\n",
			       btrfs_shared_data_ref_count(l, sref));
			break;
		case BTRFS_EXTENT_REF_V0_KEY:
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
			print_extent_ref_v0(l, i);
#else
			BUG();
#endif
			break;
		case BTRFS_CSUM_ITEM_KEY:
			ci = btrfs_item_ptr(l, i, struct btrfs_csum_item);
			printf("\t\tcsum item\n");
			break;
		case BTRFS_EXTENT_CSUM_KEY:
			ci = btrfs_item_ptr(l, i, struct btrfs_csum_item);
			printf("\t\textent csum item\n");
			break;
		case BTRFS_EXTENT_DATA_KEY:
			fi = btrfs_item_ptr(l, i,
					    struct btrfs_file_extent_item);
			print_file_extent_item(l, item, fi);
			break;
		case BTRFS_BLOCK_GROUP_ITEM_KEY:
			bi = btrfs_item_ptr(l, i,
					    struct btrfs_block_group_item);
			read_extent_buffer(l, &bg_item, (unsigned long)bi,
					   sizeof(bg_item));
			printf("\t\tblock group used %llu chunk_objectid %llu flags %llu\n",
			       (unsigned long long)btrfs_block_group_used(&bg_item),
			       (unsigned long long)btrfs_block_group_chunk_objectid(&bg_item),
			       (unsigned long long)btrfs_block_group_flags(&bg_item));
			break;
		case BTRFS_CHUNK_ITEM_KEY:
			print_chunk(l, btrfs_item_ptr(l, i, struct btrfs_chunk));
			break;
		case BTRFS_DEV_ITEM_KEY:
			print_dev_item(l, btrfs_item_ptr(l, i,
					struct btrfs_dev_item));
			break;
		case BTRFS_DEV_EXTENT_KEY:
			dev_extent = btrfs_item_ptr(l, i,
						    struct btrfs_dev_extent);
			printf("\t\tdev extent chunk_tree %llu\n"
			       "\t\tchunk objectid %llu chunk offset %llu "
			       "length %llu\n",
			       (unsigned long long)
			       btrfs_dev_extent_chunk_tree(l, dev_extent),
			       (unsigned long long)
			       btrfs_dev_extent_chunk_objectid(l, dev_extent),
			       (unsigned long long)
			       btrfs_dev_extent_chunk_offset(l, dev_extent),
			       (unsigned long long)
			       btrfs_dev_extent_length(l, dev_extent));
			break;
		case BTRFS_STRING_ITEM_KEY:
			/* dirty, but it's simple */
			str = l->data + btrfs_item_ptr_offset(l, i);
			printf("\t\titem data %.*s\n", btrfs_item_size(l, item), str);
			break;
		};
		fflush(stdout);
	}
}

void btrfs_print_tree(struct btrfs_root *root, struct extent_buffer *eb)
{
	int i;
	u32 nr;
	u32 size;
	struct btrfs_disk_key disk_key;
	struct btrfs_key key;

	if (!eb)
		return;
	nr = btrfs_header_nritems(eb);
	if (btrfs_is_leaf(eb)) {
		btrfs_print_leaf(root, eb);
		return;
	}
	printf("node %llu level %d items %d free %u generation %llu owner %llu\n",
	       (unsigned long long)eb->start,
	        btrfs_header_level(eb), nr,
		(u32)BTRFS_NODEPTRS_PER_BLOCK(root) - nr,
		(unsigned long long)btrfs_header_generation(eb),
		(unsigned long long)btrfs_header_owner(eb));
	print_uuids(eb);
	fflush(stdout);
	size = btrfs_level_size(root, btrfs_header_level(eb) - 1);
	for (i = 0; i < nr; i++) {
		u64 blocknr = btrfs_node_blockptr(eb, i);
		btrfs_node_key(eb, &disk_key, i);
		btrfs_disk_key_to_cpu(&key, &disk_key);
		printf("\t");
		btrfs_print_key(&disk_key);
		printf(" block %llu (%llu) gen %llu\n",
		       (unsigned long long)blocknr,
		       (unsigned long long)blocknr / size,
		       (unsigned long long)btrfs_node_ptr_generation(eb, i));
		fflush(stdout);
	}
	for (i = 0; i < nr; i++) {
		struct extent_buffer *next = read_tree_block(root,
					     btrfs_node_blockptr(eb, i),
					     size,
					     btrfs_node_ptr_generation(eb, i));
		if (!next) {
			fprintf(stderr, "failed to read %llu in tree %llu\n",
				(unsigned long long)btrfs_node_blockptr(eb, i),
				(unsigned long long)btrfs_header_owner(eb));
			continue;
		}
		if (btrfs_is_leaf(next) &&
		    btrfs_header_level(eb) != 1)
			BUG();
		if (btrfs_header_level(next) !=
			btrfs_header_level(eb) - 1)
			BUG();
		btrfs_print_tree(root, next);
		free_extent_buffer(next);
	}
}

