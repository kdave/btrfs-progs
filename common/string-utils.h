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

#ifndef __BTRFS_STRING_UTILS_H__
#define __BTRFS_STRING_UTILS_H__

#include "kerncompat.h"

int string_is_numerical(const char *str);
int string_has_prefix(const char *str, const char *prefix);

char *strncpy_null(char *dest, const char *src, size_t n);

/*
 * Helpers prefixed by arg_* can exit if the argument is invalid and are supposed
 * to be used when parsing command line options where the immediate exit is valid
 * error handling.
 */
u64 arg_strtou64(const char *str);
u64 arg_strtou64_with_suffix(const char *str);

#endif
