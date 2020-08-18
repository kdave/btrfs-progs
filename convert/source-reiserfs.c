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

#if BTRFSCONVERT_REISERFS

#include "kerncompat.h"
#include <linux/limits.h>
#include <linux/fs.h>
#include <limits.h>
#include <sys/stat.h>
#include <stdbool.h>
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "common/utils.h"
#include "kernel-lib/bitops.h"
#include "convert/common.h"
#include "convert/source-reiserfs.h"

static inline u8 mode_to_file_type(u32 mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:	return BTRFS_FT_REG_FILE;
	case S_IFDIR:	return BTRFS_FT_DIR;
	case S_IFCHR:	return BTRFS_FT_CHRDEV;
	case S_IFBLK:	return BTRFS_FT_BLKDEV;
	case S_IFIFO:	return BTRFS_FT_FIFO;
	case S_IFSOCK:	return BTRFS_FT_SOCK;
	case S_IFLNK:	return BTRFS_FT_SYMLINK;
	};

	return BTRFS_FT_UNKNOWN;
}

static u32 reiserfs_count_objectids(reiserfs_filsys_t fs)
{
	struct reiserfs_super_block *sb = fs->fs_ondisk_sb;
	u32 count = 0;
	u32 *map;
	int i;

	if (fs->fs_format == REISERFS_FORMAT_3_6)
		map = (u32 *) (sb + 1);
	else
		map = (u32 *)((struct reiserfs_super_block_v1 *)sb + 1);

	for (i = 0; i < get_sb_oid_cursize(sb); i += 2)
		count += le32_to_cpu(map[i + 1]) - (le32_to_cpu(map[i]) + 1);

	return count;
}


static int reiserfs_open_fs(struct btrfs_convert_context *cxt, const char *name)
{
	struct reiserfs_convert_info *info;
	reiserfs_filsys_t fs;
	long error;

	fs = reiserfs_open(name, O_RDONLY, &error, NULL, 0);
	if (!fs)
		return -1;

	error = reiserfs_open_ondisk_bitmap(fs);
	if (error) {
		reiserfs_close(fs);
		return -1;
	}

	cxt->fs_data = fs;
	cxt->blocksize = fs->fs_blocksize;
	cxt->block_count = get_sb_block_count(fs->fs_ondisk_sb);
	cxt->total_bytes = (u64)cxt->block_count * cxt->blocksize;
	cxt->volume_name = strndup(fs->fs_ondisk_sb->s_label, 16);
	cxt->first_data_block = 0;
	cxt->inodes_count = reiserfs_count_objectids(fs);
	cxt->free_inodes_count = 0;
	info = calloc(1, sizeof(*info));
	if (!info) {
		reiserfs_close(fs);
		return -1;
	}

	/*
	 * Inode attributes are somewhat of a hack on reiserfs and it was
	 * once possible to have garbage in the flags field.  A superblock
	 * field now indicates that the field has been cleared and can
	 * be considered valid, but only on v3.6 format file systems.
	 */
	if (fs->fs_format == REISERFS_FORMAT_3_6 &&
	    get_sb_v2_flag(fs->fs_ondisk_sb, reiserfs_attrs_cleared))
		info->copy_attrs = true;

	fs->fs_vp = info;
	return 0;
}

static void reiserfs_close_fs(struct btrfs_convert_context *cxt)
{
	reiserfs_filsys_t fs = cxt->fs_data;
	struct reiserfs_convert_info *info = fs->fs_vp;

	if (info) {
		if (info->objectids)
			free(info->objectids);
		free(info);
		fs->fs_vp = NULL;
	}

	/* We don't want changes to be persistent */
	fs->fs_bitmap2->bm_dirty = 0;

	reiserfs_close(fs);
}

static int compare_objectids(const void *p1, const void *p2)
{
	u64 v1 = *(u64 *)p1;
	u64 v2 = *(u64 *)p2;

	if (v1 > v2)
		return 1;
	else if (v1 < v2)
		return -1;
	return 0;
}

