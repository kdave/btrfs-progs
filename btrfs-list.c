/*
 * Copyright (C) 2010 Oracle.  All rights reserved.
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
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include "ctree.h"
#include "transaction.h"
#include "common/utils.h"
#include "ioctl.h"
#include <uuid/uuid.h>
#include "btrfs-list.h"
#include "common/rbtree-utils.h"

#define BTRFS_LIST_NFILTERS_INCREASE	(2 * BTRFS_LIST_FILTER_MAX)
#define BTRFS_LIST_NCOMPS_INCREASE	(2 * BTRFS_LIST_COMP_MAX)

/* we store all the roots we find in an rbtree so that we can
 * search for them later.
 */
struct root_lookup {
	struct rb_root root;
};

static inline struct root_info *to_root_info(struct rb_node *node)
{
	return rb_entry(node, struct root_info, rb_node);
}

static inline struct root_info *to_root_info_sorted(struct rb_node *node)
{
	return rb_entry(node, struct root_info, sort_node);
}

static struct {
	char	*name;
	char	*column_name;
	int	need_print;
} btrfs_list_columns[] = {
	{
		.name		= "ID",
		.column_name	= "ID",
		.need_print	= 0,
	},
	{
		.name		= "gen",
		.column_name	= "Gen",
		.need_print	= 0,
	},
	{
		.name		= "cgen",
		.column_name	= "CGen",
		.need_print	= 0,
	},
	{
		.name		= "parent",
		.column_name	= "Parent",
		.need_print	= 0,
	},
	{
		.name		= "top level",
		.column_name	= "Top Level",
		.need_print	= 0,
	},
	{
		.name		= "otime",
		.column_name	= "OTime",
		.need_print	= 0,
	},
	{
		.name		= "parent_uuid",
		.column_name	= "Parent UUID",
		.need_print	= 0,
	},
	{
		.name		= "received_uuid",
		.column_name	= "Received UUID",
		.need_print	= 0,
	},
	{
		.name		= "uuid",
		.column_name	= "UUID",
		.need_print	= 0,
	},
	{
		.name		= "path",
		.column_name	= "Path",
		.need_print	= 0,
	},
	{
		.name		= NULL,
		.column_name	= NULL,
		.need_print	= 0,
	},
};

static btrfs_list_filter_func all_filter_funcs[];
static btrfs_list_comp_func all_comp_funcs[];

void btrfs_list_setup_print_column(enum btrfs_list_column_enum column)
{
	int i;

	ASSERT(0 <= column && column <= BTRFS_LIST_ALL);

	if (column < BTRFS_LIST_ALL) {
		btrfs_list_columns[column].need_print = 1;
		return;
	}

	for (i = 0; i < BTRFS_LIST_ALL; i++)
		btrfs_list_columns[i].need_print = 1;
}

