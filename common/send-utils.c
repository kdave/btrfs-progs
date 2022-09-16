/*
 * Copyright (C) 2012 Alexander Block.  All rights reserved.
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

#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel-shared/ctree.h"
#include "common/send-utils.h"
#include "common/messages.h"
#include "common/utils.h"
#include "ioctl.h"

static int btrfs_subvolid_resolve_sub(int fd, char *path, size_t *path_len,
				      u64 subvol_id);

static int btrfs_get_root_id_by_sub_path(int mnt_fd, const char *sub_path,
					 u64 *root_id)
{
	int ret;
	int subvol_fd;

	subvol_fd = openat(mnt_fd, sub_path, O_RDONLY);
	if (subvol_fd < 0) {
		ret = -errno;
		error("open %s failed: %m", sub_path);
		return ret;
	}

	ret = lookup_path_rootid(subvol_fd, root_id);
	if (ret) {
		errno = -ret;
		error("cannot resolve rootid for path: %m");
	}
	close(subvol_fd);
	return ret;
}

static int btrfs_read_root_item_raw(int mnt_fd, u64 root_id, size_t buf_len,
				    u32 *read_len, void *buf)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	unsigned long off = 0;
	int found = 0;
	int i;

	*read_len = 0;
	memset(&args, 0, sizeof(args));

	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;

	/*
	 * there may be more than one ROOT_ITEM key if there are
	 * snapshots pending deletion, we have to loop through
	 * them.
	 */
	sk->min_objectid = root_id;
	sk->max_objectid = root_id;
	sk->max_type = BTRFS_ROOT_ITEM_KEY;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	while (1) {
		ret = ioctl(mnt_fd, BTRFS_IOC_TREE_SEARCH, &args);
		if (ret < 0) {
			error("can't perform the search: %m");
			return 0;
		}
		/* the ioctl returns the number of item it found in nr_items */
		if (sk->nr_items == 0)
			break;

		off = 0;
		for (i = 0; i < sk->nr_items; i++) {
			struct btrfs_root_item *item;
			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);

			off += sizeof(*sh);
			item = (struct btrfs_root_item *)(args.buf + off);
			off += btrfs_search_header_len(sh);

			sk->min_objectid = btrfs_search_header_objectid(sh);
			sk->min_type = btrfs_search_header_type(sh);
			sk->min_offset = btrfs_search_header_offset(sh);

			if (btrfs_search_header_objectid(sh) > root_id)
				break;

			if (btrfs_search_header_objectid(sh) == root_id &&
			    btrfs_search_header_type(sh) == BTRFS_ROOT_ITEM_KEY) {
				if (btrfs_search_header_len(sh) > buf_len) {
					/* btrfs-progs is too old for kernel */
					error(
			"buf for read_root_item_raw() is too small, get newer btrfs tools");
					return -EOVERFLOW;
				}
				memcpy(buf, item, btrfs_search_header_len(sh));
				*read_len = btrfs_search_header_len(sh);
				found = 1;
			}
		}
		if (sk->min_offset < (u64)-1)
			sk->min_offset++;
		else
			break;

		if (sk->min_type != BTRFS_ROOT_ITEM_KEY ||
		    sk->min_objectid != root_id)
			break;
	}

	return found ? 0 : -ENOENT;
}

/*
 * Read a root item from the tree. In case we detect a root item smaller then
 * sizeof(root_item), we know it's an old version of the root structure and
 * initialize all new fields to zero. The same happens if we detect mismatching
 * generation numbers as then we know the root was once mounted with an older
 * kernel that was not aware of the root item structure change.
 */
static int btrfs_read_root_item(int mnt_fd, u64 root_id,
				struct btrfs_root_item *item)
{
	int ret;
	u32 read_len;

	ret = btrfs_read_root_item_raw(mnt_fd, root_id, sizeof(*item),
				       &read_len, item);
	if (ret)
		return ret;

	if (read_len < sizeof(*item) ||
	    btrfs_root_generation(item) != btrfs_root_generation_v2(item)) {
		/*
		 * Workaround for gcc9 that warns that memset over
		 * generation_v2 overflows, which is what we want but would
		 * be otherwise a bug
		 *
		 * The below is &item->generation_v2
		 */
		char *start = (char *)item + offsetof(struct btrfs_root_item,
						      generation_v2);

		memset(start, 0,
			sizeof(*item) - offsetof(struct btrfs_root_item,
						 generation_v2));
	}

	return 0;
}

int btrfs_subvolid_resolve(int fd, char *path, size_t path_len, u64 subvol_id)
{
	if (path_len < 1)
		return -EOVERFLOW;
	path[0] = '\0';
	path_len--;
	path[path_len] = '\0';
	return btrfs_subvolid_resolve_sub(fd, path, &path_len, subvol_id);
}

