/*
 * Copyright (C) 2011 Red Hat.  All rights reserved.
 *
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
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <regex.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#if COMPRESSION_LZO
#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>
#endif
#include <zlib.h>
#if COMPRESSION_ZSTD
#include <zstd.h>
#endif
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/compression.h"
#include "common/utils.h"
#include "common/help.h"
#include "common/open-utils.h"
#include "common/string-utils.h"
#include "common/messages.h"
#include "cmds/commands.h"

static char fs_name[PATH_MAX];
static char path_name[PATH_MAX];
static char symlink_target[PATH_MAX];
static int get_snaps = 0;
static int restore_metadata = 0;
static int restore_symlinks = 0;
static int ignore_errors = 0;
static int overwrite = 0;
static int get_xattrs = 0;
static int dry_run = 0;

#define LZO_LEN 4
#define lzo1x_worst_compress(x) ((x) + ((x) / 16) + 64 + 3)

static int decompress_zlib(char *inbuf, char *outbuf, u64 compress_len,
			   u64 decompress_len)
{
	z_stream strm;
	int ret;

	memset(&strm, 0, sizeof(strm));
	ret = inflateInit(&strm);
	if (ret != Z_OK) {
		error("zlib init returned %d", ret);
		return -1;
	}

	strm.avail_in = compress_len;
	strm.next_in = (unsigned char *)inbuf;
	strm.avail_out = decompress_len;
	strm.next_out = (unsigned char *)outbuf;
	ret = inflate(&strm, Z_NO_FLUSH);
	if (ret != Z_STREAM_END) {
		(void)inflateEnd(&strm);
		error("zlib inflate failed: %d", ret);
		return -1;
	}

	(void)inflateEnd(&strm);
	return 0;
}
static inline size_t read_compress_length(unsigned char *buf)
{
	__le32 dlen;
	memcpy(&dlen, buf, LZO_LEN);
	return le32_to_cpu(dlen);
}

static int decompress_lzo(struct btrfs_root *root, unsigned char *inbuf,
			char *outbuf, u64 compress_len, u64 *decompress_len)
{
#if !COMPRESSION_LZO
	error("btrfs-restore not compiled with lzo support");
	return -1;
#else
	size_t new_len;
	size_t in_len;
	size_t out_len = 0;
	size_t tot_len;
	size_t tot_in;
	int ret;

	ret = lzo_init();
	if (ret != LZO_E_OK) {
		error("lzo init returned %d", ret);
		return -1;
	}

	tot_len = read_compress_length(inbuf);
	inbuf += LZO_LEN;
	tot_in = LZO_LEN;

	while (tot_in < tot_len) {
		size_t mod_page;
		size_t rem_page;
		in_len = read_compress_length(inbuf);

		if ((tot_in + LZO_LEN + in_len) > tot_len) {
			error("bad compress length %lu",
				(unsigned long)in_len);
			return -1;
		}

		inbuf += LZO_LEN;
		tot_in += LZO_LEN;
		new_len = lzo1x_worst_compress(root->fs_info->sectorsize);
		ret = lzo1x_decompress_safe((const unsigned char *)inbuf, in_len,
					    (unsigned char *)outbuf,
					    (void *)&new_len, NULL);
		if (ret != LZO_E_OK) {
			error("lzo decompress failed: %d", ret);
			return -1;
		}
		out_len += new_len;
		outbuf += new_len;
		inbuf += in_len;
		tot_in += in_len;

		/*
		 * If the 4 byte header does not fit to the rest of the page we
		 * have to move to the next one, unless we read some garbage
		 */
		mod_page = tot_in % root->fs_info->sectorsize;
		rem_page = root->fs_info->sectorsize - mod_page;
		if (rem_page < LZO_LEN) {
			inbuf += rem_page;
			tot_in += rem_page;
		}
	}

	*decompress_len = out_len;

	return 0;
#endif
}

static int decompress_zstd(const char *inbuf, char *outbuf, u64 compress_len,
			   u64 decompress_len)
{
#if !COMPRESSION_ZSTD
	error("btrfs not compiled with zstd support");
	return -1;
#else
	ZSTD_DStream *strm;
	size_t zret;
	int ret = 0;
	ZSTD_inBuffer in = {inbuf, compress_len, 0};
	ZSTD_outBuffer out = {outbuf, decompress_len, 0};

	strm = ZSTD_createDStream();
	if (!strm) {
		error("zstd create failed");
		return -1;
	}

	zret = ZSTD_initDStream(strm);
	if (ZSTD_isError(zret)) {
		error("zstd init failed: %s", ZSTD_getErrorName(zret));
		ret = -1;
		goto out;
	}

	zret = ZSTD_decompressStream(strm, &out, &in);
	if (ZSTD_isError(zret)) {
		error("zstd decompress failed %s\n", ZSTD_getErrorName(zret));
		ret = -1;
		goto out;
	}
	if (zret != 0) {
		error("zstd frame incomplete");
		ret = -1;
		goto out;
	}

out:
	ZSTD_freeDStream(strm);
	return ret;
#endif
}

static int decompress(struct btrfs_root *root, char *inbuf, char *outbuf,
			u64 compress_len, u64 *decompress_len, int compress)
{
	switch (compress) {
	case BTRFS_COMPRESS_ZLIB:
		return decompress_zlib(inbuf, outbuf, compress_len,
				       *decompress_len);
	case BTRFS_COMPRESS_LZO:
		return decompress_lzo(root, (unsigned char *)inbuf, outbuf,
					compress_len, decompress_len);
	case BTRFS_COMPRESS_ZSTD:
		return decompress_zstd(inbuf, outbuf, compress_len,
				       *decompress_len);
	default:
		break;
	}

	error("invalid compression type: %d", compress);
	return -1;
}

