// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/blockDataBase.h"
#include "common/mlog.h"
#include "common/utils.h"

#include "swmrhashmap.h"
#include "config4cpp/Configuration.h"
#include <rocksdb/db.h>
#include <rocksdb/merge_operator.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace BC {
namespace DB {

struct CEntry {
  BC::Proto::BlockHashTy BlockId;
  bool IsConnected;
  CEntry() {}
  CEntry(const BC::Proto::BlockHashTy &blockId, bool isConnected) : BlockId(blockId), IsConnected(isConnected) {}
};

template<typename CKey>
struct CLog {
  MLog Log;

  // One flat SWMR map replaces the per-key version vectors and the tbb map
  // for concurrent point reads (see swmrhashmap.h). Starts small, grows to
  // the working-set size once and keeps its capacity across window resets
  CSwmrHashMap<CKey> Map{4096};

  // Undo log: (key, previous value) recorded on the first touch of a key in
  // a revision, with revision boundaries on top; pop() replays the tail of
  // the last revision in reverse. Revision numbers are monotonic within the
  // window and never reused, so a slot's LastRev surviving from a popped
  // revision cannot alias the next block and swallow its undo record
  struct SUndo {
    CKey Key;
    void *Prev;
  };
  struct SRevision {
    BC::Proto::BlockHashTy BlockId;
    size_t UndoStart;
    uint32_t Rev;
  };
  std::vector<SUndo> Undo;
  std::vector<SRevision> Revisions;
  uint32_t NextRev = 0;

  const void *find(const CKey &key) { return Map.find(key); }

  void update(const BC::Proto::BlockHashTy &blockId, const CKey &key, void *newData) {
    if (Revisions.empty() || Revisions.back().BlockId != blockId)
      Revisions.push_back({blockId, Undo.size(), ++NextRev});
    auto result = Map.update(key, newData, Revisions.back().Rev);
    if (result.FirstInRev)
      Undo.push_back({key, result.Prev});
  }

  // Reverts the last revision if it belongs to blockId (the only case the
  // callers produce); a null previous value is restored as a tombstone, so
  // the map answers "absent in this window" without breaking probe chains
  void pop(const BC::Proto::BlockHashTy &blockId) {
    if (Revisions.empty() || Revisions.back().BlockId != blockId)
      return;
    size_t start = Revisions.back().UndoStart;
    for (size_t i = Undo.size(); i > start; ) {
      i--;
      Map.restore(Undo[i].Key, Undo[i].Prev);
    }
    Undo.resize(start);
    Revisions.pop_back();
  }

  // O(1) window reset: the generation bump invalidates every map entry at
  // once and the arena is rewound. Runs in the writer right before the log
  // is reused, as late as possible for concurrent readers chasing arena
  // pointers (the residual race is the pre-existing bug A class, closed
  // for real by EBR in the aggkv design)
  void reset() {
    Map.reset();
    Undo.clear();
    Revisions.clear();
    NextRev = 0;
    Log.reset();
  }
};

#pragma pack(push, 1)
struct CBaseCfg {
  uint32_t Version;
  uint32_t ShardsNum;
};
#pragma pack(pop)

class BaseInterface {
public:
  virtual ~BaseInterface() {}

  virtual rocksdb::MergeOperator *mergeOperator() = 0;

  virtual bool initialize(BlockInMemoryIndex &blockIndex,
                          const std::filesystem::path &dbPath,
                          BC::DB::Storage &storage,
                          config4cpp::Configuration *cfg,
                          BC::Common::BlockIndex **forConnect,
                          std::vector<BC::Common::BlockIndex*> &forDisconnect) = 0;

  virtual void connect(const BC::Common::BlockIndex *index,
                       const BC::Proto::Block &block,
                       const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                       BlockInMemoryIndex &blockIndex,
                       BlockDatabase &blockDb) = 0;

  virtual void disconnect(const BC::Common::BlockIndex *index,
                          const BC::Proto::Block &block,
                          const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                          BlockInMemoryIndex &blockIndex,
                          BlockDatabase &blockDb) = 0;

  virtual void flushImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex) = 0;
  virtual void *interface(int interface) = 0;

  const std::string &name() const { return Name_; }
  BC::Proto::BlockHashTy currentBlock() { return CurrentBlock_; }

  virtual void flush() = 0;

protected:
  BC::Proto::BlockHashTy CurrentBlock_;
  std::string Name_;
};

template<typename CKey>
class Base : public BaseInterface {
public:
  Base(const std::string &name) {
    Name_ = name;
  }

  // The derived destructor must run shutdownAsyncFlush() itself if async
  // flush was enabled: the flusher dispatches virtual flushFrozenImpl, which
  // must not happen while the derived part is already destroyed
  virtual ~Base() {
    shutdownAsyncFlush();
  }

  bool initialize(BlockInMemoryIndex &blockIndex,
                  const std::filesystem::path &dbPath,
                  BC::DB::Storage &storage,
                  config4cpp::Configuration *cfg,
                  BC::Common::BlockIndex **forConnect,
                  std::vector<BC::Common::BlockIndex*> &forDisconnect) final {
    BaseCfg_.ShardsNum = static_cast<unsigned>(cfg->lookupInt(Name_.c_str(), "shardsNum", 1));
    BaseCfg_.Version = version();

    BC::Common::BlockIndex *bestIndex = blockIndex.best();
    Shards_.reset(new CShardSlot[BaseCfg_.ShardsNum]);
    OnDiskStorage_.resize(BaseCfg_.ShardsNum);
    *forConnect = nullptr;

    // Open all shards
    BC::Proto::BlockHashTy stamp;
    for (size_t i = 0; i < BaseCfg_.ShardsNum; i++) {
      auto shardPath = dbPath / std::to_string(i);
      std::filesystem::create_directories(shardPath);

      rocksdb::DB *db;
      rocksdb::Options options;
      options.create_if_missing = true;
      options.compression = rocksdb::kZSTD;
      options.keep_log_file_num = 4;
      options.merge_operator.reset(mergeOperator());
      std::string shardPathUtf8 = pathToUtf8(shardPath);
      rocksdb::Status status = rocksdb::DB::Open(options, shardPathUtf8, &db);
      if (!status.ok()) {
        LOG_F(ERROR, "Can't open or create txdb database at %s", shardPathUtf8.c_str());
        return false;
      }

      OnDiskStorage_[i].reset(db);

      bool isEmpty = false;

      {
        // Check base configuration (shards num)
        rocksdb::Slice key("basecfg");
        std::string value;
        if (db->Get(rocksdb::ReadOptions(), key, &value).ok() && value.size() >= sizeof(CBaseCfg)) {
          const CBaseCfg *storedCfg = reinterpret_cast<const CBaseCfg*>(value.data());
          if (storedCfg->Version != BaseCfg_.Version) {
            LOG_F(ERROR, "database '%s' uses version %u, but found %u, restart with --reindex=%s",
                  Name_.c_str(), BaseCfg_.Version, storedCfg->Version, Name_.c_str());
            return false;
          }

          if (storedCfg->ShardsNum != BaseCfg_.ShardsNum) {
            LOG_F(ERROR, "database '%s configured with %u shards, found database with %u shards, restart with --reindex=%s",
                  Name_.c_str(), BaseCfg_.ShardsNum, storedCfg->ShardsNum, Name_.c_str());
            return false;
          }
        } else {
          // DB not have base configuration, write current to it
          rocksdb::Slice value(reinterpret_cast<char*>(&BaseCfg_), sizeof(BaseCfg_));
          db->Put(rocksdb::WriteOptions(), key, value);
          isEmpty = true;
        }
      }

      // Check stamp (last known block)
      std::string stampData;
      if (!isEmpty && db->Get(rocksdb::ReadOptions(), rocksdb::Slice("stamp"), &stampData).ok()) {
        if (stampData.size() != sizeof(BC::Proto::BlockHashTy)) {
          LOG_F(ERROR, "%s is corrupted: invalid stamp size (%s)", Name_.c_str(), shardPathUtf8.c_str());
          return false;
        }

        BC::Proto::BlockHashTy shardStamp;
        memcpy(shardStamp.begin(), stampData.data(), sizeof(BC::Proto::BlockHashTy));
        if (i == 0) {
          stamp = shardStamp;
          auto It = blockIndex.blockIndex().find(stamp);
          if (It == blockIndex.blockIndex().end()) {
            LOG_F(ERROR,
                  "%s is corrupted: stamp %s not exists in block index (%s)",
                  Name_.c_str(),
                  stamp.getHexLE().c_str(),
                  shardPathUtf8.c_str());
            return false;
          }

          // flush() stamps every shard with CurrentBlock_, so the position must
          // be known right after open, before any block is connected
          CurrentBlock_ = stamp;

          // Build connect and disconnect block set if need
          *forConnect = It->second == bestIndex ? nullptr : rebaseChain(bestIndex, It->second, forDisconnect);
        } else if (shardStamp != stamp) {
          LOG_F(ERROR, "%s is corrupted: shard %zu has different stamp", Name_.c_str(), i);
          return false;
        }
      } else {
        // database is empty, run full rescanning
        *forConnect = blockIndex.genesis();
      }
    }

    return initializeImpl(cfg, storage);
  }

