#include "scrypt.h"
#include "sha256.h"
#include "endianc.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define LIBPOW_ALIGN(n) __attribute__((aligned(n)))
#define LIBPOW_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define LIBPOW_ALIGN(n) __declspec(align(n))
#define LIBPOW_NOINLINE __declspec(noinline)
#else
#define LIBPOW_ALIGN(n)
#define LIBPOW_NOINLINE
#endif

typedef struct HMAC_SHA256Context {
  CCtxSha256 ictx;
  CCtxSha256 octx;
} HMAC_SHA256_CTX;

// Initialize an HMAC-SHA256 operation with the given key.
static void HMAC_SHA256_Init(HMAC_SHA256_CTX *ctx, const void *_K, size_t Klen)
{
  unsigned char pad[64];
  unsigned char khash[32];
  const unsigned char *K = (const unsigned char *)_K;
  size_t i;

  // If Klen > 64, the key is really SHA256(K)
  if (Klen > 64) {
    sha256Init(&ctx->ictx);
    sha256Update(&ctx->ictx, K, Klen);
    sha256Final(&ctx->ictx, khash);
    K = khash;
    Klen = 32;
  }

  // Inner SHA256 operation is SHA256(K xor [block of 0x36] || data)
  sha256Init(&ctx->ictx);
  memset(pad, 0x36, 64);
  for (i = 0; i < Klen; i++)
    pad[i] ^= K[i];
  sha256Update(&ctx->ictx, pad, 64);

  // Outer SHA256 operation is SHA256(K xor [block of 0x5c] || hash)
  sha256Init(&ctx->octx);
  memset(pad, 0x5c, 64);
  for (i = 0; i < Klen; i++)
    pad[i] ^= K[i];
  sha256Update(&ctx->octx, pad, 64);
}

// Add bytes to the HMAC-SHA256 operation
static void HMAC_SHA256_Update(HMAC_SHA256_CTX *ctx, const void *in, size_t len)
{
  // Feed data to the inner SHA256 operation
  sha256Update(&ctx->ictx, in, len);
}

// Finish an HMAC-SHA256 operation
static void HMAC_SHA256_Final(unsigned char digest[32], HMAC_SHA256_CTX *ctx)
{
  unsigned char ihash[32];
  // Finish the inner SHA256 operation
  sha256Final(&ctx->ictx, ihash);
  // Feed the inner hash to the outer SHA256 operation
  sha256Update(&ctx->octx, ihash, 32);
  // Finish the outer SHA256 operation
  sha256Final(&ctx->octx, digest);
}

static void PBKDF2_SHA256(const uint8_t *passwd,
    size_t passwdlen,
    const uint8_t *salt,
    size_t saltlen,
    uint64_t c,
    uint8_t *buf,
    size_t dkLen)
{
  HMAC_SHA256_CTX PShctx, hctx;
  size_t i;
  uint8_t ivec[4];
  uint8_t U[32];
  uint8_t T[32];
  uint64_t j;
  int k;
  size_t clen;

  // Compute HMAC state after processing P and S
  HMAC_SHA256_Init(&PShctx, passwd, passwdlen);
  HMAC_SHA256_Update(&PShctx, salt, saltlen);

  // Iterate through the blocks
  for (i = 0; i * 32 < dkLen; i++) {
    // Generate INT(i + 1)
    storebe32(ivec, (uint32_t)(i + 1));

    // Compute U_1 = PRF(P, S || INT(i))
    memcpy(&hctx, &PShctx, sizeof(HMAC_SHA256_CTX));
    HMAC_SHA256_Update(&hctx, ivec, 4);
    HMAC_SHA256_Final(U, &hctx);

    // T_i = U_1 ...
    memcpy(T, U, 32);

    for (j = 2; j <= c; j++) {
      // Compute U_j
      HMAC_SHA256_Init(&hctx, passwd, passwdlen);
      HMAC_SHA256_Update(&hctx, U, 32);
      HMAC_SHA256_Final(U, &hctx);

      // ... xor U_j ...
      for (k = 0; k < 32; k++)
        T[k] ^= U[k];
    }

    // Copy as many bytes as necessary into buf
    clen = dkLen - i * 32;
    if (clen > 32)
      clen = 32;
    memcpy(&buf[i * 32], T, clen);
  }
}

#if defined(LIBPOW_SSE2_ENABLED) && !defined(LIBPOW_SCRYPT_SSE2_DISABLED)
#include <emmintrin.h>

