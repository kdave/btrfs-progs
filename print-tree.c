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
#include "utils.h"


static void print_dir_item_type(struct extent_buffer *eb,
                                struct btrfs_dir_item *di)
{
	u8 type = btrfs_dir_type(eb, di);
	static const char* dir_item_str[] = {
		[BTRFS_FT_REG_FILE]	= "FILE",
		[BTRFS_FT_DIR] 		= "DIR",
		[BTRFS_FT_CHRDEV]	= "CHRDEV",
		[BTRFS_FT_BLKDEV]	= "BLKDEV",
		[BTRFS_FT_FIFO]		= "FIFO",
		[BTRFS_FT_SOCK]		= "SOCK",
		[BTRFS_FT_SYMLINK]	= "SYMLINK",
		[BTRFS_FT_XATTR]	= "XATTR"
	};

	if (type < ARRAY_SIZE(dir_item_str) && dir_item_str[type])
		printf("%s", dir_item_str[type]);
	else
		printf("DIR_ITEM.%u", type);
}

static void print_dir_item(struct extent_buffer *eb, u32 size,
			  struct btrfs_dir_item *di)
{
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u32 data_len;
	char namebuf[BTRFS_NAME_LEN];
	struct btrfs_disk_key location;

	while (cur < size) {
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
		printf("\t\ttransid %llu data_len %u name_len %u\n",
				btrfs_dir_transid(eb, di),
				data_len, name_len);
		printf("\t\tname: %.*s\n", len, namebuf);
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
}

static void print_inode_extref_item(struct extent_buffer *eb, u32 size,
		struct btrfs_inode_extref *extref)
{
	u32 cur = 0;
	u32 len;
	u32 name_len = 0;
	u64 index = 0;
	u64 parent_objid;
	char namebuf[BTRFS_NAME_LEN];

	while (cur < size) {
		index = btrfs_inode_extref_index(eb, extref);
		name_len = btrfs_inode_extref_name_len(eb, extref);
		parent_objid = btrfs_inode_extref_parent(eb, extref);

		len = (name_len <= sizeof(namebuf))? name_len: sizeof(namebuf);

		read_extent_buffer(eb, namebuf, (unsigned long)(extref->name), len);

		printf("\t\tindex %llu parent %llu namelen %u name: %.*s\n",
		       (unsigned long long)index,
		       (unsigned long long)parent_objid,
		       name_len, len, namebuf);

		len = sizeof(*extref) + name_len;
		extref = (struct btrfs_inode_extref *)((char *)extref + len);
		cur += len;
	}
}

static void print_inode_ref_item(struct extent_buffer *eb, u32 size,
				struct btrfs_inode_ref *ref)
{
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	char namebuf[BTRFS_NAME_LEN];

	while (cur < size) {
		name_len = btrfs_inode_ref_name_len(eb, ref);
		index = btrfs_inode_ref_index(eb, ref);
		len = (name_len <= sizeof(namebuf))? name_len: sizeof(namebuf);
		read_extent_buffer(eb, namebuf, (unsigned long)(ref + 1), len);
		printf("\t\tindex %llu namelen %u name: %.*s\n",
		       (unsigned long long)index, name_len, len, namebuf);
		len = sizeof(*ref) + name_len;
		ref = (struct btrfs_inode_ref *)((char *)ref + len);
		cur += len;
	}
}

/* Caller should ensure sizeof(*ret)>=21 "DATA|METADATA|RAID10" */
static void bg_flags_to_str(u64 flags, char *ret)
{
	int empty = 1;

	if (flags & BTRFS_BLOCK_GROUP_DATA) {
		empty = 0;
		strcpy(ret, "DATA");
	}
	if (flags & BTRFS_BLOCK_GROUP_METADATA) {
		if (!empty)
			strcat(ret, "|");
		strcat(ret, "METADATA");
	}
	if (flags & BTRFS_BLOCK_GROUP_SYSTEM) {
		if (!empty)
			strcat(ret, "|");
		strcat(ret, "SYSTEM");
	}
	switch (flags & BTRFS_BLOCK_GROUP_PROFILE_MASK) {
	case BTRFS_BLOCK_GROUP_RAID0:
		strcat(ret, "|RAID0");
		break;
	case BTRFS_BLOCK_GROUP_RAID1:
		strcat(ret, "|RAID1");
		break;
	case BTRFS_BLOCK_GROUP_DUP:
		strcat(ret, "|DUP");
		break;
	case BTRFS_BLOCK_GROUP_RAID10:
		strcat(ret, "|RAID10");
		break;
	case BTRFS_BLOCK_GROUP_RAID5:
		strcat(ret, "|RAID5");
		break;
	case BTRFS_BLOCK_GROUP_RAID6:
		strcat(ret, "|RAID6");
		break;
	default:
		break;
	}
}

/* Caller should ensure sizeof(*ret)>= 26 "OFF|SCANNING|INCONSISTENT" */
static void qgroup_flags_to_str(u64 flags, char *ret)
{
	if (flags & BTRFS_QGROUP_STATUS_FLAG_ON)
		strcpy(ret, "ON");
	else
		strcpy(ret, "OFF");

	if (flags & BTRFS_QGROUP_STATUS_FLAG_RESCAN)
		strcat(ret, "|SCANNING");
	if (flags & BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT)
		strcat(ret, "|INCONSISTENT");
}

void print_chunk_item(struct extent_buffer *eb, struct btrfs_chunk *chunk)
{
	u16 num_stripes = btrfs_chunk_num_stripes(eb, chunk);
	int i;
	u32 chunk_item_size;
	char chunk_flags_str[32] = {0};

	/* The chunk must contain at least one stripe */
	if (num_stripes < 1) {
		printf("invalid num_stripes: %u\n", num_stripes);
		return;
	}

	chunk_item_size = btrfs_chunk_item_size(num_stripes);

	if ((unsigned long)chunk + chunk_item_size > eb->len) {
		printf("\t\tchunk item invalid\n");
		return;
	}

	bg_flags_to_str(btrfs_chunk_type(eb, chunk), chunk_flags_str);
	printf("\t\tlength %llu owner %llu stripe_len %llu type %s\n",
	       (unsigned long long)btrfs_chunk_length(eb, chunk),
	       (unsigned long long)btrfs_chunk_owner(eb, chunk),
	       (unsigned long long)btrfs_chunk_stripe_len(eb, chunk),
		chunk_flags_str);
	printf("\t\tio_align %u io_width %u sector_size %u\n",
			btrfs_chunk_io_align(eb, chunk),
			btrfs_chunk_io_width(eb, chunk),
			btrfs_chunk_sector_size(eb, chunk));
	printf("\t\tnum_stripes %hu sub_stripes %hu\n", num_stripes,
			btrfs_chunk_sub_stripes(eb, chunk));
	for (i = 0 ; i < num_stripes ; i++) {
		unsigned char dev_uuid[BTRFS_UUID_SIZE];
		char str_dev_uuid[BTRFS_UUID_UNPARSED_SIZE];
		u64 uuid_offset;
		u64 stripe_offset;

		uuid_offset = (unsigned long)btrfs_stripe_dev_uuid_nr(chunk, i);
		stripe_offset = (unsigned long)btrfs_stripe_nr(chunk, i);

		if (uuid_offset < stripe_offset ||
			(uuid_offset + BTRFS_UUID_SIZE) >
				(stripe_offset + sizeof(struct btrfs_stripe))) {
			printf("\t\t\tstripe %d invalid\n", i);
			break;
		}

		read_extent_buffer(eb, dev_uuid,
			uuid_offset,
			BTRFS_UUID_SIZE);
		uuid_unparse(dev_uuid, str_dev_uuid);
		printf("\t\t\tstripe %d devid %llu offset %llu\n", i,
		      (unsigned long long)btrfs_stripe_devid_nr(eb, chunk, i),
		      (unsigned long long)btrfs_stripe_offset_nr(eb, chunk, i));
		printf("\t\t\tdev_uuid %s\n", str_dev_uuid);
	}
}

static void print_dev_item(struct extent_buffer *eb,
			   struct btrfs_dev_item *dev_item)
{
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];
	char fsid_str[BTRFS_UUID_UNPARSED_SIZE];
	u8 uuid[BTRFS_UUID_SIZE];
	u8 fsid[BTRFS_UUID_SIZE];

