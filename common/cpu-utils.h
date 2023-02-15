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

/*
 * Detect CPU feature bits at runtime (x86_64 only)
 */

#ifndef __CPU_UTILS_H__
#define __CPU_UTILS_H__

#include <stdbool.h>

#define ENUM_CPU_BIT(name)                              \
	__ ## name ## _BIT,                             \
	name = (1U << __ ## name ## _BIT),              \
	__ ## name ## _SEQ = __ ## name ## _BIT

enum cpu_feature {
	ENUM_CPU_BIT(CPU_FLAG_NONE),
	ENUM_CPU_BIT(CPU_FLAG_SSE2),
	ENUM_CPU_BIT(CPU_FLAG_SSSE3),
	ENUM_CPU_BIT(CPU_FLAG_SSE41),
	ENUM_CPU_BIT(CPU_FLAG_SSE42),
	ENUM_CPU_BIT(CPU_FLAG_SHA),
	ENUM_CPU_BIT(CPU_FLAG_AVX),
	ENUM_CPU_BIT(CPU_FLAG_AVX2),
};

#undef ENUM_CPU_BIT

/* Private but in public header to allow inlining */
extern unsigned long __cpu_flags;
extern unsigned long __cpu_flags_orig;

/* Public API */
void cpu_detect_flags(void);
void cpu_set_level(unsigned long topbit);
void cpu_reset_level(void);
void cpu_print_flags(void);

static inline bool cpu_has_feature(enum cpu_feature f)
{
	return __cpu_flags & f;
}

#endif
