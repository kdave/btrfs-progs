/*
 * Copyright (C) 2007 Red Hat.  All rights reserved.
 *
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

#ifndef __CRC32C__
#define __CRC32C__

#include <inttypes.h>
#include <stddef.h>

uint32_t crc32c_le(uint32_t seed, unsigned char const *data, uint32_t length);
void crc32c_init_accel(void);

/* crypto/crc32c-x86.c, built with -msse4.2 -mpclmul if the compiler has them */
#if HAVE_CFLAG_mpclmul == 1
#define CRC32C_X86		1
uint32_t crc32c_x86_sse42(uint32_t crc, unsigned char const *p, uint32_t len);
uint32_t crc32c_x86_pclmul(uint32_t crc0, const char *buf, size_t len);
#else
#define CRC32C_X86		0
#endif

/*
 * crypto/crc32c-x86-avx512.c, built with -mavx512f -mavx512vl -mvpclmulqdq if
 * the compiler has them. GCC 8 and 9 lack _mm512_zextsi128_si512().
 */
#if HAVE_CFLAG_mvpclmulqdq == 1 && (defined(__clang__) || __GNUC__ >= 10)
#define CRC32C_X86_AVX512	1
uint32_t crc32c_x86_avx512(uint32_t crc0, const char *buf, size_t len);
#else
#define CRC32C_X86_AVX512	0
#endif

/* crypto/crc32c-arm64.c, built with -march=armv8-a+crc+crypto if the compiler has it */
#if HAVE_CFLAG_march_armv8_crc_crypto == 1
#define CRC32C_ARMV8		1
uint32_t crc32c_armv8(uint32_t crc, unsigned char const *p, uint32_t len);
uint32_t crc32c_armv8_pmull(uint32_t crc0, const char *buf, size_t len);
#else
#define CRC32C_ARMV8		0
#endif

#define crc32c(seed, data, length) crc32c_le(seed, (unsigned char const *)data, length)

#endif
