/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace stand-in for the kernel's linux/linkage.h, with what the assembly
 * files copied from the kernel's lib/crc/ need.
 *
 * The functions are hidden, only crypto/crc32c.c calls them. Each object file
 * gets the notes that keep the hardening of the program it is linked into: a
 * non-executable stack, and the x86 CET or arm64 BTI/PAC properties when the
 * compiler is asked for them (-fcf-protection, -mbranch-protection), with the
 * matching landing pads.
 */

#ifndef __CRYPTO_CRC_LINUX_LINKAGE_H__
#define __CRYPTO_CRC_LINUX_LINKAGE_H__

#if defined(__x86_64__)

#if defined(__CET__) && (__CET__ & 1)
#define __SYM_FUNC_LANDING_PAD	endbr64
#else
#define __SYM_FUNC_LANDING_PAD
#endif

#define SYM_FUNC_START(name)					\
	.globl name ; .hidden name ; .type name, @function ;	\
	.p2align 4 ; name: __SYM_FUNC_LANDING_PAD
#define SYM_FUNC_END(name)	.size name, . - name
#define RET			ret

	.pushsection .note.GNU-stack, "", @progbits
	.popsection

#ifdef __CET__
	/* GNU_PROPERTY_X86_FEATURE_1_AND with the IBT and SHSTK bits of __CET__ */
	.pushsection .note.gnu.property, "a"
	.p2align 3
	.long .Lgnu_property_name_end - .Lgnu_property_name
	.long .Lgnu_property_desc_end - .Lgnu_property_name_end
	.long 5					/* NT_GNU_PROPERTY_TYPE_0 */
.Lgnu_property_name:
	.asciz "GNU"
.Lgnu_property_name_end:
	.p2align 3
	.long 0xc0000002			/* GNU_PROPERTY_X86_FEATURE_1_AND */
	.long 4
	.long __CET__
	.p2align 3
.Lgnu_property_desc_end:
	.popsection
#endif

#elif defined(__aarch64__)

#if defined(__ARM_FEATURE_BTI_DEFAULT) && __ARM_FEATURE_BTI_DEFAULT == 1
#define __SYM_FUNC_LANDING_PAD	hint 34		/* bti c */
#define __GNU_PROPERTY_AARCH64_BTI	1
#else
#define __SYM_FUNC_LANDING_PAD
#define __GNU_PROPERTY_AARCH64_BTI	0
#endif

/* The functions do not save the link register, nothing to sign */
#if defined(__ARM_FEATURE_PAC_DEFAULT) && __ARM_FEATURE_PAC_DEFAULT != 0
#define __GNU_PROPERTY_AARCH64_PAC	2
#else
#define __GNU_PROPERTY_AARCH64_PAC	0
#endif

#define SYM_FUNC_START(name)					\
	.globl name ; .hidden name ; .type name, %function ;	\
	.balign 4 ; name: __SYM_FUNC_LANDING_PAD
#define SYM_FUNC_END(name)	.size name, . - name

	.pushsection .note.GNU-stack, "", %progbits
	.popsection

#if __GNU_PROPERTY_AARCH64_BTI || __GNU_PROPERTY_AARCH64_PAC
	.pushsection .note.gnu.property, "a"
	.balign 8
	.long 4					/* name size */
	.long 16				/* descriptor size */
	.long 5					/* NT_GNU_PROPERTY_TYPE_0 */
	.asciz "GNU"
	.long 0xc0000000			/* GNU_PROPERTY_AARCH64_FEATURE_1_AND */
	.long 4
	.long __GNU_PROPERTY_AARCH64_BTI | __GNU_PROPERTY_AARCH64_PAC
	.long 0
	.popsection
#endif

#else
#error "No stand-in linkage for this architecture"
#endif

#endif
