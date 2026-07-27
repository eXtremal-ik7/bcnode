// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "merkleTree.h"
#include "hash.h"

namespace BTC {

BaseBlob<256> calculateMerkleRoot(BaseBlob<256> *hashes, size_t size)
{
  if (size) {
    size_t txNum = size;
    while (txNum > 1) {
      size_t iterNum = (txNum / 2) + (txNum % 2);
      for (size_t i = 0; i < iterNum; i++)
        hashes[i] = sha256d(hashes[i*2], hashes[i*2+1 < txNum ? i*2+1 : i*2]);

      txNum = iterNum;
    }

    return hashes[0];
  } else {
    return BaseBlob<256>::zero();
  }
}

BaseBlob<256> calculateMerkleRoot(BaseBlob<256> hash, BaseBlob<256> *tree, size_t treeSize, size_t index)
{
  BaseBlob<256> result = hash;
  if (!treeSize)
    return result;

  for (size_t i = 0; i < treeSize; i++) {
    if (index & 1)
      result = sha256d(tree[i], result);
    else
      result = sha256d(result, tree[i]);

    index >>= 1;
  }

  return result;
}

}
