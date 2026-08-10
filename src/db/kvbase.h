// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// The database half of the engine: shards, base configuration, stamp and
// chain position - everything CKvEngine deliberately does not know. connect()
// hands a CKvWriter over the active eras down and commits the unit's
// watermark; what a layer becomes on disk is the family fold below -
// CKvBase is the KV one (last write wins), merge/array bring their own.

#include "common/utils.h"
#include "db/common.h"
#include "db/kvview.h"

#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>

#include <chrono>
#include <algorithm>
#include <map>
#include <thread>

namespace BC {
namespace DB {

template<typename CKey>
class CKvDatabase : public BaseInterface, public IKvSegmentWriter<CKey> {
public:
  CKvDatabase(const std::string &name) { Name_ = name; }

  // Shutdown order: the engine waits out the readers still inside a call, the
  // handles of the named families go next, and the shards close with this object
  ~CKvDatabase() override {
    Engine_.shutdown();
    for (size_t i = 0; i < ColumnFamilies_.size(); i++) {
      for (auto &entry: ColumnFamilies_[i])
        OnDiskStorage_[i]->DestroyColumnFamilyHandle(entry.second);
    }
  }

  // Empty name keeps rocksdb's own default; an unknown one is a config typo
  // and must not silently fall back to something else
  static rocksdb::CompressionType compressionByName(config4cpp::Configuration *cfg, const char *key,
                                                    rocksdb::CompressionType defaultValue) {
    const char *name = cfg->lookupString("rocksdb", key, nullptr);
    if (!name || !*name)
      return defaultValue;
    if (strcmp(name, "none") == 0) return rocksdb::kNoCompression;
    if (strcmp(name, "snappy") == 0) return rocksdb::kSnappyCompression;
    if (strcmp(name, "zstd") == 0) return rocksdb::kZSTD;
    if (strcmp(name, "lz4") == 0) return rocksdb::kLZ4Compression;
    LOG_F(ERROR, "unknown rocksdb.%s '%s', using the default", key, name);
    return defaultValue;
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
    OnDiskStorage_.resize(BaseCfg_.ShardsNum);
    ColumnFamilies_.resize(BaseCfg_.ShardsNum);
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

      // Every write of these families reads the row it updates, and the block
      // it comes in has to be decompressed each time it is missed. rocksdb's
      // own default is 32Mb - nothing against a database of tens of GB, and
      // one cache is shared by every database of the process
      // HyperClockCache and not NewLRUCache: the clock table is what rocksdb
      // picks by default, and the legacy LRU shards behind a mutex
      static std::shared_ptr<rocksdb::Cache> blockCache =
        rocksdb::HyperClockCacheOptions(static_cast<size_t>(cfg->lookupInt("rocksdb", "blockCacheMb", 1024)) << 20, 0).MakeSharedCache();
      tableOptions.block_cache = blockCache;
      options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tableOptions));
      options.memtable_prefix_bloom_size_ratio = 0.05;
      options.memtable_whole_key_filtering = true;

      // L0 files live until the first compaction only: skip compressing them.
      // Below it compression is the single biggest cost of compaction, while
      // almost all of the data ends up on the bottom level - hence two knobs,
      // the transit levels and the floor they settle on. lz4 rather than zstd:
      // address hashes barely compress, so zstd buys 7% of space for 60% of speed
      options.compression_per_level = {rocksdb::kNoCompression,
                                       compressionByName(cfg, "compression", rocksdb::kLZ4Compression)};
      options.bottommost_compression = compressionByName(cfg, "bottommostCompression", rocksdb::kDisableCompressionOption);
      options.max_write_buffer_number = 4;

      // A batch of these families spreads over the whole key range, so every
      // compaction runs full width and a byte is rewritten once per level it
      // passes. A base level four times rocksdb's own removes one of those
      // levels: 15% less compaction on a catch-up of the address history
      options.max_bytes_for_level_base = static_cast<uint64_t>(cfg->lookupInt("rocksdb", "levelBaseMb", 1024)) << 20;

