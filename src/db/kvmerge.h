// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// Additive aggregation: a layer record is the cumulative delta of the units
// that touched the key, and the value being commutative and associative is
// what the family rests on - layers are summed instead of shadowed, a read
// folds every layer of the revision into the disk row. The revision pairs its
// layers with its snapshot by construction, so no lock guards the two-level
// read against the flush.
//
// A shard is one rocksdb database of several column families - a key space
// each, so nothing needs a prefix to tell it from its neighbours:
//   "data"          key                     -> value
//   "index.<name>"  beN(~metric) ++ key     -> value (same shard)
//   default         "stamp", "basecfg", "xcfg"
// N is the index's metric width - the width of the column itself, so no value
// is ever clamped and the memcmp order of the family IS the metric order.
// Separate families drop an index outright (no range tombstone), build it by
// one scan of the data family, and keep a compaction to rows of one kind.
//
// A zero metric has no index row: the rank of a zero is nothing anybody asks
// for, and on a chain most addresses end spent - dropping them takes the bulk
// of the balance index away. top() filters zero-metric keys on both paths.
//
// CValue requirements: trivially copyable, default state is the identity,
// merge() commutative/associative, negate() gives the inverse delta for
// disconnect.

#include "db/kvbase.h"

#include "thirdparty/ankerl/unordered_dense.h"

#include <chrono>

namespace BC {
namespace DB {

template<typename CKey, typename CValue>
class CKvMergeBase : public CKvDatabase<CKey> {
protected:
  // Extract computes at full width; Width is the metric column's size in
  // bytes - what the key stores and what every comparison is canonical to
  struct CIndexDef {
    std::string Name;
    unsigned Width;
    UInt<128> (*Extract)(const CValue&);
  };

private:
  struct CActiveIndex {
    CIndexDef Def;
    // The family holding this index, per shard
    std::vector<rocksdb::ColumnFamilyHandle*> Cf;
  };

  static const std::string &dataCfName() {
    static const std::string name = "data";
    return name;
  }

  static std::string indexCfName(const std::string &indexName) { return "index." + indexName; }

  static bool isIndexCfName(const std::string &name) { return name.compare(0, 6, "index.") == 0; }

  // Index rows are covering: the value is a copy of the data row, so a head
  // scan answers top() without going back to the data family. The key is
  // built by buildIndexRowKey - its size varies with the index's Width
  static constexpr size_t IndexRowKeyMax = sizeof(UInt<128>) + sizeof(CKey);

  // The metric at the index's width: bytes past it are zero, so in-memory
  // ordering and arithmetic agree with the stored key bytes exactly. A value
  // that outgrew the column (a wrapped delta, corrupt data) truncates the way
  // the stored bytes do - the two views can never disagree
  static UInt<128> metricOf(const CIndexDef &def, const CValue &value) {
    UInt<128> metric = def.Extract(value);
    for (size_t j = def.Width; j < sizeof(UInt<128>); j++)
      metric.data()[j / 8] &= ~(static_cast<uint64_t>(0xFF) << (8 * (j % 8)));
    return metric;
  }

  // Metric-descending, ties broken by the key: the order of the index region
  // itself, so a merged list keeps the order a plain scan would have given
  struct CMetricOrder {
    const CIndexDef *Def;
    bool operator()(const std::pair<CKey, CValue> &l, const std::pair<CKey, CValue> &r) const {
      UInt<128> lv = metricOf(*Def, l.second);
      UInt<128> rv = metricOf(*Def, r.second);
      if (lv != rv)
        return lv > rv;
      return memcmp(&l.first, &r.first, sizeof(CKey)) < 0;
    }
  };

  static uint8_t metricByte(const UInt<128> &value, size_t index) {
    return static_cast<uint8_t>(value.data()[index / 8] >> (8 * (index % 8)));
  }

  static void encodeInvertedMetric(uint8_t *out, const UInt<128> &metric, unsigned width) {
    const UInt<128> inverted = ~metric;
    for (unsigned i = 0; i < width; i++)
      out[i] = metricByte(inverted, width - 1 - i);
  }

