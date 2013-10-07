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

#include "qgroup.h"
#include <sys/ioctl.h>
#include "ctree.h"
#include "ioctl.h"

struct qgroup_lookup {
	struct rb_root root;
};

struct btrfs_qgroup {
	struct rb_node rb_node;
	u64 qgroupid;

	/*
	 * info_item
	 */
	u64 generation;
	u64 rfer;	/*referenced*/
	u64 rfer_cmpr;	/*referenced compressed*/
	u64 excl;	/*exclusive*/
	u64 excl_cmpr;	/*exclusive compressed*/

	/*
	 *limit_item
	 */
	u64 flags;	/*which limits are set*/
	u64 max_rfer;
	u64 max_excl;
	u64 rsv_rfer;
	u64 rsv_excl;

	/*qgroups this group is member of*/
	struct list_head qgroups;
	/*qgroups that are members of this group*/
	struct list_head members;
};

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

/*
 * qgroupid,rfer,excl default to set
 */
struct {
	char *name;
	char *column_name;
	int need_print;
} btrfs_qgroup_columns[] = {
	{
		.name		= "qgroupid",
		.column_name	= "Qgroupid",
		.need_print	= 1,
	},
	{
		.name		= "rfer",
		.column_name	= "Rfer",
		.need_print	= 1,
	},
	{
		.name		= "excl",
		.column_name	= "Excl",
		.need_print	= 1,
	},
	{	.name		= "max_rfer",
		.column_name	= "Max_rfer",
		.need_print	= 0,
	},
	{
		.name		= "parent",
		.column_name	= "Parent",
		.need_print	= 0,
	},
	{
		.name		= "child",
		.column_name	= "Child",
		.need_print	= 0,
	},
	{
		.name		= NULL,
		.column_name	= NULL,
		.need_print	= 0,
	},
};

void btrfs_qgroup_setup_print_column(enum btrfs_qgroup_column_enum column)
{
	int i;

	BUG_ON(column < 0 || column > BTRFS_QGROUP_ALL);

	if (column < BTRFS_QGROUP_ALL) {
		btrfs_qgroup_columns[column].need_print = 1;
		return;
	}
	for (i = 0; i < BTRFS_QGROUP_ALL; i++)
		btrfs_qgroup_columns[i].need_print = 1;
}

static void print_parent_column(struct btrfs_qgroup *qgroup)
{
	struct btrfs_qgroup_list *list = NULL;

	list_for_each_entry(list, &qgroup->qgroups, next_qgroup) {
		printf("%llu/%llu", (list->qgroup)->qgroupid >> 48,
		      ((1ll << 48) - 1) & (list->qgroup)->qgroupid);
		if (!list_is_last(&list->next_qgroup, &qgroup->qgroups))
			printf(",");
	}
	if (list_empty(&qgroup->qgroups))
		printf("---");
}

static void print_child_column(struct btrfs_qgroup *qgroup)
{
	struct btrfs_qgroup_list *list = NULL;

	list_for_each_entry(list, &qgroup->members, next_member) {
		printf("%llu/%llu", (list->member)->qgroupid >> 48,
		      ((1ll << 48) - 1) & (list->member)->qgroupid);
		if (!list_is_last(&list->next_member, &qgroup->members))
			printf(",");
	}
	if (list_empty(&qgroup->members))
		printf("---");
}

static void print_qgroup_column(struct btrfs_qgroup *qgroup,
				enum btrfs_qgroup_column_enum column)
{
	BUG_ON(column >= BTRFS_QGROUP_ALL || column < 0);

	switch (column) {

	case BTRFS_QGROUP_QGROUPID:
		printf("%llu/%llu", qgroup->qgroupid >> 48,
		       ((1ll << 48) - 1) & qgroup->qgroupid);
		break;
	case BTRFS_QGROUP_RFER:
		printf("%lld", qgroup->rfer);
		break;
	case BTRFS_QGROUP_EXCL:
		printf("%lld", qgroup->excl);
		break;
	case BTRFS_QGROUP_PARENT:
		print_parent_column(qgroup);
		break;
	case BTRFS_QGROUP_MAX_RFER:
		printf("%llu", qgroup->max_rfer);
		break;
	case BTRFS_QGROUP_CHILD:
		print_child_column(qgroup);
		break;
	default:
		break;
	}
}

static void print_single_qgroup_default(struct btrfs_qgroup *qgroup)
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

static int update_qgroup(struct qgroup_lookup *qgroup_lookup, u64 qgroupid,
			 u64 generation, u64 rfer, u64 rfer_cmpr, u64 excl,
			 u64 excl_cmpr, u64 flags, u64 max_rfer, u64 max_excl,
			 u64 rsv_rfer, u64 rsv_excl, struct btrfs_qgroup *pa,
			 struct btrfs_qgroup *child)
{
	struct btrfs_qgroup *bq;
	struct btrfs_qgroup_list *list;

	bq = qgroup_tree_search(qgroup_lookup, qgroupid);
	if (!bq || bq->qgroupid != qgroupid)
		return -ENOENT;

