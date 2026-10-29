/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COMPILER_ATTRIBUTES_H
#define _COMPILER_ATTRIBUTES_H

/*
 * Copied from linux.git/include/linux/compiler_attributes.h, only contains the
 * attributes btrfs-progs needs.
 *
 * This header must stay free of any other includes, so it can be pulled in by
 * any header which only needs a compiler attribute.
 */

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-cleanup-variable-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#cleanup
 */
#define __cleanup(func)			__attribute__((__cleanup__(func)))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-format-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#format
 */
#define __printf(a, b)			__attribute__((__format__(printf, a, b)))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-warn_005funused_005fresult-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#nodiscard-warn-unused-result
 */
#define __must_check			__attribute__((__warn_unused_result__))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-unused-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#maybe-unused-unused
 */
#ifndef __maybe_unused
#define __maybe_unused			__attribute__((__unused__))
#endif

#endif
