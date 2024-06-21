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
#include <inttypes.h>
#include <sys/ioctl.h>
#include <getopt.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include "libbtrfsutil/btrfsutil.h"
#include "kernel-lib/rbtree.h"
#include "kernel-lib/rbtree_types.h"
#include "kernel-shared/accessors.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/ctree.h"
#include "common/defs.h"
#include "common/rbtree-utils.h"
#include "common/help.h"
#include "common/messages.h"
#include "common/open-utils.h"
#include "common/string-utils.h"
#include "common/utils.h"
#include "common/format-output.h"
#include "common/tree-search.h"
#include "cmds/commands.h"
#include "cmds/subvolume.h"

enum btrfs_list_comp_enum;
enum btrfs_list_filter_enum;

/*
 * Naming of options:
 * - uppercase for filters and sort options
 * - lowercase for enabling specific items in the output
 */
static const char * const cmd_subvolume_list_usage[] = {
	"btrfs subvolume list [options] <path>",
	"List subvolumes and snapshots in the filesystem.",
	"",
	"Path filtering:",
	OPTLINE("-o", "print only the immediate children subvolumes of the "
		"subvolume containing <path>"),
	OPTLINE("-a", "print all subvolumes in the filesystem other than the "
		"root subvolume, and prefix subvolumes that are not an "
		"immediate child of the subvolume containing <path> with "
		"\"<FS_TREE>/\""),
	"",
	"If none of these are given, print all subvolumes other than the root",
	"subvolume relative to the subvolume containing <path> if below it,",
	"otherwise relative to the root of the filesystem.",
	"",
	"Field selection:",
	OPTLINE("-p", "print parent ID"),
	OPTLINE("-c", "print the ogeneration of the subvolume"),
	OPTLINE("-g", "print the generation of the subvolume"),
	OPTLINE("-u", "print the uuid of subvolumes (and snapshots)"),
	OPTLINE("-q", "print the parent uuid of the snapshots"),
	OPTLINE("-R", "print the uuid of the received snapshots"),
	"",
	"Type filtering:",
	OPTLINE("-s", "list only snapshots"),
	OPTLINE("-r", "list readonly subvolumes (including snapshots)"),
	OPTLINE("-d", "list deleted subvolumes that are not yet cleaned"),
	"",
	"Other:",
	OPTLINE("-t", "print the result as a table"),
	"",
	"Sorting:",
	OPTLINE("-G [+|-]value", "filter the subvolumes by generation "
		"(+value: >= value; -value: <= value; value: = value)"),
	OPTLINE("-C [+|-]value", "filter the subvolumes by ogeneration "
		"(+value: >= value; -value: <= value; value: = value)"),
	OPTLINE("--sort=gen,ogen,rootid,path", "list the subvolume in order of gen, ogen, rootid or path "
		"you also can add '+' or '-' in front of each items. "
		"(+:ascending, -:descending, ascending default)"),
#if EXPERIMENTAL
	HELPINFO_INSERT_GLOBALS,
	HELPINFO_INSERT_FORMAT,
#endif
	NULL,
};

#define BTRFS_LIST_NFILTERS_INCREASE	(2 * BTRFS_LIST_FILTER_MAX)
#define BTRFS_LIST_NCOMPS_INCREASE	(2 * BTRFS_LIST_COMP_MAX)

enum btrfs_list_layout {
	BTRFS_LIST_LAYOUT_DEFAULT = 0,
	BTRFS_LIST_LAYOUT_TABLE,
	BTRFS_LIST_LAYOUT_JSON
};

struct root_info {
	struct btrfs_util_subvolume_info info;
	char *path;
};

struct subvol_list {
	size_t num;
	struct root_info subvols[];
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
	BTRFS_LIST_FILTER_REMOVE_PATH_PREFIX,
	BTRFS_LIST_FILTER_MAX,
};

enum btrfs_list_comp_enum {
	BTRFS_LIST_COMP_ROOTID,
	BTRFS_LIST_COMP_OGEN,
	BTRFS_LIST_COMP_GEN,
	BTRFS_LIST_COMP_PATH,
	BTRFS_LIST_COMP_MAX,
};

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