static int next_leaf(struct btrfs_root *root, struct btrfs_path *path)
{
	int slot;
	int level = 1;
	int offset = 1;
	struct extent_buffer *c;
	struct extent_buffer *next = NULL;
	struct btrfs_fs_info *fs_info = root->fs_info;

again:
	for (; level < BTRFS_MAX_LEVEL; level++) {
		if (path->nodes[level])
			break;
	}

	if (level >= BTRFS_MAX_LEVEL)
		return 1;

	slot = path->slots[level] + 1;

	while(level < BTRFS_MAX_LEVEL) {
		if (!path->nodes[level])
			return 1;

		slot = path->slots[level] + offset;
		c = path->nodes[level];
		if (slot >= btrfs_header_nritems(c)) {
			level++;
			if (level == BTRFS_MAX_LEVEL)
				return 1;
			offset = 1;
			continue;
		}

		if (path->reada)
			reada_for_search(fs_info, path, level, slot, 0);

		next = read_node_slot(fs_info, c, slot);
		if (extent_buffer_uptodate(next))
			break;
		offset++;
	}
	path->slots[level] = slot;
	while(1) {
		level--;
		c = path->nodes[level];
		free_extent_buffer(c);
		path->nodes[level] = next;
		path->slots[level] = 0;
		if (!level)
			break;
		if (path->reada)
			reada_for_search(fs_info, path, level, 0, 0);
		next = read_node_slot(fs_info, next, 0);
		if (!extent_buffer_uptodate(next))
			goto again;
	}
	return 0;
}

static int copy_one_inline(struct btrfs_root *root, int fd,
				struct btrfs_path *path, u64 pos)
{
	struct extent_buffer *leaf = path->nodes[0];
	struct btrfs_file_extent_item *fi;
	char buf[4096];
	char *outbuf;
	u64 ram_size;
	ssize_t done;
	unsigned long ptr;
	int ret;
	int len;
	int inline_item_len;
	int compress;

	fi = btrfs_item_ptr(leaf, path->slots[0],
			    struct btrfs_file_extent_item);
	ptr = btrfs_file_extent_inline_start(fi);
	len = btrfs_file_extent_ram_bytes(leaf, fi);
	inline_item_len = btrfs_file_extent_inline_item_len(leaf, path->slots[0]);
	read_extent_buffer(leaf, buf, ptr, inline_item_len);

	compress = btrfs_file_extent_compression(leaf, fi);
	if (compress == BTRFS_COMPRESS_NONE) {
		done = pwrite(fd, buf, len, pos);
		if (done < len) {
			error("short inline write, wanted %d, did %zd: %m",
					len, done);
			return -1;
		}
		return 0;
	}

	ram_size = btrfs_file_extent_ram_bytes(leaf, fi);
	outbuf = calloc(1, ram_size);
	if (!outbuf) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		return -ENOMEM;
	}

	ret = decompress(root, buf, outbuf, inline_item_len, &ram_size,
			 compress);
	if (ret) {
		free(outbuf);
		return ret;
	}

	done = pwrite(fd, outbuf, ram_size, pos);
	free(outbuf);
	if (done < ram_size) {
		error("short compressed inline write, wanted %llu, did %zd: %m",
				ram_size, done);
		return -1;
	}

	return 0;
}

static int copy_one_extent(struct btrfs_root *root, int fd,
			   struct extent_buffer *leaf,
			   struct btrfs_file_extent_item *fi, u64 pos)
{
	char *inbuf, *outbuf = NULL;
	ssize_t done, total = 0;
	u64 bytenr;
	u64 ram_size;
	u64 disk_size;
	u64 num_bytes;
	u64 length;
	u64 size_left;
	u64 offset;
	u64 cur;
	int compress;
	int ret;
	int mirror_num = 1;
	int num_copies;

	compress = btrfs_file_extent_compression(leaf, fi);
	bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
	disk_size = btrfs_file_extent_disk_num_bytes(leaf, fi);
	ram_size = btrfs_file_extent_ram_bytes(leaf, fi);
	offset = btrfs_file_extent_offset(leaf, fi);
	num_bytes = btrfs_file_extent_num_bytes(leaf, fi);
	size_left = disk_size;
	/* Hole, early exit */
	if (disk_size == 0)
		return 0;

	/* Invalid file extent */
	if ((compress == BTRFS_COMPRESS_NONE && offset >= disk_size) ||
	    offset > ram_size) {
		error(
	"invalid data extent offset, offset %llu disk_size %llu ram_size %llu",
		      offset, disk_size, ram_size);
		return -EUCLEAN;
	}

	if (compress == BTRFS_COMPRESS_NONE && offset < disk_size) {
		bytenr += offset;
		size_left -= offset;
	}

	pr_verbose(offset ? 1 : 0, "offset is %llu\n", offset);

	inbuf = malloc(size_left);
	if (!inbuf) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		return -ENOMEM;
	}

	if (compress != BTRFS_COMPRESS_NONE) {
		outbuf = calloc(1, ram_size);
		if (!outbuf) {
			error_msg(ERROR_MSG_MEMORY, NULL);
			free(inbuf);
			return -ENOMEM;
		}
	}

	num_copies = btrfs_num_copies(root->fs_info, bytenr, disk_size - offset);
