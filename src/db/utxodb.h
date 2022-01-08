// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/blockDataBase.h"
#include "common/hashmap/spmc/hashmap.h"
#include "common/mstorage/dynamicBuffer.h"
#include "db/common.h"
#include <rocksdb/db.h>

namespace BC {
namespace DB {

class UTXODb {
public:
  UTXODb() : Data_(1u << 20), MLog_(Data_) {}
  ~UTXODb();
  bool initialize(BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, BC::Common::BlockIndex **forConnect, IndexDbMap &forDisconnect);
  void add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy actionType, bool doFlush);
  // Thread safe, search utxo in cache; if it not found - look disk storage
  bool query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xmstream &data);
  // Thread safe, search utxo in cache only
  bool queryFast(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xmstream &data);
  void flush();

private:
//  struct RowData {
//    BC::Proto::BlockHashTy Hash;
//    size_t DataOffset = 0;
//    size_t DataSize = 0;
//  };
#pragma pack(push, 1)
  struct UnspentOutputKey {
    BC::Proto::BlockHashTy TxId;
    uint8_t Index;
  };

  struct UnspentOutputValue {
    UnspentOutputKey Key;
    BC::Script::UnspentOutputInfo Info;

    UnspentOutputKey extractKey() const { return Key; }

    static inline size_t getHash(const UnspentOutputKey &key, unsigned iter) {
      uint64_t lo = key.TxId.GetUint64(0);
      uint64_t hi = key.TxId.GetUint64(1);
      return ((lo >> iter) | (hi << (64-iter))) + (static_cast<uint64_t>(key.Index) << 63);
    }

    static uint32_t loadGenerationId(const MStorage::DynamicBuffer &storage, std::memory_order order) {
      return storage.loadGenerationId(order);
    }

    static UnspentOutputValue *locate(const MStorage::DynamicBuffer &storage, uint32_t allocationId) {
      return static_cast<UnspentOutputValue*>(storage.get(allocationId));
    }
  };
#pragma pack(pop)

private:
  std::unique_ptr<rocksdb::DB> Database_;
  uint32_t BatchId_;
  const BC::Common::BlockIndex *LastAdded_ = nullptr;

  MStorage::DynamicBuffer Data_;
  HashMap::SPMC::HashMap<UnspentOutputKey, UnspentOutputValue, MStorage::DynamicBuffer> MLog_;

//  xmstream Data_;
//  std::vector<RowData> Queue_;


//  xmstream Data_;
//  std::vector<TxData> Queue_;
//  tbb::concurrent_hash_map<BC::Proto::BlockHashTy, TxLink, TbbHash<256>> Cache_;
};

}
}
