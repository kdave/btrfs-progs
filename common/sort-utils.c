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
#include <ctype.h>
#include "common/sort-utils.h"
#include "common/messages.h"

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

/*
 * Append given sort by its @id from associated sortdef.
 *
 * Return: 0  if id is valid
 *         -1 if id not in sortdef
 */
int compare_add_sort_id(struct compare *comp, int id)
{
	int i;

	if (!comp->sortdef)
		return -1;

	if (id < 0)
		return -1;

	for (i = 0; i < SORT_MAX_KEYS; i++) {
		if (comp->sortdef[i].name == NULL)
			return -1;
		if (comp->sortdef[i].id == id) {
#ifdef __ANDROID__
			comp->id[comp->count] = id;
#endif
			comp->comp[comp->count] = comp->sortdef[i].comp;
			comp->count++;
			break;
		}
	}
	return 0;
}

/*
 * Consume word-like list of key names (coma separated) and return its id if
 * found in sortdef. The @next pointer is advanced to the next expected key start.
 * Empty and NULL @next is accepted.
 *
 * Key lookup is case insensitive.
 *
 * Return: id from sortdef if a matching
 *         -1 on error
 *         -2 end of buffer
 */
int compare_parse_key_to_id(const struct compare *comp, const char **next)
{
	const char *tmp = *next, *start = *next;

	if (!comp->sortdef)
		return -1;

	/* No sort string (use defaults), or last. */
	if (!*next || !**next)
		return -2;

	do {
		/* End of word. */
		if (*tmp == ',' || *tmp == 0) {
			/* Look up in sortdef. */
			for (int i = 0; comp->sortdef[i].name; i++) {
				int len = strlen(comp->sortdef[i].name);

				if (strncasecmp(start, comp->sortdef[i].name, len) == 0) {
					/* Point to last NUL. */
					*next = tmp;
					/* Or the next valid char. */
					if (*tmp)
						(*next)++;
					return comp->sortdef[i].id;
				}
			}
			/* Not found, report which one. */
			*next = start;
			return -1;
		}
		/* Invalid char found. */
		if (!isalnum(*tmp)) {
			*next = tmp;
			return -1;
		}
		tmp++;
	} while(1);

	/* Not found. */
	*next = start;
	return -1;
}

/* Read id of its associated sort @key.  Key lookup is case insensitive. */
int compare_key_id(const struct compare *comp, const char *key)
{
	if (!comp->sortdef)
		return -1;

	for (int i = 0; comp->sortdef[i].name; i++)
		if (strcasecmp(comp->sortdef[i].name, key) == 0)
			return comp->sortdef[i].id;
	return -1;
}

/* Read sort key name associated to @id. */
const char *compare_id_name(const struct compare *comp, int id)
{
	if (!comp->sortdef)
		return NULL;

	for (int i = 0; comp->sortdef[i].name; i++)
		if (comp->sortdef[i].id == id)
			return comp->sortdef[i].name;
	return NULL;
}

/*
 * Check if the given @id (must exist in the associated sortdef) enabled in
 * @comp.
 */
bool compare_has_id(const struct compare *comp, int id)
{
	int idx;

	if (!comp->sortdef)
		return false;

	idx = -1;
	for (int i = 0; comp->sortdef[i].name; i++)
		if (comp->sortdef[i].id == id)
			idx = i;

	if (idx < 0)
		return false;

	for (int i = 0; i < comp->count; i++)
		if (comp->comp[i] == comp->sortdef[idx].comp)
			return true;
	return false;
}

/*
 * Set up compare structure with associated sortdef from a user specified list
 * of keys.
 */
int compare_setup_sort(struct compare *comp, const struct sortdef *sdef, const char *def)
{
	const char *tmp;
	int id;

	tmp = def;
	do {
		id = compare_parse_key_to_id(comp, &tmp);
		if (id == -1) {
			error("unknown sort key: %s", tmp);
			return -1;
		}
		compare_add_sort_id(comp, id);
	} while (id >= 0);

	return 0;
}
