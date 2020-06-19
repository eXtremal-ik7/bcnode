// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/blockDataBase.h"
#include "db/common.h"
#include <rocksdb/db.h>
#include <tbb/concurrent_hash_map.h>

namespace config4cpp {
class Configuration;
}

namespace BC {
namespace DB {

class Archive;

class TxDb {
public:
  static constexpr unsigned MinimalBatchSize = 8192;

  struct QueryResult {
    BC::Proto::Transaction Tx;
    BC::Proto::TxHashTy Block;
    uint32_t TxNum;
    bool Found = false;
    bool DataCorrupted = false;
  };

public:
  ~TxDb();
  bool enabled() { return Enabled_; }
  void getConfiguration(config4cpp::Configuration *cfg);
  bool initialize(BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, BC::DB::Archive &archive, BC::Common::BlockIndex **forConnect, IndexDbMap &forDisconnect);

  void add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy actionType, bool doFlush = false);
  void find(const BC::Proto::TxHashTy &hash, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, QueryResult &result);
  void find(const std::vector<BC::Proto::TxHashTy> &txHashes, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, std::vector<QueryResult> &result);

  void flush(unsigned shardNum);
  void flush() {
    for (unsigned i = 0; i < Cfg_.ShardsNum; i++)
      flush(i);
  }

private:
#pragma pack(push, 1)
  struct Configuration {
    uint32_t Version = 1;
    uint32_t ShardsNum = 1;
    uint32_t StoreFullTxHash = 1;
    uint32_t StoreFullTx = 0;

    static constexpr size_t Size[] = {16};
  };
#pragma pack(pop)

  struct TxData {
    BC::Proto::BlockHashTy Hash;
    size_t dataOffset = 0;
    size_t dataSize = 0;
  };

  struct TxLink {
    BC::Common::BlockIndex *block;
    uint32_t txIndex;
    uint32_t SerializedDataOffset;
    uint32_t SerializedDataSize;
  };

  struct Shard {
    xmstream Data;
    std::vector<TxData> Queue;
    tbb::concurrent_hash_map<BC::Proto::BlockHashTy, TxLink, TbbHash<256>> Cache;
  };

private:
  Configuration Cfg_;
  bool Enabled_ = false;
  std::vector<std::unique_ptr<rocksdb::DB>> Databases_;
  std::vector<Shard> ShardData_;
  const BC::Common::BlockIndex *LastAdded_ = nullptr;
};

}
}
