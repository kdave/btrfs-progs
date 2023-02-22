/*
 * Copyright (C) 2012 STRATO.  All rights reserved.
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
#include <getopt.h>
#include <errno.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "libbtrfsutil/btrfsutil.h"
#include "kernel-lib/list.h"
#include "kernel-lib/rbtree.h"
#include "kernel-lib/rbtree_types.h"
#include "kernel-shared/ctree.h"
#include "common/open-utils.h"
#include "common/utils.h"
#include "common/help.h"
#include "common/units.h"
#include "common/parse-utils.h"
#include "common/format-output.h"
#include "common/messages.h"
#include "cmds/commands.h"
#include "cmds/qgroup.h"
#include "ioctl.h"

#define BTRFS_QGROUP_NFILTERS_INCREASE (2 * BTRFS_QGROUP_FILTER_MAX)
#define BTRFS_QGROUP_NCOMPS_INCREASE (2 * BTRFS_QGROUP_COMP_MAX)

/*
 * glue structure to represent the relations
 * between qgroups
 */
struct btrfs_qgroup_list {
	struct list_head next_qgroup;
	struct list_head next_member;
	struct btrfs_qgroup *qgroup;
	struct btrfs_qgroup *member;
};

struct qgroup_lookup {
	struct rb_root root;
};

struct btrfs_qgroup {
	struct rb_node rb_node;
	struct rb_node sort_node;
	/*
	 *all_parent_node is used to
	 *filter a qgroup's all parent
	 */
	struct rb_node all_parent_node;
	u64 qgroupid;

	/* NULL for qgroups with level > 0 */
	const char *path;

	/*
	 * info_item
	 */
	struct btrfs_qgroup_info info;

	/*
	 *limit_item
	 */
	struct btrfs_qgroup_limit limit;

	/*qgroups this group is member of*/
	struct list_head qgroups;
	/*qgroups that are members of this group*/
	struct list_head members;
};

typedef int (*btrfs_qgroup_filter_func)(struct btrfs_qgroup *, u64);
typedef int (*btrfs_qgroup_comp_func)(struct btrfs_qgroup *,
				      struct btrfs_qgroup *, int);

struct btrfs_qgroup_filter {
	btrfs_qgroup_filter_func filter_func;
	u64 data;
};

struct btrfs_qgroup_comparer {
	btrfs_qgroup_comp_func comp_func;
	int is_descending;
};

struct btrfs_qgroup_filter_set {
	int total;
	int nfilters;
	struct btrfs_qgroup_filter filters[0];
};

struct btrfs_qgroup_comparer_set {
	int total;
	int ncomps;
	struct btrfs_qgroup_comparer comps[0];
};

enum btrfs_qgroup_column_enum {
	BTRFS_QGROUP_QGROUPID,
	BTRFS_QGROUP_RFER,
	BTRFS_QGROUP_EXCL,
	BTRFS_QGROUP_MAX_RFER,
	BTRFS_QGROUP_MAX_EXCL,
	BTRFS_QGROUP_PARENT,
	BTRFS_QGROUP_CHILD,
	BTRFS_QGROUP_PATH,
	BTRFS_QGROUP_ALL,
};

enum btrfs_qgroup_comp_enum {
	BTRFS_QGROUP_COMP_QGROUPID,
	BTRFS_QGROUP_COMP_PATH,
	BTRFS_QGROUP_COMP_RFER,
	BTRFS_QGROUP_COMP_EXCL,
	BTRFS_QGROUP_COMP_MAX_RFER,
	BTRFS_QGROUP_COMP_MAX_EXCL,
	BTRFS_QGROUP_COMP_MAX
};

enum btrfs_qgroup_filter_enum {
	BTRFS_QGROUP_FILTER_PARENT,
	BTRFS_QGROUP_FILTER_ALL_PARENT,
	BTRFS_QGROUP_FILTER_MAX,
};

/*
 * qgroupid,rfer,excl default to set
 */
static struct {
	char *name;
	char *column_name;
	int need_print;
	unsigned unit_mode;
	int max_len;
} btrfs_qgroup_columns[] = {
	{
		.name		= "qgroupid",
		.column_name	= "Qgroupid",
		.need_print	= 1,
		.unit_mode	= 0,
		.max_len	= 9,
	},
	{
		.name		= "rfer",
		.column_name	= "Referenced",
		.need_print	= 1,
		.unit_mode	= UNITS_DEFAULT,
		.max_len	= 12,
	},
	{
		.name		= "excl",
		.column_name	= "Exclusive",
		.need_print	= 1,
		.unit_mode	= UNITS_DEFAULT,
		.max_len	= 12,
	},
	{	.name		= "max_rfer",
		.column_name	= "Max referenced",
		.need_print	= 0,
		.unit_mode	= UNITS_DEFAULT,
		.max_len	= 15,
	},
	{
		.name		= "max_excl",
		.column_name	= "Max exclusive",
		.need_print	= 0,
		.unit_mode	= UNITS_DEFAULT,
		.max_len	= 14,
	},
	{
		.name		= "parent",
		.column_name	= "Parent",
		.need_print	= 0,
		.unit_mode	= 0,
		.max_len	= 8,
	},
	{
		.name		= "child",
		.column_name	= "Child",
		.need_print	= 0,
		.unit_mode	= 0,
		.max_len	= 5,
	},
	{
		.name		= "path",
		.column_name	= "Path",
		.need_print	= 1,
		.unit_mode	= 0,
		.max_len	= 6,
	},
	{
		.name		= NULL,
		.column_name	= NULL,
		.need_print	= 0,
		.unit_mode	= 0,
	},
};

static btrfs_qgroup_filter_func all_filter_funcs[];
static btrfs_qgroup_comp_func all_comp_funcs[];

static void qgroup_setup_print_column(enum btrfs_qgroup_column_enum column)
{
	int i;

	ASSERT(0 <= column && column <= BTRFS_QGROUP_ALL);

	if (column < BTRFS_QGROUP_ALL) {
		btrfs_qgroup_columns[column].need_print = 1;
		return;
	}
	for (i = 0; i < BTRFS_QGROUP_ALL; i++)
		btrfs_qgroup_columns[i].need_print = 1;
}

static void qgroup_setup_units(unsigned unit_mode)
{
	btrfs_qgroup_columns[BTRFS_QGROUP_RFER].unit_mode = unit_mode;
	btrfs_qgroup_columns[BTRFS_QGROUP_EXCL].unit_mode = unit_mode;
	btrfs_qgroup_columns[BTRFS_QGROUP_MAX_RFER].unit_mode = unit_mode;
	btrfs_qgroup_columns[BTRFS_QGROUP_MAX_EXCL].unit_mode = unit_mode;
}

static int print_parent_column(struct btrfs_qgroup *qgroup)
{
	struct btrfs_qgroup_list *list = NULL;
	int len = 0;

	list_for_each_entry(list, &qgroup->qgroups, next_qgroup) {
		len += printf("%u/%llu",
			      btrfs_qgroup_level(list->qgroup->qgroupid),
			      btrfs_qgroup_subvolid(list->qgroup->qgroupid));
		if (!list_is_last(&list->next_qgroup, &qgroup->qgroups))
			len += printf(",");
	}
	if (list_empty(&qgroup->qgroups))
		len += printf("-");

	return len;
}

static int print_child_column(struct btrfs_qgroup *qgroup)
{
	struct btrfs_qgroup_list *list = NULL;
	int len = 0;

	list_for_each_entry(list, &qgroup->members, next_member) {
		len += printf("%u/%llu",
			      btrfs_qgroup_level(list->member->qgroupid),
			      btrfs_qgroup_subvolid(list->member->qgroupid));
		if (!list_is_last(&list->next_member, &qgroup->members))
			len += printf(",");
	}
	if (list_empty(&qgroup->members))
		len += printf("-");

	return len;
}

static int print_u64(u64 value, int unit_mode, int max_len)
{
	return printf("%*s", max_len, pretty_size_mode(value, unit_mode));
}

static void print_qgroup_column_add_blank(enum btrfs_qgroup_column_enum column,
					  int len)
{
	len = btrfs_qgroup_columns[column].max_len - len;
	while (len--)
		printf(" ");
}

