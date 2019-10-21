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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <zlib.h>
#include <getopt.h>

#include "kerncompat.h"
#include "crypto/crc32c.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "common/utils.h"
#include "volumes.h"
#include "extent_io.h"
#include "common/help.h"
#include "common/device-utils.h"
#include "image/metadump.h"
#include "image/sanitize.h"
#include "common/box.h"

#define MAX_WORKER_THREADS	(32)

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

	union {
		struct meta_cluster cluster;
		char meta_cluster_bytes[BLOCK_SIZE];
	};

	pthread_t threads[MAX_WORKER_THREADS];
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
	enum sanitize_mode sanitize_names;

	int error;
};

struct mdrestore_struct {
	FILE *in;
	FILE *out;

	pthread_t threads[MAX_WORKER_THREADS];
	size_t num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	struct rb_root chunk_tree;
	struct rb_root physical_tree;
	struct list_head list;
	struct list_head overlapping_chunks;
	size_t num_items;
	u32 nodesize;
	u64 devid;
	u64 alloced_chunks;
	u64 last_physical_offset;
	u8 uuid[BTRFS_UUID_SIZE];
	u8 fsid[BTRFS_FSID_SIZE];

	int compress_method;
	int done;
	int error;
	int old_restore;
	int fixup_offset;
	int multi_devices;
	int clear_space_cache;
	struct btrfs_fs_info *info;
};

static int search_for_chunk_blocks(struct mdrestore_struct *mdres,
				   u64 search, u64 cluster_bytenr);
static struct extent_buffer *alloc_dummy_eb(u64 bytenr, u32 size);

static void csum_block(u8 *buf, size_t len)
{
	u16 csum_size = btrfs_csum_type_size(BTRFS_CSUM_TYPE_CRC32);
	u8 result[csum_size];
	u32 crc = ~(u32)0;
	crc = crc32c(crc, buf + BTRFS_CSUM_SIZE, len - BTRFS_CSUM_SIZE);
	put_unaligned_le32(~crc, result);
	memcpy(buf, result, csum_size);
}

static int has_name(struct btrfs_key *key)
{
	switch (key->type) {
	case BTRFS_DIR_ITEM_KEY:
	case BTRFS_DIR_INDEX_KEY:
	case BTRFS_INODE_REF_KEY:
	case BTRFS_INODE_EXTREF_KEY:
	case BTRFS_XATTR_ITEM_KEY:
		return 1;
	default:
		break;
	}

	return 0;
}

static int chunk_cmp(struct rb_node *a, struct rb_node *b, int fuzz)
{
	struct fs_chunk *entry = rb_entry(a, struct fs_chunk, l);
	struct fs_chunk *ins = rb_entry(b, struct fs_chunk, l);

	if (fuzz && ins->logical >= entry->logical &&
	    ins->logical < entry->logical + entry->bytes)
		return 0;

	if (ins->logical < entry->logical)
		return -1;
	else if (ins->logical > entry->logical)
		return 1;
	return 0;
}

