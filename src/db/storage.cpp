// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "storage.h"
#include "common/blockDataBase.h"
#include "db/archive.h"
#include <asyncio/asyncio.h>
#include <chrono>

namespace BC {
namespace DB {


Storage::~Storage()
{
  if (Initialized_) {
    deleteUserEvent(NewTaskEvent_);
    deleteUserEvent(TimerEvent_);
    postQuitOperation(Base_);
    Thread_.join();

    assert(Queue_.empty());

    UTXODb_.flush();
    // Quiescent point right after the final flush: the dump stamp matches
    // the database stamp written above
    UTXODb_.saveCache();
    Archive_->flush();
    flush();
  }
}

bool Storage::run(std::function<void()> errorHandler)
{
  ErrorHandler_ = errorHandler;

  Base_ = createAsyncBase(amOSDefault, 1);
  if (!Base_) {
    LOG_F(ERROR, "Can't create asyncio base for storage");
    return false;
  }

  NewTaskEvent_ = newUserEvent(Base_, 0, newTaskCb, this);
  TimerEvent_ = newUserEvent(Base_, 0, timerCb, this);

  std::thread thread([](asyncBase *base) {
    loguru::set_thread_name("storage");
    asyncLoop(base);
  }, Base_);

  Thread_ = std::move(thread);
  Initialized_ = true;
  userEventStartTimer(TimerEvent_, 10*1000000, 1);
  return true;
}

void Storage::add(ActionTy type,
                  BC::Common::BlockIndex *index,
                  const BC::Proto::Block &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  const BC::Proto::CBlockValidationData &validationData,
                  BlockInMemoryIndex &blockIndex,
                  bool wakeUp)
{
  switch (type) {
    case Connect :
      UTXODb_.connect(index, block, linkedOutputs, validationData, blockIndex, *BlockDb_);
      break;
    case Disconnect:
      UTXODb_.disconnect(index, block, linkedOutputs, validationData, blockIndex, *BlockDb_);
      break;
    default:
      break;
  }

  size_t memory = 0;
  if (BC::Common::CIndexCacheObject *object = index->Serialized.get())
    memory = object->blockData().memorySize();
  QueuedMemory_.fetch_add(memory, std::memory_order_relaxed);

  Queue_.emplace(type, index, memory);
  if (wakeUp)
    userEventActivate(NewTaskEvent_);
}

void Storage::wakeUp()
{
  if (Initialized_)
    userEventActivate(NewTaskEvent_);
}

void Storage::onTimer()
{
  // Flush all data to disk if no new block within minute; a full cache is
  // flushed right away - the reindex loader is throttled on it
  auto now = std::chrono::steady_clock::now();
  if (BlockCache.overflow() ||
      std::chrono::duration_cast<std::chrono::seconds>(now - LastFlushTime_).count() >= 60)
    flush();
  userEventStartTimer(TimerEvent_, 10*1000000, 1);
}

void Storage::onQueuePush()
{
  Task task;
  bool needFlush = false;
  while (Queue_.try_pop(task)) {
    intrusive_ptr<BTC::Common::CIndexCacheObject> object = objectByIndex(task.Index, *ChainParams_, *BlockDb_);
    assert(object.get());
    switch (task.Type) {
      case Connect :
        Archive_->connect(task.Index, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationDataConst(), *BlockIndex_, *BlockDb_);
        if (!BlockDb_->writeBlock(task.Index, &needFlush))
          ErrorHandler_();
        CachedBlocks_.push_back(task.Index);
        break;
      case Disconnect :
        Archive_->disconnect(task.Index, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationDataConst(), *BlockIndex_, *BlockDb_);
        break;
      case WriteData :
        if (!BlockDb_->writeBlock(task.Index, &needFlush))
          ErrorHandler_();
        CachedBlocks_.push_back(task.Index);
        break;

    }

    QueuedMemory_.fetch_sub(task.Memory, std::memory_order_relaxed);

    if (needFlush) {
      flush();
      needFlush = false;
    }
  }

  // A full cache is released only by flush; the throttled loaders wait on it
  if (BlockCache.overflow())
    flush();
}

void Storage::flush()
{
  if (!BlockDb_->flush())
    ErrorHandler_();

  std::for_each(CachedBlocks_.begin(), CachedBlocks_.end(), [](BC::Common::BlockIndex *index) {
    index->Serialized.reset();
  });

  CachedBlocks_.clear();
  LastFlushTime_ = std::chrono::steady_clock::now();
}

}
}
