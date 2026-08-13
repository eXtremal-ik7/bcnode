#pragma once

#include "endiantools.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <array>
#include <limits>
#include <cfloat>
#include <cmath>

#include <assert.h>

#ifdef __GNUC__
#if defined(__x86_64__) || defined(__i386__)
#include "x86intrin.h"
#endif
#endif

#ifdef _MSC_VER
#include "intrin.h"
#endif

#include <string>
#include <type_traits>

// A 128/64 divide in hardware, which only x86-64 has. Everything about division
// below turns on it; LIBPOW_FORCE_PORTABLE puts an x86 build on the other path
#if !defined(LIBPOW_FORCE_PORTABLE)
#if (defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))) || defined(_M_X64)
#define LIBPOW_HAS_DIV128 1
#endif
#endif

static_assert(sizeof(unsigned int) == sizeof(uint32_t));
static_assert(sizeof(unsigned long long) == sizeof(uint64_t));
static_assert(sizeof(unsigned int) == 4 || sizeof(unsigned int) == 8);
static_assert(sizeof(unsigned long) == 4 || sizeof(unsigned long) == 8);

static inline void addc64(uint64_t *a, uint64_t b, uint64_t *carryFlag)
{
#ifdef __clang__
  unsigned long long carry = *carryFlag;
  *a = __builtin_addcll(*a, b, carry, &carry);
  *carryFlag = carry;
#elif __GNUC__
  // use intrinsic for x86
#if defined(__x86_64__) || defined(__i386__)
  *carryFlag = _addcarry_u64(*carryFlag, *a, b, reinterpret_cast<unsigned long long*>(a));
#else
  unsigned __int128 r = static_cast<unsigned __int128>(*a) + b + *carryFlag;
  *a = r;
  *carryFlag = r >> 64;
#endif
#elif _MSC_VER
  *carryFlag = _addcarry_u64(static_cast<unsigned char>(*carryFlag), *a, b, a);
#endif
}

static inline void subb64(uint64_t *a, uint64_t b, uint64_t *borrowFlag)
{
#ifdef __clang__
  unsigned long long borrow = *borrowFlag;
  *a = __builtin_subcll(*a, b, borrow, &borrow);
  *borrowFlag = borrow;
#elif __GNUC__
  // use intrinsic for x86
#if defined(__x86_64__) || defined(__i386__)
  *borrowFlag = _subborrow_u64(*borrowFlag, *a, b, reinterpret_cast<unsigned long long*>(a));
#else
  unsigned __int128 r = static_cast<unsigned __int128>(*a) - b - *borrowFlag;
  *a = r;
  *borrowFlag = -(r >> 64);
#endif
#elif _MSC_VER
  *borrowFlag = _subborrow_u64(static_cast<unsigned char>(*borrowFlag), *a, b, a);
#endif
}

static inline void mulx64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi)
{
#ifndef _MSC_VER
  unsigned __int128 result = static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
  *lo = result;
  *hi = result >> 64;
#else
  *lo = a * b;
  *hi = __umulh(a, b);
#endif
}

// Division of (hi:lo) by divisor. The quotient has to fit a limb, that is
// hi < divisor: every caller either guards the estimate or normalizes for it.
static inline void divmod64(uint64_t lo, uint64_t hi, uint64_t divisor, uint64_t *result, uint64_t *remainder)
{
  assert(hi < divisor);
  // Dispatched on the toolchain, not on LIBPOW_HAS_DIV128: that one says the
  // divide exists, this says how it is reached
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  // By hand, because both compilers turn the __int128 form below into a call to
  // libgcc: they cannot know the quotient fits, and the hardware divide faults
  // when it does not
  uint64_t q, r;
  __asm__("divq %[divisor]" : "=a"(q), "=d"(r) : [divisor]"rm"(divisor), "a"(lo), "d"(hi));
  *result = q;
  *remainder = r;
#elif defined(_M_X64)
  *result = _udiv128(hi, lo, divisor, remainder);
#else
  *result = ((static_cast<unsigned __int128>(hi) << 64) + lo) / divisor;
  *remainder = ((static_cast<unsigned __int128>(hi) << 64) + lo) % divisor;
#endif
}

static inline unsigned clz64(uint64_t x)
{
  assert(x != 0);
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<unsigned>(__builtin_clzll(x));
#elif defined(_MSC_VER)
  unsigned long idx;
#if defined(_M_X64) || defined(_M_ARM64)
  _BitScanReverse64(&idx, x);
  return 63u - static_cast<unsigned>(idx);
#else
  uint32_t hi = static_cast<uint32_t>(x >> 32);
  if (hi) {
    _BitScanReverse(&idx, hi);
    return 31u - static_cast<unsigned>(idx);
  } else {
    uint32_t lo = static_cast<uint32_t>(x);
    _BitScanReverse(&idx, lo);
    return 63u - static_cast<unsigned>(idx);
  }
#endif
#else
  unsigned n = 0;
  while ((x & (1ULL << 63)) == 0) { x <<= 1; ++n; }
  return n;
#endif
}

// lo = a*b + addend + *carry, high half left in *carry. A full product plus two
// limbs always fits 128 bits, so nothing escapes.
static inline uint64_t mulAdd64(uint64_t a, uint64_t b, uint64_t addend, uint64_t *carry)
{
#if defined(__SIZEOF_INT128__) && !defined(LIBPOW_NO_INT128)
  unsigned __int128 r = static_cast<unsigned __int128>(a) * b + addend + *carry;
  *carry = static_cast<uint64_t>(r >> 64);
  return static_cast<uint64_t>(r);
#else
  // adc into the high half instead of pulling the carry out into a register:
  // by value it costs setb+movzbl+add per step and breaks the flag chain
  uint64_t lo, hi;
  uint64_t c = 0;
  mulx64(a, b, &lo, &hi);
  addc64(&lo, addend, &c);
  addc64(&hi, 0, &c);
  c = 0;
  addc64(&lo, *carry, &c);
  addc64(&hi, 0, &c);
  *carry = hi;
  return lo;
#endif
}

#ifndef LIBPOW_HAS_DIV128
// Seed for the reciprocal below: the first nine bits of 1/d, indexed by the top
// nine bits of d
static constexpr std::array<uint16_t, 256> ReciprocalTable = []() {
  std::array<uint16_t, 256> table{};
  for (size_t i = 0; i < table.size(); i++)
    table[i] = static_cast<uint16_t>(0x7fd00 / (i + 256));
  return table;
}();
#endif

// Every pair of decimal digits, so the decimal conversion can take two at a time
static constexpr std::array<char, 200> DecimalPairs = []() {
  std::array<char, 200> table{};
  for (size_t i = 0; i < 100; i++) {
    table[2*i] = static_cast<char>('0' + i / 10);
    table[2*i + 1] = static_cast<char>('0' + i % 10);
  }
  return table;
}();

// (2^128 - 1) / d - 2^64 for a normalized d, i.e. as much of 1/d as fits in a
// limb.
static inline uint64_t reciprocal2by1(uint64_t d)
{
  assert(d & (uint64_t{1} << 63));

#ifdef LIBPOW_HAS_DIV128
  // Where a 128/64 divide exists in hardware it beats the Newton chain below by
  // a factor of two: the quotient here fits a limb precisely because d is
  // normalized, so 2^64-1-d is already below d
  uint64_t q, r;
  divmod64(~uint64_t{0}, ~uint64_t{0} - d, d, &q, &r);
  return q;
#else
  // Algorithm 2 of Moller-Granlund "Improved division by invariant integers":
  // three Newton steps off the table, then one exact correction
  const uint32_t v0 = ReciprocalTable[static_cast<size_t>((d >> 55) - 256)];

  const uint64_t d40 = (d >> 24) + 1;
  const uint64_t v1 = (v0 << 11) - static_cast<uint32_t>(static_cast<uint32_t>(v0 * v0) * d40 >> 40) - 1;
  const uint64_t v2 = (v1 << 13) + ((v1 * (0x1000000000000000ull - v1 * d40)) >> 47);

  const uint64_t d0 = d & 1;
  const uint64_t d63 = (d >> 1) + d0;                       // ceil(d/2)
  const uint64_t e = ((v2 >> 1) & (0 - d0)) - v2 * d63;

  uint64_t lo, hi;
  mulx64(v2, e, &lo, &hi);
  const uint64_t v3 = (hi >> 1) + (v2 << 31);

  mulx64(v3, d, &lo, &hi);
  uint64_t carry = 0;
  addc64(&lo, d, &carry);
  return v3 - (hi + carry) - d;
#endif
}

