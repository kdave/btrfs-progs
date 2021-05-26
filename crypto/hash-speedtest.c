#include "../kerncompat.h"
#include <time.h>
#include <getopt.h>
#include "crypto/hash.h"
#include "crypto/crc32c.h"
#include "crypto/sha.h"
#include "crypto/blake2.h"

#ifdef __x86_64__
static const int cycles_supported = 1;
#else
static const int cycles_supported = 0;
#endif

const int blocksize = 4096;
int iterations = 100000;

#ifdef __x86_64__
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

#define get_cycles()		read_tsc()

#else

#define get_cycles()		(0)

#endif

static inline u64 get_time(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 * 1000 * 1000 + ts.tv_nsec;
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

const char *units_to_str(int units)
{
	switch (units) {
	case 0: return "cycles";
	case 1: return "nsecs";
	}
	return "unknown";
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
	int units = 0;

	optind = 0;
	while (1) {
		static const struct option long_options[] = {
			{ "cycles", no_argument, NULL, 'c' },
			{ "time", no_argument, NULL, 't' },
			{ NULL, 0, NULL, 0}
		};
		int c;

		c = getopt_long(argc, argv, "ct", long_options, NULL);
		if (c < 0)
			break;
		switch (c) {
		case 'c':
			if (!cycles_supported) {
				fprintf(stderr,
		"ERROR: cannot measure cycles on this arch, use --time\n");
				return 1;
			}
			units = 0;
			break;
		case 't':
			units = 1;
			break;
		default:
			fprintf(stderr, "ERROR: unknown option\n");
			return 1;
		}
	}

	if (argc - optind >= 1) {
		iterations = atoi(argv[optind]);
		if (iterations < 0)
			iterations = 1;
	}

	crc32c_optimization_init();
	memset(buf, 0, 4096);

	printf("Block size:     %d\n", blocksize);
	printf("Iterations:     %d\n", iterations);
	printf("Implementation: %s\n", CRYPTOPROVIDER);
	printf("Units:          %s\n", units_to_str(units));
	printf("\n");

	for (idx = 0; idx < ARRAY_SIZE(contestants); idx++) {
		struct contestant *c = &contestants[idx];
		u64 start, end;

		printf("% 12s: ", c->name);
		fflush(stdout);

		start = (units ? get_time() : get_cycles());
		for (iter = 0; iter < iterations; iter++) {
			memset(buf, iter & 0xFF, blocksize);
			memset(hash, 0, 32);
			c->digest(buf, blocksize, hash);
		}
		end = (units ? get_time() : get_cycles());
		c->cycles = end - start;

		printf("%: % 12llu, %s/i % 8llu\n",
				units_to_str(units),
				(unsigned long long)c->cycles,
				units_to_str(units),
				(unsigned long long)c->cycles / iterations);
	}

	return 0;
}
