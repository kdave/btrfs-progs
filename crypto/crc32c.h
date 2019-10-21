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

#if BTRFS_FLAT_INCLUDES
#include "kerncompat.h"
#else
#include <btrfs/kerncompat.h>
#endif /* BTRFS_FLAT_INCLUDES */

u32 crc32c_le(u32 seed, unsigned char const *data, size_t length);
void crc32c_optimization_init(void);

#define crc32c(seed, data, length) crc32c_le(seed, (unsigned char const *)data, length)
#define btrfs_crc32c crc32c
#endif
