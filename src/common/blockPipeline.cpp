// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockPipeline.h"
#include "blockDataBase.h"
#include "db/archive.h"
#include "db/storage.h"
#include "loguru.hpp"

#include <algorithm>

bool CBlockPipeline::start(BlockInMemoryIndex &blockIndex,
                           BC::Common::ChainParams &chainParams,
                           BC::DB::Storage &storage,
                           const CParams &params)
{
  BlockIndex_ = &blockIndex;
  ChainParams_ = &chainParams;
  Storage_ = &storage;
  Params_ = params;
  Frontier_ = blockIndex.best();
  Candidate_ = blockIndex.best();

  unsigned threadsNum = Params_.WaveThreads;
  if (!threadsNum)
    threadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;

  LOG_F(INFO,
        "Block pipeline: %u wave threads, %zu preparation lanes, segment %.1lfMb (%zu blocks), bulk floor %.1lfMb (%zu blocks), ready queue %zu, prepared %.1lfMb",
        threadsNum,
        Params_.PrepLanes,
        Params_.SegmentSizeLimit / 1048576.0,
        Params_.SegmentBlocksLimit,
        Params_.BiteFloorSize / 1048576.0,
        Params_.BiteFloorBlocks,
        Params_.ReadyQueueDepth,
        Params_.PreparedSizeLimit / 1048576.0);

  Runner_.start(threadsNum > 1 ? threadsNum - 1 : 0);
  SerialThread_ = std::thread([this]() { serial(); });
  for (size_t i = 0; i < std::max<size_t>(Params_.PrepLanes, 1); i++)
    PrepThreads_.emplace_back([this]() { prepare(); });

  Started_ = true;
  blockIndex.setReadyPipeline(this);
  return true;
}

void CBlockPipeline::stop()
{
  if (!Started_)
    return;

  BlockIndex_->setReadyPipeline(nullptr);
  {
    std::lock_guard lock(Mutex_);
    Stopped_ = true;
    TailWaiting_ = false;
    FlushBulk_ = false;
  }
  PrepCV_.notify_all();
  SerialCV_.notify_all();
  DrainCV_.notify_all();

  for (std::thread &thread: PrepThreads_)
    thread.join();
  PrepThreads_.clear();
  SerialCV_.notify_all();
  SerialThread_.join();
  Runner_.stop();
  Started_ = false;
}

void CBlockPipeline::submit(BC::Common::BlockIndex *candidate)
{
  if (!candidate || !candidate->ready())
    return;

  // The cheap filter first: the walk below is as long as the backlog no segment has been cut
  // from yet, and most offers lose to the tip already admitted
  BC::Common::BlockIndex *checked = nullptr;
  {
    std::lock_guard lock(Mutex_);
    if (Stopped_)
      return;
    if (Candidate_ && !(candidate->ChainWork > Candidate_->ChainWork))
      return;
    checked = Candidate_;
  }

  // A late child of a rejected branch can still reach the data combiner. The path below the tip
  // already admitted passed this same check back then, and every BSInvalid drops Candidate_
  // through resetLocked - so this is one block on a growing chain, the fork depth on a reorg
  for (BC::Common::BlockIndex *index = candidate;
       index && index != checked && !index->OnChain.load(std::memory_order_relaxed);
       index = index->Prev) {
    if (index->IndexState.load(std::memory_order_relaxed) == BSInvalid)
      return;
  }

  std::lock_guard lock(Mutex_);
  if (Stopped_)
    return;
  if (Candidate_ && !(candidate->ChainWork > Candidate_->ChainWork))
    return;

  Candidate_ = candidate;
  scheduleLocked();
  DrainCV_.notify_all();
}

void CBlockPipeline::feed(std::unique_ptr<CSegment> segment)
{
  if (!segment || segment->Objects.empty())
    return;

  std::unique_lock lock(Mutex_);
  const size_t limit = std::max<size_t>(1, Params_.ReadyQueueDepth + std::max<size_t>(Params_.PrepLanes, 1));
  DrainCV_.wait(lock, [this, limit]() { return Stopped_ || queuedLocked() < limit; });
  if (Stopped_)
    return;

  segment->CatchUp = true;
  segment->Seq = NextSeq_++;
  segment->Gen = Generation_;
  Reorder_[segment->Seq] = std::move(segment);
  publishReadyLocked();
}

bool CBlockPipeline::throttled() const
{
  // The serial stage reads utxo on every spend, and every unflushed layer is one more probe per
  // lookup: this bounds the stack under the reader. The archive is not asked - its connect only
  // writes, so its layers cost memory and no reads
  if (Storage_->utxodb().pipelineFull())
    return true;

  // No escape needed: the cache is released by whoever finishes with the blocks - a database
  // dropping its share, the storage thread flushing what it wrote - never by the reader itself
  return Storage_->cache().size() >= Params_.PreparedSizeLimit;
}

