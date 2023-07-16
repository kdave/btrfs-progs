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

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/array.h"

/*
 * Extensible array of pointers. Length is the number of user defined pointers,
 * capacity is the whole allocated size. Pointers are potentially unstable after
 * an append operation. Array initialized to all zeros is valid and can be extended.
 */

static const int alloc_increment = 32;

/* Initialize new array, preallocate @capacity elemennts. */
int array_init(struct array *arr, unsigned int capacity)
{
	void *tmp;

	arr->data = NULL;
	arr->length = 0;
	arr->capacity = capacity;
	if (arr->capacity == 0)
		arr->capacity = alloc_increment;

	tmp = calloc(arr->capacity, sizeof(void *));
	if (!tmp)
		return -1;
	arr->data = tmp;
	return 0;
}

/* Free internal data array. */
void array_free(struct array *arr)
{
	free(arr->data);
	arr->length = 0;
	arr->capacity = 0;
	arr->data = NULL;
}

/* Free all elements up to length. */
void array_free_elements(struct array *arr)
{
	unsigned int i;

	for (i = 0; i < arr->length; i++) {
		free(arr->data[i]);
		arr->data[i] = NULL;
	}
	arr->length = 0;
}

/* Reset all elements to NULL up to capacity. */
void array_clear(struct array *arr)
{
	for (unsigned int i = 0; i < arr->capacity; i++)
		arr->data[i] = NULL;
}

/* Use the whole capacity for elements. */
void array_use_capacity(struct array *arr)
{
	arr->length = arr->capacity;
}

/* Append a new element (increas length), extend the array if needed. */
int array_append(struct array *arr, void *element)
{
	if (arr->length == arr->capacity) {
		void **tmp;

		tmp = realloc(arr->data, (arr->capacity + alloc_increment) * sizeof(void *));
		if (!tmp)
			return -1;
		arr->data = tmp;
		memset(&arr->data[arr->capacity], 0, alloc_increment * sizeof(void *));
		arr->capacity += alloc_increment;
	}
	arr->data[arr->length] = element;
	arr->length++;
	return 0;
}
