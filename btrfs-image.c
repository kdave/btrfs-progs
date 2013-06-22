/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <zlib.h>
#include "kerncompat.h"
#include "crc32c.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "utils.h"
#include "version.h"
#include "volumes.h"

#define HEADER_MAGIC		0xbd5c25e27295668bULL
#define MAX_PENDING_SIZE	(256 * 1024)
#define BLOCK_SIZE		1024
#define BLOCK_MASK		(BLOCK_SIZE - 1)

#define COMPRESS_NONE		0
#define COMPRESS_ZLIB		1

struct meta_cluster_item {
	__le64 bytenr;
	__le32 size;
} __attribute__ ((__packed__));

struct meta_cluster_header {
	__le64 magic;
	__le64 bytenr;
	__le32 nritems;
	u8 compress;
} __attribute__ ((__packed__));

/* cluster header + index items + buffers */
struct meta_cluster {
	struct meta_cluster_header header;
	struct meta_cluster_item items[];
} __attribute__ ((__packed__));

#define ITEMS_PER_CLUSTER ((BLOCK_SIZE - sizeof(struct meta_cluster)) / \
			   sizeof(struct meta_cluster_item))

struct fs_chunk {
	u64 logical;
	u64 physical;
	u64 bytes;
	struct rb_node n;
};

struct async_work {
	struct list_head list;
	struct list_head ordered;
	u64 start;
	u64 size;
	u8 *buffer;
	size_t bufsize;
	int error;
};

struct metadump_struct {
	struct btrfs_root *root;
	FILE *out;

	struct meta_cluster *cluster;

	pthread_t *threads;
	size_t num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct rb_root name_tree;

	struct list_head list;
	struct list_head ordered;
	size_t num_items;
	size_t num_ready;

	u64 pending_start;
	u64 pending_size;

	int compress_level;
	int done;
	int data;
	int sanitize_names;
};

struct name {
	struct rb_node n;
	char *val;
	char *sub;
	u32 len;
};

struct mdrestore_struct {
	FILE *in;
	FILE *out;

	pthread_t *threads;
	size_t num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	struct rb_root chunk_tree;
	struct list_head list;
	size_t num_items;
	u64 leafsize;
	u64 devid;
	u8 uuid[BTRFS_UUID_SIZE];
	u8 fsid[BTRFS_FSID_SIZE];

	int compress_method;
	int done;
	int error;
	int old_restore;
};

static int search_for_chunk_blocks(struct mdrestore_struct *mdres,
				   u64 search, u64 cluster_bytenr);
static struct extent_buffer *alloc_dummy_eb(u64 bytenr, u32 size);

static void csum_block(u8 *buf, size_t len)
{
	char result[BTRFS_CRC32_SIZE];
	u32 crc = ~(u32)0;
	crc = crc32c(crc, buf + BTRFS_CSUM_SIZE, len - BTRFS_CSUM_SIZE);
	btrfs_csum_final(crc, result);
	memcpy(buf, result, BTRFS_CRC32_SIZE);
}

static int has_name(struct btrfs_key *key)
{
	switch (key->type) {
	case BTRFS_DIR_ITEM_KEY:
	case BTRFS_DIR_INDEX_KEY:
	case BTRFS_INODE_REF_KEY:
	case BTRFS_INODE_EXTREF_KEY:
		return 1;
	default:
		break;
	}

	return 0;
}

static char *generate_garbage(u32 name_len)
{
	char *buf = malloc(name_len);
	int i;

	if (!buf)
		return NULL;

	for (i = 0; i < name_len; i++) {
		char c = rand() % 94 + 33;

		if (c == '/')
			c++;
		buf[i] = c;
	}

	return buf;
}

static int name_cmp(struct rb_node *a, struct rb_node *b, int fuzz)
{
	struct name *entry = rb_entry(a, struct name, n);
	struct name *ins = rb_entry(b, struct name, n);
	u32 len;

	len = min(ins->len, entry->len);
	return memcmp(ins->val, entry->val, len);
}

static int chunk_cmp(struct rb_node *a, struct rb_node *b, int fuzz)
{
	struct fs_chunk *entry = rb_entry(a, struct fs_chunk, n);
	struct fs_chunk *ins = rb_entry(b, struct fs_chunk, n);

	if (fuzz && ins->logical >= entry->logical &&
	    ins->logical < entry->logical + entry->bytes)
		return 0;

	if (ins->logical < entry->logical)
		return -1;
	else if (ins->logical > entry->logical)
		return 1;
	return 0;
}

static void tree_insert(struct rb_root *root, struct rb_node *ins,
			int (*cmp)(struct rb_node *a, struct rb_node *b,
				   int fuzz))
{
	struct rb_node ** p = &root->rb_node;
	struct rb_node * parent = NULL;
	int dir;

	while(*p) {
		parent = *p;

		dir = cmp(*p, ins, 0);
		if (dir < 0)
			p = &(*p)->rb_left;
		else if (dir > 0)
			p = &(*p)->rb_right;
		else
			BUG();
	}

	rb_link_node(ins, parent, p);
	rb_insert_color(ins, root);
}

static struct rb_node *tree_search(struct rb_root *root,
				   struct rb_node *search,
				   int (*cmp)(struct rb_node *a,
					      struct rb_node *b, int fuzz),
				   int fuzz)
{
	struct rb_node *n = root->rb_node;
	int dir;

	while (n) {
		dir = cmp(n, search, fuzz);
		if (dir < 0)
			n = n->rb_left;
		else if (dir > 0)
			n = n->rb_right;
		else
			return n;
	}

	return NULL;
}

static char *find_collision(struct metadump_struct *md, char *name,
			    u32 name_len)
{
	struct name *val;
	struct rb_node *entry;
	struct name tmp;
	unsigned long checksum;
	int found = 0;
	int i;

	tmp.val = name;
	tmp.len = name_len;
	entry = tree_search(&md->name_tree, &tmp.n, name_cmp, 0);
	if (entry) {
		val = rb_entry(entry, struct name, n);
		free(name);
		return val->sub;
	}

	val = malloc(sizeof(struct name));
	if (!val) {
		fprintf(stderr, "Couldn't sanitize name, enomem\n");
		return NULL;
	}

	memset(val, 0, sizeof(*val));

	val->val = name;
	val->len = name_len;
	val->sub = malloc(name_len);
	if (!val->sub) {
		fprintf(stderr, "Couldn't sanitize name, enomem\n");
		free(val);
		return NULL;
	}

	checksum = crc32c(~1, val->val, name_len);
	memset(val->sub, ' ', name_len);
	i = 0;
	while (1) {
		if (crc32c(~1, val->sub, name_len) == checksum &&
		    memcmp(val->sub, val->val, val->len)) {
			found = 1;
			break;
		}

		if (val->sub[i] == 127) {
			do {
				i++;
				if (i > name_len)
					break;
			} while (val->sub[i] == 127);

			if (i > name_len)
				break;
			val->sub[i]++;
			if (val->sub[i] == '/')
				val->sub[i]++;
			memset(val->sub, ' ', i);
			i = 0;
			continue;
		} else {
			val->sub[i]++;
			if (val->sub[i] == '/')
				val->sub[i]++;
		}
	}

	if (!found) {
		fprintf(stderr, "Couldn't find a collision for '%.*s', "
			"generating normal garbage, it won't match indexes\n",
			val->len, val->val);
		for (i = 0; i < name_len; i++) {
			char c = rand() % 94 + 33;

			if (c == '/')
				c++;
			val->sub[i] = c;
		}
	}

	tree_insert(&md->name_tree, &val->n, name_cmp);
	return val->sub;
}