// (u1:u0) / d for a normalized d and v = reciprocal2by1(d). The quotient fits a
// limb because the caller keeps u1 < d
static inline uint64_t divrem2by1(uint64_t u1, uint64_t u0, uint64_t d, uint64_t v, uint64_t *remainder)
{
  assert(u1 < d);

  uint64_t qlo, qhi;
  mulx64(v, u1, &qlo, &qhi);

  uint64_t carry = 0;
  addc64(&qlo, u0, &carry);
  addc64(&qhi, u1, &carry);
  qhi++;

  uint64_t r = u0 - qhi * d;
  if (r > qlo) {
    qhi--;
    r += d;
  }
  if (r >= d) {
    qhi++;
    r -= d;
  }

  *remainder = r;
  return qhi;
}

// Short division of a limb vector, quotient into q, which may alias u. It is a
// loop of its own so that one reciprocal can cover the whole pass
static inline uint64_t divmodLimbs(const uint64_t *u, size_t limbs, uint64_t d, uint64_t *q)
{
#ifdef LIBPOW_HAS_DIV128
  uint64_t rem = 0;
  for (size_t i = limbs; i-- > 0; )
    divmod64(u[i], rem, d, &q[i], &rem);
  return rem;
#else
  // The reciprocal wants the top bit of the divisor set, so both are shifted here
  const unsigned s = clz64(d);
  const unsigned invS = (64 - s) & 63;
  const uint64_t sMask = -static_cast<uint64_t>(s != 0);   // shifting by 64 is UB
  const uint64_t dn = d << s;
  const uint64_t v = reciprocal2by1(dn);

  uint64_t rem = (u[limbs - 1] >> invS) & sMask;
  for (size_t i = limbs; i-- > 0; ) {
    const uint64_t low = i ? ((u[i - 1] >> invS) & sMask) : 0;
    q[i] = divrem2by1(rem, (u[i] << s) | low, dn, v, &rem);
  }
  return rem >> s;
#endif
}

// The same for a two-limb divisor, which is what a Knuth digit needs
static inline uint64_t reciprocal3by2(uint64_t d1, uint64_t d0)
{
  uint64_t v = reciprocal2by1(d1);

  uint64_t p = d1 * v + d0;
  if (p < d0) {
    v--;
    if (p >= d1) { v--; p -= d1; }
    p -= d1;
  }

  uint64_t tlo, thi;
  mulx64(v, d0, &tlo, &thi);

  p += thi;
  if (p < thi) {
    v--;
    if (p >= d1 && (p > d1 || tlo >= d0))
      v--;
  }

  return v;
}

// (u2:u1:u0) / (d1:d0) for a normalized divisor and v = reciprocal3by2(d1, d0).
// The caller must know the quotient fits a limb: (u2:u1) < (d1:d0).
static inline uint64_t divrem3by2(uint64_t u2, uint64_t u1, uint64_t u0,
                                  uint64_t d1, uint64_t d0, uint64_t v,
                                  uint64_t *remainder1, uint64_t *remainder0)
{
  uint64_t qlo, qhi;
  mulx64(v, u2, &qlo, &qhi);

  uint64_t carry = 0;
  addc64(&qlo, u1, &carry);
  addc64(&qhi, u2, &carry);

  uint64_t r1 = u1 - qhi * d1;

  uint64_t tlo, thi;
  mulx64(d0, qhi, &tlo, &thi);

  // r = (r1:u0) - (thi:tlo) - (d1:d0)
  uint64_t r0 = u0;
  uint64_t borrow = 0;
  subb64(&r0, tlo, &borrow);
  subb64(&r1, thi, &borrow);
  borrow = 0;
  subb64(&r0, d0, &borrow);
  subb64(&r1, d1, &borrow);

  qhi++;

  if (r1 >= qlo) {
    qhi--;
    carry = 0;
    addc64(&r0, d0, &carry);
    addc64(&r1, d1, &carry);
  }

  if (r1 > d1 || (r1 == d1 && r0 >= d0)) {
    qhi++;
    borrow = 0;
    subb64(&r0, d0, &borrow);
    subb64(&r1, d1, &borrow);
  }

  *remainder1 = r1;
  *remainder0 = r0;
  return qhi;
}

template<unsigned Bits>
class UInt {
public:
  UInt() {
    memset(Data_, 0, sizeof(Data_));
  }

  // Allow construction from unsigned integral types only
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
  UInt(T n) {
    Data_[0] = static_cast<uint64_t>(n);
    for (size_t i = 1; i < LimbsNum; i++)
      Data_[i] = 0;
  }

  // Prevent implicit conversion from signed integers and floating point
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt(T) = delete;

  // Allow implicit construction only from smaller or equal size UInt
  template<unsigned OtherBits, std::enable_if_t<(OtherBits <= Bits), int> = 0>
  UInt(const UInt<OtherBits> &other) {
    constexpr size_t copyLimbs = UInt<OtherBits>::LimbsNum;
    memcpy(Data_, other.data(), copyLimbs * sizeof(uint64_t));
    if constexpr (LimbsNum > copyLimbs)
      memset(Data_ + copyLimbs, 0, (LimbsNum - copyLimbs) * sizeof(uint64_t));
  }

  // static constructors
  static UInt<Bits> zero() {
    UInt<Bits> result;
    memset(result.Data_, 0, sizeof(result.Data_));
    return result;
  }

  static UInt<Bits> max() {
    UInt<Bits> result;
    memset(result.Data_, 0xFF, sizeof(result.Data_));
    return result;
  }

  static UInt<Bits> fromHex(const char *hex) {
    UInt<Bits> result;
    result.setHex(hex);
    return result;
  }

  // Truncate from larger UInt (explicit conversion)
  template<unsigned OtherBits, std::enable_if_t<(OtherBits > Bits), int> = 0>
  static UInt<Bits> truncate(const UInt<OtherBits> &other) {
    UInt<Bits> result;
    memcpy(result.Data_, other.data(), LimbsNum * sizeof(uint64_t));
    return result;
  }

  static UInt<Bits> fromDouble(double d) {
    static double multiplier = std::pow(FLT_RADIX, DBL_MANT_DIG);

    int exponent = 0;
    uint64_t mantissa = static_cast<uint64_t>(std::frexp(d, &exponent) * multiplier);
    exponent -= DBL_MANT_DIG;

    UInt<Bits> result;
    result.Data_[0] = mantissa;
    for (size_t i = 1; i < LimbsNum; i++)
      result.Data_[i] = 0;

    if (exponent < 0)
      result.shr(-exponent);
    else
      result.shl(exponent);

    return result;
  }

  const uint64_t *data() const { return Data_; }
  uint64_t *data() { return Data_; }
  const uint8_t *rawData() const { return reinterpret_cast<const uint8_t*>(Data_); }
  uint8_t *rawData() { return reinterpret_cast<uint8_t*>(Data_); }
  size_t rawSize() const { return sizeof(Data_); }

  void exportLE(uint8_t *out) const {
    if (isLittleEndian()) {
      memcpy(out, Data_, sizeof(Data_));
    } else {
      uint64_t *out64 = reinterpret_cast<uint64_t*>(out);
      for (unsigned i = 0; i < LimbsNum; i++)
        out64[i] = writele(Data_[i]);
    }
  }

  // Big endian is the order a word travels in outside the machine: protocol
  // fields, hashes, contract storage
  void exportBE(uint8_t *out) const {
    // Assembled first, stored once: a byte output may be unaligned, and eight
    // small stores into it cost more than one wide one
    uint64_t limbs[LimbsNum];
    for (size_t i = 0; i < LimbsNum; i++)
      limbs[i] = writebe(Data_[LimbsNum - 1 - i]);
    memcpy(out, limbs, sizeof(limbs));
  }

  void importBE(const uint8_t *in) {
    for (size_t i = 0; i < LimbsNum; i++) {
      uint64_t limb;
      memcpy(&limb, in + 8*i, sizeof(limb));
      Data_[LimbsNum - 1 - i] = readbe(limb);
    }
  }

  // Conversion functions
  uint64_t low64() const { return Data_[0]; }

  double getDouble() const {
    double ret = 0.0;
    double fact = 1.0;
    for (size_t i = 0; i < LimbsNum; i++) {
      ret += fact * Data_[i];
      fact *= 18446744073709551616.0; // 2^64
    }
    return ret;
  }

  // Divide two UInts and return result as double with minimal precision loss
  template<unsigned BitsA, unsigned BitsB>
  static double fpdiv(const UInt<BitsA> &dividend, const UInt<BitsB> &divisor) {
    if (divisor.isZero() || dividend.isZero())
      return 0.0;

    unsigned bitsA = dividend.bits();
    unsigned bitsB = divisor.bits();

    // Normalize both to fit in 64 bits for maximum precision
    UInt<BitsA> normA = dividend;
    UInt<BitsB> normB = divisor;
    int shiftA = 0;
    int shiftB = 0;

    if (bitsA > 64) {
      shiftA = bitsA - 64;
      normA >>= shiftA;
    }
    if (bitsB > 64) {
      shiftB = bitsB - 64;
      normB >>= shiftB;
    }

    double result = static_cast<double>(normA.low64()) / static_cast<double>(normB.low64());

    // Adjust for shifts: dividend was shifted right by shiftA, divisor by shiftB
    // So result needs to be multiplied by 2^(shiftA - shiftB)
    int totalShift = shiftA - shiftB;
    return std::ldexp(result, totalShift);
  }

