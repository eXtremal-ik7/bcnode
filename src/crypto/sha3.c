#include "sha3.h"
#include "endianc.h"
#include <string.h>

// Keccak-f, and the SHA-3 and Keccak sponges over it.
//
// The 1600 bit permutation is where the time goes. Two things make it fast, and neither is
// optional: the round is force inlined, and its lanes are named fields of a value rather than
// slots of an array. Inlined, the compiler can hold the lanes in registers at all; named
// rather than indexed, it actually does - gcc scalarizes a struct of fields and leaves an
// array in memory even inside an inlined body, which costs another 7%. Measured per
// permutation on a 9950X, gcc 13 and clang 18, -O2 and -O3: 178-186 ns this way, 189-200 with
// the same code over an array, 330-455 with the round left as a call. Which is also why
// sha3llRound1600 is gone from the interface: a round anyone could call is a round nobody can
// inline.

static const uint32_t KeccakConstants800[22] = {
  0x00000001, 0x00008082, 0x0000808a, 0x80008000, 0x0000808b, 0x80000001, 0x80008081,
  0x00008009, 0x0000008a, 0x00000088, 0x80008009, 0x8000000a, 0x8000808b, 0x0000008b,
  0x00008089, 0x00008003, 0x00008002, 0x00000080, 0x0000800a, 0x8000000a, 0x80008081,
  0x00008080,
};

static const uint64_t KeccakConstants1600[24] = {
    0x0000000000000001, 0x0000000000008082, 0x800000000000808a,
    0x8000000080008000, 0x000000000000808b, 0x0000000080000001,
    0x8000000080008081, 0x8000000000008009, 0x000000000000008a,
    0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
    0x000000008000808b, 0x800000000000008b, 0x8000000000008089,
    0x8000000000008003, 0x8000000000008002, 0x8000000000000080,
    0x000000000000800a, 0x800000008000000a, 0x8000000080008081,
    0x8000000000008080, 0x0000000080000001, 0x8000000080008008
};

// Padding of the last block: the two families differ in this byte and in nothing else
static const uint32_t Sha3Padding = 0x06;
static const uint32_t KeccakPadding = 0x01;

static inline uint32_t rol32(const uint32_t x, const int n)
{
  return (x << n) | (x >> (32 - n));
}

static inline uint64_t rol64(const uint64_t x, const int n)
{
  return (x << n) | (x >> (64 - n));
}

static inline uint32_t chi32(uint32_t a, uint32_t b, uint32_t c)
{
  return a ^ ((~b) & c);
}

