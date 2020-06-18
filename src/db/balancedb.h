// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/blockDataBase.h"
#include "common/linearDataStorage.h"
#include "db/common.h"
#include <rocksdb/db.h>
#include <tbb/concurrent_hash_map.h>
#include <shared_mutex>

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
  static constexpr uint32_t TransactionRowSize = 64;

  struct QueryResult {
    int64_t Balance;
    int64_t TotalSent;
    int64_t TotalReceived;
    int64_t TransactionsNum;
  };

public:
  ~BalanceDb();
  bool enabled() { return Enabled_; }
  void getConfiguration(config4cpp::Configuration *cfg);
  bool initialize(BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, BC::DB::Archive &archive, BC::Common::BlockIndex **forConnect, IndexDbMap &forDisconnect);

  void add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy actionType, bool doFlush = false);
  bool find(const BC::Proto::AddressTy &address, QueryResult *result);
  void findTxidForAddr(const BC::Proto::AddressTy &address, uint64_t from, uint32_t count, std::vector<BC::Proto::BlockHashTy> &result);

  void flush(unsigned shardNum);
  void flush() {
    for (unsigned i = 0; i < Cfg_.ShardsNum; i++)
      flush(i);
  }

#pragma pack(push, 1)
  struct Value {
    int64_t TotalSent = 0;
    int64_t TotalReceived = 0;
    int64_t TransactionsNum = 0;
    uint32_t BatchId = 0;
  };

  struct CachedValue : public Value {
    xmstream TxData;
  };

  struct TxKey {
    BC::Proto::AddressTy Hash;
    uint64_t Row;

    TxKey() {}
    TxKey(const BC::Proto::AddressTy &address, uint64_t row) : Hash(address), Row(xhtobe(row)) {}
    void setRow(uint64_t row) { Row = xhtobe(row); }
  };

  struct TxValue {
    uint32_t offset;
    BC::Proto::TxHashTy hashes[TransactionRowSize];

    static constexpr uint32_t prefixSize() { return sizeof(TxValue) - sizeof(hashes); }
    static uint32_t dataSize(uint32_t txNum) { return txNum*sizeof(BC::Proto::TxHashTy) + prefixSize(); }
    static uint32_t txNum(size_t dataSize) { return static_cast<uint32_t>((dataSize - prefixSize()) / sizeof(BC::Proto::TxHashTy)); }
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
    std::unordered_map<BC::Proto::AddressTy, CachedValue> Cache;
    std::shared_mutex Mutex;
    uint32_t BatchId;
  };

  void modify(Shard &shard, const BC::Proto::AddressTy &address, int64_t sent, int64_t received, int64_t txNum, BC::Proto::TxHashTy *transactions);
  void mergeTx(rocksdb::WriteBatch &out, const char *address, uint64_t offset, const BC::Proto::TxHashTy *data, size_t txNum);
  size_t extractTx(std::vector<BC::Proto::TxHashTy> &out, const std::vector<std::string> &data, const std::vector<rocksdb::Status> &statuses, uint64_t from, uint32_t count, uint64_t limit);

private:
  Configuration Cfg_;
  bool Enabled_ = false;
  std::vector<std::unique_ptr<rocksdb::DB>> Databases_;

  std::unique_ptr<Shard[]> ShardData_ = nullptr;
  const BC::Common::BlockIndex *LastAdded_ = nullptr;

  BlockInMemoryIndex *BlockIndex_ = nullptr;
  BlockDatabase *BlockDb_ = nullptr;
  TxDb *TxDb_ = nullptr;
};

}
}
