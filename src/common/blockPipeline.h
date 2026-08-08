// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include "common/intrusive_ptr.h"
#include "common/parallelRunner.h"
#include "common/serializedDataCache.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class BlockInMemoryIndex;

namespace BC {
namespace DB {
class Storage;
}
}

typedef std::function<void(const std::vector<BC::Common::BlockIndex*>&)> newBestCallback;

// Pull pipeline: readers only attach block data to its index; the topology in the index is what
// decides what to connect next. The selector bites a segment from the preparation frontier
// towards the best candidate, waves prepare it whole, the serial stage takes prepared segments
// from a short queue. Arrival order of data is irrelevant

// Raw bytes a block points into: one buffer per block file (borrowed by every block in it),
// one per block for the network path
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

// Block data waiting on its index for preparation, behind an atomic pointer
struct CBlockRawData {
  intrusive_ptr<CRawBlockData> Buffer;
  uint32_t Offset = 0;
  uint32_t Size = 0;
  uint32_t FileNo = std::numeric_limits<uint32_t>::max();
  uint32_t FileOffset = std::numeric_limits<uint32_t>::max();
  // Only user of its buffer: the memory moves to the block object
  bool Exclusive = false;
  // Header work not verified yet
  bool CheckWork = true;
  // Block that came outside a catch-up download: relay it once it connects
  bool Relay = false;

  void *data() const { return static_cast<uint8_t*>(Buffer.get()->data()) + Offset; }
};

// A continuous piece of the chain prepared and connected as a whole: its blocks hide utxo pairs
// from each other, and a pair with one side connected is a coin nobody can spend again. Every
// verdict is made before the first block lands
struct CSegment {
  struct CObject {
    BC::Common::BlockIndex *Index = nullptr;
    intrusive_ptr<BC::Common::CIndexCacheObject> Object;
    // Block checks that need no chain state (work, standalone, contextual)
    bool Valid = true;
    // Came outside a catch-up download: relay it once it connects
    bool Relay = false;
    // Preparation left nothing unresolved; false means it spends what it may not
    bool Completable = true;
    // Catch-up only: the stored bytes the preparation rebuilds the object from, pointing into
    // the segment's blobs below
    const void *BlockData = nullptr;
    const void *LinkedOutputsData = nullptr;
    uint32_t BlockSize = 0;
    uint32_t LinkedOutputsSize = 0;
  };

  // Input spending a coin older than the segment: only the state it connects to answers it
  struct CInput {
    uint32_t Object;
    uint32_t TxIdx;
    uint32_t InIdx;
  };

  std::vector<CObject> Objects;
  std::vector<CInput> Inputs;
  size_t RawBytes = 0;
  // Block the segment continues from: where the frontier goes back to if it falls apart
  BC::Common::BlockIndex *Anchor = nullptr;
  // Anchor is not the preparation frontier: the segment is a fork, and the serial stage rebases
  // before it can be applied
  bool Reanchor = false;
  // Order of the bite; segments are prepared in parallel and connected in this order
  uint64_t Seq = 0;
  uint64_t Gen = 0;
  // Replays blocks the chain already holds, for a database that missed them. Comes from a
  // feeder, not from the selector, and connects nothing
  bool CatchUp = false;
  // Catch-up only: the combined reads the pointers above go into. The preparation drops them -
  // the objects it builds copy what they need
  intrusive_ptr<CRawBlockData> BlockBlob;
  intrusive_ptr<CRawBlockData> LinkedOutputsBlob;
};


// Reported back by the preparation: unparsed bytes (the reader throttles on them) and blocks
// that do not parse (the reindex gives up on the file)
struct CPipelineCounters {
  std::atomic<size_t> RawBytes = 0;
  std::atomic<uint64_t> FileParseErrors = 0;
};

// Best chain candidate: the tip with the most work whose data is here. Updated by whoever
// attaches data and by the header chain builder when it hands out heights
class CCandidateTracker {
public:
  void setListener(std::function<void()> listener) { Listener_ = std::move(listener); }