static int physical_cmp(struct rb_node *a, struct rb_node *b, int fuzz)
{
	struct fs_chunk *entry = rb_entry(a, struct fs_chunk, p);
	struct fs_chunk *ins = rb_entry(b, struct fs_chunk, p);

	if (fuzz && ins->physical >= entry->physical &&
	    ins->physical < entry->physical + entry->bytes)
		return 0;

	if (fuzz && entry->physical >= ins->physical &&
	    entry->physical < ins->physical + ins->bytes)
		return 0;

	if (ins->physical < entry->physical)
		return -1;
	else if (ins->physical > entry->physical)
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

		dir = cmp(*p, ins, 1);
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

static u64 logical_to_physical(struct mdrestore_struct *mdres, u64 logical,
			       u64 *size, u64 *physical_dup)
{
	struct fs_chunk *fs_chunk;
	struct rb_node *entry;
	struct fs_chunk search;
	u64 offset;

	if (logical == BTRFS_SUPER_INFO_OFFSET)
		return logical;

	search.logical = logical;
	entry = tree_search(&mdres->chunk_tree, &search.l, chunk_cmp, 1);
	if (!entry) {
		if (mdres->in != stdin)
			warning("cannot find a chunk, using logical");
		return logical;
	}
	fs_chunk = rb_entry(entry, struct fs_chunk, l);
	if (fs_chunk->logical > logical || fs_chunk->logical + fs_chunk->bytes < logical)
		BUG();
	offset = search.logical - fs_chunk->logical;

	if (physical_dup) {
		/* Only in dup case, physical_dup is not equal to 0 */
		if (fs_chunk->physical_dup)
			*physical_dup = fs_chunk->physical_dup + offset;
		else
			*physical_dup = 0;
	}

	*size = min(*size, fs_chunk->bytes + fs_chunk->logical - logical);
	return fs_chunk->physical + offset;
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
		item = btrfs_item_nr(i);
		btrfs_item_key_to_cpu(src, &key, i);
		if (key.type == BTRFS_CSUM_ITEM_KEY) {
			size = btrfs_item_size_nr(src, i);
			memset(dst + btrfs_leaf_data(src) +
			       btrfs_item_offset_nr(src, i), 0, size);
			continue;
		}

		if (md->sanitize_names && has_name(&key)) {
			sanitize_name(md->sanitize_names, &md->name_tree, dst,
					src, &key, i);
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
			if (!async->buffer) {
				error("not enough memory for async buffer");
				pthread_mutex_lock(&md->mutex);
				if (!md->error)
					md->error = -ENOMEM;
				pthread_mutex_unlock(&md->mutex);
				pthread_exit(NULL);
			}

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
	header = &md->cluster.header;
	header->magic = cpu_to_le64(HEADER_MAGIC);
	header->bytenr = cpu_to_le64(start);
	header->nritems = cpu_to_le32(0);
	header->compress = md->compress_level > 0 ?
			   COMPRESS_ZLIB : COMPRESS_NONE;
}

static void metadump_destroy(struct metadump_struct *md, int num_threads)
{
	int i;
	struct rb_node *n;

	pthread_mutex_lock(&md->mutex);
	md->done = 1;
	pthread_cond_broadcast(&md->cond);
	pthread_mutex_unlock(&md->mutex);

	for (i = 0; i < num_threads; i++)
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
}

static int metadump_init(struct metadump_struct *md, struct btrfs_root *root,
			 FILE *out, int num_threads, int compress_level,
			 enum sanitize_mode sanitize_names)
{
	int i, ret = 0;

	memset(md, 0, sizeof(*md));
	INIT_LIST_HEAD(&md->list);
	INIT_LIST_HEAD(&md->ordered);
	md->root = root;
	md->out = out;
	md->pending_start = (u64)-1;
	md->compress_level = compress_level;
	md->sanitize_names = sanitize_names;
	if (sanitize_names == SANITIZE_COLLISIONS)
		crc32c_optimization_init();

	md->name_tree.rb_node = NULL;
	md->num_threads = num_threads;
	pthread_cond_init(&md->cond, NULL);
	pthread_mutex_init(&md->mutex, NULL);
	meta_cluster_init(md, 0);

	if (!num_threads)
		return 0;

	for (i = 0; i < num_threads; i++) {
		ret = pthread_create(md->threads + i, NULL, dump_worker, md);
		if (ret)
			break;
	}

	if (ret)
		metadump_destroy(md, i + 1);

	return ret;
}

static int write_zero(FILE *out, size_t size)
{
	static char zero[BLOCK_SIZE];
	return fwrite(zero, size, 1, out);
}

static int write_buffers(struct metadump_struct *md, u64 *next)
{
	struct meta_cluster_header *header = &md->cluster.header;
	struct meta_cluster_item *item;
	struct async_work *async;
	u64 bytenr = 0;
	u32 nritems = 0;
	int ret;
	int err = 0;

	if (list_empty(&md->ordered))
		goto out;

	/* wait until all buffers are compressed */
	while (!err && md->num_items > md->num_ready) {
		struct timespec ts = {
			.tv_sec = 0,
			.tv_nsec = 10000000,
		};
		pthread_mutex_unlock(&md->mutex);
		nanosleep(&ts, NULL);
		pthread_mutex_lock(&md->mutex);
		err = md->error;
	}

	if (err) {
		errno = -err;
		error("one of the threads failed: %m");
		goto out;
	}

	/* setup and write index block */
	list_for_each_entry(async, &md->ordered, ordered) {
		item = &md->cluster.items[nritems];
		item->bytenr = cpu_to_le64(async->start);
		item->size = cpu_to_le32(async->bufsize);
		nritems++;
	}
	header->nritems = cpu_to_le32(nritems);

	ret = fwrite(&md->cluster, BLOCK_SIZE, 1, md->out);
	if (ret != 1) {
		error("unable to write out cluster: %m");
		return -errno;
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
			error("unable to write out cluster: %m");
			err = -errno;
			ret = 0;
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
			error("unable to zero out buffer: %m");
			err = -errno;
		}
	}
out:
	*next = bytenr;
	return err;
}

static int read_data_extent(struct metadump_struct *md,
			    struct async_work *async)
{
	struct btrfs_root *root = md->root;
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 bytes_left = async->size;
	u64 logical = async->start;
	u64 offset = 0;
	u64 read_len;
	int num_copies;
	int cur_mirror;
	int ret;

	num_copies = btrfs_num_copies(root->fs_info, logical, bytes_left);

	/* Try our best to read data, just like read_tree_block() */
	for (cur_mirror = 1; cur_mirror <= num_copies; cur_mirror++) {
		while (bytes_left) {
			read_len = bytes_left;
			ret = read_extent_data(fs_info,
					(char *)(async->buffer + offset),
					logical, &read_len, cur_mirror);
			if (ret < 0)
				break;
			offset += read_len;
			logical += read_len;
			bytes_left -= read_len;
		}
	}
	if (bytes_left)
		return -EIO;
	return 0;
}

static int get_dev_fd(struct btrfs_root *root)
{
	struct btrfs_device *dev;

	dev = list_first_entry(&root->fs_info->fs_devices->devices,
			       struct btrfs_device, dev_list);
	return dev->fd;
}

static int flush_pending(struct metadump_struct *md, int done)
{
	struct async_work *async = NULL;
	struct extent_buffer *eb;
	u64 start = 0;
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

		/*
		 * Balance can make the mapping not cover the super block, so
		 * just copy directly from one of the devices.
		 */
		if (start == BTRFS_SUPER_INFO_OFFSET) {
			int fd = get_dev_fd(md->root);

			ret = pread64(fd, async->buffer, size, start);
			if (ret < size) {
				free(async->buffer);
				free(async);
				error("unable to read superblock at %llu: %m",
						(unsigned long long)start);
				return -errno;
			}
			size = 0;
			ret = 0;
		}

		while (!md->data && size > 0) {
			u64 this_read = min((u64)md->root->fs_info->nodesize,
					size);

			eb = read_tree_block(md->root->fs_info, start, 0);
			if (!extent_buffer_uptodate(eb)) {
				free(async->buffer);
				free(async);
				error("unable to read metadata block %llu",
					(unsigned long long)start);
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
		if (ret) {
			errno = -ret;
			error("unable to write buffers: %m");
		} else {
			meta_cluster_init(md, start);
		}
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
	readahead_tree_block(md->root->fs_info, start, 0);
	md->pending_size += size;
	md->data = data;
	return 0;
}

static int copy_tree_blocks(struct btrfs_root *root, struct extent_buffer *eb,
			    struct metadump_struct *metadump, int root_tree)
{
	struct extent_buffer *tmp;
	struct btrfs_root_item *ri;
	struct btrfs_key key;
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 bytenr;
	int level;
	int nritems = 0;
	int i = 0;
	int ret;

	ret = add_extent(btrfs_header_bytenr(eb), fs_info->nodesize,
			 metadump, 0);
	if (ret) {
		error("unable to add metadata block %llu: %d",
				btrfs_header_bytenr(eb), ret);
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
			tmp = read_tree_block(fs_info, bytenr, 0);
			if (!extent_buffer_uptodate(tmp)) {
				error("unable to read log root block");
				return -EIO;
			}
			ret = copy_tree_blocks(root, tmp, metadump, 0);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		} else {
			bytenr = btrfs_node_blockptr(eb, i);
			tmp = read_tree_block(fs_info, bytenr, 0);
			if (!extent_buffer_uptodate(tmp)) {
				error("unable to read log root block");
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
			  struct metadump_struct *metadump)
{
	u64 blocknr = btrfs_super_log_root(root->fs_info->super_copy);

	if (blocknr == 0)
		return 0;

	if (!root->fs_info->log_root_tree ||
	    !root->fs_info->log_root_tree->node) {
		error("unable to copy tree log, it has not been setup");
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
		error("free space inode not found: %d", ret);
		return ret;
	}

	leaf = path->nodes[0];

	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0) {
				error("cannot go to next leaf %d", ret);
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
			error("unable to add space cache blocks %d", ret);
			btrfs_release_path(path);
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
	bytenr = BTRFS_SUPER_INFO_OFFSET + BTRFS_SUPER_INFO_SIZE;
	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0) {
		error("extent root not found: %d", ret);
		return ret;
	}
	ret = 0;

	leaf = path->nodes[0];

	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, path);
			if (ret < 0) {
				error("cannot go to next leaf %d", ret);
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
		if (key.type == BTRFS_METADATA_ITEM_KEY) {
			num_bytes = extent_root->fs_info->nodesize;
		} else {
			num_bytes = key.offset;
		}

		if (num_bytes == 0) {
			error("extent length 0 at bytenr %llu key type %d",
					(unsigned long long)bytenr, key.type);
			ret = -EIO;
			break;
		}

		if (btrfs_item_size_nr(leaf, path->slots[0]) > sizeof(*ei)) {
			ei = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_extent_item);
			if (btrfs_extent_flags(leaf, ei) &
			    BTRFS_EXTENT_FLAG_TREE_BLOCK) {
				ret = add_extent(bytenr, num_bytes, metadump,
						 0);
				if (ret) {
					error("unable to add block %llu: %d",
						(unsigned long long)bytenr, ret);
					break;
				}
			}
		} else {
			error(
	"either extent tree is corrupted or deprecated extent ref format");
			ret = -EIO;
			break;
		}
		bytenr += num_bytes;
	}

	btrfs_release_path(path);

	return ret;
}

static int create_metadump(const char *input, FILE *out, int num_threads,
			   int compress_level, enum sanitize_mode sanitize,
			   int walk_trees)
{
	struct btrfs_root *root;
	struct btrfs_path path;
	struct metadump_struct metadump;
	int ret;
	int err = 0;

	root = open_ctree(input, 0, 0);
	if (!root) {
		error("open ctree failed");
		return -EIO;
	}

	ret = metadump_init(&metadump, root, out, num_threads,
			    compress_level, sanitize);
	if (ret) {
		error("failed to initialize metadump: %d", ret);
		close_ctree(root);
		return ret;
	}

	ret = add_extent(BTRFS_SUPER_INFO_OFFSET, BTRFS_SUPER_INFO_SIZE,
			&metadump, 0);
	if (ret) {
		error("unable to add metadata: %d", ret);
		err = ret;
		goto out;
	}

	btrfs_init_path(&path);

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
		ret = copy_from_extent_tree(&metadump, &path);
		if (ret) {
			err = ret;
			goto out;
		}
	}

	ret = copy_log_trees(root, &metadump);
	if (ret) {
		err = ret;
		goto out;
	}

	ret = copy_space_cache(root, &metadump, &path);
out:
	ret = flush_pending(&metadump, 1);
	if (ret) {
		if (!err)
			err = ret;
		error("failed to flush pending data: %d", ret);
	}

	metadump_destroy(&metadump, num_threads);

	btrfs_release_path(&path);
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
	btrfs_set_stack_chunk_stripe_len(chunk, BTRFS_STRIPE_LEN);
	btrfs_set_stack_chunk_type(chunk, BTRFS_BLOCK_GROUP_SYSTEM);
	btrfs_set_stack_chunk_io_align(chunk, sectorsize);
	btrfs_set_stack_chunk_io_width(chunk, sectorsize);
	btrfs_set_stack_chunk_sector_size(chunk, sectorsize);
	btrfs_set_stack_chunk_num_stripes(chunk, 1);
	btrfs_set_stack_chunk_sub_stripes(chunk, 0);
	chunk->stripe.devid = super->dev_item.devid;
	btrfs_set_stack_stripe_offset(&chunk->stripe, 0);
	memcpy(chunk->stripe.dev_uuid, super->dev_item.uuid, BTRFS_UUID_SIZE);
	btrfs_set_super_sys_array_size(super, sizeof(*key) + sizeof(*chunk));
	csum_block(buffer, BTRFS_SUPER_INFO_SIZE);
}

static int update_super(struct mdrestore_struct *mdres, u8 *buffer)
{
	struct btrfs_super_block *super = (struct btrfs_super_block *)buffer;
	struct btrfs_chunk *chunk;
	struct btrfs_disk_key *disk_key;
	struct btrfs_key key;
	u64 flags = btrfs_super_flags(super);
	u32 new_array_size = 0;
	u32 array_size;
	u32 cur = 0;
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

		if (key.type == BTRFS_CHUNK_ITEM_KEY) {
			u64 type, physical, physical_dup, size = 0;

			chunk = (struct btrfs_chunk *)ptr;
			old_num_stripes = btrfs_stack_chunk_num_stripes(chunk);
			chunk = (struct btrfs_chunk *)write_ptr;

			memmove(write_ptr, ptr, sizeof(*chunk));
			btrfs_set_stack_chunk_sub_stripes(chunk, 0);
			type = btrfs_stack_chunk_type(chunk);
			if (type & BTRFS_BLOCK_GROUP_DUP) {
				new_array_size += sizeof(struct btrfs_stripe);
				write_ptr += sizeof(struct btrfs_stripe);
			} else {
				btrfs_set_stack_chunk_num_stripes(chunk, 1);
				btrfs_set_stack_chunk_type(chunk,
						BTRFS_BLOCK_GROUP_SYSTEM);
			}
			chunk->stripe.devid = super->dev_item.devid;
			physical = logical_to_physical(mdres, key.offset,
						       &size, &physical_dup);
			if (size != (u64)-1)
				btrfs_set_stack_stripe_offset(&chunk->stripe,
							      physical);
			memcpy(chunk->stripe.dev_uuid, super->dev_item.uuid,
			       BTRFS_UUID_SIZE);
			new_array_size += sizeof(*chunk);
		} else {
			error("bogus key in the sys array %d", key.type);
			return -EIO;
		}
		write_ptr += sizeof(*chunk);
		ptr += btrfs_chunk_item_size(old_num_stripes);
		cur += btrfs_chunk_item_size(old_num_stripes);
	}

	if (mdres->clear_space_cache)
		btrfs_set_super_cache_generation(super, 0);

	flags |= BTRFS_SUPER_FLAG_METADUMP_V2;
	btrfs_set_super_flags(super, flags);
	btrfs_set_super_sys_array_size(super, new_array_size);
	btrfs_set_super_num_devices(super, 1);
	csum_block(buffer, BTRFS_SUPER_INFO_SIZE);

	return 0;
}

