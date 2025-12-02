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
  intrusive_ptr<BlockSource> current(blockSource);
  for (;;) {
    if (current.get() == nullptr && createNew) {
      BlockSource *newValue = new BlockSource(threadsNum);
      if (blockSource.compare_and_exchange(nullptr, newValue)) {
        newSourceCreated = true;
        return intrusive_ptr<BlockSource>(newValue);
      } else {
        // newValue was deleted in this branch
        // TODO: fix it
        current = current.get()->Next_;
        if (!current.get()->DownloadingFinished_)
          return current;
      }
    } else {
      if (!current.get() || !current.get()->DownloadingFinished_)
        return current;
    }

    current = current.get()->Next_;
  }
}

void BlockSource::processTask(Task *task)
{
  if (task->Indexes.empty()) {
    HeadersLastPortion_ = true;
    return;
  }

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
}

void BlockSource::processTask(TaskHP *task)
{
  if (task->TaskType == TaskHP::ProcessStalledBlocks && HighPriorityDownloadQueue_.empty()) {
    // Find last dequeued block for all threads
    BC::Common::BlockIndex *index = nullptr;
    for (unsigned i = 0; i < ThreadsNum_; i++) {
      if (LastDequeued_[i] && (!index || LastDequeued_[i]->Height > index->Height))
        index = LastDequeued_[i];
    }

    // Collect stalled blocks
    // Move from last dequeued block to first on-chain block
    auto now = std::chrono::steady_clock::now();
    std::vector<BC::Common::BlockIndex*> stalledBlocks;
    while (index && !index->OnChain) {
      if (index->IndexState != BSBlock && std::chrono::duration_cast<std::chrono::seconds>(now-index->DownloadingStartTime).count() >= 8)
        stalledBlocks.push_back(index);
      index = index->Prev;
    }

    std::reverse(stalledBlocks.begin(), stalledBlocks.end());
    if (!stalledBlocks.empty())
      LOG_F(INFO, "Retry download %zu blocks in range %s(%u): %s(%u)",
            stalledBlocks.size(),
            stalledBlocks.front()->Header.GetHash().ToString().c_str(),
            stalledBlocks.front()->Height,
            stalledBlocks.back()->Header.GetHash().ToString().c_str(),
            stalledBlocks.back()->Height);

    for (auto index: stalledBlocks)
      HighPriorityDownloadQueue_.push(index);
  }
}

void BlockSource::setHeadersDownloadingFinished()
{
  Task *task = new Task;
  task->Owner = this;
  task->Indexes.clear();
  Combiner_.call(task, [this](Task *task) { processTask(task); });
  if (HeadersLastPortion_)
    HeadersFinished_ = true;
}

bool BlockSource::downloadFinished()
{
  if (DownloadingFinished_)
    return true;
  if (HeadersFinished_ && (!LastKnownIndex_ || LastKnownIndex_->OnChain))
    return true;
  return false;
}

void BlockSource::enqueue(std::vector<BC::Common::BlockIndex*> &&indexes)
{
  Task *task = new Task;
  task->Owner = this;
  task->Indexes = indexes;
  task->Prev = !indexes.empty() ? task->Indexes[0]->Prev : nullptr;
  Combiner_.call(task, [this](Task *task) { processTask(task); });
  if (HeadersLastPortion_)
    HeadersFinished_ = true;
}

void BlockSource::enqueueHighPriority(std::vector<BC::Common::BlockIndex*> &&indexes)
{
  for (auto index: indexes)
    HighPriorityDownloadQueue_.push(index);
}

void BlockSource::processStalledBlocks()
{
  TaskHP *task = new TaskHP;
  task->TaskType = TaskHP::ProcessStalledBlocks;
  task->Owner = this;
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
  source->HeadersFinished_ = true;
  source->DownloadingFinished_ = true;
  if (Head_.get() != source)
    return;

  intrusive_ptr<BlockSource> current(source->Next_);
  for (;;) {
    if (current.get() == nullptr || !current.get()->DownloadingFinished_) {
      Head_.compare_and_exchange(source, current.get());
      return;
    }

    current = current.get()->Next_;
  }

  Head_.compare_and_exchange(source, nullptr);
}
