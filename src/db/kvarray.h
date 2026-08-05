// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// Fixed-size element array with a running aggregate, over the window engine.
// Besides the fact itself, every element carries in its Aggregate field the
// fold of all elements up to and including it, so a range read returns ready
// prefix sums (address history with the balance after each tx, balance chart).
//
// One window is normalized to "trim the durable prefix, then append":
//     final = durable[0 : durableCount - BaseTrim] ++ Tail
// BaseTrim never decreases while the window lives and the Tail holds only
// elements born in it, so a reorg that truncates the durable end and appends an
// alternate branch is an ordinary pair of operations with nothing special about
// it - the append is not lost and needs no rollback of the window.
//
// What the engine adds is that several windows stand between the caller and the
// disk, so neither a read nor a flush sees one Tail any more: compose() replays
// them in order into the same shape, trimming what an older window appended
// before it reaches the durable prefix. Everything below works on that one
// composed shape and knows nothing about how many windows it came from.
//
// CItem requirements: trivially copyable, fixed size, public uint64_t Aggregate
// field with the semantics above.

#include "db/kvbase.h"

#include <deque>

namespace BC {
namespace DB {

template<typename CKey, typename CItem>
class CKvArrayBase : public CKvDatabase<CKey> {
private:
#pragma pack(push, 1)
  // Arena record of a window: the Tail follows it. The key is not stored here -
  // it already lives in the map slot, and the fold reads it from there
  struct CHeader {
    uint64_t BaseTrim;   // elements dropped from what lies below this window
    uint64_t TailCount;  // elements appended over what survives (the payload)
  };

  struct CChunkKey {
    CKey Key;
    uint64_t Index;
  };

  // Per-key metadata row: element count plus the aggregate of the last durable
  // element - the rebase point for the unflushed windows
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

    // Recorded in the SST files of every existing database - the name of the
    // class that is gone, and it stays that way: renaming it means a reindex
    virtual const char* Name() const override {
      return "CBaseArrayAggregated";
    }
  };

public:
  CKvArrayBase(const std::string &name, size_t chunkSize) :
    CKvDatabase<CKey>(name), ChunkSize_(chunkSize) {}

  // The flusher dispatches writeSegments implemented at this level: stop it
  // while the dispatch is still valid (~CKvDatabase's shutdown is a no-op then)
  ~CKvArrayBase() override { this->Engine_.shutdown(); }

  rocksdb::MergeOperator *mergeOperator() final { return new MergeOperator(); }

  // Fill cursor of a tail: elements arrive carrying their delta in Aggregate and
  // land folded into the window-relative running sum, rebased to absolute values
  // at flush. Holds that sum, so the caller hands over one element at a time
  class CTailWriter {
  public:
    CTailWriter() = default;
    explicit CTailWriter(CItem *cursor) : Cursor_(cursor) {}

    void append(const CItem &item) {
      Running_ += item.Aggregate;
      *Cursor_ = item;
      Cursor_->Aggregate = Running_;
      Cursor_++;
    }

  private:
    CItem *Cursor_ = nullptr;
    uint64_t Running_ = 0;
  };

  // The key's whole tail in this window, at its final size, written once. The
  // caller counts its elements first and fills the cursor in chain order - which
  // is why a unit of connect may be tens of thousands of blocks without the
  // window growing beyond what those blocks actually add: growing a tail in
  // place would recopy it on every append, and one connect unit is one window
  CTailWriter allocTail(CKvWriter<CKey> &writer, const CKey &key, size_t count) {
    const size_t hash = writer.hashOf(key);
    // A second tail for one key would orphan the elements of the first: the map
    // slot holds one record, and the fold below reads exactly that one
    assert(!writer.findOwn(key, hash) && "key already has a tail in this window");

    CHeader *header = static_cast<CHeader*>(writer.alloc(hash, sizeof(CHeader) + count*sizeof(CItem)));
    header->BaseTrim = 0;
    header->TailCount = count;

    writer.update(key, hash, header);
    return CTailWriter(tail(header));
  }