static int lookup_cached_objectid(reiserfs_filsys_t fs, u64 objectid)
{
	struct reiserfs_convert_info *info = fs->fs_vp;
	u64 *result;

	if (!info->objectids)
		return 0;
	result = bsearch(&objectid, info->objectids, info->used_slots,
			 sizeof(u64), compare_objectids);
	return result != NULL;
}

static int insert_cached_objectid(reiserfs_filsys_t fs, u64 objectid)
{
	struct reiserfs_convert_info *info = fs->fs_vp;

	if (info->used_slots + 1 >= info->alloced_slots) {
		u64 *objectids = realloc(info->objectids,
				    (info->alloced_slots + 1000) * sizeof(u64));

		if (!objectids)
			return -ENOMEM;
		info->objectids = objectids;
		info->alloced_slots += 1000;
	}
	info->objectids[info->used_slots++] = objectid;

	qsort(info->objectids, info->used_slots, sizeof(u64), compare_objectids);
	return 0;
}

static int reiserfs_locate_privroot(reiserfs_filsys_t fs)
{
	int err;
	unsigned generation;
	struct reiserfs_convert_info *info = fs->fs_vp;
	struct reiserfs_key key = root_dir_key;

	err = reiserfs_find_entry(fs, &key, ".reiserfs_priv",
				  &generation, &info->privroot_key);
	if (err == 1) {
		err = reiserfs_find_entry(fs, &info->privroot_key, "xattrs",
					  &generation, &info->xattr_key);
		if (err != 1)
			memset(&info->xattr_key, 0, sizeof(info->xattr_key));
	}

	return 0;
}

static void reiserfs_convert_inode_flags(struct btrfs_inode_item *inode,
					 const struct stat_data *sd)
{
	u16 attrs = sd_v2_sd_attrs(sd);
	u64 new_flags = 0;

	if (attrs & FS_IMMUTABLE_FL)
		new_flags |= BTRFS_INODE_IMMUTABLE;

	if (attrs & FS_APPEND_FL)
		new_flags |= BTRFS_INODE_APPEND;

	if (attrs & FS_SYNC_FL)
		new_flags |= BTRFS_INODE_SYNC;

	if (attrs & FS_NOATIME_FL)
		new_flags |= BTRFS_INODE_NOATIME;

	if (attrs & FS_NODUMP_FL)
		new_flags |= BTRFS_INODE_NODUMP;

	if (attrs & FS_NODUMP_FL)
		new_flags |= BTRFS_INODE_NODUMP;

	btrfs_set_stack_inode_flags(inode, new_flags);

}

static void reiserfs_copy_inode_item(struct btrfs_inode_item *inode,
				     struct item_head *ih, void *stat_data,
				     bool copy_inode_flags)
{
	u32 mode;
	u32 rdev = 0;

	memset(inode, 0, sizeof(*inode));
	btrfs_set_stack_inode_generation(inode, 1);
	if (get_ih_key_format(ih) == KEY_FORMAT_1) {
		struct stat_data_v1 *sd = stat_data;

		mode = sd_v1_mode(sd);
		btrfs_set_stack_inode_size(inode, sd_v1_size(sd));
		btrfs_set_stack_inode_nlink(inode, sd_v1_nlink(sd));
		btrfs_set_stack_inode_uid(inode, sd_v1_uid(sd));
		btrfs_set_stack_inode_gid(inode, sd_v1_gid(sd));
		btrfs_set_stack_timespec_sec(&inode->atime, sd_v1_atime(sd));
		btrfs_set_stack_timespec_sec(&inode->ctime, sd_v1_ctime(sd));
		btrfs_set_stack_timespec_sec(&inode->mtime, sd_v1_mtime(sd));

		if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode))
			rdev = decode_dev(sd_v1_rdev(sd));
	} else {
		struct stat_data *sd = stat_data;

		mode = sd_v2_mode(sd);
		btrfs_set_stack_inode_size(inode, sd_v2_size(sd));
		btrfs_set_stack_inode_nlink(inode, sd_v2_nlink(sd));
		btrfs_set_stack_inode_uid(inode, sd_v2_uid(sd));
		btrfs_set_stack_inode_gid(inode, sd_v2_gid(sd));
		btrfs_set_stack_timespec_sec(&inode->atime, sd_v2_atime(sd));
		btrfs_set_stack_timespec_sec(&inode->ctime, sd_v2_ctime(sd));
		btrfs_set_stack_timespec_sec(&inode->mtime, sd_v2_mtime(sd));

		if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode))
			rdev = decode_dev(sd_v2_rdev(sd));

		if (copy_inode_flags)
			reiserfs_convert_inode_flags(inode, sd);

	}
	if (S_ISDIR(mode)) {
		btrfs_set_stack_inode_size(inode, 0);
		btrfs_set_stack_inode_nlink(inode, 1);
	}
	btrfs_set_stack_inode_mode(inode, mode);
	btrfs_set_stack_inode_rdev(inode, rdev);
}

