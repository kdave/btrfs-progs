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


static void print_dir_item_type(struct extent_buffer *eb,
                                struct btrfs_dir_item *di)
{
	u8 type = btrfs_dir_type(eb, di);

	switch (type) {
	case BTRFS_FT_REG_FILE:
		printf("FILE");
		break;
	case BTRFS_FT_DIR:
		printf("DIR");
		break;
	case BTRFS_FT_CHRDEV:
		printf("CHRDEV");
		break;
	case BTRFS_FT_BLKDEV:
		printf("BLKDEV");
		break;
	case BTRFS_FT_FIFO:
		printf("FIFO");
		break;
	case BTRFS_FT_SOCK:
		printf("SOCK");
		break;
	case BTRFS_FT_SYMLINK:
		printf("SYMLINK");
		break;
	case BTRFS_FT_XATTR:
		printf("XATTR");
		break;
	default:
		printf("%u", type);
	}
}

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
		printf(" type ");
		print_dir_item_type(eb, di);
		printf("\n");
		name_len = btrfs_dir_name_len(eb, di);
		data_len = btrfs_dir_data_len(eb, di);
		len = (name_len <= sizeof(namebuf))? name_len: sizeof(namebuf);
		read_extent_buffer(eb, namebuf, (unsigned long)(di + 1), len);
		printf("\t\tnamelen %u datalen %u name: %.*s\n",
		       name_len, data_len, len, namebuf);
		if (data_len) {
			len = (data_len <= sizeof(namebuf))? data_len: sizeof(namebuf);
			read_extent_buffer(eb, namebuf,
				(unsigned long)(di + 1) + name_len, len);
			printf("\t\tdata %.*s\n", len, namebuf);
		}
		len = sizeof(*di) + name_len + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}
	return 0;
}

