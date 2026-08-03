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
  // Block data the task keeps alive until it is written; the reader throttles on the sum
  size_t Memory = 0;
  Task() {};
  Task(ActionTy type, BC::Common::BlockIndex *index, size_t memory) : Type(type), Index(index), Memory(memory) {}
};


class Storage {
public:
  ~Storage();
  void init(BlockDatabase &blockDb, BlockInMemoryIndex &blockIndex, Archive &archive) {
    BlockDb_ = &blockDb;
    BlockIndex_ = &blockIndex;
    Archive_ = &archive;
  }

  bool run(std::function<void()> errorHandler);

  void add(ActionTy type,
           BC::Common::BlockIndex *index,
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
  // Parsed block data of tasks queued but not processed yet. The block cache cannot serve as
  // the read ahead measure: it counts the data the pipeline itself holds in flight, and a
  // reader throttled on that runs in lockstep with the chain
  size_t queuedMemory() const { return QueuedMemory_.load(std::memory_order_relaxed); }

private:
  static void timerCb(aioUserEvent*, void *arg) { static_cast<Storage*>(arg)->onTimer(); }
  static void newTaskCb(aioUserEvent*, void *arg) { static_cast<Storage*>(arg)->onQueuePush(); }

  void onTimer();
  void onQueuePush();

private:
  bool Initialized_ = false;
  BlockDatabase *BlockDb_ = nullptr;
  BlockInMemoryIndex *BlockIndex_ = nullptr;
  Archive *Archive_ = nullptr;
  asyncBase *Base_ = nullptr;
  aioUserEvent *NewTaskEvent_ = nullptr;
  aioUserEvent *TimerEvent_ = nullptr;
  std::thread Thread_;
  std::function<void()> ErrorHandler_;
  tbb::concurrent_queue<Task> Queue_;
  std::vector<BC::Common::BlockIndex*> CachedBlocks_;
  std::chrono::time_point<std::chrono::steady_clock> LastFlushTime_ = std::chrono::steady_clock::now();

  std::atomic<size_t> QueuedMemory_ = 0;
  CAllocationInfo BlockCache;
  UTXODb UTXODb_;
};

}
}