void print_path_column(struct btrfs_qgroup *qgroup)
{
	struct btrfs_qgroup_list *list = NULL;

	pr_verbose(LOG_DEFAULT, "  ");
	if (btrfs_qgroup_level(qgroup->qgroupid) > 0) {
		int count = 0;

		list_for_each_entry(list, &qgroup->qgroups, next_qgroup) {
			struct btrfs_qgroup *member = list->qgroup;
			u64 qgroupid = member->qgroupid;
			u64 level = btrfs_qgroup_level(qgroupid);
			u64 sid = btrfs_qgroup_subvolid(qgroupid);

			if (count)
				pr_verbose(LOG_DEFAULT, " ");
			if (level == 0) {
				const char *path = member->path;

				if (!path)
					path = "<stale>";
				pr_verbose(LOG_DEFAULT, "%s", path);
			} else {
				pr_verbose(LOG_DEFAULT, "%llu/%llu", level, sid);
			}
			count++;
		}
		pr_verbose(LOG_DEFAULT, "<%u member qgroup%c>", count,
			       (count != 1 ? 's' : '\0'));
	} else if (qgroup->path) {
		pr_verbose(LOG_DEFAULT, "%s%s", (*qgroup->path ? "" : "<toplevel>"), qgroup->path);
	} else {
		pr_verbose(LOG_DEFAULT, "<stale>");
	}
}

static void print_qgroup_column(struct btrfs_qgroup *qgroup,
				enum btrfs_qgroup_column_enum column)
{
	int len;
	int unit_mode = btrfs_qgroup_columns[column].unit_mode;
	int max_len = btrfs_qgroup_columns[column].max_len;

	ASSERT(0 <= column && column < BTRFS_QGROUP_ALL);

	switch (column) {

	case BTRFS_QGROUP_QGROUPID:
		len = printf("%u/%llu",
			     btrfs_qgroup_level(qgroup->qgroupid),
			     btrfs_qgroup_subvolid(qgroup->qgroupid));
		print_qgroup_column_add_blank(BTRFS_QGROUP_QGROUPID, len);
		break;
	case BTRFS_QGROUP_RFER:
		len = print_u64(qgroup->info.referenced, unit_mode, max_len);
		break;
	case BTRFS_QGROUP_EXCL:
		len = print_u64(qgroup->info.exclusive, unit_mode, max_len);
		break;
	case BTRFS_QGROUP_PARENT:
		len = print_parent_column(qgroup);
		print_qgroup_column_add_blank(BTRFS_QGROUP_PARENT, len);
		break;
	case BTRFS_QGROUP_MAX_RFER:
		if (qgroup->limit.flags & BTRFS_QGROUP_LIMIT_MAX_RFER)
			len = print_u64(qgroup->limit.max_referenced,
					unit_mode, max_len);
		else
			len = printf("%*s", max_len, "none");
		break;
	case BTRFS_QGROUP_MAX_EXCL:
		if (qgroup->limit.flags & BTRFS_QGROUP_LIMIT_MAX_EXCL)
			len = print_u64(qgroup->limit.max_exclusive,
					unit_mode, max_len);
		else
			len = printf("%*s", max_len, "none");
		break;
	case BTRFS_QGROUP_CHILD:
		len = print_child_column(qgroup);
		print_qgroup_column_add_blank(BTRFS_QGROUP_CHILD, len);
		break;
	case BTRFS_QGROUP_PATH:
		print_path_column(qgroup);
		break;
	default:
		break;
	}
}

static void print_single_qgroup_table(struct btrfs_qgroup *qgroup)
{
	int i;

	for (i = 0; i < BTRFS_QGROUP_ALL; i++) {
		if (!btrfs_qgroup_columns[i].need_print)
			continue;
		print_qgroup_column(qgroup, i);

		if (i != BTRFS_QGROUP_ALL - 1)
			printf(" ");
	}
	printf("\n");
}

static void print_table_head(void)
{
	int i;
	int len;
	int max_len;

	for (i = 0; i < BTRFS_QGROUP_ALL; i++) {
		max_len = btrfs_qgroup_columns[i].max_len;
		if (!btrfs_qgroup_columns[i].need_print)
			continue;
		if ((i == BTRFS_QGROUP_QGROUPID) | (i == BTRFS_QGROUP_PARENT) |
			(i == BTRFS_QGROUP_CHILD))
			printf("%-*s", max_len, btrfs_qgroup_columns[i].column_name);
		else
			printf("%*s", max_len, btrfs_qgroup_columns[i].column_name);
		printf(" ");
	}
	printf("\n");
	for (i = 0; i < BTRFS_QGROUP_ALL; i++) {
		max_len = btrfs_qgroup_columns[i].max_len;
		if (!btrfs_qgroup_columns[i].need_print)
			continue;
		if ((i == BTRFS_QGROUP_QGROUPID) | (i == BTRFS_QGROUP_PARENT) |
			(i == BTRFS_QGROUP_CHILD)) {
			len = strlen(btrfs_qgroup_columns[i].column_name);
			while (len--)
				printf("-");
			len = max_len - strlen(btrfs_qgroup_columns[i].column_name);
			while (len--)
				printf(" ");
		} else {
			len = max_len - strlen(btrfs_qgroup_columns[i].column_name);
			while (len--)
				printf(" ");
			len = strlen(btrfs_qgroup_columns[i].column_name);
			while (len--)
				printf("-");
		}
		printf(" ");
	}
	printf("\n");
}

static void qgroup_lookup_init(struct qgroup_lookup *tree)
{
	tree->root.rb_node = NULL;
}

