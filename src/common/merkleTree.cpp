// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "merkleTree.h"

uint256 calculateMerkleRoot(uint256 *hashes, size_t size)
{
  if (size) {
    size_t txNum = size;
    SHA256_CTX sha256;
    while (txNum > 1) {
      size_t iterNum = (txNum / 2) + (txNum % 2);
      for (size_t i = 0; i < iterNum; i++) {
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, hashes[i*2].begin(), sizeof(uint256));
        SHA256_Update(&sha256, hashes[i*2+1 < txNum ? i*2+1 : i*2].begin(), sizeof(uint256));
        SHA256_Final(hashes[i].begin(), &sha256);

        SHA256_Init(&sha256);
        SHA256_Update(&sha256, hashes[i].begin(), sizeof(uint256));
        SHA256_Final(hashes[i].begin(), &sha256);
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

  SHA256_CTX sha256;
  for (size_t i = 0; i < treeSize; i++) {
    if (index & 1) {
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, tree[i].begin(), sizeof(uint256));
      SHA256_Update(&sha256, result.begin(), sizeof(uint256));
      SHA256_Final(result.begin(), &sha256);

      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(uint256));
      SHA256_Final(result.begin(), &sha256);
    } else {
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(uint256));
      SHA256_Update(&sha256, tree[i].begin(), sizeof(uint256));
      SHA256_Final(result.begin(), &sha256);

      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(uint256));
      SHA256_Final(result.begin(), &sha256);
    }

    index >>= 1;
  }

  return result;
}
