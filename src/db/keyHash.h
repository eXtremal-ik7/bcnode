// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#endif

// The one place where a key becomes a table position. A txid is already a cryptographic hash:
// its words are uniform in every bit, so nothing here mixes them - it only folds vout in with
// odd multipliers and hands the words out. A table keyed by a txid alone needs none of this and
// takes a word of it directly (get64)
struct COutpointHash {
  uint64_t H1;   // low bits: index of a masked table, tag byte; high bits: fastrange
  uint64_t H2;   // second choice of a set-associative table
};

inline COutpointHash hashOutpoint(const void *txid, uint32_t vout)
{
  uint64_t a, b;
  memcpy(&a, txid, sizeof(a));
  memcpy(&b, static_cast<const uint8_t*>(txid) + sizeof(a), sizeof(b));
  const uint64_t v = static_cast<uint64_t>(vout) + 1;
  return {a ^ (0x9E3779B97F4A7C15ull * v), b ^ (0xC2B2AE3D27D4EB4Full * v)};
}

// Position in a table of n, from the HIGH bits, so a masked table below keeps the low ones.
// Two levels must not consume the same bits: sharding by "hash % shards" over a power of two
// count leaves every key of a shard with the same low bits, and the map inside the shard - which
// indexes by the low bits - loses log2(shards) of them
inline size_t fastrange(uint64_t h, size_t n)
{
#ifdef _MSC_VER
  unsigned __int64 hi;
  _umul128(h, n, &hi);
  return static_cast<size_t>(hi);
#else
  return static_cast<size_t>((static_cast<unsigned __int128>(h) * n) >> 64);
#endif
}
