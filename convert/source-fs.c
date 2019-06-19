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
#include <unistd.h>
#include "common/internal.h"
#include "disk-io.h"
#include "volumes.h"
#include "convert/common.h"
#include "convert/source-fs.h"

const struct simple_range btrfs_reserved_ranges[3] = {
	{ 0,			     SZ_1M },
	{ BTRFS_SB_MIRROR_OFFSET(1), SZ_64K },
	{ BTRFS_SB_MIRROR_OFFSET(2), SZ_64K }
};

dev_t decode_dev(u32 dev)
{
	unsigned major = (dev & 0xfff00) >> 8;
	unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);

	return MKDEV(major, minor);
}

int ext2_acl_count(size_t size)
{
	ssize_t s;

	size -= sizeof(ext2_acl_header);
	s = size - 4 * sizeof(ext2_acl_entry_short);
	if (s < 0) {
		if (size % sizeof(ext2_acl_entry_short))
			return -1;
		return size / sizeof(ext2_acl_entry_short);
	} else {
		if (s % sizeof(ext2_acl_entry))
			return -1;
		return s / sizeof(ext2_acl_entry) + 4;
	}
}

static u64 intersect_with_reserved(u64 bytenr, u64 num_bytes)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(btrfs_reserved_ranges); i++) {
		const struct simple_range *range = &btrfs_reserved_ranges[i];

		if (bytenr < range_end(range) &&
		    bytenr + num_bytes >= range->start)
			return range_end(range);
	}
	return 0;
}

void init_convert_context(struct btrfs_convert_context *cctx)
{
	memset(cctx, 0, sizeof(*cctx));

	cache_tree_init(&cctx->used_space);
	cache_tree_init(&cctx->data_chunks);
	cache_tree_init(&cctx->free_space);
}

void clean_convert_context(struct btrfs_convert_context *cctx)
{
	free_extent_cache_tree(&cctx->used_space);
	free_extent_cache_tree(&cctx->data_chunks);
	free_extent_cache_tree(&cctx->free_space);
}

int block_iterate_proc(u64 disk_block, u64 file_block,
		              struct blk_iterate_data *idata)
{
	int ret = 0;
	u64 reserved_boundary;
	int do_barrier;
	struct btrfs_root *root = idata->root;
	struct btrfs_block_group_cache *cache;
	u32 sectorsize = root->fs_info->sectorsize;
	u64 bytenr = disk_block * sectorsize;

	reserved_boundary = intersect_with_reserved(bytenr, sectorsize);
	do_barrier = reserved_boundary || disk_block >= idata->boundary;
	if ((idata->num_blocks > 0 && do_barrier) ||
	    (file_block > idata->first_block + idata->num_blocks) ||
	    (disk_block != idata->disk_block + idata->num_blocks)) {
		if (idata->num_blocks > 0) {
			ret = record_file_blocks(idata, idata->first_block,
						 idata->disk_block,
						 idata->num_blocks);
			if (ret)
				goto fail;
			idata->first_block += idata->num_blocks;
			idata->num_blocks = 0;
		}
		if (file_block > idata->first_block) {
			ret = record_file_blocks(idata, idata->first_block,
					0, file_block - idata->first_block);
			if (ret)
				goto fail;
		}

		if (reserved_boundary) {
			bytenr = reserved_boundary;
		} else {
			cache = btrfs_lookup_block_group(root->fs_info, bytenr);
			BUG_ON(!cache);
			bytenr = cache->key.objectid + cache->key.offset;
		}

		idata->first_block = file_block;
		idata->disk_block = disk_block;
		idata->boundary = bytenr / sectorsize;
	}
	idata->num_blocks++;
fail:
	return ret;
}

void init_blk_iterate_data(struct blk_iterate_data *data,
				  struct btrfs_trans_handle *trans,
				  struct btrfs_root *root,
				  struct btrfs_inode_item *inode,
				  u64 objectid, int checksum)
{
	struct btrfs_key key;

	data->trans		= trans;
	data->root		= root;
	data->inode		= inode;
	data->objectid		= objectid;
	data->first_block	= 0;
	data->disk_block	= 0;
	data->num_blocks	= 0;
	data->boundary		= (u64)-1;
	data->checksum		= checksum;
	data->errcode		= 0;

	key.objectid = CONV_IMAGE_SUBVOL_OBJECTID;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;
	data->convert_root = btrfs_read_fs_root(root->fs_info, &key);
	/* Impossible as we just opened it before */
	BUG_ON(!data->convert_root || IS_ERR(data->convert_root));
	data->convert_ino = BTRFS_FIRST_FREE_OBJECTID + 1;
}