	read_extent_buffer(eb, uuid,
			   (unsigned long)btrfs_device_uuid(dev_item),
			   BTRFS_UUID_SIZE);
	uuid_unparse(uuid, uuid_str);
	read_extent_buffer(eb, fsid,
			   (unsigned long)btrfs_device_fsid(dev_item),
			   BTRFS_UUID_SIZE);
	uuid_unparse(fsid, fsid_str);
	printf("\t\tdevid %llu total_bytes %llu bytes_used %Lu\n"
	       "\t\tio_align %u io_width %u sector_size %u type %llu\n"
	       "\t\tgeneration %llu start_offset %llu dev_group %u\n"
	       "\t\tseek_speed %hhu bandwidth %hhu\n"
	       "\t\tuuid %s\n"
	       "\t\tfsid %s\n",
	       (unsigned long long)btrfs_device_id(eb, dev_item),
	       (unsigned long long)btrfs_device_total_bytes(eb, dev_item),
	       (unsigned long long)btrfs_device_bytes_used(eb, dev_item),
	       btrfs_device_io_align(eb, dev_item),
	       btrfs_device_io_width(eb, dev_item),
	       btrfs_device_sector_size(eb, dev_item),
	       (unsigned long long)btrfs_device_type(eb, dev_item),
	       (unsigned long long)btrfs_device_generation(eb, dev_item),
	       (unsigned long long)btrfs_device_start_offset(eb, dev_item),
	       btrfs_device_group(eb, dev_item),
	       btrfs_device_seek_speed(eb, dev_item),
	       btrfs_device_bandwidth(eb, dev_item),
	       uuid_str, fsid_str);
}

static void print_uuids(struct extent_buffer *eb)
{
	char fs_uuid[BTRFS_UUID_UNPARSED_SIZE];
	char chunk_uuid[BTRFS_UUID_UNPARSED_SIZE];
	u8 disk_uuid[BTRFS_UUID_SIZE];

	read_extent_buffer(eb, disk_uuid, btrfs_header_fsid(),
			   BTRFS_FSID_SIZE);

	fs_uuid[BTRFS_UUID_UNPARSED_SIZE - 1] = '\0';
	uuid_unparse(disk_uuid, fs_uuid);

	read_extent_buffer(eb, disk_uuid,
			   btrfs_header_chunk_tree_uuid(eb),
			   BTRFS_UUID_SIZE);

	chunk_uuid[BTRFS_UUID_UNPARSED_SIZE - 1] = '\0';
	uuid_unparse(disk_uuid, chunk_uuid);
	printf("fs uuid %s\nchunk uuid %s\n", fs_uuid, chunk_uuid);
}

static void compress_type_to_str(u8 compress_type, char *ret)
{
	switch (compress_type) {
	case BTRFS_COMPRESS_NONE:
		strcpy(ret, "none");
		break;
	case BTRFS_COMPRESS_ZLIB:
		strcpy(ret, "zlib");
		break;
	case BTRFS_COMPRESS_LZO:
		strcpy(ret, "lzo");
		break;
	case BTRFS_COMPRESS_ZSTD:
		strcpy(ret, "zstd");
		break;
	default:
		sprintf(ret, "UNKNOWN.%d", compress_type);
	}
}

static const char* file_extent_type_to_str(u8 type)
{
	switch (type) {
	case BTRFS_FILE_EXTENT_INLINE: return "inline";
	case BTRFS_FILE_EXTENT_PREALLOC: return "prealloc";
	case BTRFS_FILE_EXTENT_REG: return "regular";
	default: return "unknown";
	}
}

static void print_file_extent_item(struct extent_buffer *eb,
				   struct btrfs_item *item,
				   int slot,
				   struct btrfs_file_extent_item *fi)
{
	unsigned char extent_type = btrfs_file_extent_type(eb, fi);
	char compress_str[16];

	compress_type_to_str(btrfs_file_extent_compression(eb, fi),
			     compress_str);

	printf("\t\tgeneration %llu type %hhu (%s)\n",
			btrfs_file_extent_generation(eb, fi),
			extent_type, file_extent_type_to_str(extent_type));

	if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
		printf("\t\tinline extent data size %u ram_bytes %u compression %hhu (%s)\n",
				btrfs_file_extent_inline_item_len(eb, item),
				btrfs_file_extent_inline_len(eb, slot, fi),
				btrfs_file_extent_compression(eb, fi),
				compress_str);
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
	printf("\t\textent compression %hhu (%s)\n",
			btrfs_file_extent_compression(eb, fi),
			compress_str);
}

