#include "crypto/hash.h"
#include "crypto/xxhash.h"

int hash_xxhash(const u8 *buf, size_t length, u8 *out)
{
	XXH64_hash_t hash;

	hash = XXH64(buf, length, 0);
	/*
	 * NOTE: we're not taking the canonical form here but the plain hash to
	 * be compatible with the kernel implementation!
	 */
	memcpy(out, &hash, 8);

	return 0;
}