static void sanitize_dir_item(struct metadump_struct *md, struct extent_buffer *eb,
			      int slot)
{
	struct btrfs_dir_item *dir_item;
	char *buf;
	char *garbage;
	unsigned long name_ptr;
	u32 total_len;
	u32 cur = 0;
	u32 this_len;
	u32 name_len;
	int free_garbage = (md->sanitize_names == 1);

	dir_item = btrfs_item_ptr(eb, slot, struct btrfs_dir_item);
	total_len = btrfs_item_size_nr(eb, slot);
	while (cur < total_len) {
		this_len = sizeof(*dir_item) +
			btrfs_dir_name_len(eb, dir_item) +
			btrfs_dir_data_len(eb, dir_item);
		name_ptr = (unsigned long)(dir_item + 1);
		name_len = btrfs_dir_name_len(eb, dir_item);

		if (md->sanitize_names > 1) {
			buf = malloc(name_len);
			if (!buf) {
				fprintf(stderr, "Couldn't sanitize name, "
					"enomem\n");
				return;
			}
			read_extent_buffer(eb, buf, name_ptr, name_len);
			garbage = find_collision(md, buf, name_len);
		} else {
			garbage = generate_garbage(name_len);
		}
		if (!garbage) {
			fprintf(stderr, "Couldn't sanitize name, enomem\n");
			return;
		}
		write_extent_buffer(eb, garbage, name_ptr, name_len);
		cur += this_len;
		dir_item = (struct btrfs_dir_item *)((char *)dir_item +
						     this_len);
		if (free_garbage)
			free(garbage);
	}
}

static void sanitize_inode_ref(struct metadump_struct *md,
			       struct extent_buffer *eb, int slot, int ext)
{
	struct btrfs_inode_extref *extref;
	struct btrfs_inode_ref *ref;
	char *garbage, *buf;
	unsigned long ptr;
	unsigned long name_ptr;
	u32 item_size;
	u32 cur_offset = 0;
	int len;
	int free_garbage = (md->sanitize_names == 1);

	item_size = btrfs_item_size_nr(eb, slot);
	ptr = btrfs_item_ptr_offset(eb, slot);
	while (cur_offset < item_size) {
		if (ext) {
			extref = (struct btrfs_inode_extref *)(ptr +
							       cur_offset);
			name_ptr = (unsigned long)(&extref->name);
			len = btrfs_inode_extref_name_len(eb, extref);
			cur_offset += sizeof(*extref);
		} else {
			ref = (struct btrfs_inode_ref *)(ptr + cur_offset);
			len = btrfs_inode_ref_name_len(eb, ref);
			name_ptr = (unsigned long)(ref + 1);
			cur_offset += sizeof(*ref);
		}
		cur_offset += len;

		if (md->sanitize_names > 1) {
			buf = malloc(len);
			if (!buf) {
				fprintf(stderr, "Couldn't sanitize name, "
					"enomem\n");
				return;
			}
			read_extent_buffer(eb, buf, name_ptr, len);
			garbage = find_collision(md, buf, len);
		} else {
			garbage = generate_garbage(len);
		}

		if (!garbage) {
			fprintf(stderr, "Couldn't sanitize name, enomem\n");
			return;
		}
		write_extent_buffer(eb, garbage, name_ptr, len);
		if (free_garbage)
			free(garbage);
	}
}

static void sanitize_name(struct metadump_struct *md, u8 *dst,
			  struct extent_buffer *src, struct btrfs_key *key,
			  int slot)
{
	struct extent_buffer *eb;

	eb = alloc_dummy_eb(src->start, src->len);
	if (!eb) {
		fprintf(stderr, "Couldn't sanitize name, no memory\n");
		return;
	}

	memcpy(eb->data, dst, eb->len);

	switch (key->type) {
	case BTRFS_DIR_ITEM_KEY:
	case BTRFS_DIR_INDEX_KEY:
		sanitize_dir_item(md, eb, slot);
		break;
	case BTRFS_INODE_REF_KEY:
		sanitize_inode_ref(md, eb, slot, 0);
		break;
	case BTRFS_INODE_EXTREF_KEY:
		sanitize_inode_ref(md, eb, slot, 1);
		break;
	default:
		break;
	}

	memcpy(dst, eb->data, eb->len);
	free(eb);
}

/*
 * zero inline extents and csum items
 */
static void zero_items(struct metadump_struct *md, u8 *dst,
		       struct extent_buffer *src)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_item *item;
	struct btrfs_key key;
	u32 nritems = btrfs_header_nritems(src);
	size_t size;
	unsigned long ptr;
	int i, extent_type;

	for (i = 0; i < nritems; i++) {
		item = btrfs_item_nr(src, i);
		btrfs_item_key_to_cpu(src, &key, i);
		if (key.type == BTRFS_CSUM_ITEM_KEY) {
			size = btrfs_item_size_nr(src, i);
			memset(dst + btrfs_leaf_data(src) +
			       btrfs_item_offset_nr(src, i), 0, size);
			continue;
		}

		if (md->sanitize_names && has_name(&key)) {
			sanitize_name(md, dst, src, &key, i);
			continue;
		}

		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;

		fi = btrfs_item_ptr(src, i, struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(src, fi);
		if (extent_type != BTRFS_FILE_EXTENT_INLINE)
			continue;

		ptr = btrfs_file_extent_inline_start(fi);
		size = btrfs_file_extent_inline_item_len(src, item);
		memset(dst + ptr, 0, size);
	}
}

/*
 * copy buffer and zero useless data in the buffer
 */
static void copy_buffer(struct metadump_struct *md, u8 *dst,
			struct extent_buffer *src)
{
	int level;
	size_t size;
	u32 nritems;

	memcpy(dst, src->data, src->len);
	if (src->start == BTRFS_SUPER_INFO_OFFSET)
		return;

	level = btrfs_header_level(src);
	nritems = btrfs_header_nritems(src);

	if (nritems == 0) {
		size = sizeof(struct btrfs_header);
		memset(dst + size, 0, src->len - size);
	} else if (level == 0) {
		size = btrfs_leaf_data(src) +
			btrfs_item_offset_nr(src, nritems - 1) -
			btrfs_item_nr_offset(nritems);
		memset(dst + btrfs_item_nr_offset(nritems), 0, size);
		zero_items(md, dst, src);
	} else {
		size = offsetof(struct btrfs_node, ptrs) +
			sizeof(struct btrfs_key_ptr) * nritems;
		memset(dst + size, 0, src->len - size);
	}
	csum_block(dst, src->len);
}

static void *dump_worker(void *data)
{
	struct metadump_struct *md = (struct metadump_struct *)data;
	struct async_work *async;
	int ret;

	while (1) {
		pthread_mutex_lock(&md->mutex);
		while (list_empty(&md->list)) {
			if (md->done) {
				pthread_mutex_unlock(&md->mutex);
				goto out;
			}
			pthread_cond_wait(&md->cond, &md->mutex);
		}
		async = list_entry(md->list.next, struct async_work, list);
		list_del_init(&async->list);
		pthread_mutex_unlock(&md->mutex);

		if (md->compress_level > 0) {
			u8 *orig = async->buffer;

			async->bufsize = compressBound(async->size);
			async->buffer = malloc(async->bufsize);

			ret = compress2(async->buffer,
					 (unsigned long *)&async->bufsize,
					 orig, async->size, md->compress_level);

			if (ret != Z_OK)
				async->error = 1;

			free(orig);
		}

		pthread_mutex_lock(&md->mutex);
		md->num_ready++;
		pthread_mutex_unlock(&md->mutex);
	}
out:
	pthread_exit(NULL);
}

static void meta_cluster_init(struct metadump_struct *md, u64 start)
{
	struct meta_cluster_header *header;

	md->num_items = 0;
	md->num_ready = 0;
	header = &md->cluster->header;
	header->magic = cpu_to_le64(HEADER_MAGIC);
	header->bytenr = cpu_to_le64(start);
	header->nritems = cpu_to_le32(0);
	header->compress = md->compress_level > 0 ?
			   COMPRESS_ZLIB : COMPRESS_NONE;
}

