// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include "common/inbox.h"
#include "common/intrusive_ptr.h"
#include "common/mpmcRing.h"
#include "common/serializedDataCache.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <semaphore>
#include <thread>
#include <unordered_map>
#include <vector>

class BlockInMemoryIndex;

namespace BC {
namespace DB {
class Storage;
}
}

// Batch pipeline, stage 1: block data is collected into batches (continuous runs by height)
// before any heavy work is done on it, then a batch is processed block by block by the existing
// accept path. Reindex reader and network catch-up feed the same assembler

// Raw bytes a staged block points into: one buffer per block file for the reader (borrowed by
// every block in it), one per received block for the network (handed to the block object)
class CRawBlockData {
public:
  CRawBlockData(void *data, size_t memorySize, CAllocationInfo *info) :
    Data_(data), MemorySize_(memorySize), Info_(info) {
    if (Info_)
      Info_->add(MemorySize_);
  }

  ~CRawBlockData() {
    if (Info_ && Data_)
      Info_->remove(MemorySize_);
    operator delete(Data_);
  }

  uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
  uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }

  void *data() const { return Data_; }
  size_t memorySize() const { return MemorySize_; }

  // Hand the memory to the block object built from it (single-block buffers only)
  void *detach() {
    void *data = Data_;
    if (Info_ && Data_)
      Info_->remove(MemorySize_);
    Data_ = nullptr;
    return data;
  }

private:
  mutable std::atomic<uintptr_t> Refs_ = 0;
  void *Data_;
  size_t MemorySize_;
  CAllocationInfo *Info_;
};

struct CStagedBlock {
  // Index in BSData state; nullptr means no index was reserved for this block and the worker
  // runs the full AddBlock path on it
  BC::Common::BlockIndex *Index = nullptr;
  intrusive_ptr<CRawBlockData> Buffer;
  uint32_t Offset = 0;
  uint32_t Size = 0;
  uint32_t FileNo = std::numeric_limits<uint32_t>::max();
  uint32_t FileOffset = std::numeric_limits<uint32_t>::max();
  // Only user of its buffer: the memory moves to the block object
  bool Exclusive = false;
  // Header work not verified yet
  bool CheckWork = true;

  void *data() const { return static_cast<uint8_t*>(Buffer.get()->data()) + Offset; }
};

// Publication unit of the assembler inbox: the block file reader fills a chunk of many blocks,
// the network path publishes one block at a time
struct CStagedChunk {
  CStagedChunk *Next = nullptr;
  std::vector<CStagedBlock> Blocks;
};

struct CBlockBatch {
  std::vector<CStagedBlock> Blocks;
  size_t Bytes = 0;
  // Blocks form a continuous run, each one the predecessor of the next
  bool Continuous = true;
};

// A batch is taken by one worker and processed in height order; parallelism comes from the
// batches in flight. The pool is permanent: no join at file boundaries. Lock-free: batches go
// through a ring, sleeping happens on a semaphore and on InFlight_
class CBatchExecutor {
public:
  typedef std::function<void(CStagedBlock&, BC::Common::CheckConsensusCtx&)> CBlockProc;

  bool start(unsigned threadsNum, CBlockProc proc);
  // No submit() from the moment stop() is entered
  void stop();

  void submit(std::unique_ptr<CBlockBatch> &&batch);
  void waitIdle();

private:
  void worker();

private:
  // Well above the number of batches the staging limit allows to be in flight
  static constexpr size_t QueueCapacity = 8192;

  CBlockProc Proc_;
  CMpmcRing<CBlockBatch> Queue_;
  std::counting_semaphore<> Tickets_{0};
  // Submitted but not processed yet; waitIdle() sleeps on it
  std::atomic<size_t> InFlight_ = 0;
  std::atomic<bool> Stopped_ = false;
  std::vector<std::thread> Threads_;
};

// The run state machine is sequential by nature, so it belongs to one thread at a time:
// producers publish a chunk into the inbox and leave, whoever takes the owner role runs the
// machine on data nobody else touches
class CBlockAssembler {
public:
  struct CParams {
    size_t BatchSizeLimit = 32*1048576;
    size_t BatchBlocksLimit = 256;
    // Read-ahead limit: bytes staged but not processed yet
    size_t StagingSizeLimit = 256*1048576;
    // Blocks waiting behind a hole before continuity is given up: holding them back means the
    // workers have nothing to do, so the window is small
    size_t HoldBlocksLimit = 256;
    size_t HoldSizeLimit = 64*1048576;
    unsigned FlushTimeoutMs = 1000;
    // One publication per this many blocks read from a block file
    size_t FileChunkBlocks = 64;
  };

