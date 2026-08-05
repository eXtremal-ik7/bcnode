// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// Additive aggregation over the window engine.
// A window holds one folded delta per key, and the value being commutative and
// associative is what the whole family rests on: layers are summed instead of
// shadowed, so a read folds every window of the revision into the disk row and
// a flush folds every window of the batch into one operand.
//
// No lock pairs the two-level read against the flush: the revision pairs them
// by construction - its windows are exactly what its snapshot does not hold yet.
//
// Shard key space is split into disjoint prefix regions:
//   data rows    'd' ++ key                               -> value
//   index rows   'i' ++ indexId ++ be64(~metric) ++ key   -> value (same shard)
//   service keys "stamp", "basecfg", "xcfg" - outside both regions.
// Disjointness is what makes an index droppable by a range tombstone and
// buildable by a single scan of the data region.
//
// CValue requirements: trivially copyable, default-constructed state is the
// identity, merge() commutative/associative, negate() gives the inverse delta
// for disconnect.

#include "db/kvbase.h"

#include <unordered_map>

namespace BC {
namespace DB {

template<typename CKey, typename CValue>
class CKvMergeBase : public CKvDatabase<CKey> {
protected:
  struct CIndexDef {
    std::string Name;
    uint64_t (*Extract)(const CValue&);
  };

private:
  // Window record: the delta this unit folded for the key. The key rides along
  // because the flush reads groups through sorted references, not through slots
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

    // Recorded in the SST files of every existing database - the name of the
    // class that is gone, and it stays that way: renaming it means a reindex
    virtual const char* Name() const override {
      return "CBaseMerge";
    }
  };

public:
  CKvMergeBase(const std::string &name) : CKvDatabase<CKey>(name) {}

  // The flusher dispatches writeSegments implemented at this level: stop it
  // while the dispatch is still valid (~CKvDatabase's shutdown is a no-op then)
  ~CKvMergeBase() override { this->Engine_.shutdown(); }

  rocksdb::MergeOperator *mergeOperator() final { return new MergeOperator(); }

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

  // Write side: the unit's own delta for the key, folded into whatever it has
  // written for it already
  void merge(CKvWriter<CKey> &writer, const CKey &key, const CValue &delta) {
    const size_t hash = writer.hashOf(key);
    CHeader *header = static_cast<CHeader*>(writer.alloc(hash, sizeof(CHeader)));
    header->Key = key;
    const CHeader *prev = static_cast<const CHeader*>(writer.findOwn(key, hash));
    header->Value = prev ? prev->Value : CValue();
    header->Value.merge(delta);
    writer.update(key, hash, header);
  }

  // Disk row plus every window of the revision, in any order - that is what
  // commutativity buys. A null result means the key has no row at all
  bool find(const CKey &key, CValue &value) const {
    CKvGuard<CKey> guard = this->Engine_.guard();
    const size_t hash = std::hash<CKey>()(key);
    auto layers = this->Engine_.layers(guard, fastrange(hash, this->BaseCfg_.ShardsNum));

    value = CValue();
    CDataRowKey rowKey;
    makeDataRowKey(rowKey, key);
    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = layers.Snapshot;
    std::string data;
    if (layers.Db->Get(readOptions, slice(rowKey), &data).ok() && data.size() == sizeof(CValue))
      memcpy(&value, data.data(), sizeof(CValue));

    for (size_t i = 0; i < layers.Count; i++) {
      if (const void *entry = layers.Windows[i].get()->Map.find(key, hash))
        value.merge(static_cast<const CHeader*>(entry)->Value);
    }

    return !value.isNull();
  }

  // Top of a rank index: per-shard head scans merged by the metric, exact as of
  // the revision - the same list a find() per key would give. Index rows hold
  // what the last flush wrote, so the windows are folded in here the way find()
  // folds them into a point read.
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
      // One revision for every shard of the pass: a scan that saw the disk of
      // one revision and the windows of another would double-count a flush
      CKvGuard<CKey> guard = this->Engine_.guard();
      std::vector<std::pair<CKey, CValue>> candidates;
      std::vector<uint64_t> tails;
      for (size_t shardIndex = 0; shardIndex < this->BaseCfg_.ShardsNum; shardIndex++)
        topFromShard(guard, active->Id, order, shardIndex, depth, need, candidates, tails);

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

