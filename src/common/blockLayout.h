// Copyright (c) 2026 Ivan K.
// Copyright (c) 2026 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <cstddef>

// A byte range inside a larger serialized object, counted from its start. 32 bits because
// what it points into is one block, and a block is bounded by MaxBlockSize; the absolute
// file position is this plus the block's own offset
struct CDataSpan32 {
  uint32_t Offset;
  uint32_t Size;
};

// What a coin writes after the transaction list, LTC's MWEB extension block being the only one
template<typename CBlockTy>
static inline size_t blockExtensionSize(const CBlockTy &block)
{
  if constexpr (requires { CBlockTy::extensionSize(block); })
    return CBlockTy::extensionSize(block);
  else
    return 0;
}

// The parse and the bytes on disk must describe the same block, or a span reads somebody
// else's transaction: the pieces have to add up to the stored size
template<typename CBlockTy, typename VectorTy>
static inline bool txLayoutMatchesStored(const CBlockTy &block, const VectorTy &layout, uint32_t storedSize)
{
  if (layout.size() != block.Vtx.size() || layout.size() == 0)
    return false;
  const CDataSpan32 &last = layout[layout.size() - 1];
  return last.Offset + last.Size + blockExtensionSize(block) == storedSize;
}