void CBlockPipeline::setBulkFeed(bool bulk)
{
  if (BulkFeed_.exchange(bulk, std::memory_order_acq_rel) == bulk)
    return;

  std::lock_guard lock(Mutex_);
  // Another edge may have won the mutex first. Only the current state is allowed to touch the
  // queue; a stale edge can return because the newer one performs the same scheduling step.
  if (Stopped_ || BulkFeed_.load(std::memory_order_acquire) != bulk)
    return;

  if (!bulk) {
    FlushBulk_ = true;
    TailWaiting_ = false;
  }
  scheduleLocked();
  DrainCV_.notify_all();
}

void CBlockPipeline::waitDrained()
{
  std::unique_lock lock(Mutex_);
  DrainCV_.wait(lock, [this]() { return Stopped_ || drainedLocked(); });
}

size_t CBlockPipeline::queuedLocked() const
{
  return Pending_.size() + Preparing_ + Reorder_.size() + Ready_.size();
}

bool CBlockPipeline::idleLocked() const
{
  return queuedLocked() == 0 && !SerialBusy_;
}

bool CBlockPipeline::drainedLocked() const
{
  return idleLocked() && !TailWaiting_ && !FlushBulk_ && !Resetting_;
}

std::unique_ptr<CSegment> CBlockPipeline::makeSegmentLocked()
{
  if (!Candidate_ || !Frontier_ || Candidate_ == Frontier_)
    return nullptr;

  // Down from the candidate to whichever comes first: the frontier it extends, or the connected
  // chain it forks off. Both terminators bound the walk - without them an offer from another
  // branch reaches the genesis block, once per offer and with a chain-sized vector behind it
  std::vector<BC::Common::BlockIndex*> &path = Path_;
  BC::Common::BlockIndex *index = Candidate_;
  path.clear();
  while (index && index != Frontier_ && !index->OnChain.load(std::memory_order_relaxed)) {
    if (index->IndexState.load(std::memory_order_relaxed) == BSInvalid) {
      // The branch died under us. Back to the settled chain, so its work stops being the bar
      // every later offer has to clear: the rescan that follows a rejection brings the next one
      Candidate_ = BlockIndex_->best();
      TailWaiting_ = false;
      return nullptr;
    }
    path.push_back(index);
    index = index->Prev;
  }

  // Ended on the connected chain instead of the frontier: a fork, and one branch is not prepared
  // while another is in flight. Frontier_ moves only when a segment is actually cut, below, so a
  // walk that ends empty leaves it where it was
  if (!index || (index != Frontier_ && !idleLocked()))
    return nullptr;

  if (path.empty())
    return nullptr;
  std::reverse(path.begin(), path.end());

  const bool bulkFeed = BulkFeed_.load(std::memory_order_acquire);
  const bool bulk = bulkFeed || FlushBulk_;
  if (bulkFeed && !FlushBulk_ && path.size() < Params_.BiteFloorBlocks) {
    size_t bytes = 0;
    for (BC::Common::BlockIndex *entry: path)
      bytes += entry->SerializedBlockSize;
    if (bytes < Params_.BiteFloorSize) {
      TailWaiting_ = true;
      return nullptr;
    }
  }

  auto segment = std::make_unique<CSegment>();
  const size_t blocksLimit = bulk ? std::max<size_t>(Params_.SegmentBlocksLimit, 1) : 1;
  for (BC::Common::BlockIndex *entry: path) {
    const uint32_t size = entry->SerializedBlockSize;
    if (!segment->Objects.empty() &&
        (segment->Objects.size() >= blocksLimit ||
         (bulk && segment->Size + size > Params_.SegmentSizeLimit)))
      break;

    segment->Objects.emplace_back().Index = entry;
    segment->Size += size;
  }

  Frontier_ = segment->Objects.back().Index;
  TailWaiting_ = false;
  if (FlushBulk_ && Frontier_ == Candidate_)
    FlushBulk_ = false;
  return segment;
}

void CBlockPipeline::scheduleLocked()
{
  if (Stopped_)
    return;

  if (Resetting_) {
    if (Preparing_ || SerialBusy_)
      return;
    // Both are read from the settled chain here: a best snapshot taken at reset time goes stale
    // if the serial stage was still connecting. A submit that arrived meanwhile keeps its offer
    BC::Common::BlockIndex *best = BlockIndex_->best();
    Frontier_ = best;
    if (!Candidate_ || best->ChainWork > Candidate_->ChainWork)
      Candidate_ = best;
    Resetting_ = false;
  }

  const size_t limit = std::max<size_t>(1, Params_.ReadyQueueDepth + std::max<size_t>(Params_.PrepLanes, 1));
  while (queuedLocked() < limit) {
    std::unique_ptr<CSegment> segment = makeSegmentLocked();
    if (!segment)
      break;

    segment->Seq = NextSeq_++;
    segment->Gen = Generation_;
    Pending_.push_back(std::move(segment));
    PrepCV_.notify_one();
  }

  if (FlushBulk_ && Candidate_ == Frontier_)
    FlushBulk_ = false;
}

