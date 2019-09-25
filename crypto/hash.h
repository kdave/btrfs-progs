#ifndef CRYPTO_HASH_H
#define CRYPTO_HASH_H

#include "../kerncompat.h"

#define CRYPTO_HASH_SIZE_MAX	32

int hash_xxhash(const u8 *buf, size_t length, u8 *out);

#endif
