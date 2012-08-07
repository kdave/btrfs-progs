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
#include "ctree.h"

u64 parse_qgroupid(char *p)
{
	char *s = strchr(p, '/');
	u64 level;
	u64 id;

	if (!s)
		return atoll(p);
	level = atoll(p);
	id = atoll(s + 1);

	return (level << 48) | id;
}

int qgroup_inherit_size(struct btrfs_qgroup_inherit *p)
{
	return sizeof(*p) + sizeof(p->qgroups[0]) *
			    (p->num_qgroups + 2 * p->num_ref_copies +
			     2 * p->num_excl_copies);
}

int qgroup_inherit_realloc(struct btrfs_qgroup_inherit **inherit, int n,
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
		fprintf(stderr, "ERROR: Not enough memory\n");
		return 13;
	}

	if (*inherit) {
		struct btrfs_qgroup_inherit *i = *inherit;
		int s = sizeof(out->qgroups);

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
