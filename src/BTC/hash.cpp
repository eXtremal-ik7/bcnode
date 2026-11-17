// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "hash.h"
#include "libpow/ripemd160.h"

namespace BTC {

BaseBlob<160> sha256FollowRipemd160(const void *data, size_t size)
{
  uint8_t hash[32];
  sha256(data, size, hash);
  BaseBlob<160> result;
  ripemd160(hash, sizeof(hash), result.begin());
  return result;
}

}
