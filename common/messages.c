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
#include <stdarg.h>
#include "common/messages.h"

__attribute__ ((format (printf, 1, 2)))
void __btrfs_warning(const char *fmt, ...)
{
	va_list args;

	fputs("WARNING: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

__attribute__ ((format (printf, 1, 2)))
void __btrfs_error(const char *fmt, ...)
{
	va_list args;

	fputs("ERROR: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
}

__attribute__ ((format (printf, 2, 3)))
int __btrfs_warning_on(int condition, const char *fmt, ...)
{
	va_list args;

	if (!condition)
		return 0;

	fputs("WARNING: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);

	return 1;
}

__attribute__ ((format (printf, 2, 3)))
int __btrfs_error_on(int condition, const char *fmt, ...)
{
	va_list args;

	if (!condition)
		return 0;

	fputs("ERROR: ", stderr);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);

	return 1;
}
