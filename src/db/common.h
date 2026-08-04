// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/keyHash.h"
#include "common/blockDataBase.h"
#include "common/mlog.h"
#include "common/utils.h"

#include "swmrhashmap.h"
#include "config4cpp/Configuration.h"
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace BC {
namespace DB {

template<typename CKey>
struct CLog {
  MLog Log;

  // One flat SWMR map replaces the per-key version vectors and the tbb map
  // for concurrent point reads (see swmrhashmap.h). Starts small, grows to
  // the working-set size once and keeps its capacity across window resets
  CSwmrHashMap<CKey> Map{4096};

  // Window bytes charged for records with no arena footprint (delete
  // markers): their map slot still occupies memory, and a delete-heavy
  // stretch must still reach the flush threshold
  size_t PhantomBytes = 0;

  size_t windowSize() { return Log.size() + PhantomBytes; }

  // The hash that picked this shard is passed on instead of recomputed
  const void *find(const CKey &key) { return Map.find(key); }
  const void *find(const CKey &key, size_t hash) { return Map.find(key, hash); }


  void update(const CKey &key, size_t hash, void *newData) {
    Map.update(key, hash, newData);
  }

  // One walk for "write a value derived from the one already there"
  void updateWith(const CKey &key, size_t hash, auto &&valueOf) {
    Map.updateWith(key, hash, valueOf);
  }

  // O(1) window reset: the generation bump invalidates every map entry at
  // once and the arena is rewound. Runs in the writer right before the log
  // is reused, as late as possible for concurrent readers chasing arena
  // pointers (the residual race is the pre-existing bug A class, closed
  // for real by EBR in the aggkv design)
  void reset() {
    Map.reset();
    PhantomBytes = 0;
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
                       const BC::Proto::CBlockValidationData &validationData,
                       BlockInMemoryIndex &blockIndex,
                       BlockDatabase &blockDb) = 0;

  virtual void disconnect(const BC::Common::BlockIndex *index,
                          const BC::Proto::Block &block,
                          const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                          const BC::Proto::CBlockValidationData &validationData,
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
    FlushLogThreshold_ = static_cast<size_t>(cfg->lookupInt(Name_.c_str(), "flushLogSizeMb", 16)) << 20;

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

      // Flush batches arrive pre-sorted (see the writeLog implementations), a
      // single insert location hint turns the memtable fill into an append;
      // the hint is only honored without concurrent memtable writers (each
      // shard has one writing thread anyway)
      options.memtable_insert_with_hint_prefix_extractor.reset(rocksdb::NewCappedPrefixTransform(0));
      options.allow_concurrent_memtable_write = false;

      // Point lookups dominate: bloom filters cut dead probes of SST files
      // and of the memtable on Get
      rocksdb::BlockBasedTableOptions tableOptions;
      tableOptions.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
      options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tableOptions));
      options.memtable_prefix_bloom_size_ratio = 0.05;
      options.memtable_whole_key_filtering = true;

      // L0 files live until the first compaction only: skip compressing them,
      // keep zstd for the long-lived levels below
      options.compression_per_level = {rocksdb::kNoCompression, rocksdb::kZSTD};
      options.max_background_jobs = 8;
      options.max_write_buffer_number = 4;
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
               const BC::Proto::CBlockValidationData &validationData,
               BlockInMemoryIndex &blockIndex,
               BlockDatabase &blockDb) final {
    connectImpl(index, block, linkedOutputs, validationData, blockIndex, blockDb);
    finishMutation(index->Header.GetHash());
  }

  void disconnect(const BC::Common::BlockIndex *index,
                  const BC::Proto::Block &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  const BC::Proto::CBlockValidationData &validationData,
                  BlockInMemoryIndex &blockIndex,
                  BlockDatabase &blockDb) final {
    disconnectImpl(index, block, linkedOutputs, validationData, blockIndex, blockDb);
    finishMutation(index->Header.hashPrevBlock);
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
                           const BC::Proto::CBlockValidationData &validationData,
                           BlockInMemoryIndex &blockIndex,
                           BlockDatabase &blockDb) = 0;

  virtual void disconnectImpl(const BC::Common::BlockIndex *index,
                              const BC::Proto::Block &block,
                              const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                              const BC::Proto::CBlockValidationData &validationData,
                              BlockInMemoryIndex &blockIndex,
                              BlockDatabase &blockDb) = 0;

  // Async-flush counterpart of flushImpl: write the frozen log's content and
  // touch nothing else - the flusher drains the maps afterwards, the arena is
  // rewound by the writer when the log is recycled. Implemented only by
  // databases that call enableAsyncFlush()
  virtual void flushFrozenImpl(const BC::Proto::BlockHashTy&, size_t, CLog<CKey>&) {
    abort();
  }

protected:
  // The tail every chain mutation shares: the tip moves to the block the
  // operation left the database at, then a shard whose window crossed the
  // threshold is written out. Both directions go through here - a deep
  // reorg or a startup rebase writes as much as a forward run does, and
  // checking after connect only would let its window grow unbounded
  void finishMutation(const BC::Proto::BlockHashTy &newTip) {
    CurrentBlock_ = newTip;

    for (size_t i = 0; i < BaseCfg_.ShardsNum; i++) {
      if (shard(i).windowSize() >= FlushLogThreshold_) {
        if (AsyncFlushEnabled_)
          freezeAndScheduleFlush(i);
        else
          flushImpl(CurrentBlock_, i);
      }
    }
  }

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
  std::unique_ptr<CShardSlot[]> Shards_;
  std::vector<std::unique_ptr<rocksdb::DB>> OnDiskStorage_;

  // Window size: the active log freezes for flush once its arena crosses
  // this threshold (config key flushLogSizeMb). Bigger windows annihilate
  // more short-lived keys before they ever reach the disk
  size_t FlushLogThreshold_ = 1u << 24;

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
  // Arena record of an add: the payload follows the header. The key is not
  // stored here - it already lives in the map slot, and the flush reads it
  // from there
  struct CHeader {
    // Top bit of the length: a value of this key may live below the active
    // window (frozen log or disk), so a later erase must tombstone it instead
    // of annihilating the pair
    static constexpr uint32_t MayExistBelowFlag = 0x80000000u;
    uint32_t SizeAndFlag;

    uint32_t size() const { return SizeAndFlag & ~MayExistBelowFlag; }
    bool mayExistBelow() const { return (SizeAndFlag & MayExistBelowFlag) != 0; }
  };

  // A delete allocates no arena record: the map value is the address of one
  // of these static markers, distinguished by identity. Tombstone deletes an
  // on-disk key; BornDead marks a key whose live value was born in this same
  // window - it never reached the disk and the flush drops the pair entirely
  static inline CHeader TombstoneMarker_;
  static inline CHeader BornDeadMarker_;

  static bool isMarker(const void *entry) {
    return entry == &TombstoneMarker_ || entry == &BornDeadMarker_;
  }

public:
  // Where the written value can have come from. A value that was born in this
  // window and never left it can be annihilated by a later erase - the pair
  // never reaches the disk. A value that may replace something below (frozen
  // log or RocksDB) forfeits that: its erase must write a real tombstone
  enum class EPutBase : uint8_t {
    DefinitelyAbsentBelow,
    MayExistBelow
  };

  CBaseKV(const std::string &name) : Base<CKey>(name) {}

  // The flusher dispatches virtual flushFrozenImpl implemented at this
  // level, so it must stop before this level is destroyed
  virtual ~CBaseKV() { this->shutdownAsyncFlush(); }
  rocksdb::MergeOperator *mergeOperator() final { return nullptr; }

  // A value nothing below can hold: a brand new output, a transaction of a
  // block being connected for the first time
  void putNew(const CKey &key, const void *data, size_t size, const void *suffix = nullptr, size_t suffixSize = 0) {
    put(key, data, size, suffix, suffixSize, EPutBase::DefinitelyAbsentBelow);
  }

  // A value that may land on an existing one: an output put back by a
  // disconnect, a BIP30 coinbase repeating an earlier one. Conservative by
  // design - if the key really was window-born, the flush writes one Put and
  // one Delete instead of nothing
  void putRestore(const CKey &key, const void *data, size_t size, const void *suffix = nullptr, size_t suffixSize = 0) {
    put(key, data, size, suffix, suffixSize, EPutBase::MayExistBelow);
  }

  void erase(const CKey &key) {
    const size_t hash = std::hash<CKey>()(key);
    CLog<CKey> &shard = this->shard(fastrange(hash, this->BaseCfg_.ShardsNum));

    // no arena record: charge the map slot to the window
    shard.PhantomBytes += sizeof(CKey) + 2 * sizeof(void*);

    // One walk picks the marker and writes it; a separate find() would walk
    // the same chain twice
    shard.updateWith(key, hash, [](const void *prev) -> void* {
      // Already dead and never below: a second erase changes nothing
      if (prev == &BornDeadMarker_)
        return &BornDeadMarker_;
      // Born in this window and never flushed: drop the pair whole. A value
      // that admits something below it forfeits that
      const CHeader *header = prev && !isMarker(prev) ? static_cast<const CHeader*>(prev) : nullptr;
      return header && !header->mayExistBelow() ? &BornDeadMarker_ : &TombstoneMarker_;
    });
  }

  bool find(const CKey &key, std::function<void(const void*, size_t)> callback) const {
    const size_t hash = std::hash<CKey>()(key);
    size_t shardIndex = fastrange(hash, this->BaseCfg_.ShardsNum);

    // Active first, frozen second: the newest version of a key shadows the
    // frozen one and a delete marker in the active log must hide a frozen
    // value
    const void *entry = this->activeLog(shardIndex)->find(key, hash);
    if (!entry) {
      if (CLog<CKey> *frozen = this->frozenLog(shardIndex))
        entry = frozen->find(key, hash);
    }
    if (entry) {
      if (isMarker(entry))
        return false;
      const CHeader *header = static_cast<const CHeader*>(entry);
      callback(header + 1, header->size());
      return true;
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
  // Value glued from two pieces straight in the arena: a "payload + a few bytes
  // of metadata" caller would otherwise build it in a scratch buffer and copy
  // the record a second time
  void put(const CKey &key, const void *data, size_t size, const void *suffix, size_t suffixSize, EPutBase base) {
    // One hash per operation: shard from its high bits, map slot from its low
    const size_t hash = std::hash<CKey>()(key);
    CLog<CKey> &shard = this->shard(fastrange(hash, this->BaseCfg_.ShardsNum));

    // record rounded up so every header lands 4-aligned in the arena
    size_t total = size + suffixSize;
    CHeader *header = static_cast<CHeader*>(shard.Log.alloc((sizeof(CHeader) + total + 3) & ~static_cast<size_t>(3)));
    header->SizeAndFlag = static_cast<uint32_t>(total) | (base == EPutBase::MayExistBelow ? CHeader::MayExistBelowFlag : 0);
    uint8_t *value = reinterpret_cast<uint8_t*>(header + 1);
    memcpy(value, data, size);
    if (suffixSize)
      memcpy(value + size, suffix, suffixSize);

    // The flag is monotonic inside a window: what the replaced value knew
    // about the layers below cannot be taken back by the value replacing it.
    // A tombstone is such a value too - it is only ever written for a key
    // that may exist below, a window-born key dies as BornDead instead
    shard.updateWith(key, hash, [header](const void *prev) -> void* {
      if (prev == &TombstoneMarker_ ||
          (prev && !isMarker(prev) && static_cast<const CHeader*>(prev)->mayExistBelow()))
        header->SizeAndFlag |= CHeader::MayExistBelowFlag;
      return header;
    });
  }

  // Builds and writes the batch of one log's final values; shared by the
  // synchronous flush and the background flusher
  void writeLog(const BC::Proto::BlockHashTy &blockId, size_t shardIndex, CLog<CKey> &shard) {
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();

    // The batch is fed to rocksdb in memcmp order of keys: paired with the
    // memtable insert hint this turns the skiplist fill into an append. Sort
    // by an 8-byte big-endian prefix, full key compare only inside a
    // same-prefix run
    struct CSortedRef {
      uint64_t Prefix;
      const CKey *Key;        // points into the stable frozen map slot
      const CHeader *Header;  // nullptr: delete the on-disk key
    };
    std::vector<CSortedRef> refs;
    refs.reserve(shard.Map.used());
    size_t batchBytes = 64;
    size_t annihilated = 0;
    shard.Map.forEachCurrent([&refs, &batchBytes, &annihilated](const CKey &key, void *value) {
      if (value == &BornDeadMarker_) {
        annihilated++;
        return;
      }
      const CHeader *header = value == &TombstoneMarker_ ? nullptr : static_cast<const CHeader*>(value);
      uint64_t prefix = 0;
      memcpy(&prefix, &key, std::min(sizeof(prefix), sizeof(CKey)));
      refs.push_back({xhtobe(prefix), &key, header});
      batchBytes += sizeof(CKey) + 4 + (header ? header->size() + 8 : 0);
    });

    std::sort(refs.begin(), refs.end(), [](const CSortedRef &l, const CSortedRef &r) {
      if (l.Prefix != r.Prefix)
        return l.Prefix < r.Prefix;
      return memcmp(l.Key, r.Key, sizeof(CKey)) < 0;
    });

    rocksdb::WriteBatch batch(batchBytes);
    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(blockId.begin()), sizeof(BC::Proto::BlockHashTy)));

    for (const auto &ref: refs) {
      auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(ref.Key), sizeof(CKey));
      if (ref.Header)
        batch.Put(keySlice, rocksdb::Slice(reinterpret_cast<const char*>(ref.Header + 1), ref.Header->size()));
      else
        batch.Delete(keySlice);
    }

    // Every flush batch carries the stamp, a crash without WAL rolls data and
    // stamp back together
    rocksdb::WriteOptions writeOptions;
    writeOptions.disableWAL = true;
    storage->Write(writeOptions, &batch);
    LOG_F(1, "%s: flushed %zu records, %zu window-born pairs annihilated", this->Name_.c_str(), refs.size(), annihilated);
  }
};

