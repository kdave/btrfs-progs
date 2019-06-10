#include "crypto/hash.h"
#include "crypto/crc32c.h"
#include "crypto/xxhash.h"

int hash_crc32c(const u8* buf, size_t length, u8 *out)
{
	u32 crc = ~0;

	crc = crc32c(~0, buf, length);
	put_unaligned_le32(~crc, out);

	return 0;
}

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