static void init_reiserfs_blk_iterate_data(
				struct reiserfs_blk_iterate_data *data,
				struct btrfs_trans_handle *trans,
				struct btrfs_root *root,
				struct btrfs_inode_item *inode,
				u64 objectid, u32 convert_flags)
{
	init_blk_iterate_data(&data->blk_data, trans, root, inode, objectid,
			      convert_flags & CONVERT_FLAG_DATACSUM);
	data->inline_data = NULL;
	data->inline_offset = (u64)-1;
	data->inline_length = 0;
}

static int reiserfs_record_indirect_extent(reiserfs_filsys_t fs, u64 position,
					   u64 size, int num_ptrs,
					   u32 *ptrs, void *data)
{
	struct reiserfs_blk_iterate_data *bdata = data;
	u32 file_block = position / fs->fs_blocksize;
	int i;
	int ret = 0;

	for (i = 0; i < num_ptrs; i++, file_block++) {
		u32 block = d32_get(ptrs, i);

		ret = block_iterate_proc(block, file_block, &bdata->blk_data);
		if (ret)
			break;
	}

	return ret;
}

/*
 * Unlike btrfs inline extents, reiserfs can have multiple inline extents.
 * This handles concatenating multiple tails into one inline extent
 * for insertion.
 */
static int reiserfs_record_direct_extent(reiserfs_filsys_t fs, __u64 position,
					 __u64 size, const char *body,
					 size_t len, void *data)
{
	struct reiserfs_blk_iterate_data *bdata = data;
	char *inline_data;

	if (bdata->inline_offset == (u64)-1)
		bdata->inline_offset = position;
	else if (bdata->inline_offset + bdata->inline_length != position) {
		/*
		 * This condition shouldn't actually happen, but better to
		 * catch it than break silently.
		 */
		error(
"source fs contains file with multiple tails but they are not contiguous");
		return -EINVAL;
	}

	inline_data = realloc(bdata->inline_data, bdata->inline_length + len);
	if (!inline_data)
		return -ENOMEM;

	bdata->inline_data = inline_data;
	memcpy(bdata->inline_data + bdata->inline_length, body, len);
	bdata->inline_length += len;

	return 0;
}

static int convert_direct(struct btrfs_trans_handle *trans,
			  struct btrfs_root *root, u64 objectid,
			  struct btrfs_inode_item *inode, const char *body,
			  u32 length, u64 offset, u32 convert_flags)
{
	struct btrfs_key key;
	u32 sectorsize = root->fs_info->sectorsize;
	int ret;
	struct extent_buffer *eb;

	BUG_ON(length > sectorsize);
	ret = btrfs_reserve_extent(trans, root, sectorsize,
				   0, 0, -1ULL, &key, 1);
	if (ret)
		return ret;

	eb = alloc_extent_buffer(root->fs_info, key.objectid, sectorsize);

	if (!eb)
		return -ENOMEM;

	write_extent_buffer(eb, body, 0, length);
	ret = write_and_map_eb(root->fs_info, eb);
	free_extent_buffer(eb);
	if (ret)
		return ret;

	return btrfs_record_file_extent(trans, root, objectid, inode, offset,
					key.objectid, sectorsize);
}

