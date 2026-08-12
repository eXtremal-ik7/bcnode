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

void Storage::connect(CBlockBatch batch, BlockInMemoryIndex &blockIndex, bool wakeUp)
{
  if (batch.empty())
    return;

  UTXODb_.connect(batch, blockIndex, *BlockDb_);

  // The archive databases get the same unit, the mark on the last block telling the
  // storage thread where it ends
  for (size_t i = 0; i < batch.size(); i++)
    Queue_.emplace(Connect, batch[i].Index, i + 1 == batch.size());

  if (wakeUp)
    userEventActivate(NewTaskEvent_);
}

void Storage::disconnect(BC::Common::BlockIndex *index,
                         const BC::Proto::Block &block,
                         const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                         const BC::Proto::CBlockValidationData &validationData,
                         BlockInMemoryIndex &blockIndex,
                         bool wakeUp)
{
  UTXODb_.disconnect(index, block, linkedOutputs, validationData, blockIndex, *BlockDb_);

  Queue_.emplace(Disconnect, index);

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

// The queue side of one connect: the archive databases take the unit whole, then the
// block data of every block in it goes to disk
void Storage::applyBatch()
{
  if (Batch_.empty())
    return;

  Archive_->connect(Batch_, *BlockIndex_, *BlockDb_);

  bool needFlush = false;
  for (const CBlockRef &ref: Batch_) {
    if (!BlockDb_->writeBlock(ref.Index, &needFlush))
      ErrorHandler_();
    CachedBlocks_.push_back(ref.Index);
  }

  if (needFlush)
    flush();

  Batch_.clear();
  // Only now: the refs above point into these
  BatchObjects_.clear();
}

void Storage::onQueuePush()
{
  Task task;
  while (Queue_.try_pop(task)) {
    // Anything that is not a connect ends the batch being collected: the queue is the
    // order the chain moved in
    if (task.Type != Connect) {
      applyBatch();
      bool needFlush = false;
      if (task.Type == Disconnect) {
        intrusive_ptr<BC::Common::CIndexCacheObject> object = objectByIndex(task.Index, *ChainParams_, *BlockDb_);
        assert(object.get());
        Archive_->disconnect(task.Index, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationDataConst(), *BlockIndex_, *BlockDb_);
      } else {
        if (!BlockDb_->writeBlock(task.Index, &needFlush))
          ErrorHandler_();
        CachedBlocks_.push_back(task.Index);
      }

      if (needFlush)
        flush();
      continue;
    }

    intrusive_ptr<BC::Common::CIndexCacheObject> object = objectByIndex(task.Index, *ChainParams_, *BlockDb_);
    assert(object.get());
    Batch_.push_back(CBlockRef{task.Index, object.get()->block(), &object.get()->linkedOutputs(), &object.get()->validationDataConst()});
    BatchObjects_.push_back(object);

    if (task.BatchEnd)
      applyBatch();
  }

  // Ran dry mid-batch: the mutator is still pushing, and what is here is a chain
  // position all the same - a prefix of the unit, ending at a block boundary
  applyBatch();

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
