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

struct async_work {
	struct list_head list;
	struct list_head ordered;
	u64 start;
	u64 size;
	u8 *buffer;
	size_t bufsize;
};

struct metadump_struct {
	struct btrfs_root *root;
	FILE *out;

	struct meta_cluster *cluster;

	pthread_t *threads;
	size_t num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	struct list_head list;
	struct list_head ordered;
	size_t num_items;
	size_t num_ready;

	u64 pending_start;
	u64 pending_size;

	int compress_level;
	int done;
};

struct mdrestore_struct {
	FILE *in;
	FILE *out;

	pthread_t *threads;
	size_t num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	struct list_head list;
	size_t num_items;

	int compress_method;
	int done;
};

static void csum_block(u8 *buf, size_t len)
{
	char result[BTRFS_CRC32_SIZE];
	u32 crc = ~(u32)0;
	crc = crc32c(crc, buf + BTRFS_CSUM_SIZE, len - BTRFS_CSUM_SIZE);
	btrfs_csum_final(crc, result);
	memcpy(buf, result, BTRFS_CRC32_SIZE);
}

/*
 * zero inline extents and csum items
 */
static void zero_items(u8 *dst, struct extent_buffer *src)
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
static void copy_buffer(u8 *dst, struct extent_buffer *src)
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
		zero_items(dst, src);
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
			BUG_ON(ret != Z_OK);

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
			 FILE *out, int num_threads, int compress_level)
{
	int i, ret;

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
	if (!md->cluster)
		return -ENOMEM;

	meta_cluster_init(md, 0);
	if (!num_threads)
		return 0;

	md->num_threads = num_threads;
	md->threads = calloc(num_threads, sizeof(pthread_t));
	if (!md->threads)
		return -ENOMEM;
	for (i = 0; i < num_threads; i++) {
		ret = pthread_create(md->threads + i, NULL, dump_worker, md);
		if (ret)
			break;
	}
	return ret;
}

static void metadump_destroy(struct metadump_struct *md)
{
	int i;
	pthread_mutex_lock(&md->mutex);
	md->done = 1;
	pthread_cond_broadcast(&md->cond);
	pthread_mutex_unlock(&md->mutex);

	for (i = 0; i < md->num_threads; i++)
		pthread_join(md->threads[i], NULL);

	pthread_cond_destroy(&md->cond);
	pthread_mutex_destroy(&md->mutex);
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
	BUG_ON(ret != 1);

	/* write buffers */
	bytenr += le64_to_cpu(header->bytenr) + BLOCK_SIZE;
	while (!list_empty(&md->ordered)) {
		async = list_entry(md->ordered.next, struct async_work,
				   ordered);
		list_del_init(&async->ordered);

		bytenr += async->bufsize;
		ret = fwrite(async->buffer, async->bufsize, 1, md->out);
		BUG_ON(ret != 1);

		free(async->buffer);
		free(async);
	}

	/* zero unused space in the last block */
	if (bytenr & BLOCK_MASK) {
		size_t size = BLOCK_SIZE - (bytenr & BLOCK_MASK);

		bytenr += size;
		ret = write_zero(md->out, size);
		BUG_ON(ret != 1);
	}
out:
	*next = bytenr;
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
	int ret;

	if (md->pending_size) {
		async = calloc(1, sizeof(*async));
		if (!async)
			return -ENOMEM;

		async->start = md->pending_start;
		async->size = md->pending_size;
		async->bufsize = async->size;
		async->buffer = malloc(async->bufsize);

		offset = 0;
		start = async->start;
		size = async->size;
		while (size > 0) {
			eb = read_tree_block(md->root, start, blocksize, 0);
			BUG_ON(!eb);
			copy_buffer(async->buffer + offset, eb);
			free_extent_buffer(eb);
			start += blocksize;
			offset += blocksize;
			size -= blocksize;
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
		BUG_ON(ret);
		meta_cluster_init(md, start);
	}
	pthread_mutex_unlock(&md->mutex);
	return 0;
}

static int add_metadata(u64 start, u64 size, struct metadump_struct *md)
{
	int ret;
	if (md->pending_size + size > MAX_PENDING_SIZE ||
	    md->pending_start + md->pending_size != start) {
		ret = flush_pending(md, 0);
		if (ret)
			return ret;
		md->pending_start = start;
	}
	readahead_tree_block(md->root, start, size, 0);
	md->pending_size += size;
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
			BUG_ON(ret < 0);
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

static int create_metadump(const char *input, FILE *out, int num_threads,
			   int compress_level)
{
	struct btrfs_root *root;
	struct btrfs_root *extent_root;
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;
	struct metadump_struct metadump;
	u64 bytenr;
	u64 num_bytes;
	int ret;

	root = open_ctree(input, 0, 0);
	BUG_ON(root->nodesize != root->leafsize);

	ret = metadump_init(&metadump, root, out, num_threads,
			    compress_level);
	BUG_ON(ret);

	ret = add_metadata(BTRFS_SUPER_INFO_OFFSET, 4096, &metadump);
	BUG_ON(ret);

	extent_root = root->fs_info->extent_root;
	path = btrfs_alloc_path();

	bytenr = BTRFS_SUPER_INFO_OFFSET + 4096;
	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	BUG_ON(ret < 0);

	while (1) {
		leaf = path->nodes[0];
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, path);
			BUG_ON(ret < 0);
			if (ret > 0)
				break;
			leaf = path->nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid < bytenr ||
		    key.type != BTRFS_EXTENT_ITEM_KEY) {
			path->slots[0]++;
			continue;
		}

		bytenr = key.objectid;
		num_bytes = key.offset;

		if (btrfs_item_size_nr(leaf, path->slots[0]) > sizeof(*ei)) {
			ei = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_extent_item);
			if (btrfs_extent_flags(leaf, ei) &
			    BTRFS_EXTENT_FLAG_TREE_BLOCK) {
				ret = add_metadata(bytenr, num_bytes,
						   &metadump);
				BUG_ON(ret);
			}
		} else {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
			if (is_tree_block(extent_root, path, bytenr)) {
				ret = add_metadata(bytenr, num_bytes,
						   &metadump);
				BUG_ON(ret);
			}
#else
			BUG_ON(1);
#endif
		}
		bytenr += num_bytes;
	}

	ret = flush_pending(&metadump, 1);
	BUG_ON(ret);

	metadump_destroy(&metadump);

	btrfs_free_path(path);
	ret = close_ctree(root);
	return 0;
}

