#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Low level
void ripemd160llInit(uint32_t state[5]);
void ripemd160llTransform(uint32_t state[5], const uint8_t block[64]);
void ripemd160llFinal(const uint32_t state[5], uint8_t *hash);

// High level
typedef struct CCtxRipemd160 {
  uint32_t state[5];
  uint8_t buffer[64];
  uint32_t bufferSize;
  size_t MsgSize;
} CCtxRipemd160;

void ripemd160Init(CCtxRipemd160 *ctx);
void ripemd160Update(CCtxRipemd160 *ctx, const void *data, size_t size);
void ripemd160Final(CCtxRipemd160 *ctx, uint8_t *hash);
void ripemd160(const void *data, size_t size, uint8_t *hash);

#ifdef __cplusplus
}
#endif