void sha3llRound800(uint32_t state[25], unsigned round)
{
  uint32_t bc[5];

  // Theta
  bc[0] = state[0] ^ state[5] ^ state[10] ^ state[15] ^ state[20];
  bc[1] = state[1] ^ state[6] ^ state[11] ^ state[16] ^ state[21];
  bc[2] = state[2] ^ state[7] ^ state[12] ^ state[17] ^ state[22];
  bc[3] = state[3] ^ state[8] ^ state[13] ^ state[18] ^ state[23];
  bc[4] = state[4] ^ state[9] ^ state[14] ^ state[19] ^ state[24];

  {
    uint64_t v0 = bc[4] ^ rol32(bc[1], 1);
    uint64_t v1 = bc[0] ^ rol32(bc[2], 1);
    uint64_t v2 = bc[1] ^ rol32(bc[3], 1);
    uint64_t v3 = bc[2] ^ rol32(bc[4], 1);
    uint64_t v4 = bc[3] ^ rol32(bc[0], 1);
    state[0] ^= v0;
    state[5] ^= v0;
    state[10] ^= v0;
    state[15] ^= v0;
    state[20] ^= v0;
    state[1] ^= v1;
    state[6] ^= v1;
    state[11] ^= v1;
    state[16] ^= v1;
    state[21] ^= v1;
    state[2] ^= v2;
    state[7] ^= v2;
    state[12] ^= v2;
    state[17] ^= v2;
    state[22] ^= v2;
    state[3] ^= v3 ;
    state[8] ^= v3 ;
    state[13] ^= v3 ;
    state[18] ^= v3 ;
    state[23] ^= v3 ;
    state[4] ^= v4;
    state[9] ^= v4;
    state[14] ^= v4;
    state[19] ^= v4;
    state[24] ^= v4;
  }

  // Rho Pi
  uint64_t s1 = state[1];
  state[1] = rol32(state[6], 12);
  state[6] = rol32(state[9], 20);
  state[9] = rol32(state[22], 29);
  state[22] = rol32(state[14], 7);
  state[14] = rol32(state[20], 18);
  state[20] = rol32(state[2], 30);
  state[2] = rol32(state[12], 11);
  state[12] = rol32(state[13], 25);
  state[13] = rol32(state[19], 8);
  state[19] = rol32(state[23], 24);
  state[23] = rol32(state[15], 9);
  state[15] = rol32(state[4], 27);
  state[4] = rol32(state[24], 14);
  state[24] = rol32(state[21], 2);
  state[21] = rol32(state[8], 23);
  state[8] = rol32(state[16], 13);
  state[16] = rol32(state[5], 4);
  state[5] = rol32(state[3], 28);
  state[3] = rol32(state[18], 21);
  state[18] = rol32(state[17], 15);
  state[17] = rol32(state[11], 10);
  state[11] = rol32(state[7], 6);
  state[7] = rol32(state[10], 3);
  state[10] = rol32(s1, 1);

  //  Chi
  {
    uint64_t v, w;
    v = state[0];
    w = state[1];
    state[0] = chi32(v, w, state[2]);
    state[1] = chi32(w, state[2], state[3]);
    state[2] = chi32(state[2], state[3], state[4]);
    state[3] = chi32(state[3], state[4], v);
    state[4] = chi32(state[4], v, w);

    v = state[5];
    w = state[6];
    state[5] = chi32(v, w, state[7]);
    state[6] = chi32(w, state[7], state[8]);
    state[7] = chi32(state[7], state[8], state[9]);
    state[8] = chi32(state[8], state[9], v);
    state[9] = chi32(state[9], v, w);

    v = state[10];
    w = state[11];
    state[10] = chi32(v, w, state[12]);
    state[11] = chi32(w, state[12], state[13]);
    state[12] = chi32(state[12], state[13], state[14]);
    state[13] = chi32(state[13], state[14], v);
    state[14] = chi32(state[14], v, w);

    v = state[15];
    w = state[16];
    state[15] = chi32(v, w, state[17]);
    state[16] = chi32(w, state[17], state[18]);
    state[17] = chi32(state[17], state[18], state[19]);
    state[18] = chi32(state[18], state[19], v);
    state[19] = chi32(state[19], v, w);

    v = state[20];
    w = state[21];
    state[20] = chi32(v, w, state[22]);
    state[21] = chi32(w, state[22], state[23]);
    state[22] = chi32(state[22], state[23], state[24]);
    state[23] = chi32(state[23], state[24], v);
    state[24] = chi32(state[24], v, w);
  }

  //  Iota
  state[0] ^= KeccakConstants800[round];
}

typedef struct CKeccakLanes {
  uint64_t L00, L01, L02, L03, L04;
  uint64_t L05, L06, L07, L08, L09;
  uint64_t L10, L11, L12, L13, L14;
  uint64_t L15, L16, L17, L18, L19;
  uint64_t L20, L21, L22, L23, L24;
} CKeccakLanes;

