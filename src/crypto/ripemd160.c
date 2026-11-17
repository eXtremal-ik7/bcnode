#include "ripemd160.h"
#include "endianc.h"
#include <string.h>

// RIPEMD-160: two lines of 80 steps over the same block, five groups of 16 steps each with its own
// boolean function and constant; the message words and the length are little-endian

static inline uint32_t rol32(const uint32_t x, const unsigned n)
{
  return (x << n) | (x >> (32 - n));
}

static inline uint32_t le32(uint32_t x)
{
  return ENDIANC_IS_LITTLE ? x : bswap32(x);
}

// Message word of each step, left and right line
static const uint8_t wordsLeft[80] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
  3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
  1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
  4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13
};

static const uint8_t wordsRight[80] = {
  5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
  6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
  15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
  8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
  12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11
};

// Rotation of each step
static const uint8_t shiftsLeft[80] = {
  11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
  7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
  11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
  11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
  9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6
};

static const uint8_t shiftsRight[80] = {
  8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
  9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
  9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
  15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
  8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11
};

static inline uint32_t f(unsigned i, uint32_t x, uint32_t y, uint32_t z)
{
  switch (i) {
    case 0 : return x ^ y ^ z;
    case 1 : return (x & y) | (~x & z);
    case 2 : return (x | ~y) ^ z;
    case 3 : return (x & z) | (y & ~z);
    default : return x ^ (y | ~z);
  }
}

// 16 steps of one line: a, b, c, d, e move down by one each step, c rotated by 10 on its way to d
static inline void ripemd160Group(uint32_t v[5], const uint32_t x[16], unsigned group, unsigned function, uint32_t k,
                                  const uint8_t *words, const uint8_t *shifts)
{
  for (unsigned i = group * 16; i < group * 16 + 16; i++) {
    const uint32_t t = rol32(v[0] + f(function, v[1], v[2], v[3]) + x[words[i]] + k, shifts[i]) + v[4];
    v[0] = v[4];
    v[4] = v[3];
    v[3] = rol32(v[2], 10);
    v[2] = v[1];
    v[1] = t;
  }
}

void ripemd160llInit(uint32_t state[5])
{
  state[0] = 0x67452301;
  state[1] = 0xEFCDAB89;
  state[2] = 0x98BADCFE;
  state[3] = 0x10325476;
  state[4] = 0xC3D2E1F0;
}

void ripemd160llTransform(uint32_t state[5], const uint8_t block[64])
{
  uint32_t x[16];
  memcpy(x, block, 64);
  for (unsigned i = 0; i < 16; i++)
    x[i] = le32(x[i]);

  uint32_t left[5], right[5];
  memcpy(left, state, sizeof(left));
  memcpy(right, state, sizeof(right));

  // The right line takes the functions in the reverse order
  ripemd160Group(left, x, 0, 0, 0x00000000, wordsLeft, shiftsLeft);
  ripemd160Group(right, x, 0, 4, 0x50A28BE6, wordsRight, shiftsRight);
  ripemd160Group(left, x, 1, 1, 0x5A827999, wordsLeft, shiftsLeft);
  ripemd160Group(right, x, 1, 3, 0x5C4DD124, wordsRight, shiftsRight);
  ripemd160Group(left, x, 2, 2, 0x6ED9EBA1, wordsLeft, shiftsLeft);
  ripemd160Group(right, x, 2, 2, 0x6D703EF3, wordsRight, shiftsRight);
  ripemd160Group(left, x, 3, 3, 0x8F1BBCDC, wordsLeft, shiftsLeft);
  ripemd160Group(right, x, 3, 1, 0x7A6D76E9, wordsRight, shiftsRight);
  ripemd160Group(left, x, 4, 4, 0xA953FD4E, wordsLeft, shiftsLeft);
  ripemd160Group(right, x, 4, 0, 0x00000000, wordsRight, shiftsRight);

  const uint32_t t = state[1] + left[2] + right[3];
  state[1] = state[2] + left[3] + right[4];
  state[2] = state[3] + left[4] + right[0];
  state[3] = state[4] + left[0] + right[1];
  state[4] = state[0] + left[1] + right[2];
  state[0] = t;
}

void ripemd160llFinal(const uint32_t state[5], uint8_t *hash)
{
  for (unsigned i = 0; i < 5; i++) {
    const uint32_t word = le32(state[i]);
    memcpy(hash + 4 * i, &word, 4);
  }
}

// 0x80, zeros, the length in bits as 8 little-endian bytes: one block, or two when the tail leaves
// no room for the length
static void ripemd160Pad(uint32_t state[5], uint8_t buffer[64], size_t tail, uint64_t msgSize)
{
  buffer[tail] = 0x80;
  if (tail > 64 - 1 - 8) {
    memset(buffer + tail + 1, 0, 64 - (tail + 1));
    ripemd160llTransform(state, buffer);
    memset(buffer, 0, 56);
  } else {
    memset(buffer + tail + 1, 0, 56 - (tail + 1));
  }

  const uint64_t bits = msgSize * 8;
  for (unsigned i = 0; i < 8; i++)
    buffer[56 + i] = (uint8_t)(bits >> (8 * i));
  ripemd160llTransform(state, buffer);
}

void ripemd160Init(CCtxRipemd160 *ctx)
{
  ripemd160llInit(ctx->state);
  ctx->bufferSize = 0;
  ctx->MsgSize = 0;
}

void ripemd160Update(CCtxRipemd160 *ctx, const void *data, size_t size)
{
  const uint8_t *p = (const uint8_t*)data;
  size_t remaining = size;

  // step 1: complete the buffered block
  if (ctx->bufferSize) {
    size_t copySize = 64 - ctx->bufferSize <= remaining ? 64 - ctx->bufferSize : remaining;
    memcpy(ctx->buffer + ctx->bufferSize, p, copySize);
    ctx->bufferSize += (uint32_t)copySize;
    p += copySize;
    remaining -= copySize;
    if (ctx->bufferSize < 64) {
      ctx->MsgSize += size;
      return;
    }
    ripemd160llTransform(ctx->state, ctx->buffer);
    ctx->bufferSize = 0;
  }

  // step 2: full blocks straight from the data
  for (; remaining >= 64; remaining -= 64, p += 64)
    ripemd160llTransform(ctx->state, p);

  // step 3: keep the remainder
  memcpy(ctx->buffer, p, remaining);
  ctx->bufferSize = (uint32_t)remaining;
  ctx->MsgSize += size;
}

void ripemd160Final(CCtxRipemd160 *ctx, uint8_t *hash)
{
  ripemd160Pad(ctx->state, ctx->buffer, ctx->bufferSize, ctx->MsgSize);
  ripemd160llFinal(ctx->state, hash);
}

void ripemd160(const void *data, size_t size, uint8_t *hash)
{
  uint32_t state[5];
  ripemd160llInit(state);
  const uint8_t *p = (const uint8_t*)data;
  for (size_t i = 0; i < size / 64; i++, p += 64)
    ripemd160llTransform(state, p);

  uint8_t buffer[64];
  memcpy(buffer, p, size % 64);
  ripemd160Pad(state, buffer, size % 64, size);
  ripemd160llFinal(state, hash);
}
