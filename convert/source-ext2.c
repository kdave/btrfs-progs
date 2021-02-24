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

#if BTRFSCONVERT_EXT2

#include "kerncompat.h"
#include <linux/limits.h>
#include <pthread.h>
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "common/utils.h"
#include "convert/common.h"
#include "convert/source-ext2.h"

/*
 * Open Ext2fs in readonly mode, read block allocation bitmap and
 * inode bitmap into memory.
 */
static int ext2_open_fs(struct btrfs_convert_context *cctx, const char *name)
{
	errcode_t ret;
	ext2_filsys ext2_fs;
	ext2_ino_t ino;
	u32 ro_feature;
	int open_flag = EXT2_FLAG_SOFTSUPP_FEATURES | EXT2_FLAG_64BITS;

	ret = ext2fs_open(name, open_flag, 0, 0, unix_io_manager, &ext2_fs);
	if (ret) {
		if (ret != EXT2_ET_BAD_MAGIC)
			fprintf(stderr, "ext2fs_open: %s\n", error_message(ret));
		return -1;
	}
	/*
	 * We need to know exactly the used space, some RO compat flags like
	 * BIGALLOC will affect how used space is present.
	 * So we need manuall check any unsupported RO compat flags
	 */
	ro_feature = ext2_fs->super->s_feature_ro_compat;
	if (ro_feature & ~EXT2_LIB_FEATURE_RO_COMPAT_SUPP) {
		error(
"unsupported RO features detected: %x, abort convert to avoid possible corruption",
		      ro_feature & ~EXT2_LIB_FEATURE_COMPAT_SUPP);
		goto fail;
	}
	ret = ext2fs_read_inode_bitmap(ext2_fs);
	if (ret) {
		fprintf(stderr, "ext2fs_read_inode_bitmap: %s\n",
			error_message(ret));
		goto fail;
	}
	ret = ext2fs_read_block_bitmap(ext2_fs);
	if (ret) {
		fprintf(stderr, "ext2fs_read_block_bitmap: %s\n",
			error_message(ret));
		goto fail;
	}
	/*
	 * search each block group for a free inode. this set up
	 * uninit block/inode bitmaps appropriately.
	 */
	ino = 1;
	while (ino <= ext2_fs->super->s_inodes_count) {
		ext2_ino_t foo;
		ext2fs_new_inode(ext2_fs, ino, 0, NULL, &foo);
		ino += EXT2_INODES_PER_GROUP(ext2_fs->super);
	}

	if (!(ext2_fs->super->s_feature_incompat &
	      EXT2_FEATURE_INCOMPAT_FILETYPE)) {
		error("filetype feature is missing");
		goto fail;
	}

	cctx->fs_data = ext2_fs;
	cctx->blocksize = ext2_fs->blocksize;
	cctx->block_count = ext2_fs->super->s_blocks_count;
	cctx->total_bytes = (u64)ext2_fs->super->s_blocks_count * ext2_fs->blocksize;
	cctx->volume_name = strndup((char *)ext2_fs->super->s_volume_name, 16);
	cctx->first_data_block = ext2_fs->super->s_first_data_block;
	cctx->inodes_count = ext2_fs->super->s_inodes_count;
	cctx->free_inodes_count = ext2_fs->super->s_free_inodes_count;
	return 0;
fail:
	ext2fs_close(ext2_fs);
	ext2fs_free(ext2_fs);
	return -1;
}

static int __ext2_add_one_block(ext2_filsys fs, char *bitmap,
				unsigned long group_nr, struct cache_tree *used)
{
	unsigned long offset;
	unsigned i;
	int ret = 0;

	offset = fs->super->s_first_data_block;
	offset /= EXT2FS_CLUSTER_RATIO(fs);
	offset += group_nr * EXT2_CLUSTERS_PER_GROUP(fs->super);
	for (i = 0; i < EXT2_CLUSTERS_PER_GROUP(fs->super); i++) {
		if ((i + offset) >= ext2fs_blocks_count(fs->super))
			break;

		if (ext2fs_test_bit(i, bitmap)) {
			u64 start;

			start = (i + offset) * EXT2FS_CLUSTER_RATIO(fs);
			start *= fs->blocksize;
			ret = add_merge_cache_extent(used, start,
						     fs->blocksize);
			if (ret < 0)
				break;
		}
	}
	return ret;
}

/*
 * Read all used ext2 space into cctx->used cache tree
 */
