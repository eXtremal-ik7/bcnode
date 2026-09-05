// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include "common/intrusive_ptr.h"
#include "common/inbox.h"
#include "common/parallelRunner.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
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

  // Preparation is parallel, connect is ordered. Seq reserves a position in the window;
  // Gen identifies late results from jobs that were running when their branch was discarded.
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

// A job is also its completion event: ownership travels from the combiner to a worker and
// back, without allocating a second node. Other events are consumed by the combiner once.
struct CBlockPipelineEvent {
  enum class EType { Candidate, Prepared, Connected, Feed, Bulk, Drain, Stop };

  struct CReply {
    std::atomic<bool> Done = false;
    void wait() { Done.wait(false, std::memory_order_acquire); }
    void finish() {
      Done.store(true, std::memory_order_release);
      Done.notify_all();
    }
  };

  explicit CBlockPipelineEvent(EType type) : Type(type) {}
  EType Type;
  CBlockPipelineEvent *Next = nullptr;
  BC::Common::BlockIndex *Index = nullptr;
  std::unique_ptr<CSegment> Segment;
  std::shared_ptr<CReply> Reply;
  size_t Lane = 0;
  size_t FailedAt = 0;
  bool Ok = true;
  bool Bulk = false;
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

  // Offer a fully data-reachable tip. The caller may become the combiner and plan work, but
  // never waits for validation, connection or room in the segment window.
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
  using CEvent = CBlockPipelineEvent;
  using CReplies = std::vector<std::shared_ptr<CEvent::CReply>>;

  // One outstanding job per worker. Only the combiner gives work, only this worker takes it.
  // The atomic mailbox also carries the wakeup, so there is no separate sleep/wakeup race.
  struct CWorker {
    std::atomic<CEvent*> Job = nullptr;
    std::thread Thread;
    bool Busy = false; // combiner only; stays true until the completion is handled
    void give(CEvent *event);
    CEvent *take();
  };

  struct CSlot {
    enum class EState { Pending, Preparing, Ready };
    uint64_t Seq;
    std::unique_ptr<CEvent> Task; // null while the preparation worker owns it
    EState State;
  };

  void post(CEvent *event);
  void request(CEvent *event);
  void handle(std::unique_ptr<CEvent> event, CReplies &replies);
  void worker(CWorker &worker, bool preparation);
  void schedule(CReplies &replies);
  void admit(BC::Common::BlockIndex *candidate);
  void append(std::unique_ptr<CEvent> event, CSlot::EState state);
  std::unique_ptr<CSegment> makeSegment();
  void reset();
  void discard(CSegment &segment);
  bool idle() const;

private:
  BlockInMemoryIndex *BlockIndex_ = nullptr;
  BC::Common::ChainParams *ChainParams_ = nullptr;
  BC::DB::Storage *Storage_ = nullptr;
  CParams Params_;
  bool Started_ = false;
  newBestCallback Callback_;
  catchUpSink CatchUpSink_;

  CParallelRunner Runner_;
  std::deque<CWorker> PrepWorkers_;
  CWorker SerialWorker_;
  CEvent StopEvent_{CEvent::EType::Stop};

  CInbox<CEvent> Combiner_;
  // Everything below belongs to the combiner. A null window slot reserves the position of a
  // preparation still in flight; completed segments connect only from the front.
  std::deque<CSlot> Window_;
  std::deque<std::unique_ptr<CEvent>> FeedWaiters_;
  CReplies DrainWaiters_;
  std::shared_ptr<CEvent::CReply> StopReply_;
  size_t WindowLimit_ = 0;

  uint64_t NextSeq_ = 0;
  uint64_t Generation_ = 0;
  size_t Preparing_ = 0;
  BC::Common::BlockIndex *Frontier_ = nullptr;
  BC::Common::BlockIndex *Candidate_ = nullptr;

  bool Stopped_ = false;
  bool Resetting_ = false;
  bool BulkFeed_ = false;
  bool FlushBulk_ = false;
  bool TailWaiting_ = false;

  // Candidate down to the frontier. Consume from the back, retaining the rest for the next
  // segment; a new candidate invalidates the path. No repeated walk for each batch of a backlog.
  std::vector<BC::Common::BlockIndex*> Path_;
  BC::Common::BlockIndex *PathBase_ = nullptr;
  size_t PathBytes_ = 0;
};