static int reiserfs_convert_tail(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_inode_item *inode,
				 u64 objectid, u64 offset,
				 const void *body, unsigned length,
				 u32 convert_flags)
{
	u64 isize;
	int ret;

	if (length >= BTRFS_MAX_INLINE_DATA_SIZE(root->fs_info) ||
	    length >= root->fs_info->sectorsize)
		return convert_direct(trans, root, objectid, inode, body,
				      length, offset, convert_flags);

	ret = btrfs_insert_inline_extent(trans, root, objectid,
					 offset, body, length);
	if (ret)
		return ret;

	isize = btrfs_stack_inode_nbytes(inode);
	btrfs_set_stack_inode_nbytes(inode, isize + length);

	return 0;
}

static inline u32 block_count(u64 size, u32 blocksize)
{
	return round_up(size, blocksize) / blocksize;
}

static int reiserfs_record_file_extents(reiserfs_filsys_t fs,
					struct btrfs_trans_handle *trans,
					struct btrfs_root *root,
					u64 objectid,
					struct btrfs_inode_item *inode,
					struct reiserfs_key *sd_key,
					u32 convert_flags)

{
	int ret;
	u32 blocksize = fs->fs_blocksize;
	u64 inode_size = btrfs_stack_inode_size(inode);
	u32 last_block;
	struct reiserfs_blk_iterate_data data;

	init_reiserfs_blk_iterate_data(&data, trans, root, inode,
				       objectid, convert_flags);

	ret = reiserfs_iterate_file_data(fs, sd_key,
					 reiserfs_record_indirect_extent,
					 reiserfs_record_direct_extent, &data);
	if (ret)
		return ret;

	/*
	 * blk_iterate_block has no idea that we're done iterating, so record
	 * the final range if any.  This range can end and still have a tail
	 * after it.
	 */
	if (data.blk_data.num_blocks) {
		ret = record_file_blocks(&data.blk_data,
					 data.blk_data.first_block,
					 data.blk_data.disk_block,
					 data.blk_data.num_blocks);
		if (ret)
			goto fail;
		data.blk_data.first_block += data.blk_data.num_blocks;
		data.blk_data.num_blocks = 0;
	}

	/*
	 * Handle a hole at the end of the file.  ReiserFS will
	 * not write a tail followed by a hole but it will write a hole
	 * followed by a tail.
	 */
	last_block = block_count(inode_size - data.inline_length, blocksize);
	if (last_block > data.blk_data.first_block) {
		ret = record_file_blocks(&data.blk_data,
					 data.blk_data.first_block, 0,
					 last_block - data.blk_data.first_block);
		if (ret)
			goto fail;
	}

	if (data.inline_length) {
		ret = reiserfs_convert_tail(trans, root, inode, objectid,
					    data.inline_offset,
					    data.inline_data,
					    data.inline_length, convert_flags);
		if (ret)
			goto fail;
	}

	ret = 0;
fail:
	return ret;
}

static int reiserfs_copy_meta(reiserfs_filsys_t fs, struct btrfs_root *root,
			      u32 convert_flags, u32 deh_dirid,
			      u32 deh_objectid, u8 *type);

