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
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <zlib.h>
#include "kernel-lib/list.h"
#include "kernel-lib/rbtree.h"
#include "kernel-lib/rbtree_types.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/extent-io-tree.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/file-item.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/tree-checker.h"
#include "common/internal.h"
#include "common/messages.h"
#include "image/sanitize.h"
#include "image/metadump.h"
#include "image/common.h"

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
				error_mem("async buffer");
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
	header->magic = cpu_to_le64(current_version->magic_cpu);
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
	extent_io_tree_release(&md->seen);
}

static int metadump_init(struct metadump_struct *md, struct btrfs_root *root,
			 FILE *out, int num_threads, int compress_level,
			 bool dump_data, enum sanitize_mode sanitize_names)
{
	int i, ret = 0;

	/* We need larger item/cluster limit for data extents */
	if (dump_data)
		current_version = &dump_versions[1];

	memset(md, 0, sizeof(*md));
	INIT_LIST_HEAD(&md->list);
	INIT_LIST_HEAD(&md->ordered);
	extent_io_tree_init(NULL, &md->seen, 0);
	md->root = root;
	md->out = out;
	md->pending_start = (u64)-1;
	md->compress_level = compress_level;
	md->sanitize_names = sanitize_names;
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
			ret = read_data_from_disk(fs_info,
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

static int write_zero(FILE *out, size_t size)
{
	static char zero[IMAGE_BLOCK_SIZE];
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

	ret = fwrite(&md->cluster, IMAGE_BLOCK_SIZE, 1, md->out);
	if (ret != 1) {
		error("unable to write out cluster: %m");
		return -errno;
	}

	/* write buffers */
	bytenr += get_unaligned_le64(&header->bytenr) + IMAGE_BLOCK_SIZE;
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
	if (!err && bytenr & IMAGE_BLOCK_MASK) {
		size_t size = IMAGE_BLOCK_SIZE - (bytenr & IMAGE_BLOCK_MASK);

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

static bool has_name(struct btrfs_key *key)
{
	switch (key->type) {
	case BTRFS_DIR_ITEM_KEY:
	case BTRFS_DIR_INDEX_KEY:
	case BTRFS_INODE_REF_KEY:
	case BTRFS_INODE_EXTREF_KEY:
	case BTRFS_XATTR_ITEM_KEY:
		return true;
	default:
		break;
	}

	return false;
}

/*
 * zero inline extents and csum items
 */
static void zero_items(struct metadump_struct *md, u8 *dst,
		       struct extent_buffer *src)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_key key;
	u32 nritems = btrfs_header_nritems(src);
	size_t size;
	unsigned long ptr;
	int i, extent_type;

	for (i = 0; i < nritems; i++) {
		btrfs_item_key_to_cpu(src, &key, i);
		if (key.type == BTRFS_CSUM_ITEM_KEY) {
			size = btrfs_item_size(src, i);
			memset(dst + btrfs_item_nr_offset(src, 0) +
			       btrfs_item_offset(src, i), 0, size);
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
		size = btrfs_file_extent_inline_item_len(src, i);
		memset(dst + ptr, 0, size);
	}
}

/*
 * copy buffer and zero useless data in the buffer
 */
static void copy_buffer(struct metadump_struct *md, u8 *dst, struct extent_buffer *src)
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
		size = btrfs_item_nr_offset(src, 0) +
			btrfs_item_offset(src, nritems - 1) -
			btrfs_item_nr_offset(src, nritems);
		memset(dst + btrfs_item_nr_offset(src, nritems), 0, size);
		zero_items(md, dst, src);
	} else {
		size = offsetof(struct btrfs_node, ptrs) +
			sizeof(struct btrfs_key_ptr) * nritems;
		memset(dst + size, 0, src->len - size);
	}
	csum_block(dst, src->len);
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

			ret = pread(fd, async->buffer, size, start);
			if (ret < size) {
				free(async->buffer);
				free(async);
				error("unable to read superblock at %llu: %m", start);
				return -errno;
			}
			size = 0;
			ret = 0;
		}

		while (!md->data && size > 0) {
			struct btrfs_tree_parent_check check = { 0 };
			u64 this_read = min((u64)md->root->fs_info->nodesize,
					size);

			eb = read_tree_block(md->root->fs_info, start, &check);
			if (!extent_buffer_uptodate(eb)) {
				free(async->buffer);
				free(async);
				error("unable to read metadata block %llu", start);
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
	    md->pending_size + size > current_version->max_pending_size ||
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
	struct btrfs_tree_parent_check check = { 0 };
	u64 bytenr;
	int level;
	int nritems = 0;
	int i = 0;
	int ret;

	bytenr = btrfs_header_bytenr(eb);
	if (test_range_bit(&metadump->seen, bytenr,
			   bytenr + fs_info->nodesize - 1, EXTENT_DIRTY, 1,
			   NULL))
		return 0;

	set_extent_dirty(&metadump->seen, bytenr,
			 bytenr + fs_info->nodesize - 1, GFP_NOFS);

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
			tmp = read_tree_block(fs_info, bytenr, &check);
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
			tmp = read_tree_block(fs_info, bytenr, &check);
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
				 struct btrfs_path *path, bool dump_data)
{
	struct btrfs_root *extent_root;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;
	u64 bytenr;
	u64 num_bytes;
	int ret;

	extent_root = btrfs_extent_root(metadump->root->fs_info, 0);
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
					bytenr, key.type);
			ret = -EIO;
			break;
		}

		if (btrfs_item_size(leaf, path->slots[0]) >= sizeof(*ei)) {
			ei = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_extent_item);
			if (btrfs_extent_flags(leaf, ei) &
			    BTRFS_EXTENT_FLAG_TREE_BLOCK ||
			    (dump_data && (btrfs_extent_flags(leaf, ei) &
					   BTRFS_EXTENT_FLAG_DATA))) {
				bool is_data;

				is_data = btrfs_extent_flags(leaf, ei) &
					  BTRFS_EXTENT_FLAG_DATA;
				ret = add_extent(bytenr, num_bytes, metadump,
						 is_data);
				if (ret) {
					error("unable to add block %llu: %d",
						bytenr, ret);
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

int create_metadump(const char *input, FILE *out, int num_threads,
		    int compress_level, enum sanitize_mode sanitize,
		    int walk_trees, bool dump_data)
{
	struct btrfs_root *root;
	struct btrfs_path path = { 0 };
	struct metadump_struct metadump;
	int ret;
	int err = 0;

	root = open_ctree(input, 0, OPEN_CTREE_ALLOW_TRANSID_MISMATCH |
			  OPEN_CTREE_SKIP_LEAF_ITEM_CHECKS);
	if (!root) {
		error("open ctree failed");
		return -EIO;
	}

	ret = metadump_init(&metadump, root, out, num_threads,
			    compress_level, dump_data, sanitize);
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
		ret = copy_from_extent_tree(&metadump, &path, dump_data);
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

