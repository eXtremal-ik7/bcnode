// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// The database half of the window engine: shards, base configuration, stamp and
// chain position - everything CKvEngine deliberately does not know. Rationale:
// kv-view-migration-plan.md
//
// One unit of connect is one write set: connect() hands a fresh CKvWriter down
// and attaches it whole. The set belongs to the unit, not to the database -
// which is what lets it be filled in prepare later instead of here.
//
// CKvDatabase is family-agnostic; what a shard's windows become on disk is the
// fold below it. CKvBase is the KV one - last write wins - and merge/array
// bring their own (kvmerge.h, kvarray.h).

#include "common/utils.h"
#include "db/common.h"
#include "db/kvview.h"

#include <rocksdb/filter_policy.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>

#include <algorithm>

namespace BC {
namespace DB {

template<typename CKey>
class CKvDatabase : public BaseInterface, public IKvSegmentWriter<CKey> {
public:
  CKvDatabase(const std::string &name) { Name_ = name; }

  // Shutdown order: the engine waits out the readers still inside a call, then
  // the shards below it close with this object
  ~CKvDatabase() override { Engine_.shutdown(); }

  bool initialize(BlockInMemoryIndex &blockIndex,
                  const std::filesystem::path &dbPath,
                  BC::DB::Storage &storage,
                  config4cpp::Configuration *cfg,
                  BC::Common::BlockIndex **forConnect,
                  std::vector<BC::Common::BlockIndex*> &forDisconnect) final {
    BaseCfg_.ShardsNum = static_cast<unsigned>(cfg->lookupInt(Name_.c_str(), "shardsNum", 1));
    BaseCfg_.Version = version();

    BC::Common::BlockIndex *bestIndex = blockIndex.best();
    OnDiskStorage_.resize(BaseCfg_.ShardsNum);
    *forConnect = nullptr;

    // Open all shards
    BC::Proto::BlockHashTy stamp;
    std::vector<rocksdb::DB*> shards;
    for (size_t i = 0; i < BaseCfg_.ShardsNum; i++) {
      auto shardPath = dbPath / std::to_string(i);
      std::filesystem::create_directories(shardPath);

      rocksdb::DB *db;
      rocksdb::Options options;
      options.create_if_missing = true;
      options.compression = rocksdb::kZSTD;
      options.keep_log_file_num = 4;
      options.merge_operator.reset(mergeOperator());

      // Flush batches arrive pre-sorted (the folds below), a single insert
      // location hint turns the memtable fill into an append; the hint is only
      // honored without concurrent memtable writers, and one flusher thread
      // writes a shard anyway
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
        LOG_F(ERROR, "Can't open or create %s database at %s", Name_.c_str(), shardPathUtf8.c_str());
        return false;
      }

      OnDiskStorage_[i].reset(db);
      shards.push_back(db);

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

          // flushAll() stamps every shard with CurrentBlock_, so the position
          // must be known right after open, before any block is connected
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

    // Before the engine takes its initial snapshots: initializeImpl writes to
    // the shards directly (the merge family rebuilds index rows), and a write
    // landing after a snapshot stays invisible to reads until the first flush
    if (!initializeImpl(cfg, storage))
      return false;

    // Heirs of flushLogSizeMb, per shard: the floors are the annihilation
    // horizon, the window count also bounds how deep a lookup walks
    typename CKvEngine<CKey>::CConfig engineCfg;
    engineCfg.Name = Name_;
    engineCfg.FlushBytesLower = static_cast<size_t>(cfg->lookupInt(Name_.c_str(), "flushLogSizeMb", 16)) << 20;
    engineCfg.FlushSegmentsLower = static_cast<size_t>(cfg->lookupInt(Name_.c_str(), "flushSegments", DefaultFlushSegments));
    return Engine_.initialize(engineCfg, shards, this);
  }

  // One write set per unit, published as one revision: the unit is a run of
  // blocks, a block connected on its own is a batch of one
  void connect(CBlockBatch batch, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) final {
    if (batch.empty())
      return;
    CKvWriter<CKey> writer = Engine_.newWriter(ArenaBytes_, MapCapacity_);
    connectImpl(batch, writer, blockIndex, blockDb);
    finishMutation(writer, batch.back().Index->Header.GetHash());
  }

