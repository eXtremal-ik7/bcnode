// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// The plain key-value fold over the store: the newest record of a key wins, a
// lookup stops at the first layer that has it. Merge and array replace this
// one half with their own.

#include "dbengine/kvstore.h"

namespace dbengine {

template<typename CKey>
class CKvBase : public CKvStore<CKey> {
public:
  CKvBase(const std::string &name) : CKvStore<CKey>(name) {}

  // The flusher dispatches the folds implemented at this level: stop it
  // while the dispatch is still valid (~CKvStore's shutdown is a no-op then)
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
  void writeLayer(rocksdb::DB *db, size_t shardIndex, const CLayer<CKey> *layer, const BaseBlob<256> &stamp) final {
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
