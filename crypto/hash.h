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

#ifndef CRYPTO_HASH_H
#define CRYPTO_HASH_H

#include "kerncompat.h"

#define CRYPTO_HASH_SIZE_MAX	32

int hash_crc32c(const u8 *buf, size_t length, u8 *out);
int hash_xxhash(const u8 *buf, size_t length, u8 *out);
int hash_sha256(const u8 *buf, size_t length, u8 *out);
int hash_blake2b(const u8 *buf, size_t length, u8 *out);

void hash_init_accel(void);
void hash_init_crc32c(void);
void hash_init_blake2(void);
void hash_init_sha256(void);

#endif
