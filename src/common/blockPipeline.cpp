// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockPipeline.h"
#include "blockDataBase.h"
#include "db/storage.h"
#include "loguru.hpp"

#include <algorithm>
#include <chrono>

bool CBlockPipeline::start(BlockInMemoryIndex &blockIndex,
                           BC::Common::ChainParams &chainParams,
                           BC::DB::Storage &storage,
                           const CParams &params)
{
  BlockIndex_ = &blockIndex;
  ChainParams_ = &chainParams;
  Storage_ = &storage;
  Params_ = params;

  unsigned threadsNum = Params_.WaveThreads;
  if (!threadsNum)
    threadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;

  Frontier_ = blockIndex.best();
  blockIndex.candidateTracker().setListener([this]() { wakeSelector(); });
  blockIndex.candidateTracker().update(blockIndex.best());

  LOG_F(INFO,
        "Pull pipeline: %u wave threads, %zu preparation lanes, segment %.1lfMb (%zu blocks), ready queue %zu, read ahead %.1lfMb",
        threadsNum,
        Params_.PrepLanes,
        Params_.SegmentSizeLimit / 1048576.0,
        Params_.SegmentBlocksLimit,
        Params_.ReadyQueueDepth,
        Params_.RawSizeLimit / 1048576.0);

  // One helper less than the pool width: the wave owner takes a share itself
  Runner_.start(threadsNum > 1 ? threadsNum - 1 : 0);
  SerialThread_ = std::thread([this]() { serial(); });
  for (size_t i = 0; i < std::max<size_t>(Params_.PrepLanes, 1); i++)
    PrepThreads_.emplace_back([this]() { prepare(); });
  SelectorThread_ = std::thread([this]() { selector(); });
  Started_ = true;
  return true;
}

void CBlockPipeline::stop()
{
  if (!Started_)
    return;

  {
    std::lock_guard lock(Mutex_);
    Stopped_ = true;
  }
  SelectorCV_.notify_all();
  PrepCV_.notify_all();
  SerialCV_.notify_all();
  DrainCV_.notify_all();

  SelectorThread_.join();
  for (auto &thread: PrepThreads_)
    thread.join();
  PrepThreads_.clear();
  SerialThread_.join();
  Runner_.stop();
  Started_ = false;
}

void CBlockPipeline::wakeSelector()
{
  // The lock is what makes the wakeup safe against a selector about to sleep; taking it for
  // every attached block would serialize the readers, so the flag filters the common case
  if (SelectorWaiting_.load(std::memory_order_acquire)) {
    std::lock_guard lock(Mutex_);
    SelectorCV_.notify_one();
  }
}

CBlockPipeline::EResult CBlockPipeline::attachFromFile(const intrusive_ptr<CRawBlockData> &buffer,
                                                       uint32_t offset,
                                                       uint32_t size,
                                                       uint32_t fileNo,
                                                       uint32_t fileOffset)
{
  // Header is all that is parsed here: it names the index the data is attached to
  BC::Proto::BlockHeader header;
  {
    xmstream stream(static_cast<uint8_t*>(buffer.get()->data()) + offset, size);
    if (!BC::unserializeAndCheck(stream, header))
      return Invalid;
  }

  bool checkWork = true;
  BC::Common::BlockIndex *index = attachBlockData(*BlockIndex_, *ChainParams_, header, header.GetHash(), &checkWork);
  // Already have this block: the block files hold it twice, or it came from the network first
  if (!index)
    return NotStaged;

  std::unique_ptr<CBlockRawData> raw(new CBlockRawData);
  raw->Buffer = buffer;
  raw->Offset = offset;
  raw->Size = size;
  raw->FileNo = fileNo;
  raw->FileOffset = fileOffset;
  raw->CheckWork = checkWork;

  Counters_.RawBytes.fetch_add(size, std::memory_order_relaxed);
  index->Raw.store(raw.release(), std::memory_order_release);
  FileBlocks_.push_back(index);

  // Only now may the selector see the block: the data must be there before the topology says
  // it is
  ArrivalGen_.fetch_add(1, std::memory_order_release);
  BlockIndex_->candidateTracker().update(index);
  wakeSelector();
  return Staged;
}