  // Drop the last count elements. The Tail of this set goes first, only what is
  // left over is charged to what lies below it; elements appended afterwards
  // start a fresh Tail over the new end
  void truncate(CKvWriter<CKey> &writer, const CKey &key, size_t count) {
    const size_t hash = writer.hashOf(key);
    const CHeader *prev = static_cast<const CHeader*>(writer.findOwn(key, hash));
    const uint64_t prevCount = prev ? prev->TailCount : 0;
    const uint64_t fromTail = std::min<uint64_t>(count, prevCount);
    const uint64_t newCount = prevCount - fromTail;

    CHeader *header = static_cast<CHeader*>(writer.alloc(hash, sizeof(CHeader) + newCount*sizeof(CItem)));
    header->BaseTrim = (prev ? prev->BaseTrim : 0) + (count - fromTail);
    header->TailCount = newCount;
    if (newCount)
      memcpy(static_cast<void*>(tail(header)), tail(prev), newCount*sizeof(CItem));

    writer.update(key, hash, header);
  }

  bool query(const CKey &key, size_t from, size_t count, xmstream &result, size_t *totalCount) {
    if (count == 0)
      return false;

    CKvGuard<CKey> guard = this->Engine_.guard();
    const size_t hash = std::hash<CKey>()(key);
    auto layers = this->Engine_.layers(guard, fastrange(hash, this->BaseCfg_.ShardsNum));
    rocksdb::DB *storage = layers.Db;

    // The windows of this revision, replayed into one: below them is exactly
    // what its snapshot holds
    std::vector<CItem> composedTail;
    uint64_t baseTrim = 0;
    compose(layers, key, hash, baseTrim, composedTail);

    size_t firstChunk = from / ChunkSize_;
    size_t lastChunk = (from + count - 1) / ChunkSize_;

    // Metadata, data chunks and the boundary chunk come from the revision's
    // snapshot, the same one the windows above were chosen against
    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = layers.Snapshot;
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
      // Missing metadata is not an error: the key may live only in the windows
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

    const uint64_t tailCount = composedTail.size();
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

      // Composed tail: rebase window-relative running sums by the surviving prefix
      if (tailCount) {
        int64_t available = static_cast<int64_t>(tailCount) - inMemoryOffset;
        int64_t needToRead = std::min(available, remaining);
        if (needToRead > 0) {
          CItem *items = static_cast<CItem*>(result.reserve(needToRead * sizeof(CItem)));
          memcpy(static_cast<void*>(items), composedTail.data() + inMemoryOffset, needToRead * sizeof(CItem));
          for (int64_t i = 0; i < needToRead; i++)
            items[i].Aggregate += baseAggregate;
        }
      }
    }

