// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/utxodb.h"
#include <tbb/concurrent_queue.h>
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
  ActionTy Type;
  BC::Common::BlockIndex *Index = nullptr;
  Task() {};
  Task(ActionTy type, BC::Common::BlockIndex *index) : Type(type), Index(index) {}
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

  CAllocationInfo BlockCache;
  UTXODb UTXODb_;
};

}
}