  unsigned bits() const {
    for (size_t i = LimbsNum; i > 0; --i) {
      if (Data_[i - 1] != 0)
        return static_cast<unsigned>((i - 1) * 64 + (64 - clz64(Data_[i - 1])));
    }
    return 0;
  }

  // Significant bytes, which is what a length field or a gas price counts
  unsigned bytes() const { return (bits() + 7) / 8; }

  // A single byte, indexed from the most significant end; past the width is zero
  uint8_t byte(unsigned index) const {
    if (index >= Bits / 8)
      return 0;
    const unsigned offset = Bits / 8 - 1 - index;
    return static_cast<uint8_t>(Data_[offset / 8] >> (8 * (offset % 8)));
  }

  // Eight characters at a time; only the topmost group can be partial, and only
  // when the leading zeroes are dropped
  void getHex(char *out, bool leadingZeroes = true, bool zeroxPrefix = false, bool upperCase = false) const {
    if (zeroxPrefix) {
      *out++ = '0';
      *out++ = 'x';
    }

    char *p = out;

    // The full width gets its own loop: the trip count is a constant there, so
    // it unrolls even where the call is not inlined, and it is the common case
    if (leadingZeroes) {
      for (size_t i = 2 * LimbsNum; i-- > 0; ) {
        const uint64_t chars = writebe(hexOctet(halfLimb(i), upperCase));
        memcpy(p, &chars, sizeof(chars));
        p += 8;
      }
      *p = 0;
      return;
    }

    const unsigned significant = bits();
    if (significant == 0) {
      out[0] = '0';
      out[1] = 0;
      return;
    }

    const unsigned nibbles = (significant + 3) / 4;
    const size_t groups = (nibbles + 7) / 8;
    const unsigned firstGroupChars = nibbles - 8 * static_cast<unsigned>(groups - 1);
    size_t i = groups - 1;

    {
      // Character at a time, and only for this group: a copy of a length the
      // compiler cannot see is a call into libc, which costs more than the group
      const uint64_t chars = writebe(hexOctet(halfLimb(i), upperCase));
      char group[8];
      memcpy(group, &chars, sizeof(group));
      for (unsigned k = 8 - firstGroupChars; k < 8; k++)
        *p++ = group[k];
    }

    while (i-- > 0) {
      const uint64_t chars = writebe(hexOctet(halfLimb(i), upperCase));
      memcpy(p, &chars, sizeof(chars));
      p += 8;
    }

    *p = 0;
  }

  std::string getHex(bool leadingZeroes = true, bool zeroxPrefix = false, bool upperCase = false) const {
    char data[Bits/4 + 8];
    getHex(data, leadingZeroes, zeroxPrefix, upperCase);
    return data;
  }

  // Decimal conversion
  //
  // Nineteen digits per pass over the number, not one: a pass costs a divide per
  // limb whatever the divisor is, so a whole chunk comes out for the price of a
  // single digit. The passes also shrink with the value instead of always
  // sweeping the full width
  std::string getDecimal() const {
    uint64_t u[LimbsNum];
    memcpy(u, Data_, sizeof(u));

    size_t limbs = LimbsNum;
    while (limbs > 0 && u[limbs - 1] == 0)
      limbs--;

    char buffer[Bits / 3 + 2];
    char *p = buffer + sizeof(buffer);

    while (limbs > 1) {
      uint64_t rem = 0;
      for (size_t i = limbs; i-- > 0; )
        ::divmod64(u[i], rem, DecimalChunk, &u[i], &rem);
      // No guard on limbs: two limbs and up always leave something above zero
      while (u[limbs - 1] == 0)
        limbs--;

      emitDecimalChunk(p, rem);   // padded: there is more above it
      p -= DecimalChunkDigits;
    }

    // The top chunk is the only one without padding, and it is a whole limb, so
    // it can be one digit longer than the chunks below
    uint64_t value = limbs ? u[0] : 0;
    while (value >= 100) {
      const uint64_t next = value / 100;
      p -= 2;
      memcpy(p, &DecimalPairs[2 * static_cast<size_t>(value - next * 100)], 2);
      value = next;
    }
    if (value >= 10) {
      p -= 2;
      memcpy(p, &DecimalPairs[2 * static_cast<size_t>(value)], 2);
    } else {
      *--p = static_cast<char>('0' + value);
    }

    return std::string(p, static_cast<size_t>(buffer + sizeof(buffer) - p));
  }

  void setHex(const char *hex) {
    if (hex[0] == '0' && hex[1] == 'x')
      hex += 2;

    // Search end of hex string
    const char *p = hex;
    while (isHexDigit(*p))
      p++;

    // What does not fit is dropped from the top, as before
    size_t length = static_cast<size_t>(p - hex);
    if (length > Bits / 4) {
      hex = p - Bits / 4;
      length = Bits / 4;
    }

    memset(Data_, 0, sizeof(Data_));

    // Backwards in whole limbs: sixteen characters are exactly one limb, and the
    // scan above has already proved they are all hex digits
    size_t limbOffset = 0;
    while (length >= 16) {
      p -= 16;
      Data_[limbOffset++] = (static_cast<uint64_t>(parseHexOctet(p)) << 32) | parseHexOctet(p + 8);
      length -= 16;
    }

    if (length) {
      uint64_t limb = 0;
      for (size_t i = 0; i < length; i++)
        limb = (limb << 4) | decodeHex(hex[i]);
      Data_[limbOffset] = limb;
    }
  }

  // Compare functions
  // Folded, not short-circuited: the branch chain a loop compiles to costs more
  // than the three or-s it skips, and it stalls whenever zero and non-zero
  // operands are mixed
  bool isZero() const {
    uint64_t acc = 0;
    for (size_t i = 0; i < LimbsNum; i++)
      acc |= Data_[i];
    return acc == 0;
  }

  bool nonZero() const { return !isZero(); }

  // UInt is unsigned, but this function can be used to detect
  // underflow after subtraction (MSB set means wrapped around)
  bool isNegative() const {
    return (Data_[LimbsNum - 1] >> 63) != 0;
  }

  // Folded like isZero above: the branch per limb this replaces mispredicts
  // whenever short and full-width values are compared in the same place
  int cmp64(uint64_t n) const {
    uint64_t high = 0;
    for (size_t i = 1; i < LimbsNum; i++)
      high |= Data_[i];
    if (high)
      return 1;
    if (Data_[0] < n) return -1;
    if (Data_[0] > n) return 1;
    return 0;
  }

  template<unsigned OperandBits>
  int cmp(const UInt<OperandBits> &n) const {
    if constexpr (OperandBits == Bits)
      return compare(n);
    else
      return cmp(Data_, LimbsNum, n.Data_, n.LimbsNum);
  }

  // Same width, from the top down: the leading limbs settle the question on the
  // first step for all but a vanishing share of operands, so the branch is
  // predicted and the answer does not wait on a borrow chain the whole width
  bool less(const UInt<Bits> &n) const {
    for (size_t i = LimbsNum; i-- > 1; ) {
      if (Data_[i] != n.Data_[i])
        return Data_[i] < n.Data_[i];
    }
    return Data_[0] < n.Data_[0];
  }

  int compare(const UInt<Bits> &n) const {
    for (size_t i = LimbsNum; i-- > 0; ) {
      if (Data_[i] != n.Data_[i])
        return Data_[i] < n.Data_[i] ? -1 : 1;
    }
    return 0;
  }

  // Equality does not need the ordering, and the ordered compare pays for a
  // branch per limb to answer what one or-reduce settles
  bool equal(const UInt<Bits> &n) const {
    uint64_t acc = 0;
    for (size_t i = 0; i < LimbsNum; i++)
      acc |= Data_[i] ^ n.Data_[i];
    return acc == 0;
  }

  // Two's complement ordering. Flipping the sign bit turns the
  // signed comparison of the top limb into an ordinary one, so the whole thing
  // is the unsigned compare above with one extra xor
  bool sless(const UInt<Bits> &n) const {
    constexpr uint64_t SignBit = uint64_t{1} << 63;
    const uint64_t left = Data_[LimbsNum - 1] ^ SignBit;
    const uint64_t right = n.Data_[LimbsNum - 1] ^ SignBit;
    if (left != right)
      return left < right;

    for (size_t i = LimbsNum - 1; i-- > 1; ) {
      if (Data_[i] != n.Data_[i])
        return Data_[i] < n.Data_[i];
    }
    return Data_[0] < n.Data_[0];
  }