  void connect(const BC::Common::BlockIndex *index,
               const BC::Proto::Block &block,
               const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
               BlockInMemoryIndex &blockIndex,
               BlockDatabase &blockDb) final {
    auto hash = index->Header.GetHash();

    if (!PendingBlocks_.empty()) {
      auto last = PendingBlocks_.back();
      if (!last.IsConnected && last.BlockId == hash) {
        // The pop reverts the active log only; entries that already left it
        // (frozen or flushed) are beyond cancel - same window as a threshold
        // flush between the pair (bug F), unchanged by async flush
        for (size_t i = 0; i < BaseCfg_.ShardsNum; i++)
          shard(i).pop(hash);
        connectFastImpl(index, block, linkedOutputs);
        return;
      }
    }

    connectImpl(index, block, linkedOutputs, blockIndex, blockDb);
    PendingBlocks_.emplace_back(hash, true);
    CurrentBlock_ = hash;

    for (size_t i = 0; i < BaseCfg_.ShardsNum; i++) {
      if (shard(i).Log.size() >= 1u << 24) {
        if (AsyncFlushEnabled_)
          freezeAndScheduleFlush(i);
        else
          flushImpl(CurrentBlock_, i);
      }
    }
  }

  void disconnect(const BC::Common::BlockIndex *index,
                  const BC::Proto::Block &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  BlockInMemoryIndex &blockIndex,
                  BlockDatabase &blockDb) final {
    auto hash = index->Header.GetHash();

    if (!PendingBlocks_.empty()) {
      auto last = PendingBlocks_.back();
      if (last.IsConnected && last.BlockId == hash) {
        for (size_t i = 0; i < BaseCfg_.ShardsNum; i++)
          shard(i).pop(hash);
        disconnectFastImpl(index, block, linkedOutputs);
        return;
      }
    }

    disconnectImpl(index, block, linkedOutputs, blockIndex, blockDb);
    PendingBlocks_.emplace_back(hash, false);
    CurrentBlock_ = index->Header.hashPrevBlock;
  }

  void flush() final {
    // Scheduled background flushes carry older stamps and must land first,
    // so shard stamps only move forward
    drainAsyncFlushes();

    // Freshly created database with no position yet: nothing to stamp
    if (CurrentBlock_.isNull())
      return;

    // The stamp goes to every shard, including untouched ones: a skipped empty
    // shard keeps an old stamp after a threshold flush of its neighbors, and
    // initialize() rejects unequal stamps as corruption
    for (size_t i = 0; i < BaseCfg_.ShardsNum; i++)
      flushImpl(CurrentBlock_, i);
  }

  BC::Proto::BlockHashTy currentBlock() { return CurrentBlock_; }

  virtual uint32_t version() = 0;
  virtual bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage) = 0;

  virtual void connectImpl(const BC::Common::BlockIndex *index,
                           const BC::Proto::Block &block,
                           const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                           BlockInMemoryIndex &blockIndex,
                           BlockDatabase &blockDb) = 0;

  virtual void disconnectImpl(const BC::Common::BlockIndex *index,
                              const BC::Proto::Block &block,
                              const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                              BlockInMemoryIndex &blockIndex,
                              BlockDatabase &blockDb) = 0;

  // Fast-path counterparts: connect/disconnect applied by popping the
  // unflushed log instead of running the full impl. The pop is the whole
  // story for the log; a subclass keeping state outside it reverts that
  // state here
  virtual void connectFastImpl(const BC::Common::BlockIndex*,
                               const BC::Proto::Block&,
                               const BC::Proto::CBlockLinkedOutputs&) {}

  virtual void disconnectFastImpl(const BC::Common::BlockIndex*,
                                  const BC::Proto::Block&,
                                  const BC::Proto::CBlockLinkedOutputs&) {}

  // Async-flush counterpart of flushImpl: write the frozen log's content and
  // touch nothing else - the flusher drains the maps afterwards, the arena is
  // rewound by the writer when the log is recycled. Implemented only by
  // databases that call enableAsyncFlush()
  virtual void flushFrozenImpl(const BC::Proto::BlockHashTy&, size_t, CLog<CKey>&) {
    abort();
  }

protected:
  // Two logs per shard: the active one takes writes; a threshold flush
  // freezes it and installs the recycled spare in its place, the frozen one
  // is written out by the background flusher. With async flush disabled only
  // Logs[0] is ever used and behavior is identical to a single log
  struct CShardSlot {
    CLog<CKey> Logs[2];
    std::atomic<CLog<CKey>*> Active{&Logs[0]};
    std::atomic<CLog<CKey>*> Frozen{nullptr};
    std::atomic<bool> FlushBusy{false};
  };

  // writer-side view of a shard (exactly one writer thread)
  CLog<CKey> &shard(size_t i) { return *Shards_[i].Active.load(std::memory_order_relaxed); }

  // Reader-side pair. Probe order is active first, frozen second, and the
  // freeze publishes them in the opposite order: a reader that already sees
  // the new (empty) active is guaranteed to see the matching frozen, so a
  // committed key is always visible in some layer (active, frozen or disk)
  CLog<CKey> *activeLog(size_t i) const { return Shards_[i].Active.load(std::memory_order_acquire); }
  CLog<CKey> *frozenLog(size_t i) const { return Shards_[i].Frozen.load(std::memory_order_acquire); }

  void enableAsyncFlush() {
    if (AsyncFlushEnabled_)
      return;
    AsyncFlushEnabled_ = true;
    FlushThread_ = std::thread([this]() {
      loguru::set_thread_name((Name_ + ".flush").c_str());
      flushLoop();
    });
  }

  // Stops the flusher after draining its queue. The derived destructor must
  // call this (see ~Base)
  void shutdownAsyncFlush() {
    if (!AsyncFlushEnabled_)
      return;
    {
      std::lock_guard lock(FlushMutex_);
      FlushStop_ = true;
    }
    FlushWake_.notify_all();
    FlushThread_.join();
    AsyncFlushEnabled_ = false;
  }

  // Waits until every scheduled background flush has fully completed; the
  // caller then owns both logs of every shard
  void drainAsyncFlushes() {
    if (!AsyncFlushEnabled_)
      return;
    std::unique_lock lock(FlushMutex_);
    FlushDone_.wait(lock, [this]{ return FlushQueue_.empty(); });
  }

private:
  // Freezes the active log of a shard and hands it to the flusher. Runs in
  // the writer thread at a block boundary, so the frozen log always holds a
  // complete prefix ending at CurrentBlock_ - that block becomes the shard
  // stamp of the batch. If the spare log is still being flushed, waits for
  // it: memory stays bounded by two logs per shard
  void freezeAndScheduleFlush(size_t i) {
    CShardSlot &slot = Shards_[i];
    CLog<CKey> *active = slot.Active.load(std::memory_order_relaxed);
    CLog<CKey> *spare = active == &slot.Logs[0] ? &slot.Logs[1] : &slot.Logs[0];

    {
      std::unique_lock lock(FlushMutex_);
      FlushDone_.wait(lock, [&]{ return !slot.FlushBusy.load(std::memory_order_acquire); });
    }

    // The spare was fully written out by the flusher and stayed readable
    // since (a generation-based map needs no drain); the whole reset - map
    // generation bump plus arena rewind - happens here, in the writer, as
    // late as possible: a reader that fetched a pointer before the flush has
    // had a whole fill cycle to finish with it. (The residual race is the
    // pre-existing bug A class, closed for real by EBR in the aggkv design.)
    spare->reset();

    slot.Frozen.store(active, std::memory_order_release);
    slot.Active.store(spare, std::memory_order_release);

    slot.FlushBusy.store(true, std::memory_order_relaxed);
    {
      std::lock_guard lock(FlushMutex_);
      FlushQueue_.push_back(SFlushJob{i, active, CurrentBlock_});
    }
    FlushWake_.notify_one();
  }

  void flushLoop() {
    std::unique_lock lock(FlushMutex_);
    for (;;) {
      FlushWake_.wait(lock, [this]{ return !FlushQueue_.empty() || FlushStop_; });
      if (FlushQueue_.empty())
        return;

      // The job stays in the queue while it runs: drainAsyncFlushes() waits
      // for an empty queue, so completion means the write is durable. The
      // frozen log needs no cleanup here - it stays readable as-is until the
      // writer recycles it with an O(1) generation reset
      SFlushJob job = FlushQueue_.front();
      lock.unlock();
      flushFrozenImpl(job.Stamp, job.ShardIndex, *job.Log);
      lock.lock();
      FlushQueue_.pop_front();
      Shards_[job.ShardIndex].FlushBusy.store(false, std::memory_order_release);
      FlushDone_.notify_all();
    }
  }

  struct SFlushJob {
    size_t ShardIndex;
    CLog<CKey> *Log;
    BC::Proto::BlockHashTy Stamp;
  };

