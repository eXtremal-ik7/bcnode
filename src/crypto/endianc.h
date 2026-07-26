#ifndef ENDIANC_H
#define ENDIANC_H

#include <stdint.h>

/*
 * Compile-time endianness detection via compiler/platform macros.
 * Resolves to a constant 0 or 1 — optimized away by the compiler.
 */

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
  /* GCC >= 4.6, Clang, ICC, ARM Compiler 6, TinyCC */
  #define ENDIANC_IS_LITTLE (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  #define ENDIANC_IS_BIG    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

#elif defined(_MSC_VER)
  /* MSVC — only targets little-endian (x86, x64, ARM LE) */
  #define ENDIANC_IS_LITTLE 1
  #define ENDIANC_IS_BIG    0

#elif defined(__LITTLE_ENDIAN__) || defined(__ARMEL__) || defined(__THUMBEL__) || \
      defined(__AARCH64EL__)     || defined(_MIPSEL)   || defined(__MIPSEL)    || \
      defined(__MIPSEL__)        || defined(__riscv)    || defined(__x86_64__)  || \
      defined(__x86_64)          || defined(__i386__)   || defined(__i386)      || \
      defined(_M_IX86)           || defined(_M_X64)     || defined(_M_AMD64)   || \
      defined(_M_ARM)            || defined(_M_ARM64)   || defined(__alpha__)   || \
      defined(__ia64__)          || defined(_M_IA64)
  #define ENDIANC_IS_LITTLE 1
  #define ENDIANC_IS_BIG    0

#elif defined(__BIG_ENDIAN__)  || defined(__ARMEB__)   || defined(__THUMBEB__) || \
      defined(__AARCH64EB__)   || defined(_MIPSEB)     || defined(__MIPSEB)    || \
      defined(__MIPSEB__)      || defined(__sparc)      || defined(__sparc__)   || \
      defined(__s390__)        || defined(__s390x__)    || defined(__hppa)      || \
      defined(__m68k__)        || defined(__mc68000__)
  #define ENDIANC_IS_LITTLE 0
  #define ENDIANC_IS_BIG    1

#else
  #error "endianc.h: cannot determine byte order for this platform"
#endif

static inline int isLittleEndian(void) {
  return ENDIANC_IS_LITTLE;
}

static inline int isBigEndian(void) {
  return ENDIANC_IS_BIG;
}

/* --- byte swap intrinsics --- */

static inline uint16_t bswap16(uint16_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap16(v);
#elif defined(_MSC_VER)
  return _byteswap_ushort(v);
#else
  return (uint16_t)((v >> 8) | (v << 8));
#endif
}

static inline uint32_t bswap32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap32(v);
#elif defined(_MSC_VER)
  return _byteswap_ulong(v);
#else
  return ((v >> 24))              |
         ((v >>  8) & 0x0000FF00) |
         ((v <<  8) & 0x00FF0000) |
         ((v << 24));
#endif
}

static inline uint64_t bswap64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap64(v);
#elif defined(_MSC_VER)
  return _byteswap_uint64(v);
#else
  return ((v >> 56))                         |
         ((v >> 40) & 0x000000000000FF00ULL) |
         ((v >> 24) & 0x0000000000FF0000ULL) |
         ((v >>  8) & 0x00000000FF000000ULL) |
         ((v <<  8) & 0x000000FF00000000ULL) |
         ((v << 24) & 0x0000FF0000000000ULL) |
         ((v << 40) & 0x00FF000000000000ULL) |
         ((v << 56));
#endif
}

/* --- endian value conversion --- */

static inline uint16_t readle16(uint16_t v)  { return ENDIANC_IS_LITTLE ? v : bswap16(v); }
static inline uint32_t readle32(uint32_t v)  { return ENDIANC_IS_LITTLE ? v : bswap32(v); }
static inline uint64_t readle64(uint64_t v)  { return ENDIANC_IS_LITTLE ? v : bswap64(v); }

static inline uint16_t readbe16(uint16_t v)  { return ENDIANC_IS_BIG ? v : bswap16(v); }
static inline uint32_t readbe32(uint32_t v)  { return ENDIANC_IS_BIG ? v : bswap32(v); }
static inline uint64_t readbe64(uint64_t v)  { return ENDIANC_IS_BIG ? v : bswap64(v); }

static inline uint16_t writele16(uint16_t v) { return ENDIANC_IS_LITTLE ? v : bswap16(v); }
static inline uint32_t writele32(uint32_t v) { return ENDIANC_IS_LITTLE ? v : bswap32(v); }
static inline uint64_t writele64(uint64_t v) { return ENDIANC_IS_LITTLE ? v : bswap64(v); }

static inline uint16_t writebe16(uint16_t v) { return ENDIANC_IS_BIG ? v : bswap16(v); }
static inline uint32_t writebe32(uint32_t v) { return ENDIANC_IS_BIG ? v : bswap32(v); }
static inline uint64_t writebe64(uint64_t v) { return ENDIANC_IS_BIG ? v : bswap64(v); }

/* --- load from memory with endian conversion --- */

static inline uint16_t loadle16(const void *p) { return readle16(*(const uint16_t*)p); }
static inline uint32_t loadle32(const void *p) { return readle32(*(const uint32_t*)p); }
static inline uint64_t loadle64(const void *p) { return readle64(*(const uint64_t*)p); }

static inline uint16_t loadbe16(const void *p) { return readbe16(*(const uint16_t*)p); }
static inline uint32_t loadbe32(const void *p) { return readbe32(*(const uint32_t*)p); }
static inline uint64_t loadbe64(const void *p) { return readbe64(*(const uint64_t*)p); }

/* --- store to memory with endian conversion --- */

static inline void storele16(void *p, uint16_t v) { *(uint16_t*)p = writele16(v); }
static inline void storele32(void *p, uint32_t v) { *(uint32_t*)p = writele32(v); }
static inline void storele64(void *p, uint64_t v) { *(uint64_t*)p = writele64(v); }

static inline void storebe16(void *p, uint16_t v) { *(uint16_t*)p = writebe16(v); }
static inline void storebe32(void *p, uint32_t v) { *(uint32_t*)p = writebe32(v); }
static inline void storebe64(void *p, uint64_t v) { *(uint64_t*)p = writebe64(v); }

#endif /* ENDIANC_H */