/* Caller should ensure sizeof(*ret) >= 16("DATA|TREE_BLOCK") */
static void extent_flags_to_str(u64 flags, char *ret)
{
	int empty = 1;

	if (flags & BTRFS_EXTENT_FLAG_DATA) {
		empty = 0;
		strcpy(ret, "DATA");
	}
	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		if (!empty) {
			empty = 0;
			strcat(ret, "|");
		}
		strcat(ret, "TREE_BLOCK");
	}
	if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
		strcat(ret, "|");
		strcat(ret, "FULL_BACKREF");
	}
}

void print_extent_item(struct extent_buffer *eb, int slot, int metadata)
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
	char flags_str[32] = {0};

	if (item_size < sizeof(*ei)) {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
		struct btrfs_extent_item_v0 *ei0;
		BUG_ON(item_size != sizeof(*ei0));
		ei0 = btrfs_item_ptr(eb, slot, struct btrfs_extent_item_v0);
		printf("\t\trefs %u\n",
		       btrfs_extent_refs_v0(eb, ei0));
		return;
#else
		BUG();
#endif
	}

	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
	flags = btrfs_extent_flags(eb, ei);
	extent_flags_to_str(flags, flags_str);

	printf("\t\trefs %llu gen %llu flags %s\n",
	       (unsigned long long)btrfs_extent_refs(eb, ei),
	       (unsigned long long)btrfs_extent_generation(eb, ei),
	       flags_str);

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

static void print_root_ref(struct extent_buffer *leaf, int slot, const char *tag)
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

static int empty_uuid(const u8 *uuid)
{
	int i;

	for (i = 0; i < BTRFS_UUID_SIZE; i++)
		if (uuid[i])
			return 0;
	return 1;
}

/*
 * Caller must ensure sizeof(*ret) >= 7 "RDONLY"
 */
static void root_flags_to_str(u64 flags, char *ret)
{
	if (flags & BTRFS_ROOT_SUBVOL_RDONLY)
		strcat(ret, "RDONLY");
	else
		strcat(ret, "none");
}

static void print_timespec(struct extent_buffer *eb,
		struct btrfs_timespec *timespec, const char *prefix,
		const char *suffix)
{
	struct tm tm;
	u64 tmp_u64;
	u32 tmp_u32;
	time_t tmp_time;
	char timestamp[256];

	tmp_u64 = btrfs_timespec_sec(eb, timespec);
	tmp_u32 = btrfs_timespec_nsec(eb, timespec);
	tmp_time = tmp_u64;
	localtime_r(&tmp_time, &tm);
	strftime(timestamp, sizeof(timestamp),
			"%Y-%m-%d %H:%M:%S", &tm);
	printf("%s%llu.%u (%s)%s", prefix, (unsigned long long)tmp_u64, tmp_u32,
			timestamp, suffix);
}