static int comp_entry_with_rootid(struct root_info *entry1,
				  struct root_info *entry2,
				  int is_descending)
{
	int ret;

	if (entry1->root_id > entry2->root_id)
		ret = 1;
	else if (entry1->root_id < entry2->root_id)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static int comp_entry_with_gen(struct root_info *entry1,
			       struct root_info *entry2,
			       int is_descending)
{
	int ret;

	if (entry1->gen > entry2->gen)
		ret = 1;
	else if (entry1->gen < entry2->gen)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static int comp_entry_with_ogen(struct root_info *entry1,
				struct root_info *entry2,
				int is_descending)
{
	int ret;

	if (entry1->ogen > entry2->ogen)
		ret = 1;
	else if (entry1->ogen < entry2->ogen)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static int comp_entry_with_path(struct root_info *entry1,
				struct root_info *entry2,
				int is_descending)
{
	int ret;

	if (strcmp(entry1->full_path, entry2->full_path) > 0)
		ret = 1;
	else if (strcmp(entry1->full_path, entry2->full_path) < 0)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static btrfs_list_comp_func all_comp_funcs[] = {
	[BTRFS_LIST_COMP_ROOTID]	= comp_entry_with_rootid,
	[BTRFS_LIST_COMP_OGEN]		= comp_entry_with_ogen,
	[BTRFS_LIST_COMP_GEN]		= comp_entry_with_gen,
	[BTRFS_LIST_COMP_PATH]		= comp_entry_with_path,
};

static char *all_sort_items[] = {
	[BTRFS_LIST_COMP_ROOTID]	= "rootid",
	[BTRFS_LIST_COMP_OGEN]		= "ogen",
	[BTRFS_LIST_COMP_GEN]		= "gen",
	[BTRFS_LIST_COMP_PATH]		= "path",
	[BTRFS_LIST_COMP_MAX]		= NULL,
};

static int  btrfs_list_get_sort_item(char *sort_name)
{
	int i;

	for (i = 0; i < BTRFS_LIST_COMP_MAX; i++) {
		if (strcmp(sort_name, all_sort_items[i]) == 0)
			return i;
	}
	return -1;
}

struct btrfs_list_comparer_set *btrfs_list_alloc_comparer_set(void)
{
	struct btrfs_list_comparer_set *set;
	int size;

	size = sizeof(struct btrfs_list_comparer_set) +
	       BTRFS_LIST_NCOMPS_INCREASE * sizeof(struct btrfs_list_comparer);
	set = calloc(1, size);
	if (!set) {
		fprintf(stderr, "memory allocation failed\n");
		exit(1);
	}

	set->total = BTRFS_LIST_NCOMPS_INCREASE;

	return set;
}

static int btrfs_list_setup_comparer(struct btrfs_list_comparer_set **comp_set,
		enum btrfs_list_comp_enum comparer, int is_descending)
{
	struct btrfs_list_comparer_set *set = *comp_set;
	int size;

	ASSERT(set != NULL);
	ASSERT(comparer < BTRFS_LIST_COMP_MAX);
	ASSERT(set->ncomps <= set->total);

	if (set->ncomps == set->total) {
		void *tmp;

		size = set->total + BTRFS_LIST_NCOMPS_INCREASE;
		size = sizeof(*set) + size * sizeof(struct btrfs_list_comparer);
		tmp = set;
		set = realloc(set, size);
		if (!set) {
			fprintf(stderr, "memory allocation failed\n");
			free(tmp);
			exit(1);
		}

		memset(&set->comps[set->total], 0,
		       BTRFS_LIST_NCOMPS_INCREASE *
		       sizeof(struct btrfs_list_comparer));
		set->total += BTRFS_LIST_NCOMPS_INCREASE;
		*comp_set = set;
	}

	ASSERT(set->comps[set->ncomps].comp_func == NULL);

	set->comps[set->ncomps].comp_func = all_comp_funcs[comparer];
	set->comps[set->ncomps].is_descending = is_descending;
	set->ncomps++;
	return 0;
}

static int sort_comp(struct root_info *entry1, struct root_info *entry2,
		     struct btrfs_list_comparer_set *set)
{
	int rootid_compared = 0;
	int i, ret = 0;

	if (!set || !set->ncomps)
		return comp_entry_with_rootid(entry1, entry2, 0);

	for (i = 0; i < set->ncomps; i++) {
		if (!set->comps[i].comp_func)
			break;

		ret = set->comps[i].comp_func(entry1, entry2,
					      set->comps[i].is_descending);
		if (ret)
			return ret;

		if (set->comps[i].comp_func == comp_entry_with_rootid)
			rootid_compared = 1;
	}

	if (!rootid_compared)
		ret = comp_entry_with_rootid(entry1, entry2, 0);

	return ret;
}

static int sort_tree_insert(struct root_lookup *sort_tree,
			    struct root_info *ins,
			    struct btrfs_list_comparer_set *comp_set)
{
	struct rb_node **p = &sort_tree->root.rb_node;
	struct rb_node *parent = NULL;
	struct root_info *curr;
	int ret;

	while (*p) {
		parent = *p;
		curr = to_root_info_sorted(parent);

		ret = sort_comp(ins, curr, comp_set);
		if (ret < 0)
			p = &(*p)->rb_left;
		else if (ret > 0)
			p = &(*p)->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&ins->sort_node, parent, p);
	rb_insert_color(&ins->sort_node, &sort_tree->root);
	return 0;
}

/*
 * insert a new root into the tree.  returns the existing root entry
 * if one is already there.  Both root_id and ref_tree are used
 * as the key
 */
static int root_tree_insert(struct root_lookup *root_tree,
			    struct root_info *ins)
{
	struct rb_node **p = &root_tree->root.rb_node;
	struct rb_node * parent = NULL;
	struct root_info *curr;
	int ret;

	while(*p) {
		parent = *p;
		curr = to_root_info(parent);

		ret = comp_entry_with_rootid(ins, curr, 0);
		if (ret < 0)
			p = &(*p)->rb_left;
		else if (ret > 0)
			p = &(*p)->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&ins->rb_node, parent, p);
	rb_insert_color(&ins->rb_node, &root_tree->root);
	return 0;
}

/*
 * find a given root id in the tree.  We return the smallest one,
 * rb_next can be used to move forward looking for more if required
 */
static struct root_info *root_tree_search(struct root_lookup *root_tree,
					  u64 root_id)
{
	struct rb_node *n = root_tree->root.rb_node;
	struct root_info *entry;
	struct root_info tmp;
	int ret;

	tmp.root_id = root_id;

	while(n) {
		entry = to_root_info(n);

		ret = comp_entry_with_rootid(&tmp, entry, 0);
		if (ret < 0)
			n = n->rb_left;
		else if (ret > 0)
			n = n->rb_right;
		else
			return entry;
	}
	return NULL;
}

static int update_root(struct root_lookup *root_lookup,
		       u64 root_id, u64 ref_tree, u64 root_offset, u64 flags,
		       u64 dir_id, char *name, int name_len, u64 ogen, u64 gen,
		       time_t otime, u8 *uuid, u8 *puuid, u8 *ruuid)
{
	struct root_info *ri;

	ri = root_tree_search(root_lookup, root_id);
	if (!ri || ri->root_id != root_id)
		return -ENOENT;
	if (name && name_len > 0) {
		free(ri->name);

		ri->name = malloc(name_len + 1);
		if (!ri->name) {
			fprintf(stderr, "memory allocation failed\n");
			exit(1);
		}
		strncpy(ri->name, name, name_len);
		ri->name[name_len] = 0;
	}
	if (ref_tree)
		ri->ref_tree = ref_tree;
	if (root_offset)
		ri->root_offset = root_offset;
	if (flags)
		ri->flags = flags;
	if (dir_id)
		ri->dir_id = dir_id;
	if (gen)
		ri->gen = gen;
	if (ogen)
		ri->ogen = ogen;
	if (!ri->ogen && root_offset)
		ri->ogen = root_offset;
	if (otime)
		ri->otime = otime;
	if (uuid)
		memcpy(&ri->uuid, uuid, BTRFS_UUID_SIZE);
	if (puuid)
		memcpy(&ri->puuid, puuid, BTRFS_UUID_SIZE);
	if (ruuid)
		memcpy(&ri->ruuid, ruuid, BTRFS_UUID_SIZE);

	return 0;
}

/*
 * add_root - update the existed root, or allocate a new root and insert it
 *	      into the lookup tree.
 * root_id: object id of the root
 * ref_tree: object id of the referring root.
 * root_offset: offset value of the root'key
 * dir_id: inode id of the directory in ref_tree where this root can be found.
 * name: the name of root_id in that directory
 * name_len: the length of name
 * ogen: the original generation of the root
 * gen: the current generation of the root
 * otime: the original time (creation time) of the root
 * uuid: uuid of the root
 * puuid: uuid of the root parent if any
 * ruuid: uuid of the received subvol, if any
 */
static int add_root(struct root_lookup *root_lookup,
		    u64 root_id, u64 ref_tree, u64 root_offset, u64 flags,
		    u64 dir_id, char *name, int name_len, u64 ogen, u64 gen,
		    time_t otime, u8 *uuid, u8 *puuid, u8 *ruuid)
{
	struct root_info *ri;
	int ret;

	ret = update_root(root_lookup, root_id, ref_tree, root_offset, flags,
			  dir_id, name, name_len, ogen, gen, otime,
			  uuid, puuid, ruuid);
	if (!ret)
		return 0;

	ri = calloc(1, sizeof(*ri));
	if (!ri) {
		printf("memory allocation failed\n");
		exit(1);
	}
	ri->root_id = root_id;

	if (name && name_len > 0) {
		ri->name = malloc(name_len + 1);
		if (!ri->name) {
			fprintf(stderr, "memory allocation failed\n");
			exit(1);
		}
		strncpy(ri->name, name, name_len);
		ri->name[name_len] = 0;
	}
	if (ref_tree)
		ri->ref_tree = ref_tree;
	if (dir_id)
		ri->dir_id = dir_id;
	if (root_offset)
		ri->root_offset = root_offset;
	if (flags)
		ri->flags = flags;
	if (gen)
		ri->gen = gen;
	if (ogen)
		ri->ogen = ogen;
	if (!ri->ogen && root_offset)
		ri->ogen = root_offset;
	if (otime)
		ri->otime = otime;

	if (uuid)
		memcpy(&ri->uuid, uuid, BTRFS_UUID_SIZE);

	if (puuid)
		memcpy(&ri->puuid, puuid, BTRFS_UUID_SIZE);

	if (ruuid)
		memcpy(&ri->ruuid, ruuid, BTRFS_UUID_SIZE);

	ret = root_tree_insert(root_lookup, ri);
	if (ret < 0) {
		errno = -ret;
		error("failed to insert subvolume %llu to tree: %m",
				(unsigned long long)root_id);
		exit(1);
	}
	return 0;
}

/*
 * Simplified add_root for back references, omits the uuid and original info
 * parameters, root offset and flags.
 */
static int add_root_backref(struct root_lookup *root_lookup, u64 root_id,
		u64 ref_tree, u64 dir_id, char *name, int name_len)
{
	return add_root(root_lookup, root_id, ref_tree, 0, 0, dir_id, name,
			name_len, 0, 0, 0, NULL, NULL, NULL);
}


static void free_root_info(struct rb_node *node)
{
	struct root_info *ri;

	ri = to_root_info(node);
	free(ri->name);
	free(ri->path);
	free(ri->full_path);
	free(ri);
}

/*
 * for a given root_info, search through the root_lookup tree to construct
 * the full path name to it.
 *
 * This can't be called until all the root_info->path fields are filled
 * in by lookup_ino_path
 */
static int resolve_root(struct root_lookup *rl, struct root_info *ri,
		       u64 top_id)
{
	char *full_path = NULL;
	int len = 0;
	struct root_info *found;

	/*
	 * we go backwards from the root_info object and add pathnames
	 * from parent directories as we go.
	 */
	found = ri;
	while (1) {
		char *tmp;
		u64 next;
		int add_len;

		/*
		 * ref_tree = 0 indicates the subvolume
		 * has been deleted.
		 */
		if (!found->ref_tree) {
			free(full_path);
			return -ENOENT;
		}

		add_len = strlen(found->path);

		if (full_path) {
			/* room for / and for null */
			tmp = malloc(add_len + 2 + len);
			if (!tmp) {
				perror("malloc failed");
				exit(1);
			}
			memcpy(tmp + add_len + 1, full_path, len);
			tmp[add_len] = '/';
			memcpy(tmp, found->path, add_len);
			tmp [add_len + len + 1] = '\0';
			free(full_path);
			full_path = tmp;
			len += add_len + 1;
		} else {
			full_path = strdup(found->path);
			len = add_len;
		}
		if (!ri->top_id)
			ri->top_id = found->ref_tree;

		next = found->ref_tree;
		if (next == top_id)
			break;
		/*
		* if the ref_tree = BTRFS_FS_TREE_OBJECTID,
		* we are at the top
		*/
		if (next == BTRFS_FS_TREE_OBJECTID)
			break;
		/*
		* if the ref_tree wasn't in our tree of roots, the
		* subvolume was deleted.
		*/
		found = root_tree_search(rl, next);
		if (!found) {
			free(full_path);
			return -ENOENT;
		}
	}

	ri->full_path = full_path;

	return 0;
}

/*
 * for a single root_info, ask the kernel to give us a path name
 * inside it's ref_root for the dir_id where it lives.
 *
 * This fills in root_info->path with the path to the directory and and
 * appends this root's name.
 */
static int lookup_ino_path(int fd, struct root_info *ri)
{
	struct btrfs_ioctl_ino_lookup_args args;
	int ret;

	if (ri->path)
		return 0;

	if (!ri->ref_tree)
		return -ENOENT;

	memset(&args, 0, sizeof(args));
	args.treeid = ri->ref_tree;
	args.objectid = ri->dir_id;

	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &args);
	if (ret < 0) {
		if (errno == ENOENT) {
			ri->ref_tree = 0;
			return -ENOENT;
		}
		error("failed to lookup path for root %llu: %m",
			(unsigned long long)ri->ref_tree);
		return ret;
	}

	if (args.name[0]) {
		/*
		 * we're in a subdirectory of ref_tree, the kernel ioctl
		 * puts a / in there for us
		 */
		ri->path = malloc(strlen(ri->name) + strlen(args.name) + 1);
		if (!ri->path) {
			perror("malloc failed");
			exit(1);
		}
		strcpy(ri->path, args.name);
		strcat(ri->path, ri->name);
	} else {
		/* we're at the root of ref_tree */
		ri->path = strdup(ri->name);
		if (!ri->path) {
			perror("strdup failed");
			exit(1);
		}
	}
	return 0;
}

/* finding the generation for a given path is a two step process.
 * First we use the inode lookup routine to find out the root id
 *
 * Then we use the tree search ioctl to scan all the root items for a
 * given root id and spit out the latest generation we can find
 */
static u64 find_root_gen(int fd)
{
	struct btrfs_ioctl_ino_lookup_args ino_args;
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header sh;
	unsigned long off = 0;
	u64 max_found = 0;
	int i;

	memset(&ino_args, 0, sizeof(ino_args));
	ino_args.objectid = BTRFS_FIRST_FREE_OBJECTID;

	/* this ioctl fills in ino_args->treeid */
	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &ino_args);
	if (ret < 0) {
		error("failed to lookup path for dirid %llu: %m",
			(unsigned long long)BTRFS_FIRST_FREE_OBJECTID);
		return 0;
	}

	memset(&args, 0, sizeof(args));

	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;

	/*
	 * there may be more than one ROOT_ITEM key if there are
	 * snapshots pending deletion, we have to loop through
	 * them.
	 */
	sk->min_objectid = ino_args.treeid;
	sk->max_objectid = ino_args.treeid;
	sk->max_type = BTRFS_ROOT_ITEM_KEY;
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	while (1) {
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
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

			memcpy(&sh, args.buf + off, sizeof(sh));
			off += sizeof(sh);
			item = (struct btrfs_root_item *)(args.buf + off);
			off += sh.len;

			sk->min_objectid = sh.objectid;
			sk->min_type = sh.type;
			sk->min_offset = sh.offset;

			if (sh.objectid > ino_args.treeid)
				break;

			if (sh.objectid == ino_args.treeid &&
			    sh.type == BTRFS_ROOT_ITEM_KEY) {
				max_found = max(max_found,
						btrfs_root_generation(item));
			}
		}
		if (sk->min_offset < (u64)-1)
			sk->min_offset++;
		else
			break;

		if (sk->min_type != BTRFS_ROOT_ITEM_KEY)
			break;
		if (sk->min_objectid != ino_args.treeid)
			break;
	}
	return max_found;
}

/* pass in a directory id and this will return
 * the full path of the parent directory inside its
 * subvolume root.
 *
 * It may return NULL if it is in the root, or an ERR_PTR if things
 * go badly.
 */
static char *__ino_resolve(int fd, u64 dirid)
{
	struct btrfs_ioctl_ino_lookup_args args;
	int ret;
	char *full;

	memset(&args, 0, sizeof(args));
	args.objectid = dirid;

	ret = ioctl(fd, BTRFS_IOC_INO_LOOKUP, &args);
	if (ret < 0) {
		error("failed to lookup path for dirid %llu: %m",
			(unsigned long long)dirid);
		return ERR_PTR(ret);
	}

	if (args.name[0]) {
		/*
		 * we're in a subdirectory of ref_tree, the kernel ioctl
		 * puts a / in there for us
		 */
		full = strdup(args.name);
		if (!full) {
			perror("malloc failed");
			return ERR_PTR(-ENOMEM);
		}
	} else {
		/* we're at the root of ref_tree */
		full = NULL;
	}
	return full;
}

/*
 * simple string builder, returning a new string with both
 * dirid and name
 */
static char *build_name(const char *dirid, const char *name)
{
	char *full;

	if (!dirid)
		return strdup(name);

	full = malloc(strlen(dirid) + strlen(name) + 1);
	if (!full)
		return NULL;
	strcpy(full, dirid);
	strcat(full, name);
	return full;
}

/*
 * given an inode number, this returns the full path name inside the subvolume
 * to that file/directory.  cache_dirid and cache_name are used to
 * cache the results so we can avoid tree searches if a later call goes
 * to the same directory or file name
 */
static char *ino_resolve(int fd, u64 ino, u64 *cache_dirid, char **cache_name)

{
	u64 dirid;
	char *dirname;
	char *name;
	char *full;
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	unsigned long off = 0;
	int namelen;

	memset(&args, 0, sizeof(args));

	sk->tree_id = 0;

	/*
	 * step one, we search for the inode back ref.  We just use the first
	 * one
	 */
	sk->min_objectid = ino;
	sk->max_objectid = ino;
	sk->max_type = BTRFS_INODE_REF_KEY;
	sk->max_offset = (u64)-1;
	sk->min_type = BTRFS_INODE_REF_KEY;
	sk->max_transid = (u64)-1;
	sk->nr_items = 1;

	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
	if (ret < 0) {
		error("can't perform the search: %m");
		return NULL;
	}
	/* the ioctl returns the number of item it found in nr_items */
	if (sk->nr_items == 0)
		return NULL;

	off = 0;
	sh = (struct btrfs_ioctl_search_header *)(args.buf + off);

	if (btrfs_search_header_type(sh) == BTRFS_INODE_REF_KEY) {
		struct btrfs_inode_ref *ref;
		dirid = btrfs_search_header_offset(sh);

		ref = (struct btrfs_inode_ref *)(sh + 1);
		namelen = btrfs_stack_inode_ref_name_len(ref);

		name = (char *)(ref + 1);
		name = strndup(name, namelen);

		/* use our cached value */
		if (dirid == *cache_dirid && *cache_name) {
			dirname = *cache_name;
			goto build;
		}
	} else {
		return NULL;
	}
	/*
	 * the inode backref gives us the file name and the parent directory id.
	 * From here we use __ino_resolve to get the path to the parent
	 */
	dirname = __ino_resolve(fd, dirid);
build:
	full = build_name(dirname, name);
	if (*cache_name && dirname != *cache_name)
		free(*cache_name);

	*cache_name = dirname;
	*cache_dirid = dirid;
	free(name);

	return full;
}

int btrfs_list_get_default_subvolume(int fd, u64 *default_id)
{
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	u64 found = 0;
	int ret;

	memset(&args, 0, sizeof(args));

	/*
	 * search for a dir item with a name 'default' in the tree of
	 * tree roots, it should point us to a default root
	 */
	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;

	/* don't worry about ancient format and request only one item */
	sk->nr_items = 1;

	sk->max_objectid = BTRFS_ROOT_TREE_DIR_OBJECTID;
	sk->min_objectid = BTRFS_ROOT_TREE_DIR_OBJECTID;
	sk->max_type = BTRFS_DIR_ITEM_KEY;
	sk->min_type = BTRFS_DIR_ITEM_KEY;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;

	ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
	if (ret < 0)
		return ret;

	/* the ioctl returns the number of items it found in nr_items */
	if (sk->nr_items == 0)
		goto out;

	sh = (struct btrfs_ioctl_search_header *)args.buf;

	if (btrfs_search_header_type(sh) == BTRFS_DIR_ITEM_KEY) {
		struct btrfs_dir_item *di;
		int name_len;
		char *name;

		di = (struct btrfs_dir_item *)(sh + 1);
		name_len = btrfs_stack_dir_name_len(di);
		name = (char *)(di + 1);

		if (!strncmp("default", name, name_len))
			found = btrfs_disk_key_objectid(&di->location);
	}

out:
	*default_id = found;
	return 0;
}

static int list_subvol_search(int fd, struct root_lookup *root_lookup)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header sh;
	struct btrfs_root_ref *ref;
	struct btrfs_root_item *ri;
	unsigned long off;
	int name_len;
	char *name;
	u64 dir_id;
	u64 gen = 0;
	u64 ogen;
	u64 flags;
	int i;

	root_lookup->root.rb_node = NULL;
	memset(&args, 0, sizeof(args));

	sk->tree_id = BTRFS_ROOT_TREE_OBJECTID;
	/* Search both live and deleted subvolumes */
	sk->min_type = BTRFS_ROOT_ITEM_KEY;
	sk->max_type = BTRFS_ROOT_BACKREF_KEY;
	sk->min_objectid = BTRFS_FS_TREE_OBJECTID;
	sk->max_objectid = BTRFS_LAST_FREE_OBJECTID;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;

	while(1) {
		sk->nr_items = 4096;
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		if (ret < 0)
			return ret;
		if (sk->nr_items == 0)
			break;

		off = 0;

		/*
		 * for each item, pull the key out of the header and then
		 * read the root_ref item it contains
		 */
		for (i = 0; i < sk->nr_items; i++) {
			memcpy(&sh, args.buf + off, sizeof(sh));
			off += sizeof(sh);
			if (sh.type == BTRFS_ROOT_BACKREF_KEY) {
				ref = (struct btrfs_root_ref *)(args.buf + off);
				name_len = btrfs_stack_root_ref_name_len(ref);
				name = (char *)(ref + 1);
				dir_id = btrfs_stack_root_ref_dirid(ref);

				add_root_backref(root_lookup, sh.objectid,
						sh.offset, dir_id, name,
						name_len);
			} else if (sh.type == BTRFS_ROOT_ITEM_KEY &&
				   (sh.objectid >= BTRFS_FIRST_FREE_OBJECTID ||
				    sh.objectid == BTRFS_FS_TREE_OBJECTID)) {
				time_t otime;
				u8 uuid[BTRFS_UUID_SIZE];
				u8 puuid[BTRFS_UUID_SIZE];
				u8 ruuid[BTRFS_UUID_SIZE];

				ri = (struct btrfs_root_item *)(args.buf + off);
				gen = btrfs_root_generation(ri);
				flags = btrfs_root_flags(ri);
				if(sh.len >
				   sizeof(struct btrfs_root_item_v0)) {
					otime = btrfs_stack_timespec_sec(&ri->otime);
					ogen = btrfs_root_otransid(ri);
					memcpy(uuid, ri->uuid, BTRFS_UUID_SIZE);
					memcpy(puuid, ri->parent_uuid, BTRFS_UUID_SIZE);
					memcpy(ruuid, ri->received_uuid, BTRFS_UUID_SIZE);
				} else {
					otime = 0;
					ogen = 0;
					memset(uuid, 0, BTRFS_UUID_SIZE);
					memset(puuid, 0, BTRFS_UUID_SIZE);
					memset(ruuid, 0, BTRFS_UUID_SIZE);
				}

				add_root(root_lookup, sh.objectid, 0,
					 sh.offset, flags, 0, NULL, 0, ogen,
					 gen, otime, uuid, puuid, ruuid);
			}

			off += sh.len;

			sk->min_objectid = sh.objectid;
			sk->min_type = sh.type;
			sk->min_offset = sh.offset;
		}
		sk->min_offset++;
		if (!sk->min_offset)
			sk->min_type++;
		else
			continue;

		if (sk->min_type > BTRFS_ROOT_BACKREF_KEY) {
			sk->min_type = BTRFS_ROOT_ITEM_KEY;
			sk->min_objectid++;
		} else
			continue;

		if (sk->min_objectid > sk->max_objectid)
			break;
	}

	return 0;
}

