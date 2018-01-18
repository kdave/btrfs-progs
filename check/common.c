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

#include <time.h>
#include "ctree.h"
#include "internal.h"
#include "messages.h"
#include "transaction.h"
#include "check/common.h"

/*
 * Search in csum tree to find how many bytes of range [@start, @start + @len)
 * has the corresponding csum item.
 *
 * @start:	range start
 * @len:	range length
 * @found:	return value of found csum bytes
 *		unit is BYTE.
 */
int count_csum_range(struct btrfs_fs_info *fs_info, u64 start,
		     u64 len, u64 *found)
{
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	int ret;
	size_t size;
	*found = 0;
	u64 csum_end;
	u16 csum_size = btrfs_super_csum_size(fs_info->super_copy);

	btrfs_init_path(&path);

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.offset = start;
	key.type = BTRFS_EXTENT_CSUM_KEY;

	ret = btrfs_search_slot(NULL, fs_info->csum_root,
				&key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0 && path.slots[0] > 0) {
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0] - 1);
		if (key.objectid == BTRFS_EXTENT_CSUM_OBJECTID &&
		    key.type == BTRFS_EXTENT_CSUM_KEY)
			path.slots[0]--;
	}

	while (len > 0) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(fs_info->csum_root, &path);
			if (ret > 0)
				break;
			else if (ret < 0)
				goto out;
			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
		    key.type != BTRFS_EXTENT_CSUM_KEY)
			break;

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.offset >= start + len)
			break;

		if (key.offset > start)
			start = key.offset;

		size = btrfs_item_size_nr(leaf, path.slots[0]);
		csum_end = key.offset + (size / csum_size) *
			   fs_info->sectorsize;
		if (csum_end > start) {
			size = min(csum_end - start, len);
			len -= size;
			start += size;
			*found += size;
		}

		path.slots[0]++;
	}
out:
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;
	return 0;
}

/*
 * Wrapper to insert one inode item into given @root
 * Timestamp will be set to current time.
 *
 * @root:	the root to insert inode item into
 * @ino:	inode number
 * @size:	inode size
 * @nbytes:	nbytes (real used size, without hole)
 * @nlink:	number of links
 * @mode:	file mode, including S_IF* bits
 */
int insert_inode_item(struct btrfs_trans_handle *trans,
		      struct btrfs_root *root, u64 ino, u64 size,
		      u64 nbytes, u64 nlink, u32 mode)
{
	struct btrfs_inode_item ii;
	time_t now = time(NULL);
	int ret;

	btrfs_set_stack_inode_size(&ii, size);
	btrfs_set_stack_inode_nbytes(&ii, nbytes);
	btrfs_set_stack_inode_nlink(&ii, nlink);
	btrfs_set_stack_inode_mode(&ii, mode);
	btrfs_set_stack_inode_generation(&ii, trans->transid);
	btrfs_set_stack_timespec_nsec(&ii.atime, 0);
	btrfs_set_stack_timespec_sec(&ii.ctime, now);
	btrfs_set_stack_timespec_nsec(&ii.ctime, 0);
	btrfs_set_stack_timespec_sec(&ii.mtime, now);
	btrfs_set_stack_timespec_nsec(&ii.mtime, 0);
	btrfs_set_stack_timespec_sec(&ii.otime, 0);
	btrfs_set_stack_timespec_nsec(&ii.otime, 0);

	ret = btrfs_insert_inode(trans, root, ino, &ii);
	ASSERT(!ret);

	warning("root %llu inode %llu recreating inode item, this may "
		"be incomplete, please check permissions and content after "
		"the fsck completes.\n", (unsigned long long)root->objectid,
		(unsigned long long)ino);

	return 0;
}
