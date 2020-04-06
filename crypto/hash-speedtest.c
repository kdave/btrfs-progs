#include "../kerncompat.h"
#include "crypto/hash.h"
#include "crypto/crc32c.h"
#include "crypto/sha.h"
#include "crypto/blake2.h"

#ifndef __x86_64__
#error "Only x86_64 supported"
#endif

const int blocksize = 4096;
int iterations = 100000;

static __always_inline unsigned long long rdtsc(void)
{
	unsigned low, high;

        asm volatile("rdtsc" : "=a" (low), "=d" (high));

        return (low | ((u64)(high) << 32));
}

static inline u64 read_tsc(void)
{
       asm volatile("mfence");
       return rdtsc();
}

/* Read the input and copy last bytes as the hash */
static int hash_null_memcpy(const u8 *buf, size_t length, u8 *out)
{
       const u8 *end = buf + length;

       while (buf + CRYPTO_HASH_SIZE_MAX < end) {
               memcpy(out, buf, CRYPTO_HASH_SIZE_MAX);
               buf += CRYPTO_HASH_SIZE_MAX;
       }

       return 0;
}

/* Test overhead of the calls */
static int hash_null_nop(const u8 *buf, size_t length, u8 *out)
{
       memset(out, 0xFF, CRYPTO_HASH_SIZE_MAX);

       return 0;
}

int main(int argc, char **argv) {
	u8 buf[blocksize];
	u8 hash[32];
	int idx;
	int iter;
	struct contestant {
		char name[16];
		int (*digest)(const u8 *buf, size_t length, u8 *out);
		int digest_size;
		u64 cycles;
	} contestants[] = {
		{ .name = "NULL-NOP", .digest = hash_null_nop, .digest_size = 32 },
		{ .name = "NULL-MEMCPY", .digest = hash_null_memcpy, .digest_size = 32 },
		{ .name = "CRC32C", .digest = hash_crc32c, .digest_size = 4 },
		{ .name = "XXHASH", .digest = hash_xxhash, .digest_size = 8 },
		{ .name = "SHA256", .digest = hash_sha256, .digest_size = 32 },
		{ .name = "BLAKE2b", .digest = hash_blake2b, .digest_size = 32 },
	};

	if (argc > 1) {
		iterations = atoi(argv[1]);
		if (iterations < 0)
			iterations = 1;
	}

	crc32c_optimization_init();
	memset(buf, 0, 4096);

	printf("Block size:    %d\n", blocksize);
	printf("Iterations:    %d\n", iterations);
	printf("Implementaion: %s\n", CRYPTOPROVIDER);
	printf("\n");

	for (idx = 0; idx < ARRAY_SIZE(contestants); idx++) {
		struct contestant *c = &contestants[idx];
		u64 start, end;

		printf("% 12s: ", c->name);
		fflush(stdout);

		start = read_tsc();
		for (iter = 0; iter < iterations; iter++) {
			memset(buf, iter & 0xFF, blocksize);
			memset(hash, 0, 32);
			c->digest(buf, blocksize, hash);
		}
		end = read_tsc();
		c->cycles = end - start;

		printf("cycles: % 12llu, c/i % 8llu\n",
				(unsigned long long)c->cycles,
				(unsigned long long)c->cycles / iterations);
	}

	return 0;
}