again:
	cur = bytenr;
	while (cur < bytenr + size_left) {
		length = bytenr + size_left - cur;
		ret = read_data_from_disk(root->fs_info, inbuf + cur - bytenr, cur,
					  &length, mirror_num);
		if (ret < 0) {
			mirror_num++;
			if (mirror_num > num_copies) {
				ret = -1;
				error("exhausted mirrors trying to read (%d > %d)",
					mirror_num, num_copies);
				goto out;
			}
			pr_stderr(LOG_DEFAULT, "trying another mirror\n");
			continue;
		}
		cur += length;
	}

	if (compress == BTRFS_COMPRESS_NONE) {
		while (total < num_bytes) {
			done = pwrite(fd, inbuf+total, num_bytes-total,
				      pos+total);
			if (done < 0) {
				ret = -1;
				error("cannot write data: %d %m", errno);
				goto out;
			}
			total += done;
		}
		ret = 0;
		goto out;
	}

	ret = decompress(root, inbuf, outbuf, disk_size, &ram_size, compress);
	if (ret) {
		mirror_num++;
		if (mirror_num > num_copies) {
			ret = -1;
			goto out;
		}
		pr_stderr(LOG_DEFAULT,
			"trying another mirror due to decompression error\n");
		goto again;
	}

	while (total < num_bytes) {
		done = pwrite(fd, outbuf + offset + total,
			      num_bytes - total,
			      pos + total);
		if (done < 0) {
			ret = -1;
			goto out;
		}
		total += done;
	}
out:
	free(inbuf);
	free(outbuf);
	return ret;
}

static int set_file_xattrs(struct btrfs_root *root, u64 inode,
			   int fd, const char *file_name)
{
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_dir_item *di;
	u32 name_len = 0;
	u32 data_len = 0;
	u32 len = 0;
	u32 cur, total_len;
	char *name = NULL;
	char *data = NULL;
	int ret = 0;

	btrfs_init_path(&path);
	key.objectid = inode;
	key.type = BTRFS_XATTR_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;

	leaf = path.nodes[0];
	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			do {
				ret = next_leaf(root, &path);
				if (ret < 0) {
					error("searching for extended attributes: %d",
						ret);
					goto out;
				} else if (ret) {
					/* No more leaves to search */
					ret = 0;
					goto out;
				}
				leaf = path.nodes[0];
			} while (!leaf);
			continue;
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type != BTRFS_XATTR_ITEM_KEY || key.objectid != inode)
			break;
		cur = 0;
		total_len = btrfs_item_size(leaf, path.slots[0]);
		di = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_dir_item);

		while (cur < total_len) {
			len = btrfs_dir_name_len(leaf, di);
			if (len > name_len) {
				free(name);
				name = (char *) malloc(len + 1);
				if (!name) {
					ret = -ENOMEM;
					goto out;
				}
			}
			read_extent_buffer(leaf, name,
					   (unsigned long)(di + 1), len);
			name[len] = '\0';
			name_len = len;

			len = btrfs_dir_data_len(leaf, di);
			if (len > data_len) {
				free(data);
				data = (char *) malloc(len);
				if (!data) {
					ret = -ENOMEM;
					goto out;
				}
			}
			read_extent_buffer(leaf, data,
					   (unsigned long)(di + 1) + name_len,
					   len);
			data_len = len;

			if (fsetxattr(fd, name, data, data_len, 0))
				error("setting extended attribute %s on file %s: %m",
					name, file_name);

			len = sizeof(*di) + name_len + data_len;
			cur += len;
			di = (struct btrfs_dir_item *)((char *)di + len);
		}
		path.slots[0]++;
	}
	ret = 0;
out:
	btrfs_release_path(&path);
	free(name);
	free(data);

	return ret;
}

static int copy_metadata(struct btrfs_root *root, int fd,
		struct btrfs_key *key)
{
	struct btrfs_path path;
	struct btrfs_inode_item *inode_item;
	int ret;

	btrfs_init_path(&path);
	ret = btrfs_lookup_inode(NULL, root, &path, key, 0);
	if (ret == 0) {
		struct btrfs_timespec *bts;
		struct timespec times[2];

		inode_item = btrfs_item_ptr(path.nodes[0], path.slots[0],
				struct btrfs_inode_item);

		ret = fchown(fd, btrfs_inode_uid(path.nodes[0], inode_item),
				btrfs_inode_gid(path.nodes[0], inode_item));
		if (ret) {
			error("failed to change owner: %m");
			goto out;
		}

		ret = fchmod(fd, btrfs_inode_mode(path.nodes[0], inode_item));
		if (ret) {
			error("failed to change mode: %m");
			goto out;
		}

		bts = btrfs_inode_atime(inode_item);
		times[0].tv_sec = btrfs_timespec_sec(path.nodes[0], bts);
		times[0].tv_nsec = btrfs_timespec_nsec(path.nodes[0], bts);

		bts = btrfs_inode_mtime(inode_item);
		times[1].tv_sec = btrfs_timespec_sec(path.nodes[0], bts);
		times[1].tv_nsec = btrfs_timespec_nsec(path.nodes[0], bts);

		ret = futimens(fd, times);
		if (ret) {
			error("failed to set times: %m");
			goto out;
		}
	}
out:
	btrfs_release_path(&path);
	return ret;
}