static int ext2_read_used_space(struct btrfs_convert_context *cctx)
{
	ext2_filsys fs = (ext2_filsys)cctx->fs_data;
	blk64_t blk_itr = EXT2FS_B2C(fs, fs->super->s_first_data_block);
	struct cache_tree *used_tree = &cctx->used_space;
	char *block_bitmap = NULL;
	unsigned long i;
	int block_nbytes;
	int ret = 0;

	block_nbytes = EXT2_CLUSTERS_PER_GROUP(fs->super) / 8;
	if (!block_nbytes) {
		error("EXT2_CLUSTERS_PER_GROUP too small: %llu",
			(unsigned long long)(EXT2_CLUSTERS_PER_GROUP(fs->super)));
		return -EINVAL;
	}

	block_bitmap = malloc(block_nbytes);
	if (!block_bitmap)
		return -ENOMEM;

	for (i = 0; i < fs->group_desc_count; i++) {
		ret = ext2fs_get_block_bitmap_range2(fs->block_map, blk_itr,
						block_nbytes * 8, block_bitmap);
		if (ret) {
			error("fail to get bitmap from ext2, %s",
				error_message(ret));
			ret = -EINVAL;
			break;
		}
		ret = __ext2_add_one_block(fs, block_bitmap, i, used_tree);
		if (ret < 0) {
			errno = -ret;
			error("fail to build used space tree, %m");
			break;
		}
		blk_itr += EXT2_CLUSTERS_PER_GROUP(fs->super);
	}

	free(block_bitmap);
	return ret;
}

static void ext2_close_fs(struct btrfs_convert_context *cctx)
{
	if (cctx->volume_name) {
		free(cctx->volume_name);
		cctx->volume_name = NULL;
	}
	ext2fs_close(cctx->fs_data);
	ext2fs_free(cctx->fs_data);
}

static u8 ext2_filetype_conversion_table[EXT2_FT_MAX] = {
	[EXT2_FT_UNKNOWN]	= BTRFS_FT_UNKNOWN,
	[EXT2_FT_REG_FILE]	= BTRFS_FT_REG_FILE,
	[EXT2_FT_DIR]		= BTRFS_FT_DIR,
	[EXT2_FT_CHRDEV]	= BTRFS_FT_CHRDEV,
	[EXT2_FT_BLKDEV]	= BTRFS_FT_BLKDEV,
	[EXT2_FT_FIFO]		= BTRFS_FT_FIFO,
	[EXT2_FT_SOCK]		= BTRFS_FT_SOCK,
	[EXT2_FT_SYMLINK]	= BTRFS_FT_SYMLINK,
};

static int ext2_dir_iterate_proc(ext2_ino_t dir, int entry,
			    struct ext2_dir_entry *dirent,
			    int offset, int blocksize,
			    char *buf,void *priv_data)
{
	int ret;
	int file_type;
	u64 objectid;
	char dotdot[] = "..";
	struct dir_iterate_data *idata = (struct dir_iterate_data *)priv_data;
	int name_len;

	name_len = dirent->name_len & 0xFF;

	objectid = dirent->inode + INO_OFFSET;
	if (!strncmp(dirent->name, dotdot, name_len)) {
		if (name_len == 2) {
			BUG_ON(idata->parent != 0);
			idata->parent = objectid;
		}
		return 0;
	}
	if (dirent->inode < EXT2_GOOD_OLD_FIRST_INO)
		return 0;

	file_type = dirent->name_len >> 8;
	BUG_ON(file_type > EXT2_FT_SYMLINK);

	ret = convert_insert_dirent(idata->trans, idata->root, dirent->name,
				    name_len, idata->objectid, objectid,
				    ext2_filetype_conversion_table[file_type],
				    idata->index_cnt, idata->inode);
	if (ret < 0) {
		idata->errcode = ret;
		return BLOCK_ABORT;
	}

	idata->index_cnt++;
	return 0;
}

static int ext2_create_dir_entries(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *btrfs_inode,
			      ext2_filsys ext2_fs, ext2_ino_t ext2_ino)
{
	int ret;
	errcode_t err;
	struct dir_iterate_data data = {
		.trans		= trans,
		.root		= root,
		.inode		= btrfs_inode,
		.objectid	= objectid,
		.index_cnt	= 2,
		.parent		= 0,
		.errcode	= 0,
	};

	err = ext2fs_dir_iterate2(ext2_fs, ext2_ino, 0, NULL,
				  ext2_dir_iterate_proc, &data);
	if (err)
		goto error;
	ret = data.errcode;
	if (ret == 0 && data.parent == objectid) {
		ret = btrfs_insert_inode_ref(trans, root, "..", 2,
					     objectid, objectid, 0);
	}
	return ret;
error:
	fprintf(stderr, "ext2fs_dir_iterate2: %s\n", error_message(err));
	return -1;
}