CBlockPipeline::EResult CBlockPipeline::attachFromNetwork(void *data,
                                                          uint32_t size,
                                                          uint32_t memorySize,
                                                          const BC::Proto::BlockHeader &header,
                                                          const BC::Proto::BlockHashTy &hash,
                                                          bool relay,
                                                          BC::Common::BlockIndex **attached)
{
  bool checkWork = true;
  BC::Common::BlockIndex *index = attachBlockData(*BlockIndex_, *ChainParams_, header, hash, &checkWork);
  if (!index)
    return NotStaged;
  *attached = index;

  std::unique_ptr<CBlockRawData> raw(new CBlockRawData);
  // Accounted in the block cache: the getdata scheduler throttles on it
  raw->Buffer = intrusive_ptr<CRawBlockData>(new CRawBlockData(data, memorySize, &Storage_->cache()));
  raw->Size = size;
  raw->Exclusive = true;
  raw->CheckWork = checkWork;
  raw->Relay = relay;

  Counters_.RawBytes.fetch_add(size, std::memory_order_relaxed);
  index->Raw.store(raw.release(), std::memory_order_release);

  ArrivalGen_.fetch_add(1, std::memory_order_release);
  BlockIndex_->candidateTracker().update(index);
  wakeSelector();
  return Staged;
}

bool CBlockPipeline::throttled() const
{
  // What the storage thread has to write and has not written yet: the archive is ten times
  // slower than the chain advance, and without this the whole block file set ended up parsed
  // in memory (6.9 GB on the LTC stand). Nothing to escape here - the storage thread drains it
  if (Storage_->queuedMemory() >= Params_.StorageBacklogLimit)
    return true;

  // Raw data waiting for preparation. A block whose predecessor is in an unread file waits for
  // the reader, so a reader stopped while the pipeline idles would stall the chain
  if (Counters_.RawBytes.load(std::memory_order_relaxed) >= Params_.RawSizeLimit)
    return !starving();

  return false;
}

bool CBlockPipeline::starving() const
{
  std::lock_guard lock(Mutex_);
  return drainedLocked();
}

// A block file buffer is shared by every block of it, so one block left raw (a fork block never
// gets prepared) keeps all 128 Mb alive. After a few files of grace the rest gets private copies
void CBlockPipeline::rotateFile()
{
  std::lock_guard lock(Mutex_);
  RecentFiles_.emplace_back(std::move(FileBlocks_));
  FileBlocks_.clear();
  while (RecentFiles_.size() > FileGrace) {
    Stragglers_.insert(Stragglers_.end(), RecentFiles_.front().begin(), RecentFiles_.front().end());
    RecentFiles_.pop_front();
  }
  SelectorCV_.notify_one();
}

void CBlockPipeline::releaseStragglers()
{
  std::vector<BC::Common::BlockIndex*> stragglers;
  {
    std::lock_guard lock(Mutex_);
    if (Stragglers_.empty())
      return;
    stragglers.swap(Stragglers_);
  }

  for (BC::Common::BlockIndex *index: stragglers) {
    // In a segment: the preparation owns its data, and it is about to be parsed anyway
    if (index->Prepared.load(std::memory_order_relaxed))
      continue;

    std::unique_ptr<CBlockRawData> raw(index->Raw.exchange(nullptr, std::memory_order_acq_rel));
    if (!raw)
      continue;

    void *copy = operator new(raw->Size);
    memcpy(copy, raw->data(), raw->Size);
    raw->Buffer = intrusive_ptr<CRawBlockData>(new CRawBlockData(copy, raw->Size, nullptr));
    raw->Offset = 0;
    index->Raw.store(raw.release(), std::memory_order_release);
  }
}

void CBlockPipeline::waitDrained()
{
  std::unique_lock lock(Mutex_);
  DrainCV_.wait(lock, [this]() { return Stopped_ || drainedLocked(); });
}

// Data of a block is here, or can be brought back from disk
static bool haveBlockData(BC::Common::BlockIndex *index)
{
  return index->Raw.load(std::memory_order_acquire) != nullptr ||
         index->Serialized.get() != nullptr ||
         (index->blockStored() && index->indexStored());
}