static int reiserfs_copy_dirent(reiserfs_filsys_t fs,
				const struct reiserfs_key *dir_short_key,
				const char *name, size_t len,
				__u32 deh_dirid, __u32 deh_objectid,
				void *cb_data)
{
	int ret;
	u8 type;
	struct btrfs_trans_handle *trans;
	u64 objectid = deh_objectid + OID_OFFSET;
	struct reiserfs_convert_info *info = fs->fs_vp;
	struct reiserfs_dirent_data *dirent_data = cb_data;
	struct btrfs_root *root = dirent_data->root;
	__u32 dir_objectid = get_key_objectid(dir_short_key) + OID_OFFSET;

	/*
	 * These are the extended attributes and shouldn't appear as files
	 * in the converted file systems.
	 */
	if (deh_objectid == get_key_objectid(&info->privroot_key))
		return 0;

	ret = reiserfs_copy_meta(fs, root, dirent_data->convert_flags,
				 deh_dirid, deh_objectid, &type);
	if (ret) {
		errno = -ret;
		error(
	"an error occurred while converting \"%.*s\", reiserfs key [%u %u]: %m",
			(int)len, name, deh_dirid, deh_objectid);
		return ret;
	}
	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	ret = convert_insert_dirent(trans, root, name, len, dir_objectid,
				    objectid, type, dirent_data->index++,
				    dirent_data->inode);
	return btrfs_commit_transaction(trans, root);
}

static int reiserfs_copy_symlink(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root, u64 objectid,
				 struct btrfs_inode_item *btrfs_inode,
				 reiserfs_filsys_t fs,
				 struct reiserfs_path *sd_path)
{
	INITIALIZE_REISERFS_PATH(path);
	struct item_head *ih = tp_item_head(sd_path);
	struct reiserfs_key key = ih->ih_key;
	int ret;
	char *symlink;
	int len;

	set_key_uniqueness(&key, type2uniqueness(TYPE_DIRECT));
	set_key_offset_v1(&key, 1);

	ret = reiserfs_search_by_key_3(fs, &key, &path);
	if (ret != ITEM_FOUND) {
		ret = -ENOENT;
		goto fail;
	}

	symlink = tp_item_body(&path);
	len = get_ih_item_len(tp_item_head(&path));

	ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
					 symlink, len + 1);
	btrfs_set_stack_inode_nbytes(btrfs_inode, len + 1);
fail:
	pathrelse(&path);
	return ret;
}

static int reiserfs_copy_meta(reiserfs_filsys_t fs, struct btrfs_root *root,
			      u32 convert_flags, u32 deh_dirid,
			      u32 deh_objectid, u8 *type)
{
	INITIALIZE_REISERFS_PATH(path);
	int ret = 0;
	struct item_head *ih;
	struct reiserfs_key key;
	struct btrfs_inode_item btrfs_inode;
	struct btrfs_trans_handle *trans = NULL;
	struct reiserfs_convert_info *info = fs->fs_vp;
	u32 mode;
	u64 objectid = deh_objectid + OID_OFFSET;
	u64 parent = deh_dirid + OID_OFFSET;
	struct reiserfs_dirent_data dirent_data = {
		.index = 2,
		.convert_flags = convert_flags,
		.inode = &btrfs_inode,
		.root = root,
	};

	/* The root directory's dirid in reiserfs points to an object
	 * that doesn't exist.  In btrfs it's self-referential.
	 */
	if (deh_dirid == REISERFS_ROOT_PARENT_OBJECTID)
		parent = objectid;

	set_key_dirid(&key, deh_dirid);
	set_key_objectid(&key, deh_objectid);
	set_key_offset_v2(&key, 0);
	set_key_type_v2(&key, TYPE_STAT_DATA);

	ret = reiserfs_search_by_key_3(fs, &key, &path);
	if (ret != ITEM_FOUND) {
		ret = -ENOENT;
		goto fail;
	}

	ih = tp_item_head(&path);
	if (!is_stat_data_ih(ih)) {
		ret = -EINVAL;
		goto fail;
	}

	reiserfs_copy_inode_item(&btrfs_inode, ih, tp_item_body(&path),
				 info->copy_attrs);
	mode = btrfs_stack_inode_mode(&btrfs_inode);
	*type = mode_to_file_type(mode);

	if (S_ISREG(mode)) {
		/* Inodes with hardlinks should only be inserted once */
		if (btrfs_stack_inode_nlink(&btrfs_inode) > 1) {
			if (lookup_cached_objectid(fs, deh_objectid)) {
				ret = 0;
				goto fail; /* Not a failure */
			}
			ret = insert_cached_objectid(fs, deh_objectid);
			if (ret)
				goto fail;
		}
	}

	if (!(convert_flags & CONVERT_FLAG_DATACSUM)) {
		u32 flags = btrfs_stack_inode_flags(&btrfs_inode) |
			    BTRFS_INODE_NODATASUM;
		btrfs_set_stack_inode_flags(&btrfs_inode, flags);
	}

	switch (mode & S_IFMT) {
	case S_IFREG:
		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			goto fail;
		}
		ret = reiserfs_record_file_extents(fs, trans, root, objectid,
						   &btrfs_inode, &ih->ih_key,
						   convert_flags);
		if (ret)
			goto fail;
		break;
	case S_IFDIR:
		ret = reiserfs_iterate_dir(fs, &ih->ih_key,
					   reiserfs_copy_dirent, &dirent_data);
		if (ret)
			goto fail;
		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			goto fail;
		}

		ret = btrfs_insert_inode_ref(trans, root, "..", 2, parent,
					     objectid, 0);
		break;
	case S_IFLNK:
		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			goto fail;
		}
		ret = reiserfs_copy_symlink(trans, root, objectid,
					    &btrfs_inode, fs, &path);
		if (ret)
			goto fail;
		break;
	default:
		trans = btrfs_start_transaction(root, 1);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			goto fail;
		}
	}

	ret = btrfs_insert_inode(trans, root, objectid, &btrfs_inode);
	if (ret)
		goto fail;
	ret = btrfs_commit_transaction(trans, root);
	info->progress->cur_copy_inodes++;