static int copy_file(struct btrfs_root *root, int fd, struct btrfs_key *key,
		     const char *file)
{
	struct extent_buffer *leaf;
	struct btrfs_path path;
	struct btrfs_file_extent_item *fi;
	struct btrfs_inode_item *inode_item;
	struct btrfs_timespec *bts;
	struct btrfs_key found_key;
	int ret;
	int extent_type;
	int compression;
	u64 found_size = 0;
	struct timespec times[2];
	bool times_ok = false;

	btrfs_init_path(&path);
	ret = btrfs_lookup_inode(NULL, root, &path, key, 0);
	if (ret == 0) {
		inode_item = btrfs_item_ptr(path.nodes[0], path.slots[0],
				    struct btrfs_inode_item);
		found_size = btrfs_inode_size(path.nodes[0], inode_item);

		if (restore_metadata) {
			/*
			 * Change the ownership and mode now, set times when
			 * copyout is finished.
			 */

			ret = fchown(fd, btrfs_inode_uid(path.nodes[0], inode_item),
					btrfs_inode_gid(path.nodes[0], inode_item));
			if (ret && !ignore_errors)
				goto out;

			ret = fchmod(fd, btrfs_inode_mode(path.nodes[0], inode_item));
			if (ret && !ignore_errors)
				goto out;

			bts = btrfs_inode_atime(inode_item);
			times[0].tv_sec = btrfs_timespec_sec(path.nodes[0], bts);
			times[0].tv_nsec = btrfs_timespec_nsec(path.nodes[0], bts);

			bts = btrfs_inode_mtime(inode_item);
			times[1].tv_sec = btrfs_timespec_sec(path.nodes[0], bts);
			times[1].tv_nsec = btrfs_timespec_nsec(path.nodes[0], bts);
			times_ok = true;
		}
	}
	btrfs_release_path(&path);

	key->offset = 0;
	key->type = BTRFS_EXTENT_DATA_KEY;

	ret = btrfs_search_slot(NULL, root, key, &path, 0, 0);
	if (ret < 0) {
		error("searching extent data returned %d", ret);
		goto out;
	}

	leaf = path.nodes[0];
	while (!leaf) {
		ret = next_leaf(root, &path);
		if (ret < 0) {
			error("cannot get next leaf: %d", ret);
			goto out;
		} else if (ret > 0) {
			/* No more leaves to search */
			ret = 0;
			goto out;
		}
		leaf = path.nodes[0];
	}

	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			do {
				ret = next_leaf(root, &path);
				if (ret < 0) {
					error("search to next leaf failed: %d", ret);
					goto out;
				} else if (ret) {
					/* No more leaves to search */
					btrfs_release_path(&path);
					goto set_size;
				}
				leaf = path.nodes[0];
			} while (!leaf);
			continue;
		}
		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (found_key.objectid != key->objectid)
			break;
		if (found_key.type != key->type)
			break;
		fi = btrfs_item_ptr(leaf, path.slots[0],
				    struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(leaf, fi);
		compression = btrfs_file_extent_compression(leaf, fi);
		if (compression >= BTRFS_NR_COMPRESS_TYPES) {
			warning("compression type %d not supported",
				compression);
			ret = -1;
			goto out;
		}

		if (extent_type == BTRFS_FILE_EXTENT_PREALLOC)
			goto next;
		if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
			ret = copy_one_inline(root, fd, &path, found_key.offset);
			if (ret)
				goto out;
		} else if (extent_type == BTRFS_FILE_EXTENT_REG) {
			ret = copy_one_extent(root, fd, leaf, fi,
					      found_key.offset);
			if (ret)
				goto out;
		} else {
			warning("weird extent type %d", extent_type);
		}
next:
		path.slots[0]++;
	}

	btrfs_release_path(&path);
set_size:
	if (found_size) {
		ret = ftruncate(fd, (loff_t)found_size);
		if (ret)
			return ret;
	}
	if (get_xattrs) {
		ret = set_file_xattrs(root, key->objectid, fd, file);
		if (ret)
			return ret;
	}
	if (restore_metadata && times_ok) {
		ret = futimens(fd, times);
		if (ret)
			return ret;
	}
	return 0;

out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * returns:
 *  0 if the file exists and should be skipped.
 *  1 if the file does NOT exist
 *  2 if the file exists but is OK to overwrite
 */
static int overwrite_ok(const char * path)
{
	static int warn = 0;
	struct stat st;
	int ret;

	/* don't be fooled by symlinks */
	ret = fstatat(AT_FDCWD, path_name, &st, AT_SYMLINK_NOFOLLOW);

	if (!ret) {
		if (overwrite)
			return 2;

		if (!warn) {
			pr_verbose(LOG_DEFAULT, "Skipping existing file %s\n", path);
			pr_verbose(LOG_DEFAULT, "If you wish to overwrite use -o\n");
		} else {
			pr_verbose(LOG_INFO, "Skipping existing file %s\n", path);
		}

		warn = 1;
		return 0;
	}
	return 1;
}

