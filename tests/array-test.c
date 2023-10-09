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
#include "common/array.h"

static void test_simple_create()
{
	struct array arr;
	int i;

	array_init(&arr, 0);
	printf("Create array with default intial capacity=%u\n", arr.capacity);
	array_append(&arr, (void *)0x1);
	array_append(&arr, (void *)0x2);
	array_append(&arr, (void *)0x3);
	for (i = 0; i < arr.length; i++)
		printf("array[%d]=%p\n", i, arr.data[i]);
	array_free(&arr);
}

static void test_simple_alloc_elems()
{
	struct array arr;
	int i;
	const int count = 1000000;

	array_init(&arr, 0);
	printf("Create array with default intial capacity=%u\n", arr.capacity);
	for (i = 0; i < count; i++) {
		char *tmp;
		int ret;

		ret = asprintf(&tmp, "element %d\n", i);
		if (ret < 0) {
			printf("Error creating element %d\n", i);
			exit(1);
		}
		array_append(&arr, tmp);
	}
	printf("Append %d element, length=%u, capacity=%u\n",
	       count, arr.length, arr.capacity);
	array_free_elements(&arr);
	printf("Clear all elements, length=%u, capacity=%u\n",
	       arr.length, arr.capacity);
	array_free(&arr);
}

int main(int argc, char **argv)
{
	int testno;
	static void (*tests[])() = {
		test_simple_create,
		test_simple_alloc_elems,
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
