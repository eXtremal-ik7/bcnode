#pragma once

#include <atomic>
#include "common/arith_uint256.h"
#include "common/intrusive_ptr.h"
#include "common/serializedDataCache.h"
#include "common/serializeJson.h"
#include "proto.h"

#include <chrono>

enum BlockStatus {
  BSEmpty = 0,
  BSHeader,
  BSBlock,
  BSInvalid
};

namespace BTC {
namespace Common {

template<typename T>
struct alignas(8) BlockIndexTy {
private:
  BlockIndexTy() {}

public:
  std::atomic<BlockStatus> IndexState;

  typename T::BlockHeader Header;
  uint32_t Height = std::numeric_limits<uint32_t>::max();;
  uint32_t FileNo = std::numeric_limits<uint32_t>::max();;
  uint32_t FileOffset = std::numeric_limits<uint32_t>::max();;
  uint32_t SerializedBlockSize = std::numeric_limits<uint32_t>::max();

  BlockIndexTy *Prev = nullptr;
  BlockIndexTy *Next = nullptr;

  bool OnChain = false;

  arith_uint256 ChainWork;
  atomic_intrusive_ptr<const SerializedDataObject> Serialized;
  // TODO: make union with other field for save memory
  std::chrono::time_point<std::chrono::steady_clock> DownloadingStartTime = std::chrono::time_point<std::chrono::steady_clock>::max();

  // queue to chainstate
  // TODO: don't use intrusive container
  atomic_tagged_ptr<BlockIndexTy, 3> SuccessorHeaders;
  atomic_tagged_ptr<BlockIndexTy, 3> SuccessorBlocks;
  atomic_tagged_ptr<BlockIndexTy, 3> ConcurrentHeaderNext;
  atomic_tagged_ptr<BlockIndexTy, 3> ConcurrentBlockNext;
  BlockIndexTy *CombinerNext;

public:
  static BlockIndexTy *create(BlockStatus state, BlockIndexTy *prev) {
    BlockIndexTy *index = new BlockIndexTy;
    index->Prev = prev;
    index->IndexState = state;
    index->SuccessorHeaders.set(nullptr, 0);
    index->SuccessorBlocks.set(nullptr, 0);
    index->ConcurrentHeaderNext.set(nullptr, 0);
    index->ConcurrentBlockNext.set(nullptr, 0);
    index->CombinerNext = nullptr;
    return index;
  }

  bool isOrphan() { return Height == std::numeric_limits<uint32_t>::max(); }
};

}
}

namespace BTC {

template<typename T> struct Io<Common::BlockIndexTy<T>> {
  static inline void serialize(xmstream &stream, const BTC::Common::BlockIndexTy<T> &data) {
    BTC::serialize(stream, data.Header);
    BTC::serialize(stream, data.Height);
    BTC::serialize(stream, data.FileNo);
    BTC::serialize(stream, data.FileOffset);
    BTC::serialize(stream, data.SerializedBlockSize);
    BTC::serialize(stream, data.ChainWork);
  }

  static inline void unserialize(xmstream &stream, BTC::Common::BlockIndexTy<T> &data) {
    BTC::unserialize(stream, data.Header);
    BTC::unserialize(stream, data.Height);
    BTC::unserialize(stream, data.FileNo);
    BTC::unserialize(stream, data.FileOffset);
    BTC::unserialize(stream, data.SerializedBlockSize);
    BTC::unserialize(stream, data.ChainWork);
  }
};

}

// For HTTP API
template<typename T>
void serializeJson(xmstream &stream, const BTC::Common::BlockIndexTy<T> &index, const BTC::Proto::BlockTy<T> &block) {
  stream.write('{');
  serializeJsonInside(stream, index.Header); stream.write(',');
  serializeJson(stream, "height", index.Height); stream.write(',');
  if (index.Next) {
    serializeJson(stream, "next", index.Next->Header.GetHash()); stream.write(',');
  }
  serializeJson(stream, "tx", block.vtx);
  stream.write('}');
}