fail:
	pathrelse(&path);
	return ret;
}

static int reiserfs_xattr_indirect_fn(reiserfs_filsys_t fs, u64 position,
				      u64 size, int num_blocks,
				      u32 *blocks, void *data)
{
	int i;
	struct reiserfs_xattr_data *xa_data = data;
	size_t alloc = min(position + num_blocks * fs->fs_blocksize, size);
	char *body;

	if (size > BTRFS_LEAF_DATA_SIZE(xa_data->root->fs_info) -
	    sizeof(struct btrfs_item) - sizeof(struct btrfs_dir_item)) {
		fprintf(stderr, "skip large xattr on objectid %llu name %.*s\n",
			xa_data->target_oid, (int)xa_data->namelen,
			xa_data->name);
		return -E2BIG;
	}

	body = realloc(xa_data->body, alloc);
	if (!body)
		return -ENOMEM;

	xa_data->body = body;
	xa_data->len = alloc;

	for (i = 0; i < num_blocks; i++) {
		int ret;
		u32 block = d32_get(blocks, i);
		u64 offset = (u64)block * fs->fs_blocksize;
		size_t chunk = min_t(u64, size - position, fs->fs_blocksize);
		char *buffer = xa_data->body + position;

		ret = read_disk_extent(xa_data->root, offset, chunk, buffer);
		if (ret)
			return ret;
		position += chunk;
	}

	return 0;
}

static int reiserfs_xattr_direct_fn(reiserfs_filsys_t fs, __u64 position,
				    __u64 size, const char *body, size_t len,
				    void *data)
{
	struct reiserfs_xattr_data *xa_data = data;
	char *newbody;

	if (size > BTRFS_LEAF_DATA_SIZE(xa_data->root->fs_info) -
	    sizeof(struct btrfs_item) - sizeof(struct btrfs_dir_item)) {
		fprintf(stderr, "skip large xattr on objectid %llu name %.*s\n",
			xa_data->target_oid, (int)xa_data->namelen,
			xa_data->name);
		return -E2BIG;
	}

	newbody = realloc(xa_data->body, position + len);
	if (!newbody)
		return -ENOMEM;
	xa_data->body = newbody;
	xa_data->len = position + len;
	memcpy(xa_data->body + position, body, len);
	return 0;
}