  int scmp(const UInt<Bits> &n) const {
    constexpr uint64_t SignBit = uint64_t{1} << 63;
    const uint64_t left = Data_[LimbsNum - 1] ^ SignBit;
    const uint64_t right = n.Data_[LimbsNum - 1] ^ SignBit;
    if (left != right)
      return left < right ? -1 : 1;

    for (size_t i = LimbsNum - 1; i-- > 0; ) {
      if (Data_[i] != n.Data_[i])
        return Data_[i] < n.Data_[i] ? -1 : 1;
    }
    return 0;
  }

  // Addiction functions
  void add64(uint64_t n) {
    uint64_t carryFlag = 0;
    addc64(reinterpret_cast<uint64_t*>(&Data_[0]), n, &carryFlag);
    for (size_t i = 1; i < LimbsNum; i++)
      addc64(reinterpret_cast<uint64_t*>(&Data_[i]), 0, &carryFlag);
  }

  template<unsigned OperandBits>
  void add(const UInt<OperandBits> &n) {
    uint64_t carry = 0;
    size_t minSize = std::min(LimbsNum, n.LimbsNum);
    for (size_t i = 0; i < minSize; i++)
      addc64(&Data_[i], n.Data_[i], &carry);
    for (size_t i = minSize; i < LimbsNum; i++)
      addc64(&Data_[i], 0, &carry);
  }

  // Substraction functions
  void sub64(uint64_t n) {
    uint64_t borrowFlag = 0;
    subb64(reinterpret_cast<uint64_t*>(&Data_[0]), n, &borrowFlag);
    for (size_t i = 1; i < LimbsNum; i++)
      subb64(reinterpret_cast<uint64_t*>(&Data_[i]), 0, &borrowFlag);
  }

  template<unsigned OperandBits>
  void sub(const UInt<OperandBits> &n) {
    uint64_t borrow = 0;
    size_t minSize = std::min(LimbsNum, n.LimbsNum);
    for (size_t i = 0; i < minSize; i++)
      subb64(&Data_[i], n.Data_[i], &borrow);
    for (size_t i = minSize; i < LimbsNum; i++)
      subb64(&Data_[i], 0, &borrow);
  }

  // Shift functions
  //
  // Own-width shifts do not go through the runtime-length helpers below: here
  // the length is a constant, and reading through a zero-padded copy removes
  // the last branch, so the loop unrolls into a funnel shift
  void shl(unsigned shift) {
    if (shift >= Bits) {
      memset(Data_, 0, sizeof(Data_));
      return;
    }

    // A literal shift folds into a register-only shld chain; the padded copy
    // would leave a pointless store-to-load round trip in its place
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_constant_p(shift)) {
      const unsigned limbShift = shift / 64;
      const unsigned bitShift = shift % 64;
      for (unsigned i = LimbsNum; i-- > 0; ) {   // top down: a limb is never written below its source
        const uint64_t high = i >= limbShift ? Data_[i - limbShift] : 0;
        const uint64_t low = i >= limbShift + 1 ? Data_[i - limbShift - 1] : 0;
        Data_[i] = (high << bitShift) | (bitShift ? (low >> (64 - bitShift)) : 0);
      }
      return;
    }
#endif

    uint64_t padded[2*LimbsNum] = {};
    memcpy(padded + LimbsNum, Data_, sizeof(Data_));

    const unsigned limbShift = shift / 64;
    const unsigned bitShift = shift % 64;
    const unsigned invShift = (64 - bitShift) & 63;
    const uint64_t carryMask = -static_cast<uint64_t>(bitShift != 0);  // shifting by 64 is UB

    for (unsigned i = 0; i < LimbsNum; i++) {
      const uint64_t high = padded[LimbsNum + i - limbShift];
      const uint64_t low = padded[LimbsNum + i - limbShift - 1];
      Data_[i] = (high << bitShift) | ((low >> invShift) & carryMask);
    }
  }

  void shr(unsigned shift) {
    if (shift >= Bits) {
      memset(Data_, 0, sizeof(Data_));
      return;
    }

#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_constant_p(shift)) {
      const unsigned limbShift = shift / 64;
      const unsigned bitShift = shift % 64;
      for (unsigned i = 0; i < LimbsNum; i++) {   // bottom up: a limb is never written above its source
        const uint64_t low = i + limbShift < LimbsNum ? Data_[i + limbShift] : 0;
        const uint64_t high = i + limbShift + 1 < LimbsNum ? Data_[i + limbShift + 1] : 0;
        Data_[i] = (low >> bitShift) | (bitShift ? (high << (64 - bitShift)) : 0);
      }
      return;
    }