int convert_insert_dirent(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 const char *name, size_t name_len,
				 u64 dir, u64 objectid,
				 u8 file_type, u64 index_cnt,
				 struct btrfs_inode_item *inode)
{
	int ret;
	u64 inode_size;
	struct btrfs_key location = {
		.objectid = objectid,
		.offset = 0,
		.type = BTRFS_INODE_ITEM_KEY,
	};

	ret = btrfs_insert_dir_item(trans, root, name, name_len,
				    dir, &location, file_type, index_cnt);
	if (ret)
		return ret;
	ret = btrfs_insert_inode_ref(trans, root, name, name_len,
				     objectid, dir, index_cnt);
	if (ret)
		return ret;
	inode_size = btrfs_stack_inode_size(inode) + name_len * 2;
	btrfs_set_stack_inode_size(inode, inode_size);

	return 0;
}

int read_disk_extent(struct btrfs_root *root, u64 bytenr,
		            u32 num_bytes, char *buffer)
{
	int ret;
	struct btrfs_fs_devices *fs_devs = root->fs_info->fs_devices;

	ret = pread(fs_devs->latest_bdev, buffer, num_bytes, bytenr);
	if (ret != num_bytes)
		goto fail;
	ret = 0;
fail:
	if (ret > 0)
		ret = -EIO;
	return ret;
}

/*
 * Record a file extent in original filesystem into btrfs one.
 * The special point is, old disk_block can point to a reserved range.
 * So here, we don't use disk_block directly but search convert_root
 * to get the real disk_bytenr.
 */
int record_file_blocks(struct blk_iterate_data *data,
			      u64 file_block, u64 disk_block, u64 num_blocks)
{
	int ret = 0;
	struct btrfs_root *root = data->root;
	struct btrfs_root *convert_root = data->convert_root;
	struct btrfs_path path;
	u32 sectorsize = root->fs_info->sectorsize;
	u64 file_pos = file_block * sectorsize;
	u64 old_disk_bytenr = disk_block * sectorsize;
	u64 num_bytes = num_blocks * sectorsize;
	u64 cur_off = old_disk_bytenr;

	/* Hole, pass it to record_file_extent directly */
	if (old_disk_bytenr == 0)
		return btrfs_record_file_extent(data->trans, root,
				data->objectid, data->inode, file_pos, 0,
				num_bytes);

	btrfs_init_path(&path);

	/*
	 * Search real disk bytenr from convert root
	 */
	while (cur_off < old_disk_bytenr + num_bytes) {
		struct btrfs_key key;
		struct btrfs_file_extent_item *fi;
		struct extent_buffer *node;
		int slot;
		u64 extent_disk_bytenr;
		u64 extent_num_bytes;
		u64 real_disk_bytenr;
		u64 cur_len;

		key.objectid = data->convert_ino;
		key.type = BTRFS_EXTENT_DATA_KEY;
		key.offset = cur_off;

		ret = btrfs_search_slot(NULL, convert_root, &key, &path, 0, 0);
		if (ret < 0)
			break;
		if (ret > 0) {
			ret = btrfs_previous_item(convert_root, &path,
						  data->convert_ino,
						  BTRFS_EXTENT_DATA_KEY);
			if (ret < 0)
				break;
			if (ret > 0) {
				ret = -ENOENT;
				break;
			}
		}
		node = path.nodes[0];
		slot = path.slots[0];
		btrfs_item_key_to_cpu(node, &key, slot);
		BUG_ON(key.type != BTRFS_EXTENT_DATA_KEY ||
		       key.objectid != data->convert_ino ||
		       key.offset > cur_off);
		fi = btrfs_item_ptr(node, slot, struct btrfs_file_extent_item);
		extent_disk_bytenr = btrfs_file_extent_disk_bytenr(node, fi);
		extent_num_bytes = btrfs_file_extent_num_bytes(node, fi);
		BUG_ON(cur_off - key.offset >= extent_num_bytes);
		btrfs_release_path(&path);

		if (extent_disk_bytenr)
			real_disk_bytenr = cur_off - key.offset +
					   extent_disk_bytenr;
		else
			real_disk_bytenr = 0;
		cur_len = min(key.offset + extent_num_bytes,
			      old_disk_bytenr + num_bytes) - cur_off;
		ret = btrfs_record_file_extent(data->trans, data->root,
					data->objectid, data->inode, file_pos,
					real_disk_bytenr, cur_len);
		if (ret < 0)
			break;
		cur_off += cur_len;
		file_pos += cur_len;

		/*
		 * No need to care about csum
		 * As every byte of old fs image is calculated for csum, no
		 * need to waste CPU cycles now.
		 */
	}
	btrfs_release_path(&path);
	return ret;
}

