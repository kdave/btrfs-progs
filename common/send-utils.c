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
#include <limits.h>
#include <errno.h>

#include "kernel-shared/ctree.h"
#include "common/send-utils.h"
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
		fprintf(stderr, "ERROR: open %s failed: %m\n", sub_path);
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
				"ERROR: can't perform the search - %m\n");
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
					fprintf(stderr,
						"ERROR: buf for read_root_item_raw() is too small, get newer btrfs tools!\n");
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

#ifdef BTRFS_COMPAT_SEND_NO_UUID_TREE
static struct rb_node *tree_insert(struct rb_root *root,
				   struct subvol_info *si,
				   enum subvol_search_type type)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct subvol_info *entry;
	__s64 comp;

	while (*p) {
		parent = *p;
		if (type == subvol_search_by_received_uuid) {
			entry = rb_entry(parent, struct subvol_info,
					rb_received_node);

			comp = memcmp(entry->received_uuid, si->received_uuid,
					BTRFS_UUID_SIZE);
			if (!comp) {
				if (entry->stransid < si->stransid)
					comp = -1;
				else if (entry->stransid > si->stransid)
					comp = 1;
				else
					comp = 0;
			}
		} else if (type == subvol_search_by_uuid) {
			entry = rb_entry(parent, struct subvol_info,
					rb_local_node);
			comp = memcmp(entry->uuid, si->uuid, BTRFS_UUID_SIZE);
		} else if (type == subvol_search_by_root_id) {
			entry = rb_entry(parent, struct subvol_info,
					rb_root_id_node);
			comp = entry->root_id - si->root_id;
		} else if (type == subvol_search_by_path) {
			entry = rb_entry(parent, struct subvol_info,
					rb_path_node);
			comp = strcmp(entry->path, si->path);
		} else {
			BUG();
		}

		if (comp < 0)
			p = &(*p)->rb_left;
		else if (comp > 0)
			p = &(*p)->rb_right;
		else
			return parent;
	}

	if (type == subvol_search_by_received_uuid) {
		rb_link_node(&si->rb_received_node, parent, p);
		rb_insert_color(&si->rb_received_node, root);
	} else if (type == subvol_search_by_uuid) {
		rb_link_node(&si->rb_local_node, parent, p);
		rb_insert_color(&si->rb_local_node, root);
	} else if (type == subvol_search_by_root_id) {
		rb_link_node(&si->rb_root_id_node, parent, p);
		rb_insert_color(&si->rb_root_id_node, root);
	} else if (type == subvol_search_by_path) {
		rb_link_node(&si->rb_path_node, parent, p);
		rb_insert_color(&si->rb_path_node, root);
	}
	return NULL;
}
#endif

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

#ifdef BTRFS_COMPAT_SEND_NO_UUID_TREE
static int count_bytes(void *buf, int len, char b)
{
	int cnt = 0;
	int i;

	for (i = 0; i < len; i++) {
		if (((char *)buf)[i] == b)
			cnt++;
	}
	return cnt;
}

void subvol_uuid_search_add(struct subvol_uuid_search *s,
			    struct subvol_info *si)
{
	int cnt;

	tree_insert(&s->root_id_subvols, si, subvol_search_by_root_id);
	tree_insert(&s->path_subvols, si, subvol_search_by_path);

	cnt = count_bytes(si->uuid, BTRFS_UUID_SIZE, 0);
	if (cnt != BTRFS_UUID_SIZE)
		tree_insert(&s->local_subvols, si, subvol_search_by_uuid);
	cnt = count_bytes(si->received_uuid, BTRFS_UUID_SIZE, 0);
	if (cnt != BTRFS_UUID_SIZE)
		tree_insert(&s->received_subvols, si,
				subvol_search_by_received_uuid);
}

static struct subvol_info *tree_search(struct rb_root *root,
				       u64 root_id, const u8 *uuid,
				       u64 stransid, const char *path,
				       enum subvol_search_type type)
{
	struct rb_node *n = root->rb_node;
	struct subvol_info *entry;
	__s64 comp;