  static UInt<128> decodeInvertedMetric(const uint8_t *in, unsigned width) {
    UInt<128> metric;
    for (unsigned i = 0; i < width; i++)
      metric.data()[(width - 1 - i) / 8] |=
        static_cast<uint64_t>(static_cast<uint8_t>(~in[i])) << (8 * ((width - 1 - i) % 8));
    return metric;
  }

  static size_t indexRowKeySize(const CIndexDef &def) {
    return def.Width + sizeof(CKey);
  }

  static size_t buildIndexRowKey(uint8_t *out, const CActiveIndex &index, const UInt<128> &metric, const CKey &key) {
    encodeInvertedMetric(out, metric, index.Def.Width);
    memcpy(out + index.Def.Width, &key, sizeof(CKey));
    return indexRowKeySize(index.Def);
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

    // Recorded in the SST files of every existing database - the name of the
    // class that is gone, and it stays that way: renaming it means a reindex
    virtual const char* Name() const override {
      return "CBaseMerge";
    }
  };

public:
  CKvMergeBase(const std::string &name) : CKvDatabase<CKey>(name) {}

  // The flusher dispatches the folds implemented at this level: stop it
  // while the dispatch is still valid (~CKvDatabase's shutdown is a no-op then)
  ~CKvMergeBase() override { this->Engine_.shutdown(); }

  rocksdb::MergeOperator *mergeOperator() final { return new MergeOperator(); }

  bool usesColumnFamilies() final { return true; }

  // An index row is never looked up by key - written blind, read by a head
  // scan. A bloom over tens of millions of them answers nothing, and the
  // insert hint misses too: key order is random in metric order
  void columnFamilyOptions(const std::string &name, rocksdb::ColumnFamilyOptions &options,
                           const std::shared_ptr<rocksdb::Cache> &blockCache) final {
    if (!isIndexCfName(name))
      return;

    rocksdb::BlockBasedTableOptions tableOptions;
    tableOptions.block_cache = blockCache;
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(tableOptions));
    options.memtable_whole_key_filtering = false;
    options.memtable_prefix_bloom_size_ratio = 0;
    options.memtable_insert_with_hint_prefix_extractor.reset();
  }

  // The deferred half of initialize: the catch-up is over, the windows it left
  // go to disk and the rank rows are built from them in one scan per shard
  bool finishInitialBuild() final {
    if (DeferredIndexes_.empty())
      return true;

    this->flush();
    std::vector<const CActiveIndex*> forBuild;
    for (const auto &index: DeferredIndexes_)
      forBuild.push_back(&index);

    for (size_t shardIndex = 0; shardIndex < this->BaseCfg_.ShardsNum; shardIndex++) {
      rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();
      for (auto &index: DeferredIndexes_) {
        index.Cf[shardIndex] = this->createColumnFamily(shardIndex, indexCfName(index.Def.Name));
        if (!index.Cf[shardIndex])
          return false;
      }
      if (!buildIndexes(shardIndex, forBuild))
        return false;
      // Same order as the transition above: the marker must not outlive the
      // rows it vouches for, and both the scan and the write are idempotent
      if (!this->flushColumnFamilies(shardIndex))
        return false;
      if (!storage->Put(rocksdb::WriteOptions(), rocksdb::Slice("xcfg"), rocksdb::Slice(DeferredCfg_)).ok())
        return false;
    }

    ActiveIndexes_.swap(DeferredIndexes_);
    DeferredIndexes_.clear();
    return true;
  }

  bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage&) final {
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
      index.Def = RegisteredIndexes_[i];
      index.Cf.resize(this->BaseCfg_.ShardsNum, nullptr);
      if (!indexCfg.empty())
        indexCfg.push_back(',');
      indexCfg.append(index.Def.Name);
    }