static struct extent_buffer *alloc_dummy_eb(u64 bytenr, u32 size)
{
	struct extent_buffer *eb;

	eb = calloc(1, sizeof(struct extent_buffer) + size);
	if (!eb)
		return NULL;

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
		item = btrfs_item_nr(i);
		ioff = btrfs_item_offset(eb, item);
		btrfs_set_item_offset(eb, item, ioff + size_diff);
	}

	memmove_extent_buffer(eb, btrfs_leaf_data(eb) + data_end + size_diff,
			      btrfs_leaf_data(eb) + data_end,
			      old_data_start + new_size - data_end);
	item = btrfs_item_nr(slot);
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

	if (size_left % mdres->nodesize)
		return 0;

	eb = alloc_dummy_eb(bytenr, mdres->nodesize);
	if (!eb)
		return -ENOMEM;

	while (size_left) {
		eb->start = bytenr;
		memcpy(eb->data, buffer, mdres->nodesize);

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
			struct btrfs_chunk *chunk;
			struct btrfs_key key;
			u64 type, physical, physical_dup, size = (u64)-1;

			btrfs_item_key_to_cpu(eb, &key, i);
			if (key.type != BTRFS_CHUNK_ITEM_KEY)
				continue;

			size = 0;
			physical = logical_to_physical(mdres, key.offset,
						       &size, &physical_dup);

			if (!physical_dup)
				truncate_item(eb, i, sizeof(*chunk));
			chunk = btrfs_item_ptr(eb, i, struct btrfs_chunk);


			/* Zero out the RAID profile */
			type = btrfs_chunk_type(eb, chunk);
			type &= (BTRFS_BLOCK_GROUP_DATA |
				 BTRFS_BLOCK_GROUP_SYSTEM |
				 BTRFS_BLOCK_GROUP_METADATA |
				 BTRFS_BLOCK_GROUP_DUP);
			btrfs_set_chunk_type(eb, chunk, type);

			if (!physical_dup)
				btrfs_set_chunk_num_stripes(eb, chunk, 1);
			btrfs_set_chunk_sub_stripes(eb, chunk, 0);
			btrfs_set_stripe_devid_nr(eb, chunk, 0, mdres->devid);
			if (size != (u64)-1)
				btrfs_set_stripe_offset_nr(eb, chunk, 0,
							   physical);
			/* update stripe 2 offset */
			if (physical_dup)
				btrfs_set_stripe_offset_nr(eb, chunk, 1,
							   physical_dup);

			write_extent_buffer(eb, mdres->uuid,
					(unsigned long)btrfs_stripe_dev_uuid_nr(
						chunk, 0),
					BTRFS_UUID_SIZE);
		}
		memcpy(buffer, eb->data, eb->len);
		csum_block(buffer, eb->len);
