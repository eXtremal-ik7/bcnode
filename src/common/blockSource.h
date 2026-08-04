// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once 

#include "BC/bc.h"
#include <common/combiner.h>
#include <common/intrusive_ptr.h>
#include <tbb/concurrent_queue.h>
#include <memory>
#include <unordered_map>


class alignas(512) BlockSource {
public:
  struct Task {
  public:
    enum EType {
      Batch = 0,    // portion of headers from producer peer
      LastPortion,  // empty headers message: peer has nothing more
      Cancel        // accounted message dropped before it became a batch
    };

  public:
    BlockSource *Owner;
    EType Type = Batch;
    // Releases one message accounted by headersMessageReceived
    bool Counted = false;
    std::vector<BC::Common::BlockIndex*> Indexes;
    BC::Common::BlockIndex *Prev = nullptr;
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
      ProcessStalledBlocks = 0
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
  std::atomic<uint32_t> HeadersInFlight_ = 0;
  std::atomic<bool> HeadersFinished_ = false;
  std::atomic<bool> DownloadingFinished_ = false;
  BC::Common::BlockIndex *LastKnownIndex_ = nullptr;
  std::unordered_map<BC::Common::BlockIndex*, std::vector<BC::Common::BlockIndex*>> EnqueuedTasks_;
  tbb::concurrent_queue<BC::Common::BlockIndex*> DownloadQueue_;
  tbb::concurrent_queue<BC::Common::BlockIndex*> HighPriorityDownloadQueue_;
  std::unique_ptr<BC::Common::BlockIndex*[]> LastDequeued_;

  // Progress watchdog, touched by Node::Sync only
  static constexpr int64_t StallTimeoutInSeconds = 60;
  std::chrono::time_point<std::chrono::steady_clock> ProgressTime_ = std::chrono::time_point<std::chrono::steady_clock>::max();
  BC::Common::BlockIndex *ProgressLastKnown_ = nullptr;
  uint32_t ProgressBestHeight_ = 0;
  size_t ProgressQueueSize_ = 0;

private:
  void processTask(Task *task);
  void processTask(TaskHP *task);

public:
  BlockSource(unsigned threadsNum) :
    ThreadsNum_(threadsNum),
    LastDequeued_(new BC::Common::BlockIndex* [threadsNum]) {
    std::fill(LastDequeued_.get(), LastDequeued_.get() + threadsNum, nullptr);
  }

  uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
  uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }

  // The empty marker overtakes batches still being validated, so headers are counted from the
  // receiving thread (last place with TCP order) until their task reaches the combiner. Every
  // accounted message must produce exactly one task; a dropped one produces Cancel
  void headersMessageReceived() { HeadersInFlight_.fetch_add(1); }
  void cancelHeadersMessage();

  void setHeadersDownloadingFinished(bool counted);
  bool headersDownloadingFinished() { return HeadersFinished_; }
  bool downloadFinished();

  // Nothing times out an idle source by itself: a hole in the headers (a dropped message) or a peer
  // that went quiet leaves it neither finished nor moving. Whoever calls this releases such a source
  bool stalled(uint32_t bestHeight, std::chrono::time_point<std::chrono::steady_clock> now);

  void enqueue(std::vector<BC::Common::BlockIndex*> &&indexes, bool counted);
  void enqueueHighPriority(std::vector<BC::Common::BlockIndex*> &&indexes);
  void processStalledBlocks();
  bool dequeue(std::vector<BC::Common::BlockIndex*> &indexes, size_t indexesNum, bool highPriorityOnly);
  BC::Common::BlockIndex *lastKnownIndex() { return LastKnownIndex_; }

  static intrusive_ptr<BlockSource> getOrCreateBlockSource(atomic_intrusive_ptr<BlockSource> &blockSource, unsigned threadsNum, bool createNew, bool &newSourceCreated);
  intrusive_ptr<BlockSource> next(unsigned threadsNum, bool createNew, bool &newSourceCreated);

  friend class BlockSourceList;
};

// Keeps a headers message accounted in its source until it produces a combiner task: enqueue and
// the marker take the accounting over with consume(), any other exit posts Cancel
class HeadersMessageToken {
public:
  HeadersMessageToken() {}
  explicit HeadersMessageToken(intrusive_ptr<BlockSource> &&source) : Source_(std::move(source)) {
    if (Source_.get())
      Source_.get()->headersMessageReceived();
  }

  HeadersMessageToken(const HeadersMessageToken&) = delete;
  HeadersMessageToken &operator=(const HeadersMessageToken&) = delete;
  HeadersMessageToken(HeadersMessageToken &&token) : Source_(std::move(token.Source_)) {}
  HeadersMessageToken &operator=(HeadersMessageToken &&token) {
    if (Source_.get())
      Source_.get()->cancelHeadersMessage();
    Source_ = std::move(token.Source_);
    return *this;
  }

  ~HeadersMessageToken() {
    if (Source_.get())
      Source_.get()->cancelHeadersMessage();
  }

  // Hands accounting to a task of 'source'; false if the peer switched source under the message
  bool consume(BlockSource *source) {
    if (!source || Source_.get() != source)
      return false;
    Source_ = nullptr;
    return true;
  }

private:
  intrusive_ptr<BlockSource> Source_;
};

class BlockSourceList {
private:
  atomic_intrusive_ptr<BlockSource> Head_;

public:
  // Head_ is parked on the last finished source until a walk advances it, so a plain null check
  // is not enough: walk the chain to the first active source instead
  bool hasActiveBlockSource() {
    bool newSourceCreated;
    return BlockSource::getOrCreateBlockSource(Head_, 0, false, newSourceCreated).get() != nullptr;
  }
  intrusive_ptr<BlockSource> head(unsigned threadsNum, bool createNew, bool &newSourceCreated);
  void releaseBlockSource(BlockSource *source);
};
