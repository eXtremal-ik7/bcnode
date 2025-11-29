// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/uint256.h"
#include "loguru.hpp"

template<typename X>
static inline void genesis_block_hash_assert_eq(const typename X::Proto::BlockHeader &header, const char *targetHash)
{
  uint256 hash;
  hash.SetHex(targetHash);
  if (header.GetHash() != hash) {
    LOG_F(ERROR, "Genesis block hash mismatch, expected: %s, got %s", targetHash, header.GetHash().ToString().c_str());
    abort();
  }
} 

static inline char bin2hexLowerCaseDigit(uint8_t b)
{
  return b < 10 ? '0'+b : 'a'+b-10;
}

static inline uint8_t hexDigit2bin(char c)
{
  uint8_t digit = c - '0';
  if (digit >= 10)
    digit -= ('A' - '0' - 10);
  if (digit >= 16)
    digit -= ('a' - 'A');
  return digit;
}

static inline void hex2bin(const char *in, size_t inSize, void *out)
{
  uint8_t *pOut = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < inSize/2; i++)
    pOut[i] = (hexDigit2bin(in[i*2]) << 4) | hexDigit2bin(in[i*2+1]);
}

static inline void bin2hexLowerCase(const void *in, char *out, size_t size)
{
  const uint8_t *pIn = static_cast<const uint8_t*>(in);
  for (size_t i = 0, ie = size; i != ie; ++i) {
    out[i*2] = bin2hexLowerCaseDigit(pIn[i] >> 4);
    out[i*2+1] = bin2hexLowerCaseDigit(pIn[i] & 0xF);
  }
}

static inline std::string bin2hexLowerCase(const void *in, size_t size)
{
  std::string result;
  result.resize(size * 2);
  bin2hexLowerCase(in, result.data(), size);
  return result;
}