static int filter_by_rootid(struct root_info *ri, u64 data)
{
	return ri->root_id == data;
}

static int filter_snapshot(struct root_info *ri, u64 data)
{
	return !!ri->root_offset;
}

static int filter_flags(struct root_info *ri, u64 flags)
{
	return ri->flags & flags;
}

static int filter_gen_more(struct root_info *ri, u64 data)
{
	return ri->gen >= data;
}

static int filter_gen_less(struct root_info *ri, u64 data)
{
	return ri->gen <= data;
}

static int filter_gen_equal(struct root_info  *ri, u64 data)
{
	return ri->gen == data;
}

static int filter_cgen_more(struct root_info *ri, u64 data)
{
	return ri->ogen >= data;
}

static int filter_cgen_less(struct root_info *ri, u64 data)
{
	return ri->ogen <= data;
}

static int filter_cgen_equal(struct root_info *ri, u64 data)
{
	return ri->ogen == data;
}

static int filter_topid_equal(struct root_info *ri, u64 data)
{
	return ri->top_id == data;
}

static int filter_full_path(struct root_info *ri, u64 data)
{
	if (ri->full_path && ri->top_id != data) {
		char *tmp;
		char p[] = "<FS_TREE>";
		int add_len = strlen(p);
		int len = strlen(ri->full_path);

		tmp = malloc(len + add_len + 2);
		if (!tmp) {
			fprintf(stderr, "memory allocation failed\n");
			exit(1);
		}
		memcpy(tmp + add_len + 1, ri->full_path, len);
		tmp[len + add_len + 1] = '\0';
		tmp[add_len] = '/';
		memcpy(tmp, p, add_len);
		free(ri->full_path);
		ri->full_path = tmp;
	}
	return 1;
}

