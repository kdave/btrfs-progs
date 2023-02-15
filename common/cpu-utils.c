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
#include <stdbool.h>
#include "common/cpu-utils.h"

unsigned long __cpu_flags = CPU_FLAG_NONE;
unsigned long __cpu_flags_orig = CPU_FLAG_NONE;

#ifdef __x86_64__
/*
 * Do manual cpuid as cpuid.h is not available for all versions.
 *
 * Plain cpuid(level) is __cpuidex(level, 0), otherwise look for the leaf/subleaf
 * modes at https://en.wikipedia.org/wiki/CPUID or compilers' cpuid.h.
 */

#ifndef __cpuidex
#define __cpuidex(leaf, subleaf, a, b, c, d)				\
	__asm__ __volatile__ ("cpuid\n\t"				\
			: "=a" (a), "=b" (b), "=c" (c), "=d" (d)	\
			: "0" (leaf), "2" (subleaf))
#endif

#else

#define __cpuidex(leaf, subleaf, a, b, c, d)				\
	do {								\
		a = 0;							\
		b = 0;							\
		c = 0;							\
		d = 0;							\
	} while(0)
#endif

#define FLAG(name)	if (__cpu_flags & CPU_FLAG_ ## name) printf(" " #name)
void cpu_print_flags(void) {
	printf("CPU flags: 0x%lx\n", __cpu_flags);
	printf("CPU features:");
	FLAG(SSE2);
	FLAG(SSSE3);
	FLAG(SSE41);
	FLAG(SSE42);
	FLAG(SHA);
	FLAG(AVX);
	FLAG(AVX2);
	putchar(10);
}
#undef FLAG

#ifdef __x86_64__

void cpu_detect_flags(void)
{
	unsigned int a, b, c, d;

	__builtin_cpu_init();
	__cpu_flags = CPU_FLAG_NONE;
	if (__builtin_cpu_supports("sse2"))
		__cpu_flags |= CPU_FLAG_SSE2;
	if (__builtin_cpu_supports("ssse3"))
		__cpu_flags |= CPU_FLAG_SSSE3;
	if (__builtin_cpu_supports("sse4.1"))
		__cpu_flags |= CPU_FLAG_SSE41;
	if (__builtin_cpu_supports("sse4.2"))
		__cpu_flags |= CPU_FLAG_SSE42;
	if (__builtin_cpu_supports("avx"))
		__cpu_flags |= CPU_FLAG_AVX;
	if (__builtin_cpu_supports("avx2"))
		__cpu_flags |= CPU_FLAG_AVX2;

	/* Flags unsupported by builtins */
	__cpuidex(7, 0, a, b, c, d);
	if (b & (1UL << 29))
		__cpu_flags |= CPU_FLAG_SHA;

	__cpu_flags_orig = __cpu_flags;
}

void cpu_set_level(unsigned long topbit)
{
	if (topbit)
		__cpu_flags &= (topbit << 1) - 1;
	else
		__cpu_flags = CPU_FLAG_NONE;
}

void cpu_reset_level(void)
{
	__cpu_flags = __cpu_flags_orig;
}

#else

void cpu_detect_flags() { }

void cpu_set_level(unsigned long topbit) { }

void cpu_reset_level(void) { }

#endif