// Fixed-size element array with a running aggregate: besides the fact itself,
// every element carries in its Aggregate field the fold of all elements up to
// and including it, so a range read returns ready prefix sums (address history
// with the balance after each tx, balance chart).
//
// A window is normalized to "trim the durable prefix, then append":
//     final = durable[0 : durableCount - BaseTrim] ++ Tail
// BaseTrim never decreases while the window lives and the Tail holds only
// elements born in it, so a reorg that truncates the durable end and appends
// an alternate branch is an ordinary pair of operations with nothing special
// about it - the append is not lost and needs no rollback of the window.
// add() receives elements whose Aggregate holds the element DELTA and folds
// them into window-relative running sums (uint64 wrap-around, a negative delta
// is two's complement); the flush rebases them onto the aggregate of the last
// surviving durable element and writes them from that position on, overwriting
// whatever the trimmed branch left there. Truncating part of the Tail keeps
// every surviving prefix sum valid by construction; only a trim reaching into
// the durable prefix reads one chunk back for the new rebase point.
// CItem requirements: trivially copyable, fixed size, public uint64_t
// Aggregate field with the semantics above.
template<typename CKey, typename CItem>
class CBaseArrayAggregated : public Base<CKey> {
private:
#pragma pack(push, 1)
  // Arena record of a window: the Tail follows it. The key is not stored here -
  // it already lives in the map slot, and the flush reads it from there
  struct CHeader {
    uint64_t BaseTrim;   // elements dropped from the durable prefix
    uint64_t TailCount;  // elements appended over what survives (the payload)
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

