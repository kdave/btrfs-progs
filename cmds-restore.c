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

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1

#include "kerncompat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>
#include <zlib.h>
#include <regex.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/xattr.h>

#include "ctree.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"
#include "volumes.h"
#include "utils.h"
#include "commands.h"

static char fs_name[4096];
static char path_name[4096];
static int get_snaps = 0;
static int verbose = 0;
static int ignore_errors = 0;
static int overwrite = 0;
static int get_xattrs = 0;
static int dry_run = 0;

#define LZO_LEN 4
#define PAGE_CACHE_SIZE 4096
#define lzo1x_worst_compress(x) ((x) + ((x) / 16) + 64 + 3)

static int decompress_zlib(char *inbuf, char *outbuf, u64 compress_len,
			   u64 decompress_len)
{
	z_stream strm;
	int ret;

	memset(&strm, 0, sizeof(strm));
	ret = inflateInit(&strm);
	if (ret != Z_OK) {
		fprintf(stderr, "inflate init returnd %d\n", ret);
		return -1;
	}

	strm.avail_in = compress_len;
	strm.next_in = (unsigned char *)inbuf;
	strm.avail_out = decompress_len;
	strm.next_out = (unsigned char *)outbuf;
	ret = inflate(&strm, Z_NO_FLUSH);
	if (ret != Z_STREAM_END) {
		(void)inflateEnd(&strm);
		fprintf(stderr, "failed to inflate: %d\n", ret);
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

static int decompress_lzo(unsigned char *inbuf, char *outbuf, u64 compress_len,
			  u64 *decompress_len)
{
	size_t new_len;
	size_t in_len;
	size_t out_len = 0;
	size_t tot_len;
	size_t tot_in;
	int ret;

	ret = lzo_init();
	if (ret != LZO_E_OK) {
		fprintf(stderr, "lzo init returned %d\n", ret);
		return -1;
	}

	tot_len = read_compress_length(inbuf);
	inbuf += LZO_LEN;
	tot_in = LZO_LEN;

	while (tot_in < tot_len) {
		in_len = read_compress_length(inbuf);
		inbuf += LZO_LEN;
		tot_in += LZO_LEN;

		new_len = lzo1x_worst_compress(PAGE_CACHE_SIZE);
		ret = lzo1x_decompress_safe((const unsigned char *)inbuf, in_len,
					    (unsigned char *)outbuf,
					    (void *)&new_len, NULL);
		if (ret != LZO_E_OK) {
			fprintf(stderr, "failed to inflate: %d\n", ret);
			return -1;
		}
		out_len += new_len;
		outbuf += new_len;
		inbuf += in_len;
		tot_in += in_len;
	}

	*decompress_len = out_len;

	return 0;
}

static int decompress(char *inbuf, char *outbuf, u64 compress_len,
		      u64 *decompress_len, int compress)
{
	switch (compress) {
	case BTRFS_COMPRESS_ZLIB:
		return decompress_zlib(inbuf, outbuf, compress_len,
				       *decompress_len);
	case BTRFS_COMPRESS_LZO:
		return decompress_lzo((unsigned char *)inbuf, outbuf, compress_len,
				      decompress_len);
	default:
		break;
	}

	fprintf(stderr, "invalid compression type: %d\n", compress);
	return -1;
}

static int next_leaf(struct btrfs_root *root, struct btrfs_path *path)
{
	int slot;
	int level = 1;
	int offset = 1;
	struct extent_buffer *c;
	struct extent_buffer *next = NULL;

again:
	for (; level < BTRFS_MAX_LEVEL; level++) {
		if (path->nodes[level])
			break;
	}

	if (level == BTRFS_MAX_LEVEL)
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
			continue;
		}

		if (path->reada)
			reada_for_search(root, path, level, slot, 0);

		next = read_node_slot(root, c, slot);
		if (next)
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
			reada_for_search(root, path, level, 0, 0);
		next = read_node_slot(root, next, 0);
		if (!next)
			goto again;
	}
	return 0;
}