static int copy_symlink(struct btrfs_root *root, struct btrfs_key *key,
		     const char *file)
{
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *extent_item;
	struct btrfs_inode_item *inode_item;
	u32 len;
	u32 name_offset;
	int ret;
	struct btrfs_timespec *bts;
	struct timespec times[2];

	ret = overwrite_ok(path_name);
	if (ret == 0)
	    return 0; /* skip this file */

	/* symlink() can't overwrite, so unlink first */
	if (ret == 2) {
		ret = unlink(path_name);
		if (ret) {
			error("failed to unlink '%s' for overwrite: %m", path_name);
			return ret;
		}
	}

	btrfs_init_path(&path);
	key->type = BTRFS_EXTENT_DATA_KEY;
	key->offset = 0;
	ret = btrfs_search_slot(NULL, root, key, &path, 0, 0);
	if (ret < 0)
		goto out;

	leaf = path.nodes[0];
	if (!leaf) {
		error("failed to get leaf for symlink '%s'", file);
		ret = -1;
		goto out;
	}

	extent_item = btrfs_item_ptr(leaf, path.slots[0],
			struct btrfs_file_extent_item);

	len = btrfs_file_extent_inline_item_len(leaf, path.slots[0]);
	if (len >= PATH_MAX) {
		error("symlink '%s' target length %d is longer than PATH_MAX",
				fs_name, len);
		ret = -1;
		goto out;
	}

	name_offset = (unsigned long) extent_item
			+ offsetof(struct btrfs_file_extent_item, disk_bytenr);
	read_extent_buffer(leaf, symlink_target, name_offset, len);

	symlink_target[len] = 0;

	if (!dry_run) {
		ret = symlink(symlink_target, path_name);
		if (ret < 0) {
			error("failed to restore symlink '%s': %m", path_name);
			goto out;
		}
	}

	if (bconf.verbose >= 2)
		printf("SYMLINK: '%s' => '%s'\n", path_name, symlink_target);

	ret = 0;
	if (!restore_metadata)
		goto out;

	/*
	 * Symlink metadata operates differently than files/directories, so do
	 * our own work here.
	 */
	key->type = BTRFS_INODE_ITEM_KEY;
	key->offset = 0;

	btrfs_release_path(&path);

	ret = btrfs_lookup_inode(NULL, root, &path, key, 0);
	if (ret) {
		error("failed to lookup inode for '%s'", file);
		goto out;
	}

	inode_item = btrfs_item_ptr(path.nodes[0], path.slots[0],
			struct btrfs_inode_item);

	ret = fchownat(AT_FDCWD, file, btrfs_inode_uid(path.nodes[0], inode_item),
				   btrfs_inode_gid(path.nodes[0], inode_item),
				   AT_SYMLINK_NOFOLLOW);
	if (ret) {
		error("failed to change owner of '%s': %m", file);
		goto out;
	}

	bts = btrfs_inode_atime(inode_item);
	times[0].tv_sec  = btrfs_timespec_sec(path.nodes[0], bts);
	times[0].tv_nsec = btrfs_timespec_nsec(path.nodes[0], bts);

	bts = btrfs_inode_mtime(inode_item);
	times[1].tv_sec  = btrfs_timespec_sec(path.nodes[0], bts);
	times[1].tv_nsec = btrfs_timespec_nsec(path.nodes[0], bts);

	ret = utimensat(AT_FDCWD, file, times, AT_SYMLINK_NOFOLLOW);
	if (ret)
		error("failed to set times for '%s': %m", file);
out:
	btrfs_release_path(&path);
	return ret;
}