// Path of the best candidate down to the preparation frontier, cut to the segment limits. The
// index is the whole topology: what the arrival order of the data was does not matter here
std::unique_ptr<CSegment> CBlockPipeline::bite(BC::Common::BlockIndex *frontier)
{
  CCandidateTracker &tracker = BlockIndex_->candidateTracker();

  for (unsigned attempt = 0; attempt < 64; attempt++) {
    BC::Common::BlockIndex *candidate = tracker.best();
    if (!candidate || candidate == frontier)
      return nullptr;

    // Path from the candidate down. A block with no data takes everything above it: that part
    // cannot connect before the hole is filled
    std::vector<BC::Common::BlockIndex*> path;
    BC::Common::BlockIndex *anchor = nullptr;
    BC::Common::BlockIndex *invalid = nullptr;

    for (BC::Common::BlockIndex *index = candidate; index; index = index->Prev) {
      if (index == frontier || index->Prepared.load(std::memory_order_relaxed)) {
        anchor = index;
        break;
      }
      if (index->IndexState.load(std::memory_order_relaxed) == BSInvalid) {
        invalid = index;
        break;
      }

      if (haveBlockData(index))
        path.push_back(index);
      else
        path.clear();
    }

    if (invalid) {
      // Everything walked sits above a rejected block, so it is rejected too - otherwise the
      // tracker hands the same dead tip back forever
      size_t dropped = 0;
      for (BC::Common::BlockIndex *index = candidate; index != invalid; index = index->Prev) {
        index->IndexState.store(BSInvalid);
        dropped++;
      }
      LOG_F(WARNING,
            "Pull pipeline: %zu blocks above the rejected block %s dropped",
            dropped,
            invalid->Header.GetHash().getHexLE().c_str());
      rescanCandidate();
      continue;
    }

    if (!anchor || path.empty())
      return nullptr;

    // Collected top down
    std::reverse(path.begin(), path.end());

    auto segment = std::make_unique<CSegment>();
    segment->Anchor = anchor;
    segment->Reanchor = (anchor != frontier);
    for (BC::Common::BlockIndex *index: path) {
      uint32_t size = 0;
      if (CBlockRawData *raw = index->Raw.load(std::memory_order_acquire))
        size = raw->Size;
      else
        size = index->SerializedBlockSize != std::numeric_limits<uint32_t>::max() ? index->SerializedBlockSize : 0;

      if (!segment->Objects.empty() &&
          (segment->RawBytes + size > Params_.SegmentSizeLimit ||
           segment->Objects.size() >= Params_.SegmentBlocksLimit))
        break;

      segment->Objects.emplace_back();
      segment->Objects.back().Index = index;
      segment->RawBytes += size;
      index->Prepared.store(true, std::memory_order_relaxed);
    }

    return segment;
  }

  LOG_F(ERROR, "Pull pipeline: can't find a candidate to connect");
  return nullptr;
}

// The tracker only ever hears about blocks as they arrive; after a rejection the tip it holds
// may be gone, and the alternative is somewhere in the index
void CBlockPipeline::rescanCandidate()
{
  BC::Common::BlockIndex *best = BlockIndex_->best();
  for (const auto &entry: BlockIndex_->blockIndex()) {
    BC::Common::BlockIndex *index = entry.second;
    if (index->Height == std::numeric_limits<uint32_t>::max() ||
        index->IndexState.load(std::memory_order_relaxed) == BSInvalid)
      continue;
    if (!index->Prepared.load(std::memory_order_relaxed) && !haveBlockData(index))
      continue;
    if (!best || index->ChainWork > best->ChainWork)
      best = index;
  }

  BlockIndex_->candidateTracker().reset(best);
}

void CBlockPipeline::discard(CSegment &segment)
{
  for (CSegment::CObject &object: segment.Objects) {
    if (object.Object.get())
      object.Object.get()->validationData().dropPairs();
    object.Index->Prepared.store(false, std::memory_order_relaxed);
  }
}

bool CBlockPipeline::pipelineIdleLocked() const
{
  return Pending_.empty() && Reorder_.empty() && Ready_.empty() && !PrepBusy_ && !SerialBusy_;
}