protected:
  // Configuration
  CBaseCfg BaseCfg_;

  // Database structures
  std::vector<CEntry> PendingBlocks_;
  std::unique_ptr<CShardSlot[]> Shards_;
  std::vector<std::unique_ptr<rocksdb::DB>> OnDiskStorage_;

  // Background flush machinery
  bool AsyncFlushEnabled_ = false;
  std::thread FlushThread_;
  std::mutex FlushMutex_;
  std::condition_variable FlushWake_;
  std::condition_variable FlushDone_;
  std::deque<SFlushJob> FlushQueue_;
  bool FlushStop_ = false;
};

template<typename CKey>
class CBaseKV : public Base<CKey> {
private:
  struct CHeader {
    CKey Key;
    size_t Size;
    bool IsConnected;
  };

public:
  CBaseKV(const std::string &name) : Base<CKey>(name) {}

  // The flusher dispatches virtual flushFrozenImpl implemented at this
  // level, so it must stop before this level is destroyed
  virtual ~CBaseKV() { this->shutdownAsyncFlush(); }
  rocksdb::MergeOperator *mergeOperator() final { return nullptr; }

  void add(const BC::Proto::BlockHashTy &blockId, const CKey &key, const void *data, size_t size) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->shard(hasher(key) % this->BaseCfg_.ShardsNum);

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader) + size));
    header->Key = key;
    header->Size = size;
    header->IsConnected = true;

    void *buffer = header + 1;
    memcpy(buffer, data, size);

    shard.update(blockId, key, header);
  }

  void remove(const BC::Proto::BlockHashTy &blockId, const CKey &key) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->shard(hasher(key) % this->BaseCfg_.ShardsNum);

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader)));
    header->Key = key;
    header->Size = 0;
    header->IsConnected = false;

    shard.update(blockId, key, header);
  }

  bool find(const CKey &key, std::function<void(const void*, size_t)> callback) const {
    std::hash<CKey> hasher;
    size_t shardIndex = hasher(key) % this->BaseCfg_.ShardsNum;

    // Active first, frozen second: the newest version of a key shadows the
    // frozen one and a tombstone in the active log must hide a frozen value
    const CHeader *header = static_cast<const CHeader*>(this->activeLog(shardIndex)->find(key));
    if (!header) {
      if (CLog<CKey> *frozen = this->frozenLog(shardIndex))
        header = static_cast<const CHeader*>(frozen->find(key));
    }
    if (header) {
      if (header->IsConnected) {
        callback(header + 1, header->Size);
        return true;
      } else {
        return false;
      }
    }

    // find in storage
    auto &storage = this->OnDiskStorage_[shardIndex];
    rocksdb::Slice keySlice(reinterpret_cast<const char*>(&key), sizeof(CKey));
    std::string value;
    if (storage->Get(rocksdb::ReadOptions(), keySlice, &value).ok()) {
      callback(value.data(), value.size());
      return true;
    }

    return false;
  }

  virtual void flushImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex) final {
    CLog<CKey> &shard = this->shard(shardIndex);
    writeLog(blockId, shardIndex, shard);
    shard.reset();
  }

  void flushFrozenImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex, CLog<CKey> &log) final {
    writeLog(blockId, shardIndex, log);
  }

private:
  // Builds and writes the batch of one log's final values; shared by the
  // synchronous flush and the background flusher
  void writeLog(const BC::Proto::BlockHashTy &blockId, size_t shardIndex, CLog<CKey> &shard) {
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();
    rocksdb::WriteBatch batch;

    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(blockId.begin()), sizeof(BC::Proto::BlockHashTy)));

    shard.Map.forEachCurrent([&batch](const CKey&, void *keyData) {
      const CHeader *header = static_cast<const CHeader*>(keyData);
      const void *data = header + 1;

      auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(&header->Key), sizeof(CKey));
      if (header->IsConnected) {
        auto valueSlice = rocksdb::Slice(static_cast<const char*>(data), header->Size);
        batch.Put(keySlice, valueSlice);
      } else {
        batch.Delete(keySlice);
      }
    });

    storage->Write(rocksdb::WriteOptions(), &batch);
  }
};

template<typename CKey>
class CBaseArrayFixed : public Base<CKey> {
private:
#pragma pack(push, 1)
  struct CHeader {
    CKey Key;
    size_t Size;
    int64_t Count;
    int64_t ChunkOffset;
  };


  struct CChunkKey {
    CKey Key;
    uint64_t Index;
  };
#pragma pack(pop)

private:
  class MergeOperator : public rocksdb::MergeOperator {
    virtual bool FullMerge(const rocksdb::Slice&,
                           const rocksdb::Slice *existing_value,
                           const std::deque<std::string> &operand_list,
                           std::string *new_value,
                           rocksdb::Logger*) const override {
      assert(existing_value);
      new_value->assign(existing_value->data(), existing_value->data() + existing_value->size());
      for (const auto &operand: operand_list) {
        assert(operand.size() >= 8);

        uint64_t offset = *reinterpret_cast<const uint64_t*>(operand.data());
        uint64_t requiredSize = offset + operand.size() - sizeof(uint64_t);
        if (new_value->size() < requiredSize)
          new_value->resize(requiredSize);

        memcpy(new_value->data() + offset, operand.data() + sizeof(uint64_t), operand.size() - sizeof(uint64_t));
      }
      return true;
    }

    virtual bool PartialMerge(const rocksdb::Slice&,
                              const rocksdb::Slice &left,
                              const rocksdb::Slice &right,
                              std::string *out,
                              rocksdb::Logger*) const override {
      assert(left.size() >= 8);
      assert(right.size() >= 8);
      [[maybe_unused]] uint64_t leftChunkOffset = *reinterpret_cast<const uint64_t*>(left.data());
      [[maybe_unused]] uint64_t rightChunkOffset = *reinterpret_cast<const uint64_t*>(right.data());
      assert(leftChunkOffset + left.size() - sizeof(uint64_t) == rightChunkOffset);

      out->assign(left.data(), left.data() + left.size());
      out->append(right.data() + sizeof(uint64_t), right.data() + right.size());
      return true;
    }

    virtual const char* Name() const override {
      return "CBaseArrayFixed";
    }
  };

public:
  CBaseArrayFixed(const std::string &name, size_t valueSize, size_t chunkSize) :
    Base<CKey>(name), ValueSize_(valueSize), ChunkSize_(chunkSize) {}
  virtual ~CBaseArrayFixed() {}

  rocksdb::MergeOperator *mergeOperator() final { return new MergeOperator(); }

  void add(const BC::Proto::BlockHashTy &blockId, const CKey &key, const void *data, size_t size, size_t count) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->shard(hasher(key) % this->BaseCfg_.ShardsNum);

    // get current array size
    const void *prevData = nullptr;
    size_t prevSize = 0;
    int64_t prevCount = 0;
    const CHeader *oldHeader = static_cast<const CHeader*>(shard.find(key));
    if (oldHeader) {
      prevData = oldHeader + 1;
      prevSize = oldHeader->Size;
      prevCount = oldHeader->Count;
    }