static int search_dir(struct btrfs_root *root, struct btrfs_key *key,
		      const char *output_rootdir, const char *in_dir,
		      const regex_t *mreg)
{
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_dir_item *dir_item;
	struct btrfs_key found_key, location;
	char filename[BTRFS_NAME_LEN + 1];
	unsigned long name_ptr;
	int name_len;
	int ret = 0;
	int fd;
	u8 type;

	btrfs_init_path(&path);
	key->offset = 0;
	key->type = BTRFS_DIR_INDEX_KEY;
	ret = btrfs_search_slot(NULL, root, key, &path, 0, 0);
	if (ret < 0) {
		error("search for next directory entry failed: %d", ret);
		goto out;
	}

	ret = 0;

	leaf = path.nodes[0];
	while (!leaf) {
		pr_verbose(LOG_INFO,
			   "No leaf after search, looking for the next leaf\n");
		ret = next_leaf(root, &path);
		if (ret < 0) {
			error("search for next leaf failed: %d", ret);
			goto out;
		} else if (ret > 0) {
			/* No more leaves to search */
			pr_verbose(LOG_INFO,
		   "Reached the end of the tree looking for the directory\n");
			ret = 0;
			goto out;
		}
		leaf = path.nodes[0];
	}

	while (leaf) {
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			do {
				ret = next_leaf(root, &path);
				if (ret < 0) {
					error("search for next leaf failed: %d", ret);
					goto out;
				} else if (ret > 0) {
					/* No more leaves to search */
					pr_verbose(LOG_INFO,
		"Reached the end of the tree searching the directory\n");
					ret = 0;
					goto out;
				}
				leaf = path.nodes[0];
			} while (!leaf);
			continue;
		}
		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (found_key.objectid != key->objectid) {
			pr_verbose(LOG_VERBOSE, "Found objectid=%llu, key=%llu\n",
				   found_key.objectid, key->objectid);
			break;
		}
		if (found_key.type != key->type) {
			pr_verbose(LOG_VERBOSE, "Found type=%u, want=%u\n",
				       found_key.type, key->type);
			break;
		}
		dir_item = btrfs_item_ptr(leaf, path.slots[0],
					  struct btrfs_dir_item);
		name_ptr = (unsigned long)(dir_item + 1);
		name_len = btrfs_dir_name_len(leaf, dir_item);
		read_extent_buffer(leaf, filename, name_ptr, name_len);
		filename[name_len] = '\0';
		type = btrfs_dir_type(leaf, dir_item);
		btrfs_dir_item_key_to_cpu(leaf, dir_item, &location);

		/* full path from root of btrfs being restored */
		snprintf(fs_name, PATH_MAX, "%s/%s", in_dir, filename);

		if (mreg && REG_NOMATCH == regexec(mreg, fs_name, 0, NULL, 0))
			goto next;

		/* full path from system root */
		snprintf(path_name, PATH_MAX, "%s%s", output_rootdir, fs_name);

		/*
		 * Restore directories, files, symlinks and metadata.
		 */
		if (type == BTRFS_FT_REG_FILE) {
			if (!overwrite_ok(path_name))
				goto next;

			pr_verbose(LOG_INFO, "Restoring %s\n", path_name);
			if (dry_run)
				goto next;
			fd = open(path_name, O_CREAT|O_WRONLY, 0644);
			if (fd < 0) {
				error("creating '%s' failed: %m", path_name);
				if (ignore_errors)
					goto next;
				ret = -1;
				goto out;
			}
			ret = copy_file(root, fd, &location, path_name);
			close(fd);
			if (ret) {
				error("copying data for %s failed", path_name);
				if (ignore_errors)
					goto next;
				goto out;
			}
		} else if (type == BTRFS_FT_DIR) {
			struct btrfs_root *search_root = root;
			char *dir = strdup(fs_name);

			if (!dir) {
				error_msg(ERROR_MSG_MEMORY, NULL);
				ret = -ENOMEM;
				goto out;
			}

			if (location.type == BTRFS_ROOT_ITEM_KEY) {
				/*
				 * If we are a snapshot and this is the index
				 * object to ourselves just skip it.
				 */
				if (location.objectid ==
				    root->root_key.objectid) {
					free(dir);
					goto next;
				}

				location.offset = (u64)-1;
				search_root = btrfs_read_fs_root(root->fs_info,
								 &location);
				if (IS_ERR(search_root)) {
					free(dir);
					error("reading subvolume %s failed: %lu",
						path_name, PTR_ERR(search_root));
					if (ignore_errors)
						goto next;
					ret = PTR_ERR(search_root);
					goto out;
				}

				/*
				 * A subvolume will have a key.offset of 0, a
				 * snapshot will have key.offset of a transid.
				 */
				if (search_root->root_key.offset != 0 &&
				    get_snaps == 0) {
					free(dir);
					printf("Skipping snapshot %s\n", filename);
					goto next;
				}
				location.objectid = BTRFS_FIRST_FREE_OBJECTID;
			}

			pr_verbose(LOG_INFO, "Restoring %s\n", path_name);

			errno = 0;
			if (dry_run)
				ret = 0;
			else
				ret = mkdir(path_name, 0755);
			if (ret && errno != EEXIST) {
				free(dir);
				error("failed mkdir %s: %m", path_name);
				if (ignore_errors)
					goto next;
				ret = -1;
				goto out;
			}
			ret = search_dir(search_root, &location,
					 output_rootdir, dir, mreg);
			free(dir);
			if (ret) {
				error("searching directory %s failed: %d",
					path_name, ret);
				if (ignore_errors)
					goto next;
				goto out;
			}
		} else if (type == BTRFS_FT_SYMLINK) {
			if (restore_symlinks)
				ret = copy_symlink(root, &location, path_name);
			if (ret < 0) {
				if (ignore_errors)
					goto next;
				btrfs_release_path(&path);
				return ret;
			}
		}
next:
		path.slots[0]++;
	}

	if (restore_metadata) {
		snprintf(path_name, PATH_MAX, "%s%s", output_rootdir, in_dir);
		fd = open(path_name, O_RDONLY);
		if (fd < 0) {
			error("failed to access '%s' to restore metadata: %m",
					path_name);
			if (!ignore_errors) {
				ret = -1;
				goto out;
			}
		} else {
			/*
			 * Set owner/mode/time on the directory as well
			 */
			key->type = BTRFS_INODE_ITEM_KEY;
			ret = copy_metadata(root, fd, key);
			close(fd);
			if (ret && !ignore_errors)
				goto out;
		}
	}

	pr_verbose(LOG_INFO, "Done searching %s\n", in_dir);
out:
	btrfs_release_path(&path);
	return ret;
}

static int do_list_roots(struct btrfs_root *root)
{
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_disk_key disk_key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_root_item ri;
	unsigned long offset;
	int slot;
	int ret;

	root = root->fs_info->tree_root;

	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		error("failed search next root item: %d", ret);
		btrfs_release_path(&path);
		return -1;
	}

	leaf = path.nodes[0];

	while (1) {
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key(leaf, &disk_key, slot);
		btrfs_disk_key_to_cpu(&found_key, &disk_key);
		if (found_key.type != BTRFS_ROOT_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}

		offset = btrfs_item_ptr_offset(leaf, slot);
		read_extent_buffer(leaf, &ri, offset, sizeof(ri));
		printf(" tree ");
		btrfs_print_key(&disk_key);
		printf(" %llu level %d\n", btrfs_root_bytenr(&ri),
		       btrfs_root_level(&ri));
		path.slots[0]++;
	}
	btrfs_release_path(&path);

	return 0;
}

