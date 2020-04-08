// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once 

#include "BC/bc.h"
#include <common/combiner.h>
#include <common/spmc_unbounded_queue.h>
#include <common/intrusive_ptr.h>
#include <memory>
#include <unordered_map>


class alignas(512) BlockSource {
public:
  struct Task {
  public:
    BlockSource *Owner;
    std::vector<BC::Common::BlockIndex*> Indexes;
    BC::Common::BlockIndex *Prev;
    Task *Next_ = nullptr;

  public:
    Task *next() { return Next_; }
    void setNext(Task *next) { Next_ = next; }
    void run() { Owner->processTask(this); }
    void release() { delete this; }
  };

  struct TaskHP {
  public:
    enum Type {
      Enqueue = 0,
      ProcessStalledBlocks
    };

  public:
    Type TaskType;
    BlockSource *Owner;
    std::vector<BC::Common::BlockIndex*> Indexes;
    TaskHP *Next_ = nullptr;

  public:
    TaskHP *next() { return Next_; }
    void setNext(TaskHP *next) { Next_ = next; }
    void run() { Owner->processTask(this); }
    void release() { delete this; }
  };

private:
  struct HashCmp {
    bool operator()(const Task *l, const Task *r) { return l->Prev < r->Prev; }
  };

private:
  mutable std::atomic<uintptr_t> Refs_ = 0;
  atomic_intrusive_ptr<BlockSource> Next_;
  unsigned ThreadsNum_;
  Combiner<Task> Combiner_;
  Combiner<TaskHP> CombinerHP_;
  bool HeadersLastPortion_ = false;
  bool HeadersFinished_ = false;
  bool DownloadingFinished_ = false;
  BC::Common::BlockIndex *LastKnownIndex_ = nullptr;
  std::unordered_map<BC::Common::BlockIndex*, std::vector<BC::Common::BlockIndex*>> EnqueuedTasks_;
  spmc_unbounded_queue<BC::Common::BlockIndex*> DownloadQueue_;
  spmc_unbounded_queue<BC::Common::BlockIndex*> HighPriorityDownloadQueue_;
  std::unique_ptr<BC::Common::BlockIndex*[]> LastDequeued_;

private:
  void processTask(Task *task);
  void processTask(TaskHP *task);

public:
  BlockSource(unsigned threadsNum) :
    ThreadsNum_(threadsNum),
    DownloadQueue_(65536),
    HighPriorityDownloadQueue_(4096),
    LastDequeued_(new BC::Common::BlockIndex* [threadsNum]) {
    std::fill(LastDequeued_.get(), LastDequeued_.get() + threadsNum, nullptr);
  }

  uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
  uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }

  void setHeadersDownloadingFinished();
  bool headersDownloadingFinished() { return HeadersFinished_; }
  bool downloadFinished();

  void enqueue(std::vector<BC::Common::BlockIndex*> &&indexes);
  void enqueueHighPriority(std::vector<BC::Common::BlockIndex*> &&indexes);
  void processStalledBlocks();
  bool dequeue(std::vector<BC::Common::BlockIndex*> &indexes, size_t indexesNum, bool highPriorityOnly);
  BC::Common::BlockIndex *lastKnownIndex() { return LastKnownIndex_; }

  static intrusive_ptr<BlockSource> getOrCreateBlockSource(atomic_intrusive_ptr<BlockSource> &blockSource, unsigned threadsNum, bool createNew, bool &newSourceCreated);
  intrusive_ptr<BlockSource> next(unsigned threadsNum, bool createNew, bool &newSourceCreated);

  friend class BlockSourceList;
};

class BlockSourceList {
private:
  atomic_intrusive_ptr<BlockSource> Head_;

public:
  bool hasActiveBlockSource() { return Head_.get() != nullptr; }
  intrusive_ptr<BlockSource> head(unsigned threadsNum, bool createNew, bool &newSourceCreated);
  void releaseBlockSource(BlockSource *source);
};