      // One background pool serves every database, so this is a machine-wide
      // setting: a bulk catch-up owes tens of GB of compaction and rocksdb
      // throttles the writer until it is paid, while most of a big box idles.
      // Its last waves owe a debt nothing overlaps any more, and one compaction
      // of a base level this size is seconds of wall on a single thread -
      // splitting those is what makes the bigger base level pay off
      unsigned cores = std::max(std::thread::hardware_concurrency(), 4u);
      options.max_background_jobs = cfg->lookupInt("rocksdb", "backgroundJobs", std::clamp(cores / 4, 4u, 16u));
      options.max_subcompactions = cfg->lookupInt("rocksdb", "subcompactions", std::clamp(cores / 8, 1u, 8u));

      // Nothing here is journalled: only a joint flush keeps a batch spanning
      // families from surviving in one of them and being lost in another
      options.atomic_flush = usesColumnFamilies();

      // Which named families this database wants is known after configure()
      // only - whatever is on disk is opened as it is found, and initializeImpl
      // creates and drops from there
      std::string shardPathUtf8 = pathToUtf8(shardPath);
      BlockCache_ = blockCache;
      BaseCfOptions_ = rocksdb::ColumnFamilyOptions(options);
      std::vector<std::string> cfNames;
      rocksdb::DB::ListColumnFamilies(options, shardPathUtf8, &cfNames);
      if (cfNames.empty())
        cfNames.push_back(rocksdb::kDefaultColumnFamilyName);
      std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
      for (const std::string &name: cfNames)
        descriptors.emplace_back(name, columnFamilyOptionsFor(name));

      std::vector<rocksdb::ColumnFamilyHandle*> handles;
      rocksdb::Status status = rocksdb::DB::Open(options, shardPathUtf8, descriptors, &handles, &db);
      if (!status.ok()) {
        LOG_F(ERROR, "Can't open or create %s database at %s", Name_.c_str(), shardPathUtf8.c_str());
        return false;
      }

      OnDiskStorage_[i].reset(db);
      shards.push_back(db);
      for (size_t h = 0; h < handles.size(); h++)
        ColumnFamilies_[i][cfNames[h]] = handles[h];

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

        FreshAtOpen_ = false;
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

    configure(cfg);

    // Before the engine takes its initial snapshots: initializeImpl writes to
    // the shards directly (the merge family rebuilds index rows), and a write
    // landing after a snapshot stays invisible to reads until the first flush
    if (!initializeImpl(cfg, storage))
      return false;

    typename CKvEngine<CKey>::CConfig engineCfg;
    engineCfg.Name = Name_;
    engineCfg.EraBytes = static_cast<size_t>(cfg->lookupInt(Name_.c_str(), "eraSizeMb", 256)) << 20;
    return Engine_.initialize(engineCfg, shards, this);
  }

  // One write set per unit, published as one revision: the unit is a run of
  // blocks, a block connected on its own is a batch of one
  void connect(CBlockBatch batch, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) final {
    if (batch.empty())
      return;
    CKvWriter<CKey> writer = Engine_.liveWriter();
    connectImpl(batch, writer, blockIndex, blockDb);
    finishMutation(writer, batch.back().Index->Header.GetHash());
  }

  void disconnect(const BC::Common::BlockIndex *index,
                  const BC::Proto::Block &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  const BC::Proto::CBlockValidationData &validationData,
                  BlockInMemoryIndex &blockIndex,
                  BlockDatabase &blockDb) final {
    CKvWriter<CKey> writer = Engine_.liveWriter();
    disconnectImpl(index, block, linkedOutputs, validationData, writer, blockIndex, blockDb);
    finishMutation(writer, index->Header.hashPrevBlock);
  }

