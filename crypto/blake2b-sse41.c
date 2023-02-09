/*
   BLAKE2 reference source code package - optimized C implementations

   Copyright 2012, Samuel Neves <sneves@dei.uc.pt>.  You may use this under the
   terms of the CC0, the OpenSSL Licence, or the Apache Public License 2.0, at
   your option.  The terms of these licenses can be found at:

   - CC0 1.0 Universal : http://creativecommons.org/publicdomain/zero/1.0
   - OpenSSL license   : https://www.openssl.org/source/license.html
   - Apache 2.0        : http://www.apache.org/licenses/LICENSE-2.0

   More information about the BLAKE2 hash function can be found at
   https://blake2.net.
*/

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "blake2.h"
#include "blake2-impl.h"
#include "blake2-config.h"

#ifdef HAVE_SSE41

#include <emmintrin.h>
#if defined(HAVE_SSSE3)
#include <tmmintrin.h>
#endif
#if defined(HAVE_SSE41)
#include <smmintrin.h>
#endif
#if defined(HAVE_XOP)
#include <x86intrin.h>
#endif

#include "blake2b-round.h"

static const uint64_t blake2b_IV[8] =
{
  0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
  0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
  0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
  0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

void blake2b_compress_sse41( blake2b_state *S, const uint8_t block[BLAKE2B_BLOCKBYTES] )
{
  __m128i row1l, row1h;
  __m128i row2l, row2h;
  __m128i row3l, row3h;
  __m128i row4l, row4h;
  __m128i b0, b1;
  __m128i t0, t1;
#if defined(HAVE_SSSE3) && !defined(HAVE_XOP)
  const __m128i r16 = _mm_setr_epi8( 2, 3, 4, 5, 6, 7, 0, 1, 10, 11, 12, 13, 14, 15, 8, 9 );
  const __m128i r24 = _mm_setr_epi8( 3, 4, 5, 6, 7, 0, 1, 2, 11, 12, 13, 14, 15, 8, 9, 10 );
#endif
  const __m128i m0 = LOADU( block + 00 );
  const __m128i m1 = LOADU( block + 16 );
  const __m128i m2 = LOADU( block + 32 );
  const __m128i m3 = LOADU( block + 48 );
  const __m128i m4 = LOADU( block + 64 );
  const __m128i m5 = LOADU( block + 80 );
  const __m128i m6 = LOADU( block + 96 );
  const __m128i m7 = LOADU( block + 112 );

  row1l = LOADU( &S->h[0] );
  row1h = LOADU( &S->h[2] );
  row2l = LOADU( &S->h[4] );
  row2h = LOADU( &S->h[6] );
  row3l = LOADU( &blake2b_IV[0] );
  row3h = LOADU( &blake2b_IV[2] );
  row4l = _mm_xor_si128( LOADU( &blake2b_IV[4] ), LOADU( &S->t[0] ) );
  row4h = _mm_xor_si128( LOADU( &blake2b_IV[6] ), LOADU( &S->f[0] ) );
  ROUND( 0 );
  ROUND( 1 );
  ROUND( 2 );
  ROUND( 3 );
  ROUND( 4 );
  ROUND( 5 );
  ROUND( 6 );
  ROUND( 7 );
  ROUND( 8 );
  ROUND( 9 );
  ROUND( 10 );
  ROUND( 11 );
  row1l = _mm_xor_si128( row3l, row1l );
  row1h = _mm_xor_si128( row3h, row1h );
  STOREU( &S->h[0], _mm_xor_si128( LOADU( &S->h[0] ), row1l ) );
  STOREU( &S->h[2], _mm_xor_si128( LOADU( &S->h[2] ), row1h ) );
  row2l = _mm_xor_si128( row4l, row2l );
  row2h = _mm_xor_si128( row4h, row2h );
  STOREU( &S->h[4], _mm_xor_si128( LOADU( &S->h[4] ), row2l ) );
  STOREU( &S->h[6], _mm_xor_si128( LOADU( &S->h[6] ), row2h ) );
}

#endif