static int copy_one_inline(int fd, struct btrfs_path *path, u64 pos)
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
	int compress;

	fi = btrfs_item_ptr(leaf, path->slots[0],
			    struct btrfs_file_extent_item);
	ptr = btrfs_file_extent_inline_start(fi);
	len = btrfs_file_extent_inline_item_len(leaf,
					btrfs_item_nr(path->slots[0]));
	read_extent_buffer(leaf, buf, ptr, len);

	compress = btrfs_file_extent_compression(leaf, fi);
	if (compress == BTRFS_COMPRESS_NONE) {
		done = pwrite(fd, buf, len, pos);
		if (done < len) {
			fprintf(stderr, "Short inline write, wanted %d, did "
				"%zd: %d\n", len, done, errno);
			return -1;
		}
		return 0;
	}

	ram_size = btrfs_file_extent_ram_bytes(leaf, fi);
	outbuf = malloc(ram_size);
	if (!outbuf) {
		fprintf(stderr, "No memory\n");
		return -ENOMEM;
	}

	ret = decompress(buf, outbuf, len, &ram_size, compress);
	if (ret) {
		free(outbuf);
		return ret;
	}

	done = pwrite(fd, outbuf, ram_size, pos);
	free(outbuf);
	if (done < ram_size) {
		fprintf(stderr, "Short compressed inline write, wanted %Lu, "
			"did %zd: %d\n", ram_size, done, errno);
		return -1;
	}

	return 0;
}

static int copy_one_extent(struct btrfs_root *root, int fd,
			   struct extent_buffer *leaf,
			   struct btrfs_file_extent_item *fi, u64 pos)
{
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	char *inbuf, *outbuf = NULL;
	ssize_t done, total = 0;
	u64 bytenr;
	u64 ram_size;
	u64 disk_size;
	u64 num_bytes;
	u64 length;
	u64 size_left;
	u64 dev_bytenr;
	u64 offset;
	u64 count = 0;
	int compress;
	int ret;
	int dev_fd;
	int mirror_num = 1;
	int num_copies;

	compress = btrfs_file_extent_compression(leaf, fi);
	bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
	disk_size = btrfs_file_extent_disk_num_bytes(leaf, fi);
	ram_size = btrfs_file_extent_ram_bytes(leaf, fi);
	offset = btrfs_file_extent_offset(leaf, fi);
	num_bytes = btrfs_file_extent_num_bytes(leaf, fi);
	size_left = num_bytes;
	if (compress == BTRFS_COMPRESS_NONE)
		bytenr += offset;

	if (offset)
		printf("offset is %Lu\n", offset);
	/* we found a hole */
	if (disk_size == 0)
		return 0;

	inbuf = malloc(size_left);
	if (!inbuf) {
		fprintf(stderr, "No memory\n");
		return -ENOMEM;
	}

	if (compress != BTRFS_COMPRESS_NONE) {
		outbuf = malloc(ram_size);
		if (!outbuf) {
			fprintf(stderr, "No memory\n");
			free(inbuf);
			return -ENOMEM;
		}
	}
again:
	length = size_left;
	ret = btrfs_map_block(&root->fs_info->mapping_tree, READ,
			      bytenr, &length, &multi, mirror_num, NULL);
	if (ret) {
		fprintf(stderr, "Error mapping block %d\n", ret);
		goto out;
	}
	device = multi->stripes[0].dev;
	dev_fd = device->fd;
	device->total_ios++;
	dev_bytenr = multi->stripes[0].physical;
	kfree(multi);

	if (size_left < length)
		length = size_left;

	done = pread(dev_fd, inbuf+count, length, dev_bytenr);
	/* Need both checks, or we miss negative values due to u64 conversion */
	if (done < 0 || done < length) {
		num_copies = btrfs_num_copies(&root->fs_info->mapping_tree,
					      bytenr, length);
		mirror_num++;
		/* mirror_num is 1-indexed, so num_copies is a valid mirror. */
		if (mirror_num > num_copies) {
			ret = -1;
			fprintf(stderr, "Exhausted mirrors trying to read\n");
			goto out;
		}
		fprintf(stderr, "Trying another mirror\n");
		goto again;
	}

