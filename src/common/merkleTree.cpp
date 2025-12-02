// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "merkleTree.h"
#include "crypto/sha256.h"

uint256 calculateMerkleRoot(uint256 *hashes, size_t size)
{
  if (size) {
    size_t txNum = size;
    CCtxSha256 sha256;
    while (txNum > 1) {
      size_t iterNum = (txNum / 2) + (txNum % 2);
      for (size_t i = 0; i < iterNum; i++) {
        sha256Init(&sha256);
        sha256Update(&sha256, hashes[i*2].begin(), sizeof(uint256));
        sha256Update(&sha256, hashes[i*2+1 < txNum ? i*2+1 : i*2].begin(), sizeof(uint256));
        sha256Final(&sha256, hashes[i].begin());

        sha256Init(&sha256);
        sha256Update(&sha256, hashes[i].begin(), sizeof(uint256));
        sha256Final(&sha256, hashes[i].begin());
      }

      txNum = iterNum;
    }

    return hashes[0];
  } else {
    uint256 zero;
    zero.SetNull();
    return zero;
  }
}

uint256 calculateMerkleRoot(uint256 hash, uint256 *tree, size_t treeSize, size_t index)
{
  uint256 result = hash;
  if (!treeSize)
    return result;

  CCtxSha256 sha256;
  for (size_t i = 0; i < treeSize; i++) {
    if (index & 1) {
      sha256Init(&sha256);
      sha256Update(&sha256, tree[i].begin(), sizeof(uint256));
      sha256Update(&sha256, result.begin(), sizeof(uint256));
      sha256Final(&sha256, result.begin());

      sha256Init(&sha256);
      sha256Update(&sha256, result.begin(), sizeof(uint256));
      sha256Final(&sha256, result.begin());
    } else {
      sha256Init(&sha256);
      sha256Update(&sha256, result.begin(), sizeof(uint256));
      sha256Update(&sha256, tree[i].begin(), sizeof(uint256));
      sha256Final(&sha256, result.begin());

      sha256Init(&sha256);
      sha256Update(&sha256, result.begin(), sizeof(uint256));
      sha256Final(&sha256, result.begin());
    }

    index >>= 1;
  }

  return result;
}