static void print_root_item(struct extent_buffer *leaf, int slot)
{
	struct btrfs_root_item *ri;
	struct btrfs_root_item root_item;
	int len;
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];
	char flags_str[32] = {0};
	struct btrfs_key drop_key;

	ri = btrfs_item_ptr(leaf, slot, struct btrfs_root_item);
	len = btrfs_item_size_nr(leaf, slot);

	memset(&root_item, 0, sizeof(root_item));
	read_extent_buffer(leaf, &root_item, (unsigned long)ri, len);
	root_flags_to_str(btrfs_root_flags(&root_item), flags_str);

	printf("\t\tgeneration %llu root_dirid %llu bytenr %llu level %hhu refs %u\n",
		(unsigned long long)btrfs_root_generation(&root_item),
		(unsigned long long)btrfs_root_dirid(&root_item),
		(unsigned long long)btrfs_root_bytenr(&root_item),
		btrfs_root_level(&root_item),
		btrfs_root_refs(&root_item));
	printf("\t\tlastsnap %llu byte_limit %llu bytes_used %llu flags 0x%llx(%s)\n",
		(unsigned long long)btrfs_root_last_snapshot(&root_item),
		(unsigned long long)btrfs_root_limit(&root_item),
		(unsigned long long)btrfs_root_used(&root_item),
		(unsigned long long)btrfs_root_flags(&root_item),
		flags_str);

	if (root_item.generation == root_item.generation_v2) {
		uuid_unparse(root_item.uuid, uuid_str);
		printf("\t\tuuid %s\n", uuid_str);
		if (!empty_uuid(root_item.parent_uuid)) {
			uuid_unparse(root_item.parent_uuid, uuid_str);
			printf("\t\tparent_uuid %s\n", uuid_str);
		}
		if (!empty_uuid(root_item.received_uuid)) {
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
		if (btrfs_timespec_sec(leaf, btrfs_root_ctime(ri)))
			print_timespec(leaf, btrfs_root_ctime(ri),
					"\t\tctime ", "\n");
		if (btrfs_timespec_sec(leaf, btrfs_root_otime(ri)))
			print_timespec(leaf, btrfs_root_otime(ri),
					"\t\totime ", "\n");
		if (btrfs_timespec_sec(leaf, btrfs_root_stime(ri)))
			print_timespec(leaf, btrfs_root_stime(ri),
					"\t\tstime ", "\n");
		if (btrfs_timespec_sec(leaf, btrfs_root_rtime(ri)))
			print_timespec(leaf, btrfs_root_rtime(ri),
					"\t\trtime ", "\n");
	}

	btrfs_disk_key_to_cpu(&drop_key, &root_item.drop_progress);
	printf("\t\tdrop ");
	btrfs_print_key(&root_item.drop_progress);
	printf(" level %hhu\n", root_item.drop_level);
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

void print_key_type(FILE *stream, u64 objectid, u8 type)
{
	static const char* key_to_str[256] = {
		[BTRFS_INODE_ITEM_KEY]		= "INODE_ITEM",
		[BTRFS_INODE_REF_KEY]		= "INODE_REF",
		[BTRFS_INODE_EXTREF_KEY]	= "INODE_EXTREF",
		[BTRFS_DIR_ITEM_KEY]		= "DIR_ITEM",
		[BTRFS_DIR_INDEX_KEY]		= "DIR_INDEX",
		[BTRFS_DIR_LOG_ITEM_KEY]	= "DIR_LOG_ITEM",
		[BTRFS_DIR_LOG_INDEX_KEY]	= "DIR_LOG_INDEX",
		[BTRFS_XATTR_ITEM_KEY]		= "XATTR_ITEM",
		[BTRFS_ORPHAN_ITEM_KEY]		= "ORPHAN_ITEM",
		[BTRFS_ROOT_ITEM_KEY]		= "ROOT_ITEM",
		[BTRFS_ROOT_REF_KEY]		= "ROOT_REF",
		[BTRFS_ROOT_BACKREF_KEY]	= "ROOT_BACKREF",
		[BTRFS_EXTENT_ITEM_KEY]		= "EXTENT_ITEM",
		[BTRFS_METADATA_ITEM_KEY]	= "METADATA_ITEM",
		[BTRFS_TREE_BLOCK_REF_KEY]	= "TREE_BLOCK_REF",
		[BTRFS_SHARED_BLOCK_REF_KEY]	= "SHARED_BLOCK_REF",
		[BTRFS_EXTENT_DATA_REF_KEY]	= "EXTENT_DATA_REF",
		[BTRFS_SHARED_DATA_REF_KEY]	= "SHARED_DATA_REF",
		[BTRFS_EXTENT_REF_V0_KEY]	= "EXTENT_REF_V0",
		[BTRFS_CSUM_ITEM_KEY]		= "CSUM_ITEM",
		[BTRFS_EXTENT_CSUM_KEY]		= "EXTENT_CSUM",
		[BTRFS_EXTENT_DATA_KEY]		= "EXTENT_DATA",
		[BTRFS_BLOCK_GROUP_ITEM_KEY]	= "BLOCK_GROUP_ITEM",
		[BTRFS_FREE_SPACE_INFO_KEY]	= "FREE_SPACE_INFO",
		[BTRFS_FREE_SPACE_EXTENT_KEY]	= "FREE_SPACE_EXTENT",
		[BTRFS_FREE_SPACE_BITMAP_KEY]	= "FREE_SPACE_BITMAP",
		[BTRFS_CHUNK_ITEM_KEY]		= "CHUNK_ITEM",
		[BTRFS_DEV_ITEM_KEY]		= "DEV_ITEM",
		[BTRFS_DEV_EXTENT_KEY]		= "DEV_EXTENT",
		[BTRFS_TEMPORARY_ITEM_KEY]	= "TEMPORARY_ITEM",
		[BTRFS_DEV_REPLACE_KEY]		= "DEV_REPLACE",
		[BTRFS_STRING_ITEM_KEY]		= "STRING_ITEM",
		[BTRFS_QGROUP_STATUS_KEY]	= "QGROUP_STATUS",
		[BTRFS_QGROUP_RELATION_KEY]	= "QGROUP_RELATION",
		[BTRFS_QGROUP_INFO_KEY]		= "QGROUP_INFO",
		[BTRFS_QGROUP_LIMIT_KEY]	= "QGROUP_LIMIT",
		[BTRFS_PERSISTENT_ITEM_KEY]	= "PERSISTENT_ITEM",
		[BTRFS_UUID_KEY_SUBVOL]		= "UUID_KEY_SUBVOL",
		[BTRFS_UUID_KEY_RECEIVED_SUBVOL] = "UUID_KEY_RECEIVED_SUBVOL",
	};

	if (type == 0 && objectid == BTRFS_FREE_SPACE_OBJECTID) {
		fprintf(stream, "UNTYPED");
		return;
	}


	if (key_to_str[type])
		fputs(key_to_str[type], stream);
	else
		fprintf(stream, "UNKNOWN.%d", type);
}

void print_objectid(FILE *stream, u64 objectid, u8 type)
{
	switch (type) {
	case BTRFS_DEV_EXTENT_KEY:
		/* device id */
		fprintf(stream, "%llu", (unsigned long long)objectid);
		return;
	case BTRFS_QGROUP_RELATION_KEY:
		fprintf(stream, "%llu/%llu", btrfs_qgroup_level(objectid),
		       btrfs_qgroup_subvid(objectid));
		return;
	case BTRFS_UUID_KEY_SUBVOL:
	case BTRFS_UUID_KEY_RECEIVED_SUBVOL:
		fprintf(stream, "0x%016llx", (unsigned long long)objectid);
		return;
	}

	switch (objectid) {
	case BTRFS_ROOT_TREE_OBJECTID:
		if (type == BTRFS_DEV_ITEM_KEY)
			fprintf(stream, "DEV_ITEMS");
		else
			fprintf(stream, "ROOT_TREE");
		break;
	case BTRFS_EXTENT_TREE_OBJECTID:
		fprintf(stream, "EXTENT_TREE");
		break;
	case BTRFS_CHUNK_TREE_OBJECTID:
		fprintf(stream, "CHUNK_TREE");
		break;
	case BTRFS_DEV_TREE_OBJECTID:
		fprintf(stream, "DEV_TREE");
		break;
	case BTRFS_FS_TREE_OBJECTID:
		fprintf(stream, "FS_TREE");
		break;
	case BTRFS_ROOT_TREE_DIR_OBJECTID:
		fprintf(stream, "ROOT_TREE_DIR");
		break;
	case BTRFS_CSUM_TREE_OBJECTID:
		fprintf(stream, "CSUM_TREE");
		break;
	case BTRFS_BALANCE_OBJECTID:
		fprintf(stream, "BALANCE");
		break;
	case BTRFS_ORPHAN_OBJECTID:
		fprintf(stream, "ORPHAN");
		break;
	case BTRFS_TREE_LOG_OBJECTID:
		fprintf(stream, "TREE_LOG");
		break;
	case BTRFS_TREE_LOG_FIXUP_OBJECTID:
		fprintf(stream, "LOG_FIXUP");
		break;
	case BTRFS_TREE_RELOC_OBJECTID:
		fprintf(stream, "TREE_RELOC");
		break;
	case BTRFS_DATA_RELOC_TREE_OBJECTID:
		fprintf(stream, "DATA_RELOC_TREE");
		break;
	case BTRFS_EXTENT_CSUM_OBJECTID:
		fprintf(stream, "EXTENT_CSUM");
		break;
	case BTRFS_FREE_SPACE_OBJECTID:
		fprintf(stream, "FREE_SPACE");
		break;
	case BTRFS_FREE_INO_OBJECTID:
		fprintf(stream, "FREE_INO");
		break;
	case BTRFS_QUOTA_TREE_OBJECTID:
		fprintf(stream, "QUOTA_TREE");
		break;
	case BTRFS_UUID_TREE_OBJECTID:
		fprintf(stream, "UUID_TREE");
		break;
	case BTRFS_FREE_SPACE_TREE_OBJECTID:
		fprintf(stream, "FREE_SPACE_TREE");
		break;
	case BTRFS_MULTIPLE_OBJECTIDS:
		fprintf(stream, "MULTIPLE");
		break;
	case (u64)-1:
		fprintf(stream, "-1");
		break;
	case BTRFS_FIRST_CHUNK_TREE_OBJECTID:
		if (type == BTRFS_CHUNK_ITEM_KEY) {
			fprintf(stream, "FIRST_CHUNK_TREE");
			break;
		}
		/* fall-thru */
	default:
		fprintf(stream, "%llu", (unsigned long long)objectid);
	}
}

void btrfs_print_key(struct btrfs_disk_key *disk_key)
{
	u64 objectid = btrfs_disk_key_objectid(disk_key);
	u8 type = btrfs_disk_key_type(disk_key);
	u64 offset = btrfs_disk_key_offset(disk_key);

	printf("key (");
	print_objectid(stdout, objectid, type);
	printf(" ");
	print_key_type(stdout, objectid, type);
	switch (type) {
	case BTRFS_QGROUP_RELATION_KEY:
	case BTRFS_QGROUP_INFO_KEY:
	case BTRFS_QGROUP_LIMIT_KEY:
		printf(" %llu/%llu)", btrfs_qgroup_level(offset),
		       btrfs_qgroup_subvid(offset));
		break;
	case BTRFS_UUID_KEY_SUBVOL:
	case BTRFS_UUID_KEY_RECEIVED_SUBVOL:
		printf(" 0x%016llx)", (unsigned long long)offset);
		break;

	/*
	 * Key offsets of ROOT_ITEM point to tree root, print them in human
	 * readable format.  Especially useful for trees like data/tree reloc
	 * tree, whose tree id can be negative.
	 */
	case BTRFS_ROOT_ITEM_KEY:
		printf(" ");
		print_objectid(stdout, offset, type);
		printf(")");
		break;
	default:
		if (offset == (u64)-1)
			printf(" -1)");
		else
			printf(" %llu)", (unsigned long long)offset);
		break;
	}
}

static void print_uuid_item(struct extent_buffer *l, unsigned long offset,
			    u32 item_size)
{
	if (item_size & (sizeof(u64) - 1)) {
		printf("btrfs: uuid item with illegal size %lu!\n",
		       (unsigned long)item_size);
		return;
	}
	while (item_size) {
		__le64 subvol_id;

		read_extent_buffer(l, &subvol_id, offset, sizeof(u64));
		printf("\t\tsubvol_id %llu\n",
			(unsigned long long)le64_to_cpu(subvol_id));
		item_size -= sizeof(u64);
		offset += sizeof(u64);
	}
}

/* Btrfs inode flag stringification helper */
#define STRCAT_ONE_INODE_FLAG(flags, name, empty, dst) ({			\
	if (flags & BTRFS_INODE_##name) {				\
		if (!empty)						\
			strcat(dst, "|");				\
		strcat(dst, #name);					\
		empty = 0;						\
	}								\
})