	mirror_num = 1;
	size_left -= length;
	count += length;
	bytenr += length;
	if (size_left)
		goto again;

	if (compress == BTRFS_COMPRESS_NONE) {
		while (total < num_bytes) {
			done = pwrite(fd, inbuf+total, num_bytes-total,
				      pos+total);
			if (done < 0) {
				ret = -1;
				fprintf(stderr, "Error writing: %d %s\n", errno, strerror(errno));
				goto out;
			}
			total += done;
		}
		ret = 0;
		goto out;
	}

	ret = decompress(inbuf, outbuf, num_bytes, &ram_size, compress);
	if (ret) {
		num_copies = btrfs_num_copies(&root->fs_info->mapping_tree,
					      bytenr, length);
		mirror_num++;
		if (mirror_num >= num_copies) {
			ret = -1;
			goto out;
		}
		fprintf(stderr, "Trying another mirror\n");
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

static int ask_to_continue(const char *file)
{
	char buf[2];
	char *ret;

	printf("We seem to be looping a lot on %s, do you want to keep going "
	       "on ? (y/N): ", file);
again:
	ret = fgets(buf, 2, stdin);
	if (*ret == '\n' || tolower(*ret) == 'n')
		return 1;
	if (tolower(*ret) != 'y') {
		printf("Please enter either 'y' or 'n': ");
		goto again;
	}

	return 0;
}


static int set_file_xattrs(struct btrfs_root *root, u64 inode,
			   int fd, const char *file_name)
{
	struct btrfs_key key;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_dir_item *di;
	u32 name_len = 0;
	u32 data_len = 0;
	u32 len = 0;
	u32 cur, total_len;
	char *name = NULL;
	char *data = NULL;
	int ret = 0;

	key.objectid = inode;
	key.type = BTRFS_XATTR_ITEM_KEY;
	key.offset = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;

	leaf = path->nodes[0];
	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			do {
				ret = next_leaf(root, path);
				if (ret < 0) {
					fprintf(stderr,
						"Error searching for extended attributes: %d\n",
						ret);
					goto out;
				} else if (ret) {
					/* No more leaves to search */
					ret = 0;
					goto out;
				}
				leaf = path->nodes[0];
			} while (!leaf);
			continue;
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type != BTRFS_XATTR_ITEM_KEY || key.objectid != inode)
			break;
		cur = 0;
		total_len = btrfs_item_size_nr(leaf, path->slots[0]);
		di = btrfs_item_ptr(leaf, path->slots[0],
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

			if (fsetxattr(fd, name, data, data_len, 0)) {
				int err = errno;

				fprintf(stderr,
					"Error setting extended attribute %s on file %s: %s\n",
					name, file_name, strerror(err));
			}

			len = sizeof(*di) + name_len + data_len;
			cur += len;
			di = (struct btrfs_dir_item *)((char *)di + len);
		}
		path->slots[0]++;
	}
	ret = 0;
out:
	btrfs_free_path(path);
	free(name);
	free(data);

	return ret;
}