static int filter_by_parent(struct root_info *ri, u64 data)
{
	return !uuid_compare(ri->puuid, (u8 *)(unsigned long)data);
}

static int filter_deleted(struct root_info *ri, u64 data)
{
	return ri->deleted;
}

static btrfs_list_filter_func all_filter_funcs[] = {
	[BTRFS_LIST_FILTER_ROOTID]		= filter_by_rootid,
	[BTRFS_LIST_FILTER_SNAPSHOT_ONLY]	= filter_snapshot,
	[BTRFS_LIST_FILTER_FLAGS]		= filter_flags,
	[BTRFS_LIST_FILTER_GEN_MORE]		= filter_gen_more,
	[BTRFS_LIST_FILTER_GEN_LESS]		= filter_gen_less,
	[BTRFS_LIST_FILTER_GEN_EQUAL]           = filter_gen_equal,
	[BTRFS_LIST_FILTER_CGEN_MORE]		= filter_cgen_more,
	[BTRFS_LIST_FILTER_CGEN_LESS]		= filter_cgen_less,
	[BTRFS_LIST_FILTER_CGEN_EQUAL]          = filter_cgen_equal,
	[BTRFS_LIST_FILTER_TOPID_EQUAL]		= filter_topid_equal,
	[BTRFS_LIST_FILTER_FULL_PATH]		= filter_full_path,
	[BTRFS_LIST_FILTER_BY_PARENT]		= filter_by_parent,
	[BTRFS_LIST_FILTER_DELETED]		= filter_deleted,
};