/*
 * Caller should ensure sizeof(*ret) >= 102: all charactors plus '|' of
 * BTRFS_INODE_* flags
 */
static void inode_flags_to_str(u64 flags, char *ret)
{
	int empty = 1;

	STRCAT_ONE_INODE_FLAG(flags, NODATASUM, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, NODATACOW, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, READONLY, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, NOCOMPRESS, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, PREALLOC, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, SYNC, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, IMMUTABLE, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, APPEND, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, NODUMP, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, NOATIME, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, DIRSYNC, empty, ret);
	STRCAT_ONE_INODE_FLAG(flags, COMPRESS, empty, ret);
	if (empty)
		strcat(ret, "none");
}

static void print_inode_item(struct extent_buffer *eb,
		struct btrfs_inode_item *ii)
{
	char flags_str[256];

	memset(flags_str, 0, sizeof(flags_str));
	inode_flags_to_str(btrfs_inode_flags(eb, ii), flags_str);
	printf("\t\tgeneration %llu transid %llu size %llu nbytes %llu\n"
	       "\t\tblock group %llu mode %o links %u uid %u gid %u rdev %llu\n"
	       "\t\tsequence %llu flags 0x%llx(%s)\n",
	       (unsigned long long)btrfs_inode_generation(eb, ii),
	       (unsigned long long)btrfs_inode_transid(eb, ii),
	       (unsigned long long)btrfs_inode_size(eb, ii),
	       (unsigned long long)btrfs_inode_nbytes(eb, ii),
	       (unsigned long long)btrfs_inode_block_group(eb,ii),
	       btrfs_inode_mode(eb, ii),
	       btrfs_inode_nlink(eb, ii),
	       btrfs_inode_uid(eb, ii),
	       btrfs_inode_gid(eb, ii),
	       (unsigned long long)btrfs_inode_rdev(eb,ii),
	       (unsigned long long)btrfs_inode_sequence(eb, ii),
	       (unsigned long long)btrfs_inode_flags(eb,ii),
	       flags_str);
	print_timespec(eb, btrfs_inode_atime(ii), "\t\tatime ", "\n");
	print_timespec(eb, btrfs_inode_ctime(ii), "\t\tctime ", "\n");
	print_timespec(eb, btrfs_inode_mtime(ii), "\t\tmtime ", "\n");
	print_timespec(eb, btrfs_inode_otime(ii), "\t\totime ", "\n");
}

static void print_disk_balance_args(struct btrfs_disk_balance_args *ba)
{
	printf("\t\tprofiles %llu devid %llu target %llu flags %llu\n",
			(unsigned long long)le64_to_cpu(ba->profiles),
			(unsigned long long)le64_to_cpu(ba->devid),
			(unsigned long long)le64_to_cpu(ba->target),
			(unsigned long long)le64_to_cpu(ba->flags));
	printf("\t\tusage_min %u usage_max %u pstart %llu pend %llu\n",
			le32_to_cpu(ba->usage_min),
			le32_to_cpu(ba->usage_max),
			(unsigned long long)le64_to_cpu(ba->pstart),
			(unsigned long long)le64_to_cpu(ba->pend));
	printf("\t\tvstart %llu vend %llu limit_min %u limit_max %u\n",
			(unsigned long long)le64_to_cpu(ba->vstart),
			(unsigned long long)le64_to_cpu(ba->vend),
			le32_to_cpu(ba->limit_min),
			le32_to_cpu(ba->limit_max));
	printf("\t\tstripes_min %u stripes_max %u\n",
			le32_to_cpu(ba->stripes_min),
			le32_to_cpu(ba->stripes_max));
}