static int copy_file(struct btrfs_root *root, int fd, struct btrfs_key *key,
		     const char *file)
{
	struct extent_buffer *leaf;
	struct btrfs_path *path;
	struct btrfs_file_extent_item *fi;
	struct btrfs_inode_item *inode_item;
	struct btrfs_key found_key;
	int ret;
	int extent_type;
	int compression;
	int loops = 0;
	u64 found_size = 0;

	path = btrfs_alloc_path();
	if (!path) {
		fprintf(stderr, "Ran out of memory\n");
		return -ENOMEM;
	}
	path->skip_locking = 1;

	ret = btrfs_lookup_inode(NULL, root, path, key, 0);
	if (ret == 0) {
		inode_item = btrfs_item_ptr(path->nodes[0], path->slots[0],
				    struct btrfs_inode_item);
		found_size = btrfs_inode_size(path->nodes[0], inode_item);
	}
	btrfs_release_path(path);

	key->offset = 0;
	key->type = BTRFS_EXTENT_DATA_KEY;

	ret = btrfs_search_slot(NULL, root, key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching %d\n", ret);
		btrfs_free_path(path);
		return ret;
	}

	leaf = path->nodes[0];
	while (!leaf) {
		ret = next_leaf(root, path);
		if (ret < 0) {
			fprintf(stderr, "Error getting next leaf %d\n",
				ret);
			btrfs_free_path(path);
			return ret;
		} else if (ret > 0) {
			/* No more leaves to search */
			btrfs_free_path(path);
			return 0;
		}
		leaf = path->nodes[0];
	}

	while (1) {
		if (loops++ >= 1024) {
			ret = ask_to_continue(file);
			if (ret)
				break;
			loops = 0;
		}
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			do {
				ret = next_leaf(root, path);
				if (ret < 0) {
					fprintf(stderr, "Error searching %d\n", ret);
					btrfs_free_path(path);
					return ret;
				} else if (ret) {
					/* No more leaves to search */
					btrfs_free_path(path);
					goto set_size;
				}
				leaf = path->nodes[0];
			} while (!leaf);
			continue;
		}
		btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
		if (found_key.objectid != key->objectid)
			break;
		if (found_key.type != key->type)
			break;
		fi = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(leaf, fi);
		compression = btrfs_file_extent_compression(leaf, fi);
		if (compression >= BTRFS_COMPRESS_LAST) {
			fprintf(stderr, "Don't support compression yet %d\n",
				compression);
			btrfs_free_path(path);
			return -1;
		}

		if (extent_type == BTRFS_FILE_EXTENT_PREALLOC)
			goto next;
		if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
			ret = copy_one_inline(fd, path, found_key.offset);
			if (ret) {
				btrfs_free_path(path);
				return -1;
			}
		} else if (extent_type == BTRFS_FILE_EXTENT_REG) {
			ret = copy_one_extent(root, fd, leaf, fi,
					      found_key.offset);
			if (ret) {
				btrfs_free_path(path);
				return ret;
			}
		} else {
			printf("Weird extent type %d\n", extent_type);
		}
next:
		path->slots[0]++;
	}

	btrfs_free_path(path);
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
	return 0;
}