static int print_inode_extref_item(struct extent_buffer *eb,
				   struct btrfs_item *item,
				   struct btrfs_inode_extref *extref)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len = 0;
	u64 index = 0;
	u64 parent_objid;
	char namebuf[BTRFS_NAME_LEN];

	total = btrfs_item_size(eb, item);

	while (cur < total) {
		index = btrfs_inode_extref_index(eb, extref);
		name_len = btrfs_inode_extref_name_len(eb, extref);
		parent_objid = btrfs_inode_extref_parent(eb, extref);

		len = (name_len <= sizeof(namebuf))? name_len: sizeof(namebuf);

		read_extent_buffer(eb, namebuf, (unsigned long)(extref->name), len);

		printf("\t\tinode extref index %llu parent %llu namelen %u "
		       "name: %.*s\n",
		       (unsigned long long)index,
		       (unsigned long long)parent_objid,
		       name_len, len, namebuf);

		len = sizeof(*extref) + name_len;
		extref = (struct btrfs_inode_extref *)((char *)extref + len);
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

static void print_extent_item(struct extent_buffer *eb, int slot, int metadata)
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

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK && !metadata) {
		struct btrfs_tree_block_info *info;
		info = (struct btrfs_tree_block_info *)(ei + 1);
		btrfs_tree_block_key(eb, info, &key);
		printf("\t\ttree block ");
		btrfs_print_key(&key);
		printf(" level %d\n", btrfs_tree_block_level(eb, info));
		iref = (struct btrfs_extent_inline_ref *)(info + 1);
	} else if (metadata) {
		struct btrfs_key tmp;

		btrfs_item_key_to_cpu(eb, &tmp, slot);
		printf("\t\ttree block skinny level %d\n", (int)tmp.offset);
		iref = (struct btrfs_extent_inline_ref *)(ei + 1);
	} else{
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
			return;
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

static int count_bytes(void *buf, int len, char b)
{
	int cnt = 0;
	int i;
	for (i = 0; i < len; i++) {
		if (((char*)buf)[i] == b)
			cnt++;
	}
	return cnt;
}

static void print_root(struct extent_buffer *leaf, int slot)
{
	struct btrfs_root_item *ri;
	struct btrfs_root_item root_item;
	int len;
	char uuid_str[128];

	ri = btrfs_item_ptr(leaf, slot, struct btrfs_root_item);
	len = btrfs_item_size_nr(leaf, slot);

	memset(&root_item, 0, sizeof(root_item));
	read_extent_buffer(leaf, &root_item, (unsigned long)ri, len);

	printf("\t\troot data bytenr %llu level %d dirid %llu refs %u gen %llu\n",
		(unsigned long long)btrfs_root_bytenr(&root_item),
		btrfs_root_level(&root_item),
		(unsigned long long)btrfs_root_dirid(&root_item),
		btrfs_root_refs(&root_item),
		(unsigned long long)btrfs_root_generation(&root_item));

	if (root_item.generation == root_item.generation_v2) {
		uuid_unparse(root_item.uuid, uuid_str);
		printf("\t\tuuid %s\n", uuid_str);
		if (count_bytes(root_item.parent_uuid, BTRFS_UUID_SIZE, 0) != BTRFS_UUID_SIZE) {
			uuid_unparse(root_item.parent_uuid, uuid_str);
			printf("\t\tparent_uuid %s\n", uuid_str);
		}
		if (count_bytes(root_item.received_uuid, BTRFS_UUID_SIZE, 0) != BTRFS_UUID_SIZE) {
			uuid_unparse(root_item.received_uuid, uuid_str);
			printf("\t\treceived_uuid %s\n", uuid_str);
		}
		if (root_item.ctransid) {
			printf("\t\tctransid %llu otransid %llu stransid %llu rtransid %llu\n",
				btrfs_root_ctransid(&root_item),
				btrfs_root_otransid(&root_item),
				btrfs_root_stransid(&root_item),
				btrfs_root_rtransid(&root_item));
		}
	}
	if (btrfs_root_refs(&root_item) == 0) {
		struct btrfs_key drop_key;
		btrfs_disk_key_to_cpu(&drop_key,
				      &root_item.drop_progress);
		printf("\t\tdrop ");
		btrfs_print_key(&root_item.drop_progress);
		printf(" level %d\n", root_item.drop_level);
	}
}

static void print_free_space_header(struct extent_buffer *leaf, int slot)
{
	struct btrfs_free_space_header *header;
	struct btrfs_disk_key location;

	header = btrfs_item_ptr(leaf, slot, struct btrfs_free_space_header);
	btrfs_free_space_key(leaf, header, &location);
	printf("\t\tlocation ");
	btrfs_print_key(&location);
	printf("\n");
	printf("\t\tcache generation %llu entries %llu bitmaps %llu\n",
	       (unsigned long long)btrfs_free_space_generation(leaf, header),
	       (unsigned long long)btrfs_free_space_entries(leaf, header),
	       (unsigned long long)btrfs_free_space_bitmaps(leaf, header));
}

static void print_key_type(u64 objectid, u8 type)
{
	if (type == 0 && objectid == BTRFS_FREE_SPACE_OBJECTID) {
		printf("UNTYPED");
		return;
	}

	switch (type) {
	case BTRFS_INODE_ITEM_KEY:
		printf("INODE_ITEM");
		break;
	case BTRFS_INODE_REF_KEY:
		printf("INODE_REF");
		break;
	case BTRFS_INODE_EXTREF_KEY:
		printf("INODE_EXTREF");
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
	case BTRFS_METADATA_ITEM_KEY:
		printf("METADATA_ITEM");
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
	case BTRFS_BALANCE_ITEM_KEY:
		printf("BALANCE_ITEM");
		break;
	case BTRFS_DEV_REPLACE_KEY:
		printf("DEV_REPLACE_ITEM");
		break;
	case BTRFS_STRING_ITEM_KEY:
		printf("STRING_ITEM");
		break;
	case BTRFS_QGROUP_STATUS_KEY:
		printf("BTRFS_STATUS_KEY");
		break;
	case BTRFS_QGROUP_RELATION_KEY:
		printf("BTRFS_QGROUP_RELATION_KEY");
		break;
	case BTRFS_QGROUP_INFO_KEY:
		printf("BTRFS_QGROUP_INFO_KEY");
		break;
	case BTRFS_QGROUP_LIMIT_KEY:
		printf("BTRFS_QGROUP_LIMIT_KEY");
		break;
	case BTRFS_DEV_STATS_KEY:
		printf("DEV_STATS_ITEM");
		break;
	default:
		printf("UNKNOWN.%d", type);
	};
}

static void print_objectid(u64 objectid, u8 type)
{
	if (type == BTRFS_DEV_EXTENT_KEY) {
		printf("%llu", (unsigned long long)objectid); /* device id */
		return;
	}
	switch (type) {
	case BTRFS_QGROUP_RELATION_KEY:
		printf("%llu/%llu", objectid >> 48,
			objectid & ((1ll << 48) - 1));
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
	case BTRFS_BALANCE_OBJECTID:
		printf("BALANCE");
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
	case BTRFS_FREE_SPACE_OBJECTID:
		printf("FREE_SPACE");
		break;
	case BTRFS_FREE_INO_OBJECTID:
		printf("FREE_INO");
		break;
	case BTRFS_QUOTA_TREE_OBJECTID:
		printf("QUOTA_TREE");
		break;
	case BTRFS_MULTIPLE_OBJECTIDS:
		printf("MULTIPLE");
		break;
	case (u64)-1:
		printf("-1");
		break;
	case BTRFS_FIRST_CHUNK_TREE_OBJECTID:
		if (type == BTRFS_CHUNK_ITEM_KEY) {
			printf("FIRST_CHUNK_TREE");
			break;
		}
		/* fall-thru */
	default:
		printf("%llu", (unsigned long long)objectid);
	}
}

void btrfs_print_key(struct btrfs_disk_key *disk_key)
{
	u64 objectid = btrfs_disk_key_objectid(disk_key);
	u8 type = btrfs_disk_key_type(disk_key);
	u64 offset = btrfs_disk_key_offset(disk_key);

	printf("key (");
	print_objectid(objectid, type);
	printf(" ");
	print_key_type(objectid, type);
	switch (type) {
	case BTRFS_QGROUP_RELATION_KEY:
	case BTRFS_QGROUP_INFO_KEY:
	case BTRFS_QGROUP_LIMIT_KEY:
		printf(" %llu/%llu)", (unsigned long long)(offset >> 48),
			(unsigned long long)(offset & ((1ll << 48) - 1)));
		break;
	default:
		if (offset == (u64)-1)
			printf(" -1)");
		else
			printf(" %llu)", (unsigned long long)offset);
		break;
	}
}

void btrfs_print_leaf(struct btrfs_root *root, struct extent_buffer *l)
{
	int i;
	char *str;
	struct btrfs_item *item;
	struct btrfs_dir_item *di;
	struct btrfs_inode_item *ii;
	struct btrfs_file_extent_item *fi;
	struct btrfs_block_group_item *bi;
	struct btrfs_extent_data_ref *dref;
	struct btrfs_shared_data_ref *sref;
	struct btrfs_inode_ref *iref;
	struct btrfs_inode_extref *iref2;
	struct btrfs_dev_extent *dev_extent;
	struct btrfs_disk_key disk_key;
	struct btrfs_block_group_item bg_item;
	struct btrfs_dir_log_item *dlog;
	struct btrfs_qgroup_info_item *qg_info;
	struct btrfs_qgroup_limit_item *qg_limit;
	struct btrfs_qgroup_status_item *qg_status;
	u32 nr = btrfs_header_nritems(l);
	u64 objectid;
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
		objectid = btrfs_disk_key_objectid(&disk_key);
		type = btrfs_disk_key_type(&disk_key);
		printf("\titem %d ", i);
		btrfs_print_key(&disk_key);
		printf(" itemoff %d itemsize %d\n",
			btrfs_item_offset(l, item),
			btrfs_item_size(l, item));

		if (type == 0 && objectid == BTRFS_FREE_SPACE_OBJECTID)
			print_free_space_header(l, i);

		switch (type) {
		case BTRFS_INODE_ITEM_KEY:
			ii = btrfs_item_ptr(l, i, struct btrfs_inode_item);
			printf("\t\tinode generation %llu transid %llu size %llu block group %llu mode %o links %u\n",
			       (unsigned long long)btrfs_inode_generation(l, ii),
			       (unsigned long long)btrfs_inode_transid(l, ii),
			       (unsigned long long)btrfs_inode_size(l, ii),
			       (unsigned long long)btrfs_inode_block_group(l,ii),
			       btrfs_inode_mode(l, ii),
			       btrfs_inode_nlink(l, ii));
			break;
		case BTRFS_INODE_REF_KEY:
			iref = btrfs_item_ptr(l, i, struct btrfs_inode_ref);
			print_inode_ref_item(l, item, iref);
			break;
		case BTRFS_INODE_EXTREF_KEY:
			iref2 = btrfs_item_ptr(l, i, struct btrfs_inode_extref);
			print_inode_extref_item(l, item, iref2);
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
			       (unsigned long long)btrfs_dir_log_end(l, dlog));
		       break;
		case BTRFS_ORPHAN_ITEM_KEY:
			printf("\t\torphan item\n");
			break;
		case BTRFS_ROOT_ITEM_KEY:
			print_root(l, i);
			break;
		case BTRFS_ROOT_REF_KEY:
			print_root_ref(l, i, "ref");
			break;
		case BTRFS_ROOT_BACKREF_KEY:
			print_root_ref(l, i, "backref");
			break;
		case BTRFS_EXTENT_ITEM_KEY:
			print_extent_item(l, i, 0);
			break;
		case BTRFS_METADATA_ITEM_KEY:
			print_extent_item(l, i, 1);
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
			printf("\t\tcsum item\n");
			break;
		case BTRFS_EXTENT_CSUM_KEY:
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
		case BTRFS_QGROUP_STATUS_KEY:
			qg_status = btrfs_item_ptr(l, i,
					struct btrfs_qgroup_status_item);
			printf("\t\tversion %llu generation %llu flags %#llx "
				"scan %lld\n",
				(unsigned long long)
				btrfs_qgroup_status_version(l, qg_status),
				(unsigned long long)
				btrfs_qgroup_status_generation(l, qg_status),
				(unsigned long long)
				btrfs_qgroup_status_flags(l, qg_status),
				(unsigned long long)
				btrfs_qgroup_status_scan(l, qg_status));
			break;
		case BTRFS_QGROUP_RELATION_KEY:
			break;
		case BTRFS_QGROUP_INFO_KEY:
			qg_info = btrfs_item_ptr(l, i,
						 struct btrfs_qgroup_info_item);
			printf("\t\tgeneration %llu\n"
			     "\t\treferenced %lld referenced compressed %lld\n"
			     "\t\texclusive %lld exclusive compressed %lld\n",
			       (unsigned long long)
			       btrfs_qgroup_info_generation(l, qg_info),
			       (long long)
			       btrfs_qgroup_info_referenced(l, qg_info),
			       (long long)
			       btrfs_qgroup_info_referenced_compressed(l,
								       qg_info),
			       (long long)
			       btrfs_qgroup_info_exclusive(l, qg_info),
			       (long long)
			       btrfs_qgroup_info_exclusive_compressed(l,
								      qg_info));
			break;
		case BTRFS_QGROUP_LIMIT_KEY:
			qg_limit = btrfs_item_ptr(l, i,
					 struct btrfs_qgroup_limit_item);
			printf("\t\tflags %llx\n"
			     "\t\tmax referenced %lld max exclusive %lld\n"
			     "\t\trsv referenced %lld rsv exclusive %lld\n",
			       (unsigned long long)
			       btrfs_qgroup_limit_flags(l, qg_limit),
			       (long long)
			       btrfs_qgroup_limit_max_referenced(l, qg_limit),
			       (long long)
			       btrfs_qgroup_limit_max_exclusive(l, qg_limit),
			       (long long)
			       btrfs_qgroup_limit_rsv_referenced(l, qg_limit),
			       (long long)
			       btrfs_qgroup_limit_rsv_exclusive(l, qg_limit));
			break;
		case BTRFS_STRING_ITEM_KEY:
			/* dirty, but it's simple */
			str = l->data + btrfs_item_ptr_offset(l, i);
			printf("\t\titem data %.*s\n", btrfs_item_size(l, item), str);
			break;
		case BTRFS_DEV_STATS_KEY:
			printf("\t\tdevice stats\n");
			break;
		};
		fflush(stdout);
	}
}

void btrfs_print_tree(struct btrfs_root *root, struct extent_buffer *eb, int follow)
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
	if (!follow)
		return;

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
		btrfs_print_tree(root, next, 1);
		free_extent_buffer(next);
	}
}