static int ext2_block_iterate_proc(ext2_filsys fs, blk_t *blocknr,
			        e2_blkcnt_t blockcnt, blk_t ref_block,
			        int ref_offset, void *priv_data)
{
	int ret;
	struct blk_iterate_data *idata;
	idata = (struct blk_iterate_data *)priv_data;
	ret = block_iterate_proc(*blocknr, blockcnt, idata);
	if (ret) {
		idata->errcode = ret;
		return BLOCK_ABORT;
	}
	return 0;
}

/*
 * traverse file's data blocks, record these data blocks as file extents.
 */
static int ext2_create_file_extents(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 objectid,
			       struct btrfs_inode_item *btrfs_inode,
			       ext2_filsys ext2_fs, ext2_ino_t ext2_ino,
			       u32 convert_flags)
{
	int ret;
	char *buffer = NULL;
	errcode_t err;
	u32 last_block;
	u32 sectorsize = root->fs_info->sectorsize;
	u64 inode_size = btrfs_stack_inode_size(btrfs_inode);
	struct blk_iterate_data data;

	init_blk_iterate_data(&data, trans, root, btrfs_inode, objectid,
			convert_flags & CONVERT_FLAG_DATACSUM);

	err = ext2fs_block_iterate2(ext2_fs, ext2_ino, BLOCK_FLAG_DATA_ONLY,
				    NULL, ext2_block_iterate_proc, &data);
	if (err)
		goto error;
	ret = data.errcode;
	if (ret)
		goto fail;
	if ((convert_flags & CONVERT_FLAG_INLINE_DATA) && data.first_block == 0
	    && data.num_blocks > 0 && inode_size < sectorsize
	    && inode_size <= BTRFS_MAX_INLINE_DATA_SIZE(root->fs_info)) {
		u64 num_bytes = data.num_blocks * sectorsize;
		u64 disk_bytenr = data.disk_block * sectorsize;
		u64 nbytes;

		buffer = malloc(num_bytes);
		if (!buffer)
			return -ENOMEM;
		ret = read_disk_extent(root, disk_bytenr, num_bytes, buffer);
		if (ret)
			goto fail;
		if (num_bytes > inode_size)
			num_bytes = inode_size;
		ret = btrfs_insert_inline_extent(trans, root, objectid,
						 0, buffer, num_bytes);
		if (ret)
			goto fail;
		nbytes = btrfs_stack_inode_nbytes(btrfs_inode) + num_bytes;
		btrfs_set_stack_inode_nbytes(btrfs_inode, nbytes);
	} else if (data.num_blocks > 0) {
		ret = record_file_blocks(&data, data.first_block,
					 data.disk_block, data.num_blocks);
		if (ret)
			goto fail;
	}
	data.first_block += data.num_blocks;
	last_block = (inode_size + sectorsize - 1) / sectorsize;
	if (last_block > data.first_block) {
		ret = record_file_blocks(&data, data.first_block, 0,
					 last_block - data.first_block);
	}
fail:
	free(buffer);
	return ret;
error:
	fprintf(stderr, "ext2fs_block_iterate2: %s\n", error_message(err));
	return -1;
}

static int ext2_create_symlink(struct btrfs_trans_handle *trans,
			      struct btrfs_root *root, u64 objectid,
			      struct btrfs_inode_item *btrfs_inode,
			      ext2_filsys ext2_fs, ext2_ino_t ext2_ino,
			      struct ext2_inode *ext2_inode)
{
	int ret;
	char *pathname;
	u64 inode_size = btrfs_stack_inode_size(btrfs_inode);
	if (ext2fs_inode_data_blocks2(ext2_fs, ext2_inode)) {
		btrfs_set_stack_inode_size(btrfs_inode, inode_size + 1);
		ret = ext2_create_file_extents(trans, root, objectid,
				btrfs_inode, ext2_fs, ext2_ino,
				CONVERT_FLAG_DATACSUM |
				CONVERT_FLAG_INLINE_DATA);
		btrfs_set_stack_inode_size(btrfs_inode, inode_size);
		return ret;
	}

	pathname = (char *)&(ext2_inode->i_block[0]);
	BUG_ON(pathname[inode_size] != 0);
	ret = btrfs_insert_inline_extent(trans, root, objectid, 0,
					 pathname, inode_size + 1);
	btrfs_set_stack_inode_nbytes(btrfs_inode, inode_size + 1);
	return ret;
}

/*
 * Following xattr/acl related codes are based on codes in
 * fs/ext3/xattr.c and fs/ext3/acl.c
 */
#define EXT2_XATTR_BHDR(ptr) ((struct ext2_ext_attr_header *)(ptr))
#define EXT2_XATTR_BFIRST(ptr) \
	((struct ext2_ext_attr_entry *)(EXT2_XATTR_BHDR(ptr) + 1))
