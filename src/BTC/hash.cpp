// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "hash.h"

#include <openssl/evp.h>

namespace BTC {

BaseBlob<160> sha256FollowRipemd160(const void *data, size_t size)
{
  uint8_t hash[32];
  {
    CCtxSha256 ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, data, size);
    sha256Final(&ctx, hash);
  }

  BaseBlob<160> result;
  {
    unsigned outSize = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_ripemd160(), nullptr);
    EVP_DigestUpdate(ctx, hash, sizeof(hash));
    EVP_DigestFinal_ex(ctx, result.begin(), &outSize);
    EVP_MD_CTX_free(ctx);
  }

  return result;
}

}