struct btrfs_list_filter_set *btrfs_list_alloc_filter_set(void)
{
	struct btrfs_list_filter_set *set;
	int size;

	size = sizeof(struct btrfs_list_filter_set) +
	       BTRFS_LIST_NFILTERS_INCREASE * sizeof(struct btrfs_list_filter);
	set = calloc(1, size);
	if (!set) {
		fprintf(stderr, "memory allocation failed\n");
		exit(1);
	}

	set->total = BTRFS_LIST_NFILTERS_INCREASE;

	return set;
}

/*
 * Setup list filters. Exit if there's not enough memory, as we can't continue
 * without the structures set up properly.
 */
void btrfs_list_setup_filter(struct btrfs_list_filter_set **filter_set,
			    enum btrfs_list_filter_enum filter, u64 data)
{
	struct btrfs_list_filter_set *set = *filter_set;
	int size;

	ASSERT(set != NULL);
	ASSERT(filter < BTRFS_LIST_FILTER_MAX);
	ASSERT(set->nfilters <= set->total);

	if (set->nfilters == set->total) {
		void *tmp;

		size = set->total + BTRFS_LIST_NFILTERS_INCREASE;
		size = sizeof(*set) + size * sizeof(struct btrfs_list_filter);
		tmp = set;
		set = realloc(set, size);
		if (!set) {
			fprintf(stderr, "memory allocation failed\n");
			free(tmp);
			exit(1);
		}

		memset(&set->filters[set->total], 0,
		       BTRFS_LIST_NFILTERS_INCREASE *
		       sizeof(struct btrfs_list_filter));
		set->total += BTRFS_LIST_NFILTERS_INCREASE;
		*filter_set = set;
	}

