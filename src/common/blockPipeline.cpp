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
#include <cassert>

void CBlockPipeline::CWorker::give(CEvent *event)
{
  assert(Job.load(std::memory_order_relaxed) == nullptr);
  Job.store(event, std::memory_order_release);
  Job.notify_one();
}

CBlockPipeline::CEvent *CBlockPipeline::CWorker::take()
{
  Job.wait(nullptr, std::memory_order_acquire);
  return Job.exchange(nullptr, std::memory_order_acquire);
}

bool CBlockPipeline::start(BlockInMemoryIndex &blockIndex,
                           BC::Common::ChainParams &chainParams,
                           BC::DB::Storage &storage,
                           const CParams &params)
{
  BlockIndex_ = &blockIndex;
  ChainParams_ = &chainParams;
  Storage_ = &storage;
  Params_ = params;
  Frontier_ = Candidate_ = blockIndex.best();
  const size_t lanes = std::max<size_t>(Params_.PrepLanes, 1);
  WindowLimit_ = std::max<size_t>(Params_.ReadyQueueDepth + lanes, 1);

  unsigned threadsNum = Params_.WaveThreads;
  if (!threadsNum)
    threadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;

  LOG_F(INFO,
        "Block pipeline: %u wave threads, %zu preparation lanes, segment %.1lfMb (%zu blocks), bulk floor %.1lfMb (%zu blocks), ready queue %zu, prepared %.1lfMb",
        threadsNum, lanes, Params_.SegmentSizeLimit / 1048576.0,
        Params_.SegmentBlocksLimit, Params_.BiteFloorSize / 1048576.0,
        Params_.BiteFloorBlocks, Params_.ReadyQueueDepth, Params_.PreparedSizeLimit / 1048576.0);

  Runner_.start(threadsNum > 1 ? threadsNum - 1 : 0);
  SerialWorker_.Thread = std::thread([this]() { worker(SerialWorker_, false); });
  for (size_t i = 0; i < lanes; i++) {
    CWorker &lane = PrepWorkers_.emplace_back();
    lane.Thread = std::thread([this, &lane]() { worker(lane, true); });
  }

  Started_ = true;
  blockIndex.setReadyPipeline(this);
  return true;
}

void CBlockPipeline::stop()
{
  if (!Started_)
    return;

  BlockIndex_->setReadyPipeline(nullptr);
  request(new CEvent(CEvent::EType::Stop));
  for (CWorker &lane: PrepWorkers_)
    lane.Thread.join();
  SerialWorker_.Thread.join();
  Runner_.stop();
  Started_ = false;
}

// FIFO publication and exclusive ownership are provided by CInbox. A worker returns the same
// event it received, so take/save-next must happen before handle can give it away again.
void CBlockPipeline::post(CEvent *event)
{
  Combiner_.push(event);
  if (!Combiner_.tryAcquire())
    return;

  CReplies replies;
  do {
    CEvent *list = Combiner_.take();
    while (list) {
      CEvent *next = list->Next;
      handle(std::unique_ptr<CEvent>(list), replies);
      list = next;
    }
    schedule(replies);
  } while (Combiner_.release([]() { return false; }));

  // A stop/drain caller may immediately destroy the pipeline or change a callback. Release
  // the role first, and do not access pipeline state after signalling these shared replies.
  for (auto &reply: replies)
    reply->finish();
}

void CBlockPipeline::request(CEvent *event)
{
  auto reply = std::make_shared<CEvent::CReply>();
  event->Reply = reply;
  post(event);
  reply->wait();
}

void CBlockPipeline::submit(BC::Common::BlockIndex *candidate)
{
  if (!candidate || !candidate->ready())
    return;
  auto *event = new CEvent(CEvent::EType::Candidate);
  event->Index = candidate;
  post(event);
}

void CBlockPipeline::feed(std::unique_ptr<CSegment> segment)
{
  if (!segment || segment->Objects.empty())
    return;
  auto *event = new CEvent(CEvent::EType::Feed);
  event->Segment = std::move(segment);
  request(event);
}

void CBlockPipeline::setBulkFeed(bool bulk)
{
  auto *event = new CEvent(CEvent::EType::Bulk);
  event->Bulk = bulk;
  post(event);
}

void CBlockPipeline::waitDrained()
{
  request(new CEvent(CEvent::EType::Drain));
}

bool CBlockPipeline::throttled() const
{
  return Storage_->utxodb().pipelineFull() ||
         Storage_->archive().pipelineFull() ||
         Storage_->cache().size() >= Params_.PreparedSizeLimit;
}

