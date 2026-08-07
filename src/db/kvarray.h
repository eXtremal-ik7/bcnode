// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// Fixed-size element array with a running aggregate: every element carries in
// its Aggregate field the fold of all elements up to and including it, so a
// range read returns ready prefix sums (address history with the balance
// after each tx).
//
// A key's layer state is normalized to "trim below, then append":
//     final = below[0 : belowCount - BaseTrim] ++ Items[0 : Count]
// so a reorg that truncates the durable end and appends an alternate branch
// is an ordinary pair of operations, no rollback needed. Per layer the tail
// lives in one contiguous buffer with vector-style growth; the generation
// records are small descriptors, cumulative for the layer, so a read takes
// the newest one at its watermark and has the whole composed state - no
// replay, no per-element pointer chase. Buffers and descriptors die with the
// layer (MLog frees nothing), which is what keeps every pinned reader valid.
//
// CItem requirements: trivially copyable, fixed size, public uint64_t
// Aggregate field with the semantics above.

#include "db/kvbase.h"

#include <deque>

namespace BC {
namespace DB {

template<typename CKey, typename CItem>
class CKvArrayBase : public CKvDatabase<CKey> {
private:
#pragma pack(push, 1)
  // Layer-state descriptor, one generation record each: cumulative within the
  // layer, so the newest record at a watermark is the whole composed state.
  // Slots past Count belong to newer generations; a truncate caps Capacity so
  // they are never overwritten under an older reader
  struct CHeader {
    uint64_t BaseTrim;   // elements dropped from what lies below this layer
    uint64_t Count;      // elements in Items, layer-relative running Aggregate
    uint64_t Capacity;   // Items slots; Count == Capacity forbids in-place append
    CItem *Items;        // the layer's contiguous tail of this key
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

  // One layer's surviving tail piece in a composed read
  struct CTailSegment {
    const CItem *Items;
    uint64_t Count;
  };

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

  // The flusher dispatches the folds implemented at this level: stop it
  // while the dispatch is still valid (~CKvDatabase's shutdown is a no-op then)
  ~CKvArrayBase() override { this->Engine_.shutdown(); }

  rocksdb::MergeOperator *mergeOperator() final { return new MergeOperator(); }

  // Fill cursor of a tail: elements arrive carrying their delta in Aggregate and
  // land folded into the layer-relative running sum, rebased to absolute values
  // at flush. Holds that sum, so the caller hands over one element at a time
  class CTailWriter {
  public:
    CTailWriter() = default;
    CTailWriter(CItem *cursor, uint64_t running) : Cursor_(cursor), Running_(running) {}

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

  // The key's tail elements of this unit, appended to the layer's contiguous
  // buffer: in place while the capacity lasts - the slots past the committed
  // Count are invisible to every reader - or into a doubled buffer with the
  // prefix copied, the outgrown one stays behind for pinned readers. Filling
  // the elements after the call is legal: the generation is uncommitted
  CTailWriter allocTail(CKvWriter<CKey> &writer, const CKey &key, size_t count) {
    const size_t hash = writer.hashOf(key);
    // A second tail for one key would drop the elements of the first: the
    // descriptor of an uncommitted unit is replaced, not chained over
    assert(!writer.findOwn(key, hash) && "key already has a tail in this unit");
    CItem *cursor = nullptr;
    uint64_t running = 0;
    writer.putWith(key, hash, sizeof(CHeader), [&](void *dst, const CGenRecord *prevRec) {
      CHeader *header = static_cast<CHeader*>(dst);
      const CHeader *prev = prevRec ? static_cast<const CHeader*>(prevRec->payload()) : nullptr;
      const uint64_t prevCount = prev ? prev->Count : 0;
      header->BaseTrim = prev ? prev->BaseTrim : 0;
      header->Count = prevCount + count;
      if (prev && header->Count <= prev->Capacity) {
        header->Capacity = prev->Capacity;
        header->Items = prev->Items;
      } else {
        // Exact on first touch - the unit's count is known up front - then
        // doubled, so the copies stay amortized O(1) per element
        header->Capacity = std::max<uint64_t>(header->Count, 2 * (prev ? prev->Capacity : 0));
        header->Items = static_cast<CItem*>(writer.allocRaw(hash, header->Capacity * sizeof(CItem)));
        if (prevCount)
          memcpy(static_cast<void*>(header->Items), prev->Items, prevCount * sizeof(CItem));
      }
      cursor = header->Items + prevCount;
      running = prevCount ? header->Items[prevCount - 1].Aggregate : 0;
    });
    return CTailWriter(cursor, running);
  }

