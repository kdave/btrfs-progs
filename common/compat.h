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

#ifndef __BTRFS_COMPAT_H__
#define __BTRFS_COMPAT_H__

#ifdef __ANDROID__

#include <pthread.h>

/*
 * Add missing pthread related definitions in Android.
 */

#define PTHREAD_CANCELED			((void *)-1)

#define PTHREAD_CANCEL_DEFERRED			0
#define PTHREAD_CANCEL_ASYNCHRONOUS		0
#define PTHREAD_CANCEL_ENABLE			0
#define PTHREAD_CANCEL_DISABLE			0

int pthread_setcanceltype(int type, int *oldtype);
int pthread_setcancelstate(int state, int *oldstate);
int pthread_cancel(pthread_t thread_id);

int btrfs_set_thread_exit_handler();
void btrfs_thread_exit_handler(int sig);

/**
 * @brief A compatible implementation of the GNU C Library (Glibc) qsort_r.
 *
 * Sorts an array using the quicksort algorithm. This function is thread-safe
 * by allowing a custom context pointer (arg) to be passed to the comparison
 * function.
 *
 * @param base    A pointer to the first element of the array to be sorted.
 * @param nel     The number of elements in the array.
 * @param width   The size in bytes of each element in the array.
 * @param compare The comparison function, which takes two elements and a context
 *                pointer. The function must return:
 *                - A negative integer if the first element is less than the second.
 *                - Zero if the elements are equal.
 *                - A positive integer if the first element is greater than the second.
 * @param arg     The custom pointer passed to the comparison function.
 */
void qsort_r(void *base, size_t nel, size_t width,
	     int (*compare)(const void *a, const void *b, void *context),
	     void *arg);

#endif

#endif