	while (n) {
		if (type == subvol_search_by_received_uuid) {
			entry = rb_entry(n, struct subvol_info,
					rb_received_node);
			comp = memcmp(entry->received_uuid, uuid,
					BTRFS_UUID_SIZE);
			if (!comp) {
				if (entry->stransid < stransid)
					comp = -1;
				else if (entry->stransid > stransid)
					comp = 1;
				else
					comp = 0;
			}
		} else if (type == subvol_search_by_uuid) {
			entry = rb_entry(n, struct subvol_info, rb_local_node);
			comp = memcmp(entry->uuid, uuid, BTRFS_UUID_SIZE);
		} else if (type == subvol_search_by_root_id) {
			entry = rb_entry(n, struct subvol_info,
					 rb_root_id_node);
			comp = entry->root_id - root_id;
		} else if (type == subvol_search_by_path) {
			entry = rb_entry(n, struct subvol_info, rb_path_node);
			comp = strcmp(entry->path, path);
		} else {
			BUG();
		}
		if (comp < 0)
			n = n->rb_left;
		else if (comp > 0)
			n = n->rb_right;
		else
			return entry;
	}
	return NULL;
}

/*
 * this function will be only called if kernel doesn't support uuid tree.
 */
static struct subvol_info *subvol_uuid_search_old(struct subvol_uuid_search *s,
				       u64 root_id, const u8 *uuid, u64 transid,
				       const char *path,
				       enum subvol_search_type type)
{
	struct rb_root *root;
	if (type == subvol_search_by_received_uuid)
		root = &s->received_subvols;
	else if (type == subvol_search_by_uuid)
		root = &s->local_subvols;
	else if (type == subvol_search_by_root_id)
		root = &s->root_id_subvols;
	else if (type == subvol_search_by_path)
		root = &s->path_subvols;
	else
		return NULL;
	return tree_search(root, root_id, uuid, transid, path, type);
}
#else
void subvol_uuid_search_add(struct subvol_uuid_search *s,
			    struct subvol_info *si)
{
	if (si) {
		free(si->path);
		free(si);
	}
}
#endif

struct subvol_info *subvol_uuid_search(struct subvol_uuid_search *s,
				       u64 root_id, const u8 *uuid, u64 transid,
				       const char *path,
				       enum subvol_search_type type)
{
	struct subvol_info *si;

	si = subvol_uuid_search2(s, root_id, uuid, transid, path, type);
	if (IS_ERR(si))
		return NULL;
	return si;
}

struct subvol_info *subvol_uuid_search2(struct subvol_uuid_search *s,
				       u64 root_id, const u8 *uuid, u64 transid,
				       const char *path,
				       enum subvol_search_type type)
{
	int ret = 0;
	struct btrfs_root_item root_item;
	struct subvol_info *info = NULL;

#ifdef BTRFS_COMPAT_SEND_NO_UUID_TREE
	if (!s->uuid_tree_existed)
		return subvol_uuid_search_old(s, root_id, uuid, transid,
					     path, type);
#endif
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
		ret = btrfs_subvolid_resolve(s->mnt_fd, info->path,
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

#ifdef BTRFS_COMPAT_SEND_NO_UUID_TREE
static int is_uuid_tree_supported(int fd)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;

	memset(&args, 0, sizeof(args));

	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;

	sk->min_objectid = BTRFS_UUID_TREE_OBJECTID;
	sk->max_objectid = BTRFS_UUID_TREE_OBJECTID;
	sk->max_type = BTRFS_ROOT_ITEM_KEY;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
	if (ret < 0)
		return ret;

	/* the ioctl returns the number of item it found in nr_items */
	if (sk->nr_items == 0)
		return 0;

	return 1;
}

/*
 * this function is mainly used to read all root items
 * it will be only used when we use older kernel which uuid
 * tree is not supported yet
 */
int subvol_uuid_search_init(int mnt_fd, struct subvol_uuid_search *s)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	struct btrfs_root_item *root_item_ptr;
	struct btrfs_root_item root_item = {};
	struct subvol_info *si = NULL;
	int root_item_valid = 0;
	unsigned long off = 0;
	int i;
	char *path;

	s->mnt_fd = mnt_fd;

	s->root_id_subvols = RB_ROOT;
	s->local_subvols = RB_ROOT;
	s->received_subvols = RB_ROOT;
	s->path_subvols = RB_ROOT;

	ret = is_uuid_tree_supported(mnt_fd);
	if (ret < 0) {
		fprintf(stderr,
			"ERROR: check if we support uuid tree fails - %m\n");
		return ret;
	} else if (ret) {
		/* uuid tree is supported */
		s->uuid_tree_existed = 1;
		return 0;
	}
	memset(&args, 0, sizeof(args));

	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;