// always_inline is not a hint here, it is the whole thing: left to itself the compiler keeps
// the round a call and the lanes memory, and one permutation costs 330 ns on gcc and 455 on
// clang instead of 180
static __attribute__((always_inline)) inline CKeccakLanes keccakRound(CKeccakLanes a, uint64_t roundConstant)
{
  uint64_t c0 = a.L00 ^ a.L05 ^ a.L10 ^ a.L15 ^ a.L20;
  uint64_t c1 = a.L01 ^ a.L06 ^ a.L11 ^ a.L16 ^ a.L21;
  uint64_t c2 = a.L02 ^ a.L07 ^ a.L12 ^ a.L17 ^ a.L22;
  uint64_t c3 = a.L03 ^ a.L08 ^ a.L13 ^ a.L18 ^ a.L23;
  uint64_t c4 = a.L04 ^ a.L09 ^ a.L14 ^ a.L19 ^ a.L24;

  uint64_t d0 = c4 ^ rol64(c1, 1);
  uint64_t d1 = c0 ^ rol64(c2, 1);
  uint64_t d2 = c1 ^ rol64(c3, 1);
  uint64_t d3 = c2 ^ rol64(c4, 1);
  uint64_t d4 = c3 ^ rol64(c0, 1);

  CKeccakLanes e;
  uint64_t b0, b1, b2, b3, b4;

  b0 = a.L00 ^ d0;
  b1 = rol64(a.L06 ^ d1, 44);
  b2 = rol64(a.L12 ^ d2, 43);
  b3 = rol64(a.L18 ^ d3, 21);
  b4 = rol64(a.L24 ^ d4, 14);
  e.L00 = (b0 ^ (~b1 & b2)) ^ roundConstant;
  e.L01 = b1 ^ (~b2 & b3);
  e.L02 = b2 ^ (~b3 & b4);
  e.L03 = b3 ^ (~b4 & b0);
  e.L04 = b4 ^ (~b0 & b1);

  b0 = rol64(a.L03 ^ d3, 28);
  b1 = rol64(a.L09 ^ d4, 20);
  b2 = rol64(a.L10 ^ d0, 3);
  b3 = rol64(a.L16 ^ d1, 45);
  b4 = rol64(a.L22 ^ d2, 61);
  e.L05 = b0 ^ (~b1 & b2);
  e.L06 = b1 ^ (~b2 & b3);
  e.L07 = b2 ^ (~b3 & b4);
  e.L08 = b3 ^ (~b4 & b0);
  e.L09 = b4 ^ (~b0 & b1);

  b0 = rol64(a.L01 ^ d1, 1);
  b1 = rol64(a.L07 ^ d2, 6);
  b2 = rol64(a.L13 ^ d3, 25);
  b3 = rol64(a.L19 ^ d4, 8);
  b4 = rol64(a.L20 ^ d0, 18);
  e.L10 = b0 ^ (~b1 & b2);
  e.L11 = b1 ^ (~b2 & b3);
  e.L12 = b2 ^ (~b3 & b4);
  e.L13 = b3 ^ (~b4 & b0);
  e.L14 = b4 ^ (~b0 & b1);

  b0 = rol64(a.L04 ^ d4, 27);
  b1 = rol64(a.L05 ^ d0, 36);
  b2 = rol64(a.L11 ^ d1, 10);
  b3 = rol64(a.L17 ^ d2, 15);
  b4 = rol64(a.L23 ^ d3, 56);
  e.L15 = b0 ^ (~b1 & b2);
  e.L16 = b1 ^ (~b2 & b3);
  e.L17 = b2 ^ (~b3 & b4);
  e.L18 = b3 ^ (~b4 & b0);
  e.L19 = b4 ^ (~b0 & b1);

  b0 = rol64(a.L02 ^ d2, 62);
  b1 = rol64(a.L08 ^ d3, 55);
  b2 = rol64(a.L14 ^ d4, 39);
  b3 = rol64(a.L15 ^ d0, 41);
  b4 = rol64(a.L21 ^ d1, 2);
  e.L20 = b0 ^ (~b1 & b2);
  e.L21 = b1 ^ (~b2 & b3);
  e.L22 = b2 ^ (~b3 & b4);
  e.L23 = b3 ^ (~b4 & b0);
  e.L24 = b4 ^ (~b0 & b1);
  return e;
}

// Two rounds at a time, so what a round produces feeds the next one as values instead of
// being copied back over the lanes it came from
void sha3llTransform(uint64_t state[25])
{
  CKeccakLanes a, e;
  a.L00 = state[0];
  a.L01 = state[1];
  a.L02 = state[2];
  a.L03 = state[3];
  a.L04 = state[4];
  a.L05 = state[5];
  a.L06 = state[6];
  a.L07 = state[7];
  a.L08 = state[8];
  a.L09 = state[9];
  a.L10 = state[10];
  a.L11 = state[11];
  a.L12 = state[12];
  a.L13 = state[13];
  a.L14 = state[14];
  a.L15 = state[15];
  a.L16 = state[16];
  a.L17 = state[17];
  a.L18 = state[18];
  a.L19 = state[19];
  a.L20 = state[20];
  a.L21 = state[21];
  a.L22 = state[22];
  a.L23 = state[23];
  a.L24 = state[24];

  for (unsigned i = 0; i < 24; i += 2) {
    e = keccakRound(a, KeccakConstants1600[i]);
    a = keccakRound(e, KeccakConstants1600[i + 1]);
  }

  state[0] = a.L00;
  state[1] = a.L01;
  state[2] = a.L02;
  state[3] = a.L03;
  state[4] = a.L04;
  state[5] = a.L05;
  state[6] = a.L06;
  state[7] = a.L07;
  state[8] = a.L08;
  state[9] = a.L09;
  state[10] = a.L10;
  state[11] = a.L11;
  state[12] = a.L12;
  state[13] = a.L13;
  state[14] = a.L14;
  state[15] = a.L15;
  state[16] = a.L16;
  state[17] = a.L17;
  state[18] = a.L18;
  state[19] = a.L19;
  state[20] = a.L20;
  state[21] = a.L21;
  state[22] = a.L22;
  state[23] = a.L23;
  state[24] = a.L24;

}

