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
#ifdef __aarch64__
#include <sys/auxv.h>
#endif
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
	FLAG(PCLMUL);
	FLAG(SHA);
	FLAG(AVX);
	FLAG(AVX2);
	FLAG(ARMV8_CRC32);
	FLAG(ARMV8_PMULL);
	FLAG(VPCLMUL_AVX512);
	putchar(10);
}
#undef FLAG

#ifdef __x86_64__

/*
 * VPCLMULQDQ with AVX-512 as the kernel's lib/crc/x86/ uses it, its
 * have_vpclmul() && have_avx512(): VPCLMULQDQ, AVX2, AVX512BW and AVX512VL,
 * the OS saves the AVX-512 state, and the CPU is not one of the Intel models
 * that slow down with 512-bit vectors (the kernel's zmm_exclusion_list, which
 * sets X86_FEATURE_PREFER_YMM).
 */
static bool cpu_has_vpclmul_avx512(void)
{
	unsigned int a, b, c, d;
	unsigned int max_leaf, family, model, xcr0_lo, xcr0_hi;
	unsigned long long xcr0;
	bool intel;

	__cpuidex(0, 0, max_leaf, b, c, d);
	if (max_leaf < 7)
		return false;
	/* "GenuineIntel" */
	intel = (b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e);

	__cpuidex(1, 0, a, b, c, d);
	/* OSXSAVE, the OS enabled xgetbv */
	if (!(c & (1U << 27)))
		return false;
	family = (a >> 8) & 0xf;
	model = (a >> 4) & 0xf;
	if (family >= 6)
		model += ((a >> 16) & 0xf) << 4;
	if (family == 0xf)
		family += (a >> 20) & 0xff;

	__asm__ __volatile__ ("xgetbv" : "=a" (xcr0_lo), "=d" (xcr0_hi) : "c" (0));
	xcr0 = ((unsigned long long)xcr0_hi << 32) | xcr0_lo;
	/* XMM, YMM, opmask, ZMM_Hi256 and Hi16_ZMM state */
	if ((xcr0 & 0xe6) != 0xe6)
		return false;

	__cpuidex(7, 0, a, b, c, d);
	/* EBX: AVX2, AVX512BW, AVX512VL; ECX: VPCLMULQDQ */
	if (!(b & (1U << 5)) || !(b & (1U << 30)) || !(b & (1U << 31)) ||
	    !(c & (1U << 10)))
		return false;

	if (intel && family == 6) {
		switch (model) {
		case 0x55:	/* INTEL_SKYLAKE_X */
		case 0x6a:	/* INTEL_ICELAKE_X */
		case 0x6c:	/* INTEL_ICELAKE_D */
		case 0x7d:	/* INTEL_ICELAKE */
		case 0x7e:	/* INTEL_ICELAKE_L */
		case 0x9d:	/* INTEL_ICELAKE_NNPI */
		case 0x8c:	/* INTEL_TIGERLAKE_L */
		case 0x8d:	/* INTEL_TIGERLAKE */
			return false;
		}
	}

	return true;
}

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
#if HAVE___BUILTIN_CPU_SUPPORTS__PCLMUL
	if (__builtin_cpu_supports("pclmul"))
		__cpu_flags |= CPU_FLAG_PCLMUL;
#endif
	if (__builtin_cpu_supports("avx"))
		__cpu_flags |= CPU_FLAG_AVX;
	if (__builtin_cpu_supports("avx2"))
		__cpu_flags |= CPU_FLAG_AVX2;

	/* Flags unsupported by builtins */
	__cpuidex(7, 0, a, b, c, d);
	if (b & (1UL << 29))
		__cpu_flags |= CPU_FLAG_SHA;

	if (cpu_has_vpclmul_avx512())
		__cpu_flags |= CPU_FLAG_VPCLMUL_AVX512;

	__cpu_flags_orig = __cpu_flags;
}

#elif defined(__aarch64__)

/* Values of the kernel's arch/arm64/include/uapi/asm/hwcap.h, for old headers */
#ifndef HWCAP_PMULL
#define HWCAP_PMULL		(1 << 4)
#endif
#ifndef HWCAP_CRC32
#define HWCAP_CRC32		(1 << 7)
#endif

void cpu_detect_flags(void)
{
	unsigned long hwcap = getauxval(AT_HWCAP);

	__cpu_flags = CPU_FLAG_NONE;
	if (hwcap & HWCAP_CRC32)
		__cpu_flags |= CPU_FLAG_ARMV8_CRC32;
	if (hwcap & HWCAP_PMULL)
		__cpu_flags |= CPU_FLAG_ARMV8_PMULL;

	__cpu_flags_orig = __cpu_flags;
}

#else

void cpu_detect_flags() { }

#endif

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