void CBlockPipeline::admit(BC::Common::BlockIndex *candidate)
{
  if (!candidate || !candidate->ready() ||
      (Candidate_ && !(candidate->ChainWork > Candidate_->ChainWork)))
    return;

  // The previously admitted path was checked already. A normal extension visits one block;
  // a fork visits only its new branch. Readiness publishes immutable Prev and ChainWork.
  for (auto *index = candidate;
       index && index != Candidate_ && !index->hasFlags(BFOnChain);
       index = index->Prev) {
    if (index->hasFlags(BFInvalid))
      return;
  }
  Candidate_ = candidate;
  PathBase_ = nullptr;
}

void CBlockPipeline::handle(std::unique_ptr<CEvent> event, CReplies &replies)
{
  switch (event->Type) {
    case CEvent::EType::Candidate:
      if (!Stopped_)
        admit(event->Index);
      break;
    case CEvent::EType::Bulk:
      if (!Stopped_ && BulkFeed_ != event->Bulk) {
        BulkFeed_ = event->Bulk;
        if (!BulkFeed_) {
          FlushBulk_ = true;
          TailWaiting_ = false;
        }
      }
      break;
    case CEvent::EType::Feed:
      if (Stopped_)
        replies.push_back(std::move(event->Reply));
      else
        FeedWaiters_.push_back(std::move(event));
      break;
    case CEvent::EType::Drain:
      DrainWaiters_.push_back(std::move(event->Reply));
      break;
    case CEvent::EType::Stop:
      Stopped_ = true;
      FlushBulk_ = TailWaiting_ = false;
      StopReply_ = std::move(event->Reply);
      for (auto &waiting: FeedWaiters_)
        replies.push_back(std::move(waiting->Reply));
      FeedWaiters_.clear();
      break;
    case CEvent::EType::Prepared: {
      PrepWorkers_[event->Lane].Busy = false;
      Preparing_--;
      CSegment &segment = *event->Segment;
      if (segment.Gen != Generation_) {
        discard(segment);
      } else if (!event->Ok || segment.Objects.back().Index != event->Index) {
        // A truncated segment also invalidates the frontier used to cut later segments.
        // The valid prefix is rediscovered once all old workers have relinquished its objects.
        discard(segment);
        reset();
      } else {
        assert(!Window_.empty() && segment.Seq >= Window_.front().Seq);
        CSlot &slot = Window_[segment.Seq - Window_.front().Seq];
        assert(slot.State == CSlot::EState::Preparing && !slot.Task);
        slot.State = CSlot::EState::Ready;
        slot.Task = std::move(event);
      }
      break;
    }
    case CEvent::EType::Connected:
      SerialWorker_.Busy = false;
      if (!event->Ok) {
        auto *bad = event->Segment->Objects[event->FailedAt].Index;
        LOG_F(ERROR, "Block %s (%u) rejected by the chain state",
              bad->Header.GetHash().getHexLE().c_str(), bad->Height);
        bad->Flags.fetch_or(BFInvalid, std::memory_order_relaxed);
        discard(*event->Segment);
        reset();
      }
      break;
  }
}

bool CBlockPipeline::idle() const
{
  return Window_.empty() && Preparing_ == 0 && !SerialWorker_.Busy;
}

void CBlockPipeline::append(std::unique_ptr<CEvent> event, CSlot::EState state)
{
  event->Segment->Seq = NextSeq_++;
  event->Segment->Gen = Generation_;
  Window_.push_back(CSlot{event->Segment->Seq, std::move(event), state});
}

std::unique_ptr<CSegment> CBlockPipeline::makeSegment()
{
  if (!Candidate_ || !Frontier_ || Candidate_ == Frontier_)
    return nullptr;

  if (!PathBase_) {
    Path_.clear();
    PathBytes_ = 0;
    auto *index = Candidate_;
    while (index && index != Frontier_ && !index->hasFlags(BFOnChain)) {
      if (index->hasFlags(BFInvalid)) {
        reset();
        return nullptr;
      }
      Path_.push_back(index);
      PathBytes_ += index->SerializedBlockSize;
      index = index->Prev;
    }
    PathBase_ = index;
  }

  // Switching branches waits for both stages to finish using the previous branch's objects.
  if (!PathBase_ || Path_.empty() || (PathBase_ != Frontier_ && !idle()))
    return nullptr;

  if (BulkFeed_ && !FlushBulk_ &&
      Path_.size() < Params_.BiteFloorBlocks && PathBytes_ < Params_.BiteFloorSize) {
    TailWaiting_ = true;
    return nullptr;
  }

  const bool bulk = BulkFeed_ || FlushBulk_;
  const size_t limit = bulk ? std::max<size_t>(Params_.SegmentBlocksLimit, 1) : 1;
  auto segment = std::make_unique<CSegment>();
  while (!Path_.empty() && segment->Objects.size() < limit) {
    auto *index = Path_.back();
    const size_t size = index->SerializedBlockSize;
    if (!segment->Objects.empty() && bulk && segment->Size + size > Params_.SegmentSizeLimit)
      break;
    segment->Objects.emplace_back().Index = index;
    segment->Size += size;
    PathBytes_ -= size;
    Path_.pop_back();
  }
  Frontier_ = PathBase_ = segment->Objects.back().Index;
  TailWaiting_ = false;
  if (Frontier_ == Candidate_)
    FlushBulk_ = false;
  return segment;
}