static int metadump_init(struct metadump_struct *md, struct btrfs_root *root,
			 FILE *out, int num_threads, int compress_level,
			 int sanitize_names)
{
	int i, ret = 0;

	memset(md, 0, sizeof(*md));
	pthread_cond_init(&md->cond, NULL);
	pthread_mutex_init(&md->mutex, NULL);
	INIT_LIST_HEAD(&md->list);
	INIT_LIST_HEAD(&md->ordered);
	md->root = root;
	md->out = out;
	md->pending_start = (u64)-1;
	md->compress_level = compress_level;
	md->cluster = calloc(1, BLOCK_SIZE);
	md->sanitize_names = sanitize_names;
	if (sanitize_names > 1)
		crc32c_optimization_init();

	if (!md->cluster) {
		pthread_cond_destroy(&md->cond);
		pthread_mutex_destroy(&md->mutex);
		return -ENOMEM;
	}

	meta_cluster_init(md, 0);
	if (!num_threads)
		return 0;

	md->name_tree.rb_node = NULL;
	md->num_threads = num_threads;
	md->threads = calloc(num_threads, sizeof(pthread_t));
	if (!md->threads) {
		free(md->cluster);
		pthread_cond_destroy(&md->cond);
		pthread_mutex_destroy(&md->mutex);
		return -ENOMEM;
	}

	for (i = 0; i < num_threads; i++) {
		ret = pthread_create(md->threads + i, NULL, dump_worker, md);
		if (ret)
			break;
	}

	if (ret) {
		pthread_mutex_lock(&md->mutex);
		md->done = 1;
		pthread_cond_broadcast(&md->cond);
		pthread_mutex_unlock(&md->mutex);

		for (i--; i >= 0; i--)
			pthread_join(md->threads[i], NULL);

		pthread_cond_destroy(&md->cond);
		pthread_mutex_destroy(&md->mutex);
		free(md->cluster);
		free(md->threads);
	}

	return ret;
}

static void metadump_destroy(struct metadump_struct *md)
{
	int i;
	struct rb_node *n;

	pthread_mutex_lock(&md->mutex);
	md->done = 1;
	pthread_cond_broadcast(&md->cond);
	pthread_mutex_unlock(&md->mutex);

	for (i = 0; i < md->num_threads; i++)
		pthread_join(md->threads[i], NULL);

	pthread_cond_destroy(&md->cond);
	pthread_mutex_destroy(&md->mutex);

	while ((n = rb_first(&md->name_tree))) {
		struct name *name;

		name = rb_entry(n, struct name, n);
		rb_erase(n, &md->name_tree);
		free(name->val);
		free(name->sub);
		free(name);
	}
	free(md->threads);
	free(md->cluster);
}

static int write_zero(FILE *out, size_t size)
{
	static char zero[BLOCK_SIZE];
	return fwrite(zero, size, 1, out);
}

static int write_buffers(struct metadump_struct *md, u64 *next)
{
	struct meta_cluster_header *header = &md->cluster->header;
	struct meta_cluster_item *item;
	struct async_work *async;
	u64 bytenr = 0;
	u32 nritems = 0;
	int ret;
	int err = 0;

	if (list_empty(&md->ordered))
		goto out;

	/* wait until all buffers are compressed */
	while (md->num_items > md->num_ready) {
		struct timespec ts = {
			.tv_sec = 0,
			.tv_nsec = 10000000,
		};
		pthread_mutex_unlock(&md->mutex);
		nanosleep(&ts, NULL);
		pthread_mutex_lock(&md->mutex);
	}

	/* setup and write index block */
	list_for_each_entry(async, &md->ordered, ordered) {
		item = md->cluster->items + nritems;
		item->bytenr = cpu_to_le64(async->start);
		item->size = cpu_to_le32(async->bufsize);
		nritems++;
	}
	header->nritems = cpu_to_le32(nritems);

	ret = fwrite(md->cluster, BLOCK_SIZE, 1, md->out);
	if (ret != 1) {
		fprintf(stderr, "Error writing out cluster: %d\n", errno);
		return -EIO;
	}

	/* write buffers */
	bytenr += le64_to_cpu(header->bytenr) + BLOCK_SIZE;
	while (!list_empty(&md->ordered)) {
		async = list_entry(md->ordered.next, struct async_work,
				   ordered);
		list_del_init(&async->ordered);

		bytenr += async->bufsize;
		if (!err)
			ret = fwrite(async->buffer, async->bufsize, 1,
				     md->out);
		if (ret != 1) {
			err = -EIO;
			ret = 0;
			fprintf(stderr, "Error writing out cluster: %d\n",
				errno);
		}

		free(async->buffer);
		free(async);
	}

	/* zero unused space in the last block */
	if (!err && bytenr & BLOCK_MASK) {
		size_t size = BLOCK_SIZE - (bytenr & BLOCK_MASK);

		bytenr += size;
		ret = write_zero(md->out, size);
		if (ret != 1) {
			fprintf(stderr, "Error zeroing out buffer: %d\n",
				errno);
			err = -EIO;
		}
	}
out:
	*next = bytenr;
	return err;
}

static int read_data_extent(struct metadump_struct *md,
			    struct async_work *async)
{
	struct btrfs_multi_bio *multi = NULL;
	struct btrfs_device *device;
	u64 bytes_left = async->size;
	u64 logical = async->start;
	u64 offset = 0;
	u64 bytenr;
	u64 read_len;
	ssize_t done;
	int fd;
	int ret;

	while (bytes_left) {
		read_len = bytes_left;
		ret = btrfs_map_block(&md->root->fs_info->mapping_tree, READ,
				      logical, &read_len, &multi, 0, NULL);
		if (ret) {
			fprintf(stderr, "Couldn't map data block %d\n", ret);
			return ret;
		}

		device = multi->stripes[0].dev;

		if (device->fd == 0) {
			fprintf(stderr,
				"Device we need to read from is not open\n");
			free(multi);
			return -EIO;
		}
		fd = device->fd;
		bytenr = multi->stripes[0].physical;
		free(multi);

		read_len = min(read_len, bytes_left);
		done = pread64(fd, async->buffer+offset, read_len, bytenr);
		if (done < read_len) {
			if (done < 0)
				fprintf(stderr, "Error reading extent %d\n",
					errno);
			else
				fprintf(stderr, "Short read\n");
			return -EIO;
		}

		bytes_left -= done;
		offset += done;
		logical += done;
	}

	return 0;
}

static int flush_pending(struct metadump_struct *md, int done)
{
	struct async_work *async = NULL;
	struct extent_buffer *eb;
	u64 blocksize = md->root->nodesize;
	u64 start;
	u64 size;
	size_t offset;
	int ret = 0;

	if (md->pending_size) {
		async = calloc(1, sizeof(*async));
		if (!async)
			return -ENOMEM;

		async->start = md->pending_start;
		async->size = md->pending_size;
		async->bufsize = async->size;
		async->buffer = malloc(async->bufsize);
		if (!async->buffer) {
			free(async);
			return -ENOMEM;
		}
		offset = 0;
		start = async->start;
		size = async->size;

		if (md->data) {
			ret = read_data_extent(md, async);
			if (ret) {
				free(async->buffer);
				free(async);
				return ret;
			}
		}

		while (!md->data && size > 0) {
			u64 this_read = min(blocksize, size);
			eb = read_tree_block(md->root, start, this_read, 0);
			if (!eb) {
				free(async->buffer);
				free(async);
				fprintf(stderr,
					"Error reading metadata block\n");
				return -EIO;
			}
			copy_buffer(md, async->buffer + offset, eb);
			free_extent_buffer(eb);
			start += this_read;
			offset += this_read;
			size -= this_read;
		}

		md->pending_start = (u64)-1;
		md->pending_size = 0;
	} else if (!done) {
		return 0;
	}

	pthread_mutex_lock(&md->mutex);
	if (async) {
		list_add_tail(&async->ordered, &md->ordered);
		md->num_items++;
		if (md->compress_level > 0) {
			list_add_tail(&async->list, &md->list);
			pthread_cond_signal(&md->cond);
		} else {
			md->num_ready++;
		}
	}
	if (md->num_items >= ITEMS_PER_CLUSTER || done) {
		ret = write_buffers(md, &start);
		if (ret)
			fprintf(stderr, "Error writing buffers %d\n",
				errno);
		else
			meta_cluster_init(md, start);
	}
	pthread_mutex_unlock(&md->mutex);
	return ret;
}

static int add_extent(u64 start, u64 size, struct metadump_struct *md,
		      int data)
{
	int ret;
	if (md->data != data ||
	    md->pending_size + size > MAX_PENDING_SIZE ||
	    md->pending_start + md->pending_size != start) {
		ret = flush_pending(md, 0);
		if (ret)
			return ret;
		md->pending_start = start;
	}
	readahead_tree_block(md->root, start, size, 0);
	md->pending_size += size;
	md->data = data;
	return 0;
}

#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
static int is_tree_block(struct btrfs_root *extent_root,
			 struct btrfs_path *path, u64 bytenr)
{
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 ref_objectid;
	int ret;