next:
		size_left -= mdres->nodesize;
		buffer += mdres->nodesize;
		bytenr += mdres->nodesize;
	}

	free(eb);
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
		error(
	"cannot stat restore point, won't be able to write backup supers: %m");
		return;
	}

	size = btrfs_device_size(fd, &st);

	for (i = 1; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		if (bytenr + BTRFS_SUPER_INFO_SIZE > size)
			break;
		btrfs_set_super_bytenr(super, bytenr);
		csum_block(buf, BTRFS_SUPER_INFO_SIZE);
		ret = pwrite64(fd, buf, BTRFS_SUPER_INFO_SIZE, bytenr);
		if (ret < BTRFS_SUPER_INFO_SIZE) {
			if (ret < 0)
				error(
				"problem writing out backup super block %d: %m", i);
			else
				error("short write writing out backup super block");
			break;
		}
	}
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
	int compress_size = MAX_PENDING_SIZE * 4;

	outfd = fileno(mdres->out);
	buffer = malloc(compress_size);
	if (!buffer) {
		error("not enough memory for restore worker buffer");
		pthread_mutex_lock(&mdres->mutex);
		if (!mdres->error)
			mdres->error = -ENOMEM;
		pthread_mutex_unlock(&mdres->mutex);
		pthread_exit(NULL);
	}

	while (1) {
		u64 bytenr, physical_dup;
		off_t offset = 0;
		int err = 0;

		pthread_mutex_lock(&mdres->mutex);
		while (!mdres->nodesize || list_empty(&mdres->list)) {
			if (mdres->done) {
				pthread_mutex_unlock(&mdres->mutex);
				goto out;
			}
			pthread_cond_wait(&mdres->cond, &mdres->mutex);
		}
		async = list_entry(mdres->list.next, struct async_work, list);
		list_del_init(&async->list);

		if (mdres->compress_method == COMPRESS_ZLIB) {
			size = compress_size;
			pthread_mutex_unlock(&mdres->mutex);
			ret = uncompress(buffer, (unsigned long *)&size,
					 async->buffer, async->bufsize);
			pthread_mutex_lock(&mdres->mutex);
			if (ret != Z_OK) {
				error("decompression failed with %d", ret);
				err = -EIO;
			}
			outbuf = buffer;
		} else {
			outbuf = async->buffer;
			size = async->bufsize;
		}

		if (!mdres->multi_devices) {
			if (async->start == BTRFS_SUPER_INFO_OFFSET) {
				if (mdres->old_restore) {
					update_super_old(outbuf);
				} else {
					ret = update_super(mdres, outbuf);
					if (ret)
						err = ret;
				}
			} else if (!mdres->old_restore) {
				ret = fixup_chunk_tree_block(mdres, async, outbuf, size);
				if (ret)
					err = ret;
			}
		}

		if (!mdres->fixup_offset) {
			while (size) {
				u64 chunk_size = size;
				physical_dup = 0;
				if (!mdres->multi_devices && !mdres->old_restore)
					bytenr = logical_to_physical(mdres,
						     async->start + offset,
						     &chunk_size,
						     &physical_dup);
				else
					bytenr = async->start + offset;

				ret = pwrite64(outfd, outbuf+offset, chunk_size,
					       bytenr);
				if (ret != chunk_size)
					goto error;

				if (physical_dup)
					ret = pwrite64(outfd, outbuf+offset,
						       chunk_size,
						       physical_dup);
				if (ret != chunk_size)
					goto error;

				size -= chunk_size;
				offset += chunk_size;
				continue;

error:
				if (ret < 0) {
					error("unable to write to device: %m");
					err = errno;
				} else {
					error("short write");
					err = -EIO;
				}
			}
		} else if (async->start != BTRFS_SUPER_INFO_OFFSET) {
			ret = write_data_to_disk(mdres->info, outbuf, async->start, size, 0);
			if (ret) {
				error("failed to write data");
				exit(1);
			}
		}


		/* backup super blocks are already there at fixup_offset stage */
		if (!mdres->multi_devices && async->start == BTRFS_SUPER_INFO_OFFSET)
			write_backup_supers(outfd, outbuf);

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

static void mdrestore_destroy(struct mdrestore_struct *mdres, int num_threads)
{
	struct rb_node *n;
	int i;

	while ((n = rb_first(&mdres->chunk_tree))) {
		struct fs_chunk *entry;

		entry = rb_entry(n, struct fs_chunk, l);
		rb_erase(n, &mdres->chunk_tree);
		rb_erase(&entry->p, &mdres->physical_tree);
		free(entry);
	}
	pthread_mutex_lock(&mdres->mutex);
	mdres->done = 1;
	pthread_cond_broadcast(&mdres->cond);
	pthread_mutex_unlock(&mdres->mutex);

	for (i = 0; i < num_threads; i++)
		pthread_join(mdres->threads[i], NULL);

	pthread_cond_destroy(&mdres->cond);
	pthread_mutex_destroy(&mdres->mutex);
}

static int mdrestore_init(struct mdrestore_struct *mdres,
			  FILE *in, FILE *out, int old_restore,
			  int num_threads, int fixup_offset,
			  struct btrfs_fs_info *info, int multi_devices)
{
	int i, ret = 0;

	memset(mdres, 0, sizeof(*mdres));
	pthread_cond_init(&mdres->cond, NULL);
	pthread_mutex_init(&mdres->mutex, NULL);
	INIT_LIST_HEAD(&mdres->list);
	INIT_LIST_HEAD(&mdres->overlapping_chunks);
	mdres->in = in;
	mdres->out = out;
	mdres->old_restore = old_restore;
	mdres->chunk_tree.rb_node = NULL;
	mdres->fixup_offset = fixup_offset;
	mdres->info = info;
	mdres->multi_devices = multi_devices;
	mdres->clear_space_cache = 0;
	mdres->last_physical_offset = 0;
	mdres->alloced_chunks = 0;

	if (!num_threads)
		return 0;

	mdres->num_threads = num_threads;
	for (i = 0; i < num_threads; i++) {
		ret = pthread_create(&mdres->threads[i], NULL, restore_worker,
				     mdres);
		if (ret) {
			/* pthread_create returns errno directly */
			ret = -ret;
			break;
		}
	}
	if (ret)
		mdrestore_destroy(mdres, i + 1);
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
	if (mdres->nodesize)
		return 0;

	if (mdres->compress_method == COMPRESS_ZLIB) {
		size_t size = MAX_PENDING_SIZE * 2;

		buffer = malloc(MAX_PENDING_SIZE * 2);
		if (!buffer)
			return -ENOMEM;
		ret = uncompress(buffer, (unsigned long *)&size,
				 async->buffer, async->bufsize);
		if (ret != Z_OK) {
			error("decompression failed with %d", ret);
			free(buffer);
			return -EIO;
		}
		outbuf = buffer;
	} else {
		outbuf = async->buffer;
	}

	super = (struct btrfs_super_block *)outbuf;
	mdres->nodesize = btrfs_super_nodesize(super);
	if (btrfs_super_incompat_flags(super) &
	    BTRFS_FEATURE_INCOMPAT_METADATA_UUID)
		memcpy(mdres->fsid, super->metadata_uuid, BTRFS_FSID_SIZE);
	else
		memcpy(mdres->fsid, super->fsid, BTRFS_FSID_SIZE);
	memcpy(mdres->uuid, super->dev_item.uuid, BTRFS_UUID_SIZE);
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

	pthread_mutex_lock(&mdres->mutex);
	mdres->compress_method = header->compress;
	pthread_mutex_unlock(&mdres->mutex);

	bytenr = le64_to_cpu(header->bytenr) + BLOCK_SIZE;
	nritems = le32_to_cpu(header->nritems);
	for (i = 0; i < nritems; i++) {
		item = &cluster->items[i];
		async = calloc(1, sizeof(*async));
		if (!async) {
			error("not enough memory for async data");
			return -ENOMEM;
		}
		async->start = le64_to_cpu(item->bytenr);
		async->bufsize = le32_to_cpu(item->size);
		async->buffer = malloc(async->bufsize);
		if (!async->buffer) {
			error("not enough memory for async buffer");
			free(async);
			return -ENOMEM;
		}
		ret = fread(async->buffer, async->bufsize, 1, mdres->in);
		if (ret != 1) {
			error("unable to read buffer: %m");
			free(async->buffer);
			free(async);
			return -EIO;
		}
		bytenr += async->bufsize;

		pthread_mutex_lock(&mdres->mutex);
		if (async->start == BTRFS_SUPER_INFO_OFFSET) {
			ret = fill_mdres_info(mdres, async);
			if (ret) {
				error("unable to set up restore state");
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
			error("failed to read buffer: %m");
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

	eb = alloc_dummy_eb(bytenr, mdres->nodesize);
	if (!eb) {
		ret = -ENOMEM;
		goto out;
	}

	while (item_bytenr != bytenr) {
		buffer += mdres->nodesize;
		item_bytenr += mdres->nodesize;
	}

	memcpy(eb->data, buffer, mdres->nodesize);
	if (btrfs_header_bytenr(eb) != bytenr) {
		error("eb bytenr does not match found bytenr: %llu != %llu",
				(unsigned long long)btrfs_header_bytenr(eb),
				(unsigned long long)bytenr);
		ret = -EIO;
		goto out;
	}

	if (memcmp(mdres->fsid, eb->data + offsetof(struct btrfs_header, fsid),
		   BTRFS_FSID_SIZE)) {
		error("filesystem metadata UUID of eb %llu does not match",
				(unsigned long long)bytenr);
		ret = -EIO;
		goto out;
	}

	if (btrfs_header_owner(eb) != BTRFS_CHUNK_TREE_OBJECTID) {
		error("wrong eb %llu owner %llu",
				(unsigned long long)bytenr,
				(unsigned long long)btrfs_header_owner(eb));
		ret = -EIO;
		goto out;
	}

	for (i = 0; i < btrfs_header_nritems(eb); i++) {
		struct btrfs_chunk *chunk;
		struct fs_chunk *fs_chunk;
		struct btrfs_key key;
		u64 type;

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
			error("not enough memory to allocate chunk");
			ret = -ENOMEM;
			break;
		}
		memset(fs_chunk, 0, sizeof(*fs_chunk));
		chunk = btrfs_item_ptr(eb, i, struct btrfs_chunk);

		fs_chunk->logical = key.offset;
		fs_chunk->physical = btrfs_stripe_offset_nr(eb, chunk, 0);
		fs_chunk->bytes = btrfs_chunk_length(eb, chunk);
		INIT_LIST_HEAD(&fs_chunk->list);
		if (tree_search(&mdres->physical_tree, &fs_chunk->p,
				physical_cmp, 1) != NULL)
			list_add(&fs_chunk->list, &mdres->overlapping_chunks);
		else
			tree_insert(&mdres->physical_tree, &fs_chunk->p,
				    physical_cmp);

		type = btrfs_chunk_type(eb, chunk);
		if (type & BTRFS_BLOCK_GROUP_DUP) {
			fs_chunk->physical_dup =
					btrfs_stripe_offset_nr(eb, chunk, 1);
		}

		if (fs_chunk->physical_dup + fs_chunk->bytes >
		    mdres->last_physical_offset)
			mdres->last_physical_offset = fs_chunk->physical_dup +
				fs_chunk->bytes;
		else if (fs_chunk->physical + fs_chunk->bytes >
		    mdres->last_physical_offset)
			mdres->last_physical_offset = fs_chunk->physical +
				fs_chunk->bytes;
		mdres->alloced_chunks += fs_chunk->bytes;
		/* in dup case, fs_chunk->bytes should add twice */
		if (fs_chunk->physical_dup)
			mdres->alloced_chunks += fs_chunk->bytes;
		tree_insert(&mdres->chunk_tree, &fs_chunk->l, chunk_cmp);
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
	u32 max_size = MAX_PENDING_SIZE * 2;
	u8 *buffer, *tmp = NULL;
	int ret = 0;

	cluster = malloc(BLOCK_SIZE);
	if (!cluster) {
		error("not enough memory for cluster");
		return -ENOMEM;
	}

	buffer = malloc(max_size);
	if (!buffer) {
		error("not enough memory for buffer");
		free(cluster);
		return -ENOMEM;
	}

	if (mdres->compress_method == COMPRESS_ZLIB) {
		tmp = malloc(max_size);
		if (!tmp) {
			error("not enough memory for buffer");
			free(cluster);
			free(buffer);
			return -ENOMEM;
		}
	}

	bytenr = current_cluster;
	while (1) {
		if (fseek(mdres->in, current_cluster, SEEK_SET)) {
			error("seek failed: %m");
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
			error(
	"unknown state after reading cluster at %llu, probably corrupted data",
					cluster_bytenr);
			ret = -EIO;
			break;
		} else if (ret < 0) {
			error("unable to read image at %llu: %m",
					(unsigned long long)cluster_bytenr);
			break;
		}
		ret = 0;

		header = &cluster->header;
		if (le64_to_cpu(header->magic) != HEADER_MAGIC ||
		    le64_to_cpu(header->bytenr) != current_cluster) {
			error("bad header in metadump image");
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

			if (bufsize > max_size) {
				error("item %u too big: %u > %u", i, bufsize,
						max_size);
				ret = -EIO;
				break;
			}

			if (mdres->compress_method == COMPRESS_ZLIB) {
				ret = fread(tmp, bufsize, 1, mdres->in);
				if (ret != 1) {
					error("read error: %m");
					ret = -EIO;
					break;
				}

				size = max_size;
				ret = uncompress(buffer,
						 (unsigned long *)&size, tmp,
						 bufsize);
				if (ret != Z_OK) {
					error("decompression failed with %d",
							ret);
					ret = -EIO;
					break;
				}
			} else {
				ret = fread(buffer, bufsize, 1, mdres->in);
				if (ret != 1) {
					error("read error: %m");
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
		error("unable to read cluster: %m");
		return -EIO;
	}
	ret = 0;

	header = &cluster->header;
	if (le64_to_cpu(header->magic) != HEADER_MAGIC ||
	    le64_to_cpu(header->bytenr) != 0) {
		error("bad header in metadump image");
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
			error("seek failed: %m");
			return -EIO;
		}
	}

	if (!item || le64_to_cpu(item->bytenr) != BTRFS_SUPER_INFO_OFFSET) {
		error("did not find superblock at %llu",
				le64_to_cpu(item->bytenr));
		return -EINVAL;
	}

	buffer = malloc(le32_to_cpu(item->size));
	if (!buffer) {
		error("not enough memory to allocate buffer");
		return -ENOMEM;
	}

	ret = fread(buffer, le32_to_cpu(item->size), 1, mdres->in);
	if (ret != 1) {
		error("unable to read buffer: %m");
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
			error("decompression failed with %d", ret);
			free(buffer);
			free(tmp);
			return -EIO;
		}
		free(buffer);
		buffer = tmp;
	}

	pthread_mutex_lock(&mdres->mutex);
	super = (struct btrfs_super_block *)buffer;
	ret = btrfs_check_super(super, 0);
	if (ret < 0) {
		error("invalid superblock");
		return ret;
	}
	chunk_root_bytenr = btrfs_super_chunk_root(super);
	mdres->nodesize = btrfs_super_nodesize(super);
	if (btrfs_super_incompat_flags(super) &
	    BTRFS_FEATURE_INCOMPAT_METADATA_UUID)
		memcpy(mdres->fsid, super->metadata_uuid, BTRFS_FSID_SIZE);
	else
		memcpy(mdres->fsid, super->fsid, BTRFS_FSID_SIZE);

	memcpy(mdres->uuid, super->dev_item.uuid, BTRFS_UUID_SIZE);
	mdres->devid = le64_to_cpu(super->dev_item.devid);
	free(buffer);
	pthread_mutex_unlock(&mdres->mutex);

	return search_for_chunk_blocks(mdres, chunk_root_bytenr, 0);
}

static int range_contains_super(u64 physical, u64 bytes)
{
	u64 super_bytenr;
	int i;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		super_bytenr = btrfs_sb_offset(i);
		if (super_bytenr >= physical &&
		    super_bytenr < physical + bytes)
			return 1;
	}

	return 0;
}

static void remap_overlapping_chunks(struct mdrestore_struct *mdres)
{
	struct fs_chunk *fs_chunk;

	while (!list_empty(&mdres->overlapping_chunks)) {
		fs_chunk = list_first_entry(&mdres->overlapping_chunks,
					    struct fs_chunk, list);
		list_del_init(&fs_chunk->list);
		if (range_contains_super(fs_chunk->physical,
					 fs_chunk->bytes)) {
			warning(
"remapping a chunk that had a super mirror inside of it, clearing space cache so we don't end up with corruption");
			mdres->clear_space_cache = 1;
		}
		fs_chunk->physical = mdres->last_physical_offset;
		tree_insert(&mdres->physical_tree, &fs_chunk->p, physical_cmp);
		mdres->last_physical_offset += fs_chunk->bytes;
	}
}

static int fixup_device_size(struct btrfs_trans_handle *trans,
			     struct mdrestore_struct *mdres, int out_fd)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_dev_item *dev_item;
	struct btrfs_dev_extent *dev_ext;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	struct btrfs_root *root = fs_info->chunk_root;
	struct btrfs_key key;
	struct stat buf;
	u64 devid, cur_devid;
	u64 dev_size; /* Get from last dev extents */
	int ret;

	dev_item = &fs_info->super_copy->dev_item;

	btrfs_init_path(&path);
	devid = btrfs_stack_device_id(dev_item);

	key.objectid = devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, fs_info->dev_root, &key, &path, 0, 0);
	if (ret < 0) {
		errno = -ret;
		error("failed to locate last dev extent of devid %llu: %m",
			devid);
		btrfs_release_path(&path);
		return ret;
	}
	if (ret == 0) {
		error("found invalid dev extent devid %llu offset -1", devid);
		btrfs_release_path(&path);
		return -EUCLEAN;
	}
	ret = btrfs_previous_item(fs_info->dev_root, &path, devid,
				  BTRFS_DEV_EXTENT_KEY);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0) {
		errno = -ret;
		error("failed to locate last dev extent of devid %llu: %m",
			devid);
		btrfs_release_path(&path);
		return ret;
	}

	btrfs_item_key_to_cpu(path.nodes[0], &key, path.slots[0]);
	dev_ext = btrfs_item_ptr(path.nodes[0], path.slots[0],
				 struct btrfs_dev_extent);
	dev_size = key.offset + btrfs_dev_extent_length(path.nodes[0], dev_ext);
	btrfs_release_path(&path);

	btrfs_set_stack_device_total_bytes(dev_item, dev_size);
	btrfs_set_stack_device_bytes_used(dev_item, mdres->alloced_chunks);
	ret = fstat(out_fd, &buf);
	if (ret < 0) {
		error("failed to stat result image: %m");
		return -errno;
	}
	if (S_ISREG(buf.st_mode)) {
		/* Don't forget to enlarge the real file */
		ret = ftruncate64(out_fd, dev_size);
		if (ret < 0) {
			error("failed to enlarge result image: %m");
			return -errno;
		}
	}

	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = 0;

again:
	ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
	if (ret < 0) {
		error("search failed: %d", ret);
		return ret;
	}

	while (1) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0) {
				error("cannot go to next leaf %d", ret);
				exit(1);
			}
			if (ret > 0) {
				ret = 0;
				break;
			}
			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type > BTRFS_DEV_ITEM_KEY)
			break;
		if (key.type != BTRFS_DEV_ITEM_KEY) {
			path.slots[0]++;
			continue;
		}

		dev_item = btrfs_item_ptr(leaf, path.slots[0],
					  struct btrfs_dev_item);
		cur_devid = btrfs_device_id(leaf, dev_item);
		if (devid != cur_devid) {
			ret = btrfs_del_item(trans, root, &path);
			if (ret) {
				error("cannot delete item: %d", ret);
				exit(1);
			}
			btrfs_release_path(&path);
			goto again;
		}

		btrfs_set_device_total_bytes(leaf, dev_item, dev_size);
		btrfs_set_device_bytes_used(leaf, dev_item,
					    mdres->alloced_chunks);
		btrfs_mark_buffer_dirty(leaf);
		path.slots[0]++;
	}

	btrfs_release_path(&path);
	return 0;
}

