#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Low level
void sha3llRound800(uint32_t state[25], unsigned round);
void sha3llTransform(uint64_t state[25]);

// High level
//
// SHA-3 and Keccak are the same sponge and differ in one byte of padding, so one context
// serves both and carries which one it was started as. The families are kept apart in the
// interface rather than behind a flag: a caller asks for keccak256 or for sha3-256, never for
// "sha3 with the other padding".
typedef struct CCtxSha3 {
  uint64_t State[25];
  uint32_t HashSize;
  uint32_t BlockSize;
  uint32_t BufferSize;
  uint32_t Padding;
} CCtxSha3;

void sha3Init(CCtxSha3 *ctx, unsigned hashSize);
void sha3Update(CCtxSha3 *ctx, const void *data, size_t size);
void sha3Final(CCtxSha3 *ctx, uint8_t *hash);
void sha3(const void *data, size_t size, uint8_t *hash, unsigned hashSize);

void keccakInit(CCtxSha3 *ctx, unsigned hashSize);
void keccakUpdate(CCtxSha3 *ctx, const void *data, size_t size);
void keccakFinal(CCtxSha3 *ctx, uint8_t *hash);
void keccak(const void *data, size_t size, uint8_t *hash, unsigned hashSize);

#ifdef __cplusplus
}
#endif