static int search_dir(struct btrfs_root *root, struct btrfs_key *key,
		      const char *output_rootdir, const char *in_dir,
		      const regex_t *mreg)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_dir_item *dir_item;
	struct btrfs_key found_key, location;
	char filename[BTRFS_NAME_LEN + 1];
	unsigned long name_ptr;
	int name_len;
	int ret;
	int fd;
	int loops = 0;
	u8 type;

	path = btrfs_alloc_path();
	if (!path) {
		fprintf(stderr, "Ran out of memory\n");
		return -ENOMEM;
	}
	path->skip_locking = 1;

	key->offset = 0;
	key->type = BTRFS_DIR_INDEX_KEY;

	ret = btrfs_search_slot(NULL, root, key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching %d\n", ret);
		btrfs_free_path(path);
		return ret;
	}

	leaf = path->nodes[0];
	while (!leaf) {
		if (verbose > 1)
			printf("No leaf after search, looking for the next "
			       "leaf\n");
		ret = next_leaf(root, path);
		if (ret < 0) {
			fprintf(stderr, "Error getting next leaf %d\n",
				ret);
			btrfs_free_path(path);
			return ret;
		} else if (ret > 0) {
			/* No more leaves to search */
			if (verbose)
				printf("Reached the end of the tree looking "
				       "for the directory\n");
			btrfs_free_path(path);
			return 0;
		}
		leaf = path->nodes[0];
	}

	while (leaf) {
		if (loops++ >= 1024) {
			printf("We have looped trying to restore files in %s "
			       "too many times to be making progress, "
			       "stopping\n", in_dir);
			break;
		}

		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			do {
				ret = next_leaf(root, path);
				if (ret < 0) {
					fprintf(stderr, "Error searching %d\n",
						ret);
					btrfs_free_path(path);
					return ret;
				} else if (ret > 0) {
					/* No more leaves to search */
					if (verbose)
						printf("Reached the end of "
						       "the tree searching the"
						       " directory\n");
					btrfs_free_path(path);
					return 0;
				}
				leaf = path->nodes[0];
			} while (!leaf);
			continue;
		}
		btrfs_item_key_to_cpu(leaf, &found_key, path->slots[0]);
		if (found_key.objectid != key->objectid) {
			if (verbose > 1)
				printf("Found objectid=%Lu, key=%Lu\n",
				       found_key.objectid, key->objectid);
			break;
		}
		if (found_key.type != key->type) {
			if (verbose > 1)
				printf("Found type=%u, want=%u\n",
				       found_key.type, key->type);
			break;
		}
		dir_item = btrfs_item_ptr(leaf, path->slots[0],
					  struct btrfs_dir_item);
		name_ptr = (unsigned long)(dir_item + 1);
		name_len = btrfs_dir_name_len(leaf, dir_item);
		read_extent_buffer(leaf, filename, name_ptr, name_len);
		filename[name_len] = '\0';
		type = btrfs_dir_type(leaf, dir_item);
		btrfs_dir_item_key_to_cpu(leaf, dir_item, &location);

		/* full path from root of btrfs being restored */
		snprintf(fs_name, 4096, "%s/%s", in_dir, filename);

		if (mreg && REG_NOMATCH == regexec(mreg, fs_name, 0, NULL, 0))
			goto next;

		/* full path from system root */
		snprintf(path_name, 4096, "%s%s", output_rootdir, fs_name);

		/*
		 * At this point we're only going to restore directories and
		 * files, no symlinks or anything else.
		 */
		if (type == BTRFS_FT_REG_FILE) {
			if (!overwrite) {
				static int warn = 0;
				struct stat st;

				ret = stat(path_name, &st);
				if (!ret) {
					loops = 0;
					if (verbose || !warn)
						printf("Skipping existing file"
						       " %s\n", path_name);
					if (warn)
						goto next;
					printf("If you wish to overwrite use "
					       "the -o option to overwrite\n");
					warn = 1;
					goto next;
				}
				ret = 0;
			}
			if (verbose)
				printf("Restoring %s\n", path_name);
			if (dry_run)
				goto next;
			fd = open(path_name, O_CREAT|O_WRONLY, 0644);
			if (fd < 0) {
				fprintf(stderr, "Error creating %s: %d\n",
					path_name, errno);
				if (ignore_errors)
					goto next;
				btrfs_free_path(path);
				return -1;
			}
			loops = 0;
			ret = copy_file(root, fd, &location, path_name);
			close(fd);
			if (ret) {
				if (ignore_errors)
					goto next;
				btrfs_free_path(path);
				return ret;
			}
		} else if (type == BTRFS_FT_DIR) {
			struct btrfs_root *search_root = root;
			char *dir = strdup(fs_name);

			if (!dir) {
				fprintf(stderr, "Ran out of memory\n");
				btrfs_free_path(path);
				return -ENOMEM;
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
					fprintf(stderr, "Error reading "
						"subvolume %s: %lu\n",
						path_name,
						PTR_ERR(search_root));
					if (ignore_errors)
						goto next;
					btrfs_free_path(path);
					return PTR_ERR(search_root);
				}

				/*
				 * A subvolume will have a key.offset of 0, a
				 * snapshot will have key.offset of a transid.
				 */
				if (search_root->root_key.offset != 0 &&
				    get_snaps == 0) {
					free(dir);
					printf("Skipping snapshot %s\n",
					       filename);
					goto next;
				}
				location.objectid = BTRFS_FIRST_FREE_OBJECTID;
			}

			if (verbose)
				printf("Restoring %s\n", path_name);

			errno = 0;
			if (dry_run)
				ret = 0;
			else
				ret = mkdir(path_name, 0755);
			if (ret && errno != EEXIST) {
				free(dir);
				fprintf(stderr, "Error mkdiring %s: %d\n",
					path_name, errno);
				if (ignore_errors)
					goto next;
				btrfs_free_path(path);
				return -1;
			}
			loops = 0;
			ret = search_dir(search_root, &location,
					 output_rootdir, dir, mreg);
			free(dir);
			if (ret) {
				if (ignore_errors)
					goto next;
				btrfs_free_path(path);
				return ret;
			}
		}