static void fixup_block_groups(struct btrfs_fs_info *fs_info)
{
	struct btrfs_block_group_cache *bg;
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	struct cache_extent *ce;
	struct map_lookup *map;
	u64 extra_flags;

	for (ce = search_cache_extent(&map_tree->cache_tree, 0); ce;
	     ce = next_cache_extent(ce)) {
		map = container_of(ce, struct map_lookup, ce);

		bg = btrfs_lookup_block_group(fs_info, ce->start);
		if (!bg) {
			warning(
		"cannot find block group %llu, filesystem may not be mountable",
				ce->start);
			continue;
		}
		extra_flags = map->type & BTRFS_BLOCK_GROUP_PROFILE_MASK;

		if (bg->flags == map->type)
			continue;

		/* Update the block group item and mark the bg dirty */
		bg->flags = map->type;
		btrfs_set_block_group_flags(&bg->item, bg->flags);
		set_extent_bits(&fs_info->block_group_cache, ce->start,
				ce->start + ce->size - 1, BLOCK_GROUP_DIRTY);

		/*
		 * Chunk and bg flags can be different, changing bg flags
		 * without update avail_data/meta_alloc_bits will lead to
		 * ENOSPC.
		 * So here we set avail_*_alloc_bits to match chunk types.
		 */
		if (map->type & BTRFS_BLOCK_GROUP_DATA)
			fs_info->avail_data_alloc_bits = extra_flags;
		if (map->type & BTRFS_BLOCK_GROUP_METADATA)
			fs_info->avail_metadata_alloc_bits = extra_flags;
		if (map->type & BTRFS_BLOCK_GROUP_SYSTEM)
			fs_info->avail_system_alloc_bits = extra_flags;
	}
}