static inline __m128i rol32sse2(__m128i x, int n) {
  return _mm_or_si128(_mm_slli_epi32(x, n), _mm_srli_epi32(x, 32 - n));
}

static void xorSalsa8sse2(uint32_t B[16], const uint32_t Bx[16])
{
  __m128i *B128 = (__m128i *)B;
  const __m128i *Bx128 = (const __m128i *)Bx;

  B128[0] = _mm_xor_si128(B128[0], Bx128[0]);
  B128[1] = _mm_xor_si128(B128[1], Bx128[1]);
  B128[2] = _mm_xor_si128(B128[2], Bx128[2]);
  B128[3] = _mm_xor_si128(B128[3], Bx128[3]);

  __m128i X0 = B128[0], X1 = B128[1], X2 = B128[2], X3 = B128[3];
  __m128i orig0 = X0, orig1 = X1, orig2 = X2, orig3 = X3;
  __m128i T;

  for (int i = 0; i < 4; i++) {
    T = _mm_add_epi32(X0, X3);  X1 = _mm_xor_si128(X1, rol32sse2(T,  7));
    T = _mm_add_epi32(X1, X0);  X2 = _mm_xor_si128(X2, rol32sse2(T,  9));
    T = _mm_add_epi32(X2, X1);  X3 = _mm_xor_si128(X3, rol32sse2(T, 13));
    T = _mm_add_epi32(X3, X2);  X0 = _mm_xor_si128(X0, rol32sse2(T, 18));
    X1 = _mm_shuffle_epi32(X1, 0x93);
    X2 = _mm_shuffle_epi32(X2, 0x4E);
    X3 = _mm_shuffle_epi32(X3, 0x39);
    T = _mm_add_epi32(X0, X1);  X3 = _mm_xor_si128(X3, rol32sse2(T,  7));
    T = _mm_add_epi32(X3, X0);  X2 = _mm_xor_si128(X2, rol32sse2(T,  9));
    T = _mm_add_epi32(X2, X3);  X1 = _mm_xor_si128(X1, rol32sse2(T, 13));
    T = _mm_add_epi32(X1, X2);  X0 = _mm_xor_si128(X0, rol32sse2(T, 18));
    X1 = _mm_shuffle_epi32(X1, 0x39);
    X2 = _mm_shuffle_epi32(X2, 0x4E);
    X3 = _mm_shuffle_epi32(X3, 0x93);
  }

  B128[0] = _mm_add_epi32(X0, orig0);
  B128[1] = _mm_add_epi32(X1, orig1);
  B128[2] = _mm_add_epi32(X2, orig2);
  B128[3] = _mm_add_epi32(X3, orig3);
}

