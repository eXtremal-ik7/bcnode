#pragma once
#include <stddef.h>
#include <stdint.h>

enum { LTC_SCRATCHPAD_SIZE = 131072 + 63 };

// Inputs a multi-way kernel hashes in one pass: eight 32 bit lanes fill a 512 bit register.
// Callers group by this even while the kernel is single lane
enum { SCRYPT_WAYS = 8 };

#ifdef __cplusplus
extern "C" {
#endif

void scrypt_1024_1_1_256(const void *input, uint8_t output[32]);
// Independent 80 byte inputs hashed together, 32 bytes of output each
void scrypt_1024_1_1_256_multi(const void *const *inputs, uint8_t *outputs, size_t count);

#ifdef __cplusplus
}
#endif