  // Frozen eras waiting for the flusher: the commit cannot refuse, so the
  // pipeline stops admitting work on this instead (blockPipeline throttled())
  bool pipelineFull() const final { return Engine_.isPipelineFull(); }

  // Checkpoint: everything attached reaches the disk, then every shard is
  // stamped - including untouched ones, initialize() rejects disagreeing stamps
  void flush() final { Engine_.flushAll(CurrentBlock_); }

  // Waits for the automatic compaction to work off the debt, and not a forced
  // CompactRange: the run owes the levels it dirtied, not a rewrite of a
  // bottom level it never touched. flush() must have run first - what sits in
  // a memtable is not the backend's to compact
  void settle() final {
    for (auto &shard: OnDiskStorage_) {
      rocksdb::WaitForCompactOptions options;
      options.flush = true;
      options.wait_for_purge = true;
      rocksdb::Status status = shard->WaitForCompact(options);
      if (!status.ok())
        LOG_F(ERROR, "%s: waiting for compaction failed: %s", Name_.c_str(), status.ToString().c_str());
    }
  }

  virtual uint32_t version() = 0;
  // Family-level knobs, read before initializeImpl - the leaves override that one
  virtual void configure(config4cpp::Configuration*) {}

  // Whether this database splits its key space into column families at all -
  // what a batch has to be made atomic across
  virtual bool usesColumnFamilies() { return false; }

  // What a named family wants that the database-wide options do not give it.
  // Called both at open and at create, so a family is the same however it came
  // to be; the default one carries the service keys and needs nothing
  virtual void columnFamilyOptions(const std::string&, rocksdb::ColumnFamilyOptions&,
                                   const std::shared_ptr<rocksdb::Cache>&) {}
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
  // the operation left the database, and the commit publishes the unit's cut
  void finishMutation(CKvWriter<CKey> &writer, const BC::Proto::BlockHashTy &newTip) {
    CurrentBlock_ = newTip;
    Engine_.commitLive(writer, newTip);
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

  // Named families of a shard: what exists, what has to appear, what is gone.
  // Names alone are the on-disk inventory - a family that is there and a family
  // this database still wants are two different questions
  rocksdb::ColumnFamilyHandle *columnFamily(size_t shardIndex, const std::string &name) const {
    auto It = ColumnFamilies_[shardIndex].find(name);
    return It != ColumnFamilies_[shardIndex].end() ? It->second : nullptr;
  }

  void columnFamilyNames(size_t shardIndex, std::vector<std::string> &names) const {
    for (const auto &entry: ColumnFamilies_[shardIndex])
      names.push_back(entry.first);
  }

  rocksdb::ColumnFamilyHandle *createColumnFamily(size_t shardIndex, const std::string &name) {
    rocksdb::ColumnFamilyHandle *handle = columnFamily(shardIndex, name);
    if (handle)
      return handle;
    rocksdb::Status status =
      OnDiskStorage_[shardIndex]->CreateColumnFamily(columnFamilyOptionsFor(name), name, &handle);
    if (!status.ok()) {
      LOG_F(ERROR, "%s: can't create column family '%s': %s", Name_.c_str(), name.c_str(), status.ToString().c_str());
      return nullptr;
    }
    ColumnFamilies_[shardIndex][name] = handle;
    return handle;
  }

  bool dropColumnFamily(size_t shardIndex, const std::string &name) {
    rocksdb::ColumnFamilyHandle *handle = columnFamily(shardIndex, name);
    if (!handle)
      return true;
    rocksdb::DB *db = OnDiskStorage_[shardIndex].get();
    ColumnFamilies_[shardIndex].erase(name);
    // The files go away with the last handle, so the order is drop then destroy
    bool ok = db->DropColumnFamily(handle).ok();
    db->DestroyColumnFamilyHandle(handle);
    return ok;
  }

  // Every family of the shard, at once: atomic_flush is what makes a batch
  // spanning them durable as one, and a per-family flush is not that
  bool flushColumnFamilies(size_t shardIndex) {
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    for (const auto &entry: ColumnFamilies_[shardIndex])
      handles.push_back(entry.second);
    return OnDiskStorage_[shardIndex]->Flush(rocksdb::FlushOptions(), handles).ok();
  }

private:
  rocksdb::ColumnFamilyOptions columnFamilyOptionsFor(const std::string &name) {
    rocksdb::ColumnFamilyOptions cfOptions = BaseCfOptions_;
    columnFamilyOptions(name, cfOptions, BlockCache_);
    return cfOptions;
  }

protected:
  // Configuration
  CBaseCfg BaseCfg_;

  // No shard carried a stamp at open: the database is being built from
  // nothing by this very process, and what is not yet flushed is not on disk
  bool FreshAtOpen_ = true;

  // Owned here, handed to the engine as raw pointers: must outlive it
  std::vector<std::unique_ptr<rocksdb::DB>> OnDiskStorage_;

  // Per shard, by name, the default one included. Handles are destroyed by
  // hand in the destructor - they belong to a database still open at that point
  std::vector<std::map<std::string, rocksdb::ColumnFamilyHandle*>> ColumnFamilies_;
  rocksdb::ColumnFamilyOptions BaseCfOptions_;
  std::shared_ptr<rocksdb::Cache> BlockCache_;

  CKvEngine<CKey> Engine_;
};

// Plain key-value: the newest record of a key wins, and a lookup stops at the
// first layer that has it
template<typename CKey>
class CKvBase : public CKvDatabase<CKey> {
public:
  CKvBase(const std::string &name) : CKvDatabase<CKey>(name) {}

