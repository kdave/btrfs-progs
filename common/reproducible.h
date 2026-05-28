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

#ifndef __BTRFS_REPRODUCIBLE_H__
#define __BTRFS_REPRODUCIBLE_H__

#include <time.h>

/*
 * Return SOURCE_DATE_EPOCH as a time_t, or time(NULL) when it is unset
 * or empty. Errors out if it is set to something other than a
 * non-negative integer that fits in time_t.
 */
time_t reproducible_now(void);

#endif