static void print_balance_item(struct extent_buffer *eb,
		struct btrfs_balance_item *bi)
{
	printf("\t\tbalance status flags %llu\n",
			btrfs_balance_item_flags(eb, bi));

	printf("\t\tDATA\n");
	print_disk_balance_args(btrfs_balance_item_data(eb, bi));
	printf("\t\tMETADATA\n");
	print_disk_balance_args(btrfs_balance_item_meta(eb, bi));
	printf("\t\tSYSTEM\n");
	print_disk_balance_args(btrfs_balance_item_sys(eb, bi));
}

static void print_dev_stats(struct extent_buffer *eb,
		struct btrfs_dev_stats_item *stats, u32 size)
{
	int i;
	u32 known = BTRFS_DEV_STAT_VALUES_MAX * sizeof(__le64);
	__le64 *values = btrfs_dev_stats_values(eb, stats);

	printf("\t\tdevice stats\n");
	printf("\t\twrite_errs %llu read_errs %llu flush_errs %llu corruption_errs %llu generation %llu\n",
		(unsigned long long)le64_to_cpu(values[BTRFS_DEV_STAT_WRITE_ERRS]),
		(unsigned long long)le64_to_cpu(values[BTRFS_DEV_STAT_READ_ERRS]),
		(unsigned long long)le64_to_cpu(values[BTRFS_DEV_STAT_FLUSH_ERRS]),
		(unsigned long long)le64_to_cpu(values[BTRFS_DEV_STAT_CORRUPTION_ERRS]),
		(unsigned long long)le64_to_cpu(values[BTRFS_DEV_STAT_GENERATION_ERRS]));

	if (known < size) {
		printf("\t\tunknown stats item bytes %u", size - known);
		for (i = BTRFS_DEV_STAT_VALUES_MAX; i * sizeof(__le64) < size; i++) {
			printf("\t\tunknown item %u offset %zu value %llu\n",
				i, i * sizeof(__le64),
				(unsigned long long)le64_to_cpu(values[i]));
		}
	}
}

static void print_block_group_item(struct extent_buffer *eb,
		struct btrfs_block_group_item *bgi)
{
	struct btrfs_block_group_item bg_item;
	char flags_str[256];

	read_extent_buffer(eb, &bg_item, (unsigned long)bgi, sizeof(bg_item));
	memset(flags_str, 0, sizeof(flags_str));
	bg_flags_to_str(btrfs_block_group_flags(&bg_item), flags_str);
	printf("\t\tblock group used %llu chunk_objectid %llu flags %s\n",
		(unsigned long long)btrfs_block_group_used(&bg_item),
		(unsigned long long)btrfs_block_group_chunk_objectid(&bg_item),
		flags_str);
}

static void print_extent_data_ref(struct extent_buffer *eb, int slot)
{
	struct btrfs_extent_data_ref *dref;

	dref = btrfs_item_ptr(eb, slot, struct btrfs_extent_data_ref);
	printf("\t\textent data backref root %llu "
		"objectid %llu offset %llu count %u\n",
		(unsigned long long)btrfs_extent_data_ref_root(eb, dref),
		(unsigned long long)btrfs_extent_data_ref_objectid(eb, dref),
		(unsigned long long)btrfs_extent_data_ref_offset(eb, dref),
		btrfs_extent_data_ref_count(eb, dref));
}

static void print_shared_data_ref(struct extent_buffer *eb, int slot)
{
	struct btrfs_shared_data_ref *sref;

	sref = btrfs_item_ptr(eb, slot, struct btrfs_shared_data_ref);
	printf("\t\tshared data backref count %u\n",
		btrfs_shared_data_ref_count(eb, sref));
}

static void print_free_space_info(struct extent_buffer *eb, int slot)
{
	struct btrfs_free_space_info *free_info;

	free_info = btrfs_item_ptr(eb, slot, struct btrfs_free_space_info);
	printf("\t\tfree space info extent count %u flags %u\n",
		(unsigned)btrfs_free_space_extent_count(eb, free_info),
		(unsigned)btrfs_free_space_flags(eb, free_info));
}

static void print_dev_extent(struct extent_buffer *eb, int slot)
{
	struct btrfs_dev_extent *dev_extent;
	u8 uuid[BTRFS_UUID_SIZE];
	char uuid_str[BTRFS_UUID_UNPARSED_SIZE];

	dev_extent = btrfs_item_ptr(eb, slot, struct btrfs_dev_extent);
	read_extent_buffer(eb, uuid,
		(unsigned long)btrfs_dev_extent_chunk_tree_uuid(dev_extent),
		BTRFS_UUID_SIZE);
	uuid_unparse(uuid, uuid_str);
	printf("\t\tdev extent chunk_tree %llu\n"
		"\t\tchunk_objectid %llu chunk_offset %llu "
		"length %llu\n"
		"\t\tchunk_tree_uuid %s\n",
		(unsigned long long)btrfs_dev_extent_chunk_tree(eb, dev_extent),
		(unsigned long long)btrfs_dev_extent_chunk_objectid(eb, dev_extent),
		(unsigned long long)btrfs_dev_extent_chunk_offset(eb, dev_extent),
		(unsigned long long)btrfs_dev_extent_length(eb, dev_extent),
		uuid_str);
}

static void print_qgroup_status(struct extent_buffer *eb, int slot)
{
	struct btrfs_qgroup_status_item *qg_status;
	char flags_str[256];

	qg_status = btrfs_item_ptr(eb, slot, struct btrfs_qgroup_status_item);
	memset(flags_str, 0, sizeof(flags_str));
	qgroup_flags_to_str(btrfs_qgroup_status_flags(eb, qg_status),
					flags_str);
	printf("\t\tversion %llu generation %llu flags %s scan %lld\n",
		(unsigned long long)btrfs_qgroup_status_version(eb, qg_status),
		(unsigned long long)btrfs_qgroup_status_generation(eb, qg_status),
		flags_str,
		(unsigned long long)btrfs_qgroup_status_rescan(eb, qg_status));
}