  // The flusher dispatches the folds implemented at this level: stop it
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

  // One sealed layer, one batch, in memcmp order of keys (what the memtable
  // insert hint wants): the newest record per key goes as a Put, a tombstone
  // that may exist below as a Delete, a pair born and died here not at all
  void writeLayer(rocksdb::DB *db, size_t shardIndex, const CLayer<CKey> *layer, const BC::Proto::BlockHashTy &stamp) final {
    layer->buildScattered();

    rocksdb::WriteBatch batch(layer->BatchBytesBound + 64);
    this->putStamp(batch, stamp);

    size_t written = 0;
    size_t annihilated = 0;
    for (size_t b = 0; b < KvScatterBuckets && !layer->Scattered.empty(); b++) {
      kvSortBucket(layer->Scattered, layer->Bounds, b);
      const uint32_t total = static_cast<uint32_t>(layer->Scattered.size());
      for (uint32_t k = b ? layer->Bounds[b - 1] : 0, end = layer->Bounds[b]; k < end; k++) {
        if (k + KvPrefetchDistance < total) {
          const CKvSortedRef<CKey> &ahead = layer->Scattered[k + KvPrefetchDistance];
          kvPrefetch(ahead.Entry);
          kvPrefetch(ahead.Key);
        }
        const CKvSortedRef<CKey> &ref = layer->Scattered[k];
        const CGenRecord *rec = static_cast<const CGenRecord*>(ref.Entry);

        rocksdb::Slice keySlice(reinterpret_cast<const char*>(ref.Key), sizeof(CKey));
        if (!rec->tombstone()) {
          batch.Put(keySlice, rocksdb::Slice(static_cast<const char*>(rec->payload()), rec->size()));
          written++;
        } else if (rec->mayExistBelow()) {
          batch.Delete(keySlice);
          written++;
        } else {
          // born and died without the disk ever hearing about it
          annihilated++;
        }
      }
    }

    this->writeBatch(db, batch);
    LOG_F(1, "%s: shard %zu flushed %zu records, %zu pairs annihilated, %zu MB",
          this->Name_.c_str(), shardIndex, written, annihilated, layer->Bytes >> 20);
  }
};

}
}
