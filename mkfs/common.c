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
#include <blkid/blkid.h>
#include <fcntl.h>
#include <limits.h>
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "common/utils.h"
#include "common/path-utils.h"
#include "common/device-utils.h"
#include "mkfs/common.h"

static u64 reference_root_table[] = {
	[1] =	BTRFS_ROOT_TREE_OBJECTID,
	[2] =	BTRFS_EXTENT_TREE_OBJECTID,
	[3] =	BTRFS_CHUNK_TREE_OBJECTID,
	[4] =	BTRFS_DEV_TREE_OBJECTID,
	[5] =	BTRFS_FS_TREE_OBJECTID,
	[6] =	BTRFS_CSUM_TREE_OBJECTID,
};

static int btrfs_create_tree_root(int fd, struct btrfs_mkfs_config *cfg,
			struct extent_buffer *buf)
{
	struct btrfs_root_item root_item;
	struct btrfs_inode_item *inode_item;
	struct btrfs_disk_key disk_key;
	u32 nritems = 0;
	u32 itemoff;
	int ret = 0;
	int blk;
	u8 uuid[BTRFS_UUID_SIZE];

	memset(buf->data + sizeof(struct btrfs_header), 0,
		cfg->nodesize - sizeof(struct btrfs_header));
	memset(&root_item, 0, sizeof(root_item));
	memset(&disk_key, 0, sizeof(disk_key));

	/* create the items for the root tree */
	inode_item = &root_item.inode;
	btrfs_set_stack_inode_generation(inode_item, 1);
	btrfs_set_stack_inode_size(inode_item, 3);
	btrfs_set_stack_inode_nlink(inode_item, 1);
	btrfs_set_stack_inode_nbytes(inode_item, cfg->nodesize);
	btrfs_set_stack_inode_mode(inode_item, S_IFDIR | 0755);
	btrfs_set_root_refs(&root_item, 1);
	btrfs_set_root_used(&root_item, cfg->nodesize);
	btrfs_set_root_generation(&root_item, 1);

	btrfs_set_disk_key_type(&disk_key, BTRFS_ROOT_ITEM_KEY);
	btrfs_set_disk_key_offset(&disk_key, 0);
	itemoff = __BTRFS_LEAF_DATA_SIZE(cfg->nodesize) - sizeof(root_item);

	for (blk = 0; blk < MKFS_BLOCK_COUNT; blk++) {
		if (blk == MKFS_SUPER_BLOCK || blk == MKFS_ROOT_TREE
		    || blk == MKFS_CHUNK_TREE)
			continue;

		btrfs_set_root_bytenr(&root_item, cfg->blocks[blk]);
		btrfs_set_disk_key_objectid(&disk_key,
			reference_root_table[blk]);
		btrfs_set_item_key(buf, &disk_key, nritems);
		btrfs_set_item_offset(buf, btrfs_item_nr(nritems), itemoff);
		btrfs_set_item_size(buf, btrfs_item_nr(nritems),
				sizeof(root_item));
		if (blk == MKFS_FS_TREE) {
			time_t now = time(NULL);

			uuid_generate(uuid);
			memcpy(root_item.uuid, uuid, BTRFS_UUID_SIZE);
			btrfs_set_stack_timespec_sec(&root_item.otime, now);
			btrfs_set_stack_timespec_sec(&root_item.ctime, now);
		} else {
			memset(uuid, 0, BTRFS_UUID_SIZE);
			memcpy(root_item.uuid, uuid, BTRFS_UUID_SIZE);
			btrfs_set_stack_timespec_sec(&root_item.otime, 0);
			btrfs_set_stack_timespec_sec(&root_item.ctime, 0);
		}
		write_extent_buffer(buf, &root_item,
			btrfs_item_ptr_offset(buf, nritems),
			sizeof(root_item));
		nritems++;
		itemoff -= sizeof(root_item);
	}

	/* generate checksum */
	csum_tree_block_size(buf, btrfs_csum_type_size(cfg->csum_type), 0,
			     cfg->csum_type);

	/* write back root tree */
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[MKFS_ROOT_TREE]);
	if (ret != cfg->nodesize)
		return (ret < 0 ? -errno : -EIO);

	return ret;
}