static int reiserfs_acl_to_xattr(void *dst, const void *src,
				 size_t dst_size, size_t src_size)
{
	int i, count;
	const void *end = src + src_size;
	acl_ea_header *ext_acl = (acl_ea_header *)dst;
	acl_ea_entry *dst_entry = ext_acl->a_entries;
	struct reiserfs_acl_entry *src_entry;

	if (src_size < sizeof(struct reiserfs_acl_header))
		goto fail;
	if (((struct reiserfs_acl_header *)src)->a_version !=
	    cpu_to_le32(REISERFS_ACL_VERSION))
		goto fail;
	src += sizeof(struct reiserfs_acl_header);
	count = reiserfs_acl_count(src_size);
	if (count <= 0)
		goto fail;

	BUG_ON(dst_size < acl_ea_size(count));
	ext_acl->a_version = cpu_to_le32(ACL_EA_VERSION);
	for (i = 0; i < count; i++, dst_entry++) {
		src_entry = (struct reiserfs_acl_entry *)src;
		if (src + sizeof(struct reiserfs_acl_entry_short) > end)
			goto fail;
		dst_entry->e_tag = src_entry->e_tag;
		dst_entry->e_perm = src_entry->e_perm;
		switch (le16_to_cpu(src_entry->e_tag)) {
		case ACL_USER_OBJ:
		case ACL_GROUP_OBJ:
		case ACL_MASK:
		case ACL_OTHER:
			src += sizeof(struct reiserfs_acl_entry_short);
			dst_entry->e_id = cpu_to_le32(ACL_UNDEFINED_ID);
			break;
		case ACL_USER:
		case ACL_GROUP:
			src += sizeof(struct reiserfs_acl_entry);
			if (src > end)
				goto fail;
			dst_entry->e_id = src_entry->e_id;
			break;
		default:
			goto fail;
		}
	}
	if (src != end)
		goto fail;
	return 0;
fail:
	return -EINVAL;
}

static int reiserfs_copy_one_xattr(reiserfs_filsys_t fs,
				   const struct reiserfs_key *dir_short_key,
				   const char *name, size_t namelen,
				   __u32 deh_dirid,
				   __u32 deh_objectid, void *cb_data)
{
	struct reiserfs_convert_info *info = fs->fs_vp;
	struct reiserfs_xattr_data *xa_data = cb_data;
	struct reiserfs_key key = {
		.k2_dir_id = deh_dirid,
		.k2_objectid = deh_objectid,
	};
	void *body = NULL;
	int len;
	int ret;

	xa_data->name = name;
	xa_data->namelen = namelen;

	ret = reiserfs_iterate_file_data(fs, &key, reiserfs_xattr_indirect_fn,
					 reiserfs_xattr_direct_fn, cb_data);
	if (ret)
		goto out;

	if (!reiserfs_check_xattr(xa_data->body, xa_data->len)) {
		fprintf(stderr,
			"skip corrupted xattr on objectid %u name %.*s\n",
			deh_objectid, (int)xa_data->namelen,
			xa_data->name);
		goto out;
	}

	body = xa_data->body + sizeof(struct reiserfs_xattr_header);
	len = xa_data->len - sizeof(struct reiserfs_xattr_header);

	if (!strncmp("system.posix_acl_default", name, namelen) ||
	    !strncmp("system.posix_acl_access", name, namelen)) {
		size_t bufsize = acl_ea_size(ext2_acl_count(len));
		char *databuf = malloc(bufsize);

		if (!databuf)
			goto out;
		ret = reiserfs_acl_to_xattr(databuf, body, bufsize, len);
		if (ret)
			goto out;
		body = databuf;
		len = bufsize;
	}

	ret = btrfs_insert_xattr_item(xa_data->trans, xa_data->root,
				      name, namelen, body, len,
				      xa_data->target_oid);

	info->progress->cur_copy_inodes++;
out:
	if (body &&
	    body != xa_data->body + sizeof(struct reiserfs_xattr_header))
		free(body);
	if (xa_data->body)
		free(xa_data->body);
	xa_data->body = NULL;
	xa_data->len = 0;

	return ret;
}

