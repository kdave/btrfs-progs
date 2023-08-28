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

#ifndef __INJECT_ERROR_H__
#define __INJECT_ERROR_H__

#include <stdbool.h>

#ifdef INJECT

#define inject_error(cookie)	__inject_error((cookie), __FILE__, __LINE__)
bool __inject_error(unsigned long cookie, const char *file, int line);

#else

#define inject_error(cookie)		(false)

#endif

#endif
