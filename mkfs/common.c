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

#include <unistd.h>
#include <uuid/uuid.h>
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "utils.h"
#include "mkfs/common.h"

static u64 reference_root_table[] = {
	[1] =	BTRFS_ROOT_TREE_OBJECTID,
	[2] =	BTRFS_EXTENT_TREE_OBJECTID,
	[3] =	BTRFS_CHUNK_TREE_OBJECTID,
	[4] =	BTRFS_DEV_TREE_OBJECTID,
	[5] =	BTRFS_FS_TREE_OBJECTID,
	[6] =	BTRFS_CSUM_TREE_OBJECTID,
};

/*
 * @fs_uuid - if NULL, generates a UUID, returns back the new filesystem UUID
 *
 * The superblock signature is not valid, denotes a partially created
 * filesystem, needs to be finalized.
 */
int make_btrfs(int fd, struct btrfs_mkfs_config *cfg)
{
	struct btrfs_super_block super;
	struct extent_buffer *buf;
	struct btrfs_root_item root_item;
	struct btrfs_disk_key disk_key;
	struct btrfs_extent_item *extent_item;
	struct btrfs_inode_item *inode_item;
	struct btrfs_chunk *chunk;
	struct btrfs_dev_item *dev_item;
	struct btrfs_dev_extent *dev_extent;
	u8 chunk_tree_uuid[BTRFS_UUID_SIZE];
	u8 *ptr;
	int i;
	int ret;
	u32 itemoff;
	u32 nritems = 0;
	u64 first_free;
	u64 ref_root;
	u32 array_size;
	u32 item_size;
	int skinny_metadata = !!(cfg->features &
				 BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA);
	u64 num_bytes;

	buf = malloc(sizeof(*buf) + max(cfg->sectorsize, cfg->nodesize));
	if (!buf)
		return -ENOMEM;

	first_free = BTRFS_SUPER_INFO_OFFSET + cfg->sectorsize * 2 - 1;
	first_free &= ~((u64)cfg->sectorsize - 1);

	memset(&super, 0, sizeof(super));

	num_bytes = (cfg->num_bytes / cfg->sectorsize) * cfg->sectorsize;
	if (*cfg->fs_uuid) {
		if (uuid_parse(cfg->fs_uuid, super.fsid) != 0) {
			error("cannot not parse UUID: %s", cfg->fs_uuid);
			ret = -EINVAL;
			goto out;
		}
		if (!test_uuid_unique(cfg->fs_uuid)) {
			error("non-unique UUID: %s", cfg->fs_uuid);
			ret = -EBUSY;
			goto out;
		}
	} else {
		uuid_generate(super.fsid);
		uuid_unparse(super.fsid, cfg->fs_uuid);
	}
	uuid_generate(super.dev_item.uuid);
	uuid_generate(chunk_tree_uuid);

	btrfs_set_super_bytenr(&super, cfg->blocks[0]);
	btrfs_set_super_num_devices(&super, 1);
	btrfs_set_super_magic(&super, BTRFS_MAGIC_PARTIAL);
	btrfs_set_super_generation(&super, 1);
	btrfs_set_super_root(&super, cfg->blocks[1]);
	btrfs_set_super_chunk_root(&super, cfg->blocks[3]);
	btrfs_set_super_total_bytes(&super, num_bytes);
	btrfs_set_super_bytes_used(&super, 6 * cfg->nodesize);
	btrfs_set_super_sectorsize(&super, cfg->sectorsize);
	btrfs_set_super_leafsize(&super, cfg->nodesize);
	btrfs_set_super_nodesize(&super, cfg->nodesize);
	btrfs_set_super_stripesize(&super, cfg->stripesize);
	btrfs_set_super_csum_type(&super, BTRFS_CSUM_TYPE_CRC32);
	btrfs_set_super_chunk_root_generation(&super, 1);
	btrfs_set_super_cache_generation(&super, -1);
	btrfs_set_super_incompat_flags(&super, cfg->features);
	if (cfg->label)
		__strncpy_null(super.label, cfg->label, BTRFS_LABEL_SIZE - 1);

	/* create the tree of root objects */
	memset(buf->data, 0, cfg->nodesize);
	buf->len = cfg->nodesize;
	btrfs_set_header_bytenr(buf, cfg->blocks[1]);
	btrfs_set_header_nritems(buf, 4);
	btrfs_set_header_generation(buf, 1);
	btrfs_set_header_backref_rev(buf, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(buf, BTRFS_ROOT_TREE_OBJECTID);
	write_extent_buffer(buf, super.fsid, btrfs_header_fsid(),
			    BTRFS_FSID_SIZE);

	write_extent_buffer(buf, chunk_tree_uuid,
			    btrfs_header_chunk_tree_uuid(buf),
			    BTRFS_UUID_SIZE);

	/* create the items for the root tree */
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

	memset(&disk_key, 0, sizeof(disk_key));
	btrfs_set_disk_key_type(&disk_key, BTRFS_ROOT_ITEM_KEY);
	btrfs_set_disk_key_offset(&disk_key, 0);
	nritems = 0;

	itemoff = __BTRFS_LEAF_DATA_SIZE(cfg->nodesize) - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, cfg->blocks[2]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(nritems),
			    sizeof(root_item));
	write_extent_buffer(buf, &root_item, btrfs_item_ptr_offset(buf,
			    nritems), sizeof(root_item));
	nritems++;

	itemoff = itemoff - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, cfg->blocks[4]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_DEV_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(nritems),
			    sizeof(root_item));
	write_extent_buffer(buf, &root_item,
			    btrfs_item_ptr_offset(buf, nritems),
			    sizeof(root_item));
	nritems++;

	itemoff = itemoff - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, cfg->blocks[5]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_FS_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(nritems),
			    sizeof(root_item));
	write_extent_buffer(buf, &root_item,
			    btrfs_item_ptr_offset(buf, nritems),
			    sizeof(root_item));
	nritems++;

	itemoff = itemoff - sizeof(root_item);
	btrfs_set_root_bytenr(&root_item, cfg->blocks[6]);
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_CSUM_TREE_OBJECTID);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(nritems),
			    sizeof(root_item));
	write_extent_buffer(buf, &root_item,
			    btrfs_item_ptr_offset(buf, nritems),
			    sizeof(root_item));
	nritems++;


	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[1]);
	if (ret != cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	/* create the items for the extent tree */
	memset(buf->data + sizeof(struct btrfs_header), 0,
		cfg->nodesize - sizeof(struct btrfs_header));
	nritems = 0;
	itemoff = __BTRFS_LEAF_DATA_SIZE(cfg->nodesize);
	for (i = 1; i < 7; i++) {
		item_size = sizeof(struct btrfs_extent_item);
		if (!skinny_metadata)
			item_size += sizeof(struct btrfs_tree_block_info);

		if (cfg->blocks[i] < first_free) {
			error("block[%d] below first free: %llu < %llu",
					i, (unsigned long long)cfg->blocks[i],
					(unsigned long long)first_free);
			ret = -EINVAL;
			goto out;
		}
		if (cfg->blocks[i] < cfg->blocks[i - 1]) {
			error("blocks %d and %d in reverse order: %llu < %llu",
				i, i - 1,
				(unsigned long long)cfg->blocks[i],
				(unsigned long long)cfg->blocks[i - 1]);
			ret = -EINVAL;
			goto out;
		}

		/* create extent item */
		itemoff -= item_size;
		btrfs_set_disk_key_objectid(&disk_key, cfg->blocks[i]);
		if (skinny_metadata) {
			btrfs_set_disk_key_type(&disk_key,
						BTRFS_METADATA_ITEM_KEY);
			btrfs_set_disk_key_offset(&disk_key, 0);
		} else {
			btrfs_set_disk_key_type(&disk_key,
						BTRFS_EXTENT_ITEM_KEY);
			btrfs_set_disk_key_offset(&disk_key, cfg->nodesize);
		}
		btrfs_set_item_key(buf, &disk_key, nritems);
		btrfs_set_item_offset(buf, btrfs_item_nr(nritems),
				      itemoff);
		btrfs_set_item_size(buf, btrfs_item_nr(nritems),
				    item_size);
		extent_item = btrfs_item_ptr(buf, nritems,
					     struct btrfs_extent_item);
		btrfs_set_extent_refs(buf, extent_item, 1);
		btrfs_set_extent_generation(buf, extent_item, 1);
		btrfs_set_extent_flags(buf, extent_item,
				       BTRFS_EXTENT_FLAG_TREE_BLOCK);
		nritems++;

		/* create extent ref */
		ref_root = reference_root_table[i];
		btrfs_set_disk_key_objectid(&disk_key, cfg->blocks[i]);
		btrfs_set_disk_key_offset(&disk_key, ref_root);
		btrfs_set_disk_key_type(&disk_key, BTRFS_TREE_BLOCK_REF_KEY);
		btrfs_set_item_key(buf, &disk_key, nritems);
		btrfs_set_item_offset(buf, btrfs_item_nr(nritems),
				      itemoff);
		btrfs_set_item_size(buf, btrfs_item_nr(nritems), 0);
		nritems++;
	}
	btrfs_set_header_bytenr(buf, cfg->blocks[2]);
	btrfs_set_header_owner(buf, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[2]);
	if (ret != cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	/* create the chunk tree */
	memset(buf->data + sizeof(struct btrfs_header), 0,
		cfg->nodesize - sizeof(struct btrfs_header));
	nritems = 0;
	item_size = sizeof(*dev_item);
	itemoff = __BTRFS_LEAF_DATA_SIZE(cfg->nodesize) - item_size;

	/* first device 1 (there is no device 0) */
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_DEV_ITEMS_OBJECTID);
	btrfs_set_disk_key_offset(&disk_key, 1);
	btrfs_set_disk_key_type(&disk_key, BTRFS_DEV_ITEM_KEY);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(nritems), item_size);

	dev_item = btrfs_item_ptr(buf, nritems, struct btrfs_dev_item);
	btrfs_set_device_id(buf, dev_item, 1);
	btrfs_set_device_generation(buf, dev_item, 0);
	btrfs_set_device_total_bytes(buf, dev_item, num_bytes);
	btrfs_set_device_bytes_used(buf, dev_item,
				    BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	btrfs_set_device_io_align(buf, dev_item, cfg->sectorsize);
	btrfs_set_device_io_width(buf, dev_item, cfg->sectorsize);
	btrfs_set_device_sector_size(buf, dev_item, cfg->sectorsize);
	btrfs_set_device_type(buf, dev_item, 0);

	write_extent_buffer(buf, super.dev_item.uuid,
			    (unsigned long)btrfs_device_uuid(dev_item),
			    BTRFS_UUID_SIZE);
	write_extent_buffer(buf, super.fsid,
			    (unsigned long)btrfs_device_fsid(dev_item),
			    BTRFS_UUID_SIZE);
	read_extent_buffer(buf, &super.dev_item, (unsigned long)dev_item,
			   sizeof(*dev_item));

	nritems++;
	item_size = btrfs_chunk_item_size(1);
	itemoff = itemoff - item_size;

	/* then we have chunk 0 */
	btrfs_set_disk_key_objectid(&disk_key, BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_disk_key_offset(&disk_key, 0);
	btrfs_set_disk_key_type(&disk_key, BTRFS_CHUNK_ITEM_KEY);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(nritems), item_size);

	chunk = btrfs_item_ptr(buf, nritems, struct btrfs_chunk);
	btrfs_set_chunk_length(buf, chunk, BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	btrfs_set_chunk_owner(buf, chunk, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_chunk_stripe_len(buf, chunk, BTRFS_STRIPE_LEN);
	btrfs_set_chunk_type(buf, chunk, BTRFS_BLOCK_GROUP_SYSTEM);
	btrfs_set_chunk_io_align(buf, chunk, cfg->sectorsize);
	btrfs_set_chunk_io_width(buf, chunk, cfg->sectorsize);
	btrfs_set_chunk_sector_size(buf, chunk, cfg->sectorsize);
	btrfs_set_chunk_num_stripes(buf, chunk, 1);
	btrfs_set_stripe_devid_nr(buf, chunk, 0, 1);
	btrfs_set_stripe_offset_nr(buf, chunk, 0, 0);
	nritems++;

	write_extent_buffer(buf, super.dev_item.uuid,
			    (unsigned long)btrfs_stripe_dev_uuid(&chunk->stripe),
			    BTRFS_UUID_SIZE);

	/* copy the key for the chunk to the system array */
	ptr = super.sys_chunk_array;
	array_size = sizeof(disk_key);

	memcpy(ptr, &disk_key, sizeof(disk_key));
	ptr += sizeof(disk_key);

	/* copy the chunk to the system array */
	read_extent_buffer(buf, ptr, (unsigned long)chunk, item_size);
	array_size += item_size;
	ptr += item_size;
	btrfs_set_super_sys_array_size(&super, array_size);

	btrfs_set_header_bytenr(buf, cfg->blocks[3]);
	btrfs_set_header_owner(buf, BTRFS_CHUNK_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[3]);
	if (ret != cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	/* create the device tree */
	memset(buf->data + sizeof(struct btrfs_header), 0,
		cfg->nodesize - sizeof(struct btrfs_header));
	nritems = 0;
	itemoff = __BTRFS_LEAF_DATA_SIZE(cfg->nodesize) -
		sizeof(struct btrfs_dev_extent);

	btrfs_set_disk_key_objectid(&disk_key, 1);
	btrfs_set_disk_key_offset(&disk_key, 0);
	btrfs_set_disk_key_type(&disk_key, BTRFS_DEV_EXTENT_KEY);
	btrfs_set_item_key(buf, &disk_key, nritems);
	btrfs_set_item_offset(buf, btrfs_item_nr(nritems), itemoff);
	btrfs_set_item_size(buf, btrfs_item_nr(nritems),
			    sizeof(struct btrfs_dev_extent));
	dev_extent = btrfs_item_ptr(buf, nritems, struct btrfs_dev_extent);
	btrfs_set_dev_extent_chunk_tree(buf, dev_extent,
					BTRFS_CHUNK_TREE_OBJECTID);
	btrfs_set_dev_extent_chunk_objectid(buf, dev_extent,
					BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_dev_extent_chunk_offset(buf, dev_extent, 0);

	write_extent_buffer(buf, chunk_tree_uuid,
		    (unsigned long)btrfs_dev_extent_chunk_tree_uuid(dev_extent),
		    BTRFS_UUID_SIZE);

	btrfs_set_dev_extent_length(buf, dev_extent,
				    BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	nritems++;

	btrfs_set_header_bytenr(buf, cfg->blocks[4]);
	btrfs_set_header_owner(buf, BTRFS_DEV_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[4]);
	if (ret != cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	/* create the FS root */
	memset(buf->data + sizeof(struct btrfs_header), 0,
		cfg->nodesize - sizeof(struct btrfs_header));
	btrfs_set_header_bytenr(buf, cfg->blocks[5]);
	btrfs_set_header_owner(buf, BTRFS_FS_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, 0);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[5]);
	if (ret != cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}
	/* finally create the csum root */
	memset(buf->data + sizeof(struct btrfs_header), 0,
		cfg->nodesize - sizeof(struct btrfs_header));
	btrfs_set_header_bytenr(buf, cfg->blocks[6]);
	btrfs_set_header_owner(buf, BTRFS_CSUM_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, 0);
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[6]);
	if (ret != cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	/* and write out the super block */
	memset(buf->data, 0, BTRFS_SUPER_INFO_SIZE);
	memcpy(buf->data, &super, sizeof(super));
	buf->len = BTRFS_SUPER_INFO_SIZE;
	csum_tree_block_size(buf, BTRFS_CRC32_SIZE, 0);
	ret = pwrite(fd, buf->data, BTRFS_SUPER_INFO_SIZE, cfg->blocks[0]);
	if (ret != BTRFS_SUPER_INFO_SIZE) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	ret = 0;

out:
	free(buf);
	return ret;
}

