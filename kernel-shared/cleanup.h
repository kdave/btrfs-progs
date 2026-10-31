/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Cross-ported from kernel's include/linux/cleanup.h.
 *
 * With unused macros like return_ptr() and no_free_ptr() removed, along with
 * comments mentioning them.
 */
#ifndef _LINUX_CLEANUP_H
#define _LINUX_CLEANUP_H

#include "kerncompat.h"
#include "compiler_attributes.h"

/*
 * DEFINE_FREE(name, type, free):
 *	simple helper macro that defines the required wrapper for a __free()
 *	based cleanup function. @free is an expression using '_T' to access the
 *	variable. @free should typically include a NULL test before calling a
 *	function.
 *
 * __free(name):
 *	variable attribute to add a scoped based cleanup to the variable.
 */

#define DEFINE_FREE(_name, _type, _free) \
	static __always_inline void __free_##_name(void *p) { _type _T = *(_type *)p; _free; }

#define __free(_name)	__cleanup(__free_##_name)

#endif
