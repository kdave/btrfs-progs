/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
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

#include "kerncompat.h"
#include "radix-tree.h"

#define BIT_ARRAY_BYTES 256
#define BIT_RADIX_BITS_PER_ARRAY ((BIT_ARRAY_BYTES - sizeof(unsigned long)) * 8)

int set_radix_bit(struct radix_tree_root *radix, unsigned long bit)
{
	unsigned long *bits;
	unsigned long slot;
	int bit_slot;
	int ret;

	slot = bit / BIT_RADIX_BITS_PER_ARRAY;
	bit_slot = bit % BIT_RADIX_BITS_PER_ARRAY;

	bits = radix_tree_lookup(radix, slot);
	if (!bits) {
		bits = malloc(BIT_ARRAY_BYTES);
		if (!bits)
			return -ENOMEM;
		memset(bits + 1, 0, BIT_ARRAY_BYTES - sizeof(unsigned long));
		bits[0] = slot;
		radix_tree_preload(GFP_NOFS);
		ret = radix_tree_insert(radix, slot, bits);
		radix_tree_preload_end();
		if (ret)
			return ret;
	}
	__set_bit(bit_slot, bits + 1);
	return 0;
}

int test_radix_bit(struct radix_tree_root *radix, unsigned long bit)
{
	unsigned long *bits;
	unsigned long slot;
	int bit_slot;

	slot = bit / BIT_RADIX_BITS_PER_ARRAY;
	bit_slot = bit % BIT_RADIX_BITS_PER_ARRAY;

	bits = radix_tree_lookup(radix, slot);
	if (!bits)
		return 0;
	return test_bit(bit_slot, bits + 1);
}

int clear_radix_bit(struct radix_tree_root *radix, unsigned long bit)
{
	unsigned long *bits;
	unsigned long slot;
	int bit_slot;
	int i;
	int empty = 1;
	slot = bit / BIT_RADIX_BITS_PER_ARRAY;
	bit_slot = bit % BIT_RADIX_BITS_PER_ARRAY;

	bits = radix_tree_lookup(radix, slot);
	if (!bits)
		return 0;
	__clear_bit(bit_slot, bits + 1);
	for (i = 1; i < BIT_ARRAY_BYTES / sizeof(unsigned long); i++) {
		if (bits[i]) {
			empty = 0;
			break;
		}
	}
	if (empty) {
		bits = radix_tree_delete(radix, slot);
		BUG_ON(!bits);
		free(bits);
	}
	return 0;
}
 
#define BITOP_WORD(nr)		((nr) / BITS_PER_LONG)

/**
 * __ffs - find first bit in word.
 * @word: The word to search
 *
 * Undefined if no bit exists, so code should check against 0 first.
 */
static unsigned long __ffs(unsigned long word)
{
	int num = 0;

	if (sizeof(long) == 8 && (word & 0xffffffff) == 0) {
		num += 32;
		word >>= sizeof(long) * 4;
	}
	if ((word & 0xffff) == 0) {
		num += 16;
		word >>= 16;
	}
	if ((word & 0xff) == 0) {
		num += 8;
		word >>= 8;
	}
	if ((word & 0xf) == 0) {
		num += 4;
		word >>= 4;
	}
	if ((word & 0x3) == 0) {
		num += 2;
		word >>= 2;
	}
	if ((word & 0x1) == 0)
		num += 1;
	return num;
}

/**
 * find_next_bit - find the next set bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @size: The maximum size to search
 */
unsigned long find_next_bit(const unsigned long *addr, unsigned long size,
		unsigned long offset)
{
	const unsigned long *p = addr + BITOP_WORD(offset);
	unsigned long result = offset & ~(BITS_PER_LONG-1);
	unsigned long tmp;

	if (offset >= size)
		return size;
	size -= result;
	offset %= BITS_PER_LONG;
	if (offset) {
		tmp = *(p++);
		tmp &= (~0UL << offset);
		if (size < BITS_PER_LONG)
			goto found_first;
		if (tmp)
			goto found_middle;
		size -= BITS_PER_LONG;
		result += BITS_PER_LONG;
	}
	while (size & ~(BITS_PER_LONG-1)) {
		if ((tmp = *(p++)))
			goto found_middle;
		result += BITS_PER_LONG;
		size -= BITS_PER_LONG;
	}
	if (!size)
		return result;
	tmp = *p;

found_first:
	tmp &= (~0UL >> (BITS_PER_LONG - size));
	if (tmp == 0UL)		/* Are any bits set? */
		return result + size;	/* Nope. */
found_middle:
	return result + __ffs(tmp);
}

int find_first_radix_bit(struct radix_tree_root *radix, unsigned long *retbits,
			 unsigned long start, int nr)
{
	unsigned long *bits;
	unsigned long *gang[4];
	int found;
	int ret;
	int i;
	int total_found = 0;
	unsigned long slot;

	slot = start / BIT_RADIX_BITS_PER_ARRAY;
	ret = radix_tree_gang_lookup(radix, (void *)gang, slot,
				     ARRAY_SIZE(gang));
	found = start % BIT_RADIX_BITS_PER_ARRAY;
	for (i = 0; i < ret && nr > 0; i++) {
		bits = gang[i];
		while(nr > 0) {
			found = find_next_bit(bits + 1,
					      BIT_RADIX_BITS_PER_ARRAY,
					      found);
			if (found < BIT_RADIX_BITS_PER_ARRAY) {
				*retbits = bits[0] *
					BIT_RADIX_BITS_PER_ARRAY + found;
				retbits++;
				nr--;
				total_found++;
				found++;
			} else
				break;
		}
		found = 0;
	}
	return total_found;
}
