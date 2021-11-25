// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <BC/bc.h>
#include <db/common.h>
#include <db/utxodb.h>
#include <tbb/concurrent_queue.h>
#include <functional>
#include <thread>

struct asyncBase;
struct aioUserEvent;
class BlockDatabase;

namespace BC {
namespace DB {

class Archive;

struct Task {
  ActionTy Type;
  BC::Common::BlockIndex *Index = nullptr;
  Task() {};
  Task(ActionTy type, BC::Common::BlockIndex *index) : Type(type), Index(index) {}
};

class Storage {
public:
  ~Storage();
  bool init(BlockDatabase &blockDb, Archive &archive, std::function<void()> errorHandler);
  void add(ActionTy type, BC::Common::BlockIndex *index, bool wakeUp = false);
  void wakeUp();

  BlockDatabase &blockDb() { return *BlockDb_; }
  Archive &archive() { return *Archive_; }
  UTXODb &utxodb() { return UTXODb_; }
  SerializedDataCache &cache() { return BlockCache; }

  tbb::concurrent_queue<Task> &queue() { return Queue_; }
private:
  static void timerCb(aioUserEvent*, void *arg) { static_cast<Storage*>(arg)->onTimer(); }
  static void newTaskCb(aioUserEvent*, void *arg) { static_cast<Storage*>(arg)->onQueuePush(); }

  void onTimer();
  void onQueuePush();
  void flush();

private:
  bool Initialized_ = false;
  BlockDatabase *BlockDb_ = nullptr;
  Archive *Archive_ = nullptr;
  asyncBase *Base_ = nullptr;
  aioUserEvent *NewTaskEvent_ = nullptr;
  aioUserEvent *TimerEvent_ = nullptr;
  std::thread Thread_;
  std::function<void()> ErrorHandler_;
  tbb::concurrent_queue<Task> Queue_;
  std::vector<BC::Common::BlockIndex*> CachedBlocks_;
  std::chrono::time_point<std::chrono::steady_clock> LastFlushTime_ = std::chrono::steady_clock::now();

  SerializedDataCache BlockCache;
  UTXODb UTXODb_;
};

}
}
