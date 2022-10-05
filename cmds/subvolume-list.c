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
#include <sys/ioctl.h>
#include <getopt.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>
#include "kernel-lib/rbtree.h"
#include "kernel-lib/rbtree_types.h"
#include "kernel-shared/ctree.h"
#include "common/defs.h"
#include "common/rbtree-utils.h"
#include "common/help.h"
#include "common/messages.h"
#include "common/open-utils.h"
#include "common/string-utils.h"
#include "common/utils.h"
#include "cmds/commands.h"
#include "ioctl.h"

/*
 * Naming of options:
 * - uppercase for filters and sort options
 * - lowercase for enabling specific items in the output
 */
static const char * const cmd_subvol_list_usage[] = {
	"btrfs subvolume list [options] <path>",
	"List subvolumes and snapshots in the filesystem.",
	"",
	"Path filtering:",
	"-o           print only subvolumes below specified path",
	"-a           print all the subvolumes in the filesystem and",
	"             distinguish absolute and relative path with respect",
	"             to the given <path>",
	"",
	"Field selection:",
	"-p           print parent ID",
	"-c           print the ogeneration of the subvolume",
	"-g           print the generation of the subvolume",
	"-u           print the uuid of subvolumes (and snapshots)",
	"-q           print the parent uuid of the snapshots",
	"-R           print the uuid of the received snapshots",
	"",
	"Type filtering:",
	"-s           list only snapshots",
	"-r           list readonly subvolumes (including snapshots)",
	"-d           list deleted subvolumes that are not yet cleaned",
	"",
	"Other:",
	"-t           print the result as a table",
	"",
	"Sorting:",
	"-G [+|-]value",
	"             filter the subvolumes by generation",
	"             (+value: >= value; -value: <= value; value: = value)",
	"-C [+|-]value",
	"             filter the subvolumes by ogeneration",
	"             (+value: >= value; -value: <= value; value: = value)",
	"--sort=gen,ogen,rootid,path",
	"             list the subvolume in order of gen, ogen, rootid or path",
	"             you also can add '+' or '-' in front of each items.",
	"             (+:ascending, -:descending, ascending default)",
	NULL,
};

#define BTRFS_LIST_NFILTERS_INCREASE	(2 * BTRFS_LIST_FILTER_MAX)
#define BTRFS_LIST_NCOMPS_INCREASE	(2 * BTRFS_LIST_COMP_MAX)

enum btrfs_list_layout {
	BTRFS_LIST_LAYOUT_DEFAULT = 0,
	BTRFS_LIST_LAYOUT_TABLE,
	BTRFS_LIST_LAYOUT_RAW
};

/*
 * one of these for each root we find.
 */
struct root_info {
	struct rb_node rb_node;
	struct rb_node sort_node;

	/* this root's id */
	u64 root_id;

	/* equal the offset of the root's key */
	u64 root_offset;

	/* flags of the root */
	u64 flags;

	/* the id of the root that references this one */
	u64 ref_tree;

	/* the dir id we're in from ref_tree */
	u64 dir_id;

	u64 top_id;

	/* generation when the root is created or last updated */
	u64 gen;

	/* creation generation of this root in sec*/
	u64 ogen;

	/* creation time of this root in sec*/
	time_t otime;

	u8 uuid[BTRFS_UUID_SIZE];
	u8 puuid[BTRFS_UUID_SIZE];
	u8 ruuid[BTRFS_UUID_SIZE];

	/* path from the subvol we live in to this root, including the
	 * root's name.  This is null until we do the extra lookup ioctl.
	 */
	char *path;

	/* the name of this root in the directory it lives in */
	char *name;

	char *full_path;

	int deleted;
};

typedef int (*btrfs_list_filter_func)(struct root_info *, u64);
typedef int (*btrfs_list_comp_func)(const struct root_info *a,
				    const struct root_info *b);

struct btrfs_list_filter {
	btrfs_list_filter_func filter_func;
	u64 data;
};

struct btrfs_list_comparer {
	btrfs_list_comp_func comp_func;
	int is_descending;
};

