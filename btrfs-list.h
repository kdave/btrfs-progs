/*
 * Copyright (C) 2012 FUJITSU LIMITED.  All rights reserved.
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

#ifndef __BTRFS_LIST_H__
#define __BTRFS_LIST_H__

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#include "kernel-lib/rbtree.h"
#include "ioctl.h"
#else
#include <btrfs/kerncompat.h>
#include <btrfs/rbtree.h>
#include <btrfs/ioctl.h>
#endif /* BTRFS_FLAT_INCLUDES */

#include <time.h>

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
typedef int (*btrfs_list_comp_func)(struct root_info *, struct root_info *,
				    int);

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

int btrfs_list_parse_sort_string(char *optarg,
				 struct btrfs_list_comparer_set **comps);
int btrfs_list_parse_filter_string(char *optarg,
				   struct btrfs_list_filter_set **filters,
				   enum btrfs_list_filter_enum type);
void btrfs_list_setup_print_column(enum btrfs_list_column_enum column);
struct btrfs_list_filter_set *btrfs_list_alloc_filter_set(void);
void btrfs_list_setup_filter(struct btrfs_list_filter_set **filter_set,
			    enum btrfs_list_filter_enum filter, u64 data);
struct btrfs_list_comparer_set *btrfs_list_alloc_comparer_set(void);

int btrfs_list_subvols_print(int fd, struct btrfs_list_filter_set *filter_set,
		       struct btrfs_list_comparer_set *comp_set,
		       enum btrfs_list_layout layout, int full_path,
		       const char *raw_prefix);
int btrfs_list_find_updated_files(int fd, u64 root_id, u64 oldest_gen);
int btrfs_list_get_default_subvolume(int fd, u64 *default_id);
char *btrfs_list_path_for_root(int fd, u64 root);
int btrfs_list_get_path_rootid(int fd, u64 *treeid);
int btrfs_get_subvol(int fd, struct root_info *the_ri);
int btrfs_get_toplevel_subvol(int fd, struct root_info *the_ri);

#endif
