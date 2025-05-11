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
#include "kernel-shared/disk-io.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/backref.h"
#include "common/messages.h"
#include "common/open-utils.h"
#include "cmds/rescue.h"

/*
 * Record one corrupted data blocks.
 *
 * We do not report immediately, this is for future file deleting support.
 */
struct corrupted_block {
	struct list_head list;
	/* The logical bytenr of the exact corrupted block. */
	u64 logical;

	/* The amount of mirrors above logical have. */
	unsigned int num_mirrors;

	/*
	 * Which mirror failed.
	 *
	 * Note, bit 0 means mirror 1, since mirror 0 means choosing a
	 * live mirror, and we never utilized that mirror 0.
	 */
	unsigned long *error_mirror_bitmap;
};

static int global_repair_mode;
LIST_HEAD(corrupted_blocks);

static int add_corrupted_block(struct btrfs_fs_info *fs_info, u64 logical,
			       unsigned int mirror, unsigned int num_mirrors)
{
	struct corrupted_block *last;
	if (list_empty(&corrupted_blocks))
		goto add;

	last = list_entry(corrupted_blocks.prev, struct corrupted_block, list);
	/* The last entry is the same, just set update the error mirror bitmap. */
	if (last->logical == logical) {
		UASSERT(last->error_mirror_bitmap);
		set_bit(mirror, last->error_mirror_bitmap);
		return 0;
	}
add:
	last = calloc(1, sizeof(*last));
	if (!last)
		return -ENOMEM;
	last->error_mirror_bitmap = calloc(1, BITS_TO_LONGS(num_mirrors));
	if (!last->error_mirror_bitmap) {
		free(last);
		return -ENOMEM;
	}
	set_bit(mirror - 1, last->error_mirror_bitmap);
	last->logical = logical;
	last->num_mirrors = num_mirrors;

	list_add_tail(&last->list, &corrupted_blocks);
	return 0;
}

/*
 * Verify all mirrors for @logical.
 *
 * If something critical happened, return <0 and should end the run immediately.
 * Otherwise return 0, including data checksum mismatch or read failure.
 */
static int verify_one_data_block(struct btrfs_fs_info *fs_info,
				 struct extent_buffer *leaf,
				 unsigned long leaf_offset, u64 logical,
				 unsigned int num_mirrors)
{
	const u32 blocksize = fs_info->sectorsize;
	const u32 csum_size = fs_info->csum_size;
	u8 *buf;
	u8 csum[BTRFS_CSUM_SIZE];
	u8 csum_expected[BTRFS_CSUM_SIZE];
	int ret = 0;

	buf = malloc(blocksize);
	if (!buf)
		return -ENOMEM;

	for (int mirror = 1; mirror <= num_mirrors; mirror++) {
		u64 read_len = blocksize;

		ret = read_data_from_disk(fs_info, buf, logical, &read_len, mirror);
		if (ret < 0) {
			/* IO error, add one record. */
			ret = add_corrupted_block(fs_info, logical, mirror, num_mirrors);
			if (ret < 0)
				break;
		}
		/* Verify the data checksum. */
		btrfs_csum_data(fs_info, fs_info->csum_type, buf, csum, blocksize);
		read_extent_buffer(leaf, csum_expected, leaf_offset, csum_size);
		if (memcmp(csum_expected, csum, csum_size) != 0) {
			ret = add_corrupted_block(fs_info, logical, mirror, num_mirrors);
			if (ret < 0)
				break;
		}
	}

	free(buf);
	return ret;
}

static int iterate_one_csum_item(struct btrfs_fs_info *fs_info, struct btrfs_path *path)
{
	struct btrfs_key key;
	const unsigned long item_ptr_off = btrfs_item_ptr_offset(path->nodes[0],
								 path->slots[0]);
	const u32 blocksize = fs_info->sectorsize;
	int num_mirrors;
	u64 data_size;
	u64 cur;
	char *buf;
	int ret = 0;

	buf = malloc(blocksize);
	if (!buf)
		return -ENOMEM;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	data_size = btrfs_item_size(path->nodes[0], path->slots[0]) /
		    fs_info->csum_size * blocksize;
	num_mirrors = btrfs_num_copies(fs_info, key.offset, data_size);

	for (cur = 0; cur < data_size; cur += blocksize) {
		const unsigned long leaf_offset = item_ptr_off +
			cur / blocksize * fs_info->csum_size;

		ret = verify_one_data_block(fs_info, path->nodes[0], leaf_offset,
					    key.offset + cur, num_mirrors);
		if (ret < 0)
			break;
	}
	free(buf);
	return ret;
}