bool CBlockPipeline::drainedLocked() const
{
  return pipelineIdleLocked() && !SelectorBusy_ &&
         IdleAt_ == ArrivalGen_.load(std::memory_order_acquire);
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

// A segment that falls apart takes the ones bitten after it with it: they continue a chain that
// is not going to happen. Whoever is preparing one right now sees the generation move and drops
// its result, so the sequence can start over from here
void CBlockPipeline::resetLocked(BC::Common::BlockIndex *frontier, std::vector<std::unique_ptr<CSegment>> &dropped)
{
  ResetGen_++;
  Frontier_ = frontier;

  for (auto &entry: Pending_) {
    InFlight_--;
    dropped.push_back(std::move(entry));
  }
  Pending_.clear();
  for (auto &entry: Reorder_)
    dropped.push_back(std::move(entry.second));
  Reorder_.clear();
  for (auto &entry: Ready_)
    dropped.push_back(std::move(entry));
  Ready_.clear();

  NextSeq_ = 0;
  PublishSeq_ = 0;
}

void CBlockPipeline::selector()
{
  loguru::set_thread_name("selector");

  BC::Common::BlockIndex *frontier = nullptr;
  uint64_t gen = std::numeric_limits<uint64_t>::max();

  for (;;) {
    uint64_t arrival;
    {
      std::unique_lock lock(Mutex_);
      for (;;) {
        if (Stopped_)
          return;
        if (InFlight_ + Ready_.size() < Params_.ReadyQueueDepth + Params_.PrepLanes &&
            IdleAt_ != ArrivalGen_.load(std::memory_order_acquire))
          break;

        SelectorWaiting_.store(true, std::memory_order_release);
        DrainCV_.notify_all();
        SelectorCV_.wait(lock);
        SelectorWaiting_.store(false, std::memory_order_release);
      }

      if (gen != ResetGen_) {
        gen = ResetGen_;
        frontier = Frontier_;
      }
      arrival = ArrivalGen_.load(std::memory_order_acquire);
      SelectorBusy_ = true;
    }
    releaseStragglers();

    std::unique_ptr<CSegment> segment = bite(frontier);

    // The candidate is not on the chain the queue was built for: what is in flight would be
    // connected only to be disconnected again. Let the pipeline run out first, then take the
    // fork from the settled chain
    if (segment && segment->Reanchor) {
      std::unique_lock lock(Mutex_);
      if (!pipelineIdleLocked()) {
        SelectorBusy_ = false;
        SelectorWaiting_.store(true, std::memory_order_release);
        DrainCV_.notify_all();
        lock.unlock();
        discard(*segment);
        segment.reset();
        lock.lock();
        DrainCV_.wait(lock, [this]() { return Stopped_ || pipelineIdleLocked(); });
        SelectorWaiting_.store(false, std::memory_order_release);
        gen = ResetGen_;
        frontier = Frontier_;
        continue;
      }
    }

    std::lock_guard lock(Mutex_);
    if (segment && gen == ResetGen_) {
      frontier = segment->Objects.back().Index;
      segment->Seq = NextSeq_++;
      segment->Gen = gen;
      Pending_.push_back(std::move(segment));
      InFlight_++;
      PrepCV_.notify_one();
    } else if (segment) {
      // The chain moved while this one was being bitten
      discard(*segment);
      segment.reset();
    } else if (gen == ResetGen_) {
      IdleAt_ = arrival;
    }

    SelectorBusy_ = false;
    DrainCV_.notify_all();
  }
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
      PrepBusy_++;
    }

    bool ok = prepareSegment(*BlockIndex_, *ChainParams_, *Storage_, Runner_, *segment, Counters_, Params_.Prefetch);

    std::vector<std::unique_ptr<CSegment>> dropped;
    {
      std::lock_guard lock(Mutex_);
      PrepBusy_--;
      InFlight_--;

      if (segment->Gen != ResetGen_) {
        // Prepared against a chain that is not there any more
        dropped.push_back(std::move(segment));
      } else if (ok) {
        Reorder_[segment->Seq] = std::move(segment);
        publishReadyLocked();
      } else {
        // Something in the segment was rejected by its own checks; the block is BSInvalid now,
        // so the next bite stops before it and the good part comes back as a segment of its own
        BC::Common::BlockIndex *anchor = segment->Anchor;
        dropped.push_back(std::move(segment));
        resetLocked(anchor, dropped);
      }
    }

    for (auto &entry: dropped)
      discard(*entry);
    dropped.clear();

    {
      std::lock_guard lock(Mutex_);
      DrainCV_.notify_all();
    }
    SelectorCV_.notify_one();
  }
}

void CBlockPipeline::serial()
{
  loguru::set_thread_name("serial");

  for (;;) {
    std::unique_ptr<CSegment> segment;
    {
      std::unique_lock lock(Mutex_);
      SerialCV_.wait(lock, [this]() { return Stopped_ || !Ready_.empty(); });
      if (Ready_.empty())
        return;

      segment = std::move(Ready_.front());
      Ready_.pop_front();
      SerialBusy_ = true;
    }
    SelectorCV_.notify_one();

    size_t failedAt = 0;
    bool ok = connectSegment(*BlockIndex_, *ChainParams_, *Storage_, Runner_, *segment, &failedAt);

    // Whatever the advance did, it queued it here: a disconnect leaves no connected block
    // behind, and a task nobody wakes the storage thread for waits for the next one forever
    Storage_->wakeUp();

    std::vector<std::unique_ptr<CSegment>> dropped;
    if (ok) {
      if (Callback_) {
        std::vector<BC::Common::BlockIndex*> relay;
        for (const CSegment::CObject &object: segment->Objects) {
          if (object.Relay)
            relay.push_back(object.Index);
        }
        if (!relay.empty())
          Callback_(relay);
      }
      segment.reset();
    } else {
      BC::Common::BlockIndex *bad = segment->Objects[failedAt].Index;
      LOG_F(ERROR,
            "Block %s (%u) rejected by the chain state",
            bad->Header.GetHash().getHexLE().c_str(),
            bad->Height);
      bad->IndexState.store(BSInvalid);

      dropped.push_back(std::move(segment));
      {
        std::lock_guard lock(Mutex_);
        resetLocked(BlockIndex_->best(), dropped);
      }

      for (auto &entry: dropped)
        discard(*entry);
      dropped.clear();
      rescanCandidate();
    }

    {
      std::lock_guard lock(Mutex_);
      SerialBusy_ = false;
      DrainCV_.notify_all();
    }
    SelectorCV_.notify_one();
  }
}


