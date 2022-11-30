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

#include <stdio.h>
#include <stdlib.h>
#include "common/utils.h"
#include "common/string-table.h"

void test_simple_create_free()
{
	struct string_table *tab;

	tab = table_create(2, 2);
	if (!tab) {
		fprintf(stderr, "ERROR: cannot alocate table\n");
		return;
	}
	table_printf(tab, 0, 0, ">00");
	table_printf(tab, 0, 1, "<01");
	table_printf(tab, 1, 0, ">10");
	table_printf(tab, 1, 1, "<11");

	table_dump(tab);

	table_free(tab);
}

void test_simple_header()
{
	struct string_table *tab;
	int i;

	tab = table_create(2, 6);
	if (!tab) {
		fprintf(stderr, "ERROR: cannot alocate table\n");
		return;
	}
	tab->hrows = 2;
	table_printf(tab, 0, 0, ">Id");
	table_printf(tab, 1, 0, ">Name");
	table_printf(tab, 0, 1, "*-");
	table_printf(tab, 1, 1, "*-");

	for (i = tab->hrows; i < tab->nrows; i++) {
		table_printf(tab, 0, i, ">%d", 1U << i);
		table_printf(tab, 1, i, "<%d", 100 * i);
	}

	puts("start");
	table_dump_header(tab);
	puts("separator");
	table_dump_body(tab);
	puts("end");

	table_free(tab);
}

void test_simple_paginate()
{
	struct string_table *tab;
	unsigned int page_size = 4;
	unsigned int pages = 4;
	int i;

	tab = table_create(2, 2 + page_size * pages);
	if (!tab) {
		fprintf(stderr, "ERROR: cannot alocate table\n");
		return;
	}
	tab->hrows = 2;
	table_printf(tab, 0, 0, ">Id");
	table_printf(tab, 1, 0, ">Name");
	table_printf(tab, 0, 1, "*-");
	table_printf(tab, 1, 1, "*-");

	for (i = tab->hrows; i < tab->nrows; i++) {
		table_printf(tab, 0, i, ">%d", 10 * (i - tab->hrows + 1));
		table_printf(tab, 1, i, "<Text %d", 100 * i);
	}

	puts("start");
	for (i = 0; i < pages; i++) {
		unsigned int start = tab->hrows + i * page_size;

		table_dump_header(tab);
		table_dump_range(tab, start, start + page_size - 1);
		puts("paginator");
	}
	puts("end");

	table_free(tab);
}

int main(int argc, char **argv)
{
	int testno;
	static void (*tests[])() = {
		test_simple_create_free,
		test_simple_header,
		test_simple_paginate,
	};

	/* Without arguments, print the number of tests available */
	if (argc == 1) {
		printf("%zu\n", ARRAY_SIZE(tests));
		return 0;
	}
	testno = atoi(argv[1]);
	testno--;

	if (testno < 0 || testno >= ARRAY_SIZE(tests)) {
		fprintf(stderr, "ERROR: test number %d is out of range (max %zu)\n",
				testno + 1, ARRAY_SIZE(tests));
		return 1;
	}
	tests[testno]();

	return 0;
}