struct btrfs_list_filter_set {
	int total;
	int nfilters;
	int only_deleted;
	struct btrfs_list_filter filters[0];
};

struct btrfs_list_comparer_set {
	int total;
	int ncomps;
	struct btrfs_list_comparer comps[0];
};

enum btrfs_list_column_enum {
	BTRFS_LIST_OBJECTID,
	BTRFS_LIST_GENERATION,
	BTRFS_LIST_OGENERATION,
	BTRFS_LIST_PARENT,
	BTRFS_LIST_TOP_LEVEL,
	BTRFS_LIST_OTIME,
	BTRFS_LIST_PUUID,
	BTRFS_LIST_RUUID,
	BTRFS_LIST_UUID,
	BTRFS_LIST_PATH,
	BTRFS_LIST_ALL,
};

enum btrfs_list_filter_enum {
	BTRFS_LIST_FILTER_ROOTID,
	BTRFS_LIST_FILTER_SNAPSHOT_ONLY,
	BTRFS_LIST_FILTER_FLAGS,
	BTRFS_LIST_FILTER_GEN,
	BTRFS_LIST_FILTER_GEN_EQUAL	=	BTRFS_LIST_FILTER_GEN,
	BTRFS_LIST_FILTER_GEN_LESS,
	BTRFS_LIST_FILTER_GEN_MORE,
	BTRFS_LIST_FILTER_CGEN,
	BTRFS_LIST_FILTER_CGEN_EQUAL	=	BTRFS_LIST_FILTER_CGEN,
	BTRFS_LIST_FILTER_CGEN_LESS,
	BTRFS_LIST_FILTER_CGEN_MORE,
	BTRFS_LIST_FILTER_TOPID_EQUAL,
	BTRFS_LIST_FILTER_FULL_PATH,
	BTRFS_LIST_FILTER_BY_PARENT,
	BTRFS_LIST_FILTER_DELETED,
	BTRFS_LIST_FILTER_MAX,
};