static int btrfs_subvolid_resolve_sub(int fd, char *path, size_t *path_len,
				      u64 subvol_id)
{
	int ret;
	struct btrfs_ioctl_search_args search_arg;
	struct btrfs_ioctl_ino_lookup_args ino_lookup_arg;
	struct btrfs_ioctl_search_header *search_header;
	struct btrfs_root_ref *backref_item;

	if (subvol_id == BTRFS_FS_TREE_OBJECTID) {
		if (*path_len < 1)
			return -EOVERFLOW;
		*path = '\0';
		(*path_len)--;
		return 0;
	}

	memset(&search_arg, 0, sizeof(search_arg));
	search_arg.key.tree_id = BTRFS_ROOT_TREE_OBJECTID;
	search_arg.key.min_objectid = subvol_id;
	search_arg.key.max_objectid = subvol_id;
	search_arg.key.min_type = BTRFS_ROOT_BACKREF_KEY;
	search_arg.key.max_type = BTRFS_ROOT_BACKREF_KEY;
	search_arg.key.max_offset = (u64)-1;
	search_arg.key.max_transid = (u64)-1;
	search_arg.key.nr_items = 1;
	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &search_arg);
	if (ret < 0) {
		fprintf(stderr,
			"ioctl(BTRFS_IOC_TREE_SEARCH, subvol_id %llu) ret=%d, error: %m\n",
			(unsigned long long)subvol_id, ret);
		return ret;
	}

	if (search_arg.key.nr_items < 1) {
		fprintf(stderr,
			"failed to lookup subvol_id %llu!\n",
			(unsigned long long)subvol_id);
		return -ENOENT;
	}
	search_header = (struct btrfs_ioctl_search_header *)search_arg.buf;
	backref_item = (struct btrfs_root_ref *)(search_header + 1);
	if (btrfs_search_header_offset(search_header)
	    != BTRFS_FS_TREE_OBJECTID) {
		int sub_ret;

		sub_ret = btrfs_subvolid_resolve_sub(fd, path, path_len,
				btrfs_search_header_offset(search_header));
		if (sub_ret)
			return sub_ret;
		if (*path_len < 1)
			return -EOVERFLOW;
		strcat(path, "/");
		(*path_len)--;
	}

	if (btrfs_stack_root_ref_dirid(backref_item) !=
	    BTRFS_FIRST_FREE_OBJECTID) {
		int len;

		memset(&ino_lookup_arg, 0, sizeof(ino_lookup_arg));
		ino_lookup_arg.treeid =
			btrfs_search_header_offset(search_header);
		ino_lookup_arg.objectid =
			btrfs_stack_root_ref_dirid(backref_item);
		ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &ino_lookup_arg);
		if (ret < 0) {
			fprintf(stderr,
				"ioctl(BTRFS_IOC_INO_LOOKUP) ret=%d, error: %m\n",
				ret);
			return ret;
		}

		len = strlen(ino_lookup_arg.name);
		if (*path_len < len)
			return -EOVERFLOW;
		strcat(path, ino_lookup_arg.name);
		(*path_len) -= len;
	}

	if (*path_len < btrfs_stack_root_ref_name_len(backref_item))
		return -EOVERFLOW;
	strncat(path, (char *)(backref_item + 1),
		btrfs_stack_root_ref_name_len(backref_item));
	(*path_len) -= btrfs_stack_root_ref_name_len(backref_item);
	return 0;
}

struct subvol_info *subvol_uuid_search(int mnt_fd,
				       u64 root_id, const u8 *uuid, u64 transid,
				       const char *path,
				       enum subvol_search_type type)
{
	int ret = 0;
	struct btrfs_root_item root_item;
	struct subvol_info *info = NULL;

	switch (type) {
	case subvol_search_by_received_uuid:
		ret = btrfs_lookup_uuid_received_subvol_item(mnt_fd, uuid,
							     &root_id);
		break;
	case subvol_search_by_uuid:
		ret = btrfs_lookup_uuid_subvol_item(mnt_fd, uuid, &root_id);
		break;
	case subvol_search_by_root_id:
		break;
	case subvol_search_by_path:
		ret = btrfs_get_root_id_by_sub_path(mnt_fd, path, &root_id);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		goto out;

	ret = btrfs_read_root_item(mnt_fd, root_id, &root_item);
	if (ret)
		goto out;

	info = calloc(1, sizeof(*info));
	if (!info) {
		ret = -ENOMEM;
		goto out;
	}
	info->root_id = root_id;
	memcpy(info->uuid, root_item.uuid, BTRFS_UUID_SIZE);
	memcpy(info->received_uuid, root_item.received_uuid, BTRFS_UUID_SIZE);
	memcpy(info->parent_uuid, root_item.parent_uuid, BTRFS_UUID_SIZE);
	info->ctransid = btrfs_root_ctransid(&root_item);
	info->otransid = btrfs_root_otransid(&root_item);
	info->stransid = btrfs_root_stransid(&root_item);
	info->rtransid = btrfs_root_rtransid(&root_item);
	if (type == subvol_search_by_path) {
		info->path = strdup(path);
		if (!info->path) {
			ret = -ENOMEM;
			goto out;
		}
	} else {
		info->path = malloc(PATH_MAX);
		if (!info->path) {
			ret = -ENOMEM;
			goto out;
		}
		ret = btrfs_subvolid_resolve(mnt_fd, info->path,
					     PATH_MAX, root_id);
	}

out:
	if (ret) {
		if (info) {
			free(info->path);
			free(info);
		}
		return ERR_PTR(ret);
	}

	return info;
}