void CBlockPipeline::schedule(CReplies &replies)
{
  for (;;) {
    if (Resetting_) {
      if (Preparing_ || SerialWorker_.Busy)
        break;
      Frontier_ = BlockIndex_->best();
      Candidate_ = Stopped_ ? Frontier_ : bestReadyBlock(*BlockIndex_);
      PathBase_ = nullptr;
      Resetting_ = false;
    }

    // Keep the same bound as before: WindowLimit_ queued segments plus one connecting.
    if (!SerialWorker_.Busy && !Window_.empty() && Window_.front().State == CSlot::EState::Ready) {
      auto event = std::move(Window_.front().Task);
      Window_.pop_front();
      event->Type = CEvent::EType::Connected;
      SerialWorker_.Busy = true;
      SerialWorker_.give(event.release());
    }

    while (!Stopped_ && Window_.size() < WindowLimit_) {
      if (!FeedWaiters_.empty()) {
        auto event = std::move(FeedWaiters_.front());
        FeedWaiters_.pop_front();
        replies.push_back(std::move(event->Reply));
        event->Segment->CatchUp = true;
        append(std::move(event), CSlot::EState::Ready);
      } else {
        auto segment = makeSegment();
        if (!segment)
          break;
        auto event = std::make_unique<CEvent>(CEvent::EType::Prepared);
        event->Index = segment->Objects.back().Index;
        event->Segment = std::move(segment);
        append(std::move(event), CSlot::EState::Pending);
      }
    }
    if (Resetting_)
      continue;

    auto slot = Window_.begin();
    for (size_t i = 0; i < PrepWorkers_.size(); i++) {
      CWorker &lane = PrepWorkers_[i];
      if (lane.Busy)
        continue;
      while (slot != Window_.end() && slot->State != CSlot::EState::Pending)
        ++slot;
      if (slot == Window_.end())
        break;
      slot->Task->Lane = i;
      slot->State = CSlot::EState::Preparing;
      lane.Busy = true;
      Preparing_++;
      lane.give(slot->Task.release());
      ++slot;
    }

    // A catch-up feed may have made the front ready just now; dispatch it and refill once.
    if (SerialWorker_.Busy || Window_.empty() || Window_.front().State != CSlot::EState::Ready)
      break;
  }

  if (FlushBulk_ && Candidate_ == Frontier_)
    FlushBulk_ = false;
  if (!idle() || !FeedWaiters_.empty() || TailWaiting_ || FlushBulk_ || Resetting_)
    return;

  for (auto &reply: DrainWaiters_)
    replies.push_back(std::move(reply));
  DrainWaiters_.clear();
  if (StopReply_) {
    for (CWorker &lane: PrepWorkers_)
      lane.give(&StopEvent_);
    SerialWorker_.give(&StopEvent_);
    replies.push_back(std::move(StopReply_));
  }
}

void CBlockPipeline::reset()
{
  Generation_++;
  Candidate_ = Frontier_ = PathBase_ = nullptr;
  TailWaiting_ = false;
  Resetting_ = true;
  for (auto &slot: Window_) {
    if (slot.Task)
      discard(*slot.Task->Segment);
  }
  Window_.clear();
  // Running jobs retain their events. Their generation makes late results discard-only;
  // no new preparation starts until all of them, including connect, have returned.
}

void CBlockPipeline::discard(CSegment &segment)
{
  for (auto &entry: segment.Objects) {
    if (entry.Object.get())
      entry.Object.get()->validationData().dropPairs();
  }
}

void CBlockPipeline::worker(CWorker &lane, bool preparation)
{
  loguru::set_thread_name(preparation ? "prepare" : "serial");
  for (;;) {
    CEvent *event = lane.take();
    if (event == &StopEvent_)
      return;

    if (preparation) {
      event->Ok = prepareSegment(*ChainParams_, *Storage_, Runner_, *event->Segment, Params_.Prefetch);
    } else if (event->Segment->CatchUp) {
      if (CatchUpSink_)
        CatchUpSink_(std::move(event->Segment));
    } else {
      event->Ok = connectSegment(*BlockIndex_, *ChainParams_, *Storage_, Runner_,
                                 *event->Segment, &event->FailedAt);
      Storage_->wakeUp();
      if (event->Ok && Callback_) {
        std::vector<BC::Common::BlockIndex*> relay;
        for (const auto &entry: event->Segment->Objects) {
          if (entry.Relay)
            relay.push_back(entry.Index);
        }
        if (!relay.empty())
          Callback_(relay);
      }
    }
    post(event);
  }
}
