// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/proto.h"

static inline void genesis_block_hash_assert_eq(const BC::Proto::BlockHeader &header, const char *targetHash)
{
  uint256 hash;
  hash.SetHex(targetHash);
  if (header.GetHash() != hash) {
    LOG_F(ERROR, "Genesis block hash mismatch, expected: %s, got %s", targetHash, header.GetHash().ToString().c_str());
    abort();
  }
} 