static void print_qgroup_info(struct extent_buffer *eb, int slot)
{
	struct btrfs_qgroup_info_item *qg_info;

	qg_info = btrfs_item_ptr(eb, slot, struct btrfs_qgroup_info_item);
	printf("\t\tgeneration %llu\n"
		"\t\treferenced %llu referenced_compressed %llu\n"
		"\t\texclusive %llu exclusive_compressed %llu\n",
		(unsigned long long)btrfs_qgroup_info_generation(eb, qg_info),
		(unsigned long long)btrfs_qgroup_info_referenced(eb, qg_info),
		(unsigned long long)btrfs_qgroup_info_referenced_compressed(eb,
								       qg_info),
		(unsigned long long)btrfs_qgroup_info_exclusive(eb, qg_info),
		(unsigned long long)btrfs_qgroup_info_exclusive_compressed(eb,
								      qg_info));
}

static void print_qgroup_limit(struct extent_buffer *eb, int slot)
{
	struct btrfs_qgroup_limit_item *qg_limit;

	qg_limit = btrfs_item_ptr(eb, slot, struct btrfs_qgroup_limit_item);
	printf("\t\tflags %llx\n"
		"\t\tmax_referenced %lld max_exclusive %lld\n"
		"\t\trsv_referenced %lld rsv_exclusive %lld\n",
		(unsigned long long)btrfs_qgroup_limit_flags(eb, qg_limit),
		(long long)btrfs_qgroup_limit_max_referenced(eb, qg_limit),
		(long long)btrfs_qgroup_limit_max_exclusive(eb, qg_limit),
		(long long)btrfs_qgroup_limit_rsv_referenced(eb, qg_limit),
		(long long)btrfs_qgroup_limit_rsv_exclusive(eb, qg_limit));
}

static void print_persistent_item(struct extent_buffer *eb, void *ptr,
		u32 item_size, u64 objectid, u64 offset)
{
	printf("\t\tpersistent item objectid ");
	print_objectid(stdout, objectid, BTRFS_PERSISTENT_ITEM_KEY);
	printf(" offset %llu\n", (unsigned long long)offset);
	switch (objectid) {
	case BTRFS_DEV_STATS_OBJECTID:
		print_dev_stats(eb, ptr, item_size);
		break;
	default:
		printf("\t\tunknown persistent item objectid %llu\n", objectid);
	}
}

static void print_temporary_item(struct extent_buffer *eb, void *ptr,
		u64 objectid, u64 offset)
{
	printf("\t\ttemporary item objectid ");
	print_objectid(stdout, objectid, BTRFS_TEMPORARY_ITEM_KEY);
	printf(" offset %llu\n", (unsigned long long)offset);
	switch (objectid) {
	case BTRFS_BALANCE_OBJECTID:
		print_balance_item(eb, ptr);
		break;
	default:
		printf("\t\tunknown temporary item objectid %llu\n", objectid);
	}
}

static void print_extent_csum(struct extent_buffer *eb,
		struct btrfs_fs_info *fs_info, u32 item_size, u64 start)
{
	u32 size;

	size = (item_size / btrfs_super_csum_size(fs_info->super_copy)) *
			fs_info->sectorsize;
	printf("\t\trange start %llu end %llu length %u\n",
			(unsigned long long)start,
			(unsigned long long)start + size, size);
}

/* Caller must ensure sizeof(*ret) >= 14 "WRITTEN|RELOC" */
static void header_flags_to_str(u64 flags, char *ret)
{
	int empty = 1;

	if (flags & BTRFS_HEADER_FLAG_WRITTEN) {
		empty = 0;
		strcpy(ret, "WRITTEN");
	}
	if (flags & BTRFS_HEADER_FLAG_RELOC) {
		if (!empty)
			strcat(ret, "|");
		strcat(ret, "RELOC");
	}
}