static int comp_entry_with_qgroupid(struct btrfs_qgroup *entry1,
				    struct btrfs_qgroup *entry2,
				    int is_descending)
{

	int ret;

	if (entry1->qgroupid > entry2->qgroupid)
		ret = 1;
	else if (entry1->qgroupid < entry2->qgroupid)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

/* Sorts first-level qgroups by path and nested qgroups by qgroupid */
static int comp_entry_with_path(struct btrfs_qgroup *entry1,
				    struct btrfs_qgroup *entry2,
				    int is_descending)
{
	int ret = 0;
	const char *p1 = entry1->path;
	const char *p2 = entry2->path;
	u64 level1 = btrfs_qgroup_level(entry1->qgroupid);
	u64 level2 = btrfs_qgroup_level(entry2->qgroupid);

	if (level1 != level2) {
		if (entry1->qgroupid > entry2->qgroupid)
			ret = 1;
		else if (entry1->qgroupid < entry2->qgroupid)
			ret = -1;
	}

	if (ret)
		goto out;

	while (*p1 && *p2) {
		if (*p1 != *p2)
			break;
		p1++;
		p2++;
	}

	if (*p1 == '/')
		ret = 1;
	else if (*p2 == '/')
		ret = -1;
	else if (*p1 > *p2)
		ret = 1;
	else if (*p1 < *p2)
		ret = -1;
out:
	return (is_descending ? -ret : ret);
}

static int comp_entry_with_rfer(struct btrfs_qgroup *entry1,
				struct btrfs_qgroup *entry2,
				int is_descending)
{
	int ret;

	if (entry1->info.referenced > entry2->info.referenced)
		ret = 1;
	else if (entry1->info.referenced < entry2->info.referenced)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static int comp_entry_with_excl(struct btrfs_qgroup *entry1,
				struct btrfs_qgroup *entry2,
				int is_descending)
{
	int ret;

	if (entry1->info.exclusive > entry2->info.exclusive)
		ret = 1;
	else if (entry1->info.exclusive < entry2->info.exclusive)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static int comp_entry_with_max_rfer(struct btrfs_qgroup *entry1,
				    struct btrfs_qgroup *entry2,
				    int is_descending)
{
	int ret;

	if (entry1->limit.max_referenced > entry2->limit.max_referenced)
		ret = 1;
	else if (entry1->limit.max_referenced < entry2->limit.max_referenced)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static int comp_entry_with_max_excl(struct btrfs_qgroup *entry1,
				    struct btrfs_qgroup *entry2,
				    int is_descending)
{
	int ret;

	if (entry1->limit.max_exclusive > entry2->limit.max_exclusive)
		ret = 1;
	else if (entry1->limit.max_exclusive < entry2->limit.max_exclusive)
		ret = -1;
	else
		ret = 0;

	return is_descending ? -ret : ret;
}

static btrfs_qgroup_comp_func all_comp_funcs[] = {
	[BTRFS_QGROUP_COMP_QGROUPID]	= comp_entry_with_qgroupid,
	[BTRFS_QGROUP_COMP_PATH]	= comp_entry_with_path,
	[BTRFS_QGROUP_COMP_RFER]	= comp_entry_with_rfer,
	[BTRFS_QGROUP_COMP_EXCL]	= comp_entry_with_excl,
	[BTRFS_QGROUP_COMP_MAX_RFER]	= comp_entry_with_max_rfer,
	[BTRFS_QGROUP_COMP_MAX_EXCL]	= comp_entry_with_max_excl
};

static char *all_sort_items[] = {
	[BTRFS_QGROUP_COMP_QGROUPID]	= "qgroupid",
	[BTRFS_QGROUP_COMP_PATH]	= "path",
	[BTRFS_QGROUP_COMP_RFER]	= "rfer",
	[BTRFS_QGROUP_COMP_EXCL]	= "excl",
	[BTRFS_QGROUP_COMP_MAX_RFER]	= "max_rfer",
	[BTRFS_QGROUP_COMP_MAX_EXCL]	= "max_excl",
	[BTRFS_QGROUP_COMP_MAX]		= NULL,
};

static int qgroup_get_sort_item(char *sort_name)
{
	int i;

	for (i = 0; i < BTRFS_QGROUP_COMP_MAX; i++) {
		if (strcmp(sort_name, all_sort_items[i]) == 0)
			return i;
	}
	return -1;
}

static struct btrfs_qgroup_comparer_set *qgroup_alloc_comparer_set(void)
{
	struct btrfs_qgroup_comparer_set *set;
	int size;
	size = sizeof(struct btrfs_qgroup_comparer_set) +
	       BTRFS_QGROUP_NCOMPS_INCREASE *
	       sizeof(struct btrfs_qgroup_comparer);
	set = calloc(1, size);
	if (!set) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		exit(1);
	}

	set->total = BTRFS_QGROUP_NCOMPS_INCREASE;

	return set;
}

static int qgroup_setup_comparer(struct btrfs_qgroup_comparer_set **comp_set,
				enum btrfs_qgroup_comp_enum comparer,
				int is_descending)
{
	struct btrfs_qgroup_comparer_set *set = *comp_set;
	int size;

	ASSERT(set != NULL);
	ASSERT(comparer < BTRFS_QGROUP_COMP_MAX);
	ASSERT(set->ncomps <= set->total);

	if (set->ncomps == set->total) {
		void *tmp;

		size = set->total + BTRFS_QGROUP_NCOMPS_INCREASE;
		size = sizeof(*set) +
		       size * sizeof(struct btrfs_qgroup_comparer);
		tmp = set;
		set = realloc(set, size);
		if (!set) {
			free(tmp);
			return -ENOMEM;
		}

		memset(&set->comps[set->total], 0,
		       BTRFS_QGROUP_NCOMPS_INCREASE *
		       sizeof(struct btrfs_qgroup_comparer));
		set->total += BTRFS_QGROUP_NCOMPS_INCREASE;
		*comp_set = set;
	}

	ASSERT(set->comps[set->ncomps].comp_func == NULL);

	set->comps[set->ncomps].comp_func = all_comp_funcs[comparer];
	set->comps[set->ncomps].is_descending = is_descending;
	set->ncomps++;
	return 0;
}

static int sort_comp(struct btrfs_qgroup *entry1, struct btrfs_qgroup *entry2,
		     struct btrfs_qgroup_comparer_set *set)
{
	bool qgroupid_compared = false;
	int i, ret = 0;

	if (!set || !set->ncomps)
		goto comp_qgroupid;

	for (i = 0; i < set->ncomps; i++) {
		if (!set->comps[i].comp_func)
			break;

		ret = set->comps[i].comp_func(entry1, entry2,
					      set->comps[i].is_descending);
		if (ret)
			return ret;

		if (set->comps[i].comp_func == comp_entry_with_qgroupid)
			qgroupid_compared = true;
	}

	if (!qgroupid_compared) {
comp_qgroupid:
		ret = comp_entry_with_qgroupid(entry1, entry2, 0);
	}

	return ret;
}

/*
 * insert a new root into the tree.  returns the existing root entry
 * if one is already there.  qgroupid is used
 * as the key
 */
static int qgroup_tree_insert(struct qgroup_lookup *root_tree,
			      struct btrfs_qgroup *ins)
{

	struct rb_node **p = &root_tree->root.rb_node;
	struct rb_node *parent = NULL;
	struct btrfs_qgroup *curr;
	int ret;

	while (*p) {
		parent = *p;
		curr = rb_entry(parent, struct btrfs_qgroup, rb_node);

		ret = comp_entry_with_qgroupid(ins, curr, 0);
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
 *find a given qgroupid in the tree. We return the smallest one,
 *rb_next can be used to move forward looking for more if required
 */
static struct btrfs_qgroup *qgroup_tree_search(struct qgroup_lookup *root_tree,
					       u64 qgroupid)
{
	struct rb_node *n = root_tree->root.rb_node;
	struct btrfs_qgroup *entry;
	struct btrfs_qgroup tmp;
	int ret;

	tmp.qgroupid = qgroupid;

	while (n) {
		entry = rb_entry(n, struct btrfs_qgroup, rb_node);

		ret = comp_entry_with_qgroupid(&tmp, entry, 0);
		if (ret < 0)
			n = n->rb_left;
		else if (ret > 0)
			n = n->rb_right;
		else
			return entry;

	}
	return NULL;
}

/*
 * Lookup or insert btrfs_qgroup into qgroup_lookup.
 *
 * Search a btrfs_qgroup with @qgroupid from the @qgroup_lookup. If not found,
 * initialize a btrfs_qgroup with the given qgroupid and insert it to the
 * @qgroup_lookup.
 *
 * Return the pointer to the btrfs_qgroup if found or if inserted successfully.
 * Return ERR_PTR if any error occurred.
 */
static struct btrfs_qgroup *get_or_add_qgroup(int fd,
		struct qgroup_lookup *qgroup_lookup, u64 qgroupid)
{
	struct btrfs_qgroup *bq;
	int ret;

	bq = qgroup_tree_search(qgroup_lookup, qgroupid);
	if (bq)
		return bq;

	bq = calloc(1, sizeof(*bq));
	if (!bq) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		return ERR_PTR(-ENOMEM);
	}

	bq->qgroupid = qgroupid;
	INIT_LIST_HEAD(&bq->qgroups);
	INIT_LIST_HEAD(&bq->members);

	if (btrfs_qgroup_level(qgroupid) == 0) {
		enum btrfs_util_error uret;
		char *path;

		uret = btrfs_util_subvolume_path_fd(fd, qgroupid, &path);
		if (uret == BTRFS_UTIL_OK)
			bq->path = path;
		/* Ignore stale qgroup items */
		else if (uret != BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND) {
			error("%s", btrfs_util_strerror(uret));
			if (uret == BTRFS_UTIL_ERROR_NO_MEMORY)
				return ERR_PTR(-ENOMEM);
			else
				return ERR_PTR(-EIO);
		}
	}

	ret = qgroup_tree_insert(qgroup_lookup, bq);
	if (ret) {
		errno = -ret;
		error("failed to insert %llu into tree: %m", bq->qgroupid);
		free(bq);
		return ERR_PTR(ret);
	}

	return bq;
}

static int update_qgroup_info(int fd, struct qgroup_lookup *qgroup_lookup, u64 qgroupid,
			      struct btrfs_qgroup_info_item *info)
{
	struct btrfs_qgroup *bq;

	bq = get_or_add_qgroup(fd, qgroup_lookup, qgroupid);
	if (IS_ERR_OR_NULL(bq))
		return PTR_ERR(bq);

	bq->info.generation = btrfs_stack_qgroup_info_generation(info);
	bq->info.referenced = btrfs_stack_qgroup_info_referenced(info);
	bq->info.referenced_compressed =
			btrfs_stack_qgroup_info_referenced_compressed(info);
	bq->info.exclusive = btrfs_stack_qgroup_info_exclusive(info);
	bq->info.exclusive_compressed =
			btrfs_stack_qgroup_info_exclusive_compressed(info);

	return 0;
}

static int update_qgroup_limit(int fd, struct qgroup_lookup *qgroup_lookup,
			       u64 qgroupid,
			       struct btrfs_qgroup_limit_item *limit)
{
	struct btrfs_qgroup *bq;

	bq = get_or_add_qgroup(fd, qgroup_lookup, qgroupid);
	if (IS_ERR_OR_NULL(bq))
		return PTR_ERR(bq);

	bq->limit.flags = btrfs_stack_qgroup_limit_flags(limit);
	bq->limit.max_referenced =
			btrfs_stack_qgroup_limit_max_referenced(limit);
	bq->limit.max_exclusive =
			btrfs_stack_qgroup_limit_max_exclusive(limit);
	bq->limit.rsv_referenced =
			btrfs_stack_qgroup_limit_rsv_referenced(limit);
	bq->limit.rsv_exclusive = btrfs_stack_qgroup_limit_rsv_exclusive(limit);

	return 0;
}

static int update_qgroup_relation(struct qgroup_lookup *qgroup_lookup,
				  u64 child_id, u64 parent_id)
{
	struct btrfs_qgroup *child;
	struct btrfs_qgroup *parent;
	struct btrfs_qgroup_list *list;

	child = qgroup_tree_search(qgroup_lookup, child_id);
	if (!child) {
		error("cannot find the qgroup %u/%llu",
		      btrfs_qgroup_level(child_id),
		      btrfs_qgroup_subvolid(child_id));
		return -ENOENT;
	}

	parent = qgroup_tree_search(qgroup_lookup, parent_id);
	if (!parent) {
		error("cannot find the qgroup %u/%llu",
		      btrfs_qgroup_level(parent_id),
		      btrfs_qgroup_subvolid(parent_id));
		return -ENOENT;
	}

	list = malloc(sizeof(*list));
	if (!list) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		return -ENOMEM;
	}

	list->qgroup = parent;
	list->member = child;
	list_add_tail(&list->next_qgroup, &child->qgroups);
	list_add_tail(&list->next_member, &parent->members);

	return 0;
}

static void __free_btrfs_qgroup(struct btrfs_qgroup *bq)
{
	struct btrfs_qgroup_list *list;
	while (!list_empty(&bq->qgroups)) {
		list = list_entry((&bq->qgroups)->next,
				  struct btrfs_qgroup_list,
				  next_qgroup);
		list_del(&list->next_qgroup);
		list_del(&list->next_member);
		free(list);
	}
	while (!list_empty(&bq->members)) {
		list = list_entry((&bq->members)->next,
				  struct btrfs_qgroup_list,
				  next_member);
		list_del(&list->next_qgroup);
		list_del(&list->next_member);
		free(list);
	}
	if (bq->path)
		free((void *)bq->path);
	free(bq);
}

static void __free_all_qgroups(struct qgroup_lookup *root_tree)
{
	struct btrfs_qgroup *entry;
	struct rb_node *n;

	n = rb_first(&root_tree->root);
	while (n) {
		entry = rb_entry(n, struct btrfs_qgroup, rb_node);
		rb_erase(n, &root_tree->root);
		__free_btrfs_qgroup(entry);

		n = rb_first(&root_tree->root);
	}
}

static int filter_all_parent_insert(struct qgroup_lookup *sort_tree,
				    struct btrfs_qgroup *bq)
{
	struct rb_node **p = &sort_tree->root.rb_node;
	struct rb_node *parent = NULL;
	struct btrfs_qgroup *curr;
	int ret;

	while (*p) {
		parent = *p;
		curr = rb_entry(parent, struct btrfs_qgroup, all_parent_node);

		ret = comp_entry_with_qgroupid(bq, curr, 0);
		if (ret < 0)
			p = &(*p)->rb_left;
		else if (ret > 0)
			p = &(*p)->rb_right;
		else
			return -EEXIST;
	}
	rb_link_node(&bq->all_parent_node, parent, p);
	rb_insert_color(&bq->all_parent_node, &sort_tree->root);
	return 0;
}

static int filter_by_parent(struct btrfs_qgroup *bq, u64 data)
{
	struct btrfs_qgroup *qgroup =
		(struct btrfs_qgroup *)(unsigned long)data;

	if (data == 0)
		return 0;
	if (qgroup->qgroupid == bq->qgroupid)
		return 1;
	return 0;
}

static int filter_by_all_parent(struct btrfs_qgroup *bq, u64 data)
{
	struct qgroup_lookup lookup;
	struct qgroup_lookup *ql = &lookup;
	struct btrfs_qgroup_list *list;
	struct rb_node *n;
	struct btrfs_qgroup *qgroup =
			 (struct btrfs_qgroup *)(unsigned long)data;

	if (data == 0)
		return 0;
	if (bq->qgroupid == qgroup->qgroupid)
		return 1;

	qgroup_lookup_init(ql);
	filter_all_parent_insert(ql, qgroup);
	n = rb_first(&ql->root);
	while (n) {
		qgroup = rb_entry(n, struct btrfs_qgroup, all_parent_node);
		if (!list_empty(&qgroup->qgroups)) {
			list_for_each_entry(list, &qgroup->qgroups,
					    next_qgroup) {
				if ((list->qgroup)->qgroupid == bq->qgroupid)
					return 1;
				filter_all_parent_insert(ql, list->qgroup);
			}
		}
		rb_erase(n, &ql->root);
		n = rb_first(&ql->root);
	}
	return 0;
}

static btrfs_qgroup_filter_func all_filter_funcs[] = {
	[BTRFS_QGROUP_FILTER_PARENT]		= filter_by_parent,
	[BTRFS_QGROUP_FILTER_ALL_PARENT]	= filter_by_all_parent,
};

static struct btrfs_qgroup_filter_set *qgroup_alloc_filter_set(void)
{
	struct btrfs_qgroup_filter_set *set;
	int size;

	size = sizeof(struct btrfs_qgroup_filter_set) +
	       BTRFS_QGROUP_NFILTERS_INCREASE *
	       sizeof(struct btrfs_qgroup_filter);
	set = calloc(1, size);
	if (!set) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		exit(1);
	}
	set->total = BTRFS_QGROUP_NFILTERS_INCREASE;

	return set;
}

static int qgroup_setup_filter(struct btrfs_qgroup_filter_set **filter_set,
			      enum btrfs_qgroup_filter_enum filter, u64 data)
{
	struct btrfs_qgroup_filter_set *set = *filter_set;
	int size;

	ASSERT(set != NULL);
	ASSERT(filter < BTRFS_QGROUP_FILTER_MAX);
	ASSERT(set->nfilters <= set->total);

	if (set->nfilters == set->total) {
		void *tmp;

		size = set->total + BTRFS_QGROUP_NFILTERS_INCREASE;
		size = sizeof(*set) + size * sizeof(struct btrfs_qgroup_filter);

		tmp = set;
		set = realloc(set, size);
		if (!set) {
			error_msg(ERROR_MSG_MEMORY, NULL);
			free(tmp);
			exit(1);
		}
		memset(&set->filters[set->total], 0,
		       BTRFS_QGROUP_NFILTERS_INCREASE *
		       sizeof(struct btrfs_qgroup_filter));
		set->total += BTRFS_QGROUP_NFILTERS_INCREASE;
		*filter_set = set;
	}

	ASSERT(set->filters[set->nfilters].filter_func == NULL);
	set->filters[set->nfilters].filter_func = all_filter_funcs[filter];
	set->filters[set->nfilters].data = data;
	set->nfilters++;
	return 0;
}

static int filter_qgroup(struct btrfs_qgroup *bq,
			 struct btrfs_qgroup_filter_set *set)
{
	int i, ret;

	if (!set || !set->nfilters)
		return 1;
	for (i = 0; i < set->nfilters; i++) {
		if (!set->filters[i].filter_func)
			break;
		ret = set->filters[i].filter_func(bq, set->filters[i].data);
		if (!ret)
			return 0;
	}
	return 1;
}

static void pre_process_filter_set(struct qgroup_lookup *lookup,
				   struct btrfs_qgroup_filter_set *set)
{
	int i;
	struct btrfs_qgroup *qgroup_for_filter = NULL;

	for (i = 0; i < set->nfilters; i++) {

		if (set->filters[i].filter_func == filter_by_all_parent
		    || set->filters[i].filter_func == filter_by_parent) {
			qgroup_for_filter = qgroup_tree_search(lookup,
					    set->filters[i].data);
			set->filters[i].data =
				 (u64)(unsigned long)qgroup_for_filter;
		}
	}
}

static int sort_tree_insert(struct qgroup_lookup *sort_tree,
			    struct btrfs_qgroup *bq,
			    struct btrfs_qgroup_comparer_set *comp_set)
{
	struct rb_node **p = &sort_tree->root.rb_node;
	struct rb_node *parent = NULL;
	struct btrfs_qgroup *curr;
	int ret;

	while (*p) {
		parent = *p;
		curr = rb_entry(parent, struct btrfs_qgroup, sort_node);

		ret = sort_comp(bq, curr, comp_set);
		if (ret < 0)
			p = &(*p)->rb_left;
		else if (ret > 0)
			p = &(*p)->rb_right;
		else
			return -EEXIST;
	}
	rb_link_node(&bq->sort_node, parent, p);
	rb_insert_color(&bq->sort_node, &sort_tree->root);
	return 0;
}

static void __update_columns_max_len(struct btrfs_qgroup *bq,
				     enum btrfs_qgroup_column_enum column)
{
	struct btrfs_qgroup_list *list = NULL;
	char tmp[100];
	int len;
	unsigned unit_mode = btrfs_qgroup_columns[column].unit_mode;

	ASSERT(0 <= column && column < BTRFS_QGROUP_ALL);

	switch (column) {

	case BTRFS_QGROUP_QGROUPID:
		sprintf(tmp, "%u/%llu",
			btrfs_qgroup_level(bq->qgroupid),
			btrfs_qgroup_subvolid(bq->qgroupid));
		len = strlen(tmp);
		if (btrfs_qgroup_columns[column].max_len < len)
			btrfs_qgroup_columns[column].max_len = len;
		break;
	case BTRFS_QGROUP_RFER:
		len = strlen(pretty_size_mode(bq->info.referenced, unit_mode));
		if (btrfs_qgroup_columns[column].max_len < len)
			btrfs_qgroup_columns[column].max_len = len;
		break;
	case BTRFS_QGROUP_EXCL:
		len = strlen(pretty_size_mode(bq->info.exclusive, unit_mode));
		if (btrfs_qgroup_columns[column].max_len < len)
			btrfs_qgroup_columns[column].max_len = len;
		break;
	case BTRFS_QGROUP_MAX_RFER:
		len = strlen(pretty_size_mode(bq->limit.max_referenced,
			     unit_mode));
		if (btrfs_qgroup_columns[column].max_len < len)
			btrfs_qgroup_columns[column].max_len = len;
		break;
	case BTRFS_QGROUP_MAX_EXCL:
		len = strlen(pretty_size_mode(bq->limit.max_exclusive,
			     unit_mode));
		if (btrfs_qgroup_columns[column].max_len < len)
			btrfs_qgroup_columns[column].max_len = len;
		break;
	case BTRFS_QGROUP_PARENT:
		len = 0;
		list_for_each_entry(list, &bq->qgroups, next_qgroup) {
			len += sprintf(tmp, "%u/%llu",
				btrfs_qgroup_level(list->qgroup->qgroupid),
				btrfs_qgroup_subvolid(list->qgroup->qgroupid));
			if (!list_is_last(&list->next_qgroup, &bq->qgroups))
				len += 1;
		}
		if (btrfs_qgroup_columns[column].max_len < len)
			btrfs_qgroup_columns[column].max_len = len;
		break;
	case BTRFS_QGROUP_CHILD:
		len = 0;
		list_for_each_entry(list, &bq->members, next_member) {
			len += sprintf(tmp, "%u/%llu",
				btrfs_qgroup_level(list->member->qgroupid),
				btrfs_qgroup_subvolid(list->member->qgroupid));
			if (!list_is_last(&list->next_member, &bq->members))
				len += 1;
		}
		if (btrfs_qgroup_columns[column].max_len < len)
			btrfs_qgroup_columns[column].max_len = len;
		break;
	default:
		break;
	}

}

static void update_columns_max_len(struct btrfs_qgroup *bq)
{
	int i;

	for (i = 0; i < BTRFS_QGROUP_ALL; i++) {
		if (!btrfs_qgroup_columns[i].need_print)
			continue;
		__update_columns_max_len(bq, i);
	}
}

static void __filter_and_sort_qgroups(struct qgroup_lookup *all_qgroups,
				 struct qgroup_lookup *sort_tree,
				 struct btrfs_qgroup_filter_set *filter_set,
				 struct btrfs_qgroup_comparer_set *comp_set)
{
	struct rb_node *n;
	struct btrfs_qgroup *entry;
	int ret;

	qgroup_lookup_init(sort_tree);
	pre_process_filter_set(all_qgroups, filter_set);

	n = rb_last(&all_qgroups->root);
	while (n) {
		entry = rb_entry(n, struct btrfs_qgroup, rb_node);

		ret = filter_qgroup(entry, filter_set);
		if (ret) {
			sort_tree_insert(sort_tree, entry, comp_set);

			update_columns_max_len(entry);
		}
		n = rb_prev(n);
	}
}

static inline void print_status_flag_warning(u64 flags)
{
	if (!(flags & BTRFS_QGROUP_STATUS_FLAG_ON))
		warning("quota disabled, qgroup data may be out of date");
	else if (flags & BTRFS_QGROUP_STATUS_FLAG_RESCAN)
		warning("rescan is running, qgroup data may be incorrect");
	else if (flags & BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT)
		warning("qgroup data inconsistent, rescan recommended");
}

static bool key_in_range(const struct btrfs_key *key,
			 const struct btrfs_ioctl_search_key *sk)
{
	if (key->objectid < sk->min_objectid ||
	    key->objectid > sk->max_objectid)
		return false;

	if (key->type < sk->min_type ||
	    key->type > sk->max_type)
		return false;

	if (key->offset < sk->min_offset ||
	    key->offset > sk->max_offset)
		return false;

	return true;
}

static int __qgroups_search(int fd, struct btrfs_ioctl_search_args *args,
			    struct qgroup_lookup *qgroup_lookup)
{
	int ret;
	struct btrfs_ioctl_search_key *sk = &args->key;
	struct btrfs_ioctl_search_key filter_key = args->key;
	struct btrfs_ioctl_search_header *sh;
	unsigned long off = 0;
	unsigned int i;
	struct btrfs_qgroup_status_item *si;
	struct btrfs_qgroup_info_item *info;
	struct btrfs_qgroup_limit_item *limit;
	u64 flags;
	u64 qgroupid;
	u64 child, parent;

	qgroup_lookup_init(qgroup_lookup);

	while (1) {
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, args);
		if (ret < 0) {
			if (errno == ENOENT)
				ret = -ENOTTY;
			else
				ret = -errno;
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
			struct btrfs_key key;

			sh = (struct btrfs_ioctl_search_header *)(args->buf +
								  off);
			off += sizeof(*sh);

			key.objectid = btrfs_search_header_objectid(sh);
			key.type = btrfs_search_header_type(sh);
			key.offset = btrfs_search_header_offset(sh);

			if (!key_in_range(&key, &filter_key))
				goto next;

			switch (key.type) {
			case BTRFS_QGROUP_STATUS_KEY:
				si = (struct btrfs_qgroup_status_item *)
				     (args->buf + off);
				flags = btrfs_stack_qgroup_status_flags(si);

				print_status_flag_warning(flags);
				break;
			case BTRFS_QGROUP_INFO_KEY:
				qgroupid = key.offset;
				info = (struct btrfs_qgroup_info_item *)
				       (args->buf + off);

				ret = update_qgroup_info(fd, qgroup_lookup,
							 qgroupid, info);
				break;
			case BTRFS_QGROUP_LIMIT_KEY:
				qgroupid = key.offset;
				limit = (struct btrfs_qgroup_limit_item *)
					(args->buf + off);

				ret = update_qgroup_limit(fd, qgroup_lookup,
							  qgroupid, limit);
				break;
			case BTRFS_QGROUP_RELATION_KEY:
				child = key.offset;
				parent = key.objectid;

				if (parent <= child)
					break;

				ret = update_qgroup_relation(qgroup_lookup,
							     child, parent);
				break;
			default:
				return ret;
			}

			if (ret)
				return ret;

next:
			off += btrfs_search_header_len(sh);

			/*
			 * record the mins in sk so we can make sure the
			 * next search doesn't repeat this root
			 */
			sk->min_type = key.type;
			sk->min_offset = key.offset;
			sk->min_objectid = key.objectid;
		}
		sk->nr_items = 4096;
		/*
		 * this iteration is done, step forward one qgroup for the next
		 * ioctl
		 */
		if (sk->min_offset < (u64)-1)
			sk->min_offset++;
		else
			break;
	}

	return ret;
}

static int qgroups_search_all(int fd, struct qgroup_lookup *qgroup_lookup)
{
	struct btrfs_ioctl_search_args args = {
		.key = {
			.tree_id = BTRFS_QUOTA_TREE_OBJECTID,
			.max_type = BTRFS_QGROUP_RELATION_KEY,
			.min_type = BTRFS_QGROUP_STATUS_KEY,
			.max_objectid = (u64)-1,
			.max_offset = (u64)-1,
			.max_transid = (u64)-1,
			.nr_items = 4096,
		},
	};
	int ret;

	ret = __qgroups_search(fd, &args, qgroup_lookup);
	if (ret == -ENOTTY) {
		error("can't list qgroups: quotas not enabled");
	} else if (ret < 0) {
		errno = -ret;
		error("can't list qgroups: %m");
	}
	return ret;
}

int btrfs_qgroup_query(int fd, u64 qgroupid, struct btrfs_qgroup_stats *stats)
{
	struct btrfs_ioctl_search_args args = {
		.key = {
			.tree_id = BTRFS_QUOTA_TREE_OBJECTID,
			.min_type = BTRFS_QGROUP_INFO_KEY,
			.max_type = BTRFS_QGROUP_LIMIT_KEY,
			.max_objectid = 0,
			.min_offset = qgroupid,
			.max_offset = qgroupid,
			.max_transid = (u64)-1,
			.nr_items = 4096,
		},
	};
	struct qgroup_lookup qgroup_lookup;
	struct btrfs_qgroup *qgroup;
	struct rb_node *n;
	int ret;

	ret = __qgroups_search(fd, &args, &qgroup_lookup);
	if (ret < 0)
		return ret;

	ret = -ENODATA;
	n = rb_first(&qgroup_lookup.root);
	if (n) {
		qgroup = rb_entry(n, struct btrfs_qgroup, rb_node);
		stats->qgroupid = qgroup->qgroupid;
		stats->info = qgroup->info;
		stats->limit = qgroup->limit;

		ret = 0;
	}

	__free_all_qgroups(&qgroup_lookup);
	return ret;
}

static void print_all_qgroups(struct qgroup_lookup *qgroup_lookup)
{

	struct rb_node *n;
	struct btrfs_qgroup *entry;

	print_table_head();

	n = rb_first(&qgroup_lookup->root);
	while (n) {
		entry = rb_entry(n, struct btrfs_qgroup, sort_node);
		print_single_qgroup_table(entry);
		n = rb_next(n);
	}
}

static const struct rowspec qgroup_show_rowspec[] = {
	{ .key = "qgroupid", .fmt = "qgroupid", .out_json = "qgroupid" },
	{ .key = "referenced", .fmt = "%llu", .out_json = "referenced" },
	{ .key = "exclusive", .fmt = "%llu", .out_json = "exclusive" },
	{ .key = "max_referenced", .fmt = "size", .out_json = "max_referenced" },
	/* Special value if limits not set. */
	{ .key = "max_referenced-none", .fmt = "%s", .out_json = "max_referenced" },
	{ .key = "max_exclusive", .fmt = "size", .out_json = "max_exclusive" },
	/* Special value if limits not set. */
	{ .key = "max_exclusive-none", .fmt = "%s", .out_json = "max_exclusive" },
	{ .key = "path", .fmt = "str", .out_json = "path" },
	{ .key = "parents", .fmt = "list", .out_json = "parents" },
	{ .key = "children", .fmt = "list", .out_json = "children" },
	/* Workaround for printing qgroupid in the list as a plain value */
	{ .key = "qgroupid-list", .fmt = "qgroupid", .out_json = NULL },
	ROWSPEC_END
};

static void print_all_qgroups_json(struct qgroup_lookup *qgroup_lookup)
{
	struct rb_node *n;
	struct btrfs_qgroup *qgroup;
	struct format_ctx fctx;

	fmt_start(&fctx, qgroup_show_rowspec, 24, 0);
	fmt_print_start_group(&fctx, "qgroup-show", JSON_TYPE_ARRAY);

	n = rb_first(&qgroup_lookup->root);
	while (n) {
		struct btrfs_qgroup_list *list = NULL;

		qgroup = rb_entry(n, struct btrfs_qgroup, sort_node);

		fmt_print_start_group(&fctx, NULL, JSON_TYPE_MAP);

		fmt_print(&fctx, "qgroupid",
				btrfs_qgroup_level(qgroup->qgroupid),
				btrfs_qgroup_subvolid(qgroup->qgroupid));
		fmt_print(&fctx, "referenced", qgroup->info.referenced);
		if (qgroup->limit.flags & BTRFS_QGROUP_LIMIT_MAX_RFER)
			fmt_print(&fctx, "max_referenced", qgroup->limit.max_referenced);
		else
			fmt_print(&fctx, "max_referenced-none", "none");
		fmt_print(&fctx, "exclusive", qgroup->info.exclusive);
		if (qgroup->limit.flags & BTRFS_QGROUP_LIMIT_MAX_EXCL)
			fmt_print(&fctx, "max_exclusive", qgroup->limit.max_exclusive);
		else
			fmt_print(&fctx, "max_exclusive-none", "none");
		fmt_print(&fctx, "path", qgroup->path ?: "");

		fmt_print_start_group(&fctx, "parents", JSON_TYPE_ARRAY);
		list_for_each_entry(list, &qgroup->qgroups, next_qgroup) {
			fmt_print(&fctx, "qgroupid-list",
				btrfs_qgroup_level(list->qgroup->qgroupid),
				btrfs_qgroup_subvolid(list->qgroup->qgroupid));
		}
		fmt_print_end_group(&fctx, "parents");

		fmt_print_start_group(&fctx, "children", JSON_TYPE_ARRAY);
		list_for_each_entry(list, &qgroup->members, next_member) {
			fmt_print(&fctx, "qgroupid-list",
				btrfs_qgroup_level(list->member->qgroupid),
				btrfs_qgroup_subvolid(list->member->qgroupid));
		}
		fmt_print_end_group(&fctx, "children");

		fmt_print_end_group(&fctx, NULL);

		n = rb_next(n);
	}
	fmt_print_end_group(&fctx, "qgroup-show");
	fmt_end(&fctx);
}

static int show_qgroups(int fd,
		       struct btrfs_qgroup_filter_set *filter_set,
		       struct btrfs_qgroup_comparer_set *comp_set)
{

	struct qgroup_lookup qgroup_lookup;
	struct qgroup_lookup sort_tree;
	int ret;

	ret = qgroups_search_all(fd, &qgroup_lookup);
	if (ret)
		return ret;
	__filter_and_sort_qgroups(&qgroup_lookup, &sort_tree,
				  filter_set, comp_set);
	if (bconf.output_format == CMD_FORMAT_JSON)
		print_all_qgroups_json(&sort_tree);
	else
		print_all_qgroups(&sort_tree);

	__free_all_qgroups(&qgroup_lookup);
	return ret;
}

/*
 * Parse sort string and allocate new comparator.
 *
 * Return: 0 no errors while parsing
 *         1 parse error
 *        <0 other errors
 */
static int qgroup_parse_sort_string(const char *opt_arg,
				   struct btrfs_qgroup_comparer_set **comps)
{
	bool order;
	bool flag;
	char *p;
	char **ptr_argv;
	int what_to_sort;
	char *opt_tmp;
	int ret = 0, ret2;

	opt_tmp = strdup(opt_arg);
	if (!opt_tmp)
		return -ENOMEM;

	p = strtok(opt_tmp, ",");
	while (p) {
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

		if (!flag) {
			ret = 1;
			goto out;
		} else {
			if (*p == '+') {
				order = false;
				p++;
			} else if (*p == '-') {
				order = true;
				p++;
			} else
				order = false;

			what_to_sort = qgroup_get_sort_item(p);
			if (what_to_sort < 0) {
				ret = 1;
				goto out;
			}
			ret2 = qgroup_setup_comparer(comps, what_to_sort, order);
			if (ret2 < 0) {
				ret = ret2;
				goto out;
			}
		}
		p = strtok(NULL, ",");
	}

out:
	free(opt_tmp);
	return ret;
}

int btrfs_qgroup_inherit_size(struct btrfs_qgroup_inherit *p)
{
	return sizeof(*p) + sizeof(p->qgroups[0]) *
			    (p->num_qgroups + 2 * p->num_ref_copies +
			     2 * p->num_excl_copies);
}

static int qgroup_inherit_realloc(struct btrfs_qgroup_inherit **inherit, int n,
		int pos)
{
	struct btrfs_qgroup_inherit *out;
	int nitems = 0;

	if (*inherit) {
		nitems = (*inherit)->num_qgroups +
			 (*inherit)->num_ref_copies +
			 (*inherit)->num_excl_copies;
	}

	out = calloc(sizeof(*out) + sizeof(out->qgroups[0]) * (nitems + n), 1);
	if (out == NULL) {
		error_msg(ERROR_MSG_MEMORY, NULL);
		return -ENOMEM;
	}

	if (*inherit) {
		struct btrfs_qgroup_inherit *i = *inherit;
		int s = sizeof(out->qgroups[0]);

		out->num_qgroups = i->num_qgroups;
		out->num_ref_copies = i->num_ref_copies;
		out->num_excl_copies = i->num_excl_copies;
		memcpy(out->qgroups, i->qgroups, pos * s);
		memcpy(out->qgroups + pos + n, i->qgroups + pos,
		       (nitems - pos) * s);
	}
	free(*inherit);
	*inherit = out;

	return 0;
}

int btrfs_qgroup_inherit_add_group(struct btrfs_qgroup_inherit **inherit, char *arg)
{
	int ret;
	u64 qgroupid = parse_qgroupid_or_path(arg);
	int pos = 0;

	if (qgroupid == 0) {
		error("invalid qgroup specification, qgroupid must not 0");
		return -EINVAL;
	}

	if (*inherit)
		pos = (*inherit)->num_qgroups;
	ret = qgroup_inherit_realloc(inherit, 1, pos);
	if (ret)
		return ret;

	(*inherit)->qgroups[(*inherit)->num_qgroups++] = qgroupid;

	return 0;
}

int btrfs_qgroup_inherit_add_copy(struct btrfs_qgroup_inherit **inherit, char *arg,
			    int type)
{
	int ret;
	u64 qgroup_src;
	u64 qgroup_dst;
	char *p;
	int pos = 0;

	p = strchr(arg, ':');
	if (!p) {
bad:
		error("invalid copy specification, missing separator :");
		return -EINVAL;
	}
	*p = 0;
	qgroup_src = parse_qgroupid_or_path(arg);
	qgroup_dst = parse_qgroupid_or_path(p + 1);
	*p = ':';

	if (!qgroup_src || !qgroup_dst)
		goto bad;

	if (*inherit)
		pos = (*inherit)->num_qgroups +
		      (*inherit)->num_ref_copies * 2 * type;

	ret = qgroup_inherit_realloc(inherit, 2, pos);
	if (ret)
		return ret;

	(*inherit)->qgroups[pos++] = qgroup_src;
	(*inherit)->qgroups[pos++] = qgroup_dst;

	if (!type)
		++(*inherit)->num_ref_copies;
	else
		++(*inherit)->num_excl_copies;

	return 0;
}
static const char * const qgroup_cmd_group_usage[] = {
	"btrfs qgroup <command> [options] <path>",
	NULL
};

static int _cmd_qgroup_assign(const struct cmd_struct *cmd, int assign,
			      int argc, char **argv)
{
	int ret = 0;
	int fd;
	bool rescan = true;
	char *path;
	struct btrfs_ioctl_qgroup_assign_args args;
	DIR *dirstream = NULL;

	optind = 0;
	while (1) {
		enum { GETOPT_VAL_RESCAN = GETOPT_VAL_FIRST, GETOPT_VAL_NO_RESCAN };
		static const struct option long_options[] = {
			{ "rescan", no_argument, NULL, GETOPT_VAL_RESCAN },
			{ "no-rescan", no_argument, NULL, GETOPT_VAL_NO_RESCAN },
			{ NULL, 0, NULL, 0 }
		};
		int c;

		c = getopt_long(argc, argv, "", long_options, NULL);
		if (c < 0)
			break;

		switch (c) {
		case GETOPT_VAL_RESCAN:
			rescan = true;
			break;
		case GETOPT_VAL_NO_RESCAN:
			rescan = false;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_exact(argc - optind, 3))
		return 1;

	memset(&args, 0, sizeof(args));
	args.assign = assign;
	args.src = parse_qgroupid_or_path(argv[optind]);
	args.dst = parse_qgroupid_or_path(argv[optind + 1]);

	path = argv[optind + 2];

	/*
	 * FIXME src should accept subvol path
	 */
	if (btrfs_qgroup_level(args.src) >= btrfs_qgroup_level(args.dst)) {
		error("bad relation requested: %s", path);
		return 1;
	}
	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_QGROUP_ASSIGN, &args);
	if (ret < 0) {
		error("unable to assign quota group: %s",
				errno == ENOTCONN ? "quota not enabled"
						: strerror(errno));
		close_file_or_dir(fd, dirstream);
		return 1;
	}

	/*
	 * If ret > 0, it means assign caused qgroup data inconsistent state.
	 * Schedule a quota rescan if requested.
	 *
	 * The return value change only happens in newer kernel. But will not
	 * cause problem since old kernel has a bug that will never clear
	 * INCONSISTENT bit.
	 */
	if (ret > 0) {
		if (rescan) {
			struct btrfs_ioctl_quota_rescan_args qargs;

			printf("Quota data changed, rescan scheduled\n");
			memset(&qargs, 0, sizeof(qargs));
			ret = ioctl(fd, BTRFS_IOC_QUOTA_RESCAN, &qargs);
			if (ret < 0)
				error("quota rescan failed: %m");
		} else {
			warning("quotas may be inconsistent, rescan needed");
			ret = 0;
		}
	}
	close_file_or_dir(fd, dirstream);
	return ret;
}

static int _cmd_qgroup_create(int create, int argc, char **argv)
{
	int ret = 0;
	int fd;
	char *path;
	struct btrfs_ioctl_qgroup_create_args args;
	DIR *dirstream = NULL;

	if (check_argc_exact(argc - optind, 2))
		return 1;

	memset(&args, 0, sizeof(args));
	args.create = create;
	ret = parse_qgroupid(argv[optind], &args.qgroupid);
	if (ret < 0) {
		error("invalid qgroupid %s", argv[optind]);
		return 1;
	}
	path = argv[optind + 1];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_QGROUP_CREATE, &args);
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		error("unable to %s quota group: %s",
			create ? "create":"destroy",
				errno == ENOTCONN ? "quota not enabled"
						: strerror(errno));
		return 1;
	}
	return 0;
}