  void disconnect(const BC::Common::BlockIndex *index,
                  const BC::Proto::Block &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  const BC::Proto::CBlockValidationData &validationData,
                  BlockInMemoryIndex &blockIndex,
                  BlockDatabase &blockDb) final {
    CKvWriter<CKey> writer = Engine_.newWriter(ArenaBytes_, MapCapacity_);
    disconnectImpl(index, block, linkedOutputs, validationData, writer, blockIndex, blockDb);
    finishMutation(writer, index->Header.hashPrevBlock);
  }

  // Attached windows waiting for the flusher: attach cannot refuse, so the
  // pipeline stops admitting work on this instead (blockPipeline throttled())
  bool pipelineFull() const final { return Engine_.isPipelineFull(); }

  // Checkpoint: everything attached reaches the disk, then every shard is
  // stamped - including untouched ones, initialize() rejects disagreeing stamps
  void flush() final { Engine_.flushAll(CurrentBlock_); }

  virtual uint32_t version() = 0;
  virtual bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage) = 0;

  // Only the families whose fold needs one (merge, array) return an operator
  virtual rocksdb::MergeOperator *mergeOperator() { return nullptr; }

  // The write set is an argument, not state: this is the same set that will
  // later be filled in prepare and brought here ready
  virtual void connectImpl(CBlockBatch batch,
                           CKvWriter<CKey> &writer,
                           BlockInMemoryIndex &blockIndex,
                           BlockDatabase &blockDb) = 0;

  virtual void disconnectImpl(const BC::Common::BlockIndex *index,
                              const BC::Proto::Block &block,
                              const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                              const BC::Proto::CBlockValidationData &validationData,
                              CKvWriter<CKey> &writer,
                              BlockInMemoryIndex &blockIndex,
                              BlockDatabase &blockDb) = 0;

protected:
  // Tail of every chain mutation, both directions: the position moves to where
  // the operation left the database, and the set becomes a revision stamped there
  void finishMutation(CKvWriter<CKey> &writer, const BC::Proto::BlockHashTy &newTip) {
    CurrentBlock_ = newTip;
    if (writer.empty())
      return;

    // The next set is sized by this one, before attach takes the windows: units
    // of one chain are alike, and an arena that grew mid-fill copied its prefix
    ArenaBytes_ = std::max(ArenaBytes_, writer.maxWindowSize());
    MapCapacity_ = std::max(MapCapacity_, 2 * writer.maxUsed());
    Engine_.attach(writer, newTip);
  }

  // Every batch carries the stamp: without WAL a crash rolls data and position
  // back together
  static void putStamp(rocksdb::WriteBatch &batch, const BC::Proto::BlockHashTy &stamp) {
    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(stamp.begin()), sizeof(BC::Proto::BlockHashTy)));
  }

  static void writeBatch(rocksdb::DB *db, rocksdb::WriteBatch &batch) {
    rocksdb::WriteOptions writeOptions;
    writeOptions.disableWAL = true;
    db->Write(writeOptions, &batch);
  }

protected:
  // Configuration
  CBaseCfg BaseCfg_;

  // Owned here, handed to the engine as raw pointers: must outlive it
  std::vector<std::unique_ptr<rocksdb::DB>> OnDiskStorage_;

  CKvEngine<CKey> Engine_;

  // Sizes the next unit's set, from what the units before it needed
  size_t ArenaBytes_ = 1u << 20;
  size_t MapCapacity_ = 4096;
};

// Plain key-value: the newest record of a key wins, and a lookup stops at the
// first layer that has it
template<typename CKey>
class CKvBase : public CKvDatabase<CKey> {
public:
  CKvBase(const std::string &name) : CKvDatabase<CKey>(name) {}

