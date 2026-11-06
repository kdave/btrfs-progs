/*
 * Copied from the kernel source code, lib/libcrc32c.c.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "crypto/crc32c.h"
#include "common/cpu-utils.h"

static uint32_t crc32c_ref(uint32_t crc, unsigned char const *data, uint32_t length);
static uint32_t (*crc32c_impl)(uint32_t crc, unsigned char const *data, uint32_t length) = crc32c_ref;

#ifdef __x86_64__

#include <nmmintrin.h>
#include <wmmintrin.h>

/*
 * Fast CRC32C with PCLMULQDQ, three interleaved crc32 lanes combined with
 * carry-less multiplication. Derived from the Intel implementation
 * (crc32c-pcl-intel-asm_64.S in the kernel), available under GPLv2 or the
 * OpenIB.org BSD license.
 *
 * Copyright (C) 2012 Intel Corporation.
 * Copyright 2024 Google LLC
 *
 * Authors:
 *	Wajdi Feghali <wajdi.k.feghali@intel.com>
 *	James Guilford <james.guilford@intel.com>
 *	David Cote <david.m.cote@intel.com>
 *	Tim Chen <tim.c.chen@linux.intel.com>
 */

/* ISCSI CRC 32 Implementation with crc32 and pclmulqdq Instruction */

/*
 * Define threshold below which buffers are considered "small" and routed to
 * regular CRC code that does not interleave the CRC instructions.
 */
#define PCL_SMALL_SIZE	200

/*
 * PCLMULQDQ tables
 * Table is 128 entries x 2 words (8 bytes) each. The first entry is for idx 1
 * (one qword per lane), the constants are used as K1:K2 when combining lanes.
 */
