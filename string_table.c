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

#include "string_table.h"

/*
 *  This function create an array of char * which will represent a table
 */
struct string_table *table_create(int columns, int rows)
{
	struct string_table *p;
	int size;

	size = sizeof( struct string_table ) +
		rows * columns* sizeof(char *);
	p = calloc(1, size);

	if (!p) return NULL;

	p->ncols = columns;
	p->nrows = rows;

	return p;
}

/*
 * This function is like a vprintf, but store the results in a cell of
 * the table.
 * If fmt  starts with '<', the text is left aligned; if fmt starts with
 * '>' the text is right aligned. If fmt is equal to '=' the text will
 * be replaced by a '=====' dimensioned on the basis of the column width
 */
char *table_vprintf(struct string_table *tab, int column, int row,
			  char *fmt, va_list ap)
{
	int idx = tab->ncols*row+column;
	char *msg = calloc(100, sizeof(char));

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
char *table_printf(struct string_table *tab, int column, int row,
			  char *fmt, ...)
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
	int	sizes[tab->ncols];
	int	i, j;

	for (i = 0 ; i < tab->ncols ; i++) {
		sizes[i] = 0;
		for (j = 0 ; j < tab->nrows ; j++) {
			int idx = i + j*tab->ncols;
			int s;

			if (!tab->cells[idx])
				continue;

			s = strlen(tab->cells[idx]) - 1;
			if (s < 1 || tab->cells[idx][0] == '=')
				continue;

			if (s > sizes[i])
				sizes[i] = s;
		}
	}


	for (j = 0 ; j < tab->nrows ; j++) {
		for (i = 0 ; i < tab->ncols ; i++) {

			int idx = i + j*tab->ncols;
			char *s = tab->cells[idx];

			if (!s|| !strlen(s)) {
				printf("%*s", sizes[i], "");
			} else if (s && s[0] == '=') {
				int k = sizes[i];
				while(k--)
					putchar('=');
			} else {
				printf("%*s",
					s[0] == '<' ? -sizes[i] : sizes[i],
					s+1);
			}
			if (i != (tab->ncols - 1))
				putchar(' ');
		}
		putchar('\n');
	}

}

/*
 *  Deallocate a tabular and all its content
 */

void table_free(struct string_table *tab)
{

	int	i, count;

	count = tab->ncols * tab->nrows;

	for (i=0 ; i < count ; i++)
		if (tab->cells[i])
			free(tab->cells[i]);

	free(tab);

}
