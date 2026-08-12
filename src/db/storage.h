// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/utxodb.h"
#include <tbb/concurrent_queue.h>
#include <atomic>
#include <functional>
#include <thread>

struct asyncBase;
struct aioUserEvent;
class BlockDatabase;

namespace BC {
namespace DB {

class Archive;

enum ActionTy {
  Connect = 0,
  Disconnect,
  WriteData
};

struct Task {
  ActionTy Type = Connect;
  BC::Common::BlockIndex *Index = nullptr;
  // Last block of the batch the mutator handed over: where the storage thread cuts its own
  bool BatchEnd = false;
  Task() {};
  Task(ActionTy type, BC::Common::BlockIndex *index, bool batchEnd = false) :
    Type(type), Index(index), BatchEnd(batchEnd) {}
};


class Storage {
public:
  ~Storage();
  void init(BlockDatabase &blockDb, BlockInMemoryIndex &blockIndex, BC::Common::ChainParams &chainParams, Archive &archive) {
    BlockDb_ = &blockDb;
    BlockIndex_ = &blockIndex;
    ChainParams_ = &chainParams;
    Archive_ = &archive;
  }

  bool run(std::function<void()> errorHandler);

  // Both take utxodb here, on the caller's thread, and reach the archive databases
  // through the queue. A connect is a run - a block connected on its own is a batch
  // of one; a disconnect walks a fork down and comes a block at a time
  void connect(CBlockBatch batch, BlockInMemoryIndex &blockIndex, bool wakeUp = false);

  void disconnect(BC::Common::BlockIndex *index,
                  const BC::Proto::Block &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  const BC::Proto::CBlockValidationData &validationData,
                  BlockInMemoryIndex &blockIndex,
                  bool wakeUp = false);

  void wakeUp();

  BlockDatabase &blockDb() { return *BlockDb_; }
  Archive &archive() { return *Archive_; }
  UTXODb &utxodb() { return UTXODb_; }
  CAllocationInfo &cache() { return BlockCache; }
  void flush();

  tbb::concurrent_queue<Task> &queue() { return Queue_; }

private:
  static void timerCb(aioUserEvent*, void *arg) { static_cast<Storage*>(arg)->onTimer(); }
  static void newTaskCb(aioUserEvent*, void *arg) { static_cast<Storage*>(arg)->onQueuePush(); }

  void onTimer();
  void onQueuePush();
  void applyBatch();

private:
  bool Initialized_ = false;
  BlockDatabase *BlockDb_ = nullptr;
  BlockInMemoryIndex *BlockIndex_ = nullptr;
  // A block reloaded from disk for a task learns its consensus exemptions from here
  BC::Common::ChainParams *ChainParams_ = nullptr;
  Archive *Archive_ = nullptr;
  asyncBase *Base_ = nullptr;
  aioUserEvent *NewTaskEvent_ = nullptr;
  aioUserEvent *TimerEvent_ = nullptr;
  std::thread Thread_;
  std::function<void()> ErrorHandler_;
  tbb::concurrent_queue<Task> Queue_;

  // Storage thread only: the connect batch being collected off the queue. The objects hold the
  // parsed block data the refs point into, so both live exactly as long as the batch does
  std::vector<CBlockRef> Batch_;
  std::vector<intrusive_ptr<BC::Common::CIndexCacheObject>> BatchObjects_;

  std::vector<BC::Common::BlockIndex*> CachedBlocks_;
  std::chrono::time_point<std::chrono::steady_clock> LastFlushTime_ = std::chrono::steady_clock::now();

  CAllocationInfo BlockCache;
  UTXODb UTXODb_;
};

}
}
