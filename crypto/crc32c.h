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

uint32_t crc32c_le(uint32_t seed, unsigned char const *data, uint32_t length);
void crc32c_init_accel(void);

#define crc32c(seed, data, length) crc32c_le(seed, (unsigned char const *)data, length)

#endif