static int reiserfs_copy_xattr_dir(reiserfs_filsys_t fs,
				   const struct reiserfs_key *dir_short_key,
				   const char *name, size_t len,
				   __u32 deh_dirid, __u32 deh_objectid,
				   void *cb_data)
{
	struct reiserfs_convert_info *info = fs->fs_vp;
	struct reiserfs_xattr_data *xa_data = cb_data;
	struct reiserfs_key dir_key = {
		.k2_dir_id = deh_dirid,
		.k2_objectid = deh_objectid,
	};
	int ret, err;

	errno = 0;
	xa_data->target_oid = strtoull(name, NULL, 16);
	if (xa_data->target_oid == ULLONG_MAX && errno)
		return -errno;

	xa_data->target_oid += OID_OFFSET;

	xa_data->trans = btrfs_start_transaction(xa_data->root, 1);
	if (IS_ERR(xa_data->trans))
		return PTR_ERR(xa_data->trans);

	ret = reiserfs_iterate_dir(fs, &dir_key,
				    reiserfs_copy_one_xattr, xa_data);

	err = btrfs_commit_transaction(xa_data->trans, xa_data->root);
	info->progress->cur_copy_inodes++;
	xa_data->trans = NULL;
	return ret ?: err;
}

static int reiserfs_copy_xattrs(reiserfs_filsys_t fs, struct btrfs_root *root)
{
	struct reiserfs_convert_info *info = fs->fs_vp;
	struct reiserfs_xattr_data data = {
		.root = root,
	};

	if (get_key_objectid(&info->xattr_key) == 0)
		return 0;

	return reiserfs_iterate_dir(fs, &info->xattr_key,
				    reiserfs_copy_xattr_dir, &data);
}

static int reiserfs_copy_inodes(struct btrfs_convert_context *cxt,
				struct btrfs_root *root,
				u32 convert_flags,
				struct task_ctx *p)
{
	reiserfs_filsys_t fs = cxt->fs_data;
	struct reiserfs_convert_info *info = fs->fs_vp;
	int ret;
	u8 type;

	info->progress = p;

	ret = reiserfs_locate_privroot(fs);
	if (ret)
		goto out;

	ret = reiserfs_copy_meta(fs, root, convert_flags,
				 REISERFS_ROOT_PARENT_OBJECTID,
				 REISERFS_ROOT_OBJECTID, &type);
	if (ret)
		goto out;

	if (convert_flags & CONVERT_FLAG_XATTR)
		ret = reiserfs_copy_xattrs(fs, root);

out:
	info->progress = NULL;
	return ret;
}

static int reiserfs_read_used_space(struct btrfs_convert_context *cxt)
{
	reiserfs_filsys_t fs = cxt->fs_data;
	u64 start, end = 0;
	unsigned int size = get_sb_block_count(fs->fs_ondisk_sb);
	unsigned long *bitmap = (unsigned long *)fs->fs_bitmap2->bm_map;
	int ret = 0;

	/*
	 * We have the entire bitmap loaded so we can just ping pong with
	 * ffz and ffs
	 */
	while (end < size) {
		u64 offset, length;

		start = find_next_bit(bitmap, size, end);
		if (start >= size)
			break;
		end = find_next_zero_bit(bitmap, size, start);
		if (end > size)
			end = size;
		offset = start * fs->fs_blocksize;
		length = (end - start) * fs->fs_blocksize;
		ret = add_merge_cache_extent(&cxt->used_space, offset, length);
		if (ret < 0)
			break;
	}

	return ret;
}

static int reiserfs_check_state(struct btrfs_convert_context *cxt)
{
	return 0;
}

const struct btrfs_convert_operations reiserfs_convert_ops = {
	.name		= "reiserfs",
	.open_fs	= reiserfs_open_fs,
	.read_used_space = reiserfs_read_used_space,
	.copy_inodes	= reiserfs_copy_inodes,
	.close_fs	= reiserfs_close_fs,
	.check_state	= reiserfs_check_state,
};

#endif /* BTRFSCONVERT_REISERFS */