static int remove_all_dev_extents(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *root = fs_info->dev_root;
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	int slot;
	int ret;

	key.objectid = 1;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = 0;
	btrfs_init_path(&path);

	ret = btrfs_search_slot(trans, root, &key, &path, -1, 1);
	if (ret < 0) {
		errno = -ret;
		error("failed to search dev tree: %m");
		return ret;
	}

	while (1) {
		slot = path.slots[0];
		leaf = path.nodes[0];
		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret < 0) {
				errno = -ret;
				error("failed to search dev tree: %m");
				goto out;
			}
			if (ret > 0) {
				ret = 0;
				goto out;
			}
		}

		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.type != BTRFS_DEV_EXTENT_KEY)
			break;
		ret = btrfs_del_item(trans, root, &path);
		if (ret < 0) {
			errno = -ret;
			error("failed to delete dev extent %llu, %llu: %m",
				key.objectid, key.offset);
			goto out;
		}
	}
out:
	btrfs_release_path(&path);
	return ret;
}

static int fixup_dev_extents(struct btrfs_trans_handle *trans)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_mapping_tree *map_tree = &fs_info->mapping_tree;
	struct btrfs_device *dev;
	struct cache_extent *ce;
	struct map_lookup *map;
	u64 devid = btrfs_stack_device_id(&fs_info->super_copy->dev_item);
	int i;
	int ret;

	ret = remove_all_dev_extents(trans);
	if (ret < 0) {
		errno = -ret;
		error("failed to remove all existing dev extents: %m");
	}

	dev = btrfs_find_device(fs_info, devid, NULL, NULL);
	if (!dev) {
		error("faild to find devid %llu", devid);
		return -ENODEV;
	}

	/* Rebuild all dev extents using chunk maps */
	for (ce = search_cache_extent(&map_tree->cache_tree, 0); ce;
	     ce = next_cache_extent(ce)) {
		u64 stripe_len;

		map = container_of(ce, struct map_lookup, ce);
		stripe_len = calc_stripe_length(map->type, ce->size,
						map->num_stripes);
		for (i = 0; i < map->num_stripes; i++) {
			ret = btrfs_insert_dev_extent(trans, dev, ce->start,
					stripe_len, map->stripes[i].physical);
			if (ret < 0) {
				errno = -ret;
				error(
				"failed to insert dev extent %llu %llu: %m",
					devid, map->stripes[i].physical);
				goto out;
			}
		}
	}
