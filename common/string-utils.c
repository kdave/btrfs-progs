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

#include "kerncompat.h"
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include "common/string-utils.h"
#include "common/messages.h"
#include "common/parse-utils.h"

int string_is_numerical(const char *str)
{
	if (!str)
		return 0;
	if (!(*str >= '0' && *str <= '9'))
		return 0;
	while (*str >= '0' && *str <= '9')
		str++;
	if (*str != '\0')
		return 0;
	return 1;
}

int string_has_prefix(const char *str, const char *prefix)
{
	for (; ; str++, prefix++)
		if (!*prefix)
			return 0;
		else if (*str != *prefix)
			return (unsigned char)*prefix - (unsigned char)*str;
}

/*
 * strncpy_null - strncpy with null termination
 * @dest:	the target array
 * @src:	the source string
 * @n:		maximum bytes to copy (size of *dest)
 *
 * Like strncpy, but ensures destination is null-terminated.
 *
 * Copies the string pointed to by src, including the terminating null
 * byte ('\0'), to the buffer pointed to by dest, up to a maximum
 * of n bytes.  Then ensure that dest is null-terminated.
 */
char *strncpy_null(char *dest, const char *src, size_t n)
{
	strncpy(dest, src, n);
	if (n > 0)
		dest[n - 1] = '\0';
	return dest;
}

/*
 * This function should be only used when parsing command arg, it won't return
 * error to its caller and rather exit directly just like usage().
 */
u64 arg_strtou64(const char *str)
{
	u64 value;
	int ret;

	ret = parse_u64(str, &value);
	if (ret == -ERANGE) {
		error("%s is too large", str);
		exit(1);
	} else if (ret == -EINVAL) {
		if (str[0] == '-')
			error("%s: negative value is invalid", str);
		else
			error("%s is not a valid numeric value", str);
		exit(1);
	}
	return value;
}

u64 arg_strtou64_with_suffix(const char *str)
{
	u64 value;
	int ret;

	ret = parse_u64_with_suffix(str, &value);
	if (ret == -ERANGE) {
		error("%s is too large", str);
		exit(1);
	} else if (ret == -EINVAL) {
		error("%s is not a valid numeric value with supported size suffixes", str);
		exit(1);
	} else if (ret < 0) {
		errno = -ret;
		error("failed to parse string '%s': %m", str);
		exit(1);
	}
	return value;
}