  // Drop the last count elements. The layer's tail goes first, only what is
  // left over is charged to what lies below the layer. No copy: the new
  // descriptor sees a shorter prefix of the same buffer
  void truncate(CKvWriter<CKey> &writer, const CKey &key, size_t count) {
    const size_t hash = writer.hashOf(key);
    writer.putWith(key, hash, sizeof(CHeader), [count](void *dst, const CGenRecord *prevRec) {
      CHeader *header = static_cast<CHeader*>(dst);
      const CHeader *prev = prevRec ? static_cast<const CHeader*>(prevRec->payload()) : nullptr;
      const uint64_t prevCount = prev ? prev->Count : 0;
      const uint64_t fromTail = std::min<uint64_t>(count, prevCount);
      header->BaseTrim = (prev ? prev->BaseTrim : 0) + (count - fromTail);
      header->Count = prevCount - fromTail;
      // The dropped slots stay visible to older generations: cap the buffer,
      // the next append reallocates instead of overwriting them
      header->Capacity = header->Count;
      header->Items = prev ? prev->Items : nullptr;
    });
  }

  bool query(const CKey &key, size_t from, size_t count, xmstream &result, size_t *totalCount) {
    if (count == 0)
      return false;

    CKvGuard<CKey> guard = this->Engine_.guard();
    const size_t hash = std::hash<CKey>()(key);
    const auto &shard = this->Engine_.shard(guard, fastrange(hash, this->BaseCfg_.ShardsNum));
    rocksdb::DB *storage = shard.Disk.get()->Db;

    // The layers of this revision composed into surviving segments, oldest
    // first: below them is exactly what its snapshot holds
    std::vector<CTailSegment> segments;
    uint64_t baseTrim = 0;
    const uint64_t tailCount = compose(shard, key, hash, baseTrim, segments);

    size_t firstChunk = from / ChunkSize_;
    size_t lastChunk = (from + count - 1) / ChunkSize_;

    // Metadata, data chunks and the boundary chunk come from the revision's
    // snapshot, the same one the layers above were chosen against
    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = shard.Disk.get()->Snapshot;
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

      // Composed tail: segment sums are layer-relative, each segment rebased
      // onto the last element surviving below it
      int64_t skip = inMemoryOffset;
      uint64_t segmentBase = baseAggregate;
      for (const CTailSegment &seg: segments) {
        if (remaining <= 0)
          break;
        const uint64_t last = seg.Items[seg.Count - 1].Aggregate;
        if (skip >= static_cast<int64_t>(seg.Count)) {
          skip -= seg.Count;
          segmentBase += last;
          continue;
        }
        const int64_t needToRead = std::min<int64_t>(seg.Count - skip, remaining);
        CItem *items = static_cast<CItem*>(result.reserve(needToRead * sizeof(CItem)));
        memcpy(static_cast<void*>(items), seg.Items + skip, needToRead * sizeof(CItem));
        for (int64_t i = 0; i < needToRead; i++)
          items[i].Aggregate += segmentBase;
        remaining -= needToRead;
        segmentBase += last;
        skip = 0;
      }
    }

