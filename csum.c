/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "kerncompat.h"
#include "kernel-lib/bitops.h"
#include "ctree.h"
#include "utils.h"

/*
 * TODO:
 * 1) Add write support for csum
 *    So we can write new data extents and add csum into csum tree
 *
 * Get csums of range[@start, @start + len).
 *
 * @start:    Start offset, shall be aligned to sectorsize.
 * @len:      Length, shall be aligned to sectorsize.
 * @csum_ret: The size of csum_ret shall be @len / sectorsize * csum_size.
 * @bit_map:  Every bit corresponds to the offset have csum or not.
 *            The size in byte of bit_map should be
 *            calculate_bitmap_len(csum_ret's size / csum_size).
 *
 * Returns 0  means success
 * Returns >0 means on error
 * Returns <0 means on fatal error
 */

int btrfs_read_data_csums(struct btrfs_fs_info *fs_info, u64 start, u64 len,
			  void *csum_ret, unsigned long *bitmap_ret)

{
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_root *csum_root = fs_info->csum_root;
	u32 item_offset;
	u32 item_size;
	u32 final_offset;
	u32 final_len;
	u32 i;
	u32 sectorsize = fs_info->sectorsize;
	u16 csum_size = btrfs_super_csum_size(fs_info->super_copy);
	u64 cur_start;
	u64 cur_end;
	int found = 0;
	int ret;

	ASSERT(IS_ALIGNED(start, sectorsize));
	ASSERT(IS_ALIGNED(len, sectorsize));
	ASSERT(csum_ret);
	ASSERT(bitmap_ret);

	memset(bitmap_ret, 0, calculate_bitmap_len(len / sectorsize));
	btrfs_init_path(&path);

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.type = BTRFS_EXTENT_CSUM_KEY;
	key.offset = start;

	ret = btrfs_search_slot(NULL, csum_root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = btrfs_previous_item(csum_root, &path,
					  BTRFS_EXTENT_CSUM_OBJECTID,
					  BTRFS_EXTENT_CSUM_KEY);
		if (ret < 0)
			goto out;
	}
	/* The csum tree may be empty. */
	if (!btrfs_header_nritems(path.nodes[0]))
		goto next;

	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);

		if (!IS_ALIGNED(key.offset, sectorsize)) {
			error("csum item bytenr %llu is not aligned to %u",
			      key.offset, sectorsize);
			ret = -EIO;
			break;
		}
		/* exceeds end */
		if (key.offset >= start + len)
			break;

		item_offset = btrfs_item_ptr_offset(path.nodes[0],
						    path.slots[0]);
		item_size = btrfs_item_size_nr(path.nodes[0], path.slots[0]);

		if (key.offset + item_size / csum_size * sectorsize < start)
			goto next;

		/* get start of the extent */
		cur_start = max(start, key.offset);
		/* get end of the extent */
		cur_end = min(start + len, key.offset + item_size / csum_size *
			      sectorsize);

		final_offset = (cur_start - key.offset) / sectorsize *
			csum_size + item_offset;
		final_len = (cur_end - cur_start) / sectorsize * csum_size;
		read_extent_buffer(path.nodes[0],
				   (csum_ret + (cur_start - start) /
				    sectorsize * csum_size),
				   final_offset, final_len);

		for (i = 0; i != final_len / csum_size; i++)
			set_bit(i + (cur_start - start) / sectorsize,
				bitmap_ret);

		found = 1;
next:
		ret = btrfs_next_item(csum_root, &path);
		if (ret)
			break;
	}
out:
	if (ret >= 0)
		ret = !found;
	btrfs_release_path(&path);
	return ret;
}
