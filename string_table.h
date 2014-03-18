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

#ifndef STRING_TABLE_H
#define STRING_TABLE_H

struct string_table {

	int	ncols, nrows;
	char	*cells[];

};


struct string_table *table_create(int columns, int rows);
char *table_printf(struct string_table *tab, int column, int row,
			  char *fmt, ...);
char *table_vprintf(struct string_table *tab, int column, int row,
			  char *fmt, va_list ap);
void table_dump(struct string_table *tab);
void table_free(struct string_table *);

#endif