    // allocate new buffer
    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader) + prevSize + size));
    uint8_t *dst = reinterpret_cast<uint8_t*>(header + 1);
    header->Key = key;
    header->Size = prevSize + size;
    header->Count = prevCount + count;
    if (prevData)
      memcpy(dst, prevData, prevSize);
    memcpy(dst + prevSize, data, size);

    shard.update(blockId, key, header);
  }

  void remove(const BC::Proto::BlockHashTy &blockId, const CKey &key, size_t count) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->shard(hasher(key) % this->BaseCfg_.ShardsNum);

    // get current array size
    int64_t prevCount = 0;
    const CHeader *oldHeader = static_cast<const CHeader*>(shard.find(key));
    if (oldHeader) {
      prevCount = oldHeader->Count;
    }

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader)));
    header->Key = key;
    header->Size = 0;
    header->Count = prevCount - count;

    shard.update(blockId, key, header);
  }

  bool query(const CKey &key, size_t from, size_t count, xmstream &result, size_t *totalTxCount) {
    std::hash<CKey> hasher;
    size_t shardIdx = hasher(key) % this->BaseCfg_.ShardsNum;
    CLog<CKey> &mlog = this->shard(shardIdx);
    rocksdb::DB *storage = this->OnDiskStorage_[shardIdx].get();

    if (count == 0)
      return false;
    size_t firstChunk = from / ChunkSize_;
    size_t lastChunk = (from + count - 1) / ChunkSize_;

    std::vector<std::string> readResult;

    {
      std::vector<CChunkKey> chunkKeys;
      std::vector<rocksdb::Slice> allKeySlices;

      // add metadata key
      allKeySlices.push_back(rocksdb::Slice(reinterpret_cast<const char*>(&key), sizeof(CKey)));
      // add keys for all chunks
      for (size_t chunkId = firstChunk; chunkId <= lastChunk; chunkId++) {
        CChunkKey &k = chunkKeys.emplace_back();
        k.Key = key;
        k.Index = xhtobe<uint64_t>(chunkId);
      }
      for (const auto &k: chunkKeys)
        allKeySlices.push_back(rocksdb::Slice(reinterpret_cast<const char*>(&k), sizeof(CChunkKey)));

      auto metadataReadResult = storage->MultiGet(rocksdb::ReadOptions(), allKeySlices, &readResult);
      // Missing metadata is not an error: the key may live only in the memtable tail
      if (!metadataReadResult[0].ok())
        readResult[0].clear();
    }

    int64_t onDiskArraySize = readResult[0].size() == sizeof(uint64_t) ? *reinterpret_cast<const int64_t*>(readResult[0].data()) : 0;
    int64_t onDiskAvailable = std::min((int64_t)(from + count), onDiskArraySize) - from;
    int64_t inMemoryOffset = std::max((int64_t)from - onDiskArraySize, (int64_t)0);
    int64_t remaining = count;
    *totalTxCount = onDiskArraySize;

    // Read disk data
    if (onDiskAvailable > 0) {
      int64_t chunkOffset = from % ChunkSize_;
      for (size_t chunkId = firstChunk, index = 1; chunkId <= lastChunk; chunkId++, index++) {
        const char *chunkData = readResult[index].data();
        int64_t count = readResult[index].size() / ValueSize_;
        int64_t available = count - chunkOffset;
        if (available <= 0)
          break;
        int64_t needToRead = std::min(available, remaining);

        memcpy(result.reserve(needToRead * ValueSize_), chunkData + chunkOffset * ValueSize_, needToRead * ValueSize_);
        remaining -= needToRead;
        chunkOffset = 0;
      }
    }

    // Read tail from memory (inMemoryOffset, remaining)
    const CHeader *header = static_cast<const CHeader*>(mlog.find(key));
    if (header) {
      const uint8_t *memoryData = reinterpret_cast<const uint8_t*>(header + 1);
      int64_t available = header->Count - inMemoryOffset;
      int64_t needToRead = std::min(available, remaining);
      if (needToRead > 0)
        memcpy(result.reserve(needToRead * ValueSize_), memoryData + inMemoryOffset*ValueSize_, needToRead * ValueSize_);
      *totalTxCount += header->Count;
    }

    // TODO: consistency check using MLog::generation
    return true;
  }

  virtual void flushImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex) final {
    CLog<CKey> &shard = this->shard(shardIndex);
    MLog &mlog = shard.Log;
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();

    // Collect all keys
    std::vector<CKey> allKeys;
    std::vector<CHeader*> allData;
    std::vector<rocksdb::Slice> allKeySlices;
    std::vector<std::string> metadata;

    shard.Map.forEachCurrent([&](const CKey &key, void *keyData) {
      allKeys.emplace_back(key);
      allData.push_back(static_cast<CHeader*>(keyData));
    });

    // Make slices for metadata reading
    for (size_t i = 0; i < allKeys.size(); i++)
      allKeySlices.emplace_back((const char*)&allKeys[i], sizeof(CKey));

    // Read metadata
    auto metadataReadResult = storage->MultiGet(rocksdb::ReadOptions(), allKeySlices, &metadata);

    // Processing keys
    rocksdb::WriteBatch batch;

    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(blockId.begin()), sizeof(BC::Proto::BlockHashTy)));
    for (size_t i = 0; i < allKeys.size(); i++) {
      CHeader *header = allData[i];
      int64_t currentArraySize = 0;
      if (metadataReadResult[i].ok() && metadata[i].size() == sizeof(uint64_t))
        currentArraySize = *(int64_t*)metadata[i].data();

      int64_t currentChunksNum = currentArraySize / ChunkSize_ + (currentArraySize % ChunkSize_ != 0);
      int64_t newArraySize = currentArraySize + header->Count;
      if (newArraySize < 0)
        newArraySize = 0;

      int64_t newChunksNum = newArraySize / ChunkSize_ + (newArraySize % ChunkSize_ != 0);

      // Update metadata
      {
        auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(&header->Key), sizeof(CKey));
        if (newArraySize) {
          int64_t *arraySize = mlog.alloc<int64_t>();
          *arraySize = newArraySize;
          batch.Put(keySlice, rocksdb::Slice((const char*)arraySize, sizeof(int64_t)));
        } else {
          batch.Delete(keySlice);
        }
      }

      if (newChunksNum < currentChunksNum) {
        // Delete empty chunks
        for (int64_t chunkIdx = newChunksNum; chunkIdx < currentChunksNum; chunkIdx++) {
          CChunkKey *chunkKey = mlog.alloc<CChunkKey>();
          chunkKey->Key = allKeys[i];
          chunkKey->Index = chunkIdx;
          auto slice = rocksdb::Slice(reinterpret_cast<const char*>(chunkKey), sizeof(CChunkKey));
          batch.Delete(slice);
        }
      } else {
        size_t remaining = header->Count;
        int64_t offset = currentArraySize;
        const char *dataPtr = reinterpret_cast<const char*>(&header->ChunkOffset);

        if (currentArraySize % ChunkSize_) {
          // Merge last chunk
          size_t writeSize = std::min(remaining, ChunkSize_ - (size_t)(currentArraySize % ChunkSize_));

          CChunkKey *chunkKey = mlog.alloc<CChunkKey>();
          chunkKey->Key = allKeys[i];
          chunkKey->Index = xhtobe<int64_t>(currentChunksNum - 1);
          header->ChunkOffset = ValueSize_ * (offset % ChunkSize_);
          auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(chunkKey), sizeof(CChunkKey));
          auto valueSlice = rocksdb::Slice(dataPtr, writeSize * ValueSize_ + sizeof(int64_t));
          batch.Merge(keySlice, valueSlice);

          dataPtr += writeSize * ValueSize_ + sizeof(int64_t);
          remaining -= writeSize;
          offset += writeSize;
        } else {
          dataPtr += sizeof(uint64_t);
        }

        for (int64_t chunkIdx = currentChunksNum; chunkIdx < newChunksNum; chunkIdx++) {
          size_t writeSize = std::min(remaining, ChunkSize_);
          assert(writeSize != 0);

          CChunkKey *chunkKey = mlog.alloc<CChunkKey>();
          chunkKey->Key = allKeys[i];
          chunkKey->Index = xhtobe<int64_t>(chunkIdx);
          auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(chunkKey), sizeof(CChunkKey));
          auto valueSlice = rocksdb::Slice(dataPtr, writeSize * ValueSize_);
          batch.Put(keySlice, valueSlice);

          dataPtr += writeSize * ValueSize_;
          remaining -= writeSize;
          offset += writeSize;
        }
      }
    }

    storage->Write(rocksdb::WriteOptions(), &batch);
    shard.reset();
  }

public:
  size_t ValueSize_ = 0;
  size_t ChunkSize_ = 0;
};

// Fixed-size element array with a running aggregate: besides the fact itself,
// every element carries in its Aggregate field the fold of all elements up to
// and including it, so a range read returns ready prefix sums (address history
// with the balance after each tx, balance chart).
// The write path stays as blind as CBaseArrayFixed: add() receives elements
// whose Aggregate holds the element DELTA and folds them into window-relative
// running sums (uint64 wrap-around, a negative delta is two's complement);
// flushImpl() rebases the window by the durable aggregate kept in the per-key
// metadata it reads anyway, so chunk data still leaves as offset-patch merge
// operands with no extra reads on the hot path. Truncation (disconnect) keeps
// every surviving prefix sum valid by construction; only the reorg-only
// truncate path reads one chunk back to restore the metadata aggregate.
// CItem requirements: trivially copyable, fixed size, public uint64_t
// Aggregate field with the semantics above.
template<typename CKey, typename CItem>
class CBaseArrayAggregated : public Base<CKey> {
private:
#pragma pack(push, 1)
  struct CHeader {
    CKey Key;
    size_t Size;
    int64_t Count;
    int64_t ChunkOffset;
  };

  struct CChunkKey {
    CKey Key;
    uint64_t Index;
  };

  // Per-key metadata row: element count plus the aggregate of the last durable
  // element - the rebase point for the unflushed window
  struct CMetadata {
    int64_t Count;
    uint64_t Aggregate;
  };
#pragma pack(pop)

private:
  class MergeOperator : public rocksdb::MergeOperator {
    virtual bool FullMerge(const rocksdb::Slice&,
                           const rocksdb::Slice *existing_value,
                           const std::deque<std::string> &operand_list,
                           std::string *new_value,
                           rocksdb::Logger*) const override {
      if (existing_value)
        new_value->assign(existing_value->data(), existing_value->data() + existing_value->size());
      else
        new_value->clear();
      for (const auto &operand: operand_list) {
        assert(operand.size() >= 8);

        uint64_t offset = *reinterpret_cast<const uint64_t*>(operand.data());
        uint64_t requiredSize = offset + operand.size() - sizeof(uint64_t);
        if (new_value->size() < requiredSize)
          new_value->resize(requiredSize);

        memcpy(new_value->data() + offset, operand.data() + sizeof(uint64_t), operand.size() - sizeof(uint64_t));
      }
      return true;
    }