next:
		path->slots[0]++;
	}

	if (verbose)
		printf("Done searching %s\n", in_dir);
	btrfs_free_path(path);
	return 0;
}

static int do_list_roots(struct btrfs_root *root)
{
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_disk_key disk_key;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_root_item ri;
	unsigned long offset;
	int slot;
	int ret;

	root = root->fs_info->tree_root;
	path = btrfs_alloc_path();
	if (!path) {
		fprintf(stderr, "Failed to alloc path\n");
		return -ENOMEM;
	}

	key.offset = 0;
	key.objectid = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to do search %d\n", ret);
		btrfs_free_path(path);
		return -1;
	}

	while (1) {
		leaf = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, path);
			if (ret)
				break;
			leaf = path->nodes[0];
			slot = path->slots[0];
		}
		btrfs_item_key(leaf, &disk_key, slot);
		btrfs_disk_key_to_cpu(&found_key, &disk_key);
		if (btrfs_key_type(&found_key) != BTRFS_ROOT_ITEM_KEY) {
			path->slots[0]++;
			continue;
		}

		offset = btrfs_item_ptr_offset(leaf, slot);
		read_extent_buffer(leaf, &ri, offset, sizeof(ri));
		printf(" tree ");
		btrfs_print_key(&disk_key);
		printf(" %Lu level %d\n", btrfs_root_bytenr(&ri),
		       btrfs_root_level(&ri));
		path->slots[0]++;
	}
	btrfs_free_path(path);

	return 0;
}

