/*
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

#include "kerncompat.h"
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>
#include "kernel-lib/sizes.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/file-item.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/free-space-tree.h"
#include "common/messages.h"
#include "common/string-utils.h"
#include "common/extent-tree-utils.h"
#include "common/fsfeatures.h"
#include "mkfs/common.h"
#include "convert/common.h"

#define BTRFS_CONVERT_META_GROUP_SIZE SZ_32M

/*
 * Reserve space from free_tree.
 * The algorithm is very simple, find the first cache_extent with enough space
 * and allocate from its beginning.
 */
static int reserve_free_space(struct cache_tree *free_tree, u64 len,
			      u64 *ret_start)
{
	struct cache_extent *cache;
	int found = 0;

	UASSERT(ret_start != NULL);
	cache = first_cache_extent(free_tree);
	while (cache) {
		if (cache->size > len) {
			found = 1;
			*ret_start = cache->start;

			cache->size -= len;
			if (cache->size == 0) {
				remove_cache_extent(free_tree, cache);
				free(cache);
			} else {
				cache->start += len;
			}
			break;
		}
		cache = next_cache_extent(cache);
	}
	if (!found)
		return -ENOSPC;
	return 0;
}

static inline int write_temp_super(int fd, struct btrfs_super_block *sb,
                                  u64 sb_bytenr)
{
       u8 result[BTRFS_CSUM_SIZE];
       u16 csum_type = btrfs_super_csum_type(sb);
       int ret;

       btrfs_csum_data(NULL, csum_type, (u8 *)sb + BTRFS_CSUM_SIZE,
		       result, BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
       memcpy(&sb->csum[0], result, BTRFS_CSUM_SIZE);
       ret = pwrite(fd, sb, BTRFS_SUPER_INFO_SIZE, sb_bytenr);
       if (ret < BTRFS_SUPER_INFO_SIZE)
               ret = (ret < 0 ? -errno : -EIO);
       else
               ret = 0;
       return ret;
}

/*
 * Setup temporary superblock at cfg->super_bytenr
 * Needed info are extracted from cfg, and root_bytenr, chunk_bytenr
 *
 * For now sys chunk array will be empty and dev_item is empty too.
 * They will be re-initialized at temp chunk tree setup.
 *
 * The superblock signature is not valid, denotes a partially created
 * filesystem, needs to be finalized.
 */
static int setup_temp_super(int fd, struct btrfs_mkfs_config *cfg,
			    u64 root_bytenr, u64 chunk_bytenr)
{
	unsigned char chunk_uuid[BTRFS_UUID_SIZE];
	struct btrfs_super_block super = {};
	int ret;

	cfg->num_bytes = round_down(cfg->num_bytes, cfg->sectorsize);

	if (*cfg->fs_uuid) {
		if (uuid_parse(cfg->fs_uuid, super.fsid) != 0) {
			error("could not parse UUID: %s", cfg->fs_uuid);
			ret = -EINVAL;
			goto out;
		}
		/*
		 * Caller should make sure the uuid is either unique or OK to
		 * be duplicate in case it's copied from the source filesystem.
		 */
		uuid_copy(super.metadata_uuid, super.fsid);
	} else {
		uuid_generate(super.fsid);
		uuid_unparse(super.fsid, cfg->fs_uuid);
		uuid_copy(super.metadata_uuid, super.fsid);
	}
	uuid_generate(chunk_uuid);
	uuid_unparse(chunk_uuid, cfg->chunk_uuid);

	btrfs_set_super_bytenr(&super, cfg->super_bytenr);
	btrfs_set_super_num_devices(&super, 1);
	btrfs_set_super_magic(&super, BTRFS_MAGIC_TEMPORARY);
	btrfs_set_super_generation(&super, 1);
	btrfs_set_super_root(&super, root_bytenr);
	btrfs_set_super_chunk_root(&super, chunk_bytenr);
	btrfs_set_super_total_bytes(&super, cfg->num_bytes);
	/*
	 * Temporary filesystem will only have 6 tree roots:
	 * chunk tree, root tree, extent_tree, device tree, fs tree
	 * and csum tree.
	 */
	btrfs_set_super_bytes_used(&super, 6 * cfg->nodesize);
	btrfs_set_super_sectorsize(&super, cfg->sectorsize);
	super.__unused_leafsize = cpu_to_le32(cfg->nodesize);
	btrfs_set_super_nodesize(&super, cfg->nodesize);
	btrfs_set_super_stripesize(&super, cfg->stripesize);
	btrfs_set_super_csum_type(&super, cfg->csum_type);
	btrfs_set_super_chunk_root(&super, chunk_bytenr);
	btrfs_set_super_cache_generation(&super, -1);
	btrfs_set_super_incompat_flags(&super, cfg->features.incompat_flags);
	if (cfg->label)
		strncpy_null(super.label, cfg->label, BTRFS_LABEL_SIZE);

	/* Sys chunk array will be re-initialized at chunk tree init time */
	super.sys_chunk_array_size = 0;

	ret = write_temp_super(fd, &super, cfg->super_bytenr);
out:
	return ret;
}

static int setup_temp_extent_buffer(struct extent_buffer *buf,
				    struct btrfs_mkfs_config *cfg,
				    u64 bytenr, u64 owner)
{
	unsigned char fsid[BTRFS_FSID_SIZE];
	unsigned char chunk_uuid[BTRFS_UUID_SIZE];
	int ret;

	ret = uuid_parse(cfg->fs_uuid, fsid);
	if (ret)
		return -EINVAL;
	ret = uuid_parse(cfg->chunk_uuid, chunk_uuid);
	if (ret)
		return -EINVAL;

	memset(buf->data, 0, cfg->nodesize);
	buf->len = cfg->nodesize;
	btrfs_set_header_bytenr(buf, bytenr);
	btrfs_set_header_generation(buf, 1);
	btrfs_set_header_backref_rev(buf, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(buf, owner);
	btrfs_set_header_flag(buf, BTRFS_HEADER_FLAG_WRITTEN);
	write_extent_buffer(buf, chunk_uuid, btrfs_header_chunk_tree_uuid(buf),
			    BTRFS_UUID_SIZE);
	write_extent_buffer(buf, fsid, btrfs_header_fsid(), BTRFS_FSID_SIZE);
	return 0;
}

static u32 get_item_offset(const struct extent_buffer *eb,
			   const struct btrfs_mkfs_config *cfg)
{
	u32 slot = btrfs_header_nritems(eb);

	if (slot)
		return btrfs_item_offset(eb, slot - 1);
	else
		return cfg->leaf_data_size;
}

static void insert_temp_root_item(struct extent_buffer *buf,
				  struct btrfs_mkfs_config *cfg,
				  u64 objectid, u64 bytenr)
{
	struct btrfs_root_item root_item;
	struct btrfs_inode_item *inode_item;
	struct btrfs_disk_key disk_key;
	u32 slot = btrfs_header_nritems(buf);
	u32 itemoff = get_item_offset(buf, cfg);

	btrfs_set_header_nritems(buf, slot + 1);
	itemoff -= sizeof(root_item);
	memset(&root_item, 0, sizeof(root_item));
	inode_item = &root_item.inode;
	btrfs_set_stack_inode_generation(inode_item, 1);
	btrfs_set_stack_inode_size(inode_item, 3);
	btrfs_set_stack_inode_nlink(inode_item, 1);
	btrfs_set_stack_inode_nbytes(inode_item, cfg->nodesize);
	btrfs_set_stack_inode_mode(inode_item, S_IFDIR | 0755);
	btrfs_set_root_refs(&root_item, 1);
	btrfs_set_root_used(&root_item, cfg->nodesize);
	btrfs_set_root_generation(&root_item, 1);
	btrfs_set_root_bytenr(&root_item, bytenr);

	memset(&disk_key, 0, sizeof(disk_key));
	btrfs_set_disk_key_type(&disk_key, BTRFS_ROOT_ITEM_KEY);
	btrfs_set_disk_key_objectid(&disk_key, objectid);
	btrfs_set_disk_key_offset(&disk_key, 0);

	btrfs_set_item_key(buf, &disk_key, slot);
	btrfs_set_item_offset(buf, slot, itemoff);
	btrfs_set_item_size(buf, slot, sizeof(root_item));
	write_extent_buffer(buf, &root_item,
			    btrfs_item_ptr_offset(buf, slot),
			    sizeof(root_item));
}

/*
 * Setup an extent buffer for tree block.
 */
static inline int write_temp_extent_buffer(int fd, struct extent_buffer *buf,
					   u64 bytenr,
					   struct btrfs_mkfs_config *cfg)
{
	int ret;

	csum_tree_block_size(buf, btrfs_csum_type_size(cfg->csum_type), 0,
			     cfg->csum_type);

	/* Temporary extent buffer is always mapped 1:1 on disk */
	ret = pwrite(fd, buf->data, buf->len, bytenr);
	if (ret < buf->len)
		ret = (ret < 0 ? ret : -EIO);
	else
		ret = 0;
	return ret;
}

static int setup_temp_root_tree(int fd, struct btrfs_mkfs_config *cfg,
				u64 root_bytenr, u64 extent_bytenr,
				u64 dev_bytenr, u64 fs_bytenr, u64 csum_bytenr)
{
	struct extent_buffer *buf = NULL;
	int ret;

	/*
	 * Provided bytenr must in ascending order, or tree root will have a
	 * bad key order.
	 */
	UASSERT(root_bytenr < extent_bytenr && extent_bytenr < dev_bytenr &&
	        dev_bytenr < fs_bytenr && fs_bytenr < csum_bytenr);
	buf = malloc(sizeof(*buf) + cfg->nodesize);
	if (!buf)
		return -ENOMEM;

	ret = setup_temp_extent_buffer(buf, cfg, root_bytenr,
				       BTRFS_ROOT_TREE_OBJECTID);
	if (ret < 0)
		goto out;

	insert_temp_root_item(buf, cfg, BTRFS_EXTENT_TREE_OBJECTID, extent_bytenr);
	insert_temp_root_item(buf, cfg, BTRFS_DEV_TREE_OBJECTID, dev_bytenr);
	insert_temp_root_item(buf, cfg, BTRFS_FS_TREE_OBJECTID, fs_bytenr);
	insert_temp_root_item(buf, cfg, BTRFS_CSUM_TREE_OBJECTID, csum_bytenr);

	ret = write_temp_extent_buffer(fd, buf, root_bytenr, cfg);
out:
	free(buf);
	return ret;
}

static int insert_temp_dev_item(int fd, struct extent_buffer *buf,
				struct btrfs_mkfs_config *cfg)
{
	struct btrfs_disk_key disk_key;
	struct btrfs_dev_item *dev_item;
	unsigned char dev_uuid[BTRFS_UUID_SIZE];
	unsigned char fsid[BTRFS_FSID_SIZE];
	struct btrfs_super_block super;
	u32 slot = btrfs_header_nritems(buf);
	u32 itemoff = get_item_offset(buf, cfg);
	int ret;

	ret = pread(fd, &super, BTRFS_SUPER_INFO_SIZE, cfg->super_bytenr);
	if (ret < BTRFS_SUPER_INFO_SIZE) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	btrfs_set_header_nritems(buf, slot + 1);
	itemoff -= sizeof(*dev_item);
	/* setup device item 1, 0 is for replace case */
	btrfs_set_disk_key_type(&disk_key, BTRFS_DEV_ITEM_KEY);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_DEV_ITEMS_OBJECTID);
	btrfs_set_disk_key_offset(&disk_key, 1);
	btrfs_set_item_key(buf, &disk_key, slot);
	btrfs_set_item_offset(buf, slot, itemoff);
	btrfs_set_item_size(buf, slot, sizeof(*dev_item));

	dev_item = btrfs_item_ptr(buf, slot, struct btrfs_dev_item);
	/* Generate device uuid */
	uuid_generate(dev_uuid);
	write_extent_buffer(buf, dev_uuid,
			(unsigned long)btrfs_device_uuid(dev_item),
			BTRFS_UUID_SIZE);
	uuid_parse(cfg->fs_uuid, fsid);
	write_extent_buffer(buf, fsid,
			(unsigned long)btrfs_device_fsid(dev_item),
			BTRFS_FSID_SIZE);
	btrfs_set_device_id(buf, dev_item, 1);
	btrfs_set_device_generation(buf, dev_item, 0);
	btrfs_set_device_total_bytes(buf, dev_item, cfg->num_bytes);
	/*
	 * The number must match the initial SYSTEM and META chunk size
	 */
	btrfs_set_device_bytes_used(buf, dev_item,
			BTRFS_MKFS_SYSTEM_GROUP_SIZE +
			BTRFS_CONVERT_META_GROUP_SIZE);
	btrfs_set_device_io_align(buf, dev_item, cfg->sectorsize);
	btrfs_set_device_io_width(buf, dev_item, cfg->sectorsize);
	btrfs_set_device_sector_size(buf, dev_item, cfg->sectorsize);
	btrfs_set_device_type(buf, dev_item, 0);

	/* Super dev_item is not complete, copy the complete one to sb */
	read_extent_buffer(buf, &super.dev_item, (unsigned long)dev_item,
			   sizeof(*dev_item));
	ret = write_temp_super(fd, &super, cfg->super_bytenr);
out:
	return ret;
}

static int insert_temp_chunk_item(int fd, struct extent_buffer *buf,
				  struct btrfs_mkfs_config *cfg,
				  u64 start, u64 len, u64 type)
{
	struct btrfs_chunk *chunk;
	struct btrfs_disk_key disk_key;
	struct btrfs_super_block sb;
	u32 slot = btrfs_header_nritems(buf);
	u32 itemoff = get_item_offset(buf, cfg);
	int ret = 0;

	ret = pread(fd, &sb, BTRFS_SUPER_INFO_SIZE, cfg->super_bytenr);
	if (ret < BTRFS_SUPER_INFO_SIZE) {
		ret = (ret < 0 ? ret : -EIO);
		return ret;
	}

	btrfs_set_header_nritems(buf, slot + 1);
	itemoff -= btrfs_chunk_item_size(1);
	btrfs_set_disk_key_type(&disk_key, BTRFS_CHUNK_ITEM_KEY);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_disk_key_offset(&disk_key, start);
	btrfs_set_item_key(buf, &disk_key, slot);
	btrfs_set_item_offset(buf, slot, itemoff);
	btrfs_set_item_size(buf, slot, btrfs_chunk_item_size(1));

	chunk = btrfs_item_ptr(buf, slot, struct btrfs_chunk);
	btrfs_set_chunk_length(buf, chunk, len);
	btrfs_set_chunk_owner(buf, chunk, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_chunk_stripe_len(buf, chunk, BTRFS_STRIPE_LEN);
	btrfs_set_chunk_type(buf, chunk, type);
	btrfs_set_chunk_io_align(buf, chunk, cfg->sectorsize);
	btrfs_set_chunk_io_width(buf, chunk, cfg->sectorsize);
	btrfs_set_chunk_sector_size(buf, chunk, cfg->sectorsize);
	btrfs_set_chunk_num_stripes(buf, chunk, 1);
	/* TODO: Support DUP profile for system chunk */
	btrfs_set_stripe_devid_nr(buf, chunk, 0, 1);
	/* We are doing 1:1 mapping, so start is its dev offset */
	btrfs_set_stripe_offset_nr(buf, chunk, 0, start);
	write_extent_buffer(buf, sb.dev_item.uuid,
			    (unsigned long)btrfs_stripe_dev_uuid_nr(chunk, 0),
			    BTRFS_UUID_SIZE);

	/*
	 * If it's system chunk, also copy it to super block.
	 */
	if (type & BTRFS_BLOCK_GROUP_SYSTEM) {
		char *cur;
		u32 array_size;

		cur = (char *)sb.sys_chunk_array
			+ btrfs_super_sys_array_size(&sb);
		memcpy(cur, &disk_key, sizeof(disk_key));
		cur += sizeof(disk_key);
		read_extent_buffer(buf, cur, (unsigned long int)chunk,
				   btrfs_chunk_item_size(1));
		array_size = btrfs_super_sys_array_size(&sb);
		array_size += btrfs_chunk_item_size(1) +
					    sizeof(disk_key);
		btrfs_set_super_sys_array_size(&sb, array_size);

		ret = write_temp_super(fd, &sb, cfg->super_bytenr);
	}
	return ret;
}

static int setup_temp_chunk_tree(int fd, struct btrfs_mkfs_config *cfg,
				 u64 sys_chunk_start, u64 meta_chunk_start,
				 u64 chunk_bytenr)
{
	struct extent_buffer *buf = NULL;
	int ret;

	/* Must ensure SYS chunk starts before META chunk */
	if (meta_chunk_start < sys_chunk_start) {
		error("wrong chunk order: meta < system %llu < %llu",
				meta_chunk_start, sys_chunk_start);
		return -EINVAL;
	}
	buf = malloc(sizeof(*buf) + cfg->nodesize);
	if (!buf)
		return -ENOMEM;
	ret = setup_temp_extent_buffer(buf, cfg, chunk_bytenr,
				       BTRFS_CHUNK_TREE_OBJECTID);
	if (ret < 0)
		goto out;

	ret = insert_temp_dev_item(fd, buf, cfg);
	if (ret < 0)
		goto out;
	ret = insert_temp_chunk_item(fd, buf, cfg, sys_chunk_start,
				     BTRFS_MKFS_SYSTEM_GROUP_SIZE,
				     BTRFS_BLOCK_GROUP_SYSTEM);
	if (ret < 0)
		goto out;
	ret = insert_temp_chunk_item(fd, buf, cfg, meta_chunk_start,
				     BTRFS_CONVERT_META_GROUP_SIZE,
				     BTRFS_BLOCK_GROUP_METADATA);
	if (ret < 0)
		goto out;
	ret = write_temp_extent_buffer(fd, buf, chunk_bytenr, cfg);

out:
	free(buf);
	return ret;
}

static void insert_temp_dev_extent(struct extent_buffer *buf,
				   int *slot, u32 *itemoff, u64 start, u64 len)
{
	struct btrfs_dev_extent *dev_extent;
	struct btrfs_disk_key disk_key;

	btrfs_set_header_nritems(buf, *slot + 1);
	(*itemoff) -= sizeof(*dev_extent);
	btrfs_set_disk_key_type(&disk_key, BTRFS_DEV_EXTENT_KEY);
	btrfs_set_disk_key_objectid(&disk_key, 1);
	btrfs_set_disk_key_offset(&disk_key, start);
	btrfs_set_item_key(buf, &disk_key, *slot);
	btrfs_set_item_offset(buf, *slot, *itemoff);
	btrfs_set_item_size(buf, *slot, sizeof(*dev_extent));

	dev_extent = btrfs_item_ptr(buf, *slot, struct btrfs_dev_extent);
	btrfs_set_dev_extent_chunk_objectid(buf, dev_extent,
					    BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_dev_extent_length(buf, dev_extent, len);
	btrfs_set_dev_extent_chunk_offset(buf, dev_extent, start);
	btrfs_set_dev_extent_chunk_tree(buf, dev_extent,
					BTRFS_CHUNK_TREE_OBJECTID);
	(*slot)++;
}

static int setup_temp_dev_tree(int fd, struct btrfs_mkfs_config *cfg,
			       u64 sys_chunk_start, u64 meta_chunk_start,
			       u64 dev_bytenr)
{
	struct extent_buffer *buf = NULL;
	u32 itemoff = cfg->leaf_data_size;
	int slot = 0;
	int ret;

	/* Must ensure SYS chunk starts before META chunk */
	if (meta_chunk_start < sys_chunk_start) {
		error("wrong chunk order: meta < system %llu < %llu",
				meta_chunk_start, sys_chunk_start);
		return -EINVAL;
	}
	buf = malloc(sizeof(*buf) + cfg->nodesize);
	if (!buf)
		return -ENOMEM;
	ret = setup_temp_extent_buffer(buf, cfg, dev_bytenr,
				       BTRFS_DEV_TREE_OBJECTID);
	if (ret < 0)
		goto out;
	insert_temp_dev_extent(buf, &slot, &itemoff, sys_chunk_start,
			       BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	insert_temp_dev_extent(buf, &slot, &itemoff, meta_chunk_start,
			       BTRFS_CONVERT_META_GROUP_SIZE);
	ret = write_temp_extent_buffer(fd, buf, dev_bytenr, cfg);
out:
	free(buf);
	return ret;
}

static int setup_temp_fs_tree(int fd, struct btrfs_mkfs_config *cfg,
			      u64 fs_bytenr)
{
	struct extent_buffer *buf = NULL;
	int ret;

	buf = malloc(sizeof(*buf) + cfg->nodesize);
	if (!buf)
		return -ENOMEM;
	ret = setup_temp_extent_buffer(buf, cfg, fs_bytenr,
				       BTRFS_FS_TREE_OBJECTID);
	if (ret < 0)
		goto out;
	/*
	 * Temporary fs tree is completely empty.
	 */
	ret = write_temp_extent_buffer(fd, buf, fs_bytenr, cfg);
out:
	free(buf);
	return ret;
}

static int setup_temp_csum_tree(int fd, struct btrfs_mkfs_config *cfg,
				u64 csum_bytenr)
{
	struct extent_buffer *buf = NULL;
	int ret;

	buf = malloc(sizeof(*buf) + cfg->nodesize);
	if (!buf)
		return -ENOMEM;
	ret = setup_temp_extent_buffer(buf, cfg, csum_bytenr,
				       BTRFS_CSUM_TREE_OBJECTID);
	if (ret < 0)
		goto out;
	/*
	 * Temporary csum tree is completely empty.
	 */
	ret = write_temp_extent_buffer(fd, buf, csum_bytenr, cfg);
out:
	free(buf);
	return ret;
}

/*
 * Insert one temporary extent item.
 *
 * NOTE: if skinny_metadata is not enabled, this function must be called
 * after all other trees are initialized.
 * Or fs without skinny-metadata will be screwed up.
 */
static int insert_temp_extent_item(int fd, struct extent_buffer *buf,
				   struct btrfs_mkfs_config *cfg,
				   int *slot, u32 *itemoff, u64 bytenr,
				   u64 ref_root)
{
	struct extent_buffer *tmp;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_disk_key disk_key;
	struct btrfs_disk_key tree_info_key;
	struct btrfs_tree_block_info *info;
	int itemsize;
	int skinny_metadata = cfg->features.incompat_flags &
			      BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA;
	int ret;

	if (skinny_metadata)
		itemsize = sizeof(*ei) + sizeof(*iref);
	else
		itemsize = sizeof(*ei) + sizeof(*iref) +
			   sizeof(struct btrfs_tree_block_info);

	btrfs_set_header_nritems(buf, *slot + 1);
	*(itemoff) -= itemsize;

	if (skinny_metadata) {
		btrfs_set_disk_key_type(&disk_key, BTRFS_METADATA_ITEM_KEY);
		btrfs_set_disk_key_offset(&disk_key, 0);
	} else {
		btrfs_set_disk_key_type(&disk_key, BTRFS_EXTENT_ITEM_KEY);
		btrfs_set_disk_key_offset(&disk_key, cfg->nodesize);
	}
	btrfs_set_disk_key_objectid(&disk_key, bytenr);

	btrfs_set_item_key(buf, &disk_key, *slot);
	btrfs_set_item_offset(buf, *slot, *itemoff);
	btrfs_set_item_size(buf, *slot, itemsize);

	ei = btrfs_item_ptr(buf, *slot, struct btrfs_extent_item);
	btrfs_set_extent_refs(buf, ei, 1);
	btrfs_set_extent_generation(buf, ei, 1);
	btrfs_set_extent_flags(buf, ei, BTRFS_EXTENT_FLAG_TREE_BLOCK);

	if (skinny_metadata) {
		iref = (struct btrfs_extent_inline_ref *)(ei + 1);
	} else {
		info = (struct btrfs_tree_block_info *)(ei + 1);
		iref = (struct btrfs_extent_inline_ref *)(info + 1);
	}
	btrfs_set_extent_inline_ref_type(buf, iref,
					 BTRFS_TREE_BLOCK_REF_KEY);
	btrfs_set_extent_inline_ref_offset(buf, iref, ref_root);

	(*slot)++;
	if (skinny_metadata)
		return 0;

	/*
	 * Lastly, check the tree block key by read the tree block
	 * Since we do 1:1 mapping for convert case, we can directly
	 * read the bytenr from disk
	 */
	tmp = malloc(sizeof(*tmp) + cfg->nodesize);
	if (!tmp)
		return -ENOMEM;
	ret = setup_temp_extent_buffer(tmp, cfg, bytenr, ref_root);
	if (ret < 0)
		goto out;
	ret = pread(fd, tmp->data, cfg->nodesize, bytenr);
	if (ret < cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}
	if (btrfs_header_nritems(tmp) == 0) {
		btrfs_set_disk_key_type(&tree_info_key, 0);
		btrfs_set_disk_key_objectid(&tree_info_key, 0);
		btrfs_set_disk_key_offset(&tree_info_key, 0);
	} else {
		btrfs_item_key(tmp, &tree_info_key, 0);
	}
	btrfs_set_tree_block_key(buf, info, &tree_info_key);

out:
	free(tmp);
	return ret;
}

static void insert_temp_block_group(struct extent_buffer *buf,
				   struct btrfs_mkfs_config *cfg,
				   int *slot, u32 *itemoff,
				   u64 bytenr, u64 len, u64 used, u64 flag)
{
	struct btrfs_block_group_item bgi;
	struct btrfs_disk_key disk_key;

	btrfs_set_header_nritems(buf, *slot + 1);
	(*itemoff) -= sizeof(bgi);
	btrfs_set_disk_key_type(&disk_key, BTRFS_BLOCK_GROUP_ITEM_KEY);
	btrfs_set_disk_key_objectid(&disk_key, bytenr);
	btrfs_set_disk_key_offset(&disk_key, len);
	btrfs_set_item_key(buf, &disk_key, *slot);
	btrfs_set_item_offset(buf, *slot, *itemoff);
	btrfs_set_item_size(buf, *slot, sizeof(bgi));

	btrfs_set_stack_block_group_flags(&bgi, flag);
	btrfs_set_stack_block_group_used(&bgi, used);
	btrfs_set_stack_block_group_chunk_objectid(&bgi,
			BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	write_extent_buffer(buf, &bgi, btrfs_item_ptr_offset(buf, *slot),
			    sizeof(bgi));
	(*slot)++;
}

static int setup_temp_extent_tree(int fd, struct btrfs_mkfs_config *cfg,
				  u64 chunk_bytenr, u64 root_bytenr,
				  u64 extent_bytenr, u64 dev_bytenr,
				  u64 fs_bytenr, u64 csum_bytenr)
{
	struct extent_buffer *buf = NULL;
	u32 itemoff = cfg->leaf_data_size;
	int slot = 0;
	int ret;

	/*
	 * We must ensure provided bytenr are in ascending order,
	 * or extent tree key order will be broken.
	 */
	UASSERT(chunk_bytenr < root_bytenr && root_bytenr < extent_bytenr &&
		extent_bytenr < dev_bytenr && dev_bytenr < fs_bytenr &&
		fs_bytenr < csum_bytenr);
	buf = malloc(sizeof(*buf) + cfg->nodesize);
	if (!buf)
		return -ENOMEM;

	ret = setup_temp_extent_buffer(buf, cfg, extent_bytenr,
				       BTRFS_EXTENT_TREE_OBJECTID);
	if (ret < 0)
		goto out;

	ret = insert_temp_extent_item(fd, buf, cfg, &slot, &itemoff,
			chunk_bytenr, BTRFS_CHUNK_TREE_OBJECTID);
	if (ret < 0)
		goto out;

	insert_temp_block_group(buf, cfg, &slot, &itemoff, chunk_bytenr,
			BTRFS_MKFS_SYSTEM_GROUP_SIZE, cfg->nodesize,
			BTRFS_BLOCK_GROUP_SYSTEM);

	ret = insert_temp_extent_item(fd, buf, cfg, &slot, &itemoff,
			root_bytenr, BTRFS_ROOT_TREE_OBJECTID);
	if (ret < 0)
		goto out;

	/* 5 tree block used, root, extent, dev, fs and csum*/
	insert_temp_block_group(buf, cfg, &slot, &itemoff, root_bytenr,
			BTRFS_CONVERT_META_GROUP_SIZE, cfg->nodesize * 5,
			BTRFS_BLOCK_GROUP_METADATA);

	ret = insert_temp_extent_item(fd, buf, cfg, &slot, &itemoff,
			extent_bytenr, BTRFS_EXTENT_TREE_OBJECTID);
	if (ret < 0)
		goto out;
	ret = insert_temp_extent_item(fd, buf, cfg, &slot, &itemoff,
			dev_bytenr, BTRFS_DEV_TREE_OBJECTID);
	if (ret < 0)
		goto out;
	ret = insert_temp_extent_item(fd, buf, cfg, &slot, &itemoff,
			fs_bytenr, BTRFS_FS_TREE_OBJECTID);
	if (ret < 0)
		goto out;
	ret = insert_temp_extent_item(fd, buf, cfg, &slot, &itemoff,
			csum_bytenr, BTRFS_CSUM_TREE_OBJECTID);
	if (ret < 0)
		goto out;

	ret = write_temp_extent_buffer(fd, buf, extent_bytenr, cfg);
out:
	free(buf);
	return ret;
}

static u64 largest_free_space(struct cache_tree *free_space)
{
	struct cache_extent *cache;
	u64 largest_free_space = 0;

	for (cache = first_cache_extent(free_space); cache;
	     cache = next_cache_extent(cache)) {
		if (cache->size > largest_free_space)
			largest_free_space = cache->size;
	}

	return largest_free_space;
}

/*
 * Improved version of make_btrfs().
 *
 * This one will
 * 1) Do chunk allocation to avoid used data
 *    And after this function, extent type matches chunk type
 * 2) Better structured code
 *    No super long hand written codes to initialized all tree blocks
 *    Split into small blocks and reuse codes.
 *    TODO: Reuse tree operation facilities by introducing new flags
 */
int make_convert_btrfs(int fd, struct btrfs_mkfs_config *cfg,
			      struct btrfs_convert_context *cctx)
{
	struct cache_tree *free_space = &cctx->free_space;
	struct cache_tree *used_space = &cctx->used_space;
	u64 sys_chunk_start;
	u64 meta_chunk_start;
	/* chunk tree bytenr, in system chunk */
	u64 chunk_bytenr;
	/* metadata trees bytenr, in metadata chunk */
	u64 root_bytenr;
	u64 extent_bytenr;
	u64 dev_bytenr;
	u64 fs_bytenr;
	u64 csum_bytenr;
	int ret;

	/* Source filesystem must be opened, checked and analyzed in advance */
	UASSERT(!cache_tree_empty(used_space));

	/*
	 * reserve space for temporary superblock first
	 * Here we allocate a little larger space, to keep later
	 * free space will be STRIPE_LEN aligned
	 */
	ret = reserve_free_space(free_space, BTRFS_STRIPE_LEN,
				 &cfg->super_bytenr);
	if (ret < 0) {
		error(
"failed to reserve %d bytes for temporary superblock, largest available: %llu bytes",
			BTRFS_STRIPE_LEN, largest_free_space(free_space));
		goto out;
	}

	/*
	 * Then reserve system chunk space
	 * TODO: Change system group size depending on cctx->total_bytes.
	 * If using current 4M, it can only handle less than one TB for
	 * worst case and then run out of sys space.
	 */
	ret = reserve_free_space(free_space, BTRFS_MKFS_SYSTEM_GROUP_SIZE,
				 &sys_chunk_start);
	if (ret < 0) {
		error(
"failed to reserve %d bytes for system chunk, largest available: %llu bytes",
			BTRFS_MKFS_SYSTEM_GROUP_SIZE, largest_free_space(free_space));
		goto out;
	}
	ret = reserve_free_space(free_space, BTRFS_CONVERT_META_GROUP_SIZE,
				 &meta_chunk_start);
	if (ret < 0) {
		error(
"failed to reserve %d bytes for metadata chunk, largest available: %llu bytes",
			BTRFS_CONVERT_META_GROUP_SIZE, largest_free_space(free_space));
		goto out;
	}

	/*
	 * Allocated meta/sys chunks will be mapped 1:1 with device offset.
	 *
	 * Inside the allocated metadata chunk, the layout will be:
	 *  | offset		| contents	|
	 *  -------------------------------------
	 *  | +0		| tree root	|
	 *  | +nodesize		| extent root	|
	 *  | +nodesize * 2	| device root	|
	 *  | +nodesize * 3	| fs tree	|
	 *  | +nodesize * 4	| csum tree	|
	 *  -------------------------------------
	 * Inside the allocated system chunk, the layout will be:
	 *  | offset		| contents	|
	 *  -------------------------------------
	 *  | +0		| chunk root	|
	 *  -------------------------------------
	 */
	chunk_bytenr = sys_chunk_start;
	root_bytenr = meta_chunk_start;
	extent_bytenr = meta_chunk_start + cfg->nodesize;
	dev_bytenr = meta_chunk_start + cfg->nodesize * 2;
	fs_bytenr = meta_chunk_start + cfg->nodesize * 3;
	csum_bytenr = meta_chunk_start + cfg->nodesize * 4;

	ret = setup_temp_super(fd, cfg, root_bytenr, chunk_bytenr);
	if (ret < 0)
		goto out;

	ret = setup_temp_root_tree(fd, cfg, root_bytenr, extent_bytenr,
				   dev_bytenr, fs_bytenr, csum_bytenr);
	if (ret < 0)
		goto out;
	ret = setup_temp_chunk_tree(fd, cfg, sys_chunk_start, meta_chunk_start,
				    chunk_bytenr);
	if (ret < 0)
		goto out;
	ret = setup_temp_dev_tree(fd, cfg, sys_chunk_start, meta_chunk_start,
				  dev_bytenr);
	if (ret < 0)
		goto out;
	ret = setup_temp_fs_tree(fd, cfg, fs_bytenr);
	if (ret < 0)
		goto out;
	ret = setup_temp_csum_tree(fd, cfg, csum_bytenr);
	if (ret < 0)
		goto out;
	/*
	 * Setup extent tree last, since it may need to read tree block key
	 * for non-skinny metadata case.
	 */
	ret = setup_temp_extent_tree(fd, cfg, chunk_bytenr, root_bytenr,
				     extent_bytenr, dev_bytenr, fs_bytenr,
				     csum_bytenr);
out:
	return ret;
}

static void __get_extent_size(struct btrfs_root *root, struct btrfs_path *path,
			      u64 *start, u64 *len)
{
	struct btrfs_key key;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	BUG_ON(!(key.type == BTRFS_EXTENT_ITEM_KEY ||
		 key.type == BTRFS_METADATA_ITEM_KEY));
	*start = key.objectid;
	if (key.type == BTRFS_EXTENT_ITEM_KEY)
		*len = key.offset;
	else
		*len = root->fs_info->nodesize;
}

/*
 * Find first overlap extent for range [bytenr, bytenr + len).
 *
 * Return 0 for found and point path to it.
 * Return >0 for not found.
 * Return <0 for err
 */
static int btrfs_search_overlap_extent(struct btrfs_root *root,
				       struct btrfs_path *path, u64 bytenr, u64 len)
{
	struct btrfs_key key;
	u64 cur_start;
	u64 cur_len;
	int ret;

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		return ret;
	if (ret == 0) {
		error_msg(ERROR_MSG_UNEXPECTED, "EXTENT_DATA found at %llu", bytenr);
		return -EUCLEAN;
	}

	ret = btrfs_previous_extent_item(root, path, 0);
	if (ret < 0)
		return ret;
	/* No previous, check next extent. */
	if (ret > 0)
		goto next;
	__get_extent_size(root, path, &cur_start, &cur_len);
	/* Tail overlap. */
	if (cur_start + cur_len > bytenr)
		return 1;

next:
	ret = btrfs_next_extent_item(root, path, bytenr + len);
	if (ret < 0)
		return ret;
	/* No next, prev already checked, no overlap. */
	if (ret > 0)
		return 0;
	__get_extent_size(root, path, &cur_start, &cur_len);
	/* Head overlap.*/
	if (cur_start < bytenr + len)
		return 1;
	return 0;
}

static int __btrfs_convert_file_extent(struct btrfs_trans_handle *trans,
				       struct btrfs_root *root, u64 objectid,
				       struct btrfs_inode_item *inode,
				       u64 file_pos, u64 disk_bytenr,
				       u64 *ret_num_bytes)
{
	int ret;
	struct btrfs_fs_info *info = root->fs_info;
	struct btrfs_root *extent_root = btrfs_extent_root(info, disk_bytenr);
	struct btrfs_file_extent_item stack_fi = { 0 };
	struct btrfs_key ins_key;
	struct btrfs_path *path;
	struct btrfs_extent_item *ei;
	u64 nbytes;
	u64 extent_num_bytes;
	u64 extent_bytenr;
	u64 extent_offset;
	u64 num_bytes = *ret_num_bytes;

	/*
	 * @objectid should be an inode number, thus it must not be smaller
	 * than BTRFS_FIRST_FREE_OBJECTID.
	 */
	UASSERT(objectid >= BTRFS_FIRST_FREE_OBJECTID);

	/*
	 * All supported file system should not use its 0 extent.  As it's for
	 * hole.  And hole extent has no size limit, no need to loop.
	 */
	if (disk_bytenr == 0) {
		btrfs_set_stack_file_extent_type(&stack_fi, BTRFS_FILE_EXTENT_REG);
		btrfs_set_stack_file_extent_num_bytes(&stack_fi, num_bytes);
		btrfs_set_stack_file_extent_ram_bytes(&stack_fi, num_bytes);
		ret = btrfs_insert_file_extent(trans, root, objectid,
					       file_pos, &stack_fi);
		return ret;
	}
	num_bytes = min_t(u64, num_bytes, BTRFS_MAX_EXTENT_SIZE);

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* First to check extent overlap. */
	ret = btrfs_search_overlap_extent(extent_root, path, disk_bytenr, num_bytes);
	if (ret < 0)
		goto fail;
	if (ret > 0) {
		/* Found overlap. */
		u64 cur_start;
		u64 cur_len;

		__get_extent_size(extent_root, path, &cur_start, &cur_len);
		/* For convert case, this extent should be a subset of existing one. */
		if (disk_bytenr < cur_start) {
			error_msg(ERROR_MSG_UNEXPECTED,
				  "invalid range disk_bytenr < cur_start: %llu < %llu",
				  disk_bytenr, cur_start);
			ret = -EUCLEAN;
			goto fail;
		}

		extent_bytenr = cur_start;
		extent_num_bytes = cur_len;
		extent_offset = disk_bytenr - extent_bytenr;
	} else {
		/* No overlap, create new extent. */
		btrfs_release_path(path);
		ins_key.objectid = disk_bytenr;
		ins_key.type = BTRFS_EXTENT_ITEM_KEY;
		ins_key.offset = num_bytes;

		ret = btrfs_insert_empty_item(trans, extent_root, path,
					      &ins_key, sizeof(*ei));
		if (ret == 0) {
			struct extent_buffer *leaf;

			leaf = path->nodes[0];
			ei = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_extent_item);

			btrfs_set_extent_refs(leaf, ei, 0);
			btrfs_set_extent_generation(leaf, ei, trans->transid);
			btrfs_set_extent_flags(leaf, ei,
					       BTRFS_EXTENT_FLAG_DATA);
			btrfs_mark_buffer_dirty(leaf);

			ret = btrfs_update_block_group(trans, disk_bytenr,
						       num_bytes, 1, 0);
			if (ret)
				goto fail;
		} else if (ret != -EEXIST) {
			goto fail;
		}

		ret = remove_from_free_space_tree(trans, disk_bytenr, num_bytes);
		if (ret)
			goto fail;

		btrfs_run_delayed_refs(trans, -1);
		extent_bytenr = disk_bytenr;
		extent_num_bytes = num_bytes;
		extent_offset = 0;
	}
	btrfs_release_path(path);
	ins_key.objectid = objectid;
	ins_key.type = BTRFS_EXTENT_DATA_KEY;
	ins_key.offset = file_pos;
	btrfs_set_stack_file_extent_type(&stack_fi, BTRFS_FILE_EXTENT_REG);
	btrfs_set_stack_file_extent_disk_bytenr(&stack_fi, extent_bytenr);
	btrfs_set_stack_file_extent_disk_num_bytes(&stack_fi, extent_num_bytes);
	btrfs_set_stack_file_extent_offset(&stack_fi, extent_offset);
	btrfs_set_stack_file_extent_num_bytes(&stack_fi, num_bytes);
	btrfs_set_stack_file_extent_ram_bytes(&stack_fi, extent_num_bytes);
	ret = btrfs_insert_file_extent(trans, root, objectid, file_pos, &stack_fi);
	if (ret)
		goto fail;

	nbytes = btrfs_stack_inode_nbytes(inode) + num_bytes;
	btrfs_set_stack_inode_nbytes(inode, nbytes);

	ret = btrfs_inc_extent_ref(trans, extent_bytenr, extent_num_bytes,
				   0, root->root_key.objectid, objectid,
				   file_pos - extent_offset);
	if (ret)
		goto fail;
	ret = 0;
	*ret_num_bytes = min(extent_num_bytes - extent_offset, num_bytes);
fail:
	btrfs_free_path(path);
	return ret;
}

/*
 * Insert file extent using converted image. Do all the required works,
 * such as inserting file extent item, inserting extent item and backref item
 * into extent tree and updating block accounting.
 *
 * This is for btrfs-convert only, thus it won't support compressed regular
 * file extents.
 */
int btrfs_convert_file_extent(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *inode,
			      u64 file_pos, u64 disk_bytenr,
			      u64 num_bytes)
{
	u64 cur_disk_bytenr = disk_bytenr;
	u64 cur_file_pos = file_pos;
	u64 cur_num_bytes = num_bytes;
	int ret = 0;

	while (num_bytes > 0) {
		ret = __btrfs_convert_file_extent(trans, root, objectid,
						  inode, cur_file_pos,
						  cur_disk_bytenr,
						  &cur_num_bytes);
		if (ret < 0)
			break;
		cur_disk_bytenr += cur_num_bytes;
		cur_file_pos += cur_num_bytes;
		num_bytes -= cur_num_bytes;
	}
	return ret;
}