    return ok;
  }

protected:
  // Fold of the batch: the windows of every key replayed into one trim-and-
  // append, then written the way a single window always was
  void writeSegments(rocksdb::DB *db,
                     size_t shardIndex,
                     const CWindow<CKey> *const *segments,
                     size_t count,
                     const BC::Proto::BlockHashTy &stamp) final {
    // Sorted references: keys of all windows in one order, so a group is a
    // key's whole history inside this batch and the batch itself reaches
    // rocksdb in memcmp order
    struct CSortedRef {
      uint64_t Prefix;
      const CKey *Key;
      const CHeader *Header;
      uint32_t Order;
    };

    size_t entries = 0;
    for (size_t i = 0; i < count; i++)
      entries += segments[i]->Map.used();

    std::vector<CSortedRef> refs;
    refs.reserve(entries);
    for (uint32_t order = 0; order < count; order++) {
      segments[order]->Map.forEachCurrent([&refs, order](const CKey &key, void *value) {
        uint64_t prefix = 0;
        memcpy(&prefix, &key, std::min(sizeof(prefix), sizeof(CKey)));
        refs.push_back({xhtobe(prefix), &key, static_cast<const CHeader*>(value), order});
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

    // One composed operation per key, in key order
    std::vector<const CKey*> allKeys;
    std::vector<uint64_t> allTrims;
    std::vector<size_t> allTailFirst;
    std::vector<size_t> allTailCount;
    std::vector<CItem> tailPool;
    for (size_t i = 0; i < refs.size(); ) {
      size_t j = i;
      uint64_t baseTrim = 0;
      const size_t first = tailPool.size();
      while (j != refs.size() && memcmp(refs[j].Key, refs[i].Key, sizeof(CKey)) == 0) {
        replay(refs[j].Header, baseTrim, tailPool, first);
        j++;
      }

      allKeys.push_back(refs[i].Key);
      allTrims.push_back(baseTrim);
      allTailFirst.push_back(first);
      allTailCount.push_back(tailPool.size() - first);
      i = j;
    }

    std::vector<rocksdb::Slice> allKeySlices;
    allKeySlices.reserve(allKeys.size());
    for (size_t i = 0; i < allKeys.size(); i++)
      allKeySlices.emplace_back((const char*)allKeys[i], sizeof(CKey));

    // Metadata and the boundary chunks of trimmed keys are one consistent view
    std::vector<std::string> metadata;
    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = db->GetSnapshot();
    auto metadataReadResult = db->MultiGet(readOptions, allKeySlices, &metadata);

    rocksdb::WriteBatch batch;
    this->putStamp(batch, stamp);

    // Absolute aggregates are built here and never written into a published
    // window: a query running concurrently keeps reading window-relative sums
    std::vector<uint8_t> scratch;

    for (size_t i = 0; i < allKeys.size(); i++) {
      const uint64_t headerBaseTrim = allTrims[i];
      const CItem *tailItems = tailPool.data() + allTailFirst[i];
      const uint64_t headerTailCount = allTailCount[i];

      int64_t durableCount = 0;
      uint64_t durableAggregate = 0;
      if (metadataReadResult[i].ok() && metadata[i].size() == sizeof(CMetadata)) {
        const CMetadata *currentMeta = reinterpret_cast<const CMetadata*>(metadata[i].data());
        durableCount = currentMeta->Count;
        durableAggregate = currentMeta->Aggregate;
      }

      const int64_t baseCount = durableCount - static_cast<int64_t>(headerBaseTrim);
      const int64_t newCount = baseCount + static_cast<int64_t>(headerTailCount);

      // Fail closed: a batch that trims more than the array holds means the
      // operations reaching this database no longer describe one chain, and
      // clamping it would silently write an array nobody can rebuild
      if (baseCount < 0) {
        LOG_F(ERROR, "%s: shard %zu, window trims %llu elements of an array holding %lld",
              this->Name_.c_str(), shardIndex,
              static_cast<unsigned long long>(headerBaseTrim), static_cast<long long>(durableCount));
        abort();
      }

      // The rebase point of the batch: the aggregate of the last element that
      // survived the trim, read back from its chunk when the trim moved it
      uint64_t baseAggregate = durableAggregate;
      if (headerBaseTrim) {
        baseAggregate = 0;
        if (baseCount > 0 && !aggregateAt(db, readOptions, *allKeys[i], baseCount - 1, baseAggregate)) {
          LOG_F(ERROR, "%s: shard %zu, can't read back the aggregate of element %lld",
                this->Name_.c_str(), shardIndex, static_cast<long long>(baseCount - 1));
          abort();
        }
      }

      uint64_t newAggregate = baseAggregate;
      if (headerTailCount) {
        newAggregate = baseAggregate + tailItems[headerTailCount - 1].Aggregate;

        size_t remaining = headerTailCount;
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

    db->ReleaseSnapshot(readOptions.snapshot);
    this->writeBatch(db, batch);
    LOG_F(1, "%s: shard %zu flushed %zu windows, %zu keys", this->Name_.c_str(), shardIndex, count, allKeys.size());
  }

private:
  // One window applied on top of what the older ones left: its trim eats the
  // accumulated tail first and only then reaches down, and its elements are
  // rebased onto the last survivor. The result is the same trim-and-append
  // shape one window has, which is why nothing below this function had to change
  static void replay(const CHeader *header, uint64_t &baseTrim, std::vector<CItem> &tailPool, size_t first) {
    const size_t accumulated = tailPool.size() - first;
    if (header->BaseTrim <= accumulated) {
      tailPool.resize(tailPool.size() - header->BaseTrim);
    } else {
      baseTrim += header->BaseTrim - accumulated;
      tailPool.resize(first);
    }

    const uint64_t base = tailPool.size() > first ? tailPool.back().Aggregate : 0;
    const CItem *items = tail(header);
    for (uint64_t i = 0; i < header->TailCount; i++) {
      CItem &item = tailPool.emplace_back(items[i]);
      item.Aggregate += base;
    }
  }

  // The revision's windows for one key, replayed oldest to newest
  void compose(const typename CKvEngine<CKey>::CLayers &layers,
               const CKey &key,
               size_t hash,
               uint64_t &baseTrim,
               std::vector<CItem> &composedTail) const {
    for (size_t i = 0; i < layers.Count; i++) {
      if (const void *entry = layers.Windows[i].get()->Map.find(key, hash))
        replay(static_cast<const CHeader*>(entry), baseTrim, composedTail, 0);
    }
  }

  int64_t chunksFor(int64_t count) const {
    return count / static_cast<int64_t>(ChunkSize_) + (count % static_cast<int64_t>(ChunkSize_) != 0);
  }

  // Absolute copy of a piece of the Tail; the published windows keep their
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

}
}