	if (generation)
		bq->generation = generation;
	if (rfer)
		bq->rfer = rfer;
	if (rfer_cmpr)
		bq->rfer_cmpr = rfer_cmpr;
	if (excl)
		bq->excl = excl;
	if (excl_cmpr)
		bq->excl_cmpr = excl_cmpr;
	if (flags)
		bq->flags = flags;
	if (max_rfer)
		bq->max_rfer = max_rfer;
	if (max_excl)
		bq->max_excl = max_excl;
	if (rsv_rfer)
		bq->rsv_rfer = rsv_rfer;
	if (pa && child) {
		list = malloc(sizeof(*list));
		if (!list) {
			fprintf(stderr, "memory allocation failed\n");
			exit(1);
		}
		list->qgroup = pa;
		list->member = child;
		list_add_tail(&list->next_qgroup, &child->qgroups);
		list_add_tail(&list->next_member, &pa->members);
	}
	return 0;
}

static int add_qgroup(struct qgroup_lookup *qgroup_lookup, u64 qgroupid,
		      u64 generation, u64 rfer, u64 rfer_cmpr, u64 excl,
		      u64 excl_cmpr, u64 flags, u64 max_rfer, u64 max_excl,
		      u64 rsv_rfer, u64 rsv_excl, struct btrfs_qgroup *parent,
		      struct btrfs_qgroup *child)
{
	struct btrfs_qgroup *bq;
	struct btrfs_qgroup_list *list;
	int ret;

	ret = update_qgroup(qgroup_lookup, qgroupid, generation, rfer,
			    rfer_cmpr, excl, excl_cmpr, flags, max_rfer,
			    max_excl, rsv_rfer, rsv_excl, parent, child);
	if (!ret)
		return 0;

	bq = malloc(sizeof(*bq));
	if (!bq) {
		printf("memory allocation failed\n");
		exit(1);
	}
	memset(bq, 0, sizeof(*bq));
	if (qgroupid) {
		bq->qgroupid = qgroupid;
		INIT_LIST_HEAD(&bq->qgroups);
		INIT_LIST_HEAD(&bq->members);
	}
	if (generation)
		bq->generation = generation;
	if (rfer)
		bq->rfer = rfer;
	if (rfer_cmpr)
		bq->rfer_cmpr = rfer_cmpr;
	if (excl)
		bq->excl = excl;
	if (excl_cmpr)
		bq->excl_cmpr = excl_cmpr;
	if (flags)
		bq->flags = flags;
	if (max_rfer)
		bq->max_rfer = max_rfer;
	if (max_excl)
		bq->max_excl = max_excl;
	if (rsv_rfer)
		bq->rsv_rfer = rsv_rfer;
	if (parent && child) {
		list = malloc(sizeof(*list));
		if (!list) {
			fprintf(stderr, "memory allocation failed\n");
			exit(1);
		}
		list->qgroup = parent;
		list->member = child;
		list_add_tail(&list->next_qgroup, &child->qgroups);
		list_add_tail(&list->next_member, &parent->members);
	}
	ret = qgroup_tree_insert(qgroup_lookup, bq);
	if (ret) {
		printf("failed to insert tree %llu\n",
		       bq->qgroupid);
		exit(1);
	}
	return ret;
}

void __free_btrfs_qgroup(struct btrfs_qgroup *bq)
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
	free(bq);
}

void __free_all_qgroups(struct qgroup_lookup *root_tree)
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