static void scrypt_1024_1_1_256_sp(const char *input, char *output, char *scratchpad)
{
  uint8_t B[128];
  LIBPOW_ALIGN(16) uint32_t X[32];
  uint32_t *V;
  uint32_t i, j;
  __m128i *X128 = (__m128i *)X;

  V = (uint32_t *)(((uintptr_t)(scratchpad) + 63) & ~ (uintptr_t)(63));

  PBKDF2_SHA256((const uint8_t *)input, 80, (const uint8_t *)input, 80, 1, B, 128);

  // Linear to diagonal: diag0={0,5,10,15} diag1={4,9,14,3} diag2={8,13,2,7} diag3={12,1,6,11}
  {
    const uint32_t *S = (const uint32_t *)B;
    X[0]  = S[0];
    X[1]  = S[5];
    X[2]  = S[10];
    X[3]  = S[15];
    X[4]  = S[4];
    X[5]  = S[9];
    X[6]  = S[14];
    X[7]  = S[3];
    X[8]  = S[8];
    X[9]  = S[13];
    X[10] = S[2];
    X[11] = S[7];
    X[12] = S[12];
    X[13] = S[1];
    X[14] = S[6];
    X[15] = S[11];
    S += 16;
    X[16] = S[0];
    X[17] = S[5];
    X[18] = S[10];
    X[19] = S[15];
    X[20] = S[4];
    X[21] = S[9];
    X[22] = S[14];
    X[23] = S[3];
    X[24] = S[8];
    X[25] = S[13];
    X[26] = S[2];
    X[27] = S[7];
    X[28] = S[12];
    X[29] = S[1];
    X[30] = S[6];
    X[31] = S[11];
  }

  for (i = 0; i < 1024; i++) {
    __m128i *dst = (__m128i *)&V[i * 32];
    dst[0] = X128[0]; dst[1] = X128[1];
    dst[2] = X128[2]; dst[3] = X128[3];
    dst[4] = X128[4]; dst[5] = X128[5];
    dst[6] = X128[6]; dst[7] = X128[7];
    xorSalsa8sse2(&X[0], &X[16]);
    xorSalsa8sse2(&X[16], &X[0]);
  }
  for (i = 0; i < 1024; i++) {
    j = 32 * (X[16] & 1023);
    const __m128i *src = (const __m128i *)&V[j];
    X128[0] = _mm_xor_si128(X128[0], src[0]);
    X128[1] = _mm_xor_si128(X128[1], src[1]);
    X128[2] = _mm_xor_si128(X128[2], src[2]);
    X128[3] = _mm_xor_si128(X128[3], src[3]);
    X128[4] = _mm_xor_si128(X128[4], src[4]);
    X128[5] = _mm_xor_si128(X128[5], src[5]);
    X128[6] = _mm_xor_si128(X128[6], src[6]);
    X128[7] = _mm_xor_si128(X128[7], src[7]);
    xorSalsa8sse2(&X[0], &X[16]);
    xorSalsa8sse2(&X[16], &X[0]);
  }

  // Diagonal to linear
  {
    uint32_t *D = (uint32_t *)B;
    D[0]  = X[0];
    D[5]  = X[1];
    D[10] = X[2];
    D[15] = X[3];
    D[4]  = X[4];
    D[9]  = X[5];
    D[14] = X[6];
    D[3]  = X[7];
    D[8]  = X[8];
    D[13] = X[9];
    D[2]  = X[10];
    D[7]  = X[11];
    D[12] = X[12];
    D[1]  = X[13];
    D[6]  = X[14];
    D[11] = X[15];
    D += 16;
    D[0]  = X[16];
    D[5]  = X[17];
    D[10] = X[18];
    D[15] = X[19];
    D[4]  = X[20];
    D[9]  = X[21];
    D[14] = X[22];
    D[3]  = X[23];
    D[8]  = X[24];
    D[13] = X[25];
    D[2]  = X[26];
    D[7]  = X[27];
    D[12] = X[28];
    D[1]  = X[29];
    D[6]  = X[30];
    D[11] = X[31];
  }

  PBKDF2_SHA256((const uint8_t *)input, 80, B, 128, 1, (uint8_t *)output, 32);
}

#else // scalar fallback

static inline uint32_t rol32(const uint32_t x, const int n)
{
  return (x << n) | (x >> (32 - n));
}

