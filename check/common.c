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
#include "utils.h"
#include "disk-io.h"
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

static int get_highest_inode(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, struct btrfs_path *path,
			     u64 *highest_ino)
{
	struct btrfs_key key, found_key;
	int ret;

	btrfs_init_path(path);
	key.objectid = BTRFS_LAST_FREE_OBJECTID;
	key.offset = -1;
	key.type = BTRFS_INODE_ITEM_KEY;
	ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
	if (ret == 1) {
		btrfs_item_key_to_cpu(path->nodes[0], &found_key,
				path->slots[0] - 1);
		*highest_ino = found_key.objectid;
		ret = 0;
	}
	if (*highest_ino >= BTRFS_LAST_FREE_OBJECTID)
		ret = -EOVERFLOW;
	btrfs_release_path(path);
	return ret;
}

/*
 * Link inode to dir 'lost+found'. Increase @ref_count.
 *
 * Returns 0 means success.
 * Returns <0 means failure.
 */
int link_inode_to_lostfound(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    struct btrfs_path *path,
			    u64 ino, char *namebuf, u32 name_len,
			    u8 filetype, u64 *ref_count)
{
	char *dir_name = "lost+found";
	u64 lost_found_ino;
	int ret;
	u32 mode = 0700;

	btrfs_release_path(path);
	ret = get_highest_inode(trans, root, path, &lost_found_ino);
	if (ret < 0)
		goto out;
	lost_found_ino++;

	ret = btrfs_mkdir(trans, root, dir_name, strlen(dir_name),
			  BTRFS_FIRST_FREE_OBJECTID, &lost_found_ino,
			  mode);
	if (ret < 0) {
		error("failed to create '%s' dir: %s", dir_name, strerror(-ret));
		goto out;
	}
	ret = btrfs_add_link(trans, root, ino, lost_found_ino,
			     namebuf, name_len, filetype, NULL, 1, 0);
	/*
	 * Add ".INO" suffix several times to handle case where
	 * "FILENAME.INO" is already taken by another file.
	 */
	while (ret == -EEXIST) {
		/*
		 * Conflicting file name, add ".INO" as suffix * +1 for '.'
		 */
		if (name_len + count_digits(ino) + 1 > BTRFS_NAME_LEN) {
			ret = -EFBIG;
			goto out;
		}
		snprintf(namebuf + name_len, BTRFS_NAME_LEN - name_len,
			 ".%llu", ino);
		name_len += count_digits(ino) + 1;
		ret = btrfs_add_link(trans, root, ino, lost_found_ino, namebuf,
				     name_len, filetype, NULL, 1, 0);
	}
	if (ret < 0) {
		error("failed to link the inode %llu to %s dir: %s",
		      ino, dir_name, strerror(-ret));
		goto out;
	}

	++*ref_count;
	printf("Moving file '%.*s' to '%s' dir since it has no valid backref\n",
	       name_len, namebuf, dir_name);
out:
	btrfs_release_path(path);
	if (ret)
		error("failed to move file '%.*s' to '%s' dir", name_len,
				namebuf, dir_name);
	return ret;
}

/*
 * Extra (optional) check for dev_item size to report possbile problem on a new
 * kernel.
 */
void check_dev_size_alignment(u64 devid, u64 total_bytes, u32 sectorsize)
{
	if (!IS_ALIGNED(total_bytes, sectorsize)) {
		warning(
"unaligned total_bytes detected for devid %llu, have %llu should be aligned to %u",
			devid, total_bytes, sectorsize);
		warning(
"this is OK for older kernel, but may cause kernel warning for newer kernels");
		warning("this can be fixed by 'btrfs rescue fix-device-size'");
	}
}

void reada_walk_down(struct btrfs_root *root, struct extent_buffer *node,
		     int slot)
{
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 bytenr;
	u64 ptr_gen;
	u32 nritems;
	int i;
	int level;

	level = btrfs_header_level(node);
	if (level != 1)
		return;

	nritems = btrfs_header_nritems(node);
	for (i = slot; i < nritems; i++) {
		bytenr = btrfs_node_blockptr(node, i);
		ptr_gen = btrfs_node_ptr_generation(node, i);
		readahead_tree_block(fs_info, bytenr, ptr_gen);
	}
}

/*
 * Check the child node/leaf by the following condition:
 * 1. the first item key of the node/leaf should be the same with the one
 *    in parent.
 * 2. block in parent node should match the child node/leaf.
 * 3. generation of parent node and child's header should be consistent.
 *
 * Or the child node/leaf pointed by the key in parent is not valid.
 *
 * We hope to check leaf owner too, but since subvol may share leaves,
 * which makes leaf owner check not so strong, key check should be
 * sufficient enough for that case.
 */
int check_child_node(struct extent_buffer *parent, int slot,
		     struct extent_buffer *child)
{
	struct btrfs_key parent_key;
	struct btrfs_key child_key;
	int ret = 0;

	btrfs_node_key_to_cpu(parent, &parent_key, slot);
	if (btrfs_header_level(child) == 0)
		btrfs_item_key_to_cpu(child, &child_key, 0);
	else
		btrfs_node_key_to_cpu(child, &child_key, 0);

	if (memcmp(&parent_key, &child_key, sizeof(parent_key))) {
		ret = -EINVAL;
		fprintf(stderr,
			"Wrong key of child node/leaf, wanted: (%llu, %u, %llu), have: (%llu, %u, %llu)\n",
			parent_key.objectid, parent_key.type, parent_key.offset,
			child_key.objectid, child_key.type, child_key.offset);
	}
	if (btrfs_header_bytenr(child) != btrfs_node_blockptr(parent, slot)) {
		ret = -EINVAL;
		fprintf(stderr, "Wrong block of child node/leaf, wanted: %llu, have: %llu\n",
			btrfs_node_blockptr(parent, slot),
			btrfs_header_bytenr(child));
	}
	if (btrfs_node_ptr_generation(parent, slot) !=
	    btrfs_header_generation(child)) {
		ret = -EINVAL;
		fprintf(stderr, "Wrong generation of child node/leaf, wanted: %llu, have: %llu\n",
			btrfs_header_generation(child),
			btrfs_node_ptr_generation(parent, slot));
	}
	return ret;
}