    // The data family comes first: everything below reads it, and its absence
    // under a stamped shard is the old single-family layout
    for (size_t shardIndex = 0; shardIndex < this->BaseCfg_.ShardsNum; shardIndex++) {
      if (!this->columnFamily(shardIndex, dataCfName()) && !this->FreshAtOpen_) {
        LOG_F(ERROR, "%s: shard %zu predates the column family layout, restart with --reindex=%s",
              this->Name_.c_str(), shardIndex, this->Name_.c_str());
        return false;
      }
      DataCf_.push_back(this->createColumnFamily(shardIndex, dataCfName()));
      if (!DataCf_.back())
        return false;
    }

    // Indexes of a database built from nothing wait for the catch-up: a rank
    // row kept block by block costs four writes against one, and one scan at
    // the end gives the same rows. Until "xcfg" they look never built
    if (this->FreshAtOpen_ && !ActiveIndexes_.empty()) {
      LOG_F(INFO, "%s: %zu indexes deferred until the initial catch-up is over",
            this->Name_.c_str(), ActiveIndexes_.size());
      DeferredIndexes_.swap(ActiveIndexes_);
      DeferredCfg_ = indexCfg;
      return true;
    }

    // Per-index diff against the configured set: a family nobody wants is
    // dropped, a missing one is built by one scan of the data family. "xcfg"
    // vouches for completeness - one missing from it is a cut-short build
    for (size_t shardIndex = 0; shardIndex < this->BaseCfg_.ShardsNum; shardIndex++) {
      rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();

      std::string storedCfg;
      storage->Get(rocksdb::ReadOptions(), rocksdb::Slice("xcfg"), &storedCfg);
      std::vector<std::string> built;
      for (size_t pos = 0; pos < storedCfg.size(); ) {
        size_t comma = std::min(storedCfg.find(',', pos), storedCfg.size());
        built.push_back(storedCfg.substr(pos, comma - pos));
        pos = comma + 1;
      }

      bool changed = storedCfg != indexCfg;
      std::vector<std::string> names;
      this->columnFamilyNames(shardIndex, names);
      for (const std::string &name: names) {
        if (!isIndexCfName(name))
          continue;
        const std::string indexName = name.substr(strlen("index."));
        bool complete = std::find(built.begin(), built.end(), indexName) != built.end();
        bool active = false;
        for (const auto &index: ActiveIndexes_)
          active |= index.Def.Name == indexName;
        if (complete && active)
          continue;
        LOG_F(INFO, "%s: dropping index '%s' of shard %zu", this->Name_.c_str(), indexName.c_str(), shardIndex);
        if (!this->dropColumnFamily(shardIndex, name))
          return false;
        changed = true;
      }

      std::vector<const CActiveIndex*> forBuild;
      for (auto &index: ActiveIndexes_) {
        const std::string name = indexCfName(index.Def.Name);
        bool exists = this->columnFamily(shardIndex, name) != nullptr;
        index.Cf[shardIndex] = this->createColumnFamily(shardIndex, name);
        if (!index.Cf[shardIndex])
          return false;
        if (!exists) {
          forBuild.push_back(&index);
          changed = true;
        }
      }

      if (!changed)
        continue;
      if (!forBuild.empty() && !buildIndexes(shardIndex, forBuild))
        return false;

      // The rows above went in without the journal, so they are durable only
      // once the memtables are on disk. "xcfg" is the marker of the whole
      // transition and must not outlive them: a crash before it repeats the
      // drop and the build, both idempotent
      if (!this->flushColumnFamilies(shardIndex))
        return false;
      if (!storage->Put(rocksdb::WriteOptions(), rocksdb::Slice("xcfg"), rocksdb::Slice(indexCfg)).ok())
        return false;
    }