static struct btrfs_root *open_fs(const char *dev, u64 root_location,
				  int super_mirror, int list_roots)
{
	struct btrfs_fs_info *fs_info = NULL;
	struct btrfs_root *root = NULL;
	u64 bytenr;
	int i;

	for (i = super_mirror; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		fs_info = open_ctree_fs_info(dev, bytenr, root_location,
					     OPEN_CTREE_PARTIAL);
		if (fs_info)
			break;
		fprintf(stderr, "Could not open root, trying backup super\n");
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
		root->node = read_tree_block(root, root_location,
					     root->leafsize, generation);
		if (!extent_buffer_uptodate(root->node)) {
			fprintf(stderr, "Error opening tree root\n");
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
			fprintf(stderr, "Couldn't read fs root: %ld\n",
				PTR_ERR(fs_info->fs_root));
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
	struct btrfs_path *path;
	struct btrfs_key found_key;
	struct btrfs_key key;
	int ret = -1;
	int i;

	key.objectid = 0;
	key.type = BTRFS_DIR_INDEX_KEY;
	key.offset = 0;

	path = btrfs_alloc_path();
	if (!path) {
		fprintf(stderr, "Ran out of memory\n");
		return ret;
	}

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching %d\n", ret);
		goto out;
	}

	if (!path->nodes[0]) {
		fprintf(stderr, "No leaf!\n");
		goto out;
	}
again:
	for (i = path->slots[0];
	     i < btrfs_header_nritems(path->nodes[0]); i++) {
		btrfs_item_key_to_cpu(path->nodes[0], &found_key, i);
		if (found_key.type != key.type)
			continue;

		printf("Using objectid %Lu for first dir\n",
		       found_key.objectid);
		*objectid = found_key.objectid;
		ret = 0;
		goto out;
	}
	do {
		ret = next_leaf(root, path);
		if (ret < 0) {
			fprintf(stderr, "Error getting next leaf %d\n",
				ret);
			goto out;
		} else if (ret > 0) {
			fprintf(stderr, "No more leaves\n");
			goto out;
		}
	} while (!path->nodes[0]);
	if (path->nodes[0])
		goto again;
	printf("Couldn't find a dir index item\n");
out:
	btrfs_free_path(path);
	return ret;
}

static struct option long_options[] = {
	{ "path-regex", 1, NULL, 256},
	{ "dry-run", 0, NULL, 'D'},
	{ NULL, 0, NULL, 0}
};

const char * const cmd_restore_usage[] = {
	"btrfs restore [options] <device> <path> | -l <device>",
	"Try to restore files from a damaged filesystem (unmounted)",
	"",
	"-s              get snapshots",
	"-x              get extended attributes",
	"-v              verbose",
	"-i              ignore errors",
	"-o              overwrite",
	"-t <location>   tree location",
	"-f <offset>     filesystem location",
	"-u <block>      super mirror",
	"-r <rootid>     root objectid",
	"-d              find dir",
	"-l              list tree roots",
	"-D|--dry-run    dry run (only list files that would be recovered)",
	"--path-regex <regex>",
	"                restore only filenames matching regex,",
	"                you have to use following syntax (possibly quoted):",
	"                ^/(|home(|/username(|/Desktop(|/.*))))$",
	NULL
};

int cmd_restore(int argc, char **argv)
{
	struct btrfs_root *root;
	struct btrfs_key key;
	char dir_name[128];
	u64 tree_location = 0;
	u64 fs_location = 0;
	u64 root_objectid = 0;
	int len;
	int ret;
	int opt;
	int option_index = 0;
	int super_mirror = 0;
	int find_dir = 0;
	int list_roots = 0;
	const char *match_regstr = NULL;
	int match_cflags = REG_EXTENDED | REG_NOSUB | REG_NEWLINE;
	regex_t match_reg, *mreg = NULL;
	char reg_err[256];

	while ((opt = getopt_long(argc, argv, "sxviot:u:df:r:lDc", long_options,
					&option_index)) != -1) {

		switch (opt) {
			case 's':
				get_snaps = 1;
				break;
			case 'v':
				verbose++;
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
					fprintf(stderr, "Super mirror not "
						"valid\n");
					exit(1);
				}
				break;
			case 'd':
				find_dir = 1;
				break;
			case 'r':
				root_objectid = arg_strtou64(optarg);
				break;
			case 'l':
				list_roots = 1;
				break;
			case 'D':
				dry_run = 1;
				break;
			case 'c':
				match_cflags |= REG_ICASE;
				break;
			/* long option without single letter alternative */
			case 256:
				match_regstr = optarg;
				break;
			case 'x':
				get_xattrs = 1;
				break;
			default:
				usage(cmd_restore_usage);
		}
	}

	if (!list_roots && optind + 1 >= argc)
		usage(cmd_restore_usage);
	else if (list_roots && optind >= argc)
		usage(cmd_restore_usage);

	if ((ret = check_mounted(argv[optind])) < 0) {
		fprintf(stderr, "Could not check mount status: %s\n",
			strerror(-ret));
		return 1;
	} else if (ret) {
		fprintf(stderr, "%s is currently mounted.  Aborting.\n", argv[optind]);
		return 1;
	}

	root = open_fs(argv[optind], tree_location, super_mirror, list_roots);
	if (root == NULL)
		return 1;

	if (list_roots)
		goto out;

	if (fs_location != 0) {
		free_extent_buffer(root->node);
		root->node = read_tree_block(root, fs_location, root->leafsize, 0);
		if (!root->node) {
			fprintf(stderr, "Failed to read fs location\n");
			goto out;
		}
	}

	memset(path_name, 0, 4096);

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
			fprintf(stderr, "Error reading root\n");
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
			fprintf(stderr, "Regex compile failed: %s\n", reg_err);
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