	ASSERT(set->filters[set->nfilters].filter_func == NULL);

	if (filter == BTRFS_LIST_FILTER_DELETED)
		set->only_deleted = 1;

	set->filters[set->nfilters].filter_func = all_filter_funcs[filter];
	set->filters[set->nfilters].data = data;
	set->nfilters++;
}

static int filter_root(struct root_info *ri,
		       struct btrfs_list_filter_set *set)
{
	int i, ret;

	if (!set)
		return 1;

	if (set->only_deleted && !ri->deleted)
		return 0;

	if (!set->only_deleted && ri->deleted)
		return 0;

	for (i = 0; i < set->nfilters; i++) {
		if (!set->filters[i].filter_func)
			break;
		ret = set->filters[i].filter_func(ri, set->filters[i].data);
		if (!ret)
			return 0;
	}
	return 1;
}

static void filter_and_sort_subvol(struct root_lookup *all_subvols,
				    struct root_lookup *sort_tree,
				    struct btrfs_list_filter_set *filter_set,
				    struct btrfs_list_comparer_set *comp_set,
				    u64 top_id)
{
	struct rb_node *n;
	struct root_info *entry;
	int ret;

	sort_tree->root.rb_node = NULL;

	n = rb_last(&all_subvols->root);
	while (n) {
		entry = to_root_info(n);

		ret = resolve_root(all_subvols, entry, top_id);
		if (ret == -ENOENT) {
			if (entry->root_id != BTRFS_FS_TREE_OBJECTID) {
				entry->full_path = strdup("DELETED");
				entry->deleted = 1;
			} else {
				/*
				 * The full path is not supposed to be printed,
				 * but we don't want to print an empty string,
				 * in case it appears somewhere.
				 */
				entry->full_path = strdup("TOPLEVEL");
				entry->deleted = 0;
			}
		}
		ret = filter_root(entry, filter_set);
		if (ret)
			sort_tree_insert(sort_tree, entry, comp_set);
		n = rb_prev(n);
	}
}

static int list_subvol_fill_paths(int fd, struct root_lookup *root_lookup)
{
	struct rb_node *n;

	n = rb_first(&root_lookup->root);
	while (n) {
		struct root_info *entry;
		int ret;
		entry = to_root_info(n);
		ret = lookup_ino_path(fd, entry);
		if (ret && ret != -ENOENT)
			return ret;
		n = rb_next(n);
	}

	return 0;
}

static void print_subvolume_column(struct root_info *subv,
				   enum btrfs_list_column_enum column)
{
	char tstr[256];
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];

	ASSERT(0 <= column && column < BTRFS_LIST_ALL);

	switch (column) {
	case BTRFS_LIST_OBJECTID:
		printf("%llu", subv->root_id);
		break;
	case BTRFS_LIST_GENERATION:
		printf("%llu", subv->gen);
		break;
	case BTRFS_LIST_OGENERATION:
		printf("%llu", subv->ogen);
		break;
	case BTRFS_LIST_PARENT:
		printf("%llu", subv->ref_tree);
		break;
	case BTRFS_LIST_TOP_LEVEL:
		printf("%llu", subv->top_id);
		break;
	case BTRFS_LIST_OTIME:
		if (subv->otime) {
			struct tm tm;

			localtime_r(&subv->otime, &tm);
			strftime(tstr, 256, "%Y-%m-%d %X", &tm);
		} else
			strcpy(tstr, "-");
		printf("%s", tstr);
		break;
	case BTRFS_LIST_UUID:
		if (uuid_is_null(subv->uuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subv->uuid, uuidparse);
		printf("%-36s", uuidparse);
		break;
	case BTRFS_LIST_PUUID:
		if (uuid_is_null(subv->puuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subv->puuid, uuidparse);
		printf("%-36s", uuidparse);
		break;
	case BTRFS_LIST_RUUID:
		if (uuid_is_null(subv->ruuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subv->ruuid, uuidparse);
		printf("%-36s", uuidparse);
		break;
	case BTRFS_LIST_PATH:
		BUG_ON(!subv->full_path);
		printf("%s", subv->full_path);
		break;
	default:
		break;
	}
}

static void print_one_subvol_info_raw(struct root_info *subv,
		const char *raw_prefix)
{
	int i;

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (!btrfs_list_columns[i].need_print)
			continue;

		if (raw_prefix)
			printf("%s",raw_prefix);

		print_subvolume_column(subv, i);
	}
	printf("\n");
}

static void print_one_subvol_info_table(struct root_info *subv)
{
	int i;

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (!btrfs_list_columns[i].need_print)
			continue;

		print_subvolume_column(subv, i);

		if (i != BTRFS_LIST_PATH)
			printf("\t");

		if (i == BTRFS_LIST_TOP_LEVEL)
			printf("\t");
	}
	printf("\n");
}

static void print_one_subvol_info_default(struct root_info *subv)
{
	int i;

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (!btrfs_list_columns[i].need_print)
			continue;

		printf("%s ", btrfs_list_columns[i].name);
		print_subvolume_column(subv, i);

		if (i != BTRFS_LIST_PATH)
			printf(" ");
	}
	printf("\n");
}

static void print_all_subvol_info_tab_head(void)
{
	int i;
	int len;
	char barrier[20];

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (btrfs_list_columns[i].need_print)
			printf("%s\t", btrfs_list_columns[i].name);

		if (i == BTRFS_LIST_ALL-1)
			printf("\n");
	}

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		memset(barrier, 0, sizeof(barrier));

		if (btrfs_list_columns[i].need_print) {
			len = strlen(btrfs_list_columns[i].name);
			while (len--)
				strcat(barrier, "-");

			printf("%s\t", barrier);
		}
		if (i == BTRFS_LIST_ALL-1)
			printf("\n");
	}
}

