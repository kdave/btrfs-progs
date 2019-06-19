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

#ifndef __BTRFS_MESSAGES_H__
#define __BTRFS_MESSAGES_H__

#ifdef DEBUG_VERBOSE_ERROR
#define	PRINT_VERBOSE_ERROR	fprintf(stderr, "%s:%d:", __FILE__, __LINE__)
#else
#define PRINT_VERBOSE_ERROR
#endif

#ifdef DEBUG_TRACE_ON_ERROR
#define PRINT_TRACE_ON_ERROR	print_trace()
#else
#define PRINT_TRACE_ON_ERROR	do { } while (0)
#endif

#ifdef DEBUG_ABORT_ON_ERROR
#define DO_ABORT_ON_ERROR	abort()
#else
#define DO_ABORT_ON_ERROR	do { } while (0)
#endif

#define error(fmt, ...)							\
	do {								\
		PRINT_TRACE_ON_ERROR;					\
		PRINT_VERBOSE_ERROR;					\
		__btrfs_error((fmt), ##__VA_ARGS__);			\
		DO_ABORT_ON_ERROR;					\
	} while (0)

#define error_on(cond, fmt, ...)					\
	do {								\
		if ((cond))						\
			PRINT_TRACE_ON_ERROR;				\
		if ((cond))						\
			PRINT_VERBOSE_ERROR;				\
		__btrfs_error_on((cond), (fmt), ##__VA_ARGS__);		\
		if ((cond))						\
			DO_ABORT_ON_ERROR;				\
	} while (0)

#define error_btrfs_util(err)						\
	do {								\
		const char *errno_str = strerror(errno);		\
		const char *lib_str = btrfs_util_strerror(err);		\
		PRINT_TRACE_ON_ERROR;					\
		PRINT_VERBOSE_ERROR;					\
		if (lib_str && strcmp(errno_str, lib_str) != 0)		\
			__btrfs_error("%s: %m", lib_str);		\
		else							\
			__btrfs_error("%m");				\
		DO_ABORT_ON_ERROR;					\
	} while (0)

#define warning(fmt, ...)						\
	do {								\
		PRINT_TRACE_ON_ERROR;					\
		PRINT_VERBOSE_ERROR;					\
		__btrfs_warning((fmt), ##__VA_ARGS__);			\
	} while (0)

#define warning_on(cond, fmt, ...)					\
	do {								\
		if ((cond))						\
			PRINT_TRACE_ON_ERROR;				\
		if ((cond))						\
			PRINT_VERBOSE_ERROR;				\
		__btrfs_warning_on((cond), (fmt), ##__VA_ARGS__);	\
	} while (0)

__attribute__ ((format (printf, 1, 2)))
void __btrfs_warning(const char *fmt, ...);

__attribute__ ((format (printf, 1, 2)))
void __btrfs_error(const char *fmt, ...);

__attribute__ ((format (printf, 2, 3)))
int __btrfs_warning_on(int condition, const char *fmt, ...);

__attribute__ ((format (printf, 2, 3)))
int __btrfs_error_on(int condition, const char *fmt, ...);

#endif