#define EXT2_XATTR_IHDR(inode) \
	((struct ext2_ext_attr_header *) ((void *)(inode) + \
		EXT2_GOOD_OLD_INODE_SIZE + (inode)->i_extra_isize))
#define EXT2_XATTR_IFIRST(inode) \
	((struct ext2_ext_attr_entry *) ((void *)EXT2_XATTR_IHDR(inode) + \
		sizeof(EXT2_XATTR_IHDR(inode)->h_magic)))

static int ext2_xattr_check_names(struct ext2_ext_attr_entry *entry,
				  const void *end)
{
	struct ext2_ext_attr_entry *next;

	while (!EXT2_EXT_IS_LAST_ENTRY(entry)) {
		next = EXT2_EXT_ATTR_NEXT(entry);
		if ((void *)next >= end)
			return -EIO;
		entry = next;
	}
	return 0;
}

static int ext2_xattr_check_block(const char *buf, size_t size)
{
	int error;
	struct ext2_ext_attr_header *header = EXT2_XATTR_BHDR(buf);

	if (header->h_magic != EXT2_EXT_ATTR_MAGIC ||
	    header->h_blocks != 1)
		return -EIO;
	error = ext2_xattr_check_names(EXT2_XATTR_BFIRST(buf), buf + size);
	return error;
}

static int ext2_xattr_check_entry(struct ext2_ext_attr_entry *entry,
				  size_t size)
{
	size_t value_size = entry->e_value_size;

	if (value_size > size || entry->e_value_offs + value_size > size)
		return -EIO;
	return 0;
}

