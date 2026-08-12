// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockSource.h"
#include "common/thread.h"


intrusive_ptr<BlockSource> BlockSource::getOrCreateBlockSource(atomic_intrusive_ptr<BlockSource> &blockSource,
                                                               unsigned threadsNum,
                                                               bool createNew,
                                                               bool &newSourceCreated)
{
  newSourceCreated = false;
  // Insertion point follows the walk: a new source goes where the chain ends, not into the starting slot.
  // The starting slot is advanced past its finished prefix by a best-effort CAS (helping pattern) once the
  // walk finds the active source. The slot is never set to null: a null store is the only way a concurrent
  // tail append can become unreachable, so releasing a source is reduced to marking it finished.
  intrusive_ptr<BlockSource> first;
  intrusive_ptr<BlockSource> parent;
  atomic_intrusive_ptr<BlockSource> *slot = &blockSource;
  for (;;) {
    intrusive_ptr<BlockSource> current(*slot);
    if (current.get() == nullptr) {
      if (!createNew)
        return current;

      BlockSource *newValue = new BlockSource(threadsNum);
      if (slot->compare_and_exchange(nullptr, newValue)) {
        newSourceCreated = true;
        intrusive_ptr<BlockSource> result(newValue);
        // 'first' and 'result' keep both CAS arguments alive: no ABA on the expected pointer
        if (first.get() != nullptr)
          blockSource.compare_and_exchange(first.get(), newValue);
        return result;
      }

      // Lost the race; compare_and_exchange already deleted newValue. Re-read the filled slot.
      continue;
    }

    if (!current.get()->DownloadingFinished_) {
      if (first.get() != nullptr)
        blockSource.compare_and_exchange(first.get(), current.get());
      return current;
    }

    if (slot == &blockSource)
      first = current;

    // Keeps the node alive: slot points into its Next_
    parent = current;
    slot = &parent.get()->Next_;
  }
}

void BlockSource::processTask(Task *task)
{
  if (task->Type == Task::Batch && !task->Indexes.empty()) {
    BC::Common::BlockIndex *first = task->Indexes.front();
    if ((!LastKnownIndex_ && first->Prev && first->Prev->OnChain) || LastKnownIndex_ == task->Prev) {
      for (auto index: task->Indexes)
        DownloadQueue_.push(index);
      LastKnownIndex_ = task->Indexes.back();

      decltype(EnqueuedTasks_)::iterator I;
      while ( (I = EnqueuedTasks_.find(LastKnownIndex_)) != EnqueuedTasks_.end()) {
        std::vector<BC::Common::BlockIndex*> &indexes = I->second;
        for (auto index: indexes)
          DownloadQueue_.push(index);
        LastKnownIndex_ = indexes.back();
        EnqueuedTasks_.erase(I);
      }
    } else {
      EnqueuedTasks_.emplace(task->Prev, std::move(task->Indexes));
    }
  } else if (task->Type == Task::LastPortion) {
    HeadersLastPortion_ = true;
  }

  // Headers are over when the marker is processed and nothing is left in flight. Only the combiner
  // decrements, so whoever reaches zero sets the flag: the marker or a batch that overtook it
  if (task->Counted)
    HeadersInFlight_.fetch_sub(1);
  if (HeadersLastPortion_ && HeadersInFlight_.load() == 0)
    HeadersFinished_ = true;
}

void BlockSource::processTask(TaskHP *task)
{
  if (task->TaskType == TaskHP::ProcessStalledBlocks && HighPriorityDownloadQueue_.empty()) {
    // Anchor: max of the dequeue slots and the received frontier. Slots alone fail: a retry
    // dequeue drags them below the holes, and a fresh source after a restart has none at all
    BC::Common::BlockIndex *index = task->Frontier;
    for (unsigned i = 0; i < ThreadsNum_; i++) {
      if (LastDequeued_[i] && (!index || LastDequeued_[i]->knownHeight() > index->knownHeight()))
        index = LastDequeued_[i];
    }

    // Collect stalled blocks: anchor down to first on-chain block. A block never asked for is
    // stalled too - its queue entry died with the old source, nobody else will ask
    auto now = std::chrono::steady_clock::now();
    std::vector<BC::Common::BlockIndex*> stalledBlocks;
    while (index && !index->OnChain) {
      if (!haveBlockData(index->IndexState) &&
          (index->DownloadingStartTime == std::chrono::time_point<std::chrono::steady_clock>::max() ||
           std::chrono::duration_cast<std::chrono::seconds>(now-index->DownloadingStartTime).count() >= 8))
        stalledBlocks.push_back(index);
      index = index->Prev;
    }

    std::reverse(stalledBlocks.begin(), stalledBlocks.end());
    if (!stalledBlocks.empty())
      LOG_F(INFO, "Retry download %zu blocks in range %s(%u): %s(%u)",
            stalledBlocks.size(),
            stalledBlocks.front()->Header.GetHash().getHexLE().c_str(),
            stalledBlocks.front()->knownHeight(),
            stalledBlocks.back()->Header.GetHash().getHexLE().c_str(),
            stalledBlocks.back()->knownHeight());

    for (auto index: stalledBlocks)
      HighPriorityDownloadQueue_.push(index);
  }
}

