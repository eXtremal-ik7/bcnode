#pragma once

#include <atomic>
#include "common/intrusive_ptr.h"
#include "common/serializedDataCache.h"
#include "common/serializeJson.h"
#include "proto.h"

#include <chrono>
#include <memory>

enum BlockFlags : uint32_t {
  // WriteStarted elects a single writer; only Done publishes its fields. Both bits stay set.
  BFHeaderWriteStarted = 1u << 0,
  BFHeaderDone         = 1u << 1, // Header and Prev are immutable and readable
  BFDataWriteStarted   = 1u << 2, // suppress duplicate writes/downloads; data is not necessarily readable
  BFDataDone           = 1u << 3, // Serialized and initial disk coordinates are readable
  BFWorkChecked        = 1u << 4,
  BFHeaderReady        = 1u << 5, // header path to genesis: Height and ChainWork are readable
  BFDataReady          = 1u << 6, // complete block-data path to genesis
  BFOnChain            = 1u << 7,
  BFInvalid            = 1u << 8
};

static_assert(std::atomic<uint32_t>::is_always_lock_free);

namespace BTC {
namespace Common {

class alignas(512) CIndexCacheObject {
private:
  mutable std::atomic<uintptr_t> Refs_ = 0;
  CAllocationInfo *Info_ = nullptr;
  // What this object currently contributes to Info_: it grows once preparation fills the
  // validation data and the linked outputs
  size_t Accounted_ = 0;
  bool Relay_ = false;

  SerializedDataObject BlockData_;
  Proto::CBlockValidationData ValidationData_;
  Proto::CBlockLinkedOutputs LinkedOutputs_;

public:
  uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
  uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }

public:
  CIndexCacheObject() = default;
  CIndexCacheObject(CAllocationInfo *allocationInfo,
                    void *data,
                    size_t dataSize,
                    size_t memorySize,
                    void *unpackedData,
                    size_t unpackedMemorySize,
                    bool relay = false) :
    Info_(allocationInfo),
    Relay_(relay),
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
  bool relay() const { return Relay_; }
  Proto::CBlockValidationData &validationData() { return ValidationData_; }
  const Proto::CBlockValidationData &validationDataConst() const { return ValidationData_; }
  Proto::CBlockLinkedOutputs &linkedOutputs() { return LinkedOutputs_; }
};

template<typename T>
struct alignas(8) BlockIndexTy {
private:
  BlockIndexTy() = default;

public:
  std::atomic<uint32_t> Flags = 0;

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

  UInt<256> ChainWork;
  atomic_intrusive_ptr<CIndexCacheObject> Serialized;
  // TODO: make union with other field for save memory
  std::chrono::time_point<std::chrono::steady_clock> DownloadingStartTime = std::chrono::time_point<std::chrono::steady_clock>::max();

  // Successor lists are detached once their parent's topology is ready.
  std::atomic<BlockIndexTy*> SuccessorHeaders = nullptr;
  std::atomic<BlockIndexTy*> SuccessorBlocks = nullptr;

  // Written before CAS insertion, immutable once published in the corresponding list.
  BlockIndexTy *HeaderNext = nullptr;
  BlockIndexTy *BlockNext = nullptr;

public:
  static BlockIndexTy *create() {
    return new BlockIndexTy;
  }

  bool hasFlags(uint32_t flags) const {
    return (Flags.load(std::memory_order_acquire) & flags) == flags;
  }

  bool isOrphan() const { return !hasFlags(BFHeaderReady); }
  bool blockStored() const {
    return FileNo != std::numeric_limits<uint32_t>::max() &&
           FileOffset != std::numeric_limits<uint32_t>::max() &&
           SerializedBlockSize != std::numeric_limits<uint32_t>::max();
  }

  bool indexStored() const {
    return LinkedOutputsFileNo != std::numeric_limits<uint32_t>::max() &&
           LinkedOutputsFileOffset != std::numeric_limits<uint32_t>::max() &&
           LinkedOutputsSerializedSize != std::numeric_limits<uint32_t>::max();
  }

  bool ready() const {
    return hasFlags(BFHeaderReady | BFDataReady);
  }

  uint32_t knownHeight() const {
    return hasFlags(BFHeaderReady) ?
           Height : std::numeric_limits<uint32_t>::max();
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
