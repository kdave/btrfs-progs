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

#include "kerncompat.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

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

__attribute__ ((format (printf, 1, 2)))
void internal_error(const char *fmt, ...);

/*
 * Level of messages that must be printed by default (in case the verbosity
 * options haven't been set by the user) due to backward compatibility reasons
 * where applications may expect the output.
 */
#define LOG_ALWAYS						(-1)
/*
 * Default level for any messages that should be printed by default, a one line
 * summary or with more details. Applications should not rely on such messages.
 */
#define LOG_DEFAULT						(1)
/*
 * Information about the ongoing actions, high level description
 */
#define LOG_INFO						(2)
/*
 * Verbose description and individual steps of the previous level
 */
#define LOG_VERBOSE						(3)
/*
 * Anything that should not be normally printed but can be useful for debugging
 */
#define LOG_DEBUG						(4)

__attribute__ ((format (printf, 2, 3)))
void pr_verbose(int level, const char *fmt, ...);

__attribute__ ((format (printf, 2, 3)))
void pr_stderr(int level, const char *fmt, ...);

/*
 * Commonly used errors
 */
enum common_error {
	ERROR_MSG_MEMORY,
	ERROR_MSG_START_TRANS,
	ERROR_MSG_COMMIT_TRANS,
};

__attribute__ ((format (printf, 2, 3)))
void error_msg(enum common_error error, const char *msg, ...);

#endif