static const char * const cmd_qgroup_assign_usage[] = {
	"btrfs qgroup assign [options] <src> <dst> <path>",
	"Assign SRC as the child qgroup of DST.",
	"Assign SRC qgroup as the child qgroup of DST, where the level of DST",
	"must be higher than SRC. The quota accounting will be inconsistent",
	"until the next rescan.",
	"",
	OPTLINE("--rescan", "schedule quota rescan if needed"),
	OPTLINE("--no-rescan", "don't schedule quota rescan"),
	NULL
};

static int cmd_qgroup_assign(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	return _cmd_qgroup_assign(cmd, 1, argc, argv);
}
static DEFINE_SIMPLE_COMMAND(qgroup_assign, "assign");

static const char * const cmd_qgroup_remove_usage[] = {
	"btrfs qgroup remove [options] <src> <dst> <path>",
	"Remove the relation between child qgroup SRC from DST.",
	"Remove the relation between SRC and DST qgroups. The quota accounting",
	"will be inconsistent until the next rescan.",
	"",
	OPTLINE("--rescan", "schedule quota rescan if needed"),
	OPTLINE("--no-rescan", "don't schedule quota rescan"),
	NULL
};

static int cmd_qgroup_remove(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	return _cmd_qgroup_assign(cmd, 0, argc, argv);
}
static DEFINE_SIMPLE_COMMAND(qgroup_remove, "remove");

