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
class TxDb;

class BalanceDb {
public:
  static constexpr unsigned MinimalBatchSize = 8192;

  struct QueryResult {
    int64_t Balance;
    int64_t TotalSent;
    int64_t TotalReceived;
    int32_t TransactionsNum;
  };

public:
  ~BalanceDb();
  bool enabled() { return Enabled_; }
  void getConfiguration(config4cpp::Configuration *cfg);
  bool initialize(BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, BC::DB::Archive &archive, BC::Common::BlockIndex **forConnect, IndexDbMap &forDisconnect);

  void add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy actionType, bool doFlush = false);
  bool find(const BC::Proto::AddressTy &address, QueryResult *result);

  void flush(unsigned shardNum);
  void flush() {
    for (unsigned i = 0; i < Cfg_.ShardsNum; i++)
      flush(i);
  }

#pragma pack(push, 1)
  struct Value {
    int64_t Balance = 0;
    int64_t TotalSent = 0;
    int64_t TotalReceived = 0;
    int32_t TransactionsNum = 0;
    int32_t BatchId;
  };
#pragma pack(pop)

private:
#pragma pack(push, 1)
  struct Configuration {
    uint32_t Version = 1;
    uint32_t ShardsNum = 1;
    uint32_t StoreFullAddress = 1;

    static constexpr size_t Size[] = {12};
  };

  struct Stamp {
    BC::Proto::BlockHashTy Hash;
    uint32_t BatchId;
  };
#pragma pack(pop)


  struct Shard {
    tbb::concurrent_hash_map<BC::Proto::AddressTy, Value, TbbHash<160>> Cache;
    int32_t BatchId;
  };

private:
  Configuration Cfg_;
  bool Enabled_ = false;
  std::vector<std::unique_ptr<rocksdb::DB>> Databases_;
  std::vector<Shard> ShardData_;
  const BC::Common::BlockIndex *LastAdded_ = nullptr;

  BlockInMemoryIndex *BlockIndex_ = nullptr;
  BlockDatabase *BlockDb_ = nullptr;
  TxDb *TxDb_ = nullptr;
};

}
}