static void print_all_subvol_info(struct root_lookup *sorted_tree,
		  enum btrfs_list_layout layout, const char *raw_prefix)
{
	struct rb_node *n;
	struct root_info *entry;

	if (layout == BTRFS_LIST_LAYOUT_TABLE)
		print_all_subvol_info_tab_head();

	n = rb_first(&sorted_tree->root);
	while (n) {
		entry = to_root_info_sorted(n);

		/* The toplevel subvolume is not listed by default */
		if (entry->root_id == BTRFS_FS_TREE_OBJECTID)
			goto next;

		switch (layout) {
		case BTRFS_LIST_LAYOUT_DEFAULT:
			print_one_subvol_info_default(entry);
			break;
		case BTRFS_LIST_LAYOUT_TABLE:
			print_one_subvol_info_table(entry);
			break;
		case BTRFS_LIST_LAYOUT_RAW:
			print_one_subvol_info_raw(entry, raw_prefix);
			break;
		}
next:
		n = rb_next(n);
	}
}

static int btrfs_list_subvols(int fd, struct root_lookup *root_lookup)
{
	int ret;

	ret = list_subvol_search(fd, root_lookup);
	if (ret) {
		error("can't perform the search: %m");
		return ret;
	}

	/*
	 * now we have an rbtree full of root_info objects, but we need to fill
	 * in their path names within the subvol that is referencing each one.
	 */
	ret = list_subvol_fill_paths(fd, root_lookup);
	return ret;
}

int btrfs_list_subvols_print(int fd, struct btrfs_list_filter_set *filter_set,
		       struct btrfs_list_comparer_set *comp_set,
		       enum btrfs_list_layout layout, int full_path,
		       const char *raw_prefix)
{
	struct root_lookup root_lookup;
	struct root_lookup root_sort;
	int ret = 0;
	u64 top_id = 0;

	if (full_path)
		ret = btrfs_list_get_path_rootid(fd, &top_id);
	if (ret)
		return ret;

	ret = btrfs_list_subvols(fd, &root_lookup);
	if (ret)
		return ret;
	filter_and_sort_subvol(&root_lookup, &root_sort, filter_set,
				 comp_set, top_id);

	print_all_subvol_info(&root_sort, layout, raw_prefix);
	rb_free_nodes(&root_lookup.root, free_root_info);

	return 0;
}

static char *strdup_or_null(const char *s)
{
	if (!s)
		return NULL;
	return strdup(s);
}

int btrfs_get_toplevel_subvol(int fd, struct root_info *the_ri)
{
	int ret;
	struct root_lookup rl;
	struct rb_node *rbn;
	struct root_info *ri;
	u64 root_id;

	ret = btrfs_list_get_path_rootid(fd, &root_id);
	if (ret)
		return ret;

	ret = btrfs_list_subvols(fd, &rl);
	if (ret)
		return ret;

	rbn = rb_first(&rl.root);
	ri = to_root_info(rbn);

	if (ri->root_id != BTRFS_FS_TREE_OBJECTID)
		return -ENOENT;

	memcpy(the_ri, ri, offsetof(struct root_info, path));
	the_ri->path = strdup_or_null("/");
	the_ri->name = strdup_or_null("<FS_TREE>");
	the_ri->full_path = strdup_or_null("/");
	rb_free_nodes(&rl.root, free_root_info);

	return ret;
}

int btrfs_get_subvol(int fd, struct root_info *the_ri)
{
	int ret, rr;
	struct root_lookup rl;
	struct rb_node *rbn;
	struct root_info *ri;
	u64 root_id;

	ret = btrfs_list_get_path_rootid(fd, &root_id);
	if (ret)
		return ret;

	ret = btrfs_list_subvols(fd, &rl);
	if (ret)
		return ret;

	rbn = rb_first(&rl.root);
	while(rbn) {
		ri = to_root_info(rbn);
		rr = resolve_root(&rl, ri, root_id);
		if (rr == -ENOENT) {
			ret = -ENOENT;
			rbn = rb_next(rbn);
			continue;
		}

		if (!comp_entry_with_rootid(the_ri, ri, 0) ||
			!uuid_compare(the_ri->uuid, ri->uuid)) {
			memcpy(the_ri, ri, offsetof(struct root_info, path));
			the_ri->path = strdup_or_null(ri->path);
			the_ri->name = strdup_or_null(ri->name);
			the_ri->full_path = strdup_or_null(ri->full_path);
			ret = 0;
			break;
		}
		rbn = rb_next(rbn);
	}
	rb_free_nodes(&rl.root, free_root_info);
	return ret;
}

static int print_one_extent(int fd, struct btrfs_ioctl_search_header *sh,
			    struct btrfs_file_extent_item *item,
			    u64 found_gen, u64 *cache_dirid,
			    char **cache_dir_name, u64 *cache_ino,
			    char **cache_full_name)
{
	u64 len = 0;
	u64 disk_start = 0;
	u64 disk_offset = 0;
	u8 type;
	int compressed = 0;
	int flags = 0;
	char *name = NULL;

	if (btrfs_search_header_objectid(sh) == *cache_ino) {
		name = *cache_full_name;
	} else if (*cache_full_name) {
		free(*cache_full_name);
		*cache_full_name = NULL;
	}
	if (!name) {
		name = ino_resolve(fd, btrfs_search_header_objectid(sh),
				   cache_dirid,
				   cache_dir_name);
		*cache_full_name = name;
		*cache_ino = btrfs_search_header_objectid(sh);
	}
	if (!name)
		return -EIO;

	type = btrfs_stack_file_extent_type(item);
	compressed = btrfs_stack_file_extent_compression(item);

	if (type == BTRFS_FILE_EXTENT_REG ||
	    type == BTRFS_FILE_EXTENT_PREALLOC) {
		disk_start = btrfs_stack_file_extent_disk_bytenr(item);
		disk_offset = btrfs_stack_file_extent_offset(item);
		len = btrfs_stack_file_extent_num_bytes(item);
	} else if (type == BTRFS_FILE_EXTENT_INLINE) {
		disk_start = 0;
		disk_offset = 0;
		len = btrfs_stack_file_extent_ram_bytes(item);
	} else {
		error(
	"unhandled extent type %d for inode %llu file offset %llu gen %llu",
			type,
			(unsigned long long)btrfs_search_header_objectid(sh),
			(unsigned long long)btrfs_search_header_offset(sh),
			(unsigned long long)found_gen);

		return -EIO;
	}
	printf("inode %llu file offset %llu len %llu disk start %llu "
	       "offset %llu gen %llu flags ",
	       (unsigned long long)btrfs_search_header_objectid(sh),
	       (unsigned long long)btrfs_search_header_offset(sh),
	       (unsigned long long)len,
	       (unsigned long long)disk_start,
	       (unsigned long long)disk_offset,
	       (unsigned long long)found_gen);

	if (compressed) {
		printf("COMPRESS");
		flags++;
	}
	if (type == BTRFS_FILE_EXTENT_PREALLOC) {
		printf("%sPREALLOC", flags ? "|" : "");
		flags++;
	}
	if (type == BTRFS_FILE_EXTENT_INLINE) {
		printf("%sINLINE", flags ? "|" : "");
		flags++;
	}
	if (!flags)
		printf("NONE");

	printf(" %s\n", name);
	return 0;
}