// A lane is little endian, so absorbing is a xor of whole lanes wherever the offset allows it
// and a xor of single bytes where it does not - which is only the tail of an update that did
// not end on a lane boundary.
static void absorb(uint64_t state[25], size_t offset, const uint8_t *data, size_t size)
{
  if (offset % 8 == 0) {
    uint64_t *lane = state + offset / 8;
    while (size >= 8) {
      *lane++ ^= loadle64(data);
      data += 8;
      size -= 8;
    }
    offset = (size_t)(lane - state) * 8;
  }

  for (size_t i = 0; i < size; i++) {
    const size_t position = offset + i;
    state[position / 8] ^= (uint64_t)data[i] << (8 * (position % 8));
  }
}

static void absorbByte(uint64_t state[25], size_t offset, uint8_t value)
{
  state[offset / 8] ^= (uint64_t)value << (8 * (offset % 8));
}

static void squeeze(const uint64_t state[25], uint8_t *hash, unsigned hashSize)
{
  unsigned i = 0;
  for (; i + 8 <= hashSize; i += 8)
    storele64(hash + i, state[i / 8]);
  for (; i < hashSize; i++)
    hash[i] = (uint8_t)(state[i / 8] >> (8 * (i % 8)));
}

static void spongeInit(CCtxSha3 *ctx, unsigned hashSize, uint32_t padding)
{
  ctx->HashSize = hashSize;
  ctx->BlockSize = 200 - 2 * hashSize;
  ctx->BufferSize = 0;
  ctx->Padding = padding;
  memset(ctx->State, 0, sizeof(ctx->State));
}

static void spongeUpdate(CCtxSha3 *ctx, const void *data, size_t size)
{
  const uint8_t *p = (const uint8_t*)data;
  size_t remaining = size;

  if (ctx->BufferSize) {
    // step 1: finish the block that was left open, and transform if it filled up
    size_t maxSize = ctx->BlockSize - ctx->BufferSize;
    size_t copySize = maxSize <= remaining ? maxSize : remaining;
    absorb(ctx->State, ctx->BufferSize, p, copySize);
    ctx->BufferSize += copySize;
    remaining -= copySize;
    p += copySize;
    if (ctx->BufferSize == ctx->BlockSize) {
      sha3llTransform(ctx->State);
      ctx->BufferSize = 0;
    }
  }

  if (remaining) {
    // step 2: whole blocks
    size_t fullRounds = remaining / ctx->BlockSize;
    for (size_t i = 0; i < fullRounds; i++) {
      absorb(ctx->State, 0, p, ctx->BlockSize);
      sha3llTransform(ctx->State);
      p += ctx->BlockSize;
    }
    remaining -= fullRounds * ctx->BlockSize;

    // step 3: what is left opens the next block
    absorb(ctx->State, 0, p, remaining);
    ctx->BufferSize = (uint32_t)remaining;
  }
}

static void spongeFinal(CCtxSha3 *ctx, uint8_t *hash)
{
  // Always exactly one transform here: the padded block is the last one
  absorbByte(ctx->State, ctx->BufferSize, (uint8_t)ctx->Padding);
  absorbByte(ctx->State, ctx->BlockSize - 1, 0x80);
  sha3llTransform(ctx->State);
  squeeze(ctx->State, hash, ctx->HashSize);
}

static void spongeAll(const void *data, size_t size, uint8_t *hash, unsigned hashSize, uint32_t padding)
{
  CCtxSha3 ctx;
  spongeInit(&ctx, hashSize, padding);
  spongeUpdate(&ctx, data, size);
  spongeFinal(&ctx, hash);
}

void sha3Init(CCtxSha3 *ctx, unsigned hashSize) { spongeInit(ctx, hashSize, Sha3Padding); }
void sha3Update(CCtxSha3 *ctx, const void *data, size_t size) { spongeUpdate(ctx, data, size); }
void sha3Final(CCtxSha3 *ctx, uint8_t *hash) { spongeFinal(ctx, hash); }

void sha3(const void *data, size_t size, uint8_t *hash, unsigned hashSize)
{
  spongeAll(data, size, hash, hashSize, Sha3Padding);
}

void keccakInit(CCtxSha3 *ctx, unsigned hashSize) { spongeInit(ctx, hashSize, KeccakPadding); }
void keccakUpdate(CCtxSha3 *ctx, const void *data, size_t size) { spongeUpdate(ctx, data, size); }
void keccakFinal(CCtxSha3 *ctx, uint8_t *hash) { spongeFinal(ctx, hash); }

void keccak(const void *data, size_t size, uint8_t *hash, unsigned hashSize)
{
  spongeAll(data, size, hash, hashSize, KeccakPadding);
}