    return true;
  }

  // Write side: the unit's own delta for the key, folded into whatever it has
  // written for it already - the fold at insert, against the record the new
  // one replaces
  void merge(CKvWriter<CKey> &writer, const CKey &key, const CValue &delta) {
    const size_t hash = writer.hashOf(key);
    writer.putWith(key, hash, sizeof(CValue), [&delta](void *dst, const CGenRecord *prev) {
      CValue value = prev ? *static_cast<const CValue*>(prev->payload()) : CValue();
      value.merge(delta);
      memcpy(dst, &value, sizeof(CValue));
    });
  }

  // Disk row plus every layer of the revision, in any order - that is what
  // commutativity buys. A null result means the key has no row at all
  bool find(const CKey &key, CValue &value) const {
    CKvGuard<CKey> guard = this->Engine_.guard();
    const size_t hash = std::hash<CKey>()(key);
    const size_t shardIndex = fastrange(hash, this->BaseCfg_.ShardsNum);
    const auto &shard = this->Engine_.shard(guard, shardIndex);

    value = CValue();
    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = shard.Disk.get()->Snapshot;
    std::string data;
    if (shard.Disk.get()->Db->Get(readOptions, DataCf_[shardIndex], slice(key), &data).ok() && data.size() == sizeof(CValue))
      memcpy(&value, data.data(), sizeof(CValue));

    for (size_t j = 0; j < shard.Layers.size(); j++) {
      const uint32_t watermark = j + 1 == shard.Layers.size() ? shard.Watermark : UINT32_MAX;
      mergeLayer(shard.Layers[j].get(), watermark, key, hash, value);
    }

    return !value.isNull();
  }

  // Top of a rank index: per-shard head scans merged by the metric, layer
  // deltas folded in - exact as of the revision, the same list a find() per
  // key would give. The scan goes deeper than the answer on purpose: the last
  // row read bounds every key the scan missed, so a layer key outside the
  // selection can only matter if tail + delta reaches the cut
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

    CMetricOrder order{&active->Def};
    size_t depth = need * 4 + 256;

    for (;;) {
      // One revision for every shard of the pass: a scan that saw the disk of
      // one revision and the windows of another would double-count a flush
      CKvGuard<CKey> guard = this->Engine_.guard();
      std::vector<std::pair<CKey, CValue>> candidates;
      std::vector<UInt<128>> tails;
      for (size_t shardIndex = 0; shardIndex < this->BaseCfg_.ShardsNum; shardIndex++)
        topFromShard(guard, *active, order, shardIndex, depth, need, candidates, tails);

      std::sort(candidates.begin(), candidates.end(), order);
      UInt<128> cut = candidates.size() >= need ? metricOf(active->Def, candidates[need-1].second) : UInt<128>();

      // Keys left unread in a shard are bounded by its tail; a tail above the
      // cut means one of them may belong in the answer - go deeper. Normally
      // the tail is far below (that is what depth buys), and a monotonic metric
      // like tx_count can't push the cut down at all
      bool deeper = false;
      for (const UInt<128> &tail: tails)
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
  void registerIndex(const std::string &name, unsigned width, UInt<128> (*extract)(const CValue&)) {
    RegisteredIndexes_.push_back(CIndexDef{name, width, extract});
  }

  // One sealed layer, one batch in memcmp order of keys: the newest record
  // per key is the cumulative delta of the whole layer - one operand per key,
  // the fold is just dropping the identities
  void writeLayer(rocksdb::DB *db, size_t shardIndex, const CLayer<CKey> *layer, const BC::Proto::BlockHashTy &stamp) final {
    layer->buildScattered();

    std::vector<std::pair<const CKey*, CValue>> folded;
    folded.reserve(layer->used());
    for (size_t b = 0; b < KvScatterBuckets && !layer->Scattered.empty(); b++) {
      kvSortBucket(layer->Scattered, layer->Bounds, b);
      for (uint32_t k = b ? layer->Bounds[b - 1] : 0, end = layer->Bounds[b]; k < end; k++) {
        const CKvSortedRef<CKey> &ref = layer->Scattered[k];
        const CValue &value = *static_cast<const CValue*>(static_cast<const CGenRecord*>(ref.Entry)->payload());
        // a delta that cancelled out is the identity operand: not written
        if (!value.isNull())
          folded.emplace_back(ref.Key, value);
      }
    }

    flushFolded(db, shardIndex, folded, stamp);
  }


private:
  // Disk half of the flush: folded deltas in key order become a merge operand
  // each - or a materialized RMW when an index needs the old value
  void flushFolded(rocksdb::DB *db,
                   size_t shardIndex,
                   const std::vector<std::pair<const CKey*, CValue>> &folded,
                   const BC::Proto::BlockHashTy &stamp) {
    rocksdb::ColumnFamilyHandle *dataCf = DataCf_[shardIndex];

    // Reserved once: without it the batch string of a whole layer doubles
    // its way up through a few hundred MB of memcpy
    rocksdb::WriteBatch batch(64 + folded.size() * (sizeof(CKey) + sizeof(CValue) + 16
                              + ActiveIndexes_.size() * 2 * (IndexRowKeyMax + sizeof(CValue) + 16)));
    this->putStamp(batch, stamp);

    // The key of the layer is the key of the row: no prefix to prepend, so the
    // slices point straight into the layer
    std::vector<rocksdb::Slice> keySlices;
    keySlices.reserve(folded.size());
    for (const auto &entry: folded)
      keySlices.emplace_back(reinterpret_cast<const char*>(entry.first), sizeof(CKey));

    if (ActiveIndexes_.empty()) {
      // No indexes: the folded delta goes to the backend as a merge operand,
      // no reads at all
      for (size_t i = 0; i < folded.size(); i++)
        batch.Merge(dataCf, keySlices[i], rocksdb::Slice(reinterpret_cast<const char*>(&folded[i].second), sizeof(CValue)));
    } else if (!folded.empty()) {
      // RMW: index row replacement needs the old value anyway, so the base row
      // is written materialized too (and deleted when it folds to the identity).
      // folded is in memcmp order: sorted_input spares rocksdb its per-call
      // sort of the whole batch
      std::vector<rocksdb::PinnableSlice> oldValues(folded.size());
      std::vector<rocksdb::Status> readResult(folded.size());
      db->MultiGet(rocksdb::ReadOptions(), dataCf, keySlices.size(),
                   keySlices.data(), oldValues.data(), readResult.data(),
                   /*sorted_input=*/true);
      for (size_t i = 0; i < folded.size(); i++) {
        CValue oldValue;
        bool hadValue = readResult[i].ok() && oldValues[i].size() == sizeof(CValue);
        if (hadValue)
          memcpy(&oldValue, oldValues[i].data(), sizeof(CValue));
        // Merge-era operands can fold to a zero row, it has no index rows
        bool hadRow = hadValue && !oldValue.isNull();

        CValue newValue = oldValue;
        newValue.merge(folded[i].second);
        bool hasRow = !newValue.isNull();

        if (hasRow)
          batch.Put(dataCf, keySlices[i], rocksdb::Slice(reinterpret_cast<const char*>(&newValue), sizeof(CValue)));
        else
          batch.Delete(dataCf, keySlices[i]);

        for (const auto &index: ActiveIndexes_) {
          UInt<128> oldMetric = metricOf(index.Def, oldValue);
          UInt<128> newMetric = metricOf(index.Def, newValue);
          // A zero metric is not in the index, so a data row and its index row
          // do not come and go together
          bool hadIndexRow = hadRow && oldMetric.nonZero();
          bool hasIndexRow = hasRow && newMetric.nonZero();
          // The row is covering, so an unchanged metric is not an unchanged
          // row: a tx of an address moves tx_count without moving balance.
          // Same metric means same key though - rewrite it in place
          bool sameKey = hadIndexRow && hasIndexRow && oldMetric == newMetric;

          uint8_t rowKey[IndexRowKeyMax];
          if (hadIndexRow && !sameKey) {
            size_t size = buildIndexRowKey(rowKey, index, oldMetric, *folded[i].first);
            batch.Delete(index.Cf[shardIndex], rocksdb::Slice(reinterpret_cast<const char*>(rowKey), size));
          }
          if (hasIndexRow) {
            size_t size = buildIndexRowKey(rowKey, index, newMetric, *folded[i].first);
            batch.Put(index.Cf[shardIndex], rocksdb::Slice(reinterpret_cast<const char*>(rowKey), size),
                      rocksdb::Slice(reinterpret_cast<const char*>(&newValue), sizeof(CValue)));
          }
        }
      }
    }

    this->writeBatch(db, batch);
    LOG_F(1, "%s: shard %zu flushed %zu keys", this->Name_.c_str(), shardIndex, folded.size());
  }

  template<typename T>
  static rocksdb::Slice slice(const T &value) {
    return rocksdb::Slice(reinterpret_cast<const char*>(&value), sizeof(T));
  }

  // One layer's delta for the key: the record is cumulative at the watermark
  // - it folds the layer whole, no chain walk
  static void mergeLayer(const CLayer<CKey> *layer, uint32_t watermark, const CKey &key, size_t hash, CValue &value) {
    if (!layer)
      return;
    if (const CGenRecord *rec = layer->find(key, hash, watermark))
      value.merge(*static_cast<const CValue*>(rec->payload()));
  }

  // Every delta of the revision above the disk, oldest first - each key
  // contributes its cumulative delta exactly once per layer
  template<typename F>
  static void forEachDelta(const typename CKvView<CKey>::CShardView &shard, F &&fn) {
    for (size_t j = 0; j < shard.Layers.size(); j++) {
      const uint32_t watermark = j + 1 == shard.Layers.size() ? shard.Watermark : UINT32_MAX;
      layerDeltas(shard.Layers[j].get(), watermark, fn);
    }
  }

  template<typename F>
  static void layerDeltas(const CLayer<CKey> *layer, uint32_t watermark, F &&fn) {
    if (!layer)
      return;
    layer->forEachAt(watermark, [&fn](const CKey &key, const CGenRecord &rec) {
      fn(key, *static_cast<const CValue*>(rec.payload()));
    });
  }

  // One shard's part of top(): the head of its index region, the windows folded
  // into it, and the window keys from below the head that the cut still lets in.
  // Everything is read at one revision, so a row and the deltas above it are
  // the pair find() would have built. Appends the shard's candidates; a shard
  // with rows left unread appends its tail, the metric bound for all of them
  void topFromShard(const CKvGuard<CKey> &guard, const CActiveIndex &index, const CMetricOrder &order, size_t shardIndex,
                    size_t depth, size_t need,
                    std::vector<std::pair<CKey, CValue>> &candidates,
                    std::vector<UInt<128>> &tails) const {
    const auto &shard = this->Engine_.shard(guard, shardIndex);
    const size_t rowKeySize = indexRowKeySize(index.Def);

    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = shard.Disk.get()->Snapshot;

    // Index rows are ordered by the inverted metric: iteration is metric-descending
    std::vector<std::pair<CKey, CValue>> selection;
    ankerl::unordered_dense::map<CKey, size_t> position;
    UInt<128> tail;
    bool exhausted = true;
    size_t unreadable = 0;

    std::unique_ptr<rocksdb::Iterator> It(shard.Disk.get()->Db->NewIterator(readOptions, index.Cf[shardIndex]));
    for (It->SeekToFirst(); It->Valid(); It->Next()) {
      rocksdb::Slice key = It->key();
      // The family holds index rows and nothing else, the size check is a guard
      if (key.size() != rowKeySize)
        continue;
      if (selection.size() == depth) {
        exhausted = false;
        break;
      }

      tail = decodeInvertedMetric(reinterpret_cast<const uint8_t*>(key.data()), index.Def.Width);

      // A row without its covering value is a row of a layout that is not this
      // one: it stays out of the selection, so the shard is not read to the end
      // and the disk read below is back on for the window keys
      if (It->value().size() != sizeof(CValue)) {
        unreadable++;
        exhausted = false;
        continue;
      }

      auto &row = selection.emplace_back();
      memcpy(static_cast<void*>(&row.first), key.data() + index.Def.Width, sizeof(CKey));
      memcpy(&row.second, It->value().data(), sizeof(CValue));
      position.emplace(row.first, selection.size() - 1);
    }

    // The layers hold a delta per key each. Summed aside instead of in place:
    // a sealed layer is immutable, but a key can sit in several of them
    std::vector<CValue> deltas(selection.size());
    forEachDelta(shard, [&](const CKey &key, const CValue &delta) {
      auto rowIt = position.find(key);
      if (rowIt != position.end())
        deltas[rowIt->second].merge(delta);
    });

    std::vector<std::pair<CKey, CValue>> merged;
    merged.reserve(selection.size());
    for (size_t i = 0; i < selection.size(); i++) {
      selection[i].second.merge(deltas[i]);
      // A key the windows drove to zero is gone, the way the flush drops it -
      // a null row and a zero metric alike
      if (!selection[i].second.isNull() && metricOf(index.Def, selection[i].second).nonZero())
        merged.push_back(selection[i]);
    }

    // Shard-local cut. The final one is taken over all shards and can only be
    // higher, so filtering by this one never drops a key that belongs in the top
    UInt<128> shardCut;
    if (merged.size() >= need) {
      std::nth_element(merged.begin(), merged.begin() + (need - 1), merged.end(), order);
      shardCut = metricOf(index.Def, merged[need-1].second);
    }

    // The rest of the layers: a key's disk value is bounded by the tail (a
    // shard read to the end leaves nothing on disk to bound at all), so only a
    // delta big enough to lift that bound over the cut is worth a read
    const UInt<128> bound = exhausted ? UInt<128>() : tail;
    ankerl::unordered_dense::map<CKey, CValue> extraDeltas;
    forEachDelta(shard, [&](const CKey &key, const CValue &delta) {
      if (position.count(key))
        return;
      extraDeltas[key].merge(delta);
    });

    std::vector<std::pair<CKey, CValue>> extras;
    for (const auto &entry: extraDeltas) {
      // The sign of a delta lives at the column width - its top bit, canonical
      // bytes above are zero
      UInt<128> metric = metricOf(index.Def, entry.second);
      bool lowers = metric.isZero() || (metric >> (8 * index.Def.Width - 1)).nonZero();
      // A shard read to the end holds a row for every non-zero metric it has,
      // so a key outside the selection sits at zero on disk: only its own
      // delta can lift it into the index
      if (exhausted && lowers)
        continue;
      // Below a saturated cut pruning is legal for any key: a delta lowering
      // the metric can't lift it over the cut, a positive one is bounded by
      // tail + delta. With no cut the answer takes every key the layers hold
      if (shardCut.nonZero()) {
        if (lowers)
          continue;
        if (bound <= UInt<128>::max() - metric && bound + metric < shardCut)
          continue;
      }
      extras.emplace_back(entry.first, entry.second);
    }

    if (!extras.empty()) {
      // Every extra needs its base read: a key can be outside the index and
      // still have a data row - that is what a zero metric looks like. The
      // pruning above is what keeps this off the whole window
      std::vector<rocksdb::Slice> keySlices;
      std::vector<rocksdb::ColumnFamilyHandle*> families;
      keySlices.reserve(extras.size());
      families.reserve(extras.size());
      for (const auto &extra: extras) {
        keySlices.emplace_back(reinterpret_cast<const char*>(&extra.first), sizeof(CKey));
        families.push_back(DataCf_[shardIndex]);
      }
      std::vector<std::string> values;
      std::vector<rocksdb::Status> readResult = shard.Disk.get()->Db->MultiGet(readOptions, families, keySlices, &values);

      for (size_t i = 0; i < extras.size(); i++) {
        // Nothing on disk is a legal base: the key is new in the windows
        CValue value;
        if (readResult[i].ok() && values[i].size() == sizeof(CValue))
          memcpy(&value, values[i].data(), sizeof(CValue));
        value.merge(extras[i].second);
        if (!value.isNull() && metricOf(index.Def, value).nonZero())
          candidates.emplace_back(extras[i].first, value);
      }
    }

    if (unreadable)
      LOG_F(ERROR, "%s: index '%s' of shard %zu has %zu rows in a foreign layout, rebuild the database",
            this->Name_.c_str(), index.Def.Name.c_str(), shardIndex, unreadable);

    candidates.insert(candidates.end(), merged.begin(), merged.end());
    if (!exhausted)
      tails.push_back(tail);
  }

  // The index transition writes the way the flush does - no journal. What
  // makes it durable is the memtable flush before "xcfg"
  static rocksdb::WriteOptions transitionWriteOptions() {
    rocksdb::WriteOptions writeOptions;
    writeOptions.disableWAL = true;
    return writeOptions;
  }

  // Build the rows of the given indexes by a single scan of the data family.
  // A crash in the middle repeats the build on restart: "xcfg" is written only
  // after success, and re-putting an existing row is legal (last write wins)
  bool buildIndexes(size_t shardIndex, const std::vector<const CActiveIndex*> &indexes) {
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();
    LOG_F(INFO, "%s: building %zu indexes for shard %zu...", this->Name_.c_str(), indexes.size(), shardIndex);
    auto startTime = std::chrono::steady_clock::now();
    size_t rows = 0;

    rocksdb::ReadOptions scanOptions;
    scanOptions.fill_cache = false;
    rocksdb::WriteBatch batch;
    size_t batchSize = 0;
    auto flushBatch = [&]() {
      if (!storage->Write(transitionWriteOptions(), &batch).ok())
        return false;
      batch.Clear();
      batchSize = 0;
      return true;
    };

    bool writeOk = true;
    std::unique_ptr<rocksdb::Iterator> It(storage->NewIterator(scanOptions, DataCf_[shardIndex]));
    for (It->SeekToFirst(); It->Valid() && writeOk; It->Next()) {
      rocksdb::Slice key = It->key();
      if (key.size() != sizeof(CKey))
        continue;
      rocksdb::Slice value = It->value();
      if (value.size() != sizeof(CValue))
        continue;
      CValue baseValue;
      memcpy(&baseValue, value.data(), sizeof(CValue));
      if (baseValue.isNull())
        continue;
      CKey baseKey;
      memcpy(static_cast<void*>(&baseKey), key.data(), sizeof(CKey));
      for (const CActiveIndex *index: indexes) {
        UInt<128> metric = metricOf(index->Def, baseValue);
        if (metric.isZero())
          continue;
        uint8_t rowKey[IndexRowKeyMax];
        size_t size = buildIndexRowKey(rowKey, *index, metric, baseKey);
        batch.Put(index->Cf[shardIndex], rocksdb::Slice(reinterpret_cast<const char*>(rowKey), size),
                  rocksdb::Slice(reinterpret_cast<const char*>(&baseValue), sizeof(CValue)));
        batchSize++;
        rows++;
      }
      if (batchSize >= 65536)
        writeOk = flushBatch();
    }

    if (!writeOk || !It->status().ok() || !flushBatch()) {
      LOG_F(ERROR, "%s: index build failed for shard %zu", this->Name_.c_str(), shardIndex);
      return false;
    }

    LOG_F(INFO, "%s: %zu index rows built for shard %zu (%.1lf seconds)", this->Name_.c_str(), rows, shardIndex,
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() / 1000.0);
    return true;
  }

private:
  std::vector<CIndexDef> RegisteredIndexes_;
  // The data family of every shard, cached: every read and every flush wants it
  std::vector<rocksdb::ColumnFamilyHandle*> DataCf_;
  std::vector<CActiveIndex> ActiveIndexes_;
  // Configured but not maintained yet: the initial catch-up runs without them
  std::vector<CActiveIndex> DeferredIndexes_;
  std::string DeferredCfg_;
};

}
}