	leaf = path->nodes[0];
	while (1) {
		struct btrfs_extent_ref_v0 *ref_item;
		path->slots[0]++;
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, path);
			if (ret < 0)
				return ret;
			if (ret > 0)
				break;
			leaf = path->nodes[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != bytenr)
			break;
		if (key.type != BTRFS_EXTENT_REF_V0_KEY)
			continue;
		ref_item = btrfs_item_ptr(leaf, path->slots[0],
					  struct btrfs_extent_ref_v0);
		ref_objectid = btrfs_ref_objectid_v0(leaf, ref_item);
		if (ref_objectid < BTRFS_FIRST_FREE_OBJECTID)
			return 1;
		break;
	}
	return 0;
}
#endif

static int copy_tree_blocks(struct btrfs_root *root, struct extent_buffer *eb,
			    struct metadump_struct *metadump, int root_tree)
{
	struct extent_buffer *tmp;
	struct btrfs_root_item *ri;
	struct btrfs_key key;
	u64 bytenr;
	int level;
	int nritems = 0;
	int i = 0;
	int ret;

	ret = add_extent(btrfs_header_bytenr(eb), root->leafsize, metadump, 0);
	if (ret) {
		fprintf(stderr, "Error adding metadata block\n");
		return ret;
	}

	if (btrfs_header_level(eb) == 0 && !root_tree)
		return 0;

	level = btrfs_header_level(eb);
	nritems = btrfs_header_nritems(eb);
	for (i = 0; i < nritems; i++) {
		if (level == 0) {
			btrfs_item_key_to_cpu(eb, &key, i);
			if (key.type != BTRFS_ROOT_ITEM_KEY)
				continue;
			ri = btrfs_item_ptr(eb, i, struct btrfs_root_item);
			bytenr = btrfs_disk_root_bytenr(eb, ri);
			tmp = read_tree_block(root, bytenr, root->leafsize, 0);
			if (!tmp) {
				fprintf(stderr,
					"Error reading log root block\n");
				return -EIO;
			}
			ret = copy_tree_blocks(root, tmp, metadump, 0);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		} else {
			bytenr = btrfs_node_blockptr(eb, i);
			tmp = read_tree_block(root, bytenr, root->leafsize, 0);
			if (!tmp) {
				fprintf(stderr, "Error reading log block\n");
				return -EIO;
			}
			ret = copy_tree_blocks(root, tmp, metadump, root_tree);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int copy_log_trees(struct btrfs_root *root,
			  struct metadump_struct *metadump,
			  struct btrfs_path *path)
{
	u64 blocknr = btrfs_super_log_root(root->fs_info->super_copy);

	if (blocknr == 0)
		return 0;

	if (!root->fs_info->log_root_tree ||
	    !root->fs_info->log_root_tree->node) {
		fprintf(stderr, "Error copying tree log, it wasn't setup\n");
		return -EIO;
	}

	return copy_tree_blocks(root, root->fs_info->log_root_tree->node,
				metadump, 1);
}

static int copy_space_cache(struct btrfs_root *root,
			    struct metadump_struct *metadump,
			    struct btrfs_path *path)
{
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	struct btrfs_key key;
	u64 bytenr, num_bytes;
	int ret;

	root = root->fs_info->tree_root;

	key.objectid = 0;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching for free space inode %d\n",
			ret);
		return ret;
	}

	while (1) {
		leaf = path->nodes[0];
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf "
					"%d\n", ret);
				return ret;
			}
			if (ret > 0)
				break;
			leaf = path->nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type != BTRFS_EXTENT_DATA_KEY) {
			path->slots[0]++;
			continue;
		}

		fi = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(leaf, fi) !=
		    BTRFS_FILE_EXTENT_REG) {
			path->slots[0]++;
			continue;
		}

		bytenr = btrfs_file_extent_disk_bytenr(leaf, fi);
		num_bytes = btrfs_file_extent_disk_num_bytes(leaf, fi);
		ret = add_extent(bytenr, num_bytes, metadump, 1);
		if (ret) {
			fprintf(stderr, "Error adding space cache blocks %d\n",
				ret);
			btrfs_release_path(root, path);
			return ret;
		}
		path->slots[0]++;
	}

	return 0;
}

static int copy_from_extent_tree(struct metadump_struct *metadump,
				 struct btrfs_path *path)
{
	struct btrfs_root *extent_root;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;
	u64 bytenr;
	u64 num_bytes;
	int ret;

	extent_root = metadump->root->fs_info->extent_root;
	bytenr = BTRFS_SUPER_INFO_OFFSET + 4096;
	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching extent root %d\n", ret);
		return ret;
	}
	ret = 0;

	while (1) {
		leaf = path->nodes[0];
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf %d"
					"\n", ret);
				break;
			}
			if (ret > 0) {
				ret = 0;
				break;
			}
			leaf = path->nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid < bytenr ||
		    (key.type != BTRFS_EXTENT_ITEM_KEY &&
		     key.type != BTRFS_METADATA_ITEM_KEY)) {
			path->slots[0]++;
			continue;
		}

		bytenr = key.objectid;
		if (key.type == BTRFS_METADATA_ITEM_KEY)
			num_bytes = extent_root->leafsize;
		else
			num_bytes = key.offset;

		if (btrfs_item_size_nr(leaf, path->slots[0]) > sizeof(*ei)) {
			ei = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_extent_item);
			if (btrfs_extent_flags(leaf, ei) &
			    BTRFS_EXTENT_FLAG_TREE_BLOCK) {
				ret = add_extent(bytenr, num_bytes, metadump,
						 0);
				if (ret) {
					fprintf(stderr, "Error adding block "
						"%d\n", ret);
					break;
				}
			}
		} else {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
			ret = is_tree_block(extent_root, path, bytenr);
			if (ret < 0) {
				fprintf(stderr, "Error checking tree block "
					"%d\n", ret);
				break;
			}

			if (ret) {
				ret = add_extent(bytenr, num_bytes, metadump,
						 0);
				if (ret) {
					fprintf(stderr, "Error adding block "
						"%d\n", ret);
					break;
				}
			}
			ret = 0;
#else
			fprintf(stderr, "Either extent tree corruption or "
				"you haven't built with V0 support\n");
			ret = -EIO;
			break;
#endif
		}
		bytenr += num_bytes;
	}

	btrfs_release_path(extent_root, path);

	return ret;
}

static int create_metadump(const char *input, FILE *out, int num_threads,
			   int compress_level, int sanitize, int walk_trees)
{
	struct btrfs_root *root;
	struct btrfs_path *path = NULL;
	struct metadump_struct metadump;
	int ret;
	int err = 0;

	root = open_ctree(input, 0, 0);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		return -EIO;
	}

	BUG_ON(root->nodesize != root->leafsize);

	ret = metadump_init(&metadump, root, out, num_threads,
			    compress_level, sanitize);
	if (ret) {
		fprintf(stderr, "Error initing metadump %d\n", ret);
		close_ctree(root);
		return ret;
	}

	ret = add_extent(BTRFS_SUPER_INFO_OFFSET, 4096, &metadump, 0);
	if (ret) {
		fprintf(stderr, "Error adding metadata %d\n", ret);
		err = ret;
		goto out;
	}

	path = btrfs_alloc_path();
	if (!path) {
		fprintf(stderr, "Out of memory allocing path\n");
		err = -ENOMEM;
		goto out;
	}

	if (walk_trees) {
		ret = copy_tree_blocks(root, root->fs_info->chunk_root->node,
				       &metadump, 1);
		if (ret) {
			err = ret;
			goto out;
		}

		ret = copy_tree_blocks(root, root->fs_info->tree_root->node,
				       &metadump, 1);
		if (ret) {
			err = ret;
			goto out;
		}
	} else {
		ret = copy_from_extent_tree(&metadump, path);
		if (ret) {
			err = ret;
			goto out;
		}
	}

	ret = copy_log_trees(root, &metadump, path);
	if (ret) {
		err = ret;
		goto out;
	}

	ret = copy_space_cache(root, &metadump, path);
out:
	ret = flush_pending(&metadump, 1);
	if (ret) {
		if (!err)
			err = ret;
		fprintf(stderr, "Error flushing pending %d\n", ret);
	}

	metadump_destroy(&metadump);

	btrfs_free_path(path);
	ret = close_ctree(root);
	return err ? err : ret;
}