    virtual bool PartialMerge(const rocksdb::Slice&,
                              const rocksdb::Slice &left,
                              const rocksdb::Slice &right,
                              std::string *out,
                              rocksdb::Logger*) const override {
      assert(left.size() >= 8);
      assert(right.size() >= 8);
      [[maybe_unused]] uint64_t leftChunkOffset = *reinterpret_cast<const uint64_t*>(left.data());
      [[maybe_unused]] uint64_t rightChunkOffset = *reinterpret_cast<const uint64_t*>(right.data());
      assert(leftChunkOffset + left.size() - sizeof(uint64_t) == rightChunkOffset);

      out->assign(left.data(), left.data() + left.size());
      out->append(right.data() + sizeof(uint64_t), right.data() + right.size());
      return true;
    }

    virtual const char* Name() const override {
      return "CBaseArrayAggregated";
    }
  };

public:
  CBaseArrayAggregated(const std::string &name, size_t chunkSize) :
    Base<CKey>(name), ChunkSize_(chunkSize) {}
  virtual ~CBaseArrayAggregated() {}

  rocksdb::MergeOperator *mergeOperator() final { return new MergeOperator(); }

  // items carry the element delta in Aggregate; the call folds them into
  // window-relative running sums, rebased to absolute values at flush
  void add(const BC::Proto::BlockHashTy &blockId, const CKey &key, const CItem *items, size_t count) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->shard(hasher(key) % this->BaseCfg_.ShardsNum);

    const void *prevData = nullptr;
    size_t prevSize = 0;
    int64_t prevCount = 0;
    const CHeader *oldHeader = static_cast<const CHeader*>(shard.find(key));
    if (oldHeader) {
      prevData = oldHeader + 1;
      prevSize = oldHeader->Size;
      prevCount = oldHeader->Count;
    }

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader) + prevSize + count*sizeof(CItem)));
    uint8_t *dst = reinterpret_cast<uint8_t*>(header + 1);
    header->Key = key;
    header->Size = prevSize + count*sizeof(CItem);
    header->Count = prevCount + count;
    if (prevData)
      memcpy(dst, prevData, prevSize);

    CItem *newItems = reinterpret_cast<CItem*>(dst + prevSize);
    memcpy(static_cast<void*>(newItems), items, count*sizeof(CItem));
    uint64_t running = prevSize ? reinterpret_cast<const CItem*>(dst + prevSize - sizeof(CItem))->Aggregate : 0;
    for (size_t i = 0; i < count; i++) {
      running += newItems[i].Aggregate;
      newItems[i].Aggregate = running;
    }

    shard.update(blockId, key, header);
  }

  void remove(const BC::Proto::BlockHashTy &blockId, const CKey &key, size_t count) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->shard(hasher(key) % this->BaseCfg_.ShardsNum);

    int64_t prevCount = 0;
    const CHeader *oldHeader = static_cast<const CHeader*>(shard.find(key));
    if (oldHeader)
      prevCount = oldHeader->Count;

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader)));
    header->Key = key;
    header->Size = 0;
    header->Count = prevCount - count;

    shard.update(blockId, key, header);
  }

  bool query(const CKey &key, size_t from, size_t count, xmstream &result, size_t *totalCount) {
    std::hash<CKey> hasher;
    size_t shardIdx = hasher(key) % this->BaseCfg_.ShardsNum;
    CLog<CKey> &mlog = this->shard(shardIdx);
    rocksdb::DB *storage = this->OnDiskStorage_[shardIdx].get();

    if (count == 0)
      return false;
    size_t firstChunk = from / ChunkSize_;
    size_t lastChunk = (from + count - 1) / ChunkSize_;

    std::vector<std::string> readResult;

    {
      std::vector<CChunkKey> chunkKeys;
      std::vector<rocksdb::Slice> allKeySlices;

      // add metadata key
      allKeySlices.push_back(rocksdb::Slice(reinterpret_cast<const char*>(&key), sizeof(CKey)));
      // add keys for all chunks
      for (size_t chunkId = firstChunk; chunkId <= lastChunk; chunkId++) {
        CChunkKey &k = chunkKeys.emplace_back();
        k.Key = key;
        k.Index = xhtobe<uint64_t>(chunkId);
      }
      for (const auto &k: chunkKeys)
        allKeySlices.push_back(rocksdb::Slice(reinterpret_cast<const char*>(&k), sizeof(CChunkKey)));

      auto metadataReadResult = storage->MultiGet(rocksdb::ReadOptions(), allKeySlices, &readResult);
      // Missing metadata is not an error: the key may live only in the memtable tail
      if (!metadataReadResult[0].ok())
        readResult[0].clear();
    }

    int64_t onDiskArraySize = 0;
    uint64_t onDiskAggregate = 0;
    if (readResult[0].size() == sizeof(CMetadata)) {
      const CMetadata *meta = reinterpret_cast<const CMetadata*>(readResult[0].data());
      onDiskArraySize = meta->Count;
      onDiskAggregate = meta->Aggregate;
    }

    int64_t onDiskAvailable = std::min((int64_t)(from + count), onDiskArraySize) - from;
    int64_t inMemoryOffset = std::max((int64_t)from - onDiskArraySize, (int64_t)0);
    int64_t remaining = count;
    *totalCount = onDiskArraySize;

    // Read disk data
    if (onDiskAvailable > 0) {
      int64_t chunkOffset = from % ChunkSize_;
      // Bounded by the durable count, not the chunk payload: after a truncation
      // the last chunk keeps stale bytes past the array end
      int64_t diskRemaining = onDiskAvailable;
      for (size_t chunkId = firstChunk, index = 1; chunkId <= lastChunk && diskRemaining > 0; chunkId++, index++) {
        const char *chunkData = readResult[index].data();
        int64_t elementsNum = readResult[index].size() / sizeof(CItem);
        int64_t available = elementsNum - chunkOffset;
        if (available <= 0)
          break;
        int64_t needToRead = std::min(available, diskRemaining);

        memcpy(result.reserve(needToRead * sizeof(CItem)), chunkData + chunkOffset * sizeof(CItem), needToRead * sizeof(CItem));
        diskRemaining -= needToRead;
        remaining -= needToRead;
        chunkOffset = 0;
      }
    }

    // Window tail: rebase window-relative running sums by the durable aggregate
    const CHeader *header = static_cast<const CHeader*>(mlog.find(key));
    if (header) {
      const uint8_t *memoryData = reinterpret_cast<const uint8_t*>(header + 1);
      int64_t windowCount = header->Size / sizeof(CItem);
      int64_t available = windowCount - inMemoryOffset;
      int64_t needToRead = std::min(available, remaining);
      if (needToRead > 0) {
        CItem *items = static_cast<CItem*>(result.reserve(needToRead * sizeof(CItem)));
        memcpy(static_cast<void*>(items), memoryData + inMemoryOffset * sizeof(CItem), needToRead * sizeof(CItem));
        for (int64_t i = 0; i < needToRead; i++)
          items[i].Aggregate += onDiskAggregate;
      }
      *totalCount += header->Count;
    }

    return true;
  }

  virtual void flushImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex) final {
    CLog<CKey> &shard = this->shard(shardIndex);
    MLog &mlog = shard.Log;
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();

    // Collect all keys
    std::vector<CKey> allKeys;
    std::vector<CHeader*> allData;
    std::vector<rocksdb::Slice> allKeySlices;
    std::vector<std::string> metadata;

    shard.Map.forEachCurrent([&](const CKey &key, void *keyData) {
      allKeys.emplace_back(key);
      allData.push_back(static_cast<CHeader*>(keyData));
    });

    for (size_t i = 0; i < allKeys.size(); i++)
      allKeySlices.emplace_back((const char*)&allKeys[i], sizeof(CKey));

    auto metadataReadResult = storage->MultiGet(rocksdb::ReadOptions(), allKeySlices, &metadata);

    rocksdb::WriteBatch batch;

    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(blockId.begin()), sizeof(BC::Proto::BlockHashTy)));
    for (size_t i = 0; i < allKeys.size(); i++) {
      CHeader *header = allData[i];
      int64_t currentArraySize = 0;
      uint64_t currentAggregate = 0;
      if (metadataReadResult[i].ok() && metadata[i].size() == sizeof(CMetadata)) {
        const CMetadata *currentMeta = reinterpret_cast<const CMetadata*>(metadata[i].data());
        currentArraySize = currentMeta->Count;
        currentAggregate = currentMeta->Aggregate;
      }

      int64_t currentChunksNum = currentArraySize / ChunkSize_ + (currentArraySize % ChunkSize_ != 0);
      int64_t newArraySize = currentArraySize + header->Count;
      if (newArraySize < 0)
        newArraySize = 0;

      int64_t newChunksNum = newArraySize / ChunkSize_ + (newArraySize % ChunkSize_ != 0);

      // Rebase window-relative running sums to absolute values
      CItem *windowItems = reinterpret_cast<CItem*>(header + 1);
      int64_t windowCount = header->Size / sizeof(CItem);
      for (int64_t j = 0; j < windowCount; j++)
        windowItems[j].Aggregate += currentAggregate;

      uint64_t newAggregate = windowCount ? windowItems[windowCount - 1].Aggregate : currentAggregate;

      if (header->Count != windowCount) {
        // The window contains removes - a reorg deeper than the unflushed window.
        // Only pure truncation is supported: drop the chunks past the new end and
        // take the aggregate back from the new last durable element. Elements
        // appended over a truncated window would land on stale positions and are
        // not written (same layout limitation as CBaseArrayFixed)
        if (windowCount)
          LOG_F(WARNING, "%s: dropping %lld elements appended over a truncated window", this->name().c_str(), (long long)windowCount);

        for (int64_t chunkIdx = newChunksNum; chunkIdx < currentChunksNum; chunkIdx++) {
          CChunkKey *chunkKey = mlog.alloc<CChunkKey>();
          chunkKey->Key = allKeys[i];
          chunkKey->Index = xhtobe<uint64_t>(chunkIdx);
          batch.Delete(rocksdb::Slice(reinterpret_cast<const char*>(chunkKey), sizeof(CChunkKey)));
        }

        if (newArraySize > 0) {
          CChunkKey chunkKey;
          chunkKey.Key = allKeys[i];
          chunkKey.Index = xhtobe<uint64_t>((newArraySize - 1) / ChunkSize_);
          size_t offsetInChunk = (newArraySize - 1) % ChunkSize_;
          std::string chunkData;
          if (storage->Get(rocksdb::ReadOptions(), rocksdb::Slice(reinterpret_cast<const char*>(&chunkKey), sizeof(CChunkKey)), &chunkData).ok() &&
              chunkData.size() >= (offsetInChunk + 1) * sizeof(CItem)) {
            newAggregate = reinterpret_cast<const CItem*>(chunkData.data())[offsetInChunk].Aggregate;
          } else {
            LOG_F(ERROR, "%s: can't read back the aggregate of element %lld on truncation", this->name().c_str(), (long long)(newArraySize - 1));
          }
        }
      } else if (windowCount) {
        size_t remaining = windowCount;
        int64_t offset = currentArraySize;
        const char *dataPtr = reinterpret_cast<const char*>(&header->ChunkOffset);

        if (currentArraySize % ChunkSize_) {
          // Extend the partial last chunk with an offset-patch merge operand
          size_t writeSize = std::min(remaining, ChunkSize_ - (size_t)(currentArraySize % ChunkSize_));

          CChunkKey *chunkKey = mlog.alloc<CChunkKey>();
          chunkKey->Key = allKeys[i];
          chunkKey->Index = xhtobe<uint64_t>(currentChunksNum - 1);
          header->ChunkOffset = sizeof(CItem) * (offset % ChunkSize_);
          auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(chunkKey), sizeof(CChunkKey));
          auto valueSlice = rocksdb::Slice(dataPtr, writeSize * sizeof(CItem) + sizeof(int64_t));
          batch.Merge(keySlice, valueSlice);

          dataPtr += writeSize * sizeof(CItem) + sizeof(int64_t);
          remaining -= writeSize;
          offset += writeSize;
        } else {
          dataPtr += sizeof(uint64_t);
        }

        for (int64_t chunkIdx = currentChunksNum; chunkIdx < newChunksNum; chunkIdx++) {
          size_t writeSize = std::min(remaining, ChunkSize_);
          if (writeSize == 0)
            break;

          CChunkKey *chunkKey = mlog.alloc<CChunkKey>();
          chunkKey->Key = allKeys[i];
          chunkKey->Index = xhtobe<uint64_t>(chunkIdx);
          auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(chunkKey), sizeof(CChunkKey));
          auto valueSlice = rocksdb::Slice(dataPtr, writeSize * sizeof(CItem));
          batch.Put(keySlice, valueSlice);

          dataPtr += writeSize * sizeof(CItem);
          remaining -= writeSize;
          offset += writeSize;
        }
      }

      // Update metadata
      {
        auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(&header->Key), sizeof(CKey));
        if (newArraySize) {
          CMetadata *meta = mlog.alloc<CMetadata>();
          meta->Count = newArraySize;
          meta->Aggregate = newAggregate;
          batch.Put(keySlice, rocksdb::Slice(reinterpret_cast<const char*>(meta), sizeof(CMetadata)));
        } else {
          batch.Delete(keySlice);
        }
      }
    }

    storage->Write(rocksdb::WriteOptions(), &batch);
    shard.reset();
  }

