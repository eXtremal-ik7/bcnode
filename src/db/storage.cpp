// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "storage.h"
#include "common/blockDataBase.h"
#include "db/archive.h"
#include <asyncio/asyncio.h>

namespace BC {
namespace DB {

Storage::~Storage()
{
  if (Initialized_) {
    deleteUserEvent(NewTaskEvent_);
    deleteUserEvent(TimerEvent_);
    postQuitOperation(Base_);
    Thread_.join();
    flush();
  }
}

bool Storage::init(BlockDatabase &blockDb, Archive &archive, std::function<void()> errorHandler)
{
  BlockDb_ = &blockDb;
  Archive_ = &archive;
  ErrorHandler_ = errorHandler;

  Base_ = createAsyncBase(amOSDefault);
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

void Storage::add(ActionTy type, BC::Common::BlockIndex *index, bool wakeUp)
{
  Queue_.emplace(type, index);
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
  // Flush all data to disk if no new block within minute
  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::seconds>(now - LastFlushTime_).count() >= 60)
    flush();
  userEventStartTimer(TimerEvent_, 10*1000000, 1);
}

void Storage::onQueuePush()
{
  Task task;
  while (Queue_.try_pop(task)) {
    CachedBlocks_.push_back(task.Index);

    if (task.Type == Connect || task.Type == Disconnect) {
      intrusive_ptr<const SerializedDataObject> serializedPtr(task.Index->Serialized);
      if (serializedPtr.get()) {
        const BC::Proto::Block *block = static_cast<BC::Proto::Block*>(serializedPtr.get()->unpackedData());
        Archive_->add(task.Index, *block, task.Type);
      } else {
        auto handler = [this, &task](void *data, size_t size) {
          BC::Proto::Block block;
          xmstream stream(data, size);
          BC::unserialize(stream, block);
          Archive_->add(task.Index, block, task.Type);
        };

        BlockSearcher searcher(*BlockDb_, handler, ErrorHandler_);
        searcher.add(task.Index);
      }
    } else {
      LastFlushTime_ = std::chrono::steady_clock::now();
      if (!BlockDb_->writeBlock(task.Index))
        ErrorHandler_();

      if (BlockDb_->writeBufferEmpty()) {
        // Block data was written to disk, flush all caches
        flush();
      }
    }
  }
}

void Storage::flush()
{
  if (!BlockDb_->flush())
    ErrorHandler_();

  Archive_->flush();

  std::for_each(CachedBlocks_.begin(), CachedBlocks_.end(), [](BC::Common::BlockIndex *index) {
    index->Serialized.reset();
  });

  CachedBlocks_.clear();
  LastFlushTime_ = std::chrono::steady_clock::now();
}

}
}
