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

#ifndef __COMMON_SORT_UTILS_H__
#define __COMMON_SORT_UTILS_H__

#include <stdbool.h>
#include "common/compat.h"

/*
 * Example:

struct entry {
	int id;
	long size;
};

static int cmp_entry_id(const struct entry *a, const struct entry *b)
{
	return (a->id < b->id ? -1 :
		a->id > b->id ?  1 : 0);
}

static int cmp_entry_size(const struct entry *a, const struct entry *b)
{
	return (a->size < b->size ? -1 :
		a->size > b->size ?  1 : 0);
}

void test() {
	// User data
	struct entry entries[SIZE];
	// Comparator structure
	struct compare comp = { 0 };
	// Keys, item comparators, help text definitions
	struct sortdef sortit[] = {
		{ .name = "id",   .comp = (sort_cmp_t)cmp_entry_id,
		  .desc = "sort by id" },
		{ .name = "size", .comp = (sort_cmp_t)cmp_entry_size,
		  .desc = "sort by entry size" },
		SORTDEF_END
	};
	// List of keys to use for sort (e.g. from command line options)
	const char *sortby[] = { "size", "id" };

	compare_init(&comp, sortit);
	for (i = 0; i < sizeof(sortby) / sizeof(sortby[0]); i++) {
		bool descending = true;

		ret = compare_add_sort_key(&comp, sortby[i], descending);
		if (ret < 0) {
			printf("ERROR adding sort key %s\n", sortby[i]);
			break;
		}
	}
	qsort_r(entries, SIZE, sizeof(struct entry), (sort_r_cmp_t)compare_cmp_multi,
		&comp);
}
 */

#define SORTDEF_END		{ .name = NULL, .comp = NULL }
#define SORT_MAX_KEYS		32

typedef int (*sort_cmp_t)(const void *a, const void *b);
typedef int (*sort_r_cmp_t)(const void *a, const void *b, void *data);

#define SORTDEF_END { .name = NULL,   .comp = NULL }

struct sortdef {
	const char *name;
	const char *desc;
	sort_cmp_t comp;
	/* User defined identifier of this sort key. */
	int id;
};

struct compare {
	sort_cmp_t comp[SORT_MAX_KEYS];
	unsigned long invert_map;
	int count;
	const struct sortdef *sortdef;
};

int compare_init(struct compare *comp, const struct sortdef *sortdef);
int compare_cmp_multi(const void *a, const void *b, const struct compare *comp);
int compare_add_sort_key(struct compare *comp, const char *key, bool descending);
int compare_parse_key_to_id(const struct compare *comp, const char **next, bool *descending);
int compare_add_sort_id(struct compare *comp, int id, bool descending);
int compare_key_id(const struct compare *comp, const char *key);
const char *compare_id_name(const struct compare *comp, int id);
bool compare_has_id(const struct compare *comp, int id);
int compare_setup_sort(struct compare *comp, const struct sortdef *sdef, const char *def);

#endif
