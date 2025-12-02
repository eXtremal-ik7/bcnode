// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/uint256.h"
#include <memory>

uint256 calculateMerkleRoot(uint256 *hashes, size_t size);
uint256 calculateMerkleRoot(uint256 hash, uint256 *tree, size_t treeSize, size_t index);

template<typename BlockTy>
uint256 calculateBlockMerkleRoot(const BlockTy &block)
{
  size_t txNum = block.vtx.size();
  std::unique_ptr<uint256[]> hashes(new uint256[txNum]);

  // Get hashes for all transactions
  for (size_t i = 0; i < txNum; i++)
    hashes[i] = block.vtx[i].getTxId();

  return calculateMerkleRoot(hashes.get(), txNum);
}

template<typename BlockTy>
uint256 calculateBlockWitnessMerkleRoot(const BlockTy &block)
{
  size_t txNum = block.vtx.size();
  std::unique_ptr<uint256[]> hashes(new uint256[txNum]);

  // Get hashes for all transactions
  if (txNum >= 1)
    hashes[0].SetNull();
  for (size_t i = 1; i < txNum; i++)
    hashes[i] = block.vtx[i].getWTxid();

  return calculateMerkleRoot(hashes.get(), txNum);
}
