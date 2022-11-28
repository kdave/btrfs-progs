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
#include "common/messages.h"
#include "common/string-table.h"

/*
 * Create an array of char* which will point to table cell strings
 */
struct string_table *table_create(unsigned int columns, unsigned int rows)
{
	struct string_table *tab;
	size_t size;

	size = sizeof(struct string_table) + rows * columns * sizeof(char*);
	tab = calloc(1, size);

	if (!tab)
		return NULL;

	tab->ncols = columns;
	tab->nrows = rows;
	tab->spacing = STRING_TABLE_SPACING_1;

	return tab;
}

/*
 * This is like a vprintf, but stores the results in a cell of the table.
 */
__attribute__ ((format (printf, 4, 0)))
char *table_vprintf(struct string_table *tab, unsigned int column, unsigned int row,
			  const char *fmt, va_list ap)
{
	unsigned int idx = tab->ncols * row + column;
	char *msg = calloc(100, 1);

	if (!msg)
		return NULL;

	if (column >= tab->ncols || row >= tab->nrows) {
		error("attempt to write outside of table: col %u row %u fmt %s",
			column, row, fmt);
		return NULL;
	}

	if (tab->cells[idx])
		free(tab->cells[idx]);
	tab->cells[idx] = msg;
	vsnprintf(msg, 99, fmt, ap);

	return msg;
}

/*
 * This is like a printf, but stores the results in a cell of the table.
 */
__attribute__ ((format (printf, 4, 5)))
char *table_printf(struct string_table *tab, unsigned int column, unsigned int row,
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
 * Print the table to stdout, interpret the alignment and expand specifiers.
 *
 * Formatting:
 * <TEXT - the TEXT is left aligned
 * >TEXT - the TEXT is right aligned
 * =     - the cell text will be filled by ===== (column width)
 * *C    - the cell text will be filled by character C (column width)
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
			if (i != (tab->ncols - 1)) {
				putchar(' ');
				if (tab->spacing == STRING_TABLE_SPACING_2)
					putchar(' ');
			}
		}
		putchar('\n');
	}
}

/*
 *  Deallocate a table and all of its content
 */
void table_free(struct string_table *tab)
{
	unsigned int i, count;

	count = tab->ncols * tab->nrows;

	for (i = 0; i < count; i++)
		if (tab->cells[i])
			free(tab->cells[i]);

	free(tab);
}