  enum EResult {
    Staged = 0,   // the assembler owns the data now
    NotStaged,    // caller keeps the data and processes it itself
    Invalid       // data does not even hold a block header
  };

  bool start(BlockInMemoryIndex &blockIndex,
             BC::Common::ChainParams &chainParams,
             BC::DB::Storage &storage,
             unsigned threadsNum,
             const CParams &params);
  void stop();
  bool started() const { return Started_; }

  // Reindex reader: block data lives inside a shared block file buffer
  EResult attachFromFile(const intrusive_ptr<CRawBlockData> &buffer,
                         uint32_t offset,
                         uint32_t size,
                         uint32_t fileNo,
                         uint32_t fileOffset);
  // Takes ownership of 'data' when the result is Staged, leaves it to the caller otherwise
  EResult attachFromNetwork(void *data,
                            uint32_t size,
                            uint32_t memorySize,
                            const BC::Proto::BlockHeader &header,
                            const BC::Proto::BlockHashTy &hash,
                            BC::Common::BlockIndex **staged);

  void flush();
  // ... including blocks still waiting for a predecessor; returns when it is done
  void flushAll();
  void flushOnTimeout();
  void waitIdle();

  size_t stagedBytes() const { return StagedBytes_.load(std::memory_order_relaxed); }
  bool overflow() const { return stagedBytes() >= Params_.StagingSizeLimit; }
  // A block from a block file failed to load: the reader gives up
  bool failed() const { return Failed_.load(std::memory_order_relaxed); }
  uint64_t batchCount() const { return BatchCount_.load(std::memory_order_relaxed); }
  uint64_t outOfOrderBlocks() const { return OutOfOrderBlocks_.load(std::memory_order_relaxed); }

private:
  typedef std::vector<std::unique_ptr<CBlockBatch>> CReadyBatches;

  void publish(CStagedChunk *chunk);
  void publishFileChunk();
  void drain();
  bool requestsPending() const;
  void serveRequests(CReadyBatches &ready);
  void cutOnTimeout(CReadyBatches &ready);
  void stage(CStagedBlock &&block, CReadyBatches &ready);
  void extendRun(CStagedBlock &&block, CReadyBatches &ready);
  void advance(CReadyBatches &ready);
  void addUnplaced(CStagedBlock &&block, CReadyBatches &ready);
  void cut(CReadyBatches &ready);
  void cutStaged(CReadyBatches &ready);
  void submit(CReadyBatches &ready);
  void reportStats();
  void process(CStagedBlock &block, BC::Common::CheckConsensusCtx &ccCtx);

private:
  BlockInMemoryIndex *BlockIndex_ = nullptr;
  BC::Common::ChainParams *ChainParams_ = nullptr;
  BC::DB::Storage *Storage_ = nullptr;
  CParams Params_;
  bool Started_ = false;

  CBatchExecutor Executor_;
  CInbox<CStagedChunk> Inbox_;
  // Filled and published by the block file reader thread only
  static thread_local CStagedChunk *FileChunk_;

  // Staged but not processed yet (queued batches included) - the read-ahead measure
  std::atomic<size_t> StagedBytes_ = 0;
  std::atomic<bool> Failed_ = false;
  std::atomic<uint64_t> BatchCount_ = 0;
  std::atomic<uint64_t> OutOfOrderBlocks_ = 0;

  // Requests to the owner: cut, cut everything, cut if nothing moved for a while. flushAll()
  // waits until its ticket shows up in CutAllServed_
  std::atomic<uint64_t> CutReq_ = 0;
  std::atomic<uint64_t> CutServed_ = 0;
  std::atomic<uint64_t> CutAllReq_ = 0;
  std::atomic<uint64_t> CutAllServed_ = 0;
  std::atomic<bool> TimeoutReq_ = false;

  // Owner-private from here on: whoever holds the role is the only thread touching these
  // Blocks waiting for their predecessor to be batched, keyed by that predecessor
  std::unordered_map<BC::Common::BlockIndex*, CStagedBlock> Staged_;
  size_t HoldBytes_ = 0;
  // Last block handed to the pipeline; the run continues from it
  BC::Common::BlockIndex *Frontier_ = nullptr;
  std::unique_ptr<CBlockBatch> Batch_;
  std::unique_ptr<CBlockBatch> Unplaced_;
  std::chrono::time_point<std::chrono::steady_clock> LastCutTime_;
  std::chrono::time_point<std::chrono::steady_clock> LastStatTime_;
  uint64_t LastStatBatchCount_ = 0;
};
