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

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <uuid/uuid.h>

#include "ctree.h"
#include "send-utils.h"
#include "ioctl.h"
#include "btrfs-list.h"

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
		fprintf(stderr, "ERROR: open %s failed. %s\n", sub_path,
			strerror(-ret));
		return ret;
	}

	ret = btrfs_list_get_path_rootid(subvol_fd, root_id);
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
			fprintf(stderr,
				"ERROR: can't perform the search - %s\n",
				strerror(errno));
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
			off += sh->len;

			sk->min_objectid = sh->objectid;
			sk->min_type = sh->type;
			sk->min_offset = sh->offset;

			if (sh->objectid > root_id)
				break;

			if (sh->objectid == root_id &&
			    sh->type == BTRFS_ROOT_ITEM_KEY) {
				if (sh->len > buf_len) {
					/* btrfs-progs is too old for kernel */
					fprintf(stderr,
						"ERROR: buf for read_root_item_raw() is too small, get newer btrfs tools!\n");
					return -EOVERFLOW;
				}
				memcpy(buf, item, sh->len);
				*read_len = sh->len;
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
	    btrfs_root_generation(item) != btrfs_root_generation_v2(item))
		memset(&item->generation_v2, 0,
			sizeof(*item) - offsetof(struct btrfs_root_item,
						 generation_v2));

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
	if (ret) {
		fprintf(stderr,
			"ioctl(BTRFS_IOC_TREE_SEARCH, subvol_id %llu) ret=%d, error: %s\n",
			(unsigned long long)subvol_id, ret, strerror(errno));
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
	if (search_header->offset != BTRFS_FS_TREE_OBJECTID) {
		int sub_ret;

		sub_ret = btrfs_subvolid_resolve_sub(fd, path, path_len,
						     search_header->offset);
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
		ino_lookup_arg.treeid = search_header->offset;
		ino_lookup_arg.objectid =
			btrfs_stack_root_ref_dirid(backref_item);
		ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &ino_lookup_arg);
		if (ret) {
			fprintf(stderr,
				"ioctl(BTRFS_IOC_INO_LOOKUP) ret=%d, error: %s\n",
				ret, strerror(errno));
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

void subvol_uuid_search_add(struct subvol_uuid_search *s,
			    struct subvol_info *si)
{
	if (si) {
		free(si->path);
		free(si);
	}
}

struct subvol_info *subvol_uuid_search(struct subvol_uuid_search *s,
				       u64 root_id, const u8 *uuid, u64 transid,
				       const char *path,
				       enum subvol_search_type type)
{
	int ret = 0;
	struct btrfs_root_item root_item;
	struct subvol_info *info = NULL;

	switch (type) {
	case subvol_search_by_received_uuid:
		ret = btrfs_lookup_uuid_received_subvol_item(s->mnt_fd, uuid,
							     &root_id);
		break;
	case subvol_search_by_uuid:
		ret = btrfs_lookup_uuid_subvol_item(s->mnt_fd, uuid, &root_id);
		break;
	case subvol_search_by_root_id:
		break;
	case subvol_search_by_path:
		ret = btrfs_get_root_id_by_sub_path(s->mnt_fd, path, &root_id);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		goto out;

	ret = btrfs_read_root_item(s->mnt_fd, root_id, &root_item);
	if (ret)
		goto out;

	info = calloc(1, sizeof(*info));
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
	} else {
		info->path = malloc(BTRFS_PATH_NAME_MAX);
		ret = btrfs_subvolid_resolve(s->mnt_fd, info->path,
					     BTRFS_PATH_NAME_MAX, root_id);
	}

out:
	if (ret && info) {
		free(info->path);
		free(info);
		info = NULL;
	}

	return info;
}

int subvol_uuid_search_init(int mnt_fd, struct subvol_uuid_search *s)
{
	s->mnt_fd = mnt_fd;

	return 0;
}

void subvol_uuid_search_finit(struct subvol_uuid_search *s)
{
}

char *path_cat(const char *p1, const char *p2)
{
	int p1_len = strlen(p1);
	int p2_len = strlen(p2);
	char *new = malloc(p1_len + p2_len + 2);

	if (p1_len && p1[p1_len - 1] == '/')
		p1_len--;
	if (p2_len && p2[p2_len - 1] == '/')
		p2_len--;
	sprintf(new, "%.*s/%.*s", p1_len, p1, p2_len, p2);
	return new;
}

char *path_cat3(const char *p1, const char *p2, const char *p3)
{
	int p1_len = strlen(p1);
	int p2_len = strlen(p2);
	int p3_len = strlen(p3);
	char *new = malloc(p1_len + p2_len + p3_len + 3);

	if (p1_len && p1[p1_len - 1] == '/')
		p1_len--;
	if (p2_len && p2[p2_len - 1] == '/')
		p2_len--;
	if (p3_len && p3[p3_len - 1] == '/')
		p3_len--;
	sprintf(new, "%.*s/%.*s/%.*s", p1_len, p1, p2_len, p2, p3_len, p3);
	return new;
}