static const uint32_t crc32c_pcl_k_table[128][2] = {
	{ 0x493c7d27, 0x00000001 },
	{ 0xba4fc28e, 0x493c7d27 },
	{ 0xddc0152b, 0xf20c0dfe },
	{ 0x9e4addf8, 0xba4fc28e },
	{ 0x39d3b296, 0x3da6d0cb },
	{ 0x0715ce53, 0xddc0152b },
	{ 0x47db8317, 0x1c291d04 },
	{ 0x0d3b6092, 0x9e4addf8 },
	{ 0xc96cfdc0, 0x740eef02 },
	{ 0x878a92a7, 0x39d3b296 },
	{ 0xdaece73e, 0x083a6eec },
	{ 0xab7aff2a, 0x0715ce53 },
	{ 0x2162d385, 0xc49f4f67 },
	{ 0x83348832, 0x47db8317 },
	{ 0x299847d5, 0x2ad91c30 },
	{ 0xb9e02b86, 0x0d3b6092 },
	{ 0x18b33a4e, 0x6992cea2 },
	{ 0xb6dd949b, 0xc96cfdc0 },
	{ 0x78d9ccb7, 0x7e908048 },
	{ 0xbac2fd7b, 0x878a92a7 },
	{ 0xa60ce07b, 0x1b3d8f29 },
	{ 0xce7f39f4, 0xdaece73e },
	{ 0x61d82e56, 0xf1d0f55e },
	{ 0xd270f1a2, 0xab7aff2a },
	{ 0xc619809d, 0xa87ab8a8 },
	{ 0x2b3cac5d, 0x2162d385 },
	{ 0x65863b64, 0x8462d800 },
	{ 0x1b03397f, 0x83348832 },
	{ 0xebb883bd, 0x71d111a8 },
	{ 0xb3e32c28, 0x299847d5 },
	{ 0x064f7f26, 0xffd852c6 },
	{ 0xdd7e3b0c, 0xb9e02b86 },
	{ 0xf285651c, 0xdcb17aa4 },
	{ 0x10746f3c, 0x18b33a4e },
	{ 0xc7a68855, 0xf37c5aee },
	{ 0x271d9844, 0xb6dd949b },
	{ 0x8e766a0c, 0x6051d5a2 },
	{ 0x93a5f730, 0x78d9ccb7 },
	{ 0x6cb08e5c, 0x18b0d4ff },
	{ 0x6b749fb2, 0xbac2fd7b },
	{ 0x1393e203, 0x21f3d99c },
	{ 0xcec3662e, 0xa60ce07b },
	{ 0x96c515bb, 0x8f158014 },
	{ 0xe6fc4e6a, 0xce7f39f4 },
	{ 0x8227bb8a, 0xa00457f7 },
	{ 0xb0cd4768, 0x61d82e56 },
	{ 0x39c7ff35, 0x8d6d2c43 },
	{ 0xd7a4825c, 0xd270f1a2 },
	{ 0x0ab3844b, 0x00ac29cf },
	{ 0x0167d312, 0xc619809d },
	{ 0xf6076544, 0xe9adf796 },
	{ 0x26f6a60a, 0x2b3cac5d },
	{ 0xa741c1bf, 0x96638b34 },
	{ 0x98d8d9cb, 0x65863b64 },
	{ 0x49c3cc9c, 0xe0e9f351 },
	{ 0x68bce87a, 0x1b03397f },
	{ 0x57a3d037, 0x9af01f2d },
	{ 0x6956fc3b, 0xebb883bd },
	{ 0x42d98888, 0x2cff42cf },
	{ 0x3771e98f, 0xb3e32c28 },
	{ 0xb42ae3d9, 0x88f25a3a },
	{ 0x2178513a, 0x064f7f26 },
	{ 0xe0ac139e, 0x4e36f0b0 },
	{ 0x170076fa, 0xdd7e3b0c },
	{ 0x444dd413, 0xbd6f81f8 },
	{ 0x6f345e45, 0xf285651c },
	{ 0x41d17b64, 0x91c9bd4b },
	{ 0xff0dba97, 0x10746f3c },
	{ 0xa2b73df1, 0x885f087b },
	{ 0xf872e54c, 0xc7a68855 },
	{ 0x1e41e9fc, 0x4c144932 },
	{ 0x86d8e4d2, 0x271d9844 },
	{ 0x651bd98b, 0x52148f02 },
	{ 0x5bb8f1bc, 0x8e766a0c },
	{ 0xa90fd27a, 0xa3c6f37a },
	{ 0xb3af077a, 0x93a5f730 },
	{ 0x4984d782, 0xd7c0557f },
	{ 0xca6ef3ac, 0x6cb08e5c },
	{ 0x234e0b26, 0x63ded06a },
	{ 0xdd66cbbb, 0x6b749fb2 },
	{ 0x4597456a, 0x4d56973c },
	{ 0xe9e28eb4, 0x1393e203 },
	{ 0x7b3ff57a, 0x9669c9df },
	{ 0xc9c8b782, 0xcec3662e },
	{ 0x3f70cc6f, 0xe417f38a },
	{ 0x93e106a4, 0x96c515bb },
	{ 0x62ec6c6d, 0x4b9e0f71 },
	{ 0xd813b325, 0xe6fc4e6a },
	{ 0x0df04680, 0xd104b8fc },
	{ 0x2342001e, 0x8227bb8a },
	{ 0x0a2a8d7e, 0x5b397730 },
	{ 0x6d9a4957, 0xb0cd4768 },
	{ 0xe8b6368b, 0xe78eb416 },
	{ 0xd2c3ed1a, 0x39c7ff35 },
	{ 0x995a5724, 0x61ff0e01 },
	{ 0x9ef68d35, 0xd7a4825c },
	{ 0x0c139b31, 0x8d96551c },
	{ 0xf2271e60, 0x0ab3844b },
	{ 0x0b0bf8ca, 0x0bf80dd2 },
	{ 0x2664fd8b, 0x0167d312 },
	{ 0xed64812d, 0x8821abed },
	{ 0x02ee03b2, 0xf6076544 },
	{ 0x8604ae0f, 0x6a45d2b2 },
	{ 0x363bd6b3, 0x26f6a60a },
	{ 0x135c83fd, 0xd8d26619 },
	{ 0x5fabe670, 0xa741c1bf },
	{ 0x35ec3279, 0xde87806c },
	{ 0x00bcf5f6, 0x98d8d9cb },
	{ 0x8ae00689, 0x14338754 },
	{ 0x17f27698, 0x49c3cc9c },
	{ 0x58ca5f00, 0x5bd2011f },
	{ 0xaa7c7ad5, 0x68bce87a },
	{ 0xb5cfca28, 0xdd07448e },
	{ 0xded288f8, 0x57a3d037 },
	{ 0x59f229bc, 0xdde8f5b9 },
	{ 0x6d390dec, 0x6956fc3b },
	{ 0x37170390, 0xa3e3e02c },
	{ 0x6353c1cc, 0x42d98888 },
	{ 0xc4584f5c, 0xd73c7bea },
	{ 0xf48642e9, 0x3771e98f },
	{ 0x531377e2, 0x80ff0093 },
	{ 0xdd35bc8d, 0xb42ae3d9 },
	{ 0xb25b29f2, 0x8fe4c34d },
	{ 0x9a5ede41, 0x2178513a },
	{ 0xa563905d, 0xdf99fc11 },
	{ 0x45cddf4e, 0xe0ac139e },
	{ 0xacfa3103, 0x6c23e841 },
	{ 0xa51b6135, 0x170076fa },
};

