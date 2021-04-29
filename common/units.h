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

#ifndef __BTRFS_UNITS_H__
#define __BTRFS_UNITS_H__

#include "kerncompat.h"

/*
 * Output modes of size
 */
#define UNITS_RESERVED			(0)
#define UNITS_BYTES			(1)
#define UNITS_KBYTES			(2)
#define UNITS_MBYTES			(3)
#define UNITS_GBYTES			(4)
#define UNITS_TBYTES			(5)
#define UNITS_RAW			(1U << UNITS_MODE_SHIFT)
#define UNITS_BINARY			(2U << UNITS_MODE_SHIFT)
#define UNITS_DECIMAL			(3U << UNITS_MODE_SHIFT)
/* Interpret the u64 value as s64 */
#define UNITS_NEGATIVE			(4U << UNITS_MODE_SHIFT)
#define UNITS_MODE_MASK			((1U << UNITS_MODE_SHIFT) - 1)
#define UNITS_MODE_SHIFT		(8)
#define UNITS_HUMAN_BINARY		(UNITS_BINARY)
#define UNITS_HUMAN_DECIMAL		(UNITS_DECIMAL)
#define UNITS_HUMAN			(UNITS_HUMAN_BINARY)
#define UNITS_DEFAULT			(UNITS_HUMAN)

const char *pretty_size_mode(u64 size, unsigned mode);
int pretty_size_snprintf(u64 size, char *str, size_t str_size, unsigned unit_mode);
#define pretty_size(size) 	pretty_size_mode(size, UNITS_DEFAULT)
void units_set_mode(unsigned *units, unsigned mode);
void units_set_base(unsigned *units, unsigned base);
unsigned int get_unit_mode_from_arg(int *argc, char *argv[], int df_mode);

#endif