static const char * const cmd_qgroup_create_usage[] = {
	"btrfs qgroup create <qgroupid> <path>",
	"Create a subvolume quota group.",
	"Create a subvolume quota group. The level can't be 0 as such qgroup is",
	"created automatically for a subvolume. Higher level qgroups are supposed",
	"to provide accounting for qgroups in a tree structure.",
	NULL
};

static int cmd_qgroup_create(const struct cmd_struct *cmd,
			     int argc, char **argv)
{
	clean_args_no_options(cmd, argc, argv);

	return _cmd_qgroup_create(1, argc, argv);
}
static DEFINE_SIMPLE_COMMAND(qgroup_create, "create");

static const char * const cmd_qgroup_destroy_usage[] = {
	"btrfs qgroup destroy <qgroupid> <path>",
	"Destroy a quota group.",
	NULL
};

static int cmd_qgroup_destroy(const struct cmd_struct *cmd,
			      int argc, char **argv)
{
	clean_args_no_options(cmd, argc, argv);

	return _cmd_qgroup_create(0, argc, argv);
}
static DEFINE_SIMPLE_COMMAND(qgroup_destroy, "destroy");

static const char * const cmd_qgroup_show_usage[] = {
	"btrfs qgroup show [options] <path>",
	"List subvolume quota groups.",
	"List subvolume quota groups, accounted size, limits and path.",
	"",
	OPTLINE("-p", "print parent qgroup id"),
	OPTLINE("-c", "print child qgroup id"),
	OPTLINE("-r", "print limit of referenced size of qgroup"),
	OPTLINE("-e", "print limit of exclusive size of qgroup"),
	OPTLINE("-F", "list all qgroups which impact the given path (including ancestral qgroups)"),
	OPTLINE("-f", "list all qgroups which impact the given path (excluding ancestral qgroups)"),
	HELPINFO_UNITS_LONG,
	OPTLINE("--sort=qgroupid,rfer,excl,max_rfer,max_excl,path",
		"list qgroups sorted by specified items "
		"you can use '+' or '-' in front of each item. "
		"(+:ascending, -:descending, ascending default)"),
	OPTLINE("--sync", "force sync of the filesystem before getting info"),
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_FORMAT,
	NULL
};