  // Fold of the batch: every window's delta for a key summed into one operand.
  // Two modes, as before - a merge operand when no index needs the old value,
  // a materialized RMW when one does
  void writeSegments(rocksdb::DB *db,
                     size_t shardIndex,
                     const CWindow<CKey> *const *segments,
                     size_t count,
                     const BC::Proto::BlockHashTy &stamp) final {
    // Sorted references, as in the KV fold: the batch reaches rocksdb in
    // memcmp order of keys, which is what the memtable insert hint wants
    struct CSortedRef {
      uint64_t Prefix;
      const CHeader *Header;
    };

    size_t entries = 0;
    for (size_t i = 0; i < count; i++)
      entries += segments[i]->Map.used();

    std::vector<CSortedRef> refs;
    refs.reserve(entries);
    for (size_t order = 0; order < count; order++) {
      segments[order]->Map.forEachCurrent([&refs](const CKey &key, void *value) {
        uint64_t prefix = 0;
        memcpy(&prefix, &key, std::min(sizeof(prefix), sizeof(CKey)));
        refs.push_back({xhtobe(prefix), static_cast<const CHeader*>(value)});
      });
    }

    std::sort(refs.begin(), refs.end(), [](const CSortedRef &l, const CSortedRef &r) {
      if (l.Prefix != r.Prefix)
        return l.Prefix < r.Prefix;
      return memcmp(&l.Header->Key, &r.Header->Key, sizeof(CKey)) < 0;
    });

    // One entry per key, the group summed. A delta that cancelled out is the
    // identity operand and is not written at all
    std::vector<std::pair<const CKey*, CValue>> folded;
    folded.reserve(refs.size());
    for (size_t i = 0; i < refs.size(); ) {
      size_t j = i;
      CValue value;
      while (j != refs.size() && memcmp(&refs[j].Header->Key, &refs[i].Header->Key, sizeof(CKey)) == 0) {
        value.merge(refs[j].Header->Value);
        j++;
      }
      if (!value.isNull())
        folded.emplace_back(&refs[i].Header->Key, value);
      i = j;
    }

    rocksdb::WriteBatch batch;
    this->putStamp(batch, stamp);

    std::vector<CDataRowKey> rowKeys;
    rowKeys.reserve(folded.size());
    for (const auto &entry: folded)
      makeDataRowKey(rowKeys.emplace_back(), *entry.first);

    // Slices are built after the fill: emplace_back may reallocate rowKeys
    std::vector<rocksdb::Slice> keySlices;
    keySlices.reserve(rowKeys.size());
    for (const auto &rowKey: rowKeys)
      keySlices.emplace_back(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey));

    if (ActiveIndexes_.empty()) {
      // No indexes: the folded delta goes to the backend as a merge operand,
      // no reads at all
      for (size_t i = 0; i < folded.size(); i++)
        batch.Merge(keySlices[i], rocksdb::Slice(reinterpret_cast<const char*>(&folded[i].second), sizeof(CValue)));
    } else if (!folded.empty()) {
      // RMW: index row replacement needs the old value anyway, so the base row
      // is written materialized too (and deleted when it folds to the identity)
      std::vector<std::string> oldValues;
      auto readResult = db->MultiGet(rocksdb::ReadOptions(), keySlices, &oldValues);
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
            makeIndexRowKey(rowKey, index.Id, oldMetric, *folded[i].first);
            batch.Delete(rocksdb::Slice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey)));
          }
          if (hasRow) {
            makeIndexRowKey(rowKey, index.Id, newMetric, *folded[i].first);
            batch.Put(rocksdb::Slice(reinterpret_cast<const char*>(&rowKey), sizeof(rowKey)),
                      rocksdb::Slice(reinterpret_cast<const char*>(&newValue), sizeof(CValue)));
          }
        }
      }
    }

    this->writeBatch(db, batch);
    LOG_F(1, "%s: shard %zu flushed %zu windows, %zu keys", this->Name_.c_str(), shardIndex, count, folded.size());
  }

