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
#ifdef __x86_64__
#include <cpuid.h>
#endif
#include "common/cpu-utils.h"

unsigned long __cpu_flags = CPU_FLAG_NONE;
unsigned long __cpu_flags_orig = CPU_FLAG_NONE;

#define FLAG_X64(name)	if (__cpu_flags & CPU_FLAG_X64_ ## name) printf(" " #name)
void cpu_print_flags(void) {
	printf("CPU flags: 0x%lx\n", __cpu_flags);
	printf("CPU features:");
	FLAG_X64(SSE2);
	FLAG_X64(SSSE3);
	FLAG_X64(SSE41);
	FLAG_X64(SSE42);
	FLAG_X64(PCLMUL);
	FLAG_X64(SHA);
	FLAG_X64(AVX);
	FLAG_X64(AVX2);
	putchar(10);
}
#undef FLAG_X64

#ifdef __x86_64__

void cpu_detect_flags(void)
{
	unsigned int a, b, c, d;

	__builtin_cpu_init();
	__cpu_flags = CPU_FLAG_NONE;
	if (__builtin_cpu_supports("sse2"))
		__cpu_flags |= CPU_FLAG_X64_SSE2;
	if (__builtin_cpu_supports("ssse3"))
		__cpu_flags |= CPU_FLAG_X64_SSSE3;
	if (__builtin_cpu_supports("sse4.1"))
		__cpu_flags |= CPU_FLAG_X64_SSE41;
	if (__builtin_cpu_supports("sse4.2"))
		__cpu_flags |= CPU_FLAG_X64_SSE42;
#if HAVE___BUILTIN_CPU_SUPPORTS__PCLMUL
	if (__builtin_cpu_supports("pclmul"))
		__cpu_flags |= CPU_FLAG_X64_PCLMUL;
#endif
	if (__builtin_cpu_supports("avx"))
		__cpu_flags |= CPU_FLAG_X64_AVX;
	if (__builtin_cpu_supports("avx2"))
		__cpu_flags |= CPU_FLAG_X64_AVX2;

	/* Flags unsupported by builtins */
	__cpuid_count(7, 0, a, b, c, d);
	if (b & (1UL << 29))
		__cpu_flags |= CPU_FLAG_X64_SHA;

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