enum btrfs_list_comp_enum {
	BTRFS_LIST_COMP_ROOTID,
	BTRFS_LIST_COMP_OGEN,
	BTRFS_LIST_COMP_GEN,
	BTRFS_LIST_COMP_PATH,
	BTRFS_LIST_COMP_MAX,
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

static int comp_entry_with_rootid(const struct root_info *entry1,
				  const struct root_info *entry2)
{
	if (entry1->root_id > entry2->root_id)
		return 1;
	else if (entry1->root_id < entry2->root_id)
		return -1;
	return 0;
}

static int comp_entry_with_gen(const struct root_info *entry1,
			       const struct root_info *entry2)
{
	if (entry1->gen > entry2->gen)
		return 1;
	else if (entry1->gen < entry2->gen)
		return -1;
	return 0;
}

static int comp_entry_with_ogen(const struct root_info *entry1,
				const struct root_info *entry2)
{
	if (entry1->ogen > entry2->ogen)
		return 1;
	else if (entry1->ogen < entry2->ogen)
		return -1;
	return 0;
}

static int comp_entry_with_path(const struct root_info *entry1,
				const struct root_info *entry2)
{
	if (strcmp(entry1->full_path, entry2->full_path) > 0)
		return 1;
	else if (strcmp(entry1->full_path, entry2->full_path) < 0)
		return -1;
	return 0;
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
			error_msg(ERROR_MSG_MEMORY, NULL);
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

static int sort_comp(const struct root_info *entry1, const struct root_info *entry2,
		     struct btrfs_list_comparer_set *set)
{
	bool rootid_compared = false;
	int i, ret = 0;

	if (!set || !set->ncomps)
		return comp_entry_with_rootid(entry1, entry2);

	for (i = 0; i < set->ncomps; i++) {
		if (!set->comps[i].comp_func)
			break;

		ret = set->comps[i].comp_func(entry1, entry2);
		if (set->comps[i].is_descending)
			ret = -ret;
		if (ret)
			return ret;

		if (set->comps[i].comp_func == comp_entry_with_rootid)
			rootid_compared = true;
	}

	if (!rootid_compared)
		ret = comp_entry_with_rootid(entry1, entry2);

	return ret;
}

static int sort_tree_insert(struct rb_root *sort_tree,
			    struct root_info *ins,
			    struct btrfs_list_comparer_set *comp_set)
{
	struct rb_node **p = &sort_tree->rb_node;
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
	rb_insert_color(&ins->sort_node, sort_tree);
	return 0;
}

/*
 * insert a new root into the tree.  returns the existing root entry
 * if one is already there.  Both root_id and ref_tree are used
 * as the key
 */
static int root_tree_insert(struct rb_root *root_tree,
			    struct root_info *ins)
{
	struct rb_node **p = &root_tree->rb_node;
	struct rb_node * parent = NULL;
	struct root_info *curr;
	int ret;

	while(*p) {
		parent = *p;
		curr = to_root_info(parent);

		ret = comp_entry_with_rootid(ins, curr);
		if (ret < 0)
			p = &(*p)->rb_left;
		else if (ret > 0)
			p = &(*p)->rb_right;
		else
			return -EEXIST;
	}

	rb_link_node(&ins->rb_node, parent, p);
	rb_insert_color(&ins->rb_node, root_tree);
	return 0;
}

/*
 * find a given root id in the tree.  We return the smallest one,
 * rb_next can be used to move forward looking for more if required
 */
static struct root_info *root_tree_search(struct rb_root *root_tree,
					  u64 root_id)
{
	struct rb_node *n = root_tree->rb_node;
	struct root_info *entry;
	struct root_info tmp;
	int ret;

	tmp.root_id = root_id;

	while(n) {
		entry = to_root_info(n);

		ret = comp_entry_with_rootid(&tmp, entry);
		if (ret < 0)
			n = n->rb_left;
		else if (ret > 0)
			n = n->rb_right;
		else
			return entry;
	}
	return NULL;
}

static int update_root(struct rb_root *root_lookup,
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
			error_msg(ERROR_MSG_MEMORY, NULL);
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
static int add_root(struct rb_root *root_lookup,
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
		error_msg(ERROR_MSG_MEMORY, NULL);
		exit(1);
	}
	ri->root_id = root_id;

	if (name && name_len > 0) {
		ri->name = malloc(name_len + 1);
		if (!ri->name) {
			error_msg(ERROR_MSG_MEMORY, NULL);
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
		error("failed to insert subvolume %llu to tree: %m", root_id);
		exit(1);
	}
	return 0;
}

/*
 * Simplified add_root for back references, omits the uuid and original info
 * parameters, root offset and flags.
 */
static int add_root_backref(struct rb_root *root_lookup, u64 root_id,
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
static int resolve_root(struct rb_root *rl, struct root_info *ri,
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
				error_msg(ERROR_MSG_MEMORY, NULL);
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
		error("failed to lookup path for root %llu: %m", ri->ref_tree);
		return ret;
	}

	if (args.name[0]) {
		/*
		 * we're in a subdirectory of ref_tree, the kernel ioctl
		 * puts a / in there for us
		 */
		ri->path = malloc(strlen(ri->name) + strlen(args.name) + 1);
		if (!ri->path) {
			error_msg(ERROR_MSG_MEMORY, NULL);
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

static int list_subvol_search(int fd, struct rb_root *root_lookup)
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

	root_lookup->rb_node = NULL;
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
			error_msg(ERROR_MSG_MEMORY, NULL);
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

/*
 * Setup list filters. Exit if there's not enough memory, as we can't continue
 * without the structures set up properly.
 */
static void btrfs_list_setup_filter(struct btrfs_list_filter_set **filter_set,
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
			error_msg(ERROR_MSG_MEMORY, NULL);
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

static void filter_and_sort_subvol(struct rb_root *all_subvols,
				    struct rb_root *sort_tree,
				    struct btrfs_list_filter_set *filter_set,
				    struct btrfs_list_comparer_set *comp_set,
				    u64 top_id)
{
	struct rb_node *n;
	struct root_info *entry;
	int ret;

	sort_tree->rb_node = NULL;

	n = rb_last(all_subvols);
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

static void print_subvolume_column(struct root_info *subv,
				   enum btrfs_list_column_enum column)
{
	char tstr[256];
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];

	ASSERT(0 <= column && column < BTRFS_LIST_ALL);

	switch (column) {
	case BTRFS_LIST_OBJECTID:
		pr_verbose(LOG_DEFAULT, "%llu", subv->root_id);
		break;
	case BTRFS_LIST_GENERATION:
		pr_verbose(LOG_DEFAULT, "%llu", subv->gen);
		break;
	case BTRFS_LIST_OGENERATION:
		pr_verbose(LOG_DEFAULT, "%llu", subv->ogen);
		break;
	case BTRFS_LIST_PARENT:
		pr_verbose(LOG_DEFAULT, "%llu", subv->ref_tree);
		break;
	case BTRFS_LIST_TOP_LEVEL:
		pr_verbose(LOG_DEFAULT, "%llu", subv->top_id);
		break;
	case BTRFS_LIST_OTIME:
		if (subv->otime) {
			struct tm tm;

			localtime_r(&subv->otime, &tm);
			strftime(tstr, 256, "%Y-%m-%d %X", &tm);
		} else
			strcpy(tstr, "-");
		pr_verbose(LOG_DEFAULT, "%s", tstr);
		break;
	case BTRFS_LIST_UUID:
		if (uuid_is_null(subv->uuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subv->uuid, uuidparse);
		pr_verbose(LOG_DEFAULT, "%-36s", uuidparse);
		break;
	case BTRFS_LIST_PUUID:
		if (uuid_is_null(subv->puuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subv->puuid, uuidparse);
		pr_verbose(LOG_DEFAULT, "%-36s", uuidparse);
		break;
	case BTRFS_LIST_RUUID:
		if (uuid_is_null(subv->ruuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subv->ruuid, uuidparse);
		pr_verbose(LOG_DEFAULT, "%-36s", uuidparse);
		break;
	case BTRFS_LIST_PATH:
		BUG_ON(!subv->full_path);
		pr_verbose(LOG_DEFAULT, "%s", subv->full_path);
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
			pr_verbose(LOG_DEFAULT, "%s",raw_prefix);

		print_subvolume_column(subv, i);
	}
	pr_verbose(LOG_DEFAULT, "\n");
}

static void print_one_subvol_info_table(struct root_info *subv)
{
	int i;

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (!btrfs_list_columns[i].need_print)
			continue;

		print_subvolume_column(subv, i);

		if (i != BTRFS_LIST_PATH)
			pr_verbose(LOG_DEFAULT, "\t");

		if (i == BTRFS_LIST_TOP_LEVEL)
			pr_verbose(LOG_DEFAULT, "\t");
	}
	pr_verbose(LOG_DEFAULT, "\n");
}

static void print_one_subvol_info_default(struct root_info *subv)
{
	int i;

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (!btrfs_list_columns[i].need_print)
			continue;

		pr_verbose(LOG_DEFAULT, "%s ", btrfs_list_columns[i].name);
		print_subvolume_column(subv, i);

		if (i != BTRFS_LIST_PATH)
			pr_verbose(LOG_DEFAULT, " ");
	}
	pr_verbose(LOG_DEFAULT, "\n");
}

static void print_all_subvol_info_tab_head(void)
{
	int i;
	int len;
	char barrier[20];

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (btrfs_list_columns[i].need_print)
			pr_verbose(LOG_DEFAULT, "%s\t", btrfs_list_columns[i].name);

		if (i == BTRFS_LIST_ALL-1)
			pr_verbose(LOG_DEFAULT, "\n");
	}

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		memset(barrier, 0, sizeof(barrier));

		if (btrfs_list_columns[i].need_print) {
			len = strlen(btrfs_list_columns[i].name);
			while (len--)
				strcat(barrier, "-");

			pr_verbose(LOG_DEFAULT, "%s\t", barrier);
		}
		if (i == BTRFS_LIST_ALL-1)
			pr_verbose(LOG_DEFAULT, "\n");
	}
}

static void print_all_subvol_info(struct rb_root *sorted_tree,
		  enum btrfs_list_layout layout, const char *raw_prefix)
{
	struct rb_node *n;
	struct root_info *entry;

	if (layout == BTRFS_LIST_LAYOUT_TABLE)
		print_all_subvol_info_tab_head();

	n = rb_first(sorted_tree);
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

static int btrfs_list_subvols(int fd, struct rb_root *root_lookup)
{
	int ret;
	struct rb_node *n;

	ret = list_subvol_search(fd, root_lookup);
	if (ret) {
		error("can't perform the search: %m");
		return ret;
	}

	/*
	 * now we have an rbtree full of root_info objects, but we need to fill
	 * in their path names within the subvol that is referencing each one.
	 */
	n = rb_first(root_lookup);
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

static int btrfs_list_subvols_print(int fd, struct btrfs_list_filter_set *filter_set,
		       struct btrfs_list_comparer_set *comp_set,
		       enum btrfs_list_layout layout, int full_path,
		       const char *raw_prefix)
{
	struct rb_root root_lookup;
	struct rb_root root_sort;
	int ret = 0;
	u64 top_id = 0;

	if (full_path) {
		ret = lookup_path_rootid(fd, &top_id);
		if (ret) {
			errno = -ret;
			error("cannot resolve rootid for path: %m");
			return ret;
		}
	}

	ret = btrfs_list_subvols(fd, &root_lookup);
	if (ret)
		return ret;
	filter_and_sort_subvol(&root_lookup, &root_sort, filter_set,
				 comp_set, top_id);

	print_all_subvol_info(&root_sort, layout, raw_prefix);
	rb_free_nodes(&root_lookup, free_root_info);

	return 0;
}

static int btrfs_list_parse_sort_string(char *opt_arg,
				 struct btrfs_list_comparer_set **comps)
{
	bool order;
	bool flag;
	char *p;
	char **ptr_argv;
	int what_to_sort;

	while ((p = strtok(opt_arg, ",")) != NULL) {
		flag = false;
		ptr_argv = all_sort_items;

		while (*ptr_argv) {
			if (strcmp(*ptr_argv, p) == 0) {
				flag = true;
				break;
			} else {
				p++;
				if (strcmp(*ptr_argv, p) == 0) {
					flag = true;
					p--;
					break;
				}
				p--;
			}
			ptr_argv++;
		}

		if (flag == false)
			return -1;

		else {
			if (*p == '+') {
				order = false;
				p++;
			} else if (*p == '-') {
				order = true;
				p++;
			} else
				order = false;

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
static int btrfs_list_parse_filter_string(char *opt_arg,
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

static struct btrfs_list_filter_set *btrfs_list_alloc_filter_set(void)
{
	struct btrfs_list_filter_set *set;
	int size;

	size = sizeof(struct btrfs_list_filter_set) +
	       BTRFS_LIST_NFILTERS_INCREASE * sizeof(struct btrfs_list_filter);
	set = calloc(1, size);
	if (!set) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		exit(1);
	}

	set->total = BTRFS_LIST_NFILTERS_INCREASE;

	return set;
}

static struct btrfs_list_comparer_set *btrfs_list_alloc_comparer_set(void)
{
	struct btrfs_list_comparer_set *set;
	int size;

	size = sizeof(struct btrfs_list_comparer_set) +
	       BTRFS_LIST_NCOMPS_INCREASE * sizeof(struct btrfs_list_comparer);
	set = calloc(1, size);
	if (!set) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		exit(1);
	}

	set->total = BTRFS_LIST_NCOMPS_INCREASE;

	return set;
}

static int cmd_subvol_list(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_list_filter_set *filter_set;
	struct btrfs_list_comparer_set *comparer_set;
	u64 flags = 0;
	int fd = -1;
	u64 top_id;
	int ret = -1, uerr = 0;
	char *subvol;
	bool is_list_all = false;
	bool is_only_in_path = false;
	DIR *dirstream = NULL;
	enum btrfs_list_layout layout = BTRFS_LIST_LAYOUT_DEFAULT;

	filter_set = btrfs_list_alloc_filter_set();
	comparer_set = btrfs_list_alloc_comparer_set();

	optind = 0;
	while(1) {
		int c;
		static const struct option long_options[] = {
			{"sort", required_argument, NULL, 'S'},
			{NULL, 0, NULL, 0}
		};

		c = getopt_long(argc, argv,
				    "acdgopqsurRG:C:t", long_options, NULL);
		if (c < 0)
			break;

		switch(c) {
		case 'p':
			btrfs_list_setup_print_column(BTRFS_LIST_PARENT);
			break;
		case 'a':
			is_list_all = true;
			break;
		case 'c':
			btrfs_list_setup_print_column(BTRFS_LIST_OGENERATION);
			break;
		case 'd':
			btrfs_list_setup_filter(&filter_set,
						BTRFS_LIST_FILTER_DELETED,
						0);
			break;
		case 'g':
			btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
			break;
		case 'o':
			is_only_in_path = true;
			break;
		case 't':
			layout = BTRFS_LIST_LAYOUT_TABLE;
			break;
		case 's':
			btrfs_list_setup_filter(&filter_set,
						BTRFS_LIST_FILTER_SNAPSHOT_ONLY,
						0);
			btrfs_list_setup_print_column(BTRFS_LIST_OGENERATION);
			btrfs_list_setup_print_column(BTRFS_LIST_OTIME);
			break;
		case 'u':
			btrfs_list_setup_print_column(BTRFS_LIST_UUID);
			break;
		case 'q':
			btrfs_list_setup_print_column(BTRFS_LIST_PUUID);
			break;
		case 'R':
			btrfs_list_setup_print_column(BTRFS_LIST_RUUID);
			break;
		case 'r':
			flags |= BTRFS_ROOT_SUBVOL_RDONLY;
			break;
		case 'G':
			btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
			ret = btrfs_list_parse_filter_string(optarg,
							&filter_set,
							BTRFS_LIST_FILTER_GEN);
			if (ret) {
				uerr = 1;
				goto out;
			}
			break;

		case 'C':
			btrfs_list_setup_print_column(BTRFS_LIST_OGENERATION);
			ret = btrfs_list_parse_filter_string(optarg,
							&filter_set,
							BTRFS_LIST_FILTER_CGEN);
			if (ret) {
				uerr = 1;
				goto out;
			}
			break;
		case 'S':
			ret = btrfs_list_parse_sort_string(optarg,
							   &comparer_set);
			if (ret) {
				uerr = 1;
				goto out;
			}
			break;

		default:
			uerr = 1;
			goto out;
		}
	}

	if (check_argc_exact(argc - optind, 1))
		goto out;

	subvol = argv[optind];
	fd = btrfs_open_dir(subvol, &dirstream, 1);
	if (fd < 0) {
		ret = -1;
		error("can't access '%s'", subvol);
		goto out;
	}

	if (flags)
		btrfs_list_setup_filter(&filter_set, BTRFS_LIST_FILTER_FLAGS,
					flags);

	ret = lookup_path_rootid(fd, &top_id);
	if (ret) {
		errno = -ret;
		error("cannot resolve rootid for path: %m");
		goto out;
	}

	if (is_list_all)
		btrfs_list_setup_filter(&filter_set,
					BTRFS_LIST_FILTER_FULL_PATH,
					top_id);
	else if (is_only_in_path)
		btrfs_list_setup_filter(&filter_set,
					BTRFS_LIST_FILTER_TOPID_EQUAL,
					top_id);

	/* by default we shall print the following columns*/
	btrfs_list_setup_print_column(BTRFS_LIST_OBJECTID);
	btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
	btrfs_list_setup_print_column(BTRFS_LIST_TOP_LEVEL);
	btrfs_list_setup_print_column(BTRFS_LIST_PATH);

	ret = btrfs_list_subvols_print(fd, filter_set, comparer_set,
			layout, !is_list_all && !is_only_in_path, NULL);

out:
	close_file_or_dir(fd, dirstream);
	if (filter_set)
		free(filter_set);
	if (comparer_set)
		free(comparer_set);
	if (uerr)
		usage(cmd);
	return !!ret;
}
DEFINE_SIMPLE_COMMAND(subvol_list, "list");