out:
	return ret;
}

static int fixup_chunks_and_devices(struct btrfs_fs_info *fs_info,
			 struct mdrestore_struct *mdres, int out_fd)
{
	struct btrfs_trans_handle *trans;
	int ret;

	if (btrfs_super_log_root(fs_info->super_copy)) {
		warning(
		"log tree detected, its generation will not match superblock");
	}
	trans = btrfs_start_transaction(fs_info->tree_root, 1);
	if (IS_ERR(trans)) {
		error("cannot start transaction %ld", PTR_ERR(trans));
		return PTR_ERR(trans);
	}

	fixup_block_groups(fs_info);
	ret = fixup_dev_extents(trans);
	if (ret < 0)
		goto error;

	ret = fixup_device_size(trans, mdres, out_fd);
	if (ret < 0)
		goto error;

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret) {
		error("unable to commit transaction: %d", ret);
		return ret;
	}
	return 0;
error:
	errno = -ret;
	error(
"failed to fix chunks and devices mapping, the fs may not be mountable: %m");
	btrfs_abort_transaction(trans, ret);
	return ret;
}

static int restore_metadump(const char *input, FILE *out, int old_restore,
			    int num_threads, int fixup_offset,
			    const char *target, int multi_devices)
{
	struct meta_cluster *cluster = NULL;
	struct meta_cluster_header *header;
	struct mdrestore_struct mdrestore;
	struct btrfs_fs_info *info = NULL;
	u64 bytenr = 0;
	FILE *in = NULL;
	int ret = 0;

	if (!strcmp(input, "-")) {
		in = stdin;
	} else {
		in = fopen(input, "r");
		if (!in) {
			error("unable to open metadump image: %m");
			return 1;
		}
	}

	/* NOTE: open with write mode */
	if (fixup_offset) {
		info = open_ctree_fs_info(target, 0, 0, 0,
					  OPEN_CTREE_WRITES |
					  OPEN_CTREE_RESTORE |
					  OPEN_CTREE_PARTIAL);
		if (!info) {
			error("open ctree failed");
			ret = -EIO;
			goto failed_open;
		}
	}

	cluster = malloc(BLOCK_SIZE);
	if (!cluster) {
		error("not enough memory for cluster");
		ret = -ENOMEM;
		goto failed_info;
	}

	ret = mdrestore_init(&mdrestore, in, out, old_restore, num_threads,
			     fixup_offset, info, multi_devices);
	if (ret) {
		error("failed to initialize metadata restore state: %d", ret);
		goto failed_cluster;
	}

	if (!multi_devices && !old_restore) {
		ret = build_chunk_tree(&mdrestore, cluster);
		if (ret)
			goto out;
		if (!list_empty(&mdrestore.overlapping_chunks))
			remap_overlapping_chunks(&mdrestore);
	}

	if (in != stdin && fseek(in, 0, SEEK_SET)) {
		error("seek failed: %m");
		goto out;
	}

	while (!mdrestore.error) {
		ret = fread(cluster, BLOCK_SIZE, 1, in);
		if (!ret)
			break;

		header = &cluster->header;
		if (le64_to_cpu(header->magic) != HEADER_MAGIC ||
		    le64_to_cpu(header->bytenr) != bytenr) {
			error("bad header in metadump image");
			ret = -EIO;
			break;
		}
		ret = add_cluster(cluster, &mdrestore, &bytenr);
		if (ret) {
			error("failed to add cluster: %d", ret);
			break;
		}
	}
	ret = wait_for_worker(&mdrestore);

	if (!ret && !multi_devices && !old_restore) {
		struct btrfs_root *root;
		struct stat st;

		root = open_ctree_fd(fileno(out), target, 0,
					  OPEN_CTREE_PARTIAL |
					  OPEN_CTREE_WRITES |
					  OPEN_CTREE_NO_DEVICES);
		if (!root) {
			error("open ctree failed in %s", target);
			ret = -EIO;
			goto out;
		}
		info = root->fs_info;

		if (stat(target, &st)) {
			error("stat %s failed: %m", target);
			close_ctree(info->chunk_root);
			free(cluster);
			return 1;
		}

		ret = fixup_chunks_and_devices(info, &mdrestore, fileno(out));
		close_ctree(info->chunk_root);
		if (ret)
			goto out;
	}
out:
	mdrestore_destroy(&mdrestore, num_threads);
failed_cluster:
	free(cluster);
failed_info:
	if (fixup_offset && info)
		close_ctree(info->chunk_root);
failed_open:
	if (in != stdin)
		fclose(in);
	return ret;
}

static int update_disk_super_on_device(struct btrfs_fs_info *info,
				       const char *other_dev, u64 cur_devid)
{
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_path path;
	struct btrfs_dev_item *dev_item;
	struct btrfs_super_block *disk_super;
	char dev_uuid[BTRFS_UUID_SIZE];
	char fs_uuid[BTRFS_UUID_SIZE];
	u64 devid, type, io_align, io_width;
	u64 sector_size, total_bytes, bytes_used;
	char buf[BTRFS_SUPER_INFO_SIZE];
	int fp = -1;
	int ret;

	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = cur_devid;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, info->chunk_root, &key, &path, 0, 0);
	if (ret) {
		error("search key failed: %d", ret);
		ret = -EIO;
		goto out;
	}

	leaf = path.nodes[0];
	dev_item = btrfs_item_ptr(leaf, path.slots[0],
				  struct btrfs_dev_item);

	devid = btrfs_device_id(leaf, dev_item);
	if (devid != cur_devid) {
		error("devid mismatch: %llu != %llu",
				(unsigned long long)devid,
				(unsigned long long)cur_devid);
		ret = -EIO;
		goto out;
	}

	type = btrfs_device_type(leaf, dev_item);
	io_align = btrfs_device_io_align(leaf, dev_item);
	io_width = btrfs_device_io_width(leaf, dev_item);
	sector_size = btrfs_device_sector_size(leaf, dev_item);
	total_bytes = btrfs_device_total_bytes(leaf, dev_item);
	bytes_used = btrfs_device_bytes_used(leaf, dev_item);
	read_extent_buffer(leaf, dev_uuid, (unsigned long)btrfs_device_uuid(dev_item), BTRFS_UUID_SIZE);
	read_extent_buffer(leaf, fs_uuid, (unsigned long)btrfs_device_fsid(dev_item), BTRFS_UUID_SIZE);

	btrfs_release_path(&path);

	printf("update disk super on %s devid=%llu\n", other_dev, devid);

	/* update other devices' super block */
	fp = open(other_dev, O_CREAT | O_RDWR, 0600);
	if (fp < 0) {
		error("could not open %s: %m", other_dev);
		ret = -EIO;
		goto out;
	}

	memcpy(buf, info->super_copy, BTRFS_SUPER_INFO_SIZE);

	disk_super = (struct btrfs_super_block *)buf;
	dev_item = &disk_super->dev_item;

	btrfs_set_stack_device_type(dev_item, type);
	btrfs_set_stack_device_id(dev_item, devid);
	btrfs_set_stack_device_total_bytes(dev_item, total_bytes);
	btrfs_set_stack_device_bytes_used(dev_item, bytes_used);
	btrfs_set_stack_device_io_align(dev_item, io_align);
	btrfs_set_stack_device_io_width(dev_item, io_width);
	btrfs_set_stack_device_sector_size(dev_item, sector_size);
	memcpy(dev_item->uuid, dev_uuid, BTRFS_UUID_SIZE);
	memcpy(dev_item->fsid, fs_uuid, BTRFS_UUID_SIZE);
	csum_block((u8 *)buf, BTRFS_SUPER_INFO_SIZE);

	ret = pwrite64(fp, buf, BTRFS_SUPER_INFO_SIZE, BTRFS_SUPER_INFO_OFFSET);
	if (ret != BTRFS_SUPER_INFO_SIZE) {
		if (ret < 0) {
			errno = ret;
			error("cannot write superblock: %m");
		} else {
			error("cannot write superblock");
		}
		ret = -EIO;
		goto out;
	}

	write_backup_supers(fp, (u8 *)buf);