void BlockSource::setHeadersDownloadingFinished(bool counted)
{
  Task *task = new Task;
  task->Owner = this;
  task->Type = Task::LastPortion;
  task->Counted = counted;
  Combiner_.call(task, [this](Task *task) { processTask(task); });
}

void BlockSource::cancelHeadersMessage()
{
  Task *task = new Task;
  task->Owner = this;
  task->Type = Task::Cancel;
  task->Counted = true;
  Combiner_.call(task, [this](Task *task) { processTask(task); });
}

bool BlockSource::stalled(uint32_t bestHeight, std::chrono::time_point<std::chrono::steady_clock> now)
{
  // Chain moved, headers moved or blocks left the queue - all three stand still only if the source
  // does
  BC::Common::BlockIndex *lastKnown = LastKnownIndex_;
  size_t queueSize = DownloadQueue_.unsafe_size();
  if (ProgressTime_ == std::chrono::time_point<std::chrono::steady_clock>::max() ||
      bestHeight != ProgressBestHeight_ || lastKnown != ProgressLastKnown_ || queueSize != ProgressQueueSize_) {
    ProgressTime_ = now;
    ProgressBestHeight_ = bestHeight;
    ProgressLastKnown_ = lastKnown;
    ProgressQueueSize_ = queueSize;
    return false;
  }

  return std::chrono::duration_cast<std::chrono::seconds>(now - ProgressTime_).count() >= StallTimeoutInSeconds;
}

bool BlockSource::downloadFinished()
{
  if (DownloadingFinished_)
    return true;
  if (HeadersFinished_ && (!LastKnownIndex_ || LastKnownIndex_->OnChain))
    return true;
  return false;
}

void BlockSource::enqueue(std::vector<BC::Common::BlockIndex*> &&indexes, bool counted)
{
  Task *task = new Task;
  task->Owner = this;
  task->Type = Task::Batch;
  task->Counted = counted;
  task->Indexes = std::move(indexes);
  task->Prev = !task->Indexes.empty() ? task->Indexes[0]->Prev : nullptr;
  Combiner_.call(task, [this](Task *task) { processTask(task); });
}

void BlockSource::enqueueHighPriority(std::vector<BC::Common::BlockIndex*> &&indexes)
{
  for (auto index: indexes)
    HighPriorityDownloadQueue_.push(index);
}

void BlockSource::processStalledBlocks(BC::Common::BlockIndex *frontier)
{
  TaskHP *task = new TaskHP;
  task->TaskType = TaskHP::ProcessStalledBlocks;
  task->Owner = this;
  task->Frontier = frontier;
  CombinerHP_.call(task, [this](TaskHP *task) { processTask(task); });
}

bool BlockSource::dequeue(std::vector<BC::Common::BlockIndex*> &indexes, size_t indexesNum, bool highPriorityOnly)
{
  for (size_t i = 0; i < indexesNum; i++) {
    BC::Common::BlockIndex *index;
    if (!HighPriorityDownloadQueue_.try_pop(index))
      break;
    indexes.push_back(index);
  }

  if (indexes.empty() && !highPriorityOnly) {
    for (size_t i = 0; i < indexesNum; i++) {
      BC::Common::BlockIndex *index;
      if (!DownloadQueue_.try_pop(index))
        break;
      indexes.push_back(index);
    }
  }

  if (!indexes.empty())
    LastDequeued_[GetWorkerThreadId()] = indexes.back();
  return !indexes.empty();
}

intrusive_ptr<BlockSource> BlockSource::next(unsigned threadsNum, bool createNew, bool &newSourceCreated)
{
  return getOrCreateBlockSource(Next_, threadsNum, createNew, newSourceCreated);
}

intrusive_ptr<BlockSource> BlockSourceList::head(unsigned threadsNum, bool createNew, bool &newSourceCreated)
{
  return BlockSource::getOrCreateBlockSource(Head_, threadsNum, createNew, newSourceCreated);
}

void BlockSourceList::releaseBlockSource(BlockSource *source)
{
  // Walkers in getOrCreateBlockSource route around finished sources and advance the head past
  // them themselves, so releasing is reduced to marking: no CAS on Head_ here means no way to
  // lose a concurrently appended source.
  source->HeadersFinished_ = true;
  source->DownloadingFinished_ = true;
}