static int cmd_qgroup_show(const struct cmd_struct *cmd, int argc, char **argv)
{
	char *path;
	int ret = 0;
	int fd;
	DIR *dirstream = NULL;
	u64 qgroupid;
	int filter_flag = 0;
	unsigned unit_mode;
	bool sync = false;
	enum btrfs_util_error err;
	struct btrfs_qgroup_comparer_set *comparer_set;
	struct btrfs_qgroup_filter_set *filter_set;
	filter_set = qgroup_alloc_filter_set();
	comparer_set = qgroup_alloc_comparer_set();

	unit_mode = get_unit_mode_from_arg(&argc, argv, 0);

	optind = 0;
	while (1) {
		int c;
		enum {
			GETOPT_VAL_SORT = GETOPT_VAL_FIRST,
			GETOPT_VAL_SYNC
		};
		static const struct option long_options[] = {
			{"sort", required_argument, NULL, GETOPT_VAL_SORT},
			{"sync", no_argument, NULL, GETOPT_VAL_SYNC},
			{ NULL, 0, NULL, 0 }
		};

		c = getopt_long(argc, argv, "pcreFf", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'p':
			qgroup_setup_print_column(BTRFS_QGROUP_PARENT);
			break;
		case 'c':
			qgroup_setup_print_column(BTRFS_QGROUP_CHILD);
			break;
		case 'r':
			qgroup_setup_print_column(BTRFS_QGROUP_MAX_RFER);
			break;
		case 'e':
			qgroup_setup_print_column(BTRFS_QGROUP_MAX_EXCL);
			break;
		case 'F':
			filter_flag |= 0x1;
			break;
		case 'f':
			filter_flag |= 0x2;
			break;
		case GETOPT_VAL_SORT:
			ret = qgroup_parse_sort_string(optarg, &comparer_set);
			if (ret < 0) {
				errno = -ret;
				error("cannot parse sort string: %m");
				return 1;
			}
			if (ret > 0) {
				error("unrecognized format of sort string");
				return 1;
			}
			break;
		case GETOPT_VAL_SYNC:
			sync = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}
	qgroup_setup_units(unit_mode);

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];
	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0) {
		free(filter_set);
		free(comparer_set);
		return 1;
	}

	if (sync) {
		err = btrfs_util_sync_fd(fd);
		if (err)
			warning("sync ioctl failed on '%s': %m", path);
	}

	if (filter_flag) {
		ret = lookup_path_rootid(fd, &qgroupid);
		if (ret < 0) {
			errno = -ret;
			error("cannot resolve rootid for %s: %m", path);
			close_file_or_dir(fd, dirstream);
			goto out;
		}
		if (filter_flag & 0x1)
			qgroup_setup_filter(&filter_set,
					BTRFS_QGROUP_FILTER_ALL_PARENT,
					qgroupid);
		if (filter_flag & 0x2)
			qgroup_setup_filter(&filter_set,
					BTRFS_QGROUP_FILTER_PARENT,
					qgroupid);
	}
	ret = show_qgroups(fd, filter_set, comparer_set);
	close_file_or_dir(fd, dirstream);
	free(filter_set);
	free(comparer_set);