private:
  size_t ChunkSize_ = 0;
};

// Additive aggregation database: memtable holds a folded delta per key.
// Two flush modes, chosen by the configured rank index set ("indexes" config list):
//  - no indexes: the folded window delta goes to the backend as a merge operand,
//    no reads on the write path at all (IBD/reindex fast path);
//  - indexes enabled: RMW - batched MultiGet of old values, materialized Put and
//    replacement of the rank index rows in the same atomic batch.
// Shard key space is split into disjoint prefix regions:
//   data rows    'd' ++ key
//   index rows   'i' ++ indexId ++ be64(~metric) ++ key   (same shard as the data row)
//   service keys "stamp", "basecfg", "xcfg" - outside both regions.
// Disjointness is what makes an index droppable by a range tombstone and
// buildable by a single scan of the data region.
// CValue requirements: trivially copyable, default-constructed state is the identity,
// merge() commutative/associative, negate() gives the inverse delta for disconnect.
template<typename CKey, typename CValue>
class CBaseMerge : public Base<CKey> {
protected:
  struct CIndexDef {
    std::string Name;
    uint64_t (*Extract)(const CValue&);
  };

private:
  struct CHeader {
    CKey Key;
    CValue Value;
  };

  struct CActiveIndex {
    uint8_t Id;
    CIndexDef Def;
  };

#pragma pack(push, 1)
  struct CDataRowKey {
    uint8_t Prefix;
    CKey Key;
  };

  struct CIndexRowKey {
    uint8_t Prefix;
    uint8_t IndexId;
    uint64_t InvertedValue;
    CKey Key;
  };
#pragma pack(pop)

  static void makeDataRowKey(CDataRowKey &rowKey, const CKey &key) {
    rowKey.Prefix = 'd';
    rowKey.Key = key;
  }

  static void makeIndexRowKey(CIndexRowKey &rowKey, uint8_t indexId, uint64_t value, const CKey &key) {
    rowKey.Prefix = 'i';
    rowKey.IndexId = indexId;
    rowKey.InvertedValue = xhtobe<uint64_t>(~value);
    rowKey.Key = key;
  }

  class MergeOperator : public rocksdb::AssociativeMergeOperator {
    virtual bool Merge(const rocksdb::Slice&,
                      const rocksdb::Slice *existing_value,
                      const rocksdb::Slice &value,
                      std::string *new_value,
                      rocksdb::Logger*) const override {
      if (value.size() != sizeof(CValue))
        return false;
      if (existing_value && existing_value->size() != sizeof(CValue))
        return false;

      // Missing base is legal: the key is being touched for the first time
      CValue result;
      if (existing_value)
        memcpy(&result, existing_value->data(), sizeof(CValue));

      CValue delta;
      memcpy(&delta, value.data(), sizeof(CValue));
      result.merge(delta);

      new_value->assign(reinterpret_cast<const char*>(&result), sizeof(CValue));
      return true;
    }

    virtual const char* Name() const override {
      return "CBaseMerge";
    }
  };

public:
  CBaseMerge(const std::string &name) : Base<CKey>(name) {}
  virtual ~CBaseMerge() {}
  rocksdb::MergeOperator *mergeOperator() final { return new MergeOperator(); }

  bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage&) final {
    ShardLocks_.reset(new std::shared_mutex[this->BaseCfg_.ShardsNum]);

    // Resolve the configured index list against the set registered by the subclass
    config4cpp::StringVector names;
    cfg->lookupList(this->Name_.c_str(), "indexes", names, config4cpp::StringVector());
    std::vector<bool> enabled(RegisteredIndexes_.size(), false);
    for (int i = 0; i < names.length(); i++) {
      bool found = false;
      for (size_t index = 0; index < RegisteredIndexes_.size(); index++) {
        if (RegisteredIndexes_[index].Name == names[i]) {
          found = true;
          enabled[index] = true;
          break;
        }
      }
      if (!found) {
        LOG_F(ERROR, "database '%s' has no index '%s'", this->Name_.c_str(), names[i]);
        return false;
      }
    }

    // Canonical form: registration order, so the config list order does not matter
    std::string indexCfg;
    for (size_t i = 0; i < RegisteredIndexes_.size(); i++) {
      if (!enabled[i])
        continue;
      CActiveIndex &index = ActiveIndexes_.emplace_back();
      index.Id = static_cast<uint8_t>(i);
      index.Def = RegisteredIndexes_[i];
      if (!indexCfg.empty())
        indexCfg.push_back(',');
      indexCfg.append(index.Def.Name);
    }

    // Bring the index rows of every shard in sync with the configured set.
    // The transition is a per-index diff: a dropped index is erased by a range
    // tombstone over its region, an added one is built by a single scan of the
    // data region; untouched indexes are not rebuilt and full chain reindex is
    // never needed. A stored name that is no longer registered has an unknown
    // id - that degenerates to "drop everything, build the active set"
    for (size_t shardIndex = 0; shardIndex < this->BaseCfg_.ShardsNum; shardIndex++) {
      rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();

      std::string storedCfg;
      storage->Get(rocksdb::ReadOptions(), rocksdb::Slice("xcfg"), &storedCfg);
      if (storedCfg == indexCfg)
        continue;

      std::vector<bool> stored(RegisteredIndexes_.size(), false);
      bool storedKnown = true;
      for (size_t pos = 0; pos < storedCfg.size() && storedKnown; ) {
        size_t comma = storedCfg.find(',', pos);
        if (comma == std::string::npos)
          comma = storedCfg.size();
        std::string name = storedCfg.substr(pos, comma - pos);
        pos = comma + 1;

        storedKnown = false;
        for (size_t i = 0; i < RegisteredIndexes_.size(); i++) {
          if (RegisteredIndexes_[i].Name == name) {
            stored[i] = true;
            storedKnown = true;
            break;
          }
        }
      }

      // No stamp - no data rows, nothing to build: the rows of a fresh shard
      // are maintained by regular flushes starting from the first block
      std::string stampData;
      bool hasData = storage->Get(rocksdb::ReadOptions(), rocksdb::Slice("stamp"), &stampData).ok();

      std::vector<const CActiveIndex*> forBuild;
      if (!storedKnown) {
        LOG_F(INFO, "%s: dropping all index rows for shard %zu", this->Name_.c_str(), shardIndex);
        if (!dropAllIndexRows(shardIndex))
          return false;
        for (const auto &index: ActiveIndexes_)
          forBuild.push_back(&index);
      } else {
        for (size_t i = 0; i < RegisteredIndexes_.size(); i++) {
          if (stored[i] && !enabled[i]) {
            LOG_F(INFO, "%s: dropping index '%s' for shard %zu", this->Name_.c_str(), RegisteredIndexes_[i].Name.c_str(), shardIndex);
            if (!dropIndexRows(shardIndex, static_cast<uint8_t>(i)))
              return false;
          }
        }
        for (const auto &index: ActiveIndexes_) {
          if (!stored[index.Id])
            forBuild.push_back(&index);
        }
      }

      if (hasData && !forBuild.empty() && !buildIndexes(shardIndex, forBuild))
        return false;

      if (!storage->Put(rocksdb::WriteOptions(), rocksdb::Slice("xcfg"), rocksdb::Slice(indexCfg)).ok())
        return false;
    }

    return true;
  }

  void merge(const BC::Proto::BlockHashTy &blockId, const CKey &key, const CValue &delta) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->shard(hasher(key) % this->BaseCfg_.ShardsNum);

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader)));
    header->Key = key;
    const CHeader *prev = static_cast<const CHeader*>(shard.find(key));
    header->Value = prev ? prev->Value : CValue();
    header->Value.merge(delta);

    shard.update(blockId, key, header);
  }

  bool find(const CKey &key, CValue &value) const {
    std::hash<CKey> hasher;
    size_t shardIndex = hasher(key) % this->BaseCfg_.ShardsNum;
    // async flush is never enabled for merge tables, the frozen log can't exist
    CLog<CKey> &shard = *this->activeLog(shardIndex);
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();

    // Shared lock pairs the on-disk state with the memtable window: flush publishes
    // batch and memtable reset under the exclusive lock, so the sum below can't
    // count the window twice and can't touch the arena while it is being reused.
    // Temporary measure, see the note at ShardLocks_
    std::shared_lock lock(ShardLocks_[shardIndex]);

    value = CValue();
    CDataRowKey rowKey;
    makeDataRowKey(rowKey, key);
    rocksdb::Slice keySlice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey));
    std::string data;
    if (storage->Get(rocksdb::ReadOptions(), keySlice, &data).ok() && data.size() == sizeof(CValue))
      memcpy(&value, data.data(), sizeof(CValue));

    const CHeader *header = static_cast<const CHeader*>(shard.find(key));
    if (header)
      value.merge(header->Value);

    return !value.isNull();
  }

  virtual void flushImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex) final {
    CLog<CKey> &shard = this->shard(shardIndex);
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();

    rocksdb::WriteBatch batch;
    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(blockId.begin()), sizeof(BC::Proto::BlockHashTy)));

    if (ActiveIndexes_.empty()) {
      // No indexes: write the folded window delta as a merge operand, no reads at all
      shard.Map.forEachCurrent([&batch](const CKey&, void *keyData) {
        const CHeader *header = static_cast<const CHeader*>(keyData);
        // Perfectly cancelled window delta is an identity operand, don't write it
        if (header->Value.isNull())
          return;
        CDataRowKey rowKey;
        makeDataRowKey(rowKey, header->Key);
        batch.Merge(rocksdb::Slice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey)),
                    rocksdb::Slice(reinterpret_cast<const char*>(&header->Value), sizeof(CValue)));
      });
    } else {
      // RMW: index row replacement needs the old value anyway, so the base row
      // is written materialized too (and deleted when it folds to the identity)
      std::vector<const CHeader*> allData;
      std::vector<CDataRowKey> rowKeys;
      shard.Map.forEachCurrent([&](const CKey&, void *keyData) {
        const CHeader *header = static_cast<const CHeader*>(keyData);
        if (header->Value.isNull())
          return;
        makeDataRowKey(rowKeys.emplace_back(), header->Key);
        allData.push_back(header);
      });

      // Slices are built after the fill: emplace_back may reallocate rowKeys
      std::vector<rocksdb::Slice> keySlices;
      keySlices.reserve(rowKeys.size());
      for (const auto &rowKey: rowKeys)
        keySlices.emplace_back(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey));

      std::vector<std::string> oldValues;
      if (!allData.empty()) {
        auto readResult = storage->MultiGet(rocksdb::ReadOptions(), keySlices, &oldValues);
        for (size_t i = 0; i < allData.size(); i++) {
          CValue oldValue;
          bool hadValue = readResult[i].ok() && oldValues[i].size() == sizeof(CValue);
          if (hadValue)
            memcpy(&oldValue, oldValues[i].data(), sizeof(CValue));
          // Merge-era operands can fold to a zero row, it has no index rows
          bool hadRow = hadValue && !oldValue.isNull();

          CValue newValue = oldValue;
          newValue.merge(allData[i]->Value);
          bool hasRow = !newValue.isNull();

          if (hasRow)
            batch.Put(keySlices[i], rocksdb::Slice(reinterpret_cast<const char*>(&newValue), sizeof(CValue)));
          else
            batch.Delete(keySlices[i]);

          for (const auto &index: ActiveIndexes_) {
            uint64_t oldMetric = index.Def.Extract(oldValue);
            uint64_t newMetric = index.Def.Extract(newValue);
            if (hadRow && hasRow && oldMetric == newMetric)
              continue;

            CIndexRowKey rowKey;
            if (hadRow) {
              makeIndexRowKey(rowKey, index.Id, oldMetric, allData[i]->Key);
              batch.Delete(rocksdb::Slice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey)));
            }
            if (hasRow) {
              makeIndexRowKey(rowKey, index.Id, newMetric, allData[i]->Key);
              batch.Put(rocksdb::Slice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey)), rocksdb::Slice());
            }
          }
        }
      }
    }

    std::unique_lock lock(ShardLocks_[shardIndex]);
    storage->Write(rocksdb::WriteOptions(), &batch);
    shard.reset();
  }

  // Top of a rank index: per-shard head scans merged by the metric.
  // The result is as of the last flush, the memtable window is not indexed.
  bool top(const std::string &indexName, size_t offset, size_t limit, std::vector<std::pair<CKey, CValue>> &result) const {
    const CActiveIndex *active = nullptr;
    for (const auto &index: ActiveIndexes_) {
      if (index.Def.Name == indexName) {
        active = &index;
        break;
      }
    }
    if (!active)
      return false;

    size_t need = offset + limit;
    if (need == 0)
      return true;

    std::vector<std::pair<CKey, CValue>> candidates;
    for (size_t shardIndex = 0; shardIndex < this->BaseCfg_.ShardsNum; shardIndex++) {
      rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();
      const uint8_t seekPrefix[2] = {'i', active->Id};

      std::shared_lock lock(ShardLocks_[shardIndex]);

      // Index rows are ordered by the inverted metric: iteration is metric-descending
      std::vector<CKey> shardKeys;
      std::unique_ptr<rocksdb::Iterator> It(storage->NewIterator(rocksdb::ReadOptions()));
      for (It->Seek(rocksdb::Slice(reinterpret_cast<const char*>(seekPrefix), sizeof(seekPrefix)));
           It->Valid() && shardKeys.size() < need;
           It->Next()) {
        rocksdb::Slice key = It->key();
        if (key.size() < sizeof(seekPrefix) || memcmp(key.data(), seekPrefix, sizeof(seekPrefix)) != 0)
          break;
        // Nothing else lives under the 'i' prefix, the size check is a guard
        if (key.size() != sizeof(CIndexRowKey))
          continue;
        CKey &baseKey = shardKeys.emplace_back();
        memcpy(static_cast<void*>(&baseKey), key.data() + offsetof(CIndexRowKey, Key), sizeof(CKey));
      }

      if (shardKeys.empty())
        continue;

      std::vector<CDataRowKey> rowKeys;
      rowKeys.reserve(shardKeys.size());
      for (const auto &key: shardKeys)
        makeDataRowKey(rowKeys.emplace_back(), key);
      std::vector<rocksdb::Slice> keySlices;
      keySlices.reserve(rowKeys.size());
      for (const auto &rowKey: rowKeys)
        keySlices.emplace_back(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey));
      std::vector<std::string> values;
      auto readResult = storage->MultiGet(rocksdb::ReadOptions(), keySlices, &values);
      for (size_t i = 0; i < shardKeys.size(); i++) {
        if (!readResult[i].ok() || values[i].size() != sizeof(CValue))
          continue;
        auto &candidate = candidates.emplace_back();
        candidate.first = shardKeys[i];
        memcpy(&candidate.second, values[i].data(), sizeof(CValue));
      }
    }

    auto extract = active->Def.Extract;
    std::sort(candidates.begin(), candidates.end(), [extract](const std::pair<CKey, CValue> &l, const std::pair<CKey, CValue> &r) {
      uint64_t lv = extract(l.second);
      uint64_t rv = extract(r.second);
      if (lv != rv)
        return lv > rv;
      return memcmp(&l.first, &r.first, sizeof(CKey)) < 0;
    });

    for (size_t i = offset; i < candidates.size() && result.size() < limit; i++)
      result.push_back(candidates[i]);
    return true;
  }