  void update(BC::Common::BlockIndex *index) {
    if (!index || index->Height == std::numeric_limits<uint32_t>::max())
      return;

    BC::Common::BlockIndex *best = Best_.load(std::memory_order_relaxed);
    for (;;) {
      if (best && !(index->ChainWork > best->ChainWork))
        return;
      if (Best_.compare_exchange_weak(best, index)) {
        Generation_.fetch_add(1, std::memory_order_release);
        if (Listener_)
          Listener_();
        return;
      }
    }
  }

  // A candidate turned out to sit above an invalid block: force the next scan
  void reset(BC::Common::BlockIndex *index) {
    Best_.store(index);
    Generation_.fetch_add(1, std::memory_order_release);
  }

  BC::Common::BlockIndex *best() const { return Best_.load(std::memory_order_acquire); }
  uint64_t generation() const { return Generation_.load(std::memory_order_acquire); }

private:
  std::atomic<BC::Common::BlockIndex*> Best_ = nullptr;
  std::atomic<uint64_t> Generation_ = 0;
  std::function<void()> Listener_;
};

class CBlockPipeline {
public:
  struct CParams {
    // Raw bytes in one segment: the window inside which a created-and-spent utxo pair is skipped
    // entirely, and the unit the serial stage takes
    size_t SegmentSizeLimit = 256*1048576;
    // Second cap: a chain of tiny blocks costs per-block structures, not bytes
    size_t SegmentBlocksLimit = 262144;
    // Floors of a bite under a bulk feed: dust segments pay the serial linking pass without
    // feeding it, so wait for more while the pipeline chews. Either floor opens the bite;
    // an idle pipeline takes anything
    size_t BiteFloorSize = 32*1048576;
    size_t BiteFloorBlocks = 32768;
    // Prepared segments waiting for the serial stage; deeper is colder data at connect
    size_t ReadyQueueDepth = 2;
    // Segments prepared at once. Neighbours are independent (a pair lives inside one segment),
    // and one lane cannot feed the serial stage: its linking pass is serial and costs about as
    // much as the connect it feeds
    size_t PrepLanes = 2;
    // Read ahead limit: raw block data attached but not prepared yet
    size_t RawSizeLimit = 768*1048576;
    // Prepared block data waiting for a connect. Preparation turns raw bytes into objects
    // several times their size and only a connect releases them, so without this limit a
    // connect side that falls behind the reader eats the machine instead of slowing it down.
    // Set above what a healthy reindex holds: a binding limit costs throughput
    size_t PreparedSizeLimit = 4096*1048576ull;
    // Parsed data of connected but unwritten blocks. Filled by slow databases (the archive) and
    // drained by the storage thread alone, so waiting on it never holds the chain up
    size_t StorageBacklogLimit = 512*1048576;
    // 0 - hardware concurrency
    unsigned WaveThreads = 0;
    // Warm the utxo cache for the residual inputs while the serial stage runs the previous segment
    bool Prefetch = true;
  };

  enum EResult {
    Staged = 0,   // the pipeline owns the data now
    NotStaged,    // caller keeps the data and processes it itself
    Invalid       // data does not even hold a block header
  };

  bool start(BlockInMemoryIndex &blockIndex,
             BC::Common::ChainParams &chainParams,
             BC::DB::Storage &storage,
             const CParams &params);
  void stop();
  // Failed-start paths in main never reach the explicit stop; joining here beats std::terminate from ~std::thread
  ~CBlockPipeline() { stop(); }
  bool started() const { return Started_; }
  void setCallback(newBestCallback callback) { Callback_ = std::move(callback); }

  // Where a prepared catch-up segment goes, called by the serial stage in chain order. The sink
  // owns it from there: the databases are still reading it after the call returns
  typedef std::function<void(std::unique_ptr<CSegment>)> catchUpSink;
  void setCatchUpSink(catchUpSink sink) { CatchUpSink_ = std::move(sink); }

  // A segment built outside the selector: the chain is settled, so a feeder knows what is
  // missing without walking topology. Blocks until the pipeline has room
  void feed(std::unique_ptr<CSegment> segment);
  // A stored block that would not rebuild: the block database is damaged and the run is over
  bool catchUpFailed() const { return CatchUpFailed_.load(std::memory_order_relaxed); }

