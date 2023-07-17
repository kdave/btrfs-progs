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

#include <strings.h>
#include <string.h>
#include "common/sort-utils.h"

int compare_init(struct compare *comp, const struct sortdef *sortdef)
{
	memset(comp, 0, sizeof(struct compare));
	comp->sortdef = sortdef;
	return 0;
}

int compare_cmp_multi(const void *a, const void *b, const struct compare *comp)
{
	for (int i = 0; i < comp->count; i++) {
		int ret;

		ret = comp->comp[i](a,b);
		if (ret != 0)
			return (comp->invert_map & (1U << i)) ? -ret : ret;
	}
	return 0;
}

int compare_add_sort_key(struct compare *comp, const char *key)
{
	int i;

	if (!comp->sortdef)
		return -1;

	for (i = 0; i < 32; i++) {
		if (comp->sortdef[i].name == NULL)
			return -1;
		if (strcasecmp(key, comp->sortdef[i].name) == 0) {
			comp->comp[comp->count] = comp->sortdef[i].comp;
			comp->count++;
			break;
		}
	}
	return 0;
}