  static const CItem *tail(const CHeader *header) { return reinterpret_cast<const CItem*>(header + 1); }
  static CItem *tail(CHeader *header) { return reinterpret_cast<CItem*>(header + 1); }

  static rocksdb::Slice slice(const CChunkKey &key) {
    return rocksdb::Slice(reinterpret_cast<const char*>(&key), sizeof(CChunkKey));
  }

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
  void add(const CKey &key, const CItem *items, size_t count) {
    const size_t hash = std::hash<CKey>()(key);
    CLog<CKey> &shard = this->shard(fastrange(hash, this->BaseCfg_.ShardsNum));

    const CHeader *prev = static_cast<const CHeader*>(shard.find(key, hash));
    const uint64_t prevCount = prev ? prev->TailCount : 0;

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader) + (prevCount + count)*sizeof(CItem)));
    header->BaseTrim = prev ? prev->BaseTrim : 0;
    header->TailCount = prevCount + count;

    CItem *dst = tail(header);
    if (prevCount)
      memcpy(static_cast<void*>(dst), tail(prev), prevCount*sizeof(CItem));

    CItem *newItems = dst + prevCount;
    memcpy(static_cast<void*>(newItems), items, count*sizeof(CItem));
    uint64_t running = prevCount ? dst[prevCount - 1].Aggregate : 0;
    for (size_t i = 0; i < count; i++) {
      running += newItems[i].Aggregate;
      newItems[i].Aggregate = running;
    }

    shard.update(key, hash, header);
  }

  // Drop the last count elements. The Tail goes first, only what is left over
  // is charged to the durable prefix; elements appended afterwards start a
  // fresh Tail over the new end
  void truncate(const CKey &key, size_t count) {
    const size_t hash = std::hash<CKey>()(key);
    CLog<CKey> &shard = this->shard(fastrange(hash, this->BaseCfg_.ShardsNum));

    const CHeader *prev = static_cast<const CHeader*>(shard.find(key, hash));
    const uint64_t prevCount = prev ? prev->TailCount : 0;
    const uint64_t fromTail = std::min<uint64_t>(count, prevCount);
    const uint64_t newCount = prevCount - fromTail;

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader) + newCount*sizeof(CItem)));
    header->BaseTrim = (prev ? prev->BaseTrim : 0) + (count - fromTail);
    header->TailCount = newCount;
    if (newCount)
      memcpy(static_cast<void*>(tail(header)), tail(prev), newCount*sizeof(CItem));

    shard.update(key, hash, header);
  }

  bool query(const CKey &key, size_t from, size_t count, xmstream &result, size_t *totalCount) {
    const size_t hash = std::hash<CKey>()(key);
    size_t shardIdx = fastrange(hash, this->BaseCfg_.ShardsNum);
    CLog<CKey> &mlog = this->shard(shardIdx);
    rocksdb::DB *storage = this->OnDiskStorage_[shardIdx].get();

    if (count == 0)
      return false;
    size_t firstChunk = from / ChunkSize_;
    size_t lastChunk = (from + count - 1) / ChunkSize_;

    // Metadata, data chunks and the boundary chunk come from one snapshot: a
    // flush landing between two of those reads would otherwise move the end of
    // the array without moving the data, or the other way round
    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = storage->GetSnapshot();
    bool ok = true;

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
        allKeySlices.push_back(slice(k));

      auto metadataReadResult = storage->MultiGet(readOptions, allKeySlices, &readResult);
      // Missing metadata is not an error: the key may live only in the window
      if (!metadataReadResult[0].ok())
        readResult[0].clear();
    }

    int64_t durableCount = 0;
    uint64_t durableAggregate = 0;
    if (readResult[0].size() == sizeof(CMetadata)) {
      const CMetadata *meta = reinterpret_cast<const CMetadata*>(readResult[0].data());
      durableCount = meta->Count;
      durableAggregate = meta->Aggregate;
    }

    // The window says how much of the durable prefix is still there and what
    // is appended over it
    const CHeader *header = static_cast<const CHeader*>(mlog.find(key, hash));
    const uint64_t baseTrim = header ? header->BaseTrim : 0;
    const uint64_t tailCount = header ? header->TailCount : 0;
    const int64_t baseCount = durableCount - static_cast<int64_t>(baseTrim);
    uint64_t baseAggregate = durableAggregate;

    if (baseCount < 0) {
      LOG_F(ERROR, "%s: window trims %llu elements of an array holding %lld", this->Name_.c_str(),
            static_cast<unsigned long long>(baseTrim), static_cast<long long>(durableCount));
      ok = false;
    } else if (baseTrim) {
      // The durable end moved: the rebase point is the element that survived
      baseAggregate = 0;
      if (baseCount > 0 && !aggregateAt(storage, readOptions, key, baseCount - 1, baseAggregate)) {
        LOG_F(ERROR, "%s: can't read back the aggregate of element %lld", this->Name_.c_str(),
              static_cast<long long>(baseCount - 1));
        ok = false;
      }
    }

    if (ok) {
      // Disk range, bounded by the surviving prefix and not by the durable
      // count: after a trim the last chunk keeps stale bytes past the end
      int64_t onDiskAvailable = std::min((int64_t)(from + count), baseCount) - static_cast<int64_t>(from);
      int64_t inMemoryOffset = std::max(static_cast<int64_t>(from) - baseCount, (int64_t)0);
      int64_t remaining = count;
      *totalCount = static_cast<size_t>(baseCount) + tailCount;

      if (onDiskAvailable > 0) {
        int64_t chunkOffset = from % ChunkSize_;
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

      // Window tail: rebase window-relative running sums by the surviving prefix
      if (tailCount) {
        int64_t available = static_cast<int64_t>(tailCount) - inMemoryOffset;
        int64_t needToRead = std::min(available, remaining);
        if (needToRead > 0) {
          CItem *items = static_cast<CItem*>(result.reserve(needToRead * sizeof(CItem)));
          memcpy(static_cast<void*>(items), tail(header) + inMemoryOffset, needToRead * sizeof(CItem));
          for (int64_t i = 0; i < needToRead; i++)
            items[i].Aggregate += baseAggregate;
        }
      }
    }

    storage->ReleaseSnapshot(readOptions.snapshot);
    return ok;
  }

  virtual void flushImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex) final {
    CLog<CKey> &shard = this->shard(shardIndex);
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();

    // Collect all keys; they stay put in the map slots until the reset below
    std::vector<const CKey*> allKeys;
    std::vector<const CHeader*> allData;
    std::vector<rocksdb::Slice> allKeySlices;
    std::vector<std::string> metadata;

    shard.Map.forEachCurrent([&](const CKey &key, void *keyData) {
      allKeys.push_back(&key);
      allData.push_back(static_cast<const CHeader*>(keyData));
    });

    for (size_t i = 0; i < allKeys.size(); i++)
      allKeySlices.emplace_back((const char*)allKeys[i], sizeof(CKey));

    // Metadata and the boundary chunks of trimmed keys are one consistent view
    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = storage->GetSnapshot();
    auto metadataReadResult = storage->MultiGet(readOptions, allKeySlices, &metadata);

    rocksdb::WriteBatch batch;

    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(blockId.begin()), sizeof(BC::Proto::BlockHashTy)));
    // Absolute aggregates are built here and never written into the published
    // window: a query running concurrently keeps reading window-relative sums
    std::vector<uint8_t> scratch;

    for (size_t i = 0; i < allKeys.size(); i++) {
      const CHeader *header = allData[i];
      int64_t durableCount = 0;
      uint64_t durableAggregate = 0;
      if (metadataReadResult[i].ok() && metadata[i].size() == sizeof(CMetadata)) {
        const CMetadata *currentMeta = reinterpret_cast<const CMetadata*>(metadata[i].data());
        durableCount = currentMeta->Count;
        durableAggregate = currentMeta->Aggregate;
      }

      const int64_t baseCount = durableCount - static_cast<int64_t>(header->BaseTrim);
      const int64_t newCount = baseCount + static_cast<int64_t>(header->TailCount);

      // Fail closed: a window that trims more than the array holds means the
      // operations reaching this database no longer describe one chain, and
      // clamping it would silently write an array nobody can rebuild
      if (baseCount < 0) {
        LOG_F(ERROR, "%s: shard %zu, window trims %llu elements of an array holding %lld",
              this->Name_.c_str(), shardIndex,
              static_cast<unsigned long long>(header->BaseTrim), static_cast<long long>(durableCount));
        abort();
      }

      // The rebase point of the window: the aggregate of the last element that
      // survived the trim, read back from its chunk when the trim moved it
      uint64_t baseAggregate = durableAggregate;
      if (header->BaseTrim) {
        baseAggregate = 0;
        if (baseCount > 0 && !aggregateAt(storage, readOptions, *allKeys[i], baseCount - 1, baseAggregate)) {
          LOG_F(ERROR, "%s: shard %zu, can't read back the aggregate of element %lld",
                this->Name_.c_str(), shardIndex, static_cast<long long>(baseCount - 1));
          abort();
        }
      }

      uint64_t newAggregate = baseAggregate;
      if (header->TailCount) {
        const CItem *tailItems = tail(header);
        newAggregate = baseAggregate + tailItems[header->TailCount - 1].Aggregate;

        size_t remaining = header->TailCount;
        size_t taken = 0;
        int64_t offset = baseCount;

        // A partial chunk at the boundary is patched in place by an offset
        // merge operand, whole chunks are written outright: past the boundary
        // the new branch overwrites the trimmed one element for element
        if (offset % static_cast<int64_t>(ChunkSize_)) {
          size_t writeSize = std::min(remaining, ChunkSize_ - static_cast<size_t>(offset % ChunkSize_));
          uint64_t chunkOffset = sizeof(CItem) * static_cast<uint64_t>(offset % ChunkSize_);
          scratch.resize(sizeof(uint64_t) + writeSize * sizeof(CItem));
          memcpy(scratch.data(), &chunkOffset, sizeof(chunkOffset));
          rebase(scratch.data() + sizeof(uint64_t), tailItems + taken, writeSize, baseAggregate);

          CChunkKey chunkKey;
          chunkKey.Key = *allKeys[i];
          chunkKey.Index = xhtobe<uint64_t>(offset / ChunkSize_);
          batch.Merge(slice(chunkKey), rocksdb::Slice(reinterpret_cast<const char*>(scratch.data()), scratch.size()));

          taken += writeSize;
          remaining -= writeSize;
          offset += writeSize;
        }

        while (remaining) {
          size_t writeSize = std::min(remaining, ChunkSize_);
          scratch.resize(writeSize * sizeof(CItem));
          rebase(scratch.data(), tailItems + taken, writeSize, baseAggregate);

          CChunkKey chunkKey;
          chunkKey.Key = *allKeys[i];
          chunkKey.Index = xhtobe<uint64_t>(offset / ChunkSize_);
          batch.Put(slice(chunkKey), rocksdb::Slice(reinterpret_cast<const char*>(scratch.data()), scratch.size()));

          taken += writeSize;
          remaining -= writeSize;
          offset += writeSize;
        }
      }

      // Chunks entirely past the new end die whole; the last one keeps the
      // bytes past newCount, they are unreachable through the metadata count
      // and overwritten by the next append
      for (int64_t chunkIdx = chunksFor(newCount); chunkIdx < chunksFor(durableCount); chunkIdx++) {
        CChunkKey chunkKey;
        chunkKey.Key = *allKeys[i];
        chunkKey.Index = xhtobe<uint64_t>(chunkIdx);
        batch.Delete(slice(chunkKey));
      }

      // Update metadata
      {
        auto keySlice = allKeySlices[i];
        if (newCount) {
          CMetadata meta;
          meta.Count = newCount;
          meta.Aggregate = newAggregate;
          batch.Put(keySlice, rocksdb::Slice(reinterpret_cast<const char*>(&meta), sizeof(meta)));
        } else {
          batch.Delete(keySlice);
        }
      }
    }

    storage->ReleaseSnapshot(readOptions.snapshot);

    rocksdb::WriteOptions writeOptions;
    writeOptions.disableWAL = true;
    storage->Write(writeOptions, &batch);
    shard.reset();
  }

