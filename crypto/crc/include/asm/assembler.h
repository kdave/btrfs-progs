/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace stand-in for the kernel's arch/arm64/include/asm/assembler.h, with
 * the macros that lib/crc/arm64/crc32-core.S uses.
 */

#ifndef __CRYPTO_CRC_ASM_ASSEMBLER_H__
#define __CRYPTO_CRC_ASM_ASSEMBLER_H__

	/* Address of a symbol within +/- 4 GiB of the program counter */
	.macro	adr_l, dst, sym
	adrp	\dst, \sym
	add	\dst, \dst, :lo12:\sym
	.endm

#endif
