#include "crypto/hash.h"
#include "crypto/crc32c.h"
#include "crypto/xxhash.h"
#include "crypto/sha.h"
#include "crypto/blake2.h"

/*
 * Default builtin implementations
 */
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

/*
 * Implementations of cryptographic primitives
 */
#if CRYPTOPROVIDER_BUILTIN == 1

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

#endif

#if CRYPTOPROVIDER_LIBGCRYPT == 1

#include <gcrypt.h>

int hash_sha256(const u8 *buf, size_t len, u8 *out)
{
	gcry_md_hash_buffer(GCRY_MD_SHA256, out, buf, len);
	return 0;
}

int hash_blake2b(const u8 *buf, size_t len, u8 *out)
{
	gcry_md_hash_buffer(GCRY_MD_BLAKE2B_256, out, buf, len);
	return 0;
}

#endif

#if CRYPTOPROVIDER_LIBSODIUM == 1

#include <sodium/crypto_hash_sha256.h>
#include <sodium/crypto_generichash_blake2b.h>

int hash_sha256(const u8 *buf, size_t len, u8 *out)
{
	return crypto_hash_sha256(out, buf, len);
}

int hash_blake2b(const u8 *buf, size_t len, u8 *out)
{
	return crypto_generichash_blake2b(out, CRYPTO_HASH_SIZE_MAX, buf, len,
			NULL, 0);
}

#endif