out:
	if (fp != -1)
		close(fp);
	return ret;
}

static void print_usage(int ret)
{
	printf("usage: btrfs-image [options] source target\n");
	printf("\t-r      \trestore metadump image\n");
	printf("\t-c value\tcompression level (0 ~ 9)\n");
	printf("\t-t value\tnumber of threads (1 ~ 32)\n");
	printf("\t-o      \tdon't mess with the chunk tree when restoring\n");
	printf("\t-s      \tsanitize file names, use once to just use garbage, use twice if you want crc collisions\n");
	printf("\t-w      \twalk all trees instead of using extent tree, do this if your extent tree is broken\n");
	printf("\t-m	   \trestore for multiple devices\n");
	printf("\n");
	printf("\tIn the dump mode, source is the btrfs device and target is the output file (use '-' for stdout).\n");
	printf("\tIn the restore mode, source is the dumped image and target is the btrfs device/file.\n");
	exit(ret);
}

int BOX_MAIN(image)(int argc, char *argv[])
{
	char *source;
	char *target;
	u64 num_threads = 0;
	u64 compress_level = 0;
	int create = 1;
	int old_restore = 0;
	int walk_trees = 0;
	int multi_devices = 0;
	int ret;
	enum sanitize_mode sanitize = SANITIZE_NONE;
	int dev_cnt = 0;
	int usage_error = 0;
	FILE *out;

	while (1) {
		static const struct option long_options[] = {
			{ "help", no_argument, NULL, GETOPT_VAL_HELP},
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "rc:t:oswm", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'r':
			create = 0;
			break;
		case 't':
			num_threads = arg_strtou64(optarg);
			if (num_threads > MAX_WORKER_THREADS) {
				error("number of threads out of range: %llu > %d",
					(unsigned long long)num_threads,
					MAX_WORKER_THREADS);
				return 1;
			}
			break;
		case 'c':
			compress_level = arg_strtou64(optarg);
			if (compress_level > 9) {
				error("compression level out of range: %llu",
					(unsigned long long)compress_level);
				return 1;
			}
			break;
		case 'o':
			old_restore = 1;
			break;
		case 's':
			if (sanitize == SANITIZE_NONE)
				sanitize = SANITIZE_NAMES;
			else if (sanitize == SANITIZE_NAMES)
				sanitize = SANITIZE_COLLISIONS;
			break;
		case 'w':
			walk_trees = 1;
			break;
		case 'm':
			create = 0;
			multi_devices = 1;
			break;
		case GETOPT_VAL_HELP:
		default:
			print_usage(c != GETOPT_VAL_HELP);
		}
	}

	set_argv0(argv);
	if (check_argc_min(argc - optind, 2))
		print_usage(1);

	dev_cnt = argc - optind - 1;

	if (create) {
		if (old_restore) {
			error(
			"create and restore cannot be used at the same time");
			usage_error++;
		}
	} else {
		if (walk_trees || sanitize != SANITIZE_NONE || compress_level) {
			error(
			"using -w, -s, -c options for restore makes no sense");
			usage_error++;
		}
		if (multi_devices && dev_cnt < 2) {
			error("not enough devices specified for -m option");
			usage_error++;
		}
		if (!multi_devices && dev_cnt != 1) {
			error("accepts only 1 device without -m option");
			usage_error++;
		}
	}

	if (usage_error)
		print_usage(1);

	source = argv[optind];
	target = argv[optind + 1];

	if (create && !strcmp(target, "-")) {
		out = stdout;
	} else {
		out = fopen(target, "w+");
		if (!out) {
			error("unable to create target file %s", target);
			exit(1);
		}
	}

	if (compress_level > 0 || create == 0) {
		if (num_threads == 0) {
			long tmp = sysconf(_SC_NPROCESSORS_ONLN);

			if (tmp <= 0)
				tmp = 1;
			tmp = min_t(long, tmp, MAX_WORKER_THREADS);
			num_threads = tmp;
		}
	} else {
		num_threads = 0;
	}

	if (create) {
		ret = check_mounted(source);
		if (ret < 0) {
			errno = -ret;
			warning("unable to check mount status of: %m");
		} else if (ret) {
			warning("%s already mounted, results may be inaccurate",
					source);
		}

		ret = create_metadump(source, out, num_threads,
				      compress_level, sanitize, walk_trees);
	} else {
		ret = restore_metadump(source, out, old_restore, num_threads,
				       0, target, multi_devices);
	}
	if (ret) {
		error("%s failed: %m", (create) ? "create" : "restore");
		goto out;
	}

	 /* extended support for multiple devices */
	if (!create && multi_devices) {
		struct btrfs_fs_info *info;
		u64 total_devs;
		int i;

		info = open_ctree_fs_info(target, 0, 0, 0,
					  OPEN_CTREE_PARTIAL |
					  OPEN_CTREE_RESTORE);
		if (!info) {
			error("open ctree failed at %s", target);
			return 1;
		}

		total_devs = btrfs_super_num_devices(info->super_copy);
		if (total_devs != dev_cnt) {
			error("it needs %llu devices but has only %d",
				total_devs, dev_cnt);
			close_ctree(info->chunk_root);
			goto out;
		}

		/* update super block on other disks */
		for (i = 2; i <= dev_cnt; i++) {
			ret = update_disk_super_on_device(info,
					argv[optind + i], (u64)i);
			if (ret) {
				error("update disk superblock failed devid %d: %d",
					i, ret);
				close_ctree(info->chunk_root);
				exit(1);
			}
		}

		close_ctree(info->chunk_root);

		/* fix metadata block to map correct chunk */
		ret = restore_metadump(source, out, 0, num_threads, 1,
				       target, 1);
		if (ret) {
			error("unable to fixup metadump: %d", ret);
			exit(1);
		}
	}
out:
	if (out == stdout) {
		fflush(out);
	} else {
		fclose(out);
		if (ret && create) {
			int unlink_ret;

			unlink_ret = unlink(target);
			if (unlink_ret)
				error("unlink output file %s failed: %m",
						target);
		}
	}

	btrfs_close_all_devices();

	return !!ret;
}