void CBlockPipeline::publishReadyLocked()
{
  while (!Reorder_.empty() && Reorder_.begin()->first == PublishSeq_) {
    Ready_.push_back(std::move(Reorder_.begin()->second));
    Reorder_.erase(Reorder_.begin());
    PublishSeq_++;
    SerialCV_.notify_one();
  }
}

void CBlockPipeline::resetLocked(std::vector<std::unique_ptr<CSegment>> &dropped)
{
  Generation_++;
  // Dropped, not snapshotted: the tip that led here may be the rejected branch, and holding it
  // would make its work the bar every later offer has to clear. scheduleLocked picks up the
  // settled chain once the queues run out
  Candidate_ = nullptr;
  Frontier_ = nullptr;
  TailWaiting_ = false;
  Resetting_ = true;

  for (auto &segment: Pending_)
    dropped.push_back(std::move(segment));
  Pending_.clear();
  for (auto &entry: Reorder_)
    dropped.push_back(std::move(entry.second));
  Reorder_.clear();
  for (auto &segment: Ready_)
    dropped.push_back(std::move(segment));
  Ready_.clear();

  NextSeq_ = 0;
  PublishSeq_ = 0;
}

void CBlockPipeline::discard(CSegment &segment)
{
  for (CSegment::CObject &entry: segment.Objects) {
    if (entry.Object.get())
      entry.Object.get()->validationData().dropPairs();
  }
}

void CBlockPipeline::rescanCandidate()
{
  submit(bestReadyBlock(*BlockIndex_));
}

void CBlockPipeline::prepare()
{
  loguru::set_thread_name("prepare");

  for (;;) {
    std::unique_ptr<CSegment> segment;
    {
      std::unique_lock lock(Mutex_);
      PrepCV_.wait(lock, [this]() { return Stopped_ || !Pending_.empty(); });
      if (Pending_.empty())
        return;
      segment = std::move(Pending_.front());
      Pending_.pop_front();
      Preparing_++;
    }

    const bool ok = prepareSegment(*ChainParams_, *Storage_, Runner_, *segment, Params_.Prefetch);
    bool needRescan = false;
    std::vector<std::unique_ptr<CSegment>> dropped;
    {
      std::lock_guard lock(Mutex_);
      Preparing_--;
      if (segment->Gen != Generation_) {
        dropped.push_back(std::move(segment));
      } else if (ok) {
        Reorder_[segment->Seq] = std::move(segment);
        publishReadyLocked();
      } else {
        dropped.push_back(std::move(segment));
        resetLocked(dropped);
        needRescan = true;
      }
      scheduleLocked();
      DrainCV_.notify_all();
      SerialCV_.notify_all();
    }

    for (auto &entry: dropped)
      discard(*entry);
    if (needRescan)
      rescanCandidate();
  }
}

void CBlockPipeline::serial()
{
  loguru::set_thread_name("serial");

  for (;;) {
    std::unique_ptr<CSegment> segment;
    {
      std::unique_lock lock(Mutex_);
      SerialCV_.wait(lock, [this]() {
        return !Ready_.empty() ||
               (Stopped_ && Pending_.empty() && Preparing_ == 0 && Reorder_.empty());
      });
      if (Ready_.empty())
        return;
      segment = std::move(Ready_.front());
      Ready_.pop_front();
      SerialBusy_ = true;
      scheduleLocked();
      DrainCV_.notify_all();
    }

    bool needRescan = false;
    std::vector<std::unique_ptr<CSegment>> dropped;

    if (segment->CatchUp) {
      if (CatchUpSink_)
        CatchUpSink_(std::move(segment));
    } else {
      size_t failedAt = 0;
      const bool ok = connectSegment(*BlockIndex_, *ChainParams_, *Storage_, Runner_, *segment, &failedAt);
      Storage_->wakeUp();

      if (ok) {
        if (Callback_) {
          std::vector<BC::Common::BlockIndex*> relay;
          for (const CSegment::CObject &entry: segment->Objects) {
            if (entry.Relay)
              relay.push_back(entry.Index);
          }
          if (!relay.empty())
            Callback_(relay);
        }
      } else {
        BC::Common::BlockIndex *bad = segment->Objects[failedAt].Index;
        LOG_F(ERROR,
              "Block %s (%u) rejected by the chain state",
              bad->Header.GetHash().getHexLE().c_str(),
              bad->Height);
        bad->IndexState.store(BSInvalid);
        dropped.push_back(std::move(segment));

        std::lock_guard lock(Mutex_);
        resetLocked(dropped);
        needRescan = true;
      }
    }

    for (auto &entry: dropped)
      discard(*entry);

    {
      std::lock_guard lock(Mutex_);
      SerialBusy_ = false;
      scheduleLocked();
      DrainCV_.notify_all();
      SerialCV_.notify_all();
    }
    if (needRescan)
      rescanCandidate();
  }
}
