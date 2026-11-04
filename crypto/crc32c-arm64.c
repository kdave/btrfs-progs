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
 * CRC32C using the CRC32 instructions of ARMv8 (optional in ARMv8.0, mandatory
 * since ARMv8.1). This file is built with -march=armv8-a+crc and the function
 * is used only when the CPU has the instructions (HWCAP_CRC32).
 */

#include <stdint.h>
#include <string.h>
#include <endian.h>

#ifdef __ARM_FEATURE_CRC32

#include <arm_acle.h>

uint32_t crc32c_arm64(uint32_t crc, unsigned char const *data, uint32_t length);

uint32_t crc32c_arm64(uint32_t crc, unsigned char const *data, uint32_t length)
{
	uint64_t v64;
	uint32_t v32;
	uint16_t v16;

	while (length >= sizeof(v64)) {
		memcpy(&v64, data, sizeof(v64));
		crc = __crc32cd(crc, le64toh(v64));
		data += sizeof(v64);
		length -= sizeof(v64);
	}
	if (length & sizeof(v32)) {
		memcpy(&v32, data, sizeof(v32));
		crc = __crc32cw(crc, le32toh(v32));
		data += sizeof(v32);
	}
	if (length & sizeof(v16)) {
		memcpy(&v16, data, sizeof(v16));
		crc = __crc32ch(crc, le16toh(v16));
		data += sizeof(v16);
	}
	if (length & 1)
		crc = __crc32cb(crc, *data);

	return crc;
}

#endif