static inline uint64_t crc32c_load64(const unsigned char *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

__attribute__((target("sse4.2,pclmul")))
static uint32_t crc32c_pcl(uint32_t crc, unsigned char const *data, uint32_t len)
{
	uint64_t crc0 = crc;

	if (len >= PCL_SMALL_SIZE) {
		unsigned int misaligned = -(uintptr_t)data & 7;

		/*
		 * 1) ALIGN:
		 * Process 1 <= misaligned <= 7 bytes individually in order to
		 * align the remaining data to an 8-byte boundary.
		 */
		len -= misaligned;
		while (misaligned--)
			crc0 = _mm_crc32_u8(crc0, *data++);
	}

	/*
	 * 2) PROCESS BLOCK:
	 * 5) If more blocks remain, repeat from here.
	 */
	while (len >= PCL_SMALL_SIZE) {
		/*
		 * Processing 128 qwords from each lane for a full block,
		 * otherwise compute floor(len / 24) to get the number of qwords
		 * to process from each lane. 2731 = ceil(2^16 / 24)
		 */
		unsigned int qwords = len >= 128 * 24 ? 128 : (len * 2731) >> 16;
		unsigned int chunk = qwords * 8;
		const uint32_t *k = crc32c_pcl_k_table[qwords - 1];
		const unsigned char *p1 = data + chunk;
		const unsigned char *p2 = data + 2 * chunk;
		uint64_t crc1 = 0, crc2 = 0;
		__m128i kvec, x1, x2;
		unsigned int i;

		/* 3) CRC each of three lanes: */
		for (i = 0; i < qwords - 1; i++) {
			crc0 = _mm_crc32_u64(crc0, crc32c_load64(data + i * 8));
			crc1 = _mm_crc32_u64(crc1, crc32c_load64(p1 + i * 8));
			crc2 = _mm_crc32_u64(crc2, crc32c_load64(p2 + i * 8));
		}
		crc0 = _mm_crc32_u64(crc0, crc32c_load64(data + i * 8));
		crc1 = _mm_crc32_u64(crc1, crc32c_load64(p1 + i * 8));
		/* SKIP the last qword of crc2, don't do this one yet */

		/* 4) Combine three results: */
		/* 2 consts: K1:K2 */
		kvec = _mm_set_epi64x(k[1], k[0]);
		/* CRC for block 1, multiply by K2 */
		x1 = _mm_clmulepi64_si128(_mm_cvtsi64_si128(crc0), kvec, 0x00);
		/* CRC for block 2, multiply by K1 */
		x2 = _mm_clmulepi64_si128(_mm_cvtsi64_si128(crc1), kvec, 0x10);
		x1 = _mm_xor_si128(x1, x2);
		crc0 = _mm_crc32_u64(crc2, _mm_cvtsi128_si64(x1) ^
				     crc32c_load64(p2 + i * 8));

		data += 3 * chunk;
		len -= 3 * chunk;
	}

	/* 6) Process any remainder without interleaving: */
	while (len >= 8) {
		crc0 = _mm_crc32_u64(crc0, crc32c_load64(data));
		data += 8;
		len -= 8;
	}
	if (len & 4) {
		uint32_t v;

		memcpy(&v, data, sizeof(v));
		crc0 = _mm_crc32_u32(crc0, v);
		data += 4;
	}
	if (len & 2) {
		uint16_t v;

		memcpy(&v, data, sizeof(v));
		crc0 = _mm_crc32_u16(crc0, v);
		data += 2;
	}
	if (len & 1)
		crc0 = _mm_crc32_u8(crc0, *data);

	return crc0;
}

/*
 * Based on a posting to lkml by Austin Zhang <austin.zhang@intel.com>
 *
 * Using hardware provided CRC32 instruction to accelerate the CRC32 disposal.
 * CRC32C polynomial:0x1EDC6F41(BE)/0x82F63B78(LE)
 * CRC32 is a new instruction in Intel SSE4.2, the reference can be found at:
 * http://www.intel.com/products/processor/manuals/
 * Intel(R) 64 and IA-32 Architectures Software Developer's Manual
 * Volume 2A: Instruction Set Reference, A-M
 */
#if  __SIZEOF_LONG__ == 8
#define SCALE_F 8
#define crc32c_hw_long(crc, v)	_mm_crc32_u64((crc), (v))
#else
#define SCALE_F 4
#define crc32c_hw_long(crc, v)	_mm_crc32_u32((crc), (v))
#endif

__attribute__((target("sse4.2")))
static uint32_t crc32c_intel_le_hw_byte(uint32_t crc, unsigned char const *data,
					uint32_t length)
{
	while (length--) {
		crc = _mm_crc32_u8(crc, *data);
		data++;
	}

	return crc;
}

/*
 * Accelerated implementation using SSE 4.2 extension for instruction crc32c.
 * Steps through buffer one byte at at time, calculates reflected crc using
 * table.
 */
__attribute__((target("sse4.2")))
static uint32_t crc32c_sse42(uint32_t crc, unsigned char const *data, uint32_t length)
{
	unsigned int iquotient = length / SCALE_F;
	unsigned int iremainder = length % SCALE_F;
	unsigned long *ptmp = (unsigned long *)data;

	while (iquotient--) {
		crc = crc32c_hw_long(crc, *ptmp);
		ptmp++;
	}

	if (iremainder)
		crc = crc32c_intel_le_hw_byte(crc, (unsigned char *)ptmp,
				 iremainder);

	return crc;
}

void crc32c_init_accel(void)
{
	if (0) {
	} else if (cpu_has_feature(CPU_FLAG_PCLMUL) &&
		   cpu_has_feature(CPU_FLAG_SSE42)) {
		/* printf("CRC32C: pcl\n"); */
		crc32c_impl = crc32c_pcl;
	} else if (cpu_has_feature(CPU_FLAG_SSE42)) {
		/* printf("CRC32c: intel\n"); */
		crc32c_impl = crc32c_sse42;
	} else {
		/* printf("CRC32c: fallback\n"); */
		crc32c_impl = crc32c_ref;
	}
}

#else

void crc32c_init_accel(void)
{
	crc32c_impl = crc32c_ref;
}

#endif /* __x86_64__ */

/*
 * This is the CRC-32C table
 * Generated with:
 * width = 32 bits
 * poly = 0x1EDC6F41
 * reflect input bytes = true
 * reflect output bytes = true
 */

static const uint32_t crc32c_table[256] = {
	0x00000000L, 0xF26B8303L, 0xE13B70F7L, 0x1350F3F4L,
	0xC79A971FL, 0x35F1141CL, 0x26A1E7E8L, 0xD4CA64EBL,
	0x8AD958CFL, 0x78B2DBCCL, 0x6BE22838L, 0x9989AB3BL,
	0x4D43CFD0L, 0xBF284CD3L, 0xAC78BF27L, 0x5E133C24L,
	0x105EC76FL, 0xE235446CL, 0xF165B798L, 0x030E349BL,
	0xD7C45070L, 0x25AFD373L, 0x36FF2087L, 0xC494A384L,
	0x9A879FA0L, 0x68EC1CA3L, 0x7BBCEF57L, 0x89D76C54L,
	0x5D1D08BFL, 0xAF768BBCL, 0xBC267848L, 0x4E4DFB4BL,
	0x20BD8EDEL, 0xD2D60DDDL, 0xC186FE29L, 0x33ED7D2AL,
	0xE72719C1L, 0x154C9AC2L, 0x061C6936L, 0xF477EA35L,
	0xAA64D611L, 0x580F5512L, 0x4B5FA6E6L, 0xB93425E5L,
	0x6DFE410EL, 0x9F95C20DL, 0x8CC531F9L, 0x7EAEB2FAL,
	0x30E349B1L, 0xC288CAB2L, 0xD1D83946L, 0x23B3BA45L,
	0xF779DEAEL, 0x05125DADL, 0x1642AE59L, 0xE4292D5AL,
	0xBA3A117EL, 0x4851927DL, 0x5B016189L, 0xA96AE28AL,
	0x7DA08661L, 0x8FCB0562L, 0x9C9BF696L, 0x6EF07595L,
	0x417B1DBCL, 0xB3109EBFL, 0xA0406D4BL, 0x522BEE48L,
	0x86E18AA3L, 0x748A09A0L, 0x67DAFA54L, 0x95B17957L,
	0xCBA24573L, 0x39C9C670L, 0x2A993584L, 0xD8F2B687L,
	0x0C38D26CL, 0xFE53516FL, 0xED03A29BL, 0x1F682198L,
	0x5125DAD3L, 0xA34E59D0L, 0xB01EAA24L, 0x42752927L,
	0x96BF4DCCL, 0x64D4CECFL, 0x77843D3BL, 0x85EFBE38L,
	0xDBFC821CL, 0x2997011FL, 0x3AC7F2EBL, 0xC8AC71E8L,
	0x1C661503L, 0xEE0D9600L, 0xFD5D65F4L, 0x0F36E6F7L,
	0x61C69362L, 0x93AD1061L, 0x80FDE395L, 0x72966096L,
	0xA65C047DL, 0x5437877EL, 0x4767748AL, 0xB50CF789L,
	0xEB1FCBADL, 0x197448AEL, 0x0A24BB5AL, 0xF84F3859L,
	0x2C855CB2L, 0xDEEEDFB1L, 0xCDBE2C45L, 0x3FD5AF46L,
	0x7198540DL, 0x83F3D70EL, 0x90A324FAL, 0x62C8A7F9L,
	0xB602C312L, 0x44694011L, 0x5739B3E5L, 0xA55230E6L,
	0xFB410CC2L, 0x092A8FC1L, 0x1A7A7C35L, 0xE811FF36L,
	0x3CDB9BDDL, 0xCEB018DEL, 0xDDE0EB2AL, 0x2F8B6829L,
	0x82F63B78L, 0x709DB87BL, 0x63CD4B8FL, 0x91A6C88CL,
	0x456CAC67L, 0xB7072F64L, 0xA457DC90L, 0x563C5F93L,
	0x082F63B7L, 0xFA44E0B4L, 0xE9141340L, 0x1B7F9043L,
	0xCFB5F4A8L, 0x3DDE77ABL, 0x2E8E845FL, 0xDCE5075CL,
	0x92A8FC17L, 0x60C37F14L, 0x73938CE0L, 0x81F80FE3L,
	0x55326B08L, 0xA759E80BL, 0xB4091BFFL, 0x466298FCL,
	0x1871A4D8L, 0xEA1A27DBL, 0xF94AD42FL, 0x0B21572CL,
	0xDFEB33C7L, 0x2D80B0C4L, 0x3ED04330L, 0xCCBBC033L,
	0xA24BB5A6L, 0x502036A5L, 0x4370C551L, 0xB11B4652L,
	0x65D122B9L, 0x97BAA1BAL, 0x84EA524EL, 0x7681D14DL,
	0x2892ED69L, 0xDAF96E6AL, 0xC9A99D9EL, 0x3BC21E9DL,
	0xEF087A76L, 0x1D63F975L, 0x0E330A81L, 0xFC588982L,
	0xB21572C9L, 0x407EF1CAL, 0x532E023EL, 0xA145813DL,
	0x758FE5D6L, 0x87E466D5L, 0x94B49521L, 0x66DF1622L,
	0x38CC2A06L, 0xCAA7A905L, 0xD9F75AF1L, 0x2B9CD9F2L,
	0xFF56BD19L, 0x0D3D3E1AL, 0x1E6DCDEEL, 0xEC064EEDL,
	0xC38D26C4L, 0x31E6A5C7L, 0x22B65633L, 0xD0DDD530L,
	0x0417B1DBL, 0xF67C32D8L, 0xE52CC12CL, 0x1747422FL,
	0x49547E0BL, 0xBB3FFD08L, 0xA86F0EFCL, 0x5A048DFFL,
	0x8ECEE914L, 0x7CA56A17L, 0x6FF599E3L, 0x9D9E1AE0L,
	0xD3D3E1ABL, 0x21B862A8L, 0x32E8915CL, 0xC083125FL,
	0x144976B4L, 0xE622F5B7L, 0xF5720643L, 0x07198540L,
	0x590AB964L, 0xAB613A67L, 0xB831C993L, 0x4A5A4A90L,
	0x9E902E7BL, 0x6CFBAD78L, 0x7FAB5E8CL, 0x8DC0DD8FL,
	0xE330A81AL, 0x115B2B19L, 0x020BD8EDL, 0xF0605BEEL,
	0x24AA3F05L, 0xD6C1BC06L, 0xC5914FF2L, 0x37FACCF1L,
	0x69E9F0D5L, 0x9B8273D6L, 0x88D28022L, 0x7AB90321L,
	0xAE7367CAL, 0x5C18E4C9L, 0x4F48173DL, 0xBD23943EL,
	0xF36E6F75L, 0x0105EC76L, 0x12551F82L, 0xE03E9C81L,
	0x34F4F86AL, 0xC69F7B69L, 0xD5CF889DL, 0x27A40B9EL,
	0x79B737BAL, 0x8BDCB4B9L, 0x988C474DL, 0x6AE7C44EL,
	0xBE2DA0A5L, 0x4C4623A6L, 0x5F16D052L, 0xAD7D5351L
};

/*
 * Fallback implementation. Step through buffer one byte at at time, calculates
 * reflected crc using table, can accept an unaligned buffer.
 */
static uint32_t crc32c_ref(uint32_t crc, unsigned char const *data, uint32_t length)
{
	while (length--)
		crc =
		    crc32c_table[(crc ^ *data++) & 0xFFL] ^ (crc >> 8);
	return crc;
}

uint32_t crc32c_le(uint32_t crc, unsigned char const *data, uint32_t length)
{
	/* Use by-byte access for unaligned buffers */
	if ((unsigned long)data % sizeof(unsigned long))
		return crc32c_ref(crc, data, length);

	return crc32c_impl(crc, data, length);
}