void btrfs_print_leaf(struct btrfs_root *root, struct extent_buffer *eb)
{
	struct btrfs_item *item;
	struct btrfs_disk_key disk_key;
	char flags_str[128];
	u32 i;
	u32 nr;
	u64 flags;
	u8 backref_rev;

	flags = btrfs_header_flags(eb) & ~BTRFS_BACKREF_REV_MASK;
	backref_rev = btrfs_header_flags(eb) >> BTRFS_BACKREF_REV_SHIFT;
	header_flags_to_str(flags, flags_str);
	nr = btrfs_header_nritems(eb);

	printf("leaf %llu items %d free space %d generation %llu owner %llu\n",
		(unsigned long long)btrfs_header_bytenr(eb), nr,
		btrfs_leaf_free_space(root, eb),
		(unsigned long long)btrfs_header_generation(eb),
		(unsigned long long)btrfs_header_owner(eb));
	printf("leaf %llu flags 0x%llx(%s) backref revision %d\n",
		btrfs_header_bytenr(eb), flags, flags_str, backref_rev);
	print_uuids(eb);
	fflush(stdout);

	for (i = 0; i < nr; i++) {
		u32 item_size;
		void *ptr;
		u64 objectid;
		u32 type;
		u64 offset;

		item = btrfs_item_nr(i);
		item_size = btrfs_item_size(eb, item);
		/* Untyped extraction of slot from btrfs_item_ptr */
		ptr = btrfs_item_ptr(eb, i, void*);

		btrfs_item_key(eb, &disk_key, i);
		objectid = btrfs_disk_key_objectid(&disk_key);
		type = btrfs_disk_key_type(&disk_key);
		offset = btrfs_disk_key_offset(&disk_key);

		printf("\titem %d ", i);
		btrfs_print_key(&disk_key);
		printf(" itemoff %d itemsize %d\n",
			btrfs_item_offset(eb, item),
			btrfs_item_size(eb, item));

		if (type == 0 && objectid == BTRFS_FREE_SPACE_OBJECTID)
			print_free_space_header(eb, i);

		switch (type) {
		case BTRFS_INODE_ITEM_KEY:
			print_inode_item(eb, ptr);
			break;
		case BTRFS_INODE_REF_KEY:
			print_inode_ref_item(eb, item_size, ptr);
			break;
		case BTRFS_INODE_EXTREF_KEY:
			print_inode_extref_item(eb, item_size, ptr);
			break;
		case BTRFS_DIR_ITEM_KEY:
		case BTRFS_DIR_INDEX_KEY:
		case BTRFS_XATTR_ITEM_KEY:
			print_dir_item(eb, item_size, ptr);
			break;
		case BTRFS_DIR_LOG_INDEX_KEY:
		case BTRFS_DIR_LOG_ITEM_KEY: {
			struct btrfs_dir_log_item *dlog;

			dlog = btrfs_item_ptr(eb, i, struct btrfs_dir_log_item);
			printf("\t\tdir log end %Lu\n",
			       (unsigned long long)btrfs_dir_log_end(eb, dlog));
			break;
			}
		case BTRFS_ORPHAN_ITEM_KEY:
			printf("\t\torphan item\n");
			break;
		case BTRFS_ROOT_ITEM_KEY:
			print_root_item(eb, i);
			break;
		case BTRFS_ROOT_REF_KEY:
			print_root_ref(eb, i, "ref");
			break;
		case BTRFS_ROOT_BACKREF_KEY:
			print_root_ref(eb, i, "backref");
			break;
		case BTRFS_EXTENT_ITEM_KEY:
			print_extent_item(eb, i, 0);
			break;
		case BTRFS_METADATA_ITEM_KEY:
			print_extent_item(eb, i, 1);
			break;
		case BTRFS_TREE_BLOCK_REF_KEY:
			printf("\t\ttree block backref\n");
			break;
		case BTRFS_SHARED_BLOCK_REF_KEY:
			printf("\t\tshared block backref\n");
			break;
		case BTRFS_EXTENT_DATA_REF_KEY:
			print_extent_data_ref(eb, i);
			break;
		case BTRFS_SHARED_DATA_REF_KEY:
			print_shared_data_ref(eb, i);
			break;
		case BTRFS_EXTENT_REF_V0_KEY:
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
			print_extent_ref_v0(eb, i);
#else
			BUG();
#endif
			break;
		case BTRFS_CSUM_ITEM_KEY:
			printf("\t\tcsum item\n");
			break;
		case BTRFS_EXTENT_CSUM_KEY:
			print_extent_csum(eb, root->fs_info, item_size,
					offset);
			break;
		case BTRFS_EXTENT_DATA_KEY:
			print_file_extent_item(eb, item, i, ptr);
			break;
		case BTRFS_BLOCK_GROUP_ITEM_KEY:
			print_block_group_item(eb, ptr);
			break;
		case BTRFS_FREE_SPACE_INFO_KEY:
			print_free_space_info(eb, i);
			break;
		case BTRFS_FREE_SPACE_EXTENT_KEY:
			printf("\t\tfree space extent\n");
			break;
		case BTRFS_FREE_SPACE_BITMAP_KEY:
			printf("\t\tfree space bitmap\n");
			break;
		case BTRFS_CHUNK_ITEM_KEY:
			print_chunk_item(eb, ptr);
			break;
		case BTRFS_DEV_ITEM_KEY:
			print_dev_item(eb, ptr);
			break;
		case BTRFS_DEV_EXTENT_KEY:
			print_dev_extent(eb, i);
			break;
		case BTRFS_QGROUP_STATUS_KEY:
			print_qgroup_status(eb, i);
			break;
		case BTRFS_QGROUP_RELATION_KEY:
			break;
		case BTRFS_QGROUP_INFO_KEY:
			print_qgroup_info(eb, i);
			break;
		case BTRFS_QGROUP_LIMIT_KEY:
			print_qgroup_limit(eb, i);
			break;
		case BTRFS_UUID_KEY_SUBVOL:
		case BTRFS_UUID_KEY_RECEIVED_SUBVOL:
			print_uuid_item(eb, btrfs_item_ptr_offset(eb, i),
					btrfs_item_size_nr(eb, i));
			break;
		case BTRFS_STRING_ITEM_KEY: {
			const char *str = eb->data + btrfs_item_ptr_offset(eb, i);

			printf("\t\titem data %.*s\n", item_size, str);
			break;
			}
		case BTRFS_PERSISTENT_ITEM_KEY:
			print_persistent_item(eb, ptr, item_size, objectid,
					offset);
			break;
		case BTRFS_TEMPORARY_ITEM_KEY:
			print_temporary_item(eb, ptr, objectid, offset);
			break;
		};
		fflush(stdout);
	}
}

void btrfs_print_tree(struct btrfs_root *root, struct extent_buffer *eb, int follow)
{
	u32 i;
	u32 nr;
	struct btrfs_disk_key disk_key;
	struct btrfs_key key;
	struct extent_buffer *next;

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
		(u32)BTRFS_NODEPTRS_PER_BLOCK(root->fs_info) - nr,
		(unsigned long long)btrfs_header_generation(eb),
		(unsigned long long)btrfs_header_owner(eb));
	print_uuids(eb);
	fflush(stdout);
	for (i = 0; i < nr; i++) {
		u64 blocknr = btrfs_node_blockptr(eb, i);
		btrfs_node_key(eb, &disk_key, i);
		btrfs_disk_key_to_cpu(&key, &disk_key);
		printf("\t");
		btrfs_print_key(&disk_key);
		printf(" block %llu (%llu) gen %llu\n",
		       (unsigned long long)blocknr,
		       (unsigned long long)blocknr / root->fs_info->nodesize,
		       (unsigned long long)btrfs_node_ptr_generation(eb, i));
		fflush(stdout);
	}
	if (!follow)
		return;

	for (i = 0; i < nr; i++) {
		next = read_tree_block(root->fs_info,
				btrfs_node_blockptr(eb, i),
				btrfs_node_ptr_generation(eb, i));
		if (!extent_buffer_uptodate(next)) {
			fprintf(stderr, "failed to read %llu in tree %llu\n",
				(unsigned long long)btrfs_node_blockptr(eb, i),
				(unsigned long long)btrfs_header_owner(eb));
			continue;
		}
		if (btrfs_is_leaf(next) && btrfs_header_level(eb) != 1) {
			warning(
	"eb corrupted: item %d eb level %d next level %d, skipping the rest",
				i, btrfs_header_level(next),
				btrfs_header_level(eb));
			goto out;
		}
		if (btrfs_header_level(next) != btrfs_header_level(eb) - 1) {
			warning(
	"eb corrupted: item %d eb level %d next level %d, skipping the rest",
				i, btrfs_header_level(next),
				btrfs_header_level(eb));
			goto out;
		}
		btrfs_print_tree(root, next, 1);
		free_extent_buffer(next);
	}

	return;

out:
	free_extent_buffer(next);
}
