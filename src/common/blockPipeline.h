// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include "common/intrusive_ptr.h"
#include "common/parallelRunner.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
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

// One continuous piece of a candidate chain. Deserialization happens before the pipeline; this
// object carries only indexes and decoded blocks through validation, input linking and connect.
struct CSegment {
  struct CObject {
    BC::Common::BlockIndex *Index = nullptr;
    intrusive_ptr<BC::Common::CIndexCacheObject> Object;
    bool Valid = true;
    bool Relay = false;
    bool Completable = true;
  };

  // An input spending a coin older than the segment. Only the state immediately before connect
  // can answer it.
  struct CInput {
    uint32_t Object;
    uint32_t TxIdx;
    uint32_t InIdx;
  };

  std::vector<CObject> Objects;
  std::vector<CInput> Inputs;
  size_t Size = 0;

  // Preparation is parallel, connect is ordered. Seq orders segments inside one Gen; Gen makes
  // results from a discarded generation harmless after a failed segment resets Seq.
  uint64_t Seq = 0;
  uint64_t Gen = 0;

  // A database catch-up batch is already decoded and validated. It uses the ordered serial stage
  // but does not alter the active chain.
  bool CatchUp = false;

  // A catch-up segment outlives the sink: every database reads it on its own thread, the poster
  // too. The last share released frees the blocks, which is what lets the cache fall while the
  // reader is stopped on its size - it cannot free them by reading more
  void shareAdd(size_t count) { Shares_.fetch_add(count, std::memory_order_relaxed); }
  static void shareRelease(CSegment *segment) {
    if (segment->Shares_.fetch_sub(1, std::memory_order_acq_rel) == 1)
      delete segment;
  }

private:
  std::atomic<size_t> Shares_ = 0;
};

class CBlockPipeline {
public:
  struct CParams {
    size_t SegmentSizeLimit = 256*1048576;
    size_t SegmentBlocksLimit = 262144;
    // A bulk source holds its tail until either floor is reached. endBulk() flushes the tail.
    size_t BiteFloorSize = 32*1048576;
    size_t BiteFloorBlocks = 32768;
    size_t ReadyQueueDepth = 2;
    size_t PrepLanes = 2;
    size_t PreparedSizeLimit = 4096*1048576ull;
    // 0 means hardware_concurrency().
    unsigned WaveThreads = 0;
    bool Prefetch = true;
  };

  bool start(BlockInMemoryIndex &blockIndex,
             BC::Common::ChainParams &chainParams,
             BC::DB::Storage &storage,
             const CParams &params);
  void stop();
  ~CBlockPipeline() { stop(); }
  bool started() const { return Started_; }

  void setCallback(newBestCallback callback) { Callback_ = std::move(callback); }

  typedef std::function<void(std::unique_ptr<CSegment>)> catchUpSink;
  void setCatchUpSink(catchUpSink sink) { CatchUpSink_ = std::move(sink); }

  // The only chain input: the flat combiner calls this when a fully data-reachable tip with more
  // work appears. It never blocks the producer.
  void submit(BC::Common::BlockIndex *candidate);

  // A decoded batch of blocks already on the active chain, for databases that lag behind it.
  // Admission blocks until the ordered stage has room.
  void feed(std::unique_ptr<CSegment> segment);

  bool throttled() const;
  // true while reindex/network catch-up is producing a continuous run. Turning it off flushes
  // the final short segment; live blocks after that are always one-block segments.
  void setBulkFeed(bool bulk);
  void waitDrained();

private:
  void prepare();
  void serial();
  void scheduleLocked();
  std::unique_ptr<CSegment> makeSegmentLocked();
  void publishReadyLocked();
  void resetLocked(std::vector<std::unique_ptr<CSegment>> &dropped);
  void discard(CSegment &segment);
  void rescanCandidate();
  size_t queuedLocked() const;
  bool idleLocked() const;
  bool drainedLocked() const;

private:
  BlockInMemoryIndex *BlockIndex_ = nullptr;
  BC::Common::ChainParams *ChainParams_ = nullptr;
  BC::DB::Storage *Storage_ = nullptr;
  CParams Params_;
  bool Started_ = false;
  newBestCallback Callback_;
  catchUpSink CatchUpSink_;

  CParallelRunner Runner_;
  std::vector<std::thread> PrepThreads_;
  std::thread SerialThread_;

  mutable std::mutex Mutex_;
  std::condition_variable PrepCV_;
  std::condition_variable SerialCV_;
  std::condition_variable DrainCV_;
  std::deque<std::unique_ptr<CSegment>> Pending_;
  std::map<uint64_t, std::unique_ptr<CSegment>> Reorder_;
  std::deque<std::unique_ptr<CSegment>> Ready_;

  uint64_t NextSeq_ = 0;
  uint64_t PublishSeq_ = 0;
  uint64_t Generation_ = 0;
  size_t Preparing_ = 0;
  BC::Common::BlockIndex *Frontier_ = nullptr;
  BC::Common::BlockIndex *Candidate_ = nullptr;

  bool Stopped_ = false;
  bool SerialBusy_ = false;
  bool Resetting_ = false;
  std::atomic<bool> BulkFeed_ = false;
  bool FlushBulk_ = false;
  bool TailWaiting_ = false;

  // Candidate down to the frontier, reused across calls to keep its capacity
  std::vector<BC::Common::BlockIndex*> Path_;
};