  // Block file reader: block data lives inside a shared block file buffer
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
                            bool relay,
                            BC::Common::BlockIndex **attached);

  // Read ahead limits. Only the reader can free raw data, so a reader stopped while the pipeline
  // idles is the one way to stall the chain (hence starving()); the storage backlog drains itself
  bool throttled() const;
  bool starving() const;
  // Block file boundary: data still raw a few files later gets a buffer of its own
  void rotateFile();
  // More data is coming right behind (file reindex, catch-up download): the selector may hold
  // a bite below the floor. Off lets the tail through
  void setBulkFeed(bool bulk);
  // Everything attached is connected, written or rejected
  void waitDrained();
  bool failed() const { return Counters_.FileParseErrors.load(std::memory_order_relaxed) != 0; }

  size_t rawBytes() const { return Counters_.RawBytes.load(std::memory_order_relaxed); }

private:
  void selector();
  void prepare();
  void serial();
  // Prepared out of order, connected in order
  void publishReadyLocked();
  // What is in flight belongs to a chain that is not going to happen: take it back, bite again
  // from 'frontier'
  void resetLocked(BC::Common::BlockIndex *frontier, std::vector<std::unique_ptr<CSegment>> &dropped);
  bool pipelineIdleLocked() const;
  // 'deferred' reports a run below the floor held back for a bulk feed
  std::unique_ptr<CSegment> bite(BC::Common::BlockIndex *frontier, bool floorActive, bool *deferred);
  // Nothing of a segment reaches the chain: pairs go, the blocks become free to be bitten again
  void discard(CSegment &segment);
  void wakeSelector();
  // The tip the tracker holds is gone (rejected): find the best one left
  void rescanCandidate();
  // Selector thread only - it alone puts blocks into segments, so nothing here is in use
  void releaseStragglers();
  bool drainedLocked() const;

private:
  BlockInMemoryIndex *BlockIndex_ = nullptr;
  BC::Common::ChainParams *ChainParams_ = nullptr;
  BC::DB::Storage *Storage_ = nullptr;
  CParams Params_;
  bool Started_ = false;
  newBestCallback Callback_;
  catchUpSink CatchUpSink_;
  std::atomic<bool> CatchUpFailed_ = false;

  CParallelRunner Runner_;
  std::thread SelectorThread_;
  std::vector<std::thread> PrepThreads_;
  std::thread SerialThread_;

  mutable std::mutex Mutex_;
  std::condition_variable SelectorCV_;
  std::condition_variable PrepCV_;
  std::condition_variable SerialCV_;
  std::condition_variable DrainCV_;
  // Bitten, waiting for a lane
  std::deque<std::unique_ptr<CSegment>> Pending_;
  // Prepared, waiting for the segments bitten before it
  std::map<uint64_t, std::unique_ptr<CSegment>> Reorder_;
  std::deque<std::unique_ptr<CSegment>> Ready_;
  uint64_t NextSeq_ = 0;
  uint64_t PublishSeq_ = 0;
  size_t InFlight_ = 0;
  size_t PrepBusy_ = 0;
  // Last block handed to the preparation; the segment continues from it
  BC::Common::BlockIndex *Frontier_ = nullptr;
  // Bumped when the queue is thrown away: a segment prepared against the old frontier is stale
  uint64_t ResetGen_ = 0;
  bool Stopped_ = false;
  bool SelectorBusy_ = false;
  bool SerialBusy_ = false;
  // A run below the floor is held back: not drained, and an idle pipeline picks it up
  bool FloorWait_ = false;
  std::atomic<bool> BulkFeed_ = false;
  // Bumped by every attach; the selector sleeps until it moves past the value it found nothing at
  std::atomic<uint64_t> ArrivalGen_ = 0;
  uint64_t IdleAt_ = std::numeric_limits<uint64_t>::max();
  std::atomic<bool> SelectorWaiting_ = false;
  // Blocks attached from the block file being read; the reader thread owns this one
  std::vector<BC::Common::BlockIndex*> FileBlocks_;
  // Block files still in work: the read ahead limit holds a few, older ones carry stragglers
  static constexpr size_t FileGrace = 8;
  std::deque<std::vector<BC::Common::BlockIndex*>> RecentFiles_;
  // ... what the grace ran out for, for the selector to unpin
  std::vector<BC::Common::BlockIndex*> Stragglers_;

  CPipelineCounters Counters_;
};