private:
  int64_t chunksFor(int64_t count) const {
    return count / static_cast<int64_t>(ChunkSize_) + (count % static_cast<int64_t>(ChunkSize_) != 0);
  }

  // Absolute copy of a piece of the Tail; the published window keeps its
  // window-relative sums
  static void rebase(void *dst, const CItem *items, size_t count, uint64_t base) {
    CItem *out = static_cast<CItem*>(dst);
    memcpy(dst, items, count * sizeof(CItem));
    for (size_t i = 0; i < count; i++)
      out[i].Aggregate += base;
  }

  bool aggregateAt(rocksdb::DB *storage, const rocksdb::ReadOptions &readOptions, const CKey &key, int64_t index, uint64_t &aggregate) const {
    CChunkKey chunkKey;
    chunkKey.Key = key;
    chunkKey.Index = xhtobe<uint64_t>(index / static_cast<int64_t>(ChunkSize_));
    size_t offsetInChunk = static_cast<size_t>(index % static_cast<int64_t>(ChunkSize_));

    std::string chunkData;
    if (!storage->Get(readOptions, slice(chunkKey), &chunkData).ok() ||
        chunkData.size() < (offsetInChunk + 1) * sizeof(CItem))
      return false;

    aggregate = reinterpret_cast<const CItem*>(chunkData.data())[offsetInChunk].Aggregate;
    return true;
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
//   data rows    'd' ++ key                               -> value
//   index rows   'i' ++ indexId ++ be64(~metric) ++ key   -> value (same shard)
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

  // The row is covering: its value is a copy of the data row, so a head scan
  // answers top() without going back to the data region
  struct CIndexRowKey {
    uint8_t Prefix;
    uint8_t IndexId;
    uint64_t InvertedValue;
    CKey Key;
  };
#pragma pack(pop)

  // Metric-descending, ties broken by the key: the order of the index region
  // itself, so a merged list keeps the order a plain scan would have given
  struct CMetricOrder {
    uint64_t (*Extract)(const CValue&);
    bool operator()(const std::pair<CKey, CValue> &l, const std::pair<CKey, CValue> &r) const {
      uint64_t lv = Extract(l.second);
      uint64_t rv = Extract(r.second);
      if (lv != rv)
        return lv > rv;
      return memcmp(&l.first, &r.first, sizeof(CKey)) < 0;
    }
  };

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

  void merge(const CKey &key, const CValue &delta) {
    const size_t hash = std::hash<CKey>()(key);
    CLog<CKey> &shard = this->shard(fastrange(hash, this->BaseCfg_.ShardsNum));

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader)));
    header->Key = key;
    const CHeader *prev = static_cast<const CHeader*>(shard.find(key, hash));
    header->Value = prev ? prev->Value : CValue();
    header->Value.merge(delta);

    shard.update(key, hash, header);
  }

  bool find(const CKey &key, CValue &value) const {
    const size_t hash = std::hash<CKey>()(key);
    size_t shardIndex = fastrange(hash, this->BaseCfg_.ShardsNum);
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

    const CHeader *header = static_cast<const CHeader*>(shard.find(key, hash));
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
            // The row is covering, so an unchanged metric is not an unchanged
            // row: a tx of an address moves tx_count without moving balance.
            // Same metric means same key though - rewrite it in place
            bool sameKey = hadRow && hasRow && oldMetric == newMetric;

            CIndexRowKey rowKey;
            if (hadRow && !sameKey) {
              makeIndexRowKey(rowKey, index.Id, oldMetric, allData[i]->Key);
              batch.Delete(rocksdb::Slice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey)));
            }
            if (hasRow) {
              makeIndexRowKey(rowKey, index.Id, newMetric, allData[i]->Key);
              batch.Put(rocksdb::Slice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey)),
                        rocksdb::Slice(reinterpret_cast<const char*>(&newValue), sizeof(CValue)));
            }
          }
        }
      }
    }

    std::unique_lock lock(ShardLocks_[shardIndex]);
    rocksdb::WriteOptions writeOptions;
    writeOptions.disableWAL = true;
    storage->Write(writeOptions, &batch);
    shard.reset();
  }

  // Top of a rank index: per-shard head scans merged by the metric, exact as of
  // the read - the same list a find() per key would give. Index rows hold what
  // the last flush wrote, so the window is folded in here the way find() folds
  // it into a point read.
  //
  // The scan goes deeper than the answer on purpose: the metric of the last row
  // read from a shard bounds every key of that shard the scan missed, so a
  // window key outside the selection can only reach the cut T if tail + delta
  // >= T. Deeper scan, sharper threshold - and with a covering index depth
  // costs nothing but the sequential read
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

    const size_t need = offset + limit;
    if (need == 0)
      return true;

    CMetricOrder order{active->Def.Extract};
    size_t depth = need * 4 + 256;

    for (;;) {
      std::vector<std::pair<CKey, CValue>> candidates;
      std::vector<uint64_t> tails;
      for (size_t shardIndex = 0; shardIndex < this->BaseCfg_.ShardsNum; shardIndex++)
        topFromShard(active->Id, order, shardIndex, depth, need, candidates, tails);

      std::sort(candidates.begin(), candidates.end(), order);
      uint64_t cut = candidates.size() >= need ? order.Extract(candidates[need-1].second) : 0;

      // Keys left unread in a shard are bounded by its tail; a tail above the
      // cut means one of them may belong in the answer - go deeper. Normally
      // the tail is far below (that is what depth buys), and a monotonic metric
      // like tx_count can't push the cut down at all
      bool deeper = false;
      for (uint64_t tail: tails)
        deeper |= tail > cut;
      if (deeper) {
        depth *= 4;
        continue;
      }

      for (size_t i = offset; i < candidates.size() && result.size() < limit; i++)
        result.push_back(candidates[i]);
      return true;
    }
  }

