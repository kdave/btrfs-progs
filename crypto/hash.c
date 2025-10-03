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

#include "kerncompat.h"
#include "crypto/hash.h"
#include "crypto/crc32c.h"
#include "crypto/xxhash.h"
#include "crypto/xxh_x86dispatch.h"
#include "crypto/sha.h"
#include "crypto/blake2.h"

void hash_init_crc32c(void)
{
	crc32c_init_accel();
}

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

int hash_xxh3(const u8 *buf, size_t length, u8 *out)
{
	XXH64_hash_t hash;

	hash = XXH3_64bits(buf, length);
	put_unaligned_le64(hash, out);

	return 0;
}

/*
 * Implementations of cryptographic primitives
 */
#if CRYPTOPROVIDER_BUILTIN == 1

void hash_init_accel(void)
{
	crc32c_init_accel();
	blake2_init_accel();
	sha256_init_accel();
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

#endif

#if CRYPTOPROVIDER_LIBGCRYPT == 1

#include <gcrypt.h>

void hash_init_accel(void)
{
	crc32c_init_accel();
}

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

void hash_init_accel(void)
{
	crc32c_init_accel();
}

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

#if CRYPTOPROVIDER_LIBKCAPI == 1

#include <kcapi.h>

void hash_init_accel(void)
{
	crc32c_init_accel();
}

int hash_sha256(const u8 *buf, size_t len, u8 *out)
{
	static struct kcapi_handle *handle = NULL;
	int ret;

	if (!handle) {
		ret = kcapi_md_init(&handle, "sha256", 0);
		if (ret < 0) {
			fprintf(stderr,
				"HASH: cannot instantiate sha256, error %d\n",
				ret);
			exit(1);
		}
	}
	ret = kcapi_md_digest(handle, buf, len, out, CRYPTO_HASH_SIZE_MAX);
	/* kcapi_md_destroy(handle); */

	return ret;
}

int hash_blake2b(const u8 *buf, size_t len, u8 *out)
{
	static struct kcapi_handle *handle = NULL;
	int ret;

	if (!handle) {
		ret = kcapi_md_init(&handle, "blake2b-256", 0);
		if (ret < 0) {
			fprintf(stderr,
				"HASH: cannot instantiate blake2b-256, error %d\n",
				ret);
			exit(1);
		}
	}
	ret = kcapi_md_digest(handle, buf, len, out, CRYPTO_HASH_SIZE_MAX);
	/* kcapi_md_destroy(handle); */

	return ret;
}

#endif

#if CRYPTOPROVIDER_BOTAN == 1

#include <botan/ffi.h>

void hash_init_accel(void)
{
	crc32c_init_accel();
}

int hash_sha256(const u8 *buf, size_t len, u8 *out)
{
	botan_hash_t hash;
	static bool initialized = false;
	int ret;

	if (!initialized) {
		ret = botan_hash_init(&hash, "SHA-256", 0);
		if (ret < 0) {
			fprintf(stderr, "HASH: cannot instantiate sha256, error %d\n", ret);
			exit(1);
		}
		initialized = true;
	} else {
		botan_hash_clear(hash);
	}
	botan_hash_update(hash, buf, len);
	botan_hash_final(hash, out);
	/* botan_hash_destroy(hash); */
	return 0;
}

int hash_blake2b(const u8 *buf, size_t len, u8 *out)
{
	botan_hash_t hash;
	static bool initialized = false;
	int ret;

	if (!initialized) {
		ret = botan_hash_init(&hash, "BLAKE2b(256)", 0);
		if (ret < 0) {
			fprintf(stderr, "HASH: cannot instantiate sha256, error %d\n", ret);
			exit(1);
		}
		initialized = true;
	} else {
		botan_hash_clear(hash);
	}
	botan_hash_update(hash, buf, len);
	botan_hash_final(hash, out);
	/* botan_hash_destroy(hash); */
	return 0;
}

#endif

#if CRYPTOPROVIDER_OPENSSL == 1

#include <openssl/params.h>
#include <openssl/evp.h>

void hash_init_accel(void)
{
	crc32c_init_accel();
}

int hash_sha256(const u8 *buf, size_t len, u8 *out)
{
	EVP_MD_CTX *ctx = NULL;

	if (!ctx) {
		ctx = EVP_MD_CTX_new();
		if (!ctx) {
			fprintf(stderr, "HASH: cannot instantiate sha256\n");
			exit(1);
		}
	}
	EVP_DigestInit(ctx, EVP_sha256());
	EVP_DigestUpdate(ctx, buf, len);
	EVP_DigestFinal(ctx, out, NULL);
	/* EVP_MD_CTX_free(ctx); */
	return 0;
}

int hash_blake2b(const u8 *buf, size_t len, u8 *out)
{
	EVP_MD_CTX *ctx = NULL;
	size_t digest_size = 256 / 8;
	const OSSL_PARAM params[] = {
		OSSL_PARAM_size_t("size", &digest_size),
		OSSL_PARAM_END
	};

	if (!ctx) {
		ctx = EVP_MD_CTX_new();
		if (!ctx) {
			fprintf(stderr, "HASH: cannot instantiate sha256\n");
			exit(1);
		}
	}
	EVP_DigestInit_ex2(ctx, EVP_blake2b512(), params);
	EVP_DigestUpdate(ctx, buf, len);
	EVP_DigestFinal(ctx, out, NULL);
	/* EVP_MD_CTX_free(ctx); */
	return 0;
}

#endif