int btrfs_list_find_updated_files(int fd, u64 root_id, u64 oldest_gen)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header sh;
	struct btrfs_file_extent_item *item;
	unsigned long off = 0;
	u64 found_gen;
	u64 max_found = 0;
	int i;
	u64 cache_dirid = 0;
	u64 cache_ino = 0;
	char *cache_dir_name = NULL;
	char *cache_full_name = NULL;
	struct btrfs_file_extent_item backup;

	memset(&backup, 0, sizeof(backup));
	memset(&args, 0, sizeof(args));

	sk->tree_id = root_id;

	/*
	 * set all the other params to the max, we'll take any objectid
	 * and any trans
	 */
	sk->max_objectid = (u64)-1;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->max_type = BTRFS_EXTENT_DATA_KEY;
	sk->min_transid = oldest_gen;
	/* just a big number, doesn't matter much */
	sk->nr_items = 4096;

	max_found = find_root_gen(fd);
	while(1) {
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		if (ret < 0) {
			error("can't perform the search: %m");
			break;
		}
		/* the ioctl returns the number of item it found in nr_items */
		if (sk->nr_items == 0)
			break;

		off = 0;

		/*
		 * for each item, pull the key out of the header and then
		 * read the root_ref item it contains
		 */
		for (i = 0; i < sk->nr_items; i++) {
			memcpy(&sh, args.buf + off, sizeof(sh));
			off += sizeof(sh);

			/*
			 * just in case the item was too big, pass something other
			 * than garbage
			 */
			if (sh.len == 0)
				item = &backup;
			else
				item = (struct btrfs_file_extent_item *)(args.buf +
								 off);
			found_gen = btrfs_stack_file_extent_generation(item);
			if (sh.type == BTRFS_EXTENT_DATA_KEY &&
			    found_gen >= oldest_gen) {
				print_one_extent(fd, &sh, item, found_gen,
						 &cache_dirid, &cache_dir_name,
						 &cache_ino, &cache_full_name);
			}
			off += sh.len;

			/*
			 * record the mins in sk so we can make sure the
			 * next search doesn't repeat this root
			 */
			sk->min_objectid = sh.objectid;
			sk->min_offset = sh.offset;
			sk->min_type = sh.type;
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
	free(cache_dir_name);
	free(cache_full_name);
	printf("transid marker was %llu\n", (unsigned long long)max_found);
	return ret;
}

char *btrfs_list_path_for_root(int fd, u64 root)
{
	struct root_lookup root_lookup;
	struct rb_node *n;
	char *ret_path = NULL;
	int ret;
	u64 top_id;

	ret = btrfs_list_get_path_rootid(fd, &top_id);
	if (ret)
		return ERR_PTR(ret);

	ret = list_subvol_search(fd, &root_lookup);
	if (ret < 0)
		return ERR_PTR(ret);

	ret = list_subvol_fill_paths(fd, &root_lookup);
	if (ret < 0)
		return ERR_PTR(ret);

	n = rb_last(&root_lookup.root);
	while (n) {
		struct root_info *entry;

		entry = to_root_info(n);
		ret = resolve_root(&root_lookup, entry, top_id);
		if (ret == -ENOENT && entry->root_id == root) {
			ret_path = NULL;
			break;
		}
		if (entry->root_id == root) {
			ret_path = entry->full_path;
			entry->full_path = NULL;
		}

		n = rb_prev(n);
	}
	rb_free_nodes(&root_lookup.root, free_root_info);

	return ret_path;
}

int btrfs_list_parse_sort_string(char *opt_arg,
				 struct btrfs_list_comparer_set **comps)
{
	int order;
	int flag;
	char *p;
	char **ptr_argv;
	int what_to_sort;

	while ((p = strtok(opt_arg, ",")) != NULL) {
		flag = 0;
		ptr_argv = all_sort_items;

		while (*ptr_argv) {
			if (strcmp(*ptr_argv, p) == 0) {
				flag = 1;
				break;
			} else {
				p++;
				if (strcmp(*ptr_argv, p) == 0) {
					flag = 1;
					p--;
					break;
				}
				p--;
			}
			ptr_argv++;
		}

		if (flag == 0)
			return -1;

		else {
			if (*p == '+') {
				order = 0;
				p++;
			} else if (*p == '-') {
				order = 1;
				p++;
			} else
				order = 0;

			what_to_sort = btrfs_list_get_sort_item(p);
			btrfs_list_setup_comparer(comps, what_to_sort, order);
		}
		opt_arg = NULL;
	}

	return 0;
}

/*
 * This function is used to parse the argument of filter condition.
 *
 * type is the filter object.
 */
int btrfs_list_parse_filter_string(char *opt_arg,
				   struct btrfs_list_filter_set **filters,
				   enum btrfs_list_filter_enum type)
{

	u64 arg;

	switch (*(opt_arg++)) {
	case '+':
		arg = arg_strtou64(opt_arg);
		type += 2;

		btrfs_list_setup_filter(filters, type, arg);
		break;
	case '-':
		arg = arg_strtou64(opt_arg);
		type += 1;

		btrfs_list_setup_filter(filters, type, arg);
		break;
	default:
		opt_arg--;
		arg = arg_strtou64(opt_arg);

		btrfs_list_setup_filter(filters, type, arg);
		break;
	}

	return 0;
}

int btrfs_list_get_path_rootid(int fd, u64 *treeid)
{
	int ret;

	ret = lookup_path_rootid(fd, treeid);
	if (ret < 0)
		error("cannot resolve rootid for path: %m");

	return ret;
}