  // The flusher dispatches writeSegments implemented at this level: stop it
  // while the dispatch is still valid (~CKvDatabase's shutdown is a no-op then)
  ~CKvBase() override { this->Engine_.shutdown(); }

protected:
  // The published revision and nothing else. One guard per call for now: the
  // wave and the prefetch each holding one per pass is a pipeline change
  template<typename F>
  bool find(const CKey &key, F &&callback) const {
    CKvGuard<CKey> guard = this->Engine_.guard();
    return this->Engine_.find(guard, key, callback);
  }

  // Fold of several windows into the final value of every key: one sort instead
  // of a batch per window. A window holds at most one record per key, so a
  // sorted group is that key's whole history inside this batch
  void writeSegments(rocksdb::DB *db,
                     size_t shardIndex,
                     const CWindow<CKey> *const *segments,
                     size_t count,
                     const BC::Proto::BlockHashTy &stamp) final {
    // The batch reaches rocksdb in memcmp order of keys: with the memtable
    // insert hint that turns the skiplist fill into an append. Sort on a big
    // endian prefix, full compare only inside a same-prefix run, order last
    struct CSortedRef {
      uint64_t Prefix;
      const CKey *Key;      // points into the sealed map slot, stable while pinned
      const void *Entry;    // CKvHeader* or one of the markers
      uint32_t Order;       // position of the window, oldest first
    };

    size_t entries = 0;
    for (size_t i = 0; i < count; i++)
      entries += segments[i]->Map.used();

    std::vector<CSortedRef> refs;
    refs.reserve(entries);
    size_t batchBytes = 64;
    for (uint32_t order = 0; order < count; order++) {
      segments[order]->Map.forEachCurrent([&refs, &batchBytes, order](const CKey &key, void *value) {
        uint64_t prefix = 0;
        memcpy(&prefix, &key, std::min(sizeof(prefix), sizeof(CKey)));
        refs.push_back({xhtobe(prefix), &key, value, order});
        batchBytes += sizeof(CKey) + 4 + (isKvMarker(value) ? 0 : static_cast<const CKvHeader*>(value)->size() + 8);
      });
    }

    std::sort(refs.begin(), refs.end(), [](const CSortedRef &l, const CSortedRef &r) {
      if (l.Prefix != r.Prefix)
        return l.Prefix < r.Prefix;
      const int cmp = memcmp(l.Key, r.Key, sizeof(CKey));
      if (cmp != 0)
        return cmp < 0;
      return l.Order < r.Order;
    });

    rocksdb::WriteBatch batch(batchBytes);
    this->putStamp(batch, stamp);

    size_t written = 0;
    size_t annihilated = 0;
    for (size_t i = 0; i < refs.size(); ) {
      // What lies under the batch is decided by the OLDEST record of the group,
      // not by all of them: a value written as definitely-absent-below (or a
      // pair already annihilated inside its window) means the key was born
      // here, so a marker at the end drops the pair instead of tombstoning a
      // key the disk never had. Tombstone is the opposite - it is only ever
      // written for a key that may exist below
      const void *first = refs[i].Entry;
      const bool mayExistBelow = first == &KvTombstoneMarker ||
                                 (first != &KvBornDeadMarker && static_cast<const CKvHeader*>(first)->mayExistBelow());

      size_t j = i;
      while (j != refs.size() && memcmp(refs[j].Key, refs[i].Key, sizeof(CKey)) == 0)
        j++;

      const CSortedRef &last = refs[j - 1];
      rocksdb::Slice keySlice(reinterpret_cast<const char*>(last.Key), sizeof(CKey));
      if (!isKvMarker(last.Entry)) {
        const CKvHeader *header = static_cast<const CKvHeader*>(last.Entry);
        batch.Put(keySlice, rocksdb::Slice(reinterpret_cast<const char*>(header + 1), header->size()));
        written++;
      } else if (mayExistBelow) {
        batch.Delete(keySlice);
        written++;
      } else {
        // born and died without the disk ever hearing about it
        annihilated++;
      }

      i = j;
    }

    this->writeBatch(db, batch);
    LOG_F(1, "%s: shard %zu flushed %zu windows, %zu records, %zu pairs annihilated",
          this->Name_.c_str(), shardIndex, count, written, annihilated);
  }
};

}
}