out:
	return !!ret;
}
static DEFINE_COMMAND_WITH_FLAGS(qgroup_show, "show", CMD_FORMAT_JSON);

static const char * const cmd_qgroup_limit_usage[] = {
	"btrfs qgroup limit [options] <size>|none [<qgroupid>] <path>",
	"Set the limits a subvolume quota group.",
	"",
	OPTLINE("-c", "limit amount of data after compression. This is the default, it is currently not possible to turn off this option"),
	OPTLINE("-e", "limit space exclusively assigned to this qgroup"),
	NULL
};

static int cmd_qgroup_limit(const struct cmd_struct *cmd, int argc, char **argv)
{
	int ret = 0;
	int fd;
	char *path = NULL;
	struct btrfs_ioctl_qgroup_limit_args args;
	unsigned long long size;
	bool compressed = false;
	bool exclusive = false;
	DIR *dirstream = NULL;
	enum btrfs_util_error err;

	optind = 0;
	while (1) {
		int c = getopt(argc, argv, "ce");
		if (c < 0)
			break;
		switch (c) {
		case 'c':
			compressed = true;
			break;
		case 'e':
			exclusive = true;
			break;
		default:
			usage_unknown_option(cmd, argv);
		}
	}

	if (check_argc_min(argc - optind, 2))
		return 1;

	if (!strcasecmp(argv[optind], "none"))
		size = -1ULL;
	else
		size = parse_size_from_string(argv[optind]);

	memset(&args, 0, sizeof(args));
	if (compressed)
		args.lim.flags |= BTRFS_QGROUP_LIMIT_RFER_CMPR |
				  BTRFS_QGROUP_LIMIT_EXCL_CMPR;
	if (exclusive) {
		args.lim.flags |= BTRFS_QGROUP_LIMIT_MAX_EXCL;
		args.lim.max_exclusive = size;
	} else {
		args.lim.flags |= BTRFS_QGROUP_LIMIT_MAX_RFER;
		args.lim.max_referenced = size;
	}

	if (argc - optind == 2) {
		args.qgroupid = 0;
		path = argv[optind + 1];
		err = btrfs_util_is_subvolume(path);
		if (err) {
			error_btrfs_util(err);
			return 1;
		}
		/*
		 * keep qgroupid at 0, this indicates that the subvolume the
		 * fd refers to is to be limited
		 */
	} else if (argc - optind == 3) {
		args.qgroupid = parse_qgroupid_or_path(argv[optind + 1]);
		path = argv[optind + 2];
	} else
		usage(cmd, 1);

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_QGROUP_LIMIT, &args);
	close_file_or_dir(fd, dirstream);
	if (ret < 0) {
		error("unable to limit requested quota group: %s",
				errno == ENOTCONN ? "quota not enabled"
						: strerror(errno));

		return 1;
	}
	return 0;
}
static DEFINE_SIMPLE_COMMAND(qgroup_limit, "limit");

