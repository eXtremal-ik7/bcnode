// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/baseBlob.h"
#include "common/uint.h"
#include "crypto/sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace BTC {

// SHA256(SHA256(x)) - hash used by the whole bitcoin family for block headers,
// transaction ids, merkle trees and base58 checksums

static inline BaseBlob<256> sha256d(const void *data, size_t size)
{
  BaseBlob<256> result;
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, data, size);
  sha256Final(&ctx, result.begin());
  sha256Init(&ctx);
  sha256Update(&ctx, result.begin(), result.size());
  sha256Final(&ctx, result.begin());
  return result;
}

// Same for a message split in two parts (fixed size header prefix + serialized tail,
// pair of merkle tree nodes, witness merkle root + nonce)
static inline BaseBlob<256> sha256d(const void *first, size_t firstSize, const void *second, size_t secondSize)
{
  BaseBlob<256> result;
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, first, firstSize);
  sha256Update(&ctx, second, secondSize);
  sha256Final(&ctx, result.begin());
  sha256Init(&ctx);
  sha256Update(&ctx, result.begin(), result.size());
  sha256Final(&ctx, result.begin());
  return result;
}

static inline BaseBlob<256> sha256d(const BaseBlob<256> &first, const BaseBlob<256> &second)
{
  return sha256d(first.begin(), first.size(), second.begin(), second.size());
}

// Double sha256 as a little endian 256-bit number (proof of work comparisons)
static inline UInt<256> sha256dInt(const void *data, size_t size)
{
  UInt<256> result;
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, data, size);
  sha256Final(&ctx, result.rawData());
  sha256Init(&ctx);
  sha256Update(&ctx, result.rawData(), result.rawSize());
  sha256Final(&ctx, result.rawData());
  for (size_t i = 0, ie = result.rawSize() / sizeof(uint64_t); i < ie; i++)
    result.data()[i] = readle(result.data()[i]);
  return result;
}

// First 4 bytes of double sha256 in wire order (base58check checksum)
static inline uint32_t sha256dChecksum(const void *data, size_t size)
{
  BaseBlob<256> hash = sha256d(data, size);
  uint32_t checksum;
  memcpy(&checksum, hash.begin(), sizeof(checksum));
  return checksum;
}

// RIPEMD160(SHA256(x)) - address from a public key or a redeem script.
// Defined in hash.cpp, ripemd160 comes from openssl
BaseBlob<160> sha256FollowRipemd160(const void *data, size_t size);

}
