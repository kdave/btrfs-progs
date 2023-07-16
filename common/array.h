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

#ifndef __COMMON_ARRAY_H__
#define __COMMON_ARRAY_H__

/*
 * Extensible array of pointers.
 */
struct array {
	void **data;
	unsigned int length;
	unsigned int capacity;
};

int array_init(struct array *arr, unsigned int capacity);
void array_free(struct array *arr);
void array_free_elements(struct array *arr);
void array_clear(struct array *arr);
void array_use_capacity(struct array *arr);
int array_append(struct array *arr, void *element);

#endif