static const char * const cmd_qgroup_clear_stale_usage[] = {
	"btrfs qgroup clear-stale <path>",
	"Clear all stale qgroups (level 0/subvolid), without a subvolume.",
	"Clear all stale qgroups whose subvolume does not exist anymore, this is the",
	"level 0 qgroup like 0/subvolid. Higher level qgroups are not deleted even",
	"if they don't have any child qgroups.",
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_QUIET,
	NULL
};

static int cmd_qgroup_clear_stale(const struct cmd_struct *cmd, int argc, char **argv)
{
	int ret = 0;
	int fd;
	char *path = NULL;
	DIR *dirstream = NULL;
	struct qgroup_lookup qgroup_lookup;
	struct rb_node *node;
	struct btrfs_qgroup *entry;

	if (check_argc_exact(argc - optind, 1))
		return 1;

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = qgroups_search_all(fd, &qgroup_lookup);
	if (ret == -ENOTTY) {
		error("can't list qgroups: quotas not enabled");
		goto out;
	} else if (ret < 0) {
		errno = -ret;
		error("can't list qgroups: %m");
		goto out;
	}

	node = rb_first(&qgroup_lookup.root);
	while (node) {
		u64 level;
		struct btrfs_ioctl_qgroup_create_args args = { .create = false };

		entry = rb_entry(node, struct btrfs_qgroup, rb_node);
		level = btrfs_qgroup_level(entry->qgroupid);
		if (!entry->path && level == 0) {
			pr_verbose(LOG_DEFAULT, "Delete stale qgroup %llu/%llu\n",
					level, btrfs_qgroup_subvolid(entry->qgroupid));
			args.qgroupid = entry->qgroupid;
			ret = ioctl(fd, BTRFS_IOC_QGROUP_CREATE, &args);
			if (ret < 0) {
				error("cannot delete qgroup %llu/%llu: %m",
					level,
					btrfs_qgroup_subvolid(entry->qgroupid));
			}
		}
		node = rb_next(node);
	}

out:
	close_file_or_dir(fd, dirstream);
	return !!ret;
}
static DEFINE_SIMPLE_COMMAND(qgroup_clear_stale, "clear-stale");

static const char qgroup_cmd_group_info[] =
"manage quota groups";

static const struct cmd_group qgroup_cmd_group = {
	qgroup_cmd_group_usage, qgroup_cmd_group_info, {
		&cmd_struct_qgroup_assign,
		&cmd_struct_qgroup_remove,
		&cmd_struct_qgroup_create,
		&cmd_struct_qgroup_clear_stale,
		&cmd_struct_qgroup_destroy,
		&cmd_struct_qgroup_show,
		&cmd_struct_qgroup_limit,
		NULL
	}
};

DEFINE_GROUP_COMMAND_TOKEN(qgroup);