static int __qgroups_search(int fd, struct qgroup_lookup *qgroup_lookup)
{
	int ret;
	struct btrfs_ioctl_search_args args;
	struct btrfs_ioctl_search_key *sk = &args.key;
	struct btrfs_ioctl_search_header *sh;
	unsigned long off = 0;
	unsigned int i;
	int e;
	struct btrfs_qgroup_info_item *info;
	struct btrfs_qgroup_limit_item *limit;
	struct btrfs_qgroup *bq;
	struct btrfs_qgroup *bq1;
	u64 a1;
	u64 a2;
	u64 a3;
	u64 a4;
	u64 a5;

	memset(&args, 0, sizeof(args));

	sk->tree_id = BTRFS_QUOTA_TREE_OBJECTID;
	sk->max_type = BTRFS_QGROUP_RELATION_KEY;
	sk->min_type = BTRFS_QGROUP_INFO_KEY;
	sk->max_objectid = (u64)-1;
	sk->max_offset = (u64)-1;
	sk->max_transid = (u64)-1;
	sk->nr_items = 4096;

	qgroup_lookup_init(qgroup_lookup);

	while (1) {
		ret = ioctl(fd, BTRFS_IOC_TREE_SEARCH, &args);
		e = errno;
		if (ret < 0) {
			fprintf(stderr,
				"ERROR: can't perform the search - %s\n",
				strerror(e));
			return ret;
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
			sh = (struct btrfs_ioctl_search_header *)(args.buf +
								  off);
			off += sizeof(*sh);

			if (sh->type == BTRFS_QGROUP_INFO_KEY) {
				info = (struct btrfs_qgroup_info_item *)
				       (args.buf + off);
				a1 = btrfs_stack_qgroup_info_generation(info);
				a2 = btrfs_stack_qgroup_info_referenced(info);
				a3 =
				  btrfs_stack_qgroup_info_referenced_compressed
				  (info);
				a4 = btrfs_stack_qgroup_info_exclusive(info);
				a5 =
				  btrfs_stack_qgroup_info_exclusive_compressed
				  (info);
				add_qgroup(qgroup_lookup, sh->offset, a1, a2,
					   a3, a4, a5, 0, 0, 0, 0, 0, 0, 0);
			} else if (sh->type == BTRFS_QGROUP_LIMIT_KEY) {
				limit = (struct btrfs_qgroup_limit_item *)
				    (args.buf + off);

				a1 = btrfs_stack_qgroup_limit_flags(limit);
				a2 = btrfs_stack_qgroup_limit_max_referenced
				     (limit);
				a3 = btrfs_stack_qgroup_limit_max_exclusive
				     (limit);
				a4 = btrfs_stack_qgroup_limit_rsv_referenced
				     (limit);
				a5 = btrfs_stack_qgroup_limit_rsv_exclusive
				     (limit);
				add_qgroup(qgroup_lookup, sh->offset, 0, 0,
					   0, 0, 0, a1, a2, a3, a4, a5, 0, 0);
			} else if (sh->type == BTRFS_QGROUP_RELATION_KEY) {
				if (sh->offset < sh->objectid)
					goto skip;
				bq = qgroup_tree_search(qgroup_lookup,
							sh->offset);
				if (!bq)
					goto skip;
				bq1 = qgroup_tree_search(qgroup_lookup,
							 sh->objectid);
				if (!bq1)
					goto skip;
				add_qgroup(qgroup_lookup, sh->offset, 0, 0,
					   0, 0, 0, 0, 0, 0, 0, 0, bq, bq1);
			} else
				goto done;
skip:
			off += sh->len;

			/*
			 * record the mins in sk so we can make sure the
			 * next search doesn't repeat this root
			 */
			sk->min_type = sh->type;
			sk->min_offset = sh->offset;
			sk->min_objectid = sh->objectid;
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

done:
	return ret;
}

static void print_all_qgroups(struct qgroup_lookup *qgroup_lookup)
{

	struct rb_node *n;
	struct btrfs_qgroup *entry;

	n = rb_first(&qgroup_lookup->root);
	while (n) {
		entry = rb_entry(n, struct btrfs_qgroup, rb_node);
		print_single_qgroup_default(entry);
		n = rb_next(n);
	}
}

int btrfs_show_qgroups(int fd)
{

	struct qgroup_lookup qgroup_lookup;
	int ret;

	ret = __qgroups_search(fd, &qgroup_lookup);
	if (ret)
		return ret;

	print_all_qgroups(&qgroup_lookup);
	__free_all_qgroups(&qgroup_lookup);

	return ret;
}

u64 parse_qgroupid(char *p)
{
	char *s = strchr(p, '/');
	char *ptr_src_end = p + strlen(p);
	char *ptr_parse_end = NULL;
	u64 level;
	u64 id;

	if (!s) {
		id = strtoull(p, &ptr_parse_end, 10);
		if (ptr_parse_end != ptr_src_end)
			goto err;
		return id;
	}
	level = strtoull(p, &ptr_parse_end, 10);
	if (ptr_parse_end != s)
		goto err;

	id = strtoull(s+1, &ptr_parse_end, 10);
	if (ptr_parse_end != ptr_src_end)
		goto  err;

	return (level << 48) | id;
err:
	fprintf(stderr, "ERROR:invalid qgroupid\n");
	exit(-1);
}

int qgroup_inherit_size(struct btrfs_qgroup_inherit *p)
{
	return sizeof(*p) + sizeof(p->qgroups[0]) *
			    (p->num_qgroups + 2 * p->num_ref_copies +
			     2 * p->num_excl_copies);
}

static int
qgroup_inherit_realloc(struct btrfs_qgroup_inherit **inherit, int n, int pos)
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
		fprintf(stderr, "ERROR: Not enough memory\n");
		return 13;
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

int qgroup_inherit_add_group(struct btrfs_qgroup_inherit **inherit, char *arg)
{
	int ret;
	u64 qgroupid = parse_qgroupid(arg);
	int pos = 0;

	if (qgroupid == 0) {
		fprintf(stderr, "ERROR: bad qgroup specification\n");
		return 12;
	}

	if (*inherit)
		pos = (*inherit)->num_qgroups;
	ret = qgroup_inherit_realloc(inherit, 1, pos);
	if (ret)
		return ret;

	(*inherit)->qgroups[(*inherit)->num_qgroups++] = qgroupid;

	return 0;
}

int qgroup_inherit_add_copy(struct btrfs_qgroup_inherit **inherit, char *arg,
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
		fprintf(stderr, "ERROR: bad copy specification\n");
		return 12;
	}
	*p = 0;
	qgroup_src = parse_qgroupid(arg);
	qgroup_dst = parse_qgroupid(p + 1);
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