static void LIBPOW_NOINLINE xorSalsa8(uint32_t B[16], const uint32_t Bx[16])
{
  uint32_t x00,x01,x02,x03,x04,x05,x06,x07,x08,x09,x10,x11,x12,x13,x14,x15;

  x00 = (B[ 0] ^= Bx[ 0]);
  x01 = (B[ 1] ^= Bx[ 1]);
  x02 = (B[ 2] ^= Bx[ 2]);
  x03 = (B[ 3] ^= Bx[ 3]);
  x04 = (B[ 4] ^= Bx[ 4]);
  x05 = (B[ 5] ^= Bx[ 5]);
  x06 = (B[ 6] ^= Bx[ 6]);
  x07 = (B[ 7] ^= Bx[ 7]);
  x08 = (B[ 8] ^= Bx[ 8]);
  x09 = (B[ 9] ^= Bx[ 9]);
  x10 = (B[10] ^= Bx[10]);
  x11 = (B[11] ^= Bx[11]);
  x12 = (B[12] ^= Bx[12]);
  x13 = (B[13] ^= Bx[13]);
  x14 = (B[14] ^= Bx[14]);
  x15 = (B[15] ^= Bx[15]);

  // Round 1-2
  x04 ^= rol32(x00 + x12,  7);  x09 ^= rol32(x05 + x01,  7);
  x14 ^= rol32(x10 + x06,  7);  x03 ^= rol32(x15 + x11,  7);
  x08 ^= rol32(x04 + x00,  9);  x13 ^= rol32(x09 + x05,  9);
  x02 ^= rol32(x14 + x10,  9);  x07 ^= rol32(x03 + x15,  9);
  x12 ^= rol32(x08 + x04, 13);  x01 ^= rol32(x13 + x09, 13);
  x06 ^= rol32(x02 + x14, 13);  x11 ^= rol32(x07 + x03, 13);
  x00 ^= rol32(x12 + x08, 18);  x05 ^= rol32(x01 + x13, 18);
  x10 ^= rol32(x06 + x02, 18);  x15 ^= rol32(x11 + x07, 18);
  x01 ^= rol32(x00 + x03,  7);  x06 ^= rol32(x05 + x04,  7);
  x11 ^= rol32(x10 + x09,  7);  x12 ^= rol32(x15 + x14,  7);
  x02 ^= rol32(x01 + x00,  9);  x07 ^= rol32(x06 + x05,  9);
  x08 ^= rol32(x11 + x10,  9);  x13 ^= rol32(x12 + x15,  9);
  x03 ^= rol32(x02 + x01, 13);  x04 ^= rol32(x07 + x06, 13);
  x09 ^= rol32(x08 + x11, 13);  x14 ^= rol32(x13 + x12, 13);
  x00 ^= rol32(x03 + x02, 18);  x05 ^= rol32(x04 + x07, 18);
  x10 ^= rol32(x09 + x08, 18);  x15 ^= rol32(x14 + x13, 18);

  // Round 3-4
  x04 ^= rol32(x00 + x12,  7);  x09 ^= rol32(x05 + x01,  7);
  x14 ^= rol32(x10 + x06,  7);  x03 ^= rol32(x15 + x11,  7);
  x08 ^= rol32(x04 + x00,  9);  x13 ^= rol32(x09 + x05,  9);
  x02 ^= rol32(x14 + x10,  9);  x07 ^= rol32(x03 + x15,  9);
  x12 ^= rol32(x08 + x04, 13);  x01 ^= rol32(x13 + x09, 13);
  x06 ^= rol32(x02 + x14, 13);  x11 ^= rol32(x07 + x03, 13);
  x00 ^= rol32(x12 + x08, 18);  x05 ^= rol32(x01 + x13, 18);
  x10 ^= rol32(x06 + x02, 18);  x15 ^= rol32(x11 + x07, 18);
  x01 ^= rol32(x00 + x03,  7);  x06 ^= rol32(x05 + x04,  7);
  x11 ^= rol32(x10 + x09,  7);  x12 ^= rol32(x15 + x14,  7);
  x02 ^= rol32(x01 + x00,  9);  x07 ^= rol32(x06 + x05,  9);
  x08 ^= rol32(x11 + x10,  9);  x13 ^= rol32(x12 + x15,  9);
  x03 ^= rol32(x02 + x01, 13);  x04 ^= rol32(x07 + x06, 13);
  x09 ^= rol32(x08 + x11, 13);  x14 ^= rol32(x13 + x12, 13);
  x00 ^= rol32(x03 + x02, 18);  x05 ^= rol32(x04 + x07, 18);
  x10 ^= rol32(x09 + x08, 18);  x15 ^= rol32(x14 + x13, 18);

  // Round 5-6
  x04 ^= rol32(x00 + x12,  7);  x09 ^= rol32(x05 + x01,  7);
  x14 ^= rol32(x10 + x06,  7);  x03 ^= rol32(x15 + x11,  7);
  x08 ^= rol32(x04 + x00,  9);  x13 ^= rol32(x09 + x05,  9);
  x02 ^= rol32(x14 + x10,  9);  x07 ^= rol32(x03 + x15,  9);
  x12 ^= rol32(x08 + x04, 13);  x01 ^= rol32(x13 + x09, 13);
  x06 ^= rol32(x02 + x14, 13);  x11 ^= rol32(x07 + x03, 13);
  x00 ^= rol32(x12 + x08, 18);  x05 ^= rol32(x01 + x13, 18);
  x10 ^= rol32(x06 + x02, 18);  x15 ^= rol32(x11 + x07, 18);
  x01 ^= rol32(x00 + x03,  7);  x06 ^= rol32(x05 + x04,  7);
  x11 ^= rol32(x10 + x09,  7);  x12 ^= rol32(x15 + x14,  7);
  x02 ^= rol32(x01 + x00,  9);  x07 ^= rol32(x06 + x05,  9);
  x08 ^= rol32(x11 + x10,  9);  x13 ^= rol32(x12 + x15,  9);
  x03 ^= rol32(x02 + x01, 13);  x04 ^= rol32(x07 + x06, 13);
  x09 ^= rol32(x08 + x11, 13);  x14 ^= rol32(x13 + x12, 13);
  x00 ^= rol32(x03 + x02, 18);  x05 ^= rol32(x04 + x07, 18);
  x10 ^= rol32(x09 + x08, 18);  x15 ^= rol32(x14 + x13, 18);

  // Round 7-8
  x04 ^= rol32(x00 + x12,  7);  x09 ^= rol32(x05 + x01,  7);
  x14 ^= rol32(x10 + x06,  7);  x03 ^= rol32(x15 + x11,  7);
  x08 ^= rol32(x04 + x00,  9);  x13 ^= rol32(x09 + x05,  9);
  x02 ^= rol32(x14 + x10,  9);  x07 ^= rol32(x03 + x15,  9);
  x12 ^= rol32(x08 + x04, 13);  x01 ^= rol32(x13 + x09, 13);
  x06 ^= rol32(x02 + x14, 13);  x11 ^= rol32(x07 + x03, 13);
  x00 ^= rol32(x12 + x08, 18);  x05 ^= rol32(x01 + x13, 18);
  x10 ^= rol32(x06 + x02, 18);  x15 ^= rol32(x11 + x07, 18);
  x01 ^= rol32(x00 + x03,  7);  x06 ^= rol32(x05 + x04,  7);
  x11 ^= rol32(x10 + x09,  7);  x12 ^= rol32(x15 + x14,  7);
  x02 ^= rol32(x01 + x00,  9);  x07 ^= rol32(x06 + x05,  9);
  x08 ^= rol32(x11 + x10,  9);  x13 ^= rol32(x12 + x15,  9);
  x03 ^= rol32(x02 + x01, 13);  x04 ^= rol32(x07 + x06, 13);
  x09 ^= rol32(x08 + x11, 13);  x14 ^= rol32(x13 + x12, 13);
  x00 ^= rol32(x03 + x02, 18);  x05 ^= rol32(x04 + x07, 18);
  x10 ^= rol32(x09 + x08, 18);  x15 ^= rol32(x14 + x13, 18);

  B[ 0] += x00;
  B[ 1] += x01;
  B[ 2] += x02;
  B[ 3] += x03;
  B[ 4] += x04;
  B[ 5] += x05;
  B[ 6] += x06;
  B[ 7] += x07;
  B[ 8] += x08;
  B[ 9] += x09;
  B[10] += x10;
  B[11] += x11;
  B[12] += x12;
  B[13] += x13;
  B[14] += x14;
  B[15] += x15;
}