/*
 * @fs_uuid - if NULL, generates a UUID, returns back the new filesystem UUID
 *
 * The superblock signature is not valid, denotes a partially created
 * filesystem, needs to be finalized.
 *
 * The temporary fs will have the following chunk layout:
 * Device extent:
 * 0		1M				5M	......
 * | Reserved	| dev extent for SYS chunk      |
 *
 * And chunk mapping will be:
 * Chunk mapping:
 * 0		1M				5M
 * |		| System chunk, 1:1 mapped	|
 *
 * That's to say, there will only be *ONE* system chunk, mapped to
 * [1M, 5M) physical offset.
 * And the only chunk is also in logical address [1M, 5M), containing
 * all essential tree blocks.
 */
int make_btrfs(int fd, struct btrfs_mkfs_config *cfg)
{
	struct btrfs_super_block super;
	struct extent_buffer *buf;
	struct btrfs_disk_key disk_key;
	struct btrfs_extent_item *extent_item;
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

	cfg->blocks[MKFS_SUPER_BLOCK] = BTRFS_SUPER_INFO_OFFSET;
	for (i = 1; i < MKFS_BLOCK_COUNT; i++) {
		cfg->blocks[i] = BTRFS_BLOCK_RESERVED_1M_FOR_SUPER +
			cfg->nodesize * (i - 1);
	}

	btrfs_set_super_bytenr(&super, cfg->blocks[MKFS_SUPER_BLOCK]);
	btrfs_set_super_num_devices(&super, 1);
	btrfs_set_super_magic(&super, BTRFS_MAGIC_TEMPORARY);
	btrfs_set_super_generation(&super, 1);
	btrfs_set_super_root(&super, cfg->blocks[MKFS_ROOT_TREE]);
	btrfs_set_super_chunk_root(&super, cfg->blocks[MKFS_CHUNK_TREE]);
	btrfs_set_super_total_bytes(&super, num_bytes);
	btrfs_set_super_bytes_used(&super, 6 * cfg->nodesize);
	btrfs_set_super_sectorsize(&super, cfg->sectorsize);
	super.__unused_leafsize = cpu_to_le32(cfg->nodesize);
	btrfs_set_super_nodesize(&super, cfg->nodesize);
	btrfs_set_super_stripesize(&super, cfg->stripesize);
	btrfs_set_super_csum_type(&super, cfg->csum_type);
	btrfs_set_super_chunk_root_generation(&super, 1);
	btrfs_set_super_cache_generation(&super, -1);
	btrfs_set_super_incompat_flags(&super, cfg->features);
	if (cfg->label)
		__strncpy_null(super.label, cfg->label, BTRFS_LABEL_SIZE - 1);

	/* create the tree of root objects */
	memset(buf->data, 0, cfg->nodesize);
	buf->len = cfg->nodesize;
	btrfs_set_header_bytenr(buf, cfg->blocks[MKFS_ROOT_TREE]);
	btrfs_set_header_nritems(buf, 4);
	btrfs_set_header_generation(buf, 1);
	btrfs_set_header_backref_rev(buf, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(buf, BTRFS_ROOT_TREE_OBJECTID);
	write_extent_buffer(buf, super.fsid, btrfs_header_fsid(),
			    BTRFS_FSID_SIZE);

	write_extent_buffer(buf, chunk_tree_uuid,
			    btrfs_header_chunk_tree_uuid(buf),
			    BTRFS_UUID_SIZE);

	ret = btrfs_create_tree_root(fd, cfg, buf);
	if (ret < 0)
		goto out;

	/* create the items for the extent tree */
	memset(buf->data + sizeof(struct btrfs_header), 0,
		cfg->nodesize - sizeof(struct btrfs_header));
	nritems = 0;
	itemoff = __BTRFS_LEAF_DATA_SIZE(cfg->nodesize);
	for (i = 1; i < MKFS_BLOCK_COUNT; i++) {
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
	btrfs_set_header_bytenr(buf, cfg->blocks[MKFS_EXTENT_TREE]);
	btrfs_set_header_owner(buf, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	csum_tree_block_size(buf, btrfs_csum_type_size(cfg->csum_type), 0,
			     cfg->csum_type);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[MKFS_EXTENT_TREE]);
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
	btrfs_set_disk_key_offset(&disk_key, BTRFS_BLOCK_RESERVED_1M_FOR_SUPER);
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
	btrfs_set_stripe_offset_nr(buf, chunk, 0,
				   BTRFS_BLOCK_RESERVED_1M_FOR_SUPER);
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

	btrfs_set_header_bytenr(buf, cfg->blocks[MKFS_CHUNK_TREE]);
	btrfs_set_header_owner(buf, BTRFS_CHUNK_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	csum_tree_block_size(buf, btrfs_csum_type_size(cfg->csum_type), 0,
			     cfg->csum_type);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[MKFS_CHUNK_TREE]);
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
	btrfs_set_disk_key_offset(&disk_key, BTRFS_BLOCK_RESERVED_1M_FOR_SUPER);
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
	btrfs_set_dev_extent_chunk_offset(buf, dev_extent,
					  BTRFS_BLOCK_RESERVED_1M_FOR_SUPER);

	write_extent_buffer(buf, chunk_tree_uuid,
		    (unsigned long)btrfs_dev_extent_chunk_tree_uuid(dev_extent),
		    BTRFS_UUID_SIZE);

	btrfs_set_dev_extent_length(buf, dev_extent,
				    BTRFS_MKFS_SYSTEM_GROUP_SIZE);
	nritems++;

	btrfs_set_header_bytenr(buf, cfg->blocks[MKFS_DEV_TREE]);
	btrfs_set_header_owner(buf, BTRFS_DEV_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, nritems);
	csum_tree_block_size(buf, btrfs_csum_type_size(cfg->csum_type), 0,
			     cfg->csum_type);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[MKFS_DEV_TREE]);
	if (ret != cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	/* create the FS root */
	memset(buf->data + sizeof(struct btrfs_header), 0,
		cfg->nodesize - sizeof(struct btrfs_header));
	btrfs_set_header_bytenr(buf, cfg->blocks[MKFS_FS_TREE]);
	btrfs_set_header_owner(buf, BTRFS_FS_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, 0);
	csum_tree_block_size(buf, btrfs_csum_type_size(cfg->csum_type), 0,
			     cfg->csum_type);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[MKFS_FS_TREE]);
	if (ret != cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}
	/* finally create the csum root */
	memset(buf->data + sizeof(struct btrfs_header), 0,
		cfg->nodesize - sizeof(struct btrfs_header));
	btrfs_set_header_bytenr(buf, cfg->blocks[MKFS_CSUM_TREE]);
	btrfs_set_header_owner(buf, BTRFS_CSUM_TREE_OBJECTID);
	btrfs_set_header_nritems(buf, 0);
	csum_tree_block_size(buf, btrfs_csum_type_size(cfg->csum_type), 0,
			     cfg->csum_type);
	ret = pwrite(fd, buf->data, cfg->nodesize, cfg->blocks[MKFS_CSUM_TREE]);
	if (ret != cfg->nodesize) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	/* and write out the super block */
	memset(buf->data, 0, BTRFS_SUPER_INFO_SIZE);
	memcpy(buf->data, &super, sizeof(super));
	buf->len = BTRFS_SUPER_INFO_SIZE;
	csum_tree_block_size(buf, btrfs_csum_type_size(cfg->csum_type), 0,
			     cfg->csum_type);
	ret = pwrite(fd, buf->data, BTRFS_SUPER_INFO_SIZE,
			cfg->blocks[MKFS_SUPER_BLOCK]);
	if (ret != BTRFS_SUPER_INFO_SIZE) {
		ret = (ret < 0 ? -errno : -EIO);
		goto out;
	}

	ret = 0;

out:
	free(buf);
	return ret;
}

/*
 * Btrfs minimum size calculation is complicated, it should include at least:
 * 1. system group size
 * 2. minimum global block reserve
 * 3. metadata used at mkfs
 * 4. space reservation to create uuid for first mount.
 * Also, raid factor should also be taken into consideration.
 * To avoid the overkill calculation, (system group + global block rsv) * 2
 * for *EACH* device should be good enough.
 */
static u64 btrfs_min_global_blk_rsv_size(u32 nodesize)
{
	return (u64)nodesize << 10;
}

u64 btrfs_min_dev_size(u32 nodesize, int mixed, u64 meta_profile,
		       u64 data_profile)
{
	u64 reserved = 0;
	u64 meta_size;
	u64 data_size;

	if (mixed)
		return 2 * (BTRFS_MKFS_SYSTEM_GROUP_SIZE +
			    btrfs_min_global_blk_rsv_size(nodesize));

	/*
	 * Minimal size calculation is complex due to several factors:
	 * 0) Reserved 1M range.
	 *
	 * 1) Temporary chunk reuse
	 *    If specified chunk profile is SINGLE, we can reuse
	 *    temporary chunks, no need to allocate new chunks.
	 *
	 * 2) Different minimal chunk size for different profiles:
	 *    For initial sys chunk, chunk size is fixed to 4M.
	 *    For single profile, minimal chunk size is 8M for all.
	 *    For other profiles, minimal chunk and stripe size ranges from 8M
	 *    to 64M.
	 *
	 * To calculate it a little easier, here we assume we don't reuse any
	 * temporary chunk, and calculate the size completely by ourselves.
	 *
	 * Temporary chunks sizes are always fixed:
	 * One initial sys chunk, one SINGLE meta, and one SINGLE data.
	 * The latter two are all 8M, according to @calc_size of
	 * btrfs_alloc_chunk().
	 */
	reserved += BTRFS_BLOCK_RESERVED_1M_FOR_SUPER +
		    BTRFS_MKFS_SYSTEM_GROUP_SIZE + SZ_8M * 2;

	/*
	 * For real chunks, we need to select different sizes:
	 * For SINGLE, it's still fixed to 8M (@calc_size).
	 * For other profiles, refer to max(@min_stripe_size, @calc_size).
	 *
	 * And use the stripe size to calculate its physical used space.
	 */
	if (meta_profile & BTRFS_BLOCK_GROUP_PROFILE_MASK)
		meta_size = SZ_8M + SZ_32M;
	else
		meta_size = SZ_8M + SZ_8M;
	/* For DUP/metadata,  2 stripes on one disk */
	if (meta_profile & BTRFS_BLOCK_GROUP_DUP)
		meta_size *= 2;
	reserved += meta_size;

	if (data_profile & BTRFS_BLOCK_GROUP_PROFILE_MASK)
		data_size = SZ_64M;
	else
		data_size = SZ_8M;
	/* For DUP/data,  2 stripes on one disk */
	if (data_profile & BTRFS_BLOCK_GROUP_DUP)
		data_size *= 2;
	reserved += data_size;

	return reserved;
}

#define isoctal(c)	(((c) & ~7) == '0')

static inline void translate(char *f, char *t)
{
	while (*f != '\0') {
		if (*f == '\\' &&
		    isoctal(f[1]) && isoctal(f[2]) && isoctal(f[3])) {
			*t++ = 64*(f[1] & 7) + 8*(f[2] & 7) + (f[3] & 7);
			f += 4;
		} else
			*t++ = *f++;
	}
	*t = '\0';
	return;
}

/*
 * Checks if the swap device.
 * Returns 1 if swap device, < 0 on error or 0 if not swap device.
 */
static int is_swap_device(const char *file)
{
	FILE	*f;
	struct stat	st_buf;
	dev_t	dev;
	ino_t	ino = 0;
	char	tmp[PATH_MAX];
	char	buf[PATH_MAX];
	char	*cp;
	int	ret = 0;

	if (stat(file, &st_buf) < 0)
		return -errno;
	if (S_ISBLK(st_buf.st_mode))
		dev = st_buf.st_rdev;
	else if (S_ISREG(st_buf.st_mode)) {
		dev = st_buf.st_dev;
		ino = st_buf.st_ino;
	} else
		return 0;

	if ((f = fopen("/proc/swaps", "r")) == NULL)
		return 0;

	/* skip the first line */
	if (fgets(tmp, sizeof(tmp), f) == NULL)
		goto out;

	while (fgets(tmp, sizeof(tmp), f) != NULL) {
		if ((cp = strchr(tmp, ' ')) != NULL)
			*cp = '\0';
		if ((cp = strchr(tmp, '\t')) != NULL)
			*cp = '\0';
		translate(tmp, buf);
		if (stat(buf, &st_buf) != 0)
			continue;
		if (S_ISBLK(st_buf.st_mode)) {
			if (dev == st_buf.st_rdev) {
				ret = 1;
				break;
			}
		} else if (S_ISREG(st_buf.st_mode)) {
			if (dev == st_buf.st_dev && ino == st_buf.st_ino) {
				ret = 1;
				break;
			}
		}
	}

out:
	fclose(f);

	return ret;
}

/*
 * Check for existing filesystem or partition table on device.
 * Returns:
 *	 1 for existing fs or partition
 *	 0 for nothing found
 *	-1 for internal error
 */
static int check_overwrite(const char *device)
{
	const char	*type;
	blkid_probe	pr = NULL;
	int		ret;
	blkid_loff_t	size;

	if (!device || !*device)
		return 0;

	ret = -1; /* will reset on success of all setup calls */

	pr = blkid_new_probe_from_filename(device);
	if (!pr)
		goto out;

	size = blkid_probe_get_size(pr);
	if (size < 0)
		goto out;

	/* nothing to overwrite on a 0-length device */
	if (size == 0) {
		ret = 0;
		goto out;
	}

	ret = blkid_probe_enable_partitions(pr, 1);
	if (ret < 0)
		goto out;

	ret = blkid_do_fullprobe(pr);
	if (ret < 0)
		goto out;

	/*
	 * Blkid returns 1 for nothing found and 0 when it finds a signature,
	 * but we want the exact opposite, so reverse the return value here.
	 *
	 * In addition print some useful diagnostics about what actually is
	 * on the device.
	 */
	if (ret) {
		ret = 0;
		goto out;
	}

	if (!blkid_probe_lookup_value(pr, "TYPE", &type, NULL)) {
		fprintf(stderr,
			"%s appears to contain an existing "
			"filesystem (%s).\n", device, type);
	} else if (!blkid_probe_lookup_value(pr, "PTTYPE", &type, NULL)) {
		fprintf(stderr,
			"%s appears to contain a partition "
			"table (%s).\n", device, type);
	} else {
		fprintf(stderr,
			"%s appears to contain something weird "
			"according to blkid\n", device);
	}
	ret = 1;

out:
	if (pr)
		blkid_free_probe(pr);
	if (ret == -1)
		fprintf(stderr,
			"probe of %s failed, cannot detect "
			  "existing filesystem.\n", device);
	return ret;
}

/*
 * Check if a device is suitable for btrfs
 * returns:
 *  1: something is wrong, an error is printed
 *  0: all is fine
 */
int test_dev_for_mkfs(const char *file, int force_overwrite)
{
	int ret, fd;
	struct stat st;

	ret = is_swap_device(file);
	if (ret < 0) {
		errno = -ret;
		error("checking status of %s: %m", file);
		return 1;
	}
	if (ret == 1) {
		error("%s is a swap device", file);
		return 1;
	}
	ret = test_status_for_mkfs(file, force_overwrite);
	if (ret)
		return 1;
	/* check if the device is busy */
	fd = open(file, O_RDWR|O_EXCL);
	if (fd < 0) {
		error("unable to open %s: %m", file);
		return 1;
	}
	if (fstat(fd, &st)) {
		error("unable to stat %s: %m", file);
		close(fd);
		return 1;
	}
	if (!S_ISBLK(st.st_mode)) {
		error("%s is not a block device", file);
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

/*
 * check if the file (device) is formatted or mounted
 */
int test_status_for_mkfs(const char *file, bool force_overwrite)
{
	int ret;

	if (!force_overwrite) {
		if (check_overwrite(file)) {
			error("use the -f option to force overwrite of %s",
					file);
			return 1;
		}
	}
	ret = check_mounted(file);
	if (ret < 0) {
		errno = -ret;
		error("cannot check mount status of %s: %m", file);
		return 1;
	}
	if (ret == 1) {
		error("%s is mounted", file);
		return 1;
	}

	return 0;
}

int is_vol_small(const char *file)
{
	int fd = -1;
	int e;
	struct stat st;
	u64 size;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0) {
		e = -errno;
		close(fd);
		return e;
	}
	size = btrfs_device_size(fd, &st);
	if (size == 0) {
		close(fd);
		return -1;
	}
	if (size < BTRFS_MKFS_SMALL_VOLUME_SIZE) {
		close(fd);
		return 1;
	} else {
		close(fd);
		return 0;
	}
}

int test_minimum_size(const char *file, u64 min_dev_size)
{
	int fd;
	struct stat statbuf;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return -errno;
	if (stat(file, &statbuf) < 0) {
		close(fd);
		return -errno;
	}
	if (btrfs_device_size(fd, &statbuf) < min_dev_size) {
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}