static void btrfs_list_setup_print_column(enum btrfs_list_column_enum column)
{
	int i;

	UASSERT(0 <= column && column <= BTRFS_LIST_ALL);

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
	if (entry1->info.id > entry2->info.id)
		return 1;
	else if (entry1->info.id < entry2->info.id)
		return -1;
	return 0;
}

static int comp_entry_with_gen(const struct root_info *entry1,
			       const struct root_info *entry2)
{
	if (entry1->info.generation > entry2->info.generation)
		return 1;
	else if (entry1->info.generation < entry2->info.generation)
		return -1;
	return 0;
}

static int comp_entry_with_ogen(const struct root_info *entry1,
				const struct root_info *entry2)
{
	if (entry1->info.otransid > entry2->info.otransid)
		return 1;
	else if (entry1->info.otransid < entry2->info.otransid)
		return -1;
	return 0;
}

static int comp_entry_with_path(const struct root_info *entry1,
				const struct root_info *entry2)
{
	if (strcmp(entry1->path, entry2->path) > 0)
		return 1;
	else if (strcmp(entry1->path, entry2->path) < 0)
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

	UASSERT(set != NULL);
	UASSERT(comparer < BTRFS_LIST_COMP_MAX);
	UASSERT(set->ncomps <= set->total);

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

	UASSERT(set->comps[set->ncomps].comp_func == NULL);

	set->comps[set->ncomps].comp_func = all_comp_funcs[comparer];
	set->comps[set->ncomps].is_descending = is_descending;
	set->ncomps++;
	return 0;
}