static struct btrfs_root *open_fs(const char *dev, u64 root_location,
				  int super_mirror, int list_roots)
{
	struct btrfs_fs_info *fs_info = NULL;
	struct btrfs_root *root = NULL;
	struct open_ctree_flags ocf = { 0 };
	u64 bytenr;
	int i;

	for (i = super_mirror; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);

		/*
		 * Restore won't allocate extent and doesn't care anything
		 * in extent tree. Skip block group item search will allow
		 * restore to be executed on heavily damaged fs.
		 */
		ocf.filename = dev;
		ocf.sb_bytenr = bytenr;
		ocf.root_tree_bytenr = root_location;
		ocf.flags = OPEN_CTREE_PARTIAL | OPEN_CTREE_NO_BLOCK_GROUPS |
			    OPEN_CTREE_ALLOW_TRANSID_MISMATCH;
		fs_info = open_ctree_fs_info(&ocf);
		if (fs_info)
			break;
		pr_stderr(LOG_DEFAULT, "Could not open root, trying backup super\n");
	}

	if (!fs_info)
		return NULL;

	/*
	 * All we really need to succeed is reading the chunk tree, everything
	 * else we can do by hand, since we only need to read the tree root and
	 * the fs_root.
	 */
	if (!extent_buffer_uptodate(fs_info->tree_root->node)) {
		u64 generation;

		root = fs_info->tree_root;
		if (!root_location)
			root_location = btrfs_super_root(fs_info->super_copy);
		generation = btrfs_super_generation(fs_info->super_copy);
		root->node = read_tree_block(fs_info, root_location,
					     generation);
		if (!extent_buffer_uptodate(root->node)) {
			error("opening tree root failed");
			close_ctree(root);
			return NULL;
		}
	}

	if (!list_roots && !fs_info->fs_root) {
		struct btrfs_key key;

		key.objectid = BTRFS_FS_TREE_OBJECTID;
		key.type = BTRFS_ROOT_ITEM_KEY;
		key.offset = (u64)-1;
		fs_info->fs_root = btrfs_read_fs_root_no_cache(fs_info, &key);
		if (IS_ERR(fs_info->fs_root)) {
			error("could not read fs root: %ld", PTR_ERR(fs_info->fs_root));
			close_ctree(fs_info->tree_root);
			return NULL;
		}
	}

	if (list_roots && do_list_roots(fs_info->tree_root)) {
		close_ctree(fs_info->tree_root);
		return NULL;
	}

	return fs_info->fs_root;
}

static int find_first_dir(struct btrfs_root *root, u64 *objectid)
{
	struct btrfs_path path;
	struct btrfs_key found_key;
	struct btrfs_key key;
	int ret = -1;
	int i;

	btrfs_init_path(&path);
	key.objectid = 0;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0) {
		error("searching next directory entry failed: %d", ret);
		goto out;
	}

	if (!path.nodes[0]) {
		error("no leaf when looking for directory");
		goto out;
	}
again:
	for (i = path.slots[0];
	     i < btrfs_header_nritems(path.nodes[0]); i++) {
		btrfs_item_key_to_cpu(path.nodes[0], &found_key, i);
		if (found_key.type != key.type)
			continue;

		printf("Using objectid %llu for first dir\n",
		       found_key.objectid);
		*objectid = found_key.objectid;
		ret = 0;
		goto out;
	}
	do {
		ret = next_leaf(root, &path);
		if (ret < 0) {
			error("search for next leaf failed: %d", ret);
			goto out;
		} else if (ret > 0) {
			error("no more leaves to search");
			goto out;
		}
	} while (!path.nodes[0]);
	if (path.nodes[0])
		goto again;
	printf("Couldn't find a dir index item\n");
out:
	btrfs_release_path(&path);
	return ret;
}