#endif

    uint64_t padded[2*LimbsNum] = {};
    memcpy(padded, Data_, sizeof(Data_));

    const unsigned limbShift = shift / 64;
    const unsigned bitShift = shift % 64;
    const unsigned invShift = (64 - bitShift) & 63;
    const uint64_t carryMask = -static_cast<uint64_t>(bitShift != 0);

    for (unsigned i = 0; i < LimbsNum; i++) {
      const uint64_t low = padded[i + limbShift];
      const uint64_t high = padded[i + limbShift + 1];
      Data_[i] = (low >> bitShift) | ((high << invShift) & carryMask);
    }
  }

  // Arithmetic shift right: the padding above the value is the sign
  // bit repeated, so a shift past the width leaves zero or all ones
  void sar(unsigned shift) {
    const uint64_t fill = -static_cast<uint64_t>(isNegative());

    if (shift >= Bits) {
      for (size_t i = 0; i < LimbsNum; i++)
        Data_[i] = fill;
      return;
    }

    uint64_t padded[2*LimbsNum];
    memcpy(padded, Data_, sizeof(Data_));
    for (size_t i = LimbsNum; i < 2*LimbsNum; i++)
      padded[i] = fill;

    const unsigned limbShift = shift / 64;
    const unsigned bitShift = shift % 64;
    const unsigned invShift = (64 - bitShift) & 63;
    const uint64_t carryMask = -static_cast<uint64_t>(bitShift != 0);

    for (unsigned i = 0; i < LimbsNum; i++) {
      const uint64_t low = padded[i + limbShift];
      const uint64_t high = padded[i + limbShift + 1];
      Data_[i] = (low >> bitShift) | ((high << invShift) & carryMask);
    }
  }

  // The shift amount as a full width value, which is how it arrives from a
  // stack or a wire; anything that does not fit an unsigned is past the width
  void shl(const UInt<Bits> &shift) { shl(shiftAmount(shift)); }
  void shr(const UInt<Bits> &shift) { shr(shiftAmount(shift)); }
  void sar(const UInt<Bits> &shift) { sar(shiftAmount(shift)); }

  // Sign extension of a byteIndex+1 bytes wide signed number to the full width.
  // From the top byte on there is nothing left to extend
  void signExtend(unsigned byteIndex) {
    if (byteIndex >= Bits / 8 - 1)
      return;

    const unsigned bit = 8 * byteIndex + 7;
    const size_t limb = bit / 64;
    const unsigned pos = bit % 64;

    const uint64_t sign = -((Data_[limb] >> pos) & 1);
    const uint64_t keep = ~uint64_t{0} >> (63 - pos);   // bit and everything below it
    Data_[limb] = (Data_[limb] & keep) | (sign & ~keep);
    for (size_t i = limb + 1; i < LimbsNum; i++)
      Data_[i] = sign;
  }

  void signExtend(const UInt<Bits> &byteIndex) { signExtend(shiftAmount(byteIndex)); }

  // Multiplication
  void mul64(uint64_t n) {
    uint64_t oldHi = 0;
    uint64_t carry = 0;
    for (size_t i = 0; i < LimbsNum; i++) {
      uint64_t hi;
      mulx64(Data_[i], n, reinterpret_cast<uint64_t*>(&Data_[i]), &hi);
      addc64(reinterpret_cast<uint64_t*>(&Data_[i]), oldHi, &carry);
      oldHi = hi;
    }
  }

  // Multiply by double with minimal precision loss
  // Extracts 53-bit mantissa, multiplies as integer, then shifts by exponent
  void mulfp(double d) {
    static const double multiplier = std::pow(FLT_RADIX, DBL_MANT_DIG);

    int exponent = 0;
    uint64_t mantissa = static_cast<uint64_t>(std::frexp(d, &exponent) * multiplier);
    exponent -= DBL_MANT_DIG;

    if (Data_[LimbsNum - 1] == 0) {
      // No overflow risk, multiply in place
      mul64(mantissa);

      if (exponent < 0)
        shr(-exponent);
      else if (exponent > 0)
        shl(exponent);
    } else {
      // Use extended precision to avoid overflow
      UInt<Bits + 64> tmp(*this);
      tmp.mul64(mantissa);

      if (exponent < 0)
        tmp.shr(-exponent);
      else if (exponent > 0)
        tmp.shl(exponent);

      *this = truncate(tmp);
    }
  }

  template<unsigned OperandBits>
  void mul(const UInt<OperandBits> &n) { *this = product(*this, n); }

  // Operand scan: one running carry per row and two adds per partial product.
  // The product scan it replaced carried a three-wide accumulator, so it paid
  // three adds for the same multiply.
  //
  // The scan builds a fresh value anyway, so handing it back lets operator*
  // skip both the copy of the left operand and the copy back
  template<unsigned OperandBits>
  static UInt<Bits> product(const UInt<Bits> &a, const UInt<OperandBits> &n) {
    UInt<Bits> result;   // zeroed by the default constructor: the rows accumulate into it
    constexpr size_t OperandLimbs = UInt<OperandBits>::LimbsNum;
    const size_t rows = std::min(OperandLimbs, LimbsNum);

    for (size_t j = 0; j < rows; j++) {
      uint64_t carry = 0;
      const size_t last = LimbsNum - j - 1;
      for (size_t i = 0; i < last; i++)
        result.Data_[i + j] = mulAdd64(a.Data_[i], n.Data_[j], result.Data_[i + j], &carry);
      // nothing is kept above the top limb, so its high half is dead
      result.Data_[LimbsNum - 1] += a.Data_[last] * n.Data_[j] + carry;
    }

    return result;
  }

  // The whole 2*Bits wide product, which is what a modular multiply needs before
  // it can reduce. Same operand scan as product() above, only nothing is dropped
  template<unsigned OperandBits>
  static UInt<Bits + OperandBits> wideProduct(const UInt<Bits> &a, const UInt<OperandBits> &n) {
    constexpr size_t OperandLimbs = UInt<OperandBits>::LimbsNum;
    UInt<Bits + OperandBits> result{typename UInt<Bits + OperandBits>::CNoInit{}};
    for (size_t i = 0; i < LimbsNum; i++)   // the upper half is filled by the carries
      result.Data_[i] = 0;

    for (size_t j = 0; j < OperandLimbs; j++) {
      uint64_t carry = 0;
      for (size_t i = 0; i < LimbsNum; i++)
        result.Data_[i + j] = mulAdd64(a.Data_[i], n.Data_[j], result.Data_[i + j], &carry);
      result.Data_[LimbsNum + j] = carry;
    }

    return result;
  }

  // (a + b) mod m, on the full width: the sum of two values of this width does
  // not fit it, so it cannot be taken modulo afterwards. A zero modulus answers
  // zero - there is no numeric answer, and a fault would make every caller guard
  static UInt<Bits> addmod(const UInt<Bits> &a, const UInt<Bits> &b, const UInt<Bits> &m) {
    if (m.isZero())
      return zero();

    // Reduced operands are the common case, and there the sum is below 2*m: one
    // conditional subtraction finishes it without any division
    if (a.less(m) && b.less(m)) {
      UInt<Bits> sum;
      uint64_t carry = 0;
      for (size_t i = 0; i < LimbsNum; i++) {
        uint64_t limb = a.Data_[i];
        addc64(&limb, b.Data_[i], &carry);
        sum.Data_[i] = limb;
      }

      UInt<Bits> reduced;
      uint64_t borrow = 0;
      for (size_t i = 0; i < LimbsNum; i++) {
        uint64_t limb = sum.Data_[i];
        subb64(&limb, m.Data_[i], &borrow);
        reduced.Data_[i] = limb;
      }

      // The sum overflowed the width or is at least m: either way take sum - m
      return (carry || !borrow) ? reduced : sum;
    }

    UInt<Bits + 64> wide(a);
    wide.add(b);
    UInt<Bits + 64> remainder{typename UInt<Bits + 64>::CNoInit{}};   // divmod writes every limb
    wide.divmod(m, nullptr, &remainder);
    return truncate(remainder);
  }

  // (a * b) mod m through the full width product, with the same zero modulus rule
  static UInt<Bits> mulmod(const UInt<Bits> &a, const UInt<Bits> &b, const UInt<Bits> &m) {
    if (m.isZero())
      return zero();

    const UInt<2*Bits> product = wideProduct(a, b);
    UInt<2*Bits> remainder{typename UInt<2*Bits>::CNoInit{}};
    product.divmod(m, nullptr, &remainder);
    return truncate(remainder);
  }

  // base^e wrapped to the width, square and multiply from the top bit down.
  // Starting from the base rather than from one saves the squaring of one and
  // the multiply by it that the leading bit would otherwise cost
  static UInt<Bits> exp(const UInt<Bits> &base, const UInt<Bits> &e) {
    // A power of two is a shift, and that is the base most exponents come with
    if (base == uint64_t{2}) {
      UInt<Bits> result;
      if (e < static_cast<uint64_t>(Bits)) {
        result = UInt<Bits>(uint64_t{1});
        result.shl(static_cast<unsigned>(e.low64()));
      }
      return result;
    }

    const unsigned significant = e.bits();
    if (significant == 0)
      return UInt<Bits>(uint64_t{1});

    UInt<Bits> result = base;
    for (unsigned i = significant - 1; i-- > 0; ) {
      result = product(result, result);
      if ((e.Data_[i / 64] >> (i % 64)) & 1)
        result = product(result, base);
    }
    return result;
  }

  // Division by 64-bit value
  // Non-const version: stores quotient in *this, returns remainder
  uint64_t divmod64(uint64_t n) {
    return ::divmodLimbs(Data_, LimbsNum, n, Data_);
  }

  // Const version: stores quotient in *quotient (if not null), returns remainder
  uint64_t divmod64(uint64_t n, UInt<Bits> *quotient = nullptr) const {
    uint64_t scratch[LimbsNum];
    return ::divmodLimbs(Data_, LimbsNum, n, quotient ? quotient->Data_ : scratch);
  }

  template<unsigned OperandBits>
  void divmod(const UInt<OperandBits> &divisor, UInt<Bits> *quotient, UInt<Bits> *remainder) const {
    constexpr size_t OperandLimbs = std::min(LimbsNum, UInt<OperandBits>::LimbsNum);

    // Top down with an early exit, not a constant-length sweep: the sweep unrolls
    // into conditional moves, but they form a dependent chain that costs more
    // than the branch it removes
    size_t divisorLimbs = 0;
    size_t dividendLimbs = 0;

    for (size_t i = OperandLimbs; i > 0; --i) {
      if (divisor.Data_[i - 1] != 0) {
        divisorLimbs = i;
        break;
      }
    }

    for (size_t i = LimbsNum; i > 0; --i) {
      if (Data_[i - 1] != 0) {
        dividendLimbs = i;
        break;
      }
    }

    if (divisorLimbs == 0) {
      // division by zero, cause OS exception
      uint64_t x = Data_[0];
      x /= divisor.Data_[0];
      return;
    }

    if (dividendLimbs == 0) {
      // dividend is zero, so quotient and remainder also 0
      if (quotient)
        *quotient  = UInt<Bits>::zero();
      if (remainder)
        *remainder = UInt<Bits>::zero();
      return;
    }

    // A single-limb divisor is the common case (powers of ten, powers of two,
    // small constants) and Knuth D is at its worst there: it emits one quotient
    // digit per dividend limb, where the short division does the whole thing in
    // one pass
    if (divisorLimbs == 1) {
      const uint64_t rem = divmod64(divisor.Data_[0], quotient);
      if (remainder) {
        remainder->Data_[0] = rem;
        for (size_t i = 1; i < LimbsNum; i++)
          remainder->Data_[i] = 0;
      }
      return;
    }

    // Quick exit if U < V
    if (dividendLimbs < divisorLimbs) {
      if (quotient)
        *quotient  = UInt<Bits>::zero();
      if (remainder)
        *remainder = *this;
      return;
    }

    // -------- Knuth D (base 2^64) --------
    const size_t m = dividendLimbs - divisorLimbs;

    // Normalization: shift so that MSB of v[n-1] becomes 1. Folded into the copy
    // rather than done as a second pass, and without branching on s: the shift is
    // under 64 bits by construction, and s is zero for exactly half of all
    // divisors, so a branch here would mispredict on every other division
    const unsigned s = clz64(divisor.Data_[divisorLimbs - 1]);
    const unsigned invS = (64 - s) & 63;
    const uint64_t sMask = -static_cast<uint64_t>(s != 0);   // shifting by 64 is UB

    // Shifted over the full width rather than over the significant limbs: the
    // limbs above are zero, so they shift to zero, and a constant trip count
    // unrolls where a runtime one does not. The same reason the digits are
    // cleared wholesale below instead of only past the top one.
    uint64_t u[LimbsNum + 1];   // normalized dividend + extra limb
    uint64_t v[LimbsNum];       // normalized divisor

    u[LimbsNum] = (Data_[LimbsNum - 1] >> invS) & sMask;
    for (size_t i = LimbsNum; i-- > 1; )
      u[i] = (Data_[i] << s) | ((Data_[i - 1] >> invS) & sMask);
    u[0] = Data_[0] << s;

    for (size_t i = OperandLimbs; i-- > 1; )
      v[i] = (divisor.Data_[i] << s) | ((divisor.Data_[i - 1] >> invS) & sMask);
    v[0] = divisor.Data_[0] << s;

    // Digits go straight into the caller's quotient: both operands have been
    // read out by now, so an output aliasing either of them is safe. The digits
    // are cleared over the full width on purpose: clearing only what the digit
    // loop leaves behind is a runtime length, and that costs more than it saves
    uint64_t scratch[LimbsNum];
    uint64_t *qd = quotient ? quotient->Data_ : scratch;
    for (size_t i = 0; i < LimbsNum; i++)
      qd[i] = 0;

    // Two ways to get a trial quotient, and the reciprocal only pays for itself
    // once there are enough digits to spread its setup over - see the threshold
    if constexpr (LimbsNum >= ReciprocalDigits + 1) {
      if (m + 1 >= ReciprocalDigits)
        knuthReciprocal(u, v, qd, m, divisorLimbs);
      else
        knuthDivide(u, v, qd, m, divisorLimbs);
    } else {
      knuthDivide(u, v, qd, m, divisorLimbs);
    }

    // Denormalization of remainder: r = u[0..n-1] >> s, written in place of a
    // second copy
    // Here the runtime length wins: the full-width form needs a select per limb
    // to mask off what the digit loop left above the divisor
    if (remainder) {
      for (size_t i = 0; i + 1 < divisorLimbs; i++)
        remainder->Data_[i] = (u[i] >> s) | ((u[i + 1] << invS) & sMask);
      remainder->Data_[divisorLimbs - 1] = u[divisorLimbs - 1] >> s;
      for (size_t i = divisorLimbs; i < LimbsNum; i++)
        remainder->Data_[i] = 0;
    }
  }

  // Two's complement division, truncated towards zero: the remainder takes the
  // sign of the dividend. As in divmod above, a zero divisor is the caller's to
  // rule out
  void sdivmod(const UInt<Bits> &divisor, UInt<Bits> *quotient, UInt<Bits> *remainder) const {
    const bool dividendNegative = isNegative();
    const bool divisorNegative = divisor.isNegative();

    const UInt<Bits> u = dividendNegative ? -*this : *this;
    const UInt<Bits> v = divisorNegative ? -divisor : divisor;

    UInt<Bits> q, r;
    u.divmod(v, quotient ? &q : nullptr, remainder ? &r : nullptr);

    if (quotient)
      *quotient = dividendNegative != divisorNegative ? -q : q;
    if (remainder)
      *remainder = dividendNegative ? -r : r;
  }

  // operator overload
  template<unsigned OperandBits> friend class UInt;

  // comparision
  bool operator==(uint64_t n) const { return cmp64(n) == 0; }
  bool operator!=(uint64_t n) const { return cmp64(n) != 0; }
  bool operator<(uint64_t n) const { return cmp64(n) < 0; }
  bool operator<=(uint64_t n) const { return cmp64(n) <= 0; }
  bool operator>(uint64_t n) const { return cmp64(n) > 0; }
  bool operator>=(uint64_t n) const { return cmp64(n) >= 0; }

  template<unsigned OperandBits>
    bool operator==(const UInt<OperandBits> &n) const {
      if constexpr (OperandBits == Bits)
        return equal(n);
      else
        return cmp(n) == 0;
    }
  template<unsigned OperandBits>
    bool operator!=(const UInt<OperandBits> &n) const { return !(*this == n); }
  template<unsigned OperandBits>
    bool operator<(const UInt<OperandBits> &n) const {
      if constexpr (OperandBits == Bits)
        return less(n);
      else
        return cmp(n) < 0;
    }
  template<unsigned OperandBits>
    bool operator<=(const UInt<OperandBits> &n) const { return cmp(n) <= 0; }
  template<unsigned OperandBits>
    bool operator>(const UInt<OperandBits> &n) const { return cmp(n) > 0; }
  template<unsigned OperandBits>
    bool operator>=(const UInt<OperandBits> &n) const { return cmp(n) >= 0; }

  // addition
  // The compound operators hand back a reference: returning a copy makes a
  // chained assignment silently update the temporary instead of the object
  UInt<Bits>& operator+=(uint64_t n) { add64(n); return *this; }
  template<unsigned OperandBits>
    UInt<Bits>& operator+=(const UInt<OperandBits> &n) { add(n); return *this; }
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
    UInt<Bits> friend operator+(const UInt<Bits> &a, T b) { return UInt<Bits>(a) += b; }
  // Built in one pass rather than copy-then-mutate: with the copy in the way the
  // compiler spills the left operand to the stack instead of keeping it in registers
  template<unsigned OperandBits>
    friend UInt<Bits> operator+(const UInt<Bits> &a, const UInt<OperandBits> &b) {
      if constexpr (OperandBits != Bits) {
        return UInt<Bits>(a) += b;
      } else {
        UInt<Bits> result{CNoInit{}};
        uint64_t carry = 0;
#ifdef __clang__
        for (size_t i = 0; i < LimbsNum; i++) {
          uint64_t limb = a.Data_[i];
          addc64(&limb, b.Data_[i], &carry);
          result.Data_[i] = limb;
        }
#else
        // Carry by hand: gcc spills the flag to a register for addc64, three
        // extra instructions per limb. clang vectorizes this form instead, and
        // emulating the carry with vector compares costs it twice as much
        for (size_t i = 0; i < LimbsNum; i++) {
          const uint64_t sum = a.Data_[i] + b.Data_[i];
          const uint64_t total = sum + carry;
          carry = (sum < a.Data_[i]) | (total < sum);
          result.Data_[i] = total;
        }
#endif
        return result;
      }
    }

  // substraction
  UInt<Bits>& operator-=(uint64_t n) { sub64(n); return *this; }
  template<unsigned OperandBits>
    UInt<Bits>& operator-=(const UInt<OperandBits> &n) { sub(n); return *this; }
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
    UInt<Bits> friend operator-(const UInt<Bits> &a, T b) { return UInt<Bits>(a) -= b; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator-(const UInt<Bits> &a, const UInt<OperandBits> &b) {
      if constexpr (OperandBits != Bits) {
        return UInt<Bits>(a) -= b;
      } else {
        UInt<Bits> result{CNoInit{}};
        uint64_t borrow = 0;
#ifdef __clang__
        for (size_t i = 0; i < LimbsNum; i++) {
          uint64_t limb = a.Data_[i];
          subb64(&limb, b.Data_[i], &borrow);
          result.Data_[i] = limb;
        }
#else
        // Borrow by hand, for the same reason as the carry in operator+ above
        for (size_t i = 0; i < LimbsNum; i++) {
          const uint64_t difference = a.Data_[i] - b.Data_[i];
          const uint64_t total = difference - borrow;
          borrow = (difference > a.Data_[i]) | (total > difference);
          result.Data_[i] = total;
        }
#endif
        return result;
      }
    }

  // shift
  UInt<Bits>& operator<<=(unsigned n) { shl(n); return *this; }
  UInt<Bits>& operator>>=(unsigned n) { shr(n); return *this; }
  UInt<Bits> friend operator<<(const UInt<Bits> &a, unsigned n) { return UInt<Bits>(a) <<= n; }
  UInt<Bits> friend operator>>(const UInt<Bits> &a, unsigned n) { return UInt<Bits>(a) >>= n; }

  // multiplication short
  UInt<Bits>& operator*=(uint64_t n) { mul64(n); return *this; }
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
    UInt<Bits> friend operator*(const UInt<Bits> &a, T b) { return UInt<Bits>(a) *= b; }

  // multiplication long
  template<unsigned OperandBits>
    UInt<Bits>& operator*=(const UInt<OperandBits> &n) { mul(n); return *this; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator*(const UInt<Bits> &a, const UInt<OperandBits> &b) { return product(a, b); }

  // division short
  UInt<Bits>& operator/=(uint64_t n) { divmod64(n); return *this; }
  template<typename T, std::enable_if_t<std::is_integral_v<T> && std::is_unsigned_v<T>, int> = 0>
    UInt<Bits> friend operator/(const UInt<Bits> &a, T b) { return UInt<Bits>(a) /= b; }

  // modulo short
  uint64_t operator%(uint64_t n) const { return divmod64(n); }


  // division long
  template<unsigned OperandBits>
  UInt<Bits>& operator/=(const UInt<OperandBits> &n) {
    divmod(n, this, nullptr);
    return *this;
  }
  template<unsigned OperandBits>
  friend UInt<Bits> operator/(const UInt<Bits> &a, const UInt<OperandBits> &b) {
    UInt<Bits> quotient;
    a.divmod(b, &quotient, nullptr);
    return quotient;
  }

  // modulo long
  template<unsigned OperandBits>
  UInt<Bits>& operator%=(const UInt<OperandBits> &n) {
    divmod(n, nullptr, this);
    return *this;
  }
  template<unsigned OperandBits>
  friend UInt<Bits> operator%(const UInt<Bits> &a, const UInt<OperandBits> &b) {
    UInt<Bits> remainder;
    a.divmod(b, nullptr, &remainder);
    return remainder;
  }

  // bitwise
  // A narrower operand is read as zero extended, the same way add and sub read it
  template<unsigned OperandBits>
  void and_(const UInt<OperandBits> &n) {
    constexpr size_t minSize = std::min(LimbsNum, UInt<OperandBits>::LimbsNum);
    for (size_t i = 0; i < minSize; i++)
      Data_[i] &= n.Data_[i];
    for (size_t i = minSize; i < LimbsNum; i++)
      Data_[i] = 0;
  }

  template<unsigned OperandBits>
  void or_(const UInt<OperandBits> &n) {
    constexpr size_t minSize = std::min(LimbsNum, UInt<OperandBits>::LimbsNum);
    for (size_t i = 0; i < minSize; i++)
      Data_[i] |= n.Data_[i];
  }

  template<unsigned OperandBits>
  void xor_(const UInt<OperandBits> &n) {
    constexpr size_t minSize = std::min(LimbsNum, UInt<OperandBits>::LimbsNum);
    for (size_t i = 0; i < minSize; i++)
      Data_[i] ^= n.Data_[i];
  }

  // bitwise not
  void not_() {
    for (size_t i = 0; i < LimbsNum; ++i)
      Data_[i] = ~Data_[i];
  }

  UInt<Bits> operator~() const {
    UInt<Bits> result{CNoInit{}};   // every limb is written right below
    for (size_t i = 0; i < LimbsNum; i++)
      result.Data_[i] = ~Data_[i];
    return result;
  }

  // Two's complement negation (in-place)
  void negate() {
    not_();
    add64(1u);
  }

  // Unary minus
  UInt<Bits> operator-() const {
    UInt<Bits> result(*this);
    result.negate();
    return result;
  }

  // bitwise binary
  template<unsigned OperandBits>
    UInt<Bits>& operator&=(const UInt<OperandBits> &n) { and_(n); return *this; }
  template<unsigned OperandBits>
    UInt<Bits>& operator|=(const UInt<OperandBits> &n) { or_(n); return *this; }
  template<unsigned OperandBits>
    UInt<Bits>& operator^=(const UInt<OperandBits> &n) { xor_(n); return *this; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator&(const UInt<Bits> &a, const UInt<OperandBits> &b) { return UInt<Bits>(a) &= b; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator|(const UInt<Bits> &a, const UInt<OperandBits> &b) { return UInt<Bits>(a) |= b; }
  template<unsigned OperandBits>
    friend UInt<Bits> operator^(const UInt<Bits> &a, const UInt<OperandBits> &b) { return UInt<Bits>(a) ^= b; }

  // Prevent operations with signed integers and floating point types
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator==(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator!=(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator<(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator<=(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator>(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  bool operator>=(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt<Bits> operator+=(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt<Bits> operator-=(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt<Bits> operator*=(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  UInt<Bits> operator/=(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  uint64_t operator%(T) const = delete;

  // Prevent member functions with signed/floating point arguments
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  int cmp64(T) const = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  void add64(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  void sub64(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  void mul64(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  uint64_t divmod64(T) = delete;
  template<typename T, std::enable_if_t<std::is_signed_v<T> || std::is_floating_point_v<T>, int> = 0>
  uint64_t divmod64(T, UInt<Bits>*) const = delete;

private:
  // A result that is filled in whole right away has no use for the zeroing
  struct CNoInit {};
  explicit UInt(CNoInit) {}

  static constexpr size_t LimbSize = 8 * sizeof(uint64_t);
  static constexpr size_t LimbsNum = Bits / 8 / sizeof(uint64_t);
  static_assert(LimbsNum >= 2);

  // How many quotient digits it takes before a precomputed reciprocal beats the
  // estimate it replaces. With a hardware divide 256 bits never reaches it;
  // without one the estimate is dearer, so the threshold drops
#ifdef LIBPOW_HAS_DIV128
  static constexpr size_t ReciprocalDigits = 4;
#else
  static constexpr size_t ReciprocalDigits = 3;
#endif

  // The largest power of ten a limb holds, and how many digits that is
  static constexpr uint64_t DecimalChunk = 10000000000000000000ull;
  static constexpr unsigned DecimalChunkDigits = 19;

private:
  // A bitmap over the 55 characters from '0' to 'f', so the answer takes one
  // shift and no branch. The chain of range comparisons this replaces
  // mispredicted on every other character of the scan in setHex: hex strings
  // come with digits and letters mixed at random
  static bool isHexDigit(char c) {
    constexpr uint64_t validMask = 0x3FFull | (0x3Full << ('A' - '0')) | (0x3Full << ('a' - '0'));
    const unsigned value = static_cast<unsigned>(static_cast<unsigned char>(c)) - '0';
    return ((validMask >> (value & 63)) & 1 & static_cast<uint64_t>(value < 64)) != 0;
  }

  // A shift or byte index given at full width, capped at the width: everything
  // at or above it means "past the end", which the callers handle already
  static unsigned shiftAmount(const UInt<Bits> &n) {
    return n.cmp64(Bits) >= 0 ? Bits : static_cast<unsigned>(n.Data_[0]);
  }

  static uint8_t decodeHex(char c) {
    const unsigned value = static_cast<unsigned char>(c);
    return static_cast<uint8_t>((value & 0xF) + ((value >> 6) & 1) * 9);   // letters carry bit 6, digits do not
  }

  uint32_t halfLimb(size_t index) const {
    return static_cast<uint32_t>(Data_[index / 2] >> (32 * (index & 1)));
  }

  // Eight nibbles of a half-limb spread into eight bytes, most significant
  // first, then turned into characters: the group becomes one store
  static uint64_t hexOctet(uint32_t value, bool upperCase) {
    uint64_t x = value;
    x = ((x << 16) | x) & 0x0000FFFF0000FFFFull;
    x = ((x << 8) | x) & 0x00FF00FF00FF00FFull;
    x = ((x << 4) | x) & 0x0F0F0F0F0F0F0F0Full;

    // Adding six sets bit 4 of a byte exactly when its nibble is above nine
    const uint64_t overNine = ((x + 0x0606060606060606ull) >> 4) & 0x0101010101010101ull;
    return x + 0x3030303030303030ull + overNine * (upperCase ? 'A' - '0' - 10 : 'a' - '0' - 10);
  }

  // The inverse: eight characters folded back into eight nibbles
  static uint32_t parseHexOctet(const char *p) {
    uint64_t chars;
    memcpy(&chars, p, sizeof(chars));
    chars = readbe(chars);

    uint64_t x = (chars & 0x0F0F0F0F0F0F0F0Full) + ((chars >> 6) & 0x0101010101010101ull) * 9;
    x = (x | (x >> 4)) & 0x00FF00FF00FF00FFull;
    x = (x | (x >> 8)) & 0x0000FFFF0000FFFFull;
    x = (x | (x >> 16)) & 0x00000000FFFFFFFFull;
    return static_cast<uint32_t>(x);
  }

  // A chunk of the decimal conversion, zero padded and written backwards from
  // end. Two digits per divide, so the dependent chain is half as long
  static void emitDecimalChunk(char *end, uint64_t value) {
    for (unsigned i = 0; i < DecimalChunkDigits / 2; i++) {
      const uint64_t next = value / 100;
      end -= 2;
      memcpy(end, &DecimalPairs[2 * static_cast<size_t>(value - next * 100)], 2);
      value = next;
    }
    end[-1] = static_cast<char>('0' + value);
  }

  // Knuth D, quotient digits from a hardware 128/64 divide plus the D3
  // correction loop. u is the normalized dividend, v the normalized divisor.
  static void knuthDivide(uint64_t *u, const uint64_t *v, uint64_t *qd, size_t m, size_t divisorLimbs) {
#ifndef LIBPOW_HAS_DIV128
    // Without a hardware divide the estimate would be a call into libgcc. One
    // reciprocal for the pass replaces it, and the divisor is already normalized
    const uint64_t recip = reciprocal2by1(v[divisorLimbs - 1]);
#endif
    for (size_t jj = m + 1; jj-- > 0; ) {
      const size_t j = jj;

      uint64_t qhat = 0;
      uint64_t rhat = 0;
      bool rhatOverflowed = false;

      const uint64_t ujn  = u[j + divisorLimbs];
      const uint64_t ujn1 = u[j + divisorLimbs - 1];

      // Estimate qhat from two most significant words
      // (protection against case ujn == v[n-1], where true division would give 2^64)
      if (ujn >= v[divisorLimbs - 1]) {
        qhat = std::numeric_limits<uint64_t>::max(); // B-1
        rhat = ujn1;
        uint64_t c = 0;
        addc64(&rhat, v[divisorLimbs - 1], &c);
        rhatOverflowed = (c != 0); // rhat >= B => correction check not needed
      } else {
#ifdef LIBPOW_HAS_DIV128
        ::divmod64(ujn1, ujn, v[divisorLimbs - 1], &qhat, &rhat);
#else
        qhat = divrem2by1(ujn, ujn1, v[divisorLimbs - 1], recip, &rhat);
#endif
      }

      // Correction of qhat (Knuth D3)
      if (!rhatOverflowed) {
        const uint64_t ujn2 = u[j + divisorLimbs - 2];
        for (;;) {
          uint64_t pLo, pHi;
          mulx64(qhat, v[divisorLimbs - 2], &pLo, &pHi);

          // if qhat*v[n-2] <= rhat*B + ujn2, then ok
          if (pHi < rhat)
            break;
          if (pHi == rhat && pLo <= ujn2)
            break;

          qhat--;

          uint64_t c = 0;
          addc64(&rhat, v[divisorLimbs - 1], &c);
          if (c)
            break;   // rhat >= B: no further correction can be required
        }
      }

      // D4: u[j..j+n] -= qhat * v[0..n-1]
      uint64_t borrow = 0;
      subb64(&u[j + divisorLimbs], submul(&u[j], v, divisorLimbs, qhat), &borrow);

      // D6: if went negative - rollback (add divisor back) and decrease qhat
      if (borrow) {
        qhat--;
        u[j + divisorLimbs] += addTo(&u[j], v, divisorLimbs);
      }

      qd[j] = qhat;
    }
  }

  // The same loop with the trial quotient off a precomputed reciprocal of the
  // divisor's top two limbs. The estimate is exact for that 3-by-2 division, so
  // Knuth's D3 loop disappears and only the rare D6 add-back is left.
  static void knuthReciprocal(uint64_t *u, const uint64_t *v, uint64_t *qd, size_t m, size_t divisorLimbs) {
    const uint64_t d1 = v[divisorLimbs - 1];
    const uint64_t d0 = v[divisorLimbs - 2];
    const uint64_t recip = reciprocal3by2(d1, d0);

    for (size_t jj = m + 1; jj-- > 0; ) {
      const size_t j = jj;

      const uint64_t u2 = u[j + divisorLimbs];
      const uint64_t u1 = u[j + divisorLimbs - 1];
      const uint64_t u0 = u[j + divisorLimbs - 2];

      uint64_t qhat;
      if (u2 == d1 && u1 == d0) {
        // the 3-by-2 quotient would be 2^64: the digit is B-1 and D4 has to run
        // over the whole divisor
        qhat = std::numeric_limits<uint64_t>::max();
        u[j + divisorLimbs] = u2 - submul(&u[j], v, divisorLimbs, qhat);
      } else {
        uint64_t r1, r0;
        qhat = divrem3by2(u2, u1, u0, d1, d0, recip, &r1, &r0);

        // D4 only touches the limbs below the top pair: for the pair itself the
        // 3-by-2 remainder is already the answer
        const uint64_t overflow = submul(&u[j], v, divisorLimbs - 2, qhat);
        uint64_t borrow = 0;
        subb64(&r0, overflow, &borrow);
        subb64(&r1, 0, &borrow);
        u[j + divisorLimbs - 2] = r0;
        u[j + divisorLimbs - 1] = r1;

        // D6: went negative - add the divisor back. The carry out of the top
        // cancels the borrow, so nothing above the pair needs touching
        if (borrow) {
          qhat--;
          u[j + divisorLimbs - 1] += d1 + addTo(&u[j], v, divisorLimbs - 1);
        }
      }

      qd[j] = qhat;
    }
  }

  // x[0..len) -= multiplier * y[0..len). What comes back is the borrow out of
  // the top, a whole limb rather than a bit, since the product is twice as wide
  static uint64_t submul(uint64_t *x, const uint64_t *y, size_t len, uint64_t multiplier) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < len; i++) {
      const uint64_t s = x[i] - borrow;
      uint64_t pLo, pHi;
      mulx64(y[i], multiplier, &pLo, &pHi);
      borrow = pHi + (x[i] < s);
      x[i] = s - pLo;
      borrow += (s < x[i]);
    }
    return borrow;
  }

  // x[0..len) += y[0..len), returns the carry out
  static uint64_t addTo(uint64_t *x, const uint64_t *y, size_t len) {
    uint64_t carry = 0;
    for (size_t i = 0; i < len; i++)
      addc64(&x[i], y[i], &carry);
    return carry;
  }

  // Compare two arrays of limbs, returns -1 if left < right, 0 if equal, 1 if left > right
  static int cmp(const uint64_t *left, size_t leftLen, const uint64_t *right, size_t rightLen) {
    // Check extra limbs of the longer operand
    if (leftLen > rightLen) {
      for (size_t i = leftLen; i > rightLen; --i) {
        if (left[i - 1] != 0)
          return 1;
      }
    } else if (rightLen > leftLen) {
      for (size_t i = rightLen; i > leftLen; --i) {
        if (right[i - 1] != 0)
          return -1;
      }
    }

    // Compare common limbs from most significant to least significant
    size_t minLen = std::min(leftLen, rightLen);
    for (size_t i = minLen; i > 0; --i) {
      if (left[i - 1] < right[i - 1])
        return -1;
      if (left[i - 1] > right[i - 1])
        return 1;
    }

    return 0;
  }

  // Shift array of limbs left by arbitrary number of bits
  static void shl(uint64_t *data, size_t len, unsigned shift) {
    if (shift == 0 || len == 0)
      return;

    const size_t limbShift = shift / 64;
    const unsigned bitShift = shift % 64;

    if (limbShift >= len) {
      memset(data, 0, len * sizeof(uint64_t));
      return;
    }

    const size_t remaining = len - limbShift;

    if (bitShift == 0) {
      // Shift by whole limbs only
      memmove(data + limbShift, data, remaining * sizeof(uint64_t));
    } else {
      // Shift by limbs + bits
      const unsigned invBitShift = 64 - bitShift;
      for (size_t i = len; i-- > limbShift + 1; )
        data[i] = (data[i - limbShift] << bitShift) | (data[i - limbShift - 1] >> invBitShift);
      data[limbShift] = data[0] << bitShift;
    }

    // Zero out lower limbs
    memset(data, 0, limbShift * sizeof(uint64_t));
  }

  // Shift array of limbs right by arbitrary number of bits
  static void shr(uint64_t *data, size_t len, unsigned shift) {
    if (shift == 0 || len == 0)
      return;

    const size_t limbShift = shift / 64;
    const unsigned bitShift = shift % 64;

    if (limbShift >= len) {
      memset(data, 0, len * sizeof(uint64_t));
      return;
    }

    const size_t remaining = len - limbShift;

    if (bitShift == 0) {
      // Shift by whole limbs only
      memmove(data, data + limbShift, remaining * sizeof(uint64_t));
    } else {
      // Shift by limbs + bits
      const unsigned invBitShift = 64 - bitShift;
      for (size_t i = 0; i < remaining - 1; i++)
        data[i] = (data[i + limbShift] >> bitShift) | (data[i + limbShift + 1] << invBitShift);
      data[remaining - 1] = data[len - 1] >> bitShift;
    }

    // Zero out upper limbs
    memset(data + remaining, 0, limbShift * sizeof(uint64_t));
  }

private:
  uint64_t Data_[LimbsNum];
};

// Compact format functions for UInt<256> (Bitcoin difficulty target encoding)
static inline UInt<256> uint256Compact(uint32_t nCompact, bool *pfNegative = nullptr, bool *pfOverflow = nullptr)
{
  UInt<256> result;
  int nSize = nCompact >> 24;
  uint32_t nWord = nCompact & 0x007fffff;

  if (nSize <= 3) {
    nWord >>= 8 * (3 - nSize);
    result = nWord;
  } else {
    result = nWord;
    result <<= 8 * (nSize - 3);
  }

  if (pfNegative)
    *pfNegative = nWord != 0 && (nCompact & 0x00800000) != 0;
  if (pfOverflow)
    *pfOverflow = nWord != 0 && ((nSize > 34) ||
                                 (nWord > 0xff && nSize > 33) ||
                                 (nWord > 0xffff && nSize > 32));
  return result;
}

static inline uint32_t uint256GetCompact(const UInt<256> &value, bool fNegative = false)
{
  int nSize = (value.bits() + 7) / 8;
  uint32_t nCompact = 0;

  if (nSize <= 3) {
    nCompact = static_cast<uint32_t>(value.low64() << 8 * (3 - nSize));
  } else {
    UInt<256> bn = value >> 8 * (nSize - 3);
    nCompact = static_cast<uint32_t>(bn.low64());
  }

  // The 0x00800000 bit denotes the sign.
  // Thus, if it is already set, divide the mantissa by 256 and increase the exponent.
  if (nCompact & 0x00800000) {
    nCompact >>= 8;
    nSize++;
  }

  assert((nCompact & ~0x007fffffU) == 0);
  assert(nSize < 256);
  nCompact |= nSize << 24;
  nCompact |= (fNegative && (nCompact & 0x007fffff) ? 0x00800000 : 0);
  return nCompact;
}
