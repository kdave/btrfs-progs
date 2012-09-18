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

#include "ctree.h"
#include "send-utils.h"
#include "ioctl.h"
#include "btrfs-list.h"

static struct rb_node *tree_insert(struct rb_root *root,
				   struct subvol_info *si,
				   enum subvol_search_type type)
{
	struct rb_node ** p = &root->rb_node;
	struct rb_node * parent = NULL;
	struct subvol_info *entry;
	__s64 comp;

	while(*p) {
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

static struct subvol_info *tree_search(struct rb_root *root,
				       u64 root_id, const u8 *uuid,
				       u64 stransid, const char *path,
				       enum subvol_search_type type)
{
	struct rb_node * n = root->rb_node;
	struct subvol_info *entry;
	__s64 comp;

	while(n) {
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
			entry = rb_entry(n, struct subvol_info, rb_root_id_node);
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

static int count_bytes(void *buf, int len, char b)
{
	int cnt = 0;
	int i;
	for (i = 0; i < len; i++) {
		if (((char*)buf)[i] == b)
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

struct subvol_info *subvol_uuid_search(struct subvol_uuid_search *s,
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

int subvol_uuid_search_init(int mnt_fd, struct subvol_uuid_search *s)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	struct btrfs_root_item *root_item_ptr;
	struct btrfs_root_item root_item;
	struct subvol_info *si = NULL;
	int root_item_valid = 0;
	unsigned long off = 0;
	int i;
	int e;
	char *path;

	memset(&args, 0, sizeof(args));

	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;

	sk->max_objectid = (u64)-1;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_type = BTRFS_ROOT_BACKREF_KEY;
	sk->nr_items = 4096;

	while(1) {
		ret = ioctl(mnt_fd, BTRFS_IOC_TREE_SEARCH, &args);
		e = errno;
		if (ret < 0) {
			fprintf(stderr, "ERROR: can't perform the search- %s\n",
				strerror(e));
			return ret;
		}
		if (sk->nr_items == 0)
			break;

		off = 0;

		for (i = 0; i < sk->nr_items; i++) {
			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);
			off += sizeof(*sh);

			if ((sh->objectid != 5 &&
			    sh->objectid < BTRFS_FIRST_FREE_OBJECTID) ||
			    sh->objectid == BTRFS_FREE_INO_OBJECTID)
				goto skip;

			if (sh->type == BTRFS_ROOT_ITEM_KEY) {
				/* older kernels don't have uuids+times */
				if (sh->len < sizeof(root_item)) {
					root_item_valid = 0;
					goto skip;
				}
				root_item_ptr = (struct btrfs_root_item *)
						(args.buf + off);
				memcpy(&root_item, root_item_ptr,
						sizeof(root_item));
				root_item_valid = 1;
			} else if (sh->type == BTRFS_ROOT_BACKREF_KEY) {
				if (!root_item_valid)
					goto skip;

				path = btrfs_list_path_for_root(mnt_fd,
								sh->objectid);
				if (!path)
					path = strdup("");
				if (IS_ERR(path)) {
					ret = PTR_ERR(path);
					fprintf(stderr, "ERROR: unable to "
							"resolve path "
							"for root %llu\n",
							sh->objectid);
					goto out;
				}

				si = calloc(1, sizeof(*si));
				si->root_id = sh->objectid;
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
				root_item_valid = 0;
				goto skip;
			}

skip:
			off += sh->len;

			/*
			 * record the mins in sk so we can make sure the
			 * next search doesn't repeat this root
			 */
			sk->min_objectid = sh->objectid;
			sk->min_offset = sh->offset;
			sk->min_type = sh->type;
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


char *path_cat(const char *p1, const char *p2)
{
	int p1_len = strlen(p1);
	int p2_len = strlen(p2);
	char *new = malloc(p1_len + p2_len + 3);

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
	char *new = malloc(p1_len + p2_len + p3_len + 4);

	if (p1_len && p1[p1_len - 1] == '/')
		p1_len--;
	if (p2_len && p2[p2_len - 1] == '/')
		p2_len--;
	if (p3_len && p3[p3_len - 1] == '/')
		p3_len--;
	sprintf(new, "%.*s/%.*s/%.*s", p1_len, p1, p2_len, p2, p3_len, p3);
	return new;
}