private:
  template<typename T>
  static rocksdb::Slice slice(const T &value) {
    return rocksdb::Slice(reinterpret_cast<const char*>(&value), sizeof(T));
  }

  // One shard's part of top(): the head of its index region, the windows folded
  // into it, and the window keys from below the head that the cut still lets in.
  // Everything is read at one revision, so a row and the deltas above it are
  // the pair find() would have built. Appends the shard's candidates; a shard
  // with rows left unread appends its tail, the metric bound for all of them
  void topFromShard(const CKvGuard<CKey> &guard, uint8_t indexId, const CMetricOrder &order, size_t shardIndex,
                    size_t depth, size_t need,
                    std::vector<std::pair<CKey, CValue>> &candidates,
                    std::vector<uint64_t> &tails) const {
    auto layers = this->Engine_.layers(guard, shardIndex);
    const uint8_t seekPrefix[2] = {'i', indexId};

    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = layers.Snapshot;

    // Index rows are ordered by the inverted metric: iteration is metric-descending
    std::vector<std::pair<CKey, CValue>> selection;
    std::unordered_map<CKey, size_t> position;
    uint64_t tail = 0;
    bool exhausted = true;
    size_t unreadable = 0;

    std::unique_ptr<rocksdb::Iterator> It(layers.Db->NewIterator(readOptions));
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

    // The windows hold a folded delta per key each. Summed aside instead of in
    // place: a sealed window is immutable, but a key can sit in several of them
    std::vector<CValue> deltas(selection.size());
    for (size_t w = 0; w < layers.Count; w++) {
      layers.Windows[w].get()->Map.forEachCurrent([&](const CKey &key, const void *data) {
        auto rowIt = position.find(key);
        if (rowIt != position.end())
          deltas[rowIt->second].merge(static_cast<const CHeader*>(data)->Value);
      });
    }

    std::vector<std::pair<CKey, CValue>> merged;
    merged.reserve(selection.size());
    for (size_t i = 0; i < selection.size(); i++) {
      selection[i].second.merge(deltas[i]);
      // A key the windows drove to zero is gone, the way the flush drops it
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

    // The rest of the windows: a key's disk value is bounded by the tail (a
    // shard read to the end leaves nothing on disk to bound at all), so only a
    // delta big enough to lift that bound over the cut is worth a read
    const uint64_t bound = exhausted ? 0 : tail;
    std::unordered_map<CKey, CValue> extraDeltas;
    for (size_t w = 0; w < layers.Count; w++) {
      layers.Windows[w].get()->Map.forEachCurrent([&](const CKey &key, const void *data) {
        if (position.count(key))
          return;
        extraDeltas[key].merge(static_cast<const CHeader*>(data)->Value);
      });
    }

    std::vector<std::pair<CKey, CValue>> extras;
    for (const auto &entry: extraDeltas) {
      int64_t metric = static_cast<int64_t>(order.Extract(entry.second));
      // A delta that lowers the metric can't lift its key over the cut. A metric
      // narrower than 64 bits reads its negatives as huge positives - that costs
      // a read and nothing else, the value below is computed, not extrapolated
      if (metric <= 0)
        continue;
      if (bound <= UINT64_MAX - static_cast<uint64_t>(metric) &&
          bound + static_cast<uint64_t>(metric) < shardCut)
        continue;
      extras.emplace_back(entry.first, entry.second);
    }

    if (!extras.empty()) {
      // A shard read to the end has an index row for every non-null data row it
      // holds, so a key outside the selection has no base to read - it is new in
      // the windows. Skipping the reads is what keeps a fresh database (empty
      // index, everything still in memory) off a full-window MultiGet
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
        readResult = layers.Db->MultiGet(readOptions, keySlices, &values);
      }

      for (size_t i = 0; i < extras.size(); i++) {
        // Nothing on disk is a legal base: the key is new in the windows
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
  std::vector<CIndexDef> RegisteredIndexes_;
  std::vector<CActiveIndex> ActiveIndexes_;
};

}
}
