/*
 * Copyright (C) 2012 Fujitsu.  All rights reserved.
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

#include "kerncompat.h"

struct root_info;

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
void btrfs_list_free_filter_set(struct btrfs_list_filter_set *filter_set);
int btrfs_list_setup_filter(struct btrfs_list_filter_set **filter_set,
			    enum btrfs_list_filter_enum filter, u64 data);
struct btrfs_list_comparer_set *btrfs_list_alloc_comparer_set(void);
void btrfs_list_free_comparer_set(struct btrfs_list_comparer_set *comp_set);
int btrfs_list_setup_comparer(struct btrfs_list_comparer_set **comp_set,
			      enum btrfs_list_comp_enum comparer,
			      int is_descending);

int btrfs_list_subvols(int fd, struct btrfs_list_filter_set *filter_set,
		       struct btrfs_list_comparer_set *comp_set,
			int is_tab_result);
int btrfs_list_find_updated_files(int fd, u64 root_id, u64 oldest_gen);
int btrfs_list_get_default_subvolume(int fd, u64 *default_id);
char *btrfs_list_path_for_root(int fd, u64 root);
u64 btrfs_list_get_path_rootid(int fd);