static void update_super(u8 *buffer)
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
	BUG_ON(!buffer);

	while (1) {
		pthread_mutex_lock(&mdres->mutex);
		while (list_empty(&mdres->list)) {
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
			BUG_ON(ret != Z_OK);
			outbuf = buffer;
		} else {
			outbuf = async->buffer;
			size = async->bufsize;
		}

		if (async->start == BTRFS_SUPER_INFO_OFFSET)
			update_super(outbuf);

		ret = pwrite64(outfd, outbuf, size, async->start);
		BUG_ON(ret != size);

		pthread_mutex_lock(&mdres->mutex);
		mdres->num_items--;
		pthread_mutex_unlock(&mdres->mutex);

		free(async->buffer);
		free(async);
	}
out:
	free(buffer);
	pthread_exit(NULL);
}

static int mdresotre_init(struct mdrestore_struct *mdres,
			  FILE *in, FILE *out, int num_threads)
{
	int i, ret = 0;

	memset(mdres, 0, sizeof(*mdres));
	pthread_cond_init(&mdres->cond, NULL);
	pthread_mutex_init(&mdres->mutex, NULL);
	INIT_LIST_HEAD(&mdres->list);
	mdres->in = in;
	mdres->out = out;

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
	return ret;
}

static void mdresotre_destroy(struct mdrestore_struct *mdres)
{
	int i;
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
		async->start = le64_to_cpu(item->bytenr);
		async->bufsize = le32_to_cpu(item->size);
		async->buffer = malloc(async->bufsize);
		ret = fread(async->buffer, async->bufsize, 1, mdres->in);
		BUG_ON(ret != 1);
		bytenr += async->bufsize;

		pthread_mutex_lock(&mdres->mutex);
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
		BUG_ON(ret != 1);
	}
	*next = bytenr;
	return 0;
}

static int wait_for_worker(struct mdrestore_struct *mdres)
{
	pthread_mutex_lock(&mdres->mutex);
	while (mdres->num_items > 0) {
		struct timespec ts = {
			.tv_sec = 0,
			.tv_nsec = 10000000,
		};
		pthread_mutex_unlock(&mdres->mutex);
		nanosleep(&ts, NULL);
		pthread_mutex_lock(&mdres->mutex);
	}
	pthread_mutex_unlock(&mdres->mutex);
	return 0;
}

static int restore_metadump(const char *input, FILE *out, int num_threads)
{
	struct meta_cluster *cluster;
	struct meta_cluster_header *header;
	struct mdrestore_struct mdrestore;
	u64 bytenr = 0;
	FILE *in;
	int ret;

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
	BUG_ON(!cluster);

	ret = mdresotre_init(&mdrestore, in, out, num_threads);
	BUG_ON(ret);

	while (1) {
		ret = fread(cluster, BLOCK_SIZE, 1, in);
		if (!ret)
			break;

		header = &cluster->header;
		if (le64_to_cpu(header->magic) != HEADER_MAGIC ||
		    le64_to_cpu(header->bytenr) != bytenr) {
			fprintf(stderr, "bad header in metadump image\n");
			return 1;
		}
		ret = add_cluster(cluster, &mdrestore, &bytenr);
		BUG_ON(ret);

		wait_for_worker(&mdrestore);
	}

	mdresotre_destroy(&mdrestore);
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
	exit(1);
}

int main(int argc, char *argv[])
{
	char *source;
	char *target;
	int num_threads = 0;
	int compress_level = 0;
	int create = 1;
	int ret;
	FILE *out;

	while (1) {
		int c = getopt(argc, argv, "rc:t:");
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
		default:
			print_usage();
		}
	}

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
				      compress_level);
	else
		ret = restore_metadump(source, out, 1);

	if (out == stdout)
		fflush(out);
	else
		fclose(out);

	exit(ret);
}