    return ok;
  }

protected:
  // One sealed layer, one batch: the newest descriptor of a key IS its
  // composed trim-and-append, the buffers are read where they lie
  void writeLayer(rocksdb::DB *db, size_t shardIndex, const CLayer<CKey> *layer, const BC::Proto::BlockHashTy &stamp) final {
    layer->buildScattered();

    std::vector<const CKey*> keys;
    std::vector<uint64_t> trims;
    std::vector<const CItem*> tails;
    std::vector<uint64_t> counts;
    keys.reserve(layer->used());
    trims.reserve(layer->used());
    tails.reserve(layer->used());
    counts.reserve(layer->used());
    for (size_t b = 0; b < KvScatterBuckets && !layer->Scattered.empty(); b++) {
      kvSortBucket(layer->Scattered, layer->Bounds, b);
      for (uint32_t k = b ? layer->Bounds[b - 1] : 0, end = layer->Bounds[b]; k < end; k++) {
        const CKvSortedRef<CKey> &ref = layer->Scattered[k];
        const CHeader *header = static_cast<const CHeader*>(static_cast<const CGenRecord*>(ref.Entry)->payload());
        keys.push_back(ref.Key);
        trims.push_back(header->BaseTrim);
        tails.push_back(header->Items);
        counts.push_back(header->Count);
      }
    }

    flushComposed(db, shardIndex, stamp, keys, trims, tails, counts);
  }


private:
  // Disk half of the flush: one composed trim-and-append per key, in key order
  void flushComposed(rocksdb::DB *db,
                     size_t shardIndex,
                     const BC::Proto::BlockHashTy &stamp,
                     const std::vector<const CKey*> &allKeys,
                     const std::vector<uint64_t> &allTrims,
                     const std::vector<const CItem*> &allTails,
                     const std::vector<uint64_t> &allTailCount) {
    std::vector<rocksdb::Slice> allKeySlices;
    allKeySlices.reserve(allKeys.size());
    for (size_t i = 0; i < allKeys.size(); i++)
      allKeySlices.emplace_back((const char*)allKeys[i], sizeof(CKey));

    // Metadata and the boundary chunks of trimmed keys are one consistent view.
    // The keys leave kvSortBucket in memcmp order - the comparator's own - so
    // sorted_input spares rocksdb its per-call sort of the whole batch
    std::vector<rocksdb::PinnableSlice> metadata(allKeySlices.size());
    std::vector<rocksdb::Status> metadataReadResult(allKeySlices.size());
    rocksdb::ReadOptions readOptions;
    readOptions.snapshot = db->GetSnapshot();
    if (!allKeySlices.empty())
      db->MultiGet(readOptions, db->DefaultColumnFamily(), allKeySlices.size(),
                   allKeySlices.data(), metadata.data(), metadataReadResult.data(),
                   /*sorted_input=*/true);

    // Reserved once: without it the batch string of a whole layer doubles
    // its way up through a few hundred MB of memcpy
    size_t batchBound = 64;
    for (size_t i = 0; i < allKeys.size(); i++) {
      batchBound += sizeof(CKey) + sizeof(CMetadata) + 16
                  + allTailCount[i] * sizeof(CItem)
                  + (allTailCount[i] / ChunkSize_ + 2) * (sizeof(CChunkKey) + 24);
    }
    rocksdb::WriteBatch batch(batchBound);
    this->putStamp(batch, stamp);

    // Absolute aggregates are built here and never written into a published
    // window: a query running concurrently keeps reading window-relative sums
    std::vector<uint8_t> scratch;

    for (size_t i = 0; i < allKeys.size(); i++) {
      const uint64_t headerBaseTrim = allTrims[i];
      const CItem *tailItems = allTails[i];
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
    LOG_F(1, "%s: shard %zu flushed %zu keys", this->Name_.c_str(), shardIndex, allKeys.size());
  }
  // The revision's layers for one key, oldest first, the last one - the live
  // era - cut at the revision's watermark. Each layer contributes its newest
  // descriptor: its trim eats the accumulated segments from the back first,
  // the leftover reaches past the layers into the disk. Returns the composed
  // tail length
  static uint64_t compose(const typename CKvView<CKey>::CShardView &shard,
                          const CKey &key,
                          size_t hash,
                          uint64_t &baseTrim,
                          std::vector<CTailSegment> &segments) {
    for (size_t j = 0; j < shard.Layers.size(); j++) {
      const uint32_t watermark = j + 1 == shard.Layers.size() ? shard.Watermark : UINT32_MAX;
      const CGenRecord *rec = shard.Layers[j].get()->find(key, hash, watermark);
      if (!rec)
        continue;
      const CHeader *header = static_cast<const CHeader*>(rec->payload());
      uint64_t trim = header->BaseTrim;
      while (trim && !segments.empty()) {
        CTailSegment &back = segments.back();
        const uint64_t cut = std::min<uint64_t>(trim, back.Count);
        back.Count -= cut;
        trim -= cut;
        if (back.Count == 0)
          segments.pop_back();
      }
      baseTrim += trim;
      if (header->Count)
        segments.push_back({header->Items, header->Count});
    }

    uint64_t tailCount = 0;
    for (const auto &seg: segments)
      tailCount += seg.Count;
    return tailCount;
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