static int print_filenames(u64 ino, u64 offset, u64 rootid, void *ctx)
{
	struct btrfs_fs_info *fs_info = ctx;
	struct btrfs_root *root;
	struct btrfs_key key;
	struct inode_fs_paths *ipath;
	struct btrfs_path path = { 0 };
	int ret;

	key.objectid = rootid;
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		errno = -ret;
		error("failed to get subvolume %llu: %m", rootid);
		return ret;
	}
	ipath = init_ipath(128 * BTRFS_PATH_NAME_MAX, root, &path);
	if (IS_ERR(ipath)) {
		ret = PTR_ERR(ipath);
		errno = -ret;
		error("failed to initialize ipath: %m");
		return ret;
	}
	ret = paths_from_inode(ino, ipath);
	if (ret < 0) {
		errno = -ret;
		error("failed to resolve root %llu ino %llu to paths: %m", rootid, ino);
		goto out;
	}
	for (int i = 0; i < ipath->fspath->elem_cnt; i++)
		printf("  (subvolume %llu)/%s\n", rootid, (char *)ipath->fspath->val[i]);
	if (ipath->fspath->elem_missed)
		printf("  (subvolume %llu) %d files not printed\n", rootid,
		       ipath->fspath->elem_missed);
out:
	free_ipath(ipath);
	return ret;
}

static int iterate_csum_root(struct btrfs_fs_info *fs_info, struct btrfs_root *csum_root)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int ret;

	key.objectid = 0;
	key.type = 0;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, csum_root, &key, &path, 0, 0);
	if (ret < 0) {
		errno = -ret;
		error("failed to get the first tree block of csum tree: %m");
		return ret;
	}
	UASSERT(ret > 0);
	while (true) {
		btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
		if (key.type != BTRFS_EXTENT_CSUM_KEY)
			goto next;
		ret = iterate_one_csum_item(fs_info, &path);
		if (ret < 0)
			break;
next:
		ret = btrfs_next_item(csum_root, &path);
		if (ret > 0) {
			ret = 0;
			break;
		}
		if (ret < 0) {
			errno = -ret;
			error("failed to get next csum item: %m");
		}
	}
	btrfs_release_path(&path);
	return ret;
}

static void report_corrupted_blocks(struct btrfs_fs_info *fs_info)
{
	struct corrupted_block *entry;
	struct btrfs_path path = { 0 };

	if (list_empty(&corrupted_blocks)) {
		printf("No data checksum mismatch found\n");
		return;
	}

	list_for_each_entry(entry, &corrupted_blocks, list) {
		bool has_printed = false;
		int ret;

		printf("logical=%llu corrtuped mirrors=", entry->logical);
		/* Poor man's bitmap print. */
		for (int i = 0; i < entry->num_mirrors; i++) {
			if (test_bit(i, entry->error_mirror_bitmap)) {
				if (has_printed)
					printf(",");
				/*
				 * Bit 0 means mirror 1, thus we need to increase
				 * the value by 1.
				 */
				printf("%d", i + 1);
				has_printed=true;
			}
		}
		printf(" affected files:\n");
		ret = iterate_inodes_from_logical(entry->logical, fs_info, &path,
						  print_filenames, fs_info);
		if (ret < 0) {
			errno = -ret;
			error("failed to iterate involved files: %m");
			break;
		}
	}
}

static void free_corrupted_blocks(void)
{
	while (!list_empty(&corrupted_blocks)) {
		struct corrupted_block *entry;

		entry = list_entry(corrupted_blocks.next, struct corrupted_block, list);
		list_del_init(&entry->list);
		free(entry->error_mirror_bitmap);
		free(entry);
	}
}

int btrfs_recover_fix_data_checksum(const char *path,
				    enum btrfs_fix_data_checksum_mode mode)
{
	struct btrfs_fs_info *fs_info;
	struct btrfs_root *csum_root;
	struct open_ctree_args oca = { 0 };
	int ret;

	if (mode >= BTRFS_FIX_DATA_CSUMS_LAST)
		return -EINVAL;

	ret = check_mounted(path);
	if (ret < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		return ret;
	}
	if (ret > 0) {
		error("%s is currently mounted", path);
		return -EBUSY;
	}

	global_repair_mode = mode;
	oca.filename = path;
	oca.flags = OPEN_CTREE_WRITES;
	fs_info = open_ctree_fs_info(&oca);
	if (!fs_info) {
		error("failed to open btrfs at %s", path);
		return -EIO;
	}
	csum_root = btrfs_csum_root(fs_info, 0);
	if (!csum_root) {
		error("failed to get csum root");
		ret = -EIO;
		goto out_close;
	}
	ret = iterate_csum_root(fs_info, csum_root);
	if (ret) {
		errno = -ret;
		error("failed to iterate csum tree: %m");
	}
	report_corrupted_blocks(fs_info);
out_close:
	free_corrupted_blocks();
	close_ctree_fs_info(fs_info);
	return ret;
}
