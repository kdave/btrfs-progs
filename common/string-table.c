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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "common/string-table.h"

/*
 *  This function create an array of char * which will represent a table
 */
struct string_table *table_create(int columns, int rows)
{
	struct string_table *tab;
	int size;

	size = sizeof(struct string_table) + rows * columns * sizeof(char*);
	tab = calloc(1, size);

	if (!tab)
		return NULL;

	tab->ncols = columns;
	tab->nrows = rows;

	return tab;
}

/*
 * This function is like a vprintf, but store the results in a cell of
 * the table.
 * If fmt  starts with '<', the text is left aligned; if fmt starts with
 * '>' the text is right aligned. If fmt is equal to '=' the text will
 * be replaced by a '=====' dimensioned on the basis of the column width
 */
__attribute__ ((format (printf, 4, 0)))
char *table_vprintf(struct string_table *tab, int column, int row,
			  const char *fmt, va_list ap)
{
	int idx = tab->ncols * row + column;
	char *msg = calloc(100, 1);

	if (!msg)
		return NULL;

	if (tab->cells[idx])
		free(tab->cells[idx]);
	tab->cells[idx] = msg;
	vsnprintf(msg, 99, fmt, ap);

	return msg;
}

/*
 * This function is like a printf, but store the results in a cell of
 * the table.
 */
__attribute__ ((format (printf, 4, 5)))
char *table_printf(struct string_table *tab, int column, int row,
			  const char *fmt, ...)
{
	va_list ap;
	char *ret;

	va_start(ap, fmt);
	ret = table_vprintf(tab, column, row, fmt, ap);
	va_end(ap);

	return ret;
}

/*
 * This function dumps the table. Every "=" string will be replaced by
 * a "=======" length as the column
 */
void table_dump(struct string_table *tab)
{
	int sizes[tab->ncols];
	int i, j;

	for (i = 0; i < tab->ncols; i++) {
		sizes[i] = 0;
		for (j = 0; j < tab->nrows; j++) {
			int idx = i + j * tab->ncols;
			int len;

			if (!tab->cells[idx])
				continue;

			len = strlen(tab->cells[idx]) - 1;
			if (len == 0 || tab->cells[idx][0] == '*')
				continue;

			if (len > sizes[i])
				sizes[i] = len;
		}
	}

	for (j = 0; j < tab->nrows; j++) {
		for (i = 0; i < tab->ncols; i++) {
			int idx = i + j * tab->ncols;
			char *cell = tab->cells[idx];

			if (!cell || !strlen(cell)) {
				printf("%*s", sizes[i], "");
			} else if (cell && cell[0] == '*' && cell[1]) {
				int k = sizes[i];

				while (k--)
					putchar(cell[1]);
			} else {
				printf("%*s",
					cell[0] == '<' ? -sizes[i] : sizes[i],
					cell + 1);
			}
			if (i != (tab->ncols - 1))
				putchar(' ');
		}
		putchar('\n');
	}
}

/*
 *  Deallocate a table and all of its content
 */
void table_free(struct string_table *tab)
{
	int i, count;

	count = tab->ncols * tab->nrows;

	for (i = 0; i < count; i++)
		if (tab->cells[i])
			free(tab->cells[i]);

	free(tab);
}