static const char * const cmd_restore_usage[] = {
	"btrfs restore [options] <device> <path>\n"
	"btrfs restore [options] -l <device>",
	"Try to restore files from a damaged filesystem (unmounted)",
	"",
	"Control:",
	OPTLINE("-D|--dry-run", "dry run (only list files that would be recovered)"),
	OPTLINE("-i|--ignore-errors", "ignore errors"),
	OPTLINE("-o|--overwrite", "overwrite"),
	"",
	"Restoration:",
	OPTLINE("-m|--metadata", "restore owner, mode and times"),
	OPTLINE("-S|--symlink", "restore symbolic links"),
	OPTLINE("-s|--snapshots", "get snapshots"),
	OPTLINE("-x|--xattr", "restore extended attributes"),
	"",
	"Filtering:",
	OPTLINE("--path-regex <regex>", "restore only filenames matching regex, "
		"you have to use following syntax (possibly quoted): "
		"^/(|home(|/username(|/Desktop(|/.*))))$"),
	OPTLINE("-c", "ignore case (--path-regex only)"),
	"",
	"Analysis:",
	OPTLINE("-d", "find dir"),
	OPTLINE("-l|--list-roots", "list tree roots"),
	"",
	"Alternate starting point:",
	OPTLINE("-f <bytenr>", "filesystem location"),
	OPTLINE("-r|--root <rootid>", "root objectid"),
	OPTLINE("-t <bytenr>", "tree location"),
	OPTLINE("-u|--super <mirror>", "super mirror"),
	"",
	"Other:",
	OPTLINE("-v|--verbose", "deprecated, alias for global -v option"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_VERBOSE,
	"",
	"Compression support: zlib"
#if COMPRESSION_LZO
		", lzo"
#endif
#if COMPRESSION_ZSTD
		", zstd"
#endif
	,
	NULL
};

static int cmd_restore(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_root *root;
	struct btrfs_key key;
	char dir_name[PATH_MAX];
	u64 tree_location = 0;
	u64 fs_location = 0;
	u64 root_objectid = 0;
	int len;
	int ret;
	int super_mirror = 0;
	int find_dir = 0;
	int list_roots = 0;
	const char *match_regstr = NULL;
	int match_cflags = REG_EXTENDED | REG_NOSUB | REG_NEWLINE;
	regex_t match_reg, *mreg = NULL;
	char reg_err[256];

	optind = 0;
	while (1) {
		int opt;
		enum { GETOPT_VAL_PATH_REGEX = GETOPT_VAL_FIRST };
		static const struct option long_options[] = {
			{ "path-regex", required_argument, NULL,
				GETOPT_VAL_PATH_REGEX },
			{ "dry-run", no_argument, NULL, 'D'},
			{ "metadata", no_argument, NULL, 'm'},
			{ "symlinks", no_argument, NULL, 'S'},
			{ "snapshots", no_argument, NULL, 's'},
			{ "xattr", no_argument, NULL, 'x'},
			{ "verbose", no_argument, NULL, 'v'},
			{ "ignore-errors", no_argument, NULL, 'i'},
			{ "overwrite", no_argument, NULL, 'o'},
			{ "super", required_argument, NULL, 'u'},
			{ "root", required_argument, NULL, 'r'},
			{ "list-roots", no_argument, NULL, 'l'},
			{ NULL, 0, NULL, 0}
		};

		opt = getopt_long(argc, argv, "sSxviot:u:dmf:r:lDc", long_options,
					NULL);
		if (opt < 0)
			break;

		switch (opt) {
			case 's':
				get_snaps = 1;
				break;
			case 'v':
				bconf_be_verbose();
				break;
			case 'i':
				ignore_errors = 1;
				break;
			case 'o':
				overwrite = 1;
				break;
			case 't':
				tree_location = arg_strtou64(optarg);
				break;
			case 'f':
				fs_location = arg_strtou64(optarg);
				break;
			case 'u':
				super_mirror = arg_strtou64(optarg);
				if (super_mirror >= BTRFS_SUPER_MIRROR_MAX) {
					error("super mirror %d not valid",
							super_mirror);
					exit(1);
				}
				break;
			case 'd':
				find_dir = 1;
				break;
			case 'r':
				root_objectid = arg_strtou64(optarg);
				if (!is_fstree(root_objectid)) {
					error("objectid %llu is not a valid fs/file tree",
							root_objectid);
					exit(1);
				}
				break;
			case 'l':
				list_roots = 1;
				break;
			case 'm':
				restore_metadata = 1;
				break;
			case 'S':
				restore_symlinks = 1;
				break;
			case 'D':
				dry_run = 1;
				break;
			case 'c':
				match_cflags |= REG_ICASE;
				break;
			case GETOPT_VAL_PATH_REGEX:
				match_regstr = optarg;
				break;
			case 'x':
				get_xattrs = 1;
				break;
			default:
				usage_unknown_option(cmd, argv);
		}
	}

	if (!list_roots && check_argc_min(argc - optind, 2))
		usage(cmd, 1);
	else if (list_roots && check_argc_min(argc - optind, 1))
		usage(cmd, 1);

	if (fs_location && root_objectid) {
		error("can't use -f and -r at the same time");
		return 1;
	}

	if ((ret = check_mounted(argv[optind])) < 0) {
		errno = -ret;
		error("could not check mount status: %m");
		return 1;
	} else if (ret) {
		error("%s is currently mounted, cannot continue", argv[optind]);
		return 1;
	}

	root = open_fs(argv[optind], tree_location, super_mirror, list_roots);
	if (root == NULL)
		return 1;

	if (list_roots)
		goto out;

	if (fs_location != 0) {
		free_extent_buffer(root->node);
		root->node = read_tree_block(root->fs_info, fs_location, 0);
		if (!extent_buffer_uptodate(root->node)) {
			error("failed to read fs location");
			ret = 1;
			goto out;
		}
	}

	memset(path_name, 0, PATH_MAX);

	if (strlen(argv[optind + 1]) >= PATH_MAX) {
		error("path '%s' too long", argv[optind + 1]);
		ret = 1;
		goto out;
	}
	strncpy(dir_name, argv[optind + 1], sizeof dir_name);
	dir_name[sizeof dir_name - 1] = 0;

	/* Strip the trailing / on the dir name */
	len = strlen(dir_name);
	while (len && dir_name[--len] == '/') {
		dir_name[len] = '\0';
	}

	if (root_objectid != 0) {
		struct btrfs_root *orig_root = root;

		key.objectid = root_objectid;
		key.type = BTRFS_ROOT_ITEM_KEY;
		key.offset = (u64)-1;
		root = btrfs_read_fs_root(orig_root->fs_info, &key);
		if (IS_ERR(root)) {
			errno = -PTR_ERR(root);
			error("failed to read root %llu: %m", root_objectid);
			root = orig_root;
			ret = 1;
			goto out;
		}
		key.type = 0;
		key.offset = 0;
	}

	if (find_dir) {
		ret = find_first_dir(root, &key.objectid);
		if (ret)
			goto out;
	} else {
		key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	}

	if (match_regstr) {
		ret = regcomp(&match_reg, match_regstr, match_cflags);
		if (ret) {
			regerror(ret, &match_reg, reg_err, sizeof(reg_err));
			error("regex compilation failed: %s", reg_err);
			goto out;
		}
		mreg = &match_reg;
	}

	if (dry_run)
		printf("This is a dry-run, no files are going to be restored\n");

	ret = search_dir(root, &key, dir_name, "", mreg);

out:
	if (mreg)
		regfree(mreg);
	close_ctree(root);
	return !!ret;
}
DEFINE_SIMPLE_COMMAND(restore, "restore");