static void scrypt_1024_1_1_256_sp(const char *input, char *output, char *scratchpad)
{
  uint8_t B[128];
  uint32_t X[32];
  uint32_t *V;
  uint32_t i, j, k;

  V = (uint32_t *)(((uintptr_t)(scratchpad) + 63) & ~ (uintptr_t)(63));

  PBKDF2_SHA256((const uint8_t *)input, 80, (const uint8_t *)input, 80, 1, B, 128);

  if (isLittleEndian()) {
    memcpy(X, B, 128);
  } else {
    for (k = 0; k < 32; k++)
      X[k] = loadle32(&B[4 * k]);
  }

  for (i = 0; i < 1024; i++) {
    memcpy(&V[i * 32], X, 128);
    xorSalsa8(&X[0], &X[16]);
    xorSalsa8(&X[16], &X[0]);
  }
  for (i = 0; i < 1024; i++) {
    j = 32 * (X[16] & 1023);
    for (k = 0; k < 32; k++)
      X[k] ^= V[j + k];
    xorSalsa8(&X[0], &X[16]);
    xorSalsa8(&X[16], &X[0]);
  }

  if (isLittleEndian()) {
    memcpy(B, X, 128);
  } else {
    for (k = 0; k < 32; k++)
      storele32(&B[4 * k], X[k]);
  }

  PBKDF2_SHA256((const uint8_t *)input, 80, B, 128, 1, (uint8_t *)output, 32);
}

#endif // LIBPOW_SSE2_ENABLED && !LIBPOW_SCRYPT_SSE2_DISABLED

void scrypt_1024_1_1_256(const void *input, uint8_t output[32])
{
  char scratchpad[LTC_SCRATCHPAD_SIZE];
  scrypt_1024_1_1_256_sp(input, (char*)output, scratchpad);
}

// One scratchpad for the group and the inputs already collected: what a lane interleaved kernel
// needs. Until there is one the group is walked lane by lane
void scrypt_1024_1_1_256_multi(const void *const *inputs, uint8_t *outputs, size_t count)
{
  char scratchpad[LTC_SCRATCHPAD_SIZE];
  size_t i;
  for (i = 0; i < count; i++)
    scrypt_1024_1_1_256_sp((const char*)inputs[i], (char*)(outputs + i*32), scratchpad);
}