	sk->max_objectid = (u64)-1;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_type = BTRFS_ROOT_BACKREF_KEY;
	sk->nr_items = 4096;

	while (1) {
		ret = ioctl(mnt_fd, BTRFS_IOC_TREE_SEARCH, &args);
		if (ret < 0) {
			fprintf(stderr, "ERROR: can't perform the search - %m\n");
			return ret;
		}
		if (sk->nr_items == 0)
			break;

		off = 0;

		for (i = 0; i < sk->nr_items; i++) {
			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);
			off += sizeof(*sh);

			if ((btrfs_search_header_objectid(sh) != 5 &&
			     btrfs_search_header_objectid(sh)
			     < BTRFS_FIRST_FREE_OBJECTID) ||
			    btrfs_search_header_objectid(sh)
			    > BTRFS_LAST_FREE_OBJECTID) {
				goto skip;
			}

			if (btrfs_search_header_type(sh)
			    == BTRFS_ROOT_ITEM_KEY) {
				/* older kernels don't have uuids+times */
				if (btrfs_search_header_len(sh)
				    < sizeof(root_item)) {
					root_item_valid = 0;
					goto skip;
				}
				root_item_ptr = (struct btrfs_root_item *)
						(args.buf + off);
				memcpy(&root_item, root_item_ptr,
						sizeof(root_item));
				root_item_valid = 1;
			} else if (btrfs_search_header_type(sh)
				   == BTRFS_ROOT_BACKREF_KEY ||
				   root_item_valid) {
				if (!root_item_valid)
					goto skip;

				path = btrfs_list_path_for_root(mnt_fd,
					btrfs_search_header_objectid(sh));
				if (!path)
					path = strdup("");
				if (IS_ERR(path)) {
					ret = PTR_ERR(path);
					fprintf(stderr, "ERROR: unable to "
							"resolve path "
							"for root %llu\n",
						btrfs_search_header_objectid(sh));
					goto out;
				}

				si = calloc(1, sizeof(*si));
				si->root_id = btrfs_search_header_objectid(sh);
				memcpy(si->uuid, root_item.uuid,
						BTRFS_UUID_SIZE);
				memcpy(si->parent_uuid, root_item.parent_uuid,
						BTRFS_UUID_SIZE);
				memcpy(si->received_uuid,
						root_item.received_uuid,
						BTRFS_UUID_SIZE);
				si->ctransid = btrfs_root_ctransid(&root_item);
				si->otransid = btrfs_root_otransid(&root_item);
				si->stransid = btrfs_root_stransid(&root_item);
				si->rtransid = btrfs_root_rtransid(&root_item);
				si->path = path;
				subvol_uuid_search_add(s, si);
				root_item_valid = 0;
			} else {
				goto skip;
			}

skip:
			off += btrfs_search_header_len(sh);

			/*
			 * record the mins in sk so we can make sure the
			 * next search doesn't repeat this root
			 */
			sk->min_objectid = btrfs_search_header_objectid(sh);
			sk->min_offset = btrfs_search_header_offset(sh);
			sk->min_type = btrfs_search_header_type(sh);
		}
		sk->nr_items = 4096;
		if (sk->min_offset < (u64)-1)
			sk->min_offset++;
		else if (sk->min_objectid < (u64)-1) {
			sk->min_objectid++;
			sk->min_offset = 0;
			sk->min_type = 0;
		} else
			break;
	}

out:
	return ret;
}

void subvol_uuid_search_finit(struct subvol_uuid_search *s)
{
	struct rb_root *root = &s->root_id_subvols;
	struct rb_node *node;

	if (!s->uuid_tree_existed)
		return;

	while ((node = rb_first(root))) {
		struct subvol_info *entry =
			rb_entry(node, struct subvol_info, rb_root_id_node);

		free(entry->path);
		rb_erase(node, root);
		free(entry);
	}

	s->root_id_subvols = RB_ROOT;
	s->local_subvols = RB_ROOT;
	s->received_subvols = RB_ROOT;
	s->path_subvols = RB_ROOT;
}
#else
int subvol_uuid_search_init(int mnt_fd, struct subvol_uuid_search *s)
{
	s->mnt_fd = mnt_fd;

	return 0;
}

void subvol_uuid_search_finit(struct subvol_uuid_search *s)
{
}
#endif