static int sort_comp(const void *entry1, const void *entry2, void *arg)
{
	struct btrfs_list_comparer_set *set = arg;
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

static void sort_subvols(struct btrfs_list_comparer_set *comp_set,
			 struct subvol_list *subvols)
{
	qsort_r(subvols->subvols, subvols->num, sizeof(subvols->subvols[0]),
		sort_comp, comp_set);
}

static int filter_snapshot(struct root_info *ri, u64 data)
{
	return !uuid_is_null(ri->info.parent_uuid);
}

static int filter_flags(struct root_info *ri, u64 flags)
{
	return ri->info.flags & flags;
}

static int filter_gen_more(struct root_info *ri, u64 data)
{
	return ri->info.generation >= data;
}

static int filter_gen_less(struct root_info *ri, u64 data)
{
	return ri->info.generation <= data;
}

static int filter_gen_equal(struct root_info  *ri, u64 data)
{
	return ri->info.generation == data;
}

static int filter_cgen_more(struct root_info *ri, u64 data)
{
	return ri->info.otransid >= data;
}

static int filter_cgen_less(struct root_info *ri, u64 data)
{
	return ri->info.otransid <= data;
}

static int filter_cgen_equal(struct root_info *ri, u64 data)
{
	return ri->info.otransid == data;
}

static int filter_topid_equal(struct root_info *ri, u64 data)
{
	/* See the comment in print_subvolume_column() about top level. */
	return ri->info.parent_id == data;
}

static int filter_full_path(struct root_info *ri, u64 data)
{
	/*
	 * If this subvolume's parent is not the subvolume containing the path
	 * given on the command line, prepend "<FS_TREE>/". This behavior is
	 * nonsense, but we keep it for backwards compatibility. It was
	 * introduced by the same change to top level mentioned in
	 * print_subvolume_column().
	 */
	if (ri->info.parent_id != data) {
		char *tmp;
		int ret;

		ret = asprintf(&tmp, "<FS_TREE>/%s", ri->path);
		if (ret == -1) {
			error("out of memory");
			exit(1);
		}

		free(ri->path);
		ri->path = tmp;
	}
	return 1;
}

static int filter_remove_path_prefix(struct root_info *ri, u64 data)
{
	/*
	 * If this subvolume is a descendant of the given path, remove that path
	 * prefix. Otherwise, leave it alone. This is also nonsense that we keep
	 * for backwards compatibility.
	 */
	const char *prefix = (const char *)data;
	size_t len = strlen(prefix);
	if (strncmp(ri->path, prefix, len) == 0 && ri->path[len] == '/')
		memmove(ri->path, &ri->path[len + 1], strlen(ri->path) - len);
	return 1;
}

static btrfs_list_filter_func all_filter_funcs[] = {
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
	[BTRFS_LIST_FILTER_REMOVE_PATH_PREFIX]	= filter_remove_path_prefix,
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

	UASSERT(set != NULL);
	UASSERT(filter < BTRFS_LIST_FILTER_MAX);
	UASSERT(set->nfilters <= set->total);

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

	UASSERT(set->filters[set->nfilters].filter_func == NULL);

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

	for (i = 0; i < set->nfilters; i++) {
		if (!set->filters[i].filter_func)
			break;
		ret = set->filters[i].filter_func(ri, set->filters[i].data);
		if (!ret)
			return 0;
	}
	return 1;
}

static void print_subvolume_column(struct root_info *subv,
				   enum btrfs_list_column_enum column)
{
	char tstr[256];
	char uuidparse[BTRFS_UUID_UNPARSED_SIZE];

	UASSERT(0 <= column && column < BTRFS_LIST_ALL);

	switch (column) {
	case BTRFS_LIST_OBJECTID:
		pr_verbose(LOG_DEFAULT, "%" PRIu64, subv->info.id);
		break;
	case BTRFS_LIST_GENERATION:
		pr_verbose(LOG_DEFAULT, "%" PRIu64, subv->info.generation);
		break;
	case BTRFS_LIST_OGENERATION:
		pr_verbose(LOG_DEFAULT, "%" PRIu64, subv->info.otransid);
		break;
	case BTRFS_LIST_PARENT:
	/*
	 * Top level used to mean something else, but since 4f5ebb3ef553
	 * ("Btrfs-progs: fix to make list specified directory's subvolumes
	 * work") it was always set to the parent ID. See
	 * https://lore.kernel.org/all/bdd9af61-b408-c8d2-6697-84230b0bcf89@gmail.com/.
	 */
	case BTRFS_LIST_TOP_LEVEL:
		pr_verbose(LOG_DEFAULT, "%" PRIu64, subv->info.parent_id);
		break;
	case BTRFS_LIST_OTIME:
		if (subv->info.otime.tv_sec) {
			struct tm tm;

			localtime_r(&subv->info.otime.tv_sec, &tm);
			strftime(tstr, 256, "%Y-%m-%d %X", &tm);
		} else
			strcpy(tstr, "-");
		pr_verbose(LOG_DEFAULT, "%s", tstr);
		break;
	case BTRFS_LIST_UUID:
		if (uuid_is_null(subv->info.uuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subv->info.uuid, uuidparse);
		pr_verbose(LOG_DEFAULT, "%-36s", uuidparse);
		break;
	case BTRFS_LIST_PUUID:
		if (uuid_is_null(subv->info.parent_uuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subv->info.parent_uuid, uuidparse);
		pr_verbose(LOG_DEFAULT, "%-36s", uuidparse);
		break;
	case BTRFS_LIST_RUUID:
		if (uuid_is_null(subv->info.received_uuid))
			strcpy(uuidparse, "-");
		else
			uuid_unparse(subv->info.received_uuid, uuidparse);
		pr_verbose(LOG_DEFAULT, "%-36s", uuidparse);
		break;
	case BTRFS_LIST_PATH:
		BUG_ON(!subv->path);
		pr_verbose(LOG_DEFAULT, "%s", subv->path);
		break;
	default:
		break;
	}
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

static void print_subvol_json_key(struct format_ctx *fctx,
				  const struct root_info *subv,
				  const enum btrfs_list_column_enum column)
{
	const char *column_name;

	UASSERT(0 <= column && column < BTRFS_LIST_ALL);

	column_name = btrfs_list_columns[column].name;
	switch (column) {
	case BTRFS_LIST_OBJECTID:
		fmt_print(fctx, column_name, subv->info.id);
		break;
	case BTRFS_LIST_GENERATION:
		fmt_print(fctx, column_name, subv->info.generation);
		break;
	case BTRFS_LIST_OGENERATION:
		fmt_print(fctx, column_name, subv->info.otransid);
		break;
	case BTRFS_LIST_PARENT:
	/* See the comment in print_subvolume_column() about top level. */
	case BTRFS_LIST_TOP_LEVEL:
		fmt_print(fctx, column_name, subv->info.parent_id);
		break;
	case BTRFS_LIST_OTIME:
		fmt_print(fctx, column_name, subv->info.otime.tv_sec);
		break;
	case BTRFS_LIST_UUID:
		fmt_print(fctx, column_name, subv->info.uuid);
		break;
	case BTRFS_LIST_PUUID:
		fmt_print(fctx, column_name, subv->info.parent_uuid);
		break;
	case BTRFS_LIST_RUUID:
		fmt_print(fctx, column_name, subv->info.received_uuid);
		break;
	case BTRFS_LIST_PATH:
		BUG_ON(!subv->path);
		fmt_print(fctx, column_name, subv->path);
		break;
	default:
		break;
	}
}

static void print_one_subvol_info_json(struct format_ctx *fctx,
				struct root_info *subv)
{
	int i;

	fmt_print_start_group(fctx, NULL, JSON_TYPE_MAP);

	for (i = 0; i < BTRFS_LIST_ALL; i++) {
		if (!btrfs_list_columns[i].need_print)
			continue;

		print_subvol_json_key(fctx, subv, i);
	}

	fmt_print_end_group(fctx, NULL);
}


static void print_all_subvol_info(struct subvol_list *subvols,
		  enum btrfs_list_layout layout)
{
	size_t i;
	struct root_info *entry;
	struct format_ctx fctx;

	if (layout == BTRFS_LIST_LAYOUT_TABLE) {
		print_all_subvol_info_tab_head();
	} else if (layout == BTRFS_LIST_LAYOUT_JSON) {
		fmt_start(&fctx, btrfs_subvolume_rowspec, 1, 0);
		fmt_print_start_group(&fctx, "subvolume-list", JSON_TYPE_ARRAY);
	}

	for (i = 0; i < subvols->num; i++) {
		entry = &subvols->subvols[i];

		switch (layout) {
		case BTRFS_LIST_LAYOUT_DEFAULT:
			print_one_subvol_info_default(entry);
			break;
		case BTRFS_LIST_LAYOUT_TABLE:
			print_one_subvol_info_table(entry);
			break;
		case BTRFS_LIST_LAYOUT_JSON:
			print_one_subvol_info_json(&fctx, entry);
			break;
		}
	}

	if (layout == BTRFS_LIST_LAYOUT_JSON) {
		fmt_print_end_group(&fctx, "subvolume-list");
		fmt_end(&fctx);
	}
}

static void free_subvol_list(struct subvol_list *subvols)
{
	size_t i;

	if (subvols) {
		for (i = 0; i < subvols->num; i++)
			free(subvols->subvols[i].path);
		free(subvols);
	}
}

static struct subvol_list *btrfs_list_deleted_subvols(int fd,
				struct btrfs_list_filter_set *filter_set)
{
	struct subvol_list *subvols = NULL;
	uint64_t *ids = NULL;
	size_t i, n;
	enum btrfs_util_error err;
	int ret = -1;

	err = btrfs_util_deleted_subvolumes_fd(fd, &ids, &n);
	if (err) {
		error_btrfs_util(err);
		return NULL;
	}

	subvols = malloc(sizeof(*subvols) + n * sizeof(subvols->subvols[0]));
	if (!subvols) {
		error("out of memory");
		goto out;
	}

	subvols->num = 0;
	for (i = 0; i < n; i++) {
		struct root_info *subvol = &subvols->subvols[subvols->num];

		err = btrfs_util_subvolume_info_fd(fd, ids[i], &subvol->info);
		if (err == BTRFS_UTIL_ERROR_SUBVOLUME_NOT_FOUND) {
			/*
			 * The subvolume might have been cleaned up since it was
			 * returned.
			 */
			continue;
		} else if (err) {
			error_btrfs_util(err);
			goto out;
		}

		subvol->path = strdup("DELETED");
		if (!subvol->path)
			goto out;

		if (!filter_root(subvol, filter_set)) {
			free(subvol->path);
			continue;
		}

		subvols->num++;
	}

	ret = 0;
out:
	if (ret) {
		free_subvol_list(subvols);
		subvols = NULL;
	}
	free(ids);
	return subvols;
}

static struct subvol_list *btrfs_list_subvols(int fd,
				struct btrfs_list_filter_set *filter_set)
{
	struct subvol_list *subvols;
	size_t capacity = 4;
	struct btrfs_util_subvolume_iterator *iter;
	enum btrfs_util_error err;
	int ret = -1;

	subvols = malloc(sizeof(*subvols) +
			 capacity * sizeof(subvols->subvols[0]));
	if (!subvols) {
		error("out of memory");
		return NULL;
	}
	subvols->num = 0;

	err = btrfs_util_create_subvolume_iterator_fd(fd,
						      BTRFS_FS_TREE_OBJECTID, 0,
						      &iter);
	if (err) {
		iter = NULL;
		error_btrfs_util(err);
		goto out;
	}

	for (;;) {
		struct root_info subvol;

		err = btrfs_util_subvolume_iterator_next_info(iter,
							      &subvol.path,
							      &subvol.info);
		if (err == BTRFS_UTIL_ERROR_STOP_ITERATION) {
			break;
		} else if (err) {
			error_btrfs_util(err);
			goto out;
		}

		if (!filter_root(&subvol, filter_set)) {
			free(subvol.path);
			continue;
		}

		if (subvols->num >= capacity) {
			struct subvol_list *new_subvols;
			size_t new_capacity = max_t(size_t, 1, capacity * 2);

			new_subvols = realloc(subvols,
					      sizeof(*new_subvols) +
					      new_capacity *
					      sizeof(new_subvols->subvols[0]));
			if (!new_subvols) {
				error("out of memory");
				free(subvol.path);
				goto out;
			}

			subvols = new_subvols;
			capacity = new_capacity;
		}

		subvols->subvols[subvols->num] = subvol;
		subvols->num++;
	}

	ret = 0;
out:
	if (iter)
		btrfs_util_destroy_subvolume_iterator(iter);
	if (ret) {
		free_subvol_list(subvols);
		subvols = NULL;
	}
	return subvols;
}

static int btrfs_list_subvols_print(int fd, struct btrfs_list_filter_set *filter_set,
		       struct btrfs_list_comparer_set *comp_set,
		       enum btrfs_list_layout layout)
{
	struct subvol_list *subvols;

	if (filter_set->only_deleted)
		subvols = btrfs_list_deleted_subvols(fd, filter_set);
	else
		subvols = btrfs_list_subvols(fd, filter_set);
	if (!subvols)
		return -1;

	sort_subvols(comp_set, subvols);

	print_all_subvol_info(subvols, layout);
	free_subvol_list(subvols);

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

static int cmd_subvolume_list(const struct cmd_struct *cmd, int argc, char **argv)
{
	struct btrfs_list_filter_set *filter_set;
	struct btrfs_list_comparer_set *comparer_set;
	u64 flags = 0;
	int fd = -1;
	u64 top_id;
	char *top_path = NULL;
	int ret = -1, uerr = 0;
	char *subvol;
	bool is_list_all = false;
	bool is_only_in_path = false;
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
			filter_set->only_deleted = 1;
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
	fd = btrfs_open_dir(subvol);
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
	else if (!filter_set->only_deleted) {
		enum btrfs_util_error err;

		err = btrfs_util_subvolume_get_path_fd(fd, top_id, &top_path);
		if (err) {
			ret = -1;
			error_btrfs_util(err);
			goto out;
		}
		btrfs_list_setup_filter(&filter_set,
					BTRFS_LIST_FILTER_REMOVE_PATH_PREFIX,
					(u64)top_path);
	}

	/* by default we shall print the following columns*/
	btrfs_list_setup_print_column(BTRFS_LIST_OBJECTID);
	btrfs_list_setup_print_column(BTRFS_LIST_GENERATION);
	btrfs_list_setup_print_column(BTRFS_LIST_TOP_LEVEL);
	btrfs_list_setup_print_column(BTRFS_LIST_PATH);

	if (bconf.output_format == CMD_FORMAT_JSON)
		layout = BTRFS_LIST_LAYOUT_JSON;

	ret = btrfs_list_subvols_print(fd, filter_set, comparer_set, layout);

out:
	free(top_path);
	close(fd);
	if (filter_set)
		free(filter_set);
	if (comparer_set)
		free(comparer_set);
	if (uerr)
		usage(cmd, 1);
	return !!ret;
}
#if EXPERIMENTAL
DEFINE_COMMAND_WITH_FLAGS(subvolume_list, "list", CMD_FORMAT_JSON);
#else
DEFINE_SIMPLE_COMMAND(subvolume_list, "list");
#endif
