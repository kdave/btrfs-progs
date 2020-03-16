#include "crypto/hash.h"
#include "crypto/crc32c.h"
#include "crypto/xxhash.h"
#include "crypto/sha.h"
#include "crypto/blake2.h"

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
	put_unaligned_le64(hash, out);

	return 0;
}

int hash_sha256(const u8 *buf, size_t len, u8 *out)
{
	SHA256Context context;

	SHA256Reset(&context);
	SHA256Input(&context, buf, len);
	SHA256Result(&context, out);

	return 0;
}

int hash_blake2b(const u8 *buf, size_t len, u8 *out)
{
	blake2b_state S;

	blake2b_init(&S, CRYPTO_HASH_SIZE_MAX);
	blake2b_update(&S, buf, len);
	blake2b_final(&S, out, CRYPTO_HASH_SIZE_MAX);

	return 0;
}
