#pragma once
#include <stddef.h>
#include <stdint.h>

// __builtin_bswap* are GCC/clang builtins; MSVC has its own intrinsics
#if defined(_MSC_VER)
#include <stdlib.h>
static inline uint32_t bswap32(uint32_t value) { return _byteswap_ulong(value); }
static inline uint64_t bswap64(uint64_t value) { return _byteswap_uint64(value); }
#else
static inline uint32_t bswap32(uint32_t value) { return __builtin_bswap32(value); }
static inline uint64_t bswap64(uint64_t value) { return __builtin_bswap64(value); }
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Low level
void sha256llInit(uint32_t state[8]);
void sha256llTransform(uint32_t state[8], const uint32_t in[16], int bswap);
void sha256llFinal(const uint32_t state[8], uint8_t *hash, int bswap);

// High level
typedef struct CCtxSha256 {
  uint32_t state[16];
  uint8_t buffer[64];
  uint32_t bufferSize;
  size_t MsgSize;
} CCtxSha256;

void sha256Init(CCtxSha256 *ctx);
void sha256Update(CCtxSha256 *ctx, const void *data, size_t size);
void sha256Final(CCtxSha256 *ctx, uint8_t *hash);
void sha256(const void *data, size_t size, uint8_t *hash);

#ifdef __cplusplus
}
#endif