protected:
  void registerIndex(const std::string &name, uint64_t (*extract)(const CValue&)) {
    RegisteredIndexes_.push_back(CIndexDef{name, extract});
  }

private:
  // The regions are disjoint, so one index is exactly the range ['i' id, next)
  bool dropIndexRows(size_t shardIndex, uint8_t id) {
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();
    const uint8_t begin[2] = {'i', id};
    const uint8_t end[2] = {'i', static_cast<uint8_t>(id + 1)};
    const uint8_t endAll = 'i' + 1;
    rocksdb::Slice endSlice = id != 0xFF ?
      rocksdb::Slice(reinterpret_cast<const char*>(end), sizeof(end)) :
      rocksdb::Slice(reinterpret_cast<const char*>(&endAll), sizeof(endAll));
    return storage->DeleteRange(rocksdb::WriteOptions(), storage->DefaultColumnFamily(),
                                rocksdb::Slice(reinterpret_cast<const char*>(begin), sizeof(begin)), endSlice).ok();
  }

  bool dropAllIndexRows(size_t shardIndex) {
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();
    const uint8_t begin = 'i';
    const uint8_t end = 'i' + 1;
    return storage->DeleteRange(rocksdb::WriteOptions(), storage->DefaultColumnFamily(),
                                rocksdb::Slice(reinterpret_cast<const char*>(&begin), sizeof(begin)),
                                rocksdb::Slice(reinterpret_cast<const char*>(&end), sizeof(end))).ok();
  }

  // Build the rows of the given indexes by a single scan of the data region.
  // A crash in the middle repeats the build on restart: "xcfg" is written only
  // after success, and re-putting an existing row is legal (last write wins)
  bool buildIndexes(size_t shardIndex, const std::vector<const CActiveIndex*> &indexes) {
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();
    LOG_F(INFO, "%s: building %zu indexes for shard %zu...", this->Name_.c_str(), indexes.size(), shardIndex);

    rocksdb::ReadOptions scanOptions;
    scanOptions.fill_cache = false;
    rocksdb::WriteBatch batch;
    size_t batchSize = 0;
    auto flushBatch = [&]() {
      if (!storage->Write(rocksdb::WriteOptions(), &batch).ok())
        return false;
      batch.Clear();
      batchSize = 0;
      return true;
    };

    bool writeOk = true;
    const uint8_t dataPrefix = 'd';
    std::unique_ptr<rocksdb::Iterator> It(storage->NewIterator(scanOptions));
    for (It->Seek(rocksdb::Slice(reinterpret_cast<const char*>(&dataPrefix), sizeof(dataPrefix)));
         It->Valid() && writeOk;
         It->Next()) {
      rocksdb::Slice key = It->key();
      if (key.empty() || static_cast<uint8_t>(key[0]) != dataPrefix)
        break;
      if (key.size() != sizeof(CDataRowKey))
        continue;
      rocksdb::Slice value = It->value();
      if (value.size() != sizeof(CValue))
        continue;
      CValue baseValue;
      memcpy(&baseValue, value.data(), sizeof(CValue));
      if (baseValue.isNull())
        continue;
      CKey baseKey;
      memcpy(static_cast<void*>(&baseKey), key.data() + offsetof(CDataRowKey, Key), sizeof(CKey));
      for (const CActiveIndex *index: indexes) {
        CIndexRowKey rowKey;
        makeIndexRowKey(rowKey, index->Id, index->Def.Extract(baseValue), baseKey);
        batch.Put(rocksdb::Slice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey)), rocksdb::Slice());
        batchSize++;
      }
      if (batchSize >= 65536)
        writeOk = flushBatch();
    }

    if (!writeOk || !It->status().ok() || !flushBatch()) {
      LOG_F(ERROR, "%s: index build failed for shard %zu", this->Name_.c_str(), shardIndex);
      return false;
    }

    return true;
  }

private:
  // Temporary reader/flush synchronization: pairs the two-level read (disk + memtable)
  // against the two-step flush (batch write + memtable reset). To be replaced with
  // a lock-free scheme - versioned View {memtable generation, disk snapshot} with
  // epoch-based reclamation of retired arenas (see db-engine-fable.md, 4.2)
  std::unique_ptr<std::shared_mutex[]> ShardLocks_;
  std::vector<CIndexDef> RegisteredIndexes_;
  std::vector<CActiveIndex> ActiveIndexes_;
};

// Interfaces
enum EInterfaceTy {
  EIQueryTransaction = 0,
  EIQueryAddrHistory,
  EIQueryAddr
};

struct CQueryTransactionResult {
  BC::Proto::Transaction Tx;
  BC::Proto::CTxLinkedOutputs LinkedOutputs;
  BC::Proto::BlockHashTy Block;
  uint32_t TxNum;
  bool Found = false;
  bool DataCorrupted = false;
};

// History element of addrhistorydb: the fact (txid) plus scalars denormalized
// for the list UI and the balance chart; Aggregate is the running balance
// maintained by CBaseArrayAggregated - the address balance right after this tx
#pragma pack(push, 1)
struct CAddrHistoryItem {
  BC::Proto::TxHashTy TxId;
  uint32_t Height;
  uint32_t Time;
  uint64_t Aggregate;
};
#pragma pack(pop)

struct CQueryAddrHistory {
  std::vector<CAddrHistoryItem> Items;
  size_t TotalTxCount;
};

class ITransactionDb {
public:
  virtual bool queryTransaction(const BC::Proto::TxHashTy &txid,
                                BlockInMemoryIndex &blockIndex,
                                BlockDatabase &blockDb,
                                CQueryTransactionResult &result) = 0;
};

class IAddrHistoryDb {
public:
  virtual bool queryAddrHistory(const BC::Script::CAddress &address, size_t from, size_t count, CQueryAddrHistory &result) = 0;
};

// Cumulative per-address counters; also serves as the in-memory delta
// (all fields are additive, negative deltas use two's complement wrap)
#pragma pack(push, 1)
struct CAddrValue {
  uint64_t Received = 0;
  uint64_t Sent = 0;
  uint64_t Mined = 0;
  uint32_t TxCount = 0;
  uint32_t TxInCount = 0;
  uint32_t TxOutCount = 0;
  uint32_t MinedTxCount = 0;

  void merge(const CAddrValue &delta) {
    Received += delta.Received;
    Sent += delta.Sent;
    Mined += delta.Mined;
    TxCount += delta.TxCount;
    TxInCount += delta.TxInCount;
    TxOutCount += delta.TxOutCount;
    MinedTxCount += delta.MinedTxCount;
  }

  void negate() {
    Received = 0 - Received;
    Sent = 0 - Sent;
    Mined = 0 - Mined;
    TxCount = 0 - TxCount;
    TxInCount = 0 - TxInCount;
    TxOutCount = 0 - TxOutCount;
    MinedTxCount = 0 - MinedTxCount;
  }

  bool isNull() const {
    return Received == 0 && Sent == 0 && Mined == 0 &&
           TxCount == 0 && TxInCount == 0 && TxOutCount == 0 && MinedTxCount == 0;
  }
};
#pragma pack(pop)

class IAddrDb {
public:
  virtual bool queryAddr(const BC::Script::CAddress &address, CAddrValue &result) = 0;
  virtual bool queryTop(const std::string &index, size_t offset, size_t limit,
                        std::vector<std::pair<BC::Script::CAddress, CAddrValue>> &result) = 0;
};

}
}