protected:
  void registerIndex(const std::string &name, uint64_t (*extract)(const CValue&)) {
    RegisteredIndexes_.push_back(CIndexDef{name, extract});
  }

private:
  // One shard's part of top(): the head of its index region, the window folded
  // into it, and the window keys from below the head that the cut still lets in.
  // All under the shard's shared lock - pairing a disk row with a window delta
  // is only valid while the flush that moves one into the other is kept out
  // (the find() contract). Appends the shard's candidates; a shard with rows
  // left unread appends its tail, the metric bound for all of them
  void topFromShard(uint8_t indexId, const CMetricOrder &order, size_t shardIndex,
                    size_t depth, size_t need,
                    std::vector<std::pair<CKey, CValue>> &candidates,
                    std::vector<uint64_t> &tails) const {
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();
    // async flush is never enabled for merge tables, the frozen log can't exist
    CLog<CKey> &shard = *this->activeLog(shardIndex);
    const uint8_t seekPrefix[2] = {'i', indexId};

    std::shared_lock lock(ShardLocks_[shardIndex]);

    // Index rows are ordered by the inverted metric: iteration is metric-descending
    std::vector<std::pair<CKey, CValue>> selection;
    std::unordered_map<CKey, size_t> position;
    uint64_t tail = 0;
    bool exhausted = true;
    size_t unreadable = 0;

    std::unique_ptr<rocksdb::Iterator> It(storage->NewIterator(rocksdb::ReadOptions()));
    for (It->Seek(rocksdb::Slice(reinterpret_cast<const char*>(seekPrefix), sizeof(seekPrefix)));
         It->Valid();
         It->Next()) {
      rocksdb::Slice key = It->key();
      if (key.size() < sizeof(seekPrefix) || memcmp(key.data(), seekPrefix, sizeof(seekPrefix)) != 0)
        break;
      // Nothing else lives under the 'i' prefix, the size check is a guard
      if (key.size() != sizeof(CIndexRowKey))
        continue;
      if (selection.size() == depth) {
        exhausted = false;
        break;
      }

      uint64_t inverted;
      memcpy(&inverted, key.data() + offsetof(CIndexRowKey, InvertedValue), sizeof(inverted));
      tail = ~xbetoh<uint64_t>(inverted);

      // A row without its covering value is a row of a layout that is not this
      // one: it stays out of the selection, so the shard is not read to the end
      // and the disk read below is back on for the window keys
      if (It->value().size() != sizeof(CValue)) {
        unreadable++;
        exhausted = false;
        continue;
      }

      auto &row = selection.emplace_back();
      memcpy(static_cast<void*>(&row.first), key.data() + offsetof(CIndexRowKey, Key), sizeof(CKey));
      memcpy(&row.second, It->value().data(), sizeof(CValue));
      position.emplace(row.first, selection.size() - 1);
    }

    // The window holds the folded delta per key. Kept aside instead of merged
    // in place: the walk may see a key twice if the map grows under it, and an
    // overwritten delta stays right where a second merge would double-count
    std::vector<CValue> deltas(selection.size());
    shard.Map.forEachConcurrent([&](const CKey &key, const void *data) {
      auto rowIt = position.find(key);
      if (rowIt != position.end())
        deltas[rowIt->second] = static_cast<const CHeader*>(data)->Value;
    });

    std::vector<std::pair<CKey, CValue>> merged;
    merged.reserve(selection.size());
    for (size_t i = 0; i < selection.size(); i++) {
      selection[i].second.merge(deltas[i]);
      // A key the window drove to zero is gone, the way flushImpl drops it
      if (!selection[i].second.isNull())
        merged.push_back(selection[i]);
    }

    // Shard-local cut. The final one is taken over all shards and can only be
    // higher, so filtering by this one never drops a key that belongs in the top
    uint64_t shardCut = 0;
    if (merged.size() >= need) {
      std::nth_element(merged.begin(), merged.begin() + (need - 1), merged.end(), order);
      shardCut = order.Extract(merged[need-1].second);
    }

    // The rest of the window: its disk value is bounded by the tail (a shard
    // read to the end leaves nothing on disk to bound at all), so only a delta
    // big enough to lift that bound over the cut is worth a read
    const uint64_t bound = exhausted ? 0 : tail;
    std::vector<std::pair<CKey, CValue>> extras;
    shard.Map.forEachConcurrent([&](const CKey &key, const void *data) {
      if (position.count(key))
        return;
      const CValue &delta = static_cast<const CHeader*>(data)->Value;
      int64_t metric = static_cast<int64_t>(order.Extract(delta));
      // A delta that lowers the metric can't lift its key over the cut. A metric
      // narrower than 64 bits reads its negatives as huge positives - that costs
      // a read and nothing else, the value below is computed, not extrapolated
      if (metric <= 0)
        return;
      if (bound <= UINT64_MAX - static_cast<uint64_t>(metric) &&
          bound + static_cast<uint64_t>(metric) < shardCut)
        return;
      extras.emplace_back(key, delta);
    });

    if (!extras.empty()) {
      // A shard read to the end has an index row for every non-null data row it
      // holds, so a key outside the selection has no base to read - it is new in
      // this window. Skipping the reads is what keeps a fresh database (empty
      // index, everything still in the window) off a full-window MultiGet
      std::vector<std::string> values;
      std::vector<rocksdb::Status> readResult;
      if (!exhausted) {
        std::vector<CDataRowKey> rowKeys;
        rowKeys.reserve(extras.size());
        for (const auto &extra: extras)
          makeDataRowKey(rowKeys.emplace_back(), extra.first);
        std::vector<rocksdb::Slice> keySlices;
        keySlices.reserve(rowKeys.size());
        for (const auto &rowKey: rowKeys)
          keySlices.emplace_back(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey));
        readResult = storage->MultiGet(rocksdb::ReadOptions(), keySlices, &values);
      }

      for (size_t i = 0; i < extras.size(); i++) {
        // Nothing on disk is a legal base: the key is new in this window
        CValue value;
        if (!readResult.empty() && readResult[i].ok() && values[i].size() == sizeof(CValue))
          memcpy(&value, values[i].data(), sizeof(CValue));
        value.merge(extras[i].second);
        if (!value.isNull())
          candidates.emplace_back(extras[i].first, value);
      }
    }

    if (unreadable)
      LOG_F(ERROR, "%s: index '%u' of shard %zu has %zu rows in a foreign layout, rebuild the database",
            this->Name_.c_str(), indexId, shardIndex, unreadable);

    candidates.insert(candidates.end(), merged.begin(), merged.end());
    if (!exhausted)
      tails.push_back(tail);
  }

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
        batch.Put(rocksdb::Slice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey)),
                  rocksdb::Slice(reinterpret_cast<const char*>(&baseValue), sizeof(CValue)));
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