static void update_super_old(u8 *buffer)
{
	struct btrfs_super_block *super = (struct btrfs_super_block *)buffer;
	struct btrfs_chunk *chunk;
	struct btrfs_disk_key *key;
	u32 sectorsize = btrfs_super_sectorsize(super);
	u64 flags = btrfs_super_flags(super);

	flags |= BTRFS_SUPER_FLAG_METADUMP;
	btrfs_set_super_flags(super, flags);

	key = (struct btrfs_disk_key *)(super->sys_chunk_array);
	chunk = (struct btrfs_chunk *)(super->sys_chunk_array +
				       sizeof(struct btrfs_disk_key));

	btrfs_set_disk_key_objectid(key, BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_disk_key_type(key, BTRFS_CHUNK_ITEM_KEY);
	btrfs_set_disk_key_offset(key, 0);

	btrfs_set_stack_chunk_length(chunk, (u64)-1);
	btrfs_set_stack_chunk_owner(chunk, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_stack_chunk_stripe_len(chunk, 64 * 1024);
	btrfs_set_stack_chunk_type(chunk, BTRFS_BLOCK_GROUP_SYSTEM);
	btrfs_set_stack_chunk_io_align(chunk, sectorsize);
	btrfs_set_stack_chunk_io_width(chunk, sectorsize);
	btrfs_set_stack_chunk_sector_size(chunk, sectorsize);
	btrfs_set_stack_chunk_num_stripes(chunk, 1);
	btrfs_set_stack_chunk_sub_stripes(chunk, 0);
	chunk->stripe.devid = super->dev_item.devid;
	chunk->stripe.offset = cpu_to_le64(0);
	memcpy(chunk->stripe.dev_uuid, super->dev_item.uuid, BTRFS_UUID_SIZE);
	btrfs_set_super_sys_array_size(super, sizeof(*key) + sizeof(*chunk));
	csum_block(buffer, 4096);
}

static int update_super(u8 *buffer)
{
	struct btrfs_super_block *super = (struct btrfs_super_block *)buffer;
	struct btrfs_chunk *chunk;
	struct btrfs_disk_key *disk_key;
	struct btrfs_key key;
	u32 new_array_size = 0;
	u32 array_size;
	u32 cur = 0;
	u32 new_cur = 0;
	u8 *ptr, *write_ptr;
	int old_num_stripes;

	write_ptr = ptr = super->sys_chunk_array;
	array_size = btrfs_super_sys_array_size(super);

	while (cur < array_size) {
		disk_key = (struct btrfs_disk_key *)ptr;
		btrfs_disk_key_to_cpu(&key, disk_key);

		new_array_size += sizeof(*disk_key);
		memmove(write_ptr, ptr, sizeof(*disk_key));

		write_ptr += sizeof(*disk_key);
		ptr += sizeof(*disk_key);
		cur += sizeof(*disk_key);
		new_cur += sizeof(*disk_key);

		if (key.type == BTRFS_CHUNK_ITEM_KEY) {
			chunk = (struct btrfs_chunk *)ptr;
			old_num_stripes = btrfs_stack_chunk_num_stripes(chunk);
			chunk = (struct btrfs_chunk *)write_ptr;

			memmove(write_ptr, ptr, sizeof(*chunk));
			btrfs_set_stack_chunk_num_stripes(chunk, 1);
			btrfs_set_stack_chunk_sub_stripes(chunk, 0);
			btrfs_set_stack_chunk_type(chunk,
						   BTRFS_BLOCK_GROUP_SYSTEM);
			chunk->stripe.devid = super->dev_item.devid;
			memcpy(chunk->stripe.dev_uuid, super->dev_item.uuid,
			       BTRFS_UUID_SIZE);
			new_array_size += sizeof(*chunk);
			new_cur += sizeof(*chunk);
		} else {
			fprintf(stderr, "Bogus key in the sys chunk array "
				"%d\n", key.type);
			return -EIO;
		}
		write_ptr += sizeof(*chunk);
		ptr += btrfs_chunk_item_size(old_num_stripes);
		cur += btrfs_chunk_item_size(old_num_stripes);
	}

	btrfs_set_super_sys_array_size(super, new_array_size);
	csum_block(buffer, 4096);

	return 0;
}

static struct extent_buffer *alloc_dummy_eb(u64 bytenr, u32 size)
{
	struct extent_buffer *eb;

	eb = malloc(sizeof(struct extent_buffer) + size);
	if (!eb)
		return NULL;
	memset(eb, 0, sizeof(struct extent_buffer) + size);

	eb->start = bytenr;
	eb->len = size;
	return eb;
}

static void truncate_item(struct extent_buffer *eb, int slot, u32 new_size)
{
	struct btrfs_item *item;
	u32 nritems;
	u32 old_size;
	u32 old_data_start;
	u32 size_diff;
	u32 data_end;
	int i;

	old_size = btrfs_item_size_nr(eb, slot);
	if (old_size == new_size)
		return;

	nritems = btrfs_header_nritems(eb);
	data_end = btrfs_item_offset_nr(eb, nritems - 1);

	old_data_start = btrfs_item_offset_nr(eb, slot);
	size_diff = old_size - new_size;

	for (i = slot; i < nritems; i++) {
		u32 ioff;
		item = btrfs_item_nr(eb, i);
		ioff = btrfs_item_offset(eb, item);
		btrfs_set_item_offset(eb, item, ioff + size_diff);
	}

	memmove_extent_buffer(eb, btrfs_leaf_data(eb) + data_end + size_diff,
			      btrfs_leaf_data(eb) + data_end,
			      old_data_start + new_size - data_end);
	item = btrfs_item_nr(eb, slot);
	btrfs_set_item_size(eb, item, new_size);
}

static int fixup_chunk_tree_block(struct mdrestore_struct *mdres,
				  struct async_work *async, u8 *buffer,
				  size_t size)
{
	struct extent_buffer *eb;
	size_t size_left = size;
	u64 bytenr = async->start;
	int i;

	if (size_left % mdres->leafsize)
		return 0;

	eb = alloc_dummy_eb(bytenr, mdres->leafsize);
	if (!eb)
		return -ENOMEM;

	while (size_left) {
		eb->start = bytenr;
		memcpy(eb->data, buffer, mdres->leafsize);

		if (btrfs_header_bytenr(eb) != bytenr)
			break;
		if (memcmp(mdres->fsid,
			   eb->data + offsetof(struct btrfs_header, fsid),
			   BTRFS_FSID_SIZE))
			break;

		if (btrfs_header_owner(eb) != BTRFS_CHUNK_TREE_OBJECTID)
			goto next;

		if (btrfs_header_level(eb) != 0)
			goto next;

		for (i = 0; i < btrfs_header_nritems(eb); i++) {
			struct btrfs_chunk chunk;
			struct btrfs_key key;
			u64 type;

			btrfs_item_key_to_cpu(eb, &key, i);
			if (key.type != BTRFS_CHUNK_ITEM_KEY)
				continue;
			truncate_item(eb, i, sizeof(chunk));
			read_extent_buffer(eb, &chunk,
					   btrfs_item_ptr_offset(eb, i),
					   sizeof(chunk));

			/* Zero out the RAID profile */
			type = btrfs_stack_chunk_type(&chunk);
			type &= (BTRFS_BLOCK_GROUP_DATA |
				 BTRFS_BLOCK_GROUP_SYSTEM |
				 BTRFS_BLOCK_GROUP_METADATA);
			btrfs_set_stack_chunk_type(&chunk, type);

			btrfs_set_stack_chunk_num_stripes(&chunk, 1);
			btrfs_set_stack_chunk_sub_stripes(&chunk, 0);
			btrfs_set_stack_stripe_devid(&chunk.stripe, mdres->devid);
			memcpy(chunk.stripe.dev_uuid, mdres->uuid,
			       BTRFS_UUID_SIZE);
			write_extent_buffer(eb, &chunk,
					    btrfs_item_ptr_offset(eb, i),
					    sizeof(chunk));
		}
		memcpy(buffer, eb->data, eb->len);
		csum_block(buffer, eb->len);
next:
		size_left -= mdres->leafsize;
		buffer += mdres->leafsize;
		bytenr += mdres->leafsize;
	}

	return 0;
}

static void write_backup_supers(int fd, u8 *buf)
{
	struct btrfs_super_block *super = (struct btrfs_super_block *)buf;
	struct stat st;
	u64 size;
	u64 bytenr;
	int i;
	int ret;

	if (fstat(fd, &st)) {
		fprintf(stderr, "Couldn't stat restore point, won't be able "
			"to write backup supers: %d\n", errno);
		return;
	}

	size = btrfs_device_size(fd, &st);

	for (i = 1; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		if (bytenr + 4096 > size)
			break;
		btrfs_set_super_bytenr(super, bytenr);
		csum_block(buf, 4096);
		ret = pwrite64(fd, buf, 4096, bytenr);
		if (ret < 4096) {
			if (ret < 0)
				fprintf(stderr, "Problem writing out backup "
					"super block %d, err %d\n", i, errno);
			else
				fprintf(stderr, "Short write writing out "
					"backup super block\n");
			break;
		}
	}
}

static u64 logical_to_physical(struct mdrestore_struct *mdres, u64 logical, u64 *size)
{
	struct fs_chunk *fs_chunk;
	struct rb_node *entry;
	struct fs_chunk search;
	u64 offset;

	if (logical == BTRFS_SUPER_INFO_OFFSET)
		return logical;

	search.logical = logical;
	entry = tree_search(&mdres->chunk_tree, &search.n, chunk_cmp, 1);
	if (!entry) {
		if (mdres->in != stdin)
			printf("Couldn't find a chunk, using logical\n");
		return logical;
	}
	fs_chunk = rb_entry(entry, struct fs_chunk, n);
	if (fs_chunk->logical > logical || fs_chunk->logical + fs_chunk->bytes < logical)
		BUG();
	offset = search.logical - fs_chunk->logical;

	*size = min(*size, fs_chunk->bytes + fs_chunk->logical - logical);
	return fs_chunk->physical + offset;
}

static void *restore_worker(void *data)
{
	struct mdrestore_struct *mdres = (struct mdrestore_struct *)data;
	struct async_work *async;
	size_t size;
	u8 *buffer;
	u8 *outbuf;
	int outfd;
	int ret;

	outfd = fileno(mdres->out);
	buffer = malloc(MAX_PENDING_SIZE * 2);
	if (!buffer) {
		fprintf(stderr, "Error allocing buffer\n");
		pthread_mutex_lock(&mdres->mutex);
		if (!mdres->error)
			mdres->error = -ENOMEM;
		pthread_mutex_unlock(&mdres->mutex);
		goto out;
	}

	while (1) {
		u64 bytenr;
		off_t offset = 0;
		int err = 0;

		pthread_mutex_lock(&mdres->mutex);
		while (!mdres->leafsize || list_empty(&mdres->list)) {
			if (mdres->done) {
				pthread_mutex_unlock(&mdres->mutex);
				goto out;
			}
			pthread_cond_wait(&mdres->cond, &mdres->mutex);
		}
		async = list_entry(mdres->list.next, struct async_work, list);
		list_del_init(&async->list);
		pthread_mutex_unlock(&mdres->mutex);

		if (mdres->compress_method == COMPRESS_ZLIB) {
			size = MAX_PENDING_SIZE * 2;
			ret = uncompress(buffer, (unsigned long *)&size,
					 async->buffer, async->bufsize);
			if (ret != Z_OK) {
				fprintf(stderr, "Error decompressing %d\n",
					ret);
				err = -EIO;
			}
			outbuf = buffer;
		} else {
			outbuf = async->buffer;
			size = async->bufsize;
		}

		if (async->start == BTRFS_SUPER_INFO_OFFSET) {
			if (mdres->old_restore) {
				update_super_old(outbuf);
			} else {
				ret = update_super(outbuf);
				if (ret)
					err = ret;
			}
		} else if (!mdres->old_restore) {
			ret = fixup_chunk_tree_block(mdres, async, outbuf, size);
			if (ret)
				err = ret;
		}

		while (size) {
			u64 chunk_size = size;
			bytenr = logical_to_physical(mdres,
						     async->start + offset,
						     &chunk_size);
			ret = pwrite64(outfd, outbuf+offset, chunk_size,
				       bytenr);
			if (ret < chunk_size) {
				if (ret < 0) {
					fprintf(stderr, "Error writing to "
						"device %d\n", errno);
					err = errno;
					break;
				} else {
					fprintf(stderr, "Short write\n");
					err = -EIO;
					break;
				}
			}
			size -= chunk_size;
			offset += chunk_size;
		}

		if (async->start == BTRFS_SUPER_INFO_OFFSET)
			write_backup_supers(outfd, outbuf);

		pthread_mutex_lock(&mdres->mutex);
		if (err && !mdres->error)
			mdres->error = err;
		mdres->num_items--;
		pthread_mutex_unlock(&mdres->mutex);

		free(async->buffer);
		free(async);
	}
out:
	free(buffer);
	pthread_exit(NULL);
}

static void mdrestore_destroy(struct mdrestore_struct *mdres)
{
	struct rb_node *n;
	int i;

	while ((n = rb_first(&mdres->chunk_tree))) {
		struct fs_chunk *entry;

		entry = rb_entry(n, struct fs_chunk, n);
		rb_erase(n, &mdres->chunk_tree);
		free(entry);
	}
	pthread_mutex_lock(&mdres->mutex);
	mdres->done = 1;
	pthread_cond_broadcast(&mdres->cond);
	pthread_mutex_unlock(&mdres->mutex);

	for (i = 0; i < mdres->num_threads; i++)
		pthread_join(mdres->threads[i], NULL);

	pthread_cond_destroy(&mdres->cond);
	pthread_mutex_destroy(&mdres->mutex);
	free(mdres->threads);
}

static int mdrestore_init(struct mdrestore_struct *mdres,
			  FILE *in, FILE *out, int old_restore,
			  int num_threads)
{
	int i, ret = 0;

	memset(mdres, 0, sizeof(*mdres));
	pthread_cond_init(&mdres->cond, NULL);
	pthread_mutex_init(&mdres->mutex, NULL);
	INIT_LIST_HEAD(&mdres->list);
	mdres->in = in;
	mdres->out = out;
	mdres->old_restore = old_restore;
	mdres->chunk_tree.rb_node = NULL;

	if (!num_threads)
		return 0;

	mdres->num_threads = num_threads;
	mdres->threads = calloc(num_threads, sizeof(pthread_t));
	if (!mdres->threads)
		return -ENOMEM;
	for (i = 0; i < num_threads; i++) {
		ret = pthread_create(mdres->threads + i, NULL, restore_worker,
				     mdres);
		if (ret)
			break;
	}
	if (ret)
		mdrestore_destroy(mdres);
	return ret;
}

static int fill_mdres_info(struct mdrestore_struct *mdres,
			   struct async_work *async)
{
	struct btrfs_super_block *super;
	u8 *buffer = NULL;
	u8 *outbuf;
	int ret;

	/* We've already been initialized */
	if (mdres->leafsize)
		return 0;

	if (mdres->compress_method == COMPRESS_ZLIB) {
		size_t size = MAX_PENDING_SIZE * 2;

		buffer = malloc(MAX_PENDING_SIZE * 2);
		if (!buffer)
			return -ENOMEM;
		ret = uncompress(buffer, (unsigned long *)&size,
				 async->buffer, async->bufsize);
		if (ret != Z_OK) {
			fprintf(stderr, "Error decompressing %d\n", ret);
			free(buffer);
			return -EIO;
		}
		outbuf = buffer;
	} else {
		outbuf = async->buffer;
	}

	super = (struct btrfs_super_block *)outbuf;
	mdres->leafsize = btrfs_super_leafsize(super);
	memcpy(mdres->fsid, super->fsid, BTRFS_FSID_SIZE);
	memcpy(mdres->uuid, super->dev_item.uuid,
		       BTRFS_UUID_SIZE);
	mdres->devid = le64_to_cpu(super->dev_item.devid);
	free(buffer);
	return 0;
}

static int add_cluster(struct meta_cluster *cluster,
		       struct mdrestore_struct *mdres, u64 *next)
{
	struct meta_cluster_item *item;
	struct meta_cluster_header *header = &cluster->header;
	struct async_work *async;
	u64 bytenr;
	u32 i, nritems;
	int ret;

	BUG_ON(mdres->num_items);
	mdres->compress_method = header->compress;

	bytenr = le64_to_cpu(header->bytenr) + BLOCK_SIZE;
	nritems = le32_to_cpu(header->nritems);
	for (i = 0; i < nritems; i++) {
		item = &cluster->items[i];
		async = calloc(1, sizeof(*async));
		if (!async) {
			fprintf(stderr, "Error allocating async\n");
			return -ENOMEM;
		}
		async->start = le64_to_cpu(item->bytenr);
		async->bufsize = le32_to_cpu(item->size);
		async->buffer = malloc(async->bufsize);
		if (!async->buffer) {
			fprintf(stderr, "Error allocing async buffer\n");
			free(async);
			return -ENOMEM;
		}
		ret = fread(async->buffer, async->bufsize, 1, mdres->in);
		if (ret != 1) {
			fprintf(stderr, "Error reading buffer %d\n", errno);
			free(async->buffer);
			free(async);
			return -EIO;
		}
		bytenr += async->bufsize;

		pthread_mutex_lock(&mdres->mutex);
		if (async->start == BTRFS_SUPER_INFO_OFFSET) {
			ret = fill_mdres_info(mdres, async);
			if (ret) {
				fprintf(stderr, "Error setting up restore\n");
				pthread_mutex_unlock(&mdres->mutex);
				free(async->buffer);
				free(async);
				return ret;
			}
		}
		list_add_tail(&async->list, &mdres->list);
		mdres->num_items++;
		pthread_cond_signal(&mdres->cond);
		pthread_mutex_unlock(&mdres->mutex);
	}
	if (bytenr & BLOCK_MASK) {
		char buffer[BLOCK_MASK];
		size_t size = BLOCK_SIZE - (bytenr & BLOCK_MASK);

		bytenr += size;
		ret = fread(buffer, size, 1, mdres->in);
		if (ret != 1) {
			fprintf(stderr, "Error reading in buffer %d\n", errno);
			return -EIO;
		}
	}
	*next = bytenr;
	return 0;
}

static int wait_for_worker(struct mdrestore_struct *mdres)
{
	int ret = 0;

	pthread_mutex_lock(&mdres->mutex);
	ret = mdres->error;
	while (!ret && mdres->num_items > 0) {
		struct timespec ts = {
			.tv_sec = 0,
			.tv_nsec = 10000000,
		};
		pthread_mutex_unlock(&mdres->mutex);
		nanosleep(&ts, NULL);
		pthread_mutex_lock(&mdres->mutex);
		ret = mdres->error;
	}
	pthread_mutex_unlock(&mdres->mutex);
	return ret;
}

static int read_chunk_block(struct mdrestore_struct *mdres, u8 *buffer,
			    u64 bytenr, u64 item_bytenr, u32 bufsize,
			    u64 cluster_bytenr)
{
	struct extent_buffer *eb;
	int ret = 0;
	int i;

	eb = alloc_dummy_eb(bytenr, mdres->leafsize);
	if (!eb) {
		ret = -ENOMEM;
		goto out;
	}

	while (item_bytenr != bytenr) {
		buffer += mdres->leafsize;
		item_bytenr += mdres->leafsize;
	}

	memcpy(eb->data, buffer, mdres->leafsize);
	if (btrfs_header_bytenr(eb) != bytenr) {
		fprintf(stderr, "Eb bytenr doesn't match found bytenr\n");
		ret = -EIO;
		goto out;
	}

	if (memcmp(mdres->fsid, eb->data + offsetof(struct btrfs_header, fsid),
		   BTRFS_FSID_SIZE)) {
		fprintf(stderr, "Fsid doesn't match\n");
		ret = -EIO;
		goto out;
	}

	if (btrfs_header_owner(eb) != BTRFS_CHUNK_TREE_OBJECTID) {
		fprintf(stderr, "Does not belong to the chunk tree\n");
		ret = -EIO;
		goto out;
	}

	for (i = 0; i < btrfs_header_nritems(eb); i++) {
		struct btrfs_chunk chunk;
		struct fs_chunk *fs_chunk;
		struct btrfs_key key;

		if (btrfs_header_level(eb)) {
			u64 blockptr = btrfs_node_blockptr(eb, i);

			ret = search_for_chunk_blocks(mdres, blockptr,
						      cluster_bytenr);
			if (ret)
				break;
			continue;
		}

		/* Yay a leaf!  We loves leafs! */
		btrfs_item_key_to_cpu(eb, &key, i);
		if (key.type != BTRFS_CHUNK_ITEM_KEY)
			continue;

		fs_chunk = malloc(sizeof(struct fs_chunk));
		if (!fs_chunk) {
			fprintf(stderr, "Erorr allocating chunk\n");
			ret = -ENOMEM;
			break;
		}
		memset(fs_chunk, 0, sizeof(*fs_chunk));
		read_extent_buffer(eb, &chunk, btrfs_item_ptr_offset(eb, i),
				   sizeof(chunk));

		fs_chunk->logical = key.offset;
		fs_chunk->physical = btrfs_stack_stripe_offset(&chunk.stripe);
		fs_chunk->bytes = btrfs_stack_chunk_length(&chunk);
		tree_insert(&mdres->chunk_tree, &fs_chunk->n, chunk_cmp);
	}
out:
	free(eb);
	return ret;
}

/* If you have to ask you aren't worthy */
static int search_for_chunk_blocks(struct mdrestore_struct *mdres,
				   u64 search, u64 cluster_bytenr)
{
	struct meta_cluster *cluster;
	struct meta_cluster_header *header;
	struct meta_cluster_item *item;
	u64 current_cluster = cluster_bytenr, bytenr;
	u64 item_bytenr;
	u32 bufsize, nritems, i;
	u8 *buffer, *tmp = NULL;
	int ret = 0;

	cluster = malloc(BLOCK_SIZE);
	if (!cluster) {
		fprintf(stderr, "Error allocating cluster\n");
		return -ENOMEM;
	}

	buffer = malloc(MAX_PENDING_SIZE * 2);
	if (!buffer) {
		fprintf(stderr, "Error allocing buffer\n");
		free(cluster);
		return -ENOMEM;
	}

	if (mdres->compress_method == COMPRESS_ZLIB) {
		tmp = malloc(MAX_PENDING_SIZE * 2);
		if (!tmp) {
			fprintf(stderr, "Error allocing tmp buffer\n");
			free(cluster);
			free(buffer);
			return -ENOMEM;
		}
	}

	bytenr = current_cluster;
	while (1) {
		if (fseek(mdres->in, current_cluster, SEEK_SET)) {
			fprintf(stderr, "Error seeking: %d\n", errno);
			ret = -EIO;
			break;
		}

		ret = fread(cluster, BLOCK_SIZE, 1, mdres->in);
		if (ret == 0) {
			if (cluster_bytenr != 0) {
				cluster_bytenr = 0;
				current_cluster = 0;
				bytenr = 0;
				continue;
			}
			printf("ok this is where we screwed up?\n");
			ret = -EIO;
			break;
		} else if (ret < 0) {
			fprintf(stderr, "Error reading image\n");
			break;
		}
		ret = 0;

		header = &cluster->header;
		if (le64_to_cpu(header->magic) != HEADER_MAGIC ||
		    le64_to_cpu(header->bytenr) != current_cluster) {
			fprintf(stderr, "bad header in metadump image\n");
			ret = -EIO;
			break;
		}

		bytenr += BLOCK_SIZE;
		nritems = le32_to_cpu(header->nritems);
		for (i = 0; i < nritems; i++) {
			size_t size;

			item = &cluster->items[i];
			bufsize = le32_to_cpu(item->size);
			item_bytenr = le64_to_cpu(item->bytenr);

			if (mdres->compress_method == COMPRESS_ZLIB) {
				ret = fread(tmp, bufsize, 1, mdres->in);
				if (ret != 1) {
					fprintf(stderr, "Error reading: %d\n",
						errno);
					ret = -EIO;
					break;
				}

				size = MAX_PENDING_SIZE * 2;
				ret = uncompress(buffer,
						 (unsigned long *)&size, tmp,
						 bufsize);
				if (ret != Z_OK) {
					fprintf(stderr, "Error decompressing "
						"%d\n", ret);
					ret = -EIO;
					break;
				}
			} else {
				ret = fread(buffer, bufsize, 1, mdres->in);
				if (ret != 1) {
					fprintf(stderr, "Error reading: %d\n",
						errno);
					ret = -EIO;
					break;
				}
				size = bufsize;
			}
			ret = 0;

			if (item_bytenr <= search &&
			    item_bytenr + size > search) {
				ret = read_chunk_block(mdres, buffer, search,
						       item_bytenr, size,
						       current_cluster);
				if (!ret)
					ret = 1;
				break;
			}
			bytenr += bufsize;
		}
		if (ret) {
			if (ret > 0)
				ret = 0;
			break;
		}
		if (bytenr & BLOCK_MASK)
			bytenr += BLOCK_SIZE - (bytenr & BLOCK_MASK);
		current_cluster = bytenr;
	}

	free(tmp);
	free(buffer);
	free(cluster);
	return ret;
}

static int build_chunk_tree(struct mdrestore_struct *mdres,
			    struct meta_cluster *cluster)
{
	struct btrfs_super_block *super;
	struct meta_cluster_header *header;
	struct meta_cluster_item *item = NULL;
	u64 chunk_root_bytenr = 0;
	u32 i, nritems;
	u64 bytenr = 0;
	u8 *buffer;
	int ret;

	/* We can't seek with stdin so don't bother doing this */
	if (mdres->in == stdin)
		return 0;

	ret = fread(cluster, BLOCK_SIZE, 1, mdres->in);
	if (ret <= 0) {
		fprintf(stderr, "Error reading in cluster: %d\n", errno);
		return -EIO;
	}
	ret = 0;

	header = &cluster->header;
	if (le64_to_cpu(header->magic) != HEADER_MAGIC ||
	    le64_to_cpu(header->bytenr) != 0) {
		fprintf(stderr, "bad header in metadump image\n");
		return -EIO;
	}

	bytenr += BLOCK_SIZE;
	mdres->compress_method = header->compress;
	nritems = le32_to_cpu(header->nritems);
	for (i = 0; i < nritems; i++) {
		item = &cluster->items[i];

		if (le64_to_cpu(item->bytenr) == BTRFS_SUPER_INFO_OFFSET)
			break;
		bytenr += le32_to_cpu(item->size);
		if (fseek(mdres->in, le32_to_cpu(item->size), SEEK_CUR)) {
			fprintf(stderr, "Error seeking: %d\n", errno);
			return -EIO;
		}
	}

	if (!item || le64_to_cpu(item->bytenr) != BTRFS_SUPER_INFO_OFFSET) {
		fprintf(stderr, "Huh, didn't find the super?\n");
		return -EINVAL;
	}

	buffer = malloc(le32_to_cpu(item->size));
	if (!buffer) {
		fprintf(stderr, "Error allocing buffer\n");
		return -ENOMEM;
	}

	ret = fread(buffer, le32_to_cpu(item->size), 1, mdres->in);
	if (ret != 1) {
		fprintf(stderr, "Error reading buffer: %d\n", errno);
		free(buffer);
		return -EIO;
	}

	if (mdres->compress_method == COMPRESS_ZLIB) {
		size_t size = MAX_PENDING_SIZE * 2;
		u8 *tmp;

		tmp = malloc(MAX_PENDING_SIZE * 2);
		if (!tmp) {
			free(buffer);
			return -ENOMEM;
		}
		ret = uncompress(tmp, (unsigned long *)&size,
				 buffer, le32_to_cpu(item->size));
		if (ret != Z_OK) {
			fprintf(stderr, "Error decompressing %d\n", ret);
			free(buffer);
			free(tmp);
			return -EIO;
		}
		free(buffer);
		buffer = tmp;
	}

	super = (struct btrfs_super_block *)buffer;
	chunk_root_bytenr = btrfs_super_chunk_root(super);
	mdres->leafsize = btrfs_super_leafsize(super);
	memcpy(mdres->fsid, super->fsid, BTRFS_FSID_SIZE);
	memcpy(mdres->uuid, super->dev_item.uuid,
		       BTRFS_UUID_SIZE);
	mdres->devid = le64_to_cpu(super->dev_item.devid);
	free(buffer);

	return search_for_chunk_blocks(mdres, chunk_root_bytenr, 0);
}

static int restore_metadump(const char *input, FILE *out, int old_restore,
			    int num_threads)
{
	struct meta_cluster *cluster = NULL;
	struct meta_cluster_header *header;
	struct mdrestore_struct mdrestore;
	u64 bytenr = 0;
	FILE *in = NULL;
	int ret = 0;

	if (!strcmp(input, "-")) {
		in = stdin;
	} else {
		in = fopen(input, "r");
		if (!in) {
			perror("unable to open metadump image");
			return 1;
		}
	}

	cluster = malloc(BLOCK_SIZE);
	if (!cluster) {
		fprintf(stderr, "Error allocating cluster\n");
		if (in != stdin)
			fclose(in);
		return -ENOMEM;
	}

	ret = mdrestore_init(&mdrestore, in, out, old_restore, num_threads);
	if (ret) {
		fprintf(stderr, "Error initing mdrestore %d\n", ret);
		if (in != stdin)
			fclose(in);
		free(cluster);
		return ret;
	}

	ret = build_chunk_tree(&mdrestore, cluster);
	if (ret)
		goto out;

	if (in != stdin && fseek(in, 0, SEEK_SET)) {
		fprintf(stderr, "Error seeking %d\n", errno);
		goto out;
	}

	while (1) {
		ret = fread(cluster, BLOCK_SIZE, 1, in);
		if (!ret)
			break;

		header = &cluster->header;
		if (le64_to_cpu(header->magic) != HEADER_MAGIC ||
		    le64_to_cpu(header->bytenr) != bytenr) {
			fprintf(stderr, "bad header in metadump image\n");
			ret = -EIO;
			break;
		}
		ret = add_cluster(cluster, &mdrestore, &bytenr);
		if (ret) {
			fprintf(stderr, "Error adding cluster\n");
			break;
		}

		ret = wait_for_worker(&mdrestore);
		if (ret) {
			fprintf(stderr, "One of the threads errored out %d\n",
				ret);
			break;
		}
	}
out:
	mdrestore_destroy(&mdrestore);
	free(cluster);
	if (in != stdin)
		fclose(in);
	return ret;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfs-image [options] source target\n");
	fprintf(stderr, "\t-r      \trestore metadump image\n");
	fprintf(stderr, "\t-c value\tcompression level (0 ~ 9)\n");
	fprintf(stderr, "\t-t value\tnumber of threads (1 ~ 32)\n");
	fprintf(stderr, "\t-o      \tdon't mess with the chunk tree when restoring\n");
	fprintf(stderr, "\t-s      \tsanitize file names, use once to just use garbage, use twice if you want crc collisions\n");
	fprintf(stderr, "\t-w      \twalk all trees instead of using extent tree, do this if your extent tree is broken\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	char *source;
	char *target;
	int num_threads = 0;
	int compress_level = 0;
	int create = 1;
	int old_restore = 0;
	int walk_trees = 0;
	int ret;
	int sanitize = 0;
	FILE *out;

	while (1) {
		int c = getopt(argc, argv, "rc:t:osw");
		if (c < 0)
			break;
		switch (c) {
		case 'r':
			create = 0;
			break;
		case 't':
			num_threads = atoi(optarg);
			if (num_threads <= 0 || num_threads > 32)
				print_usage();
			break;
		case 'c':
			compress_level = atoi(optarg);
			if (compress_level < 0 || compress_level > 9)
				print_usage();
			break;
		case 'o':
			old_restore = 1;
			break;
		case 's':
			sanitize++;
			break;
		case 'w':
			walk_trees = 1;
			break;
		default:
			print_usage();
		}
	}

	if (old_restore && create)
		print_usage();

	argc = argc - optind;
	if (argc != 2)
		print_usage();
	source = argv[optind];
	target = argv[optind + 1];

	if (create && !strcmp(target, "-")) {
		out = stdout;
	} else {
		out = fopen(target, "w+");
		if (!out) {
			perror("unable to create target file");
			exit(1);
		}
	}

	if (num_threads == 0 && compress_level > 0) {
		num_threads = sysconf(_SC_NPROCESSORS_ONLN);
		if (num_threads <= 0)
			num_threads = 1;
	}

	if (create)
		ret = create_metadump(source, out, num_threads,
				      compress_level, sanitize, walk_trees);
	else
		ret = restore_metadump(source, out, old_restore, 1);

	if (out == stdout)
		fflush(out);
	else
		fclose(out);

	return ret;
}
