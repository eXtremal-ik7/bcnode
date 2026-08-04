#pragma once

#include <atomic>
#include "common/intrusive_ptr.h"
#include "common/serializedDataCache.h"
#include "common/serializeJson.h"
#include "proto.h"

#include <chrono>
#include <memory>

struct CBlockRawData;

enum BlockStatus {
  BSEmpty = 0,  // stub for a block known only as someone's predecessor: no header yet
  BSHeader,     // header accepted, no block data
  BSData,       // block data attached and waiting for the batch pipeline, nothing validated yet
  BSBlock,      // block data unpacked and accepted (failed checks stay here: not asked again)
  BSInvalid     // rejected for good: consensus or connect check failed
};

// Block data is here or on its way through the pipeline: don't ask peers for it again
static inline bool haveBlockData(BlockStatus state) { return state == BSData || state == BSBlock; }

namespace BTC {
namespace Common {

template<typename T>
struct alignas(8) BlockAcceptDataTy {
  typename T::BlockValidationData ValidationData;
  atomic_tagged_ptr<BlockAcceptDataTy, 3> SuccessorHeaders;
  atomic_tagged_ptr<BlockAcceptDataTy, 3> SuccessorBlocks;
  atomic_tagged_ptr<BlockAcceptDataTy, 3> ConcurrentHeaderNext;
  atomic_tagged_ptr<BlockAcceptDataTy, 3> ConcurrentBlockNext;
  BlockAcceptDataTy *CombinerNext;
};

class alignas(512) CIndexCacheObject {
private:
  mutable std::atomic<uintptr_t> Refs_ = 0;
  CAllocationInfo *Info_ = nullptr;
  // What this object currently contributes to Info_: it grows once preparation fills the
  // validation data and the linked outputs
  size_t Accounted_ = 0;

  SerializedDataObject BlockData_;
  Proto::CBlockValidationData ValidationData_;
  Proto::CBlockLinkedOutputs LinkedOutputs_;

public:
  uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
  uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }

public:
  CIndexCacheObject() {}
  CIndexCacheObject(CAllocationInfo *allocationInfo,
                    void *data,
                    size_t dataSize,
                    size_t memorySize,
                    void *unpackedData,
                    size_t unpackedMemorySize) :
    Info_(allocationInfo),
    BlockData_(data, dataSize, memorySize, unpackedData, unpackedMemorySize)
  {
    Accounted_ = BlockData_.memorySize();
    if (Info_)
      Info_->add(Accounted_);
  }

  ~CIndexCacheObject() {
    if (Info_)
      Info_->remove(Accounted_);
  }

  // Called by the owner of the block once preparation is done with it; a delta update, so
  // a segment cut and prepared again costs nothing extra
  void reaccount() {
    if (!Info_)
      return;
    size_t size = BlockData_.memorySize() + ValidationData_.memorySize() + LinkedOutputs_.memorySize();
    if (size > Accounted_)
      Info_->add(size - Accounted_);
    else
      Info_->remove(Accounted_ - size);
    Accounted_ = size;
  }

  const SerializedDataObject &blockData() const { return BlockData_; }
  BC::Proto::Block *block() const { return static_cast<BC::Proto::Block*>(BlockData_.unpackedData()); }
  Proto::CBlockValidationData &validationData() { return ValidationData_; }
  const Proto::CBlockValidationData &validationDataConst() const { return ValidationData_; }
  Proto::CBlockLinkedOutputs &linkedOutputs() { return LinkedOutputs_; }
};

template<typename T>
struct alignas(8) BlockIndexTy {
private:
  BlockIndexTy() {}

public:
  std::atomic<BlockStatus> IndexState;

  typename T::BlockHeader Header;
  uint32_t Height = std::numeric_limits<uint32_t>::max();
  uint32_t FileNo = std::numeric_limits<uint32_t>::max();
  uint32_t FileOffset = std::numeric_limits<uint32_t>::max();
  uint32_t SerializedBlockSize = std::numeric_limits<uint32_t>::max();
  uint32_t LinkedOutputsFileNo = std::numeric_limits<uint32_t>::max();
  uint32_t LinkedOutputsFileOffset = std::numeric_limits<uint32_t>::max();
  uint32_t LinkedOutputsSerializedSize = std::numeric_limits<uint32_t>::max();

  BlockIndexTy *Prev = nullptr;
  BlockIndexTy *Next = nullptr;

  // On the best chain: written by connect/disconnect, read from network threads (the block source
  // decides by it what is left to download)
  std::atomic<bool> OnChain = false;
  // Block data went into a segment: it is the preparation frontier or below it, and the walk
  // from a candidate stops here. Cleared when the block is disconnected or its segment is
  // thrown away
  std::atomic<bool> Prepared = false;

  UInt<256> ChainWork;
  atomic_intrusive_ptr<CIndexCacheObject> Serialized;
  // Raw block data waiting for a preparation wave; owned by whoever takes it out
  std::atomic<CBlockRawData*> Raw = nullptr;
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
  bool blockStored() {
    return FileNo != std::numeric_limits<uint32_t>::max() &&
           FileOffset != std::numeric_limits<uint32_t>::max() &&
           SerializedBlockSize != std::numeric_limits<uint32_t>::max();
  }

  bool indexStored() {
    return LinkedOutputsFileNo != std::numeric_limits<uint32_t>::max() &&
           LinkedOutputsFileOffset != std::numeric_limits<uint32_t>::max() &&
           LinkedOutputsSerializedSize != std::numeric_limits<uint32_t>::max();
  }
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
    BTC::serialize(stream, data.LinkedOutputsFileNo);
    BTC::serialize(stream, data.LinkedOutputsFileOffset);
    BTC::serialize(stream, data.LinkedOutputsSerializedSize);
    BTC::serialize(stream, data.ChainWork);
  }

  static inline void unserialize(xmstream &stream, BTC::Common::BlockIndexTy<T> &data) {
    BTC::unserialize(stream, data.Header);
    BTC::unserialize(stream, data.Height);
    BTC::unserialize(stream, data.FileNo);
    BTC::unserialize(stream, data.FileOffset);
    BTC::unserialize(stream, data.SerializedBlockSize);
    BTC::unserialize(stream, data.LinkedOutputsFileNo);
    BTC::unserialize(stream, data.LinkedOutputsFileOffset);
    BTC::unserialize(stream, data.LinkedOutputsSerializedSize);
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