// Where a database keyed by txid starts its walk over a block. A BIP30 repeat
// brings a coinbase already stored under that key - the same transaction byte
// for byte - so rewriting it would only move the record to the newer block, and
// the remove that undoes it would take the twin's record with it while the twin
// is still connected. The chain params carry both inclusions for the query side
static inline size_t firstTx(const BC::Proto::CBlockValidationData &validationData) {
  return validationData.CoinbaseRepeat ? 1 : 0;
}

// Interfaces
enum EInterfaceTy {
  EIQueryTransaction = 0,
  EIQueryAddrHistory,
  EIQueryAddr,
  EIQuerySpent
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

// What spentdb stores per spent outpoint: the input that took it. The height
// bounds a walk down the spend graph without reading the spending transaction
#pragma pack(push, 1)
struct CSpentValue {
  BC::Proto::TxHashTy SpentBy;  // txid of the spending transaction
  uint32_t InputIndex;          // its input taking this output
  uint32_t Height;              // height of the block holding the spender
};
#pragma pack(pop)

static_assert(sizeof(CSpentValue) == sizeof(BC::Proto::TxHashTy) + 2 * sizeof(uint32_t),
              "unexpected padding in CSpentValue");

struct CQuerySpentResult {
  CSpentValue Value;
  bool Found = false;
};

class ISpentDb {
public:
  virtual bool querySpent(const BC::Proto::TxHashTy &txid, uint32_t index, CQuerySpentResult &result) = 0;
  // Every output of one transaction, result sized to count: what a transaction
  // page needs, one call instead of a lookup per output
  virtual bool querySpentOutputs(const BC::Proto::TxHashTy &txid, uint32_t count, std::vector<CQuerySpentResult> &result) = 0;
};

}
}
