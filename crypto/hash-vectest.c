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

#include "../kerncompat.h"
#include "crypto/hash.h"
#include "crypto/crc32c.h"
#include "crypto/sha.h"
#include "crypto/blake2.h"

struct hash_testvec {
	const char *plaintext;
	size_t psize;
	const char *digest;
};

struct hash_testspec {
	const char *name;
	int digest_size;
	const struct hash_testvec *testvec;
	size_t count;
	int (*hash)(const u8 *buf, size_t length, u8 *out);
};

static const struct hash_testvec crc32c_tv[] = {
	{
		.psize = 0,
		.digest = "\x00\x00\x00\x00",
	},
	{
		.plaintext = "abcdefg",
		.psize = 7,
		.digest = "\x41\xf4\x27\xe6",
	},
};

static const struct hash_testvec xxhash64_tv[] = {
	{
		.psize = 0,
		.digest = "\x99\xe9\xd8\x51\x37\xdb\x46\xef",
	},
	{
		.plaintext = "\x40",
		.psize = 1,
		.digest = "\x20\x5c\x91\xaa\x88\xeb\x59\xd0",
	},
	{
		.plaintext = "\x40\x8b\xb8\x41\xe4\x42\x15\x2d"
			     "\x88\xc7\x9a\x09\x1a\x9b",
		.psize = 14,
		.digest = "\xa8\xe8\x2b\xa9\x92\xa1\x37\x4a",
	}
};

static const struct hash_testvec sha256_tv[] = {
	{
		.plaintext = "",
		.psize	= 0,
		.digest	= "\xe3\xb0\xc4\x42\x98\xfc\x1c\x14"
			  "\x9a\xfb\xf4\xc8\x99\x6f\xb9\x24"
			  "\x27\xae\x41\xe4\x64\x9b\x93\x4c"
			  "\xa4\x95\x99\x1b\x78\x52\xb8\x55",
	}, {
		.plaintext = "abc",
		.psize	= 3,
		.digest	= "\xba\x78\x16\xbf\x8f\x01\xcf\xea"
			  "\x41\x41\x40\xde\x5d\xae\x22\x23"
			  "\xb0\x03\x61\xa3\x96\x17\x7a\x9c"
			  "\xb4\x10\xff\x61\xf2\x00\x15\xad",
	}, {
		.plaintext = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		.psize	= 56,
		.digest	= "\x24\x8d\x6a\x61\xd2\x06\x38\xb8"
			  "\xe5\xc0\x26\x93\x0c\x3e\x60\x39"
			  "\xa3\x3c\xe4\x59\x64\xff\x21\x67"
			  "\xf6\xec\xed\xd4\x19\xdb\x06\xc1",
	}
};

static const struct hash_testvec blake2b_256_tv[] = {
	{
		.plaintext =
			"",
		.psize     = 0,
		.digest    =
			"\x0e\x57\x51\xc0\x26\xe5\x43\xb2"
			"\xe8\xab\x2e\xb0\x60\x99\xda\xa1"
			"\xd1\xe5\xdf\x47\x77\x8f\x77\x87"
			"\xfa\xab\x45\xcd\xf1\x2f\xe3\xa8",
	}, {
		.plaintext =
			"\x00\x01\x02\x03\x04\x05\x06\x07"
			"\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
			"\x10\x11\x12\x13\x14\x15\x16\x17"
			"\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
			"\x20\x21\x22\x23\x24\x25\x26\x27"
			"\x28\x29\x2a\x2b\x2c\x2d\x2e\x2f"
			"\x30\x31\x32\x33\x34\x35\x36\x37"
			"\x38\x39\x3a\x3b\x3c\x3d\x3e\x3f",
		.psize     = 64,
		.digest    =
			"\x10\xd8\xe6\xd5\x34\xb0\x09\x39"
			"\x84\x3f\xe9\xdc\xc4\xda\xe4\x8c"
			"\xdf\x00\x8f\x6b\x8b\x2b\x82\xb1"
			"\x56\xf5\x40\x4d\x87\x48\x87\xf5",
	}
};

static const struct hash_testspec test_spec[] = {
	{
		.name = "crc32c",
		.digest_size = 4,
		.testvec = crc32c_tv,
		.count = ARRAY_SIZE(crc32c_tv),
		.hash = hash_crc32c
	}, {
		.name = "xxhash64",
		.digest_size = 8,
		.testvec = xxhash64_tv,
		.count = ARRAY_SIZE(xxhash64_tv),
		.hash = hash_xxhash
	}, {
		.name = "sha256",
		.digest_size = 32,
		.testvec = sha256_tv,
		.count = ARRAY_SIZE(sha256_tv),
		.hash = hash_sha256
	}, {
		.name = "blake2b",
		.digest_size = 32,
		.testvec = blake2b_256_tv,
		.count = ARRAY_SIZE(blake2b_256_tv),
		.hash = hash_blake2b
	}
};

int test_hash(const struct hash_testspec *spec)
{
	int i;

	for (i = 0; i < spec->count; i++) {
		int ret;
		const struct hash_testvec *vec = &spec->testvec[i];
		u8 csum[CRYPTO_HASH_SIZE_MAX];

		ret = spec->hash((const u8 *)vec->plaintext, vec->psize, csum);
		if (ret < 0) {
			printf("ERROR: hash %s = %d\n", spec->name, ret);
			return 1;
		}
		if (memcmp(csum, vec->digest, spec->digest_size) == 0) {
			printf("%s vector %d: match\n", spec->name, i);
		} else {
			int j;

			printf("%s vector %d: MISMATCH\n", spec->name, i);
			printf("  want:");
			for (j = 0; j < spec->digest_size; j++)
				printf(" %02hhx", vec->digest[j]);
			putchar('\n');
			printf("  have:");
			for (j = 0; j < spec->digest_size; j++)
				printf(" %02hhx", csum[j]);
			putchar('\n');
		}
	}

	return 0;
}

int main(int argc, char **argv) {
	int i;

	for (i = 0; i < ARRAY_SIZE(test_spec); i++) {
		printf("TEST: name=%s vectors=%zd\n", test_spec[i].name,
				test_spec[i].count);
		test_hash(&test_spec[i]);
	}

	return 0;
}