static int ext2_acl_to_xattr(void *dst, const void *src,
			     size_t dst_size, size_t src_size)
{
	int i, count;
	const void *end = src + src_size;
	acl_ea_header *ext_acl = (acl_ea_header *)dst;
	acl_ea_entry *dst_entry = ext_acl->a_entries;
	ext2_acl_entry *src_entry;

	if (src_size < sizeof(ext2_acl_header))
		goto fail;
	if (((ext2_acl_header *)src)->a_version !=
	    cpu_to_le32(EXT2_ACL_VERSION))
		goto fail;
	src += sizeof(ext2_acl_header);
	count = ext2_acl_count(src_size);
	if (count <= 0)
		goto fail;

	BUG_ON(dst_size < acl_ea_size(count));
	ext_acl->a_version = cpu_to_le32(ACL_EA_VERSION);
	for (i = 0; i < count; i++, dst_entry++) {
		src_entry = (ext2_acl_entry *)src;
		if (src + sizeof(ext2_acl_entry_short) > end)
			goto fail;
		dst_entry->e_tag = src_entry->e_tag;
		dst_entry->e_perm = src_entry->e_perm;
		switch (le16_to_cpu(src_entry->e_tag)) {
		case ACL_USER_OBJ:
		case ACL_GROUP_OBJ:
		case ACL_MASK:
		case ACL_OTHER:
			src += sizeof(ext2_acl_entry_short);
			dst_entry->e_id = cpu_to_le32(ACL_UNDEFINED_ID);
			break;
		case ACL_USER:
		case ACL_GROUP:
			src += sizeof(ext2_acl_entry);
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

static char *xattr_prefix_table[] = {
	[1] =	"user.",
	[2] =	"system.posix_acl_access",
	[3] =	"system.posix_acl_default",
	[4] =	"trusted.",
	[6] =	"security.",
};

static int ext2_copy_single_xattr(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 objectid,
			     struct ext2_ext_attr_entry *entry,
			     const void *data, u32 datalen)
{
	int ret = 0;
	int name_len;
	int name_index;
	void *databuf = NULL;
	char namebuf[XATTR_NAME_MAX + 1];

	name_index = entry->e_name_index;
	if (name_index >= ARRAY_SIZE(xattr_prefix_table) ||
	    xattr_prefix_table[name_index] == NULL)
		return -EOPNOTSUPP;
	name_len = strlen(xattr_prefix_table[name_index]) +
		   entry->e_name_len;
	if (name_len >= sizeof(namebuf))
		return -ERANGE;

	if (name_index == 2 || name_index == 3) {
		size_t bufsize = acl_ea_size(ext2_acl_count(datalen));
		databuf = malloc(bufsize);
		if (!databuf)
		       return -ENOMEM;
		ret = ext2_acl_to_xattr(databuf, data, bufsize, datalen);
		if (ret)
			goto out;
		data = databuf;
		datalen = bufsize;
	}
	strncpy(namebuf, xattr_prefix_table[name_index], XATTR_NAME_MAX);
	strncat(namebuf, EXT2_EXT_ATTR_NAME(entry), entry->e_name_len);
	if (name_len + datalen > BTRFS_LEAF_DATA_SIZE(root->fs_info) -
	    sizeof(struct btrfs_item) - sizeof(struct btrfs_dir_item)) {
		fprintf(stderr, "skip large xattr on inode %Lu name %.*s\n",
			objectid - INO_OFFSET, name_len, namebuf);
		goto out;
	}
	ret = btrfs_insert_xattr_item(trans, root, namebuf, name_len,
				      data, datalen, objectid);
out:
	free(databuf);
	return ret;
}

static int ext2_copy_extended_attrs(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root, u64 objectid,
			       struct btrfs_inode_item *btrfs_inode,
			       ext2_filsys ext2_fs, ext2_ino_t ext2_ino)
{
	int ret = 0;
	int inline_ea = 0;
	errcode_t err;
	u32 datalen;
	u32 block_size = ext2_fs->blocksize;
	u32 inode_size = EXT2_INODE_SIZE(ext2_fs->super);
	struct ext2_inode_large *ext2_inode;
	struct ext2_ext_attr_entry *entry;
	void *data;
	char *buffer = NULL;
	char inode_buf[EXT2_GOOD_OLD_INODE_SIZE];

	if (inode_size <= EXT2_GOOD_OLD_INODE_SIZE) {
		ext2_inode = (struct ext2_inode_large *)inode_buf;
	} else {
		ext2_inode = (struct ext2_inode_large *)malloc(inode_size);
		if (!ext2_inode)
		       return -ENOMEM;
	}
	err = ext2fs_read_inode_full(ext2_fs, ext2_ino, (void *)ext2_inode,
				     inode_size);
	if (err) {
		fprintf(stderr, "ext2fs_read_inode_full: %s\n",
			error_message(err));
		ret = -1;
		goto out;
	}

	if (ext2_ino > ext2_fs->super->s_first_ino &&
	    inode_size > EXT2_GOOD_OLD_INODE_SIZE) {
		if (EXT2_GOOD_OLD_INODE_SIZE +
		    ext2_inode->i_extra_isize > inode_size) {
			ret = -EIO;
			goto out;
		}
		if (ext2_inode->i_extra_isize != 0 &&
		    EXT2_XATTR_IHDR(ext2_inode)->h_magic ==
		    EXT2_EXT_ATTR_MAGIC) {
			inline_ea = 1;
		}
	}
	if (inline_ea) {
		int total;
		void *end = (void *)ext2_inode + inode_size;
		entry = EXT2_XATTR_IFIRST(ext2_inode);
		total = end - (void *)entry;
		ret = ext2_xattr_check_names(entry, end);
		if (ret)
			goto out;
		while (!EXT2_EXT_IS_LAST_ENTRY(entry)) {
			ret = ext2_xattr_check_entry(entry, total);
			if (ret)
				goto out;
			data = (void *)EXT2_XATTR_IFIRST(ext2_inode) +
				entry->e_value_offs;
			datalen = entry->e_value_size;
			ret = ext2_copy_single_xattr(trans, root, objectid,
						entry, data, datalen);
			if (ret)
				goto out;
			entry = EXT2_EXT_ATTR_NEXT(entry);
		}
	}

	if (ext2_inode->i_file_acl == 0)
		goto out;

	buffer = malloc(block_size);
	if (!buffer) {
		ret = -ENOMEM;
		goto out;
	}
	err = ext2fs_read_ext_attr2(ext2_fs, ext2_inode->i_file_acl, buffer);
	if (err) {
		fprintf(stderr, "ext2fs_read_ext_attr2: %s\n",
			error_message(err));
		ret = -1;
		goto out;
	}
	ret = ext2_xattr_check_block(buffer, block_size);
	if (ret)
		goto out;

	entry = EXT2_XATTR_BFIRST(buffer);
	while (!EXT2_EXT_IS_LAST_ENTRY(entry)) {
		ret = ext2_xattr_check_entry(entry, block_size);
		if (ret)
			goto out;
		data = buffer + entry->e_value_offs;
		datalen = entry->e_value_size;
		ret = ext2_copy_single_xattr(trans, root, objectid,
					entry, data, datalen);
		if (ret)
			goto out;
		entry = EXT2_EXT_ATTR_NEXT(entry);
	}
out:
	free(buffer);
	if ((void *)ext2_inode != inode_buf)
		free(ext2_inode);
	return ret;
}

static inline dev_t old_decode_dev(u16 val)
{
	return MKDEV((val >> 8) & 255, val & 255);
}

static void ext2_copy_inode_item(struct btrfs_inode_item *dst,
			   struct ext2_inode *src, u32 blocksize)
{
	btrfs_set_stack_inode_generation(dst, 1);
	btrfs_set_stack_inode_sequence(dst, 0);
	btrfs_set_stack_inode_transid(dst, 1);
	btrfs_set_stack_inode_size(dst, src->i_size);
	btrfs_set_stack_inode_nbytes(dst, 0);
	btrfs_set_stack_inode_block_group(dst, 0);
	btrfs_set_stack_inode_nlink(dst, src->i_links_count);
	btrfs_set_stack_inode_uid(dst, src->i_uid | (src->i_uid_high << 16));
	btrfs_set_stack_inode_gid(dst, src->i_gid | (src->i_gid_high << 16));
	btrfs_set_stack_inode_mode(dst, src->i_mode);
	btrfs_set_stack_inode_rdev(dst, 0);
	btrfs_set_stack_inode_flags(dst, 0);
	btrfs_set_stack_timespec_sec(&dst->atime, src->i_atime);
	btrfs_set_stack_timespec_nsec(&dst->atime, 0);
	btrfs_set_stack_timespec_sec(&dst->ctime, src->i_ctime);
	btrfs_set_stack_timespec_nsec(&dst->ctime, 0);
	btrfs_set_stack_timespec_sec(&dst->mtime, src->i_mtime);
	btrfs_set_stack_timespec_nsec(&dst->mtime, 0);
	btrfs_set_stack_timespec_sec(&dst->otime, 0);
	btrfs_set_stack_timespec_nsec(&dst->otime, 0);

	if (S_ISDIR(src->i_mode)) {
		btrfs_set_stack_inode_size(dst, 0);
		btrfs_set_stack_inode_nlink(dst, 1);
	}
	if (S_ISREG(src->i_mode)) {
		btrfs_set_stack_inode_size(dst, (u64)src->i_size_high << 32 |
					   (u64)src->i_size);
	}
	if (!S_ISREG(src->i_mode) && !S_ISDIR(src->i_mode) &&
	    !S_ISLNK(src->i_mode)) {
		if (src->i_block[0]) {
			btrfs_set_stack_inode_rdev(dst,
				old_decode_dev(src->i_block[0]));
		} else {
			btrfs_set_stack_inode_rdev(dst,
				decode_dev(src->i_block[1]));
		}
	}
	memset(&dst->reserved, 0, sizeof(dst->reserved));
}

#if HAVE_OWN_EXT4_EPOCH_MASK_DEFINE

/*
 * Copied and modified from fs/ext4/ext4.h
 */
static inline void ext4_decode_extra_time(__le32 * tv_sec, __le32 * tv_nsec,
                                          __le32 extra)
{
        if (extra & cpu_to_le32(EXT4_EPOCH_MASK))
                *tv_sec += (u64)(le32_to_cpu(extra) & EXT4_EPOCH_MASK) << 32;
        *tv_nsec = (le32_to_cpu(extra) & EXT4_NSEC_MASK) >> EXT4_EPOCH_BITS;
}

#define EXT4_COPY_XTIME(xtime, dst, tv_sec, tv_nsec)					\
do {											\
	tv_sec = src->i_ ## xtime ;							\
	if (inode_includes(inode_size, i_ ## xtime ## _extra)) {			\
		tv_sec = src->i_ ## xtime ;						\
		ext4_decode_extra_time(&tv_sec, &tv_nsec, src->i_ ## xtime ## _extra);	\
		btrfs_set_stack_timespec_sec(&dst->xtime , tv_sec);			\
		btrfs_set_stack_timespec_nsec(&dst->xtime , tv_nsec);			\
	} else {									\
		btrfs_set_stack_timespec_sec(&dst->xtime , tv_sec);			\
		btrfs_set_stack_timespec_nsec(&dst->xtime , 0);				\
	}										\
} while (0);

/*
 * Decode and copy i_[cma]time_extra and i_crtime{,_extra} field
 */
static int ext4_copy_inode_timespec_extra(struct btrfs_inode_item *dst,
				ext2_ino_t ext2_ino, u32 s_inode_size,
				ext2_filsys ext2_fs)
{
	struct ext2_inode_large *src;
	u32 inode_size, tv_sec, tv_nsec;
	int ret, err;
	ret = 0;

	src = (struct ext2_inode_large *)malloc(s_inode_size);
	if (!src)
		return -ENOMEM;
	err = ext2fs_read_inode_full(ext2_fs, ext2_ino, (void *)src,
				     s_inode_size);
	if (err) {
		fprintf(stderr, "ext2fs_read_inode_full: %s\n", error_message(err));
		ret = -1;
		goto out;
	}

	inode_size = EXT2_GOOD_OLD_INODE_SIZE + src->i_extra_isize;

	EXT4_COPY_XTIME(atime, dst, tv_sec, tv_nsec);
	EXT4_COPY_XTIME(mtime, dst, tv_sec, tv_nsec);
	EXT4_COPY_XTIME(ctime, dst, tv_sec, tv_nsec);

	tv_sec = src->i_crtime;
	if (inode_includes(inode_size, i_crtime_extra)) {
		tv_sec = src->i_crtime;
		ext4_decode_extra_time(&tv_sec, &tv_nsec, src->i_crtime_extra);
		btrfs_set_stack_timespec_sec(&dst->otime, tv_sec);
		btrfs_set_stack_timespec_nsec(&dst->otime, tv_nsec);
	} else {
		btrfs_set_stack_timespec_sec(&dst->otime, tv_sec);
		btrfs_set_stack_timespec_nsec(&dst->otime, 0);
	}
out:
	free(src);
	return ret;
}

#else /* HAVE_OWN_EXT4_EPOCH_MASK_DEFINE */

static int ext4_copy_inode_timespec_extra(struct btrfs_inode_item *dst,
				ext2_ino_t ext2_ino, u32 s_inode_size,
				ext2_filsys ext2_fs)
{
	static int warn = 0;

	if (!warn) {
		warning(
"extended inode (size %u) found but e2fsprogs don't support reading extra timespec",
			s_inode_size);
		warn = 1;
	}
	return 0;
}

#endif /* !HAVE_OWN_EXT4_EPOCH_MASK_DEFINE */

static int ext2_check_state(struct btrfs_convert_context *cctx)
{
	ext2_filsys fs = cctx->fs_data;

        if (!(fs->super->s_state & EXT2_VALID_FS))
		return 1;
	else if (fs->super->s_state & EXT2_ERROR_FS)
		return 1;
	else
		return 0;
}

/* EXT2_*_FL to BTRFS_INODE_FLAG_* stringification helper */
#define COPY_ONE_EXT2_FLAG(flags, ext2_inode, name) ({			\
	if (ext2_inode->i_flags & EXT2_##name##_FL)			\
		flags |= BTRFS_INODE_##name;				\
})

/*
 * Convert EXT2_*_FL to corresponding BTRFS_INODE_* flags
 *
 * Only a subset of EXT_*_FL is supported in btrfs.
 */
static void ext2_convert_inode_flags(struct btrfs_inode_item *dst,
				     struct ext2_inode *src)
{
	u64 flags = btrfs_stack_inode_flags(dst);

	COPY_ONE_EXT2_FLAG(flags, src, APPEND);
	COPY_ONE_EXT2_FLAG(flags, src, SYNC);
	COPY_ONE_EXT2_FLAG(flags, src, IMMUTABLE);
	COPY_ONE_EXT2_FLAG(flags, src, NODUMP);
	COPY_ONE_EXT2_FLAG(flags, src, NOATIME);
	COPY_ONE_EXT2_FLAG(flags, src, DIRSYNC);
	btrfs_set_stack_inode_flags(dst, flags);
}

/*
 * copy a single inode. do all the required works, such as cloning
 * inode item, creating file extents and creating directory entries.
 */
static int ext2_copy_single_inode(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root, u64 objectid,
			     ext2_filsys ext2_fs, ext2_ino_t ext2_ino,
			     struct ext2_inode *ext2_inode,
			     u32 convert_flags)
{
	int ret;
	int s_inode_size;
	struct btrfs_inode_item btrfs_inode;

	if (ext2_inode->i_links_count == 0)
		return 0;

	ext2_copy_inode_item(&btrfs_inode, ext2_inode, ext2_fs->blocksize);
	s_inode_size = EXT2_INODE_SIZE(ext2_fs->super);
	if (s_inode_size > EXT2_GOOD_OLD_INODE_SIZE) {
		ret = ext4_copy_inode_timespec_extra(&btrfs_inode, ext2_ino,
				s_inode_size, ext2_fs);
		if (ret)
			return ret;
	}

	if (!(convert_flags & CONVERT_FLAG_DATACSUM)
	    && S_ISREG(ext2_inode->i_mode)) {
		u32 flags = btrfs_stack_inode_flags(&btrfs_inode) |
			    BTRFS_INODE_NODATASUM;
		btrfs_set_stack_inode_flags(&btrfs_inode, flags);
	}
	ext2_convert_inode_flags(&btrfs_inode, ext2_inode);

	switch (ext2_inode->i_mode & S_IFMT) {
	case S_IFREG:
		ret = ext2_create_file_extents(trans, root, objectid,
			&btrfs_inode, ext2_fs, ext2_ino, convert_flags);
		break;
	case S_IFDIR:
		ret = ext2_create_dir_entries(trans, root, objectid,
				&btrfs_inode, ext2_fs, ext2_ino);
		break;
	case S_IFLNK:
		ret = ext2_create_symlink(trans, root, objectid,
				&btrfs_inode, ext2_fs, ext2_ino, ext2_inode);
		break;
	default:
		ret = 0;
		break;
	}
	if (ret)
		return ret;

	if (convert_flags & CONVERT_FLAG_XATTR) {
		ret = ext2_copy_extended_attrs(trans, root, objectid,
				&btrfs_inode, ext2_fs, ext2_ino);
		if (ret)
			return ret;
	}
	return btrfs_insert_inode(trans, root, objectid, &btrfs_inode);
}

static int ext2_is_special_inode(ext2_ino_t ino)
{
	if (ino < EXT2_GOOD_OLD_FIRST_INO && ino != EXT2_ROOT_INO)
		return 1;
	return 0;
}

/*
 * scan ext2's inode bitmap and copy all used inodes.
 */
static int ext2_copy_inodes(struct btrfs_convert_context *cctx,
			    struct btrfs_root *root,
			    u32 convert_flags, struct task_ctx *p)
{
	ext2_filsys ext2_fs = cctx->fs_data;
	int ret = 0;
	errcode_t err;
	ext2_inode_scan ext2_scan;
	struct ext2_inode ext2_inode;
	ext2_ino_t ext2_ino;
	u64 objectid;
	struct btrfs_trans_handle *trans;

	trans = btrfs_start_transaction(root, 1);
	if (IS_ERR(trans))
		return PTR_ERR(trans);
	err = ext2fs_open_inode_scan(ext2_fs, 0, &ext2_scan);
	if (err) {
		error("ext2fs_open_inode_scan failed: %s", error_message(err));
		btrfs_commit_transaction(trans, root);
		return -EIO;
	}
	while (!(err = ext2fs_get_next_inode(ext2_scan, &ext2_ino,
					     &ext2_inode))) {
		/* no more inodes */
		if (ext2_ino == 0)
			break;
		if (ext2_is_special_inode(ext2_ino))
			continue;
		objectid = ext2_ino + INO_OFFSET;
		ret = ext2_copy_single_inode(trans, root,
					objectid, ext2_fs, ext2_ino,
					&ext2_inode, convert_flags);
		pthread_mutex_lock(&p->mutex);
		p->cur_copy_inodes++;
		pthread_mutex_unlock(&p->mutex);
		if (ret) {
			error("failed to copy ext2 inode %llu: %d",
					(unsigned long long)ext2_ino, ret);
			goto out;
		}
		/*
		 * blocks_used is the number of new tree blocks allocated in
		 * current transaction.
		 * Use a small amount of it to workaround a bug where delayed
		 * ref may fail to locate tree blocks in extent tree.
		 *
		 * 2M is the threshold to kick chunk preallocator into work,
		 * For default (16K) nodesize it will be 128 tree blocks,
		 * large enough to contain over 300 inlined files or
		 * around 26k file extents. Which should be good enough.
		 */
		if (trans->blocks_used >= SZ_2M / root->fs_info->nodesize) {
			ret = btrfs_commit_transaction(trans, root);
			if (ret < 0) {
				error("failed to commit transaction: %d", ret);
				goto out;
			}
			trans = btrfs_start_transaction(root, 1);
			if (IS_ERR(trans)) {
				ret = PTR_ERR(trans);
				error("failed to start transaction: %d", ret);
				trans = NULL;
				goto out;
			}
		}
	}
	if (err) {
		error("ext2fs_get_next_inode failed: %s", error_message(err));
		ret = -EIO;
		goto out;
	}
out:
	if (ret < 0) {
		if (trans)
			btrfs_abort_transaction(trans, ret);
	} else {
		ret = btrfs_commit_transaction(trans, root);
		if (ret < 0)
			error("failed to commit transaction: %d", ret);
	}
	ext2fs_close_inode_scan(ext2_scan);

	return ret;
}

const struct btrfs_convert_operations ext2_convert_ops = {
	.name			= "ext2",
	.open_fs		= ext2_open_fs,
	.read_used_space	= ext2_read_used_space,
	.copy_inodes		= ext2_copy_inodes,
	.close_fs		= ext2_close_fs,
	.check_state		= ext2_check_state,
};

#endif	/* BTRFSCONVERT_EXT2 */
