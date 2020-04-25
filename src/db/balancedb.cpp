// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "balancedb.h"
#include "config4cpp/Configuration.h"
#include "BC/bc.h"
#include "BC/script.h"
#include "db/archive.h"
#include "db/storage.h"
#include "../loguru.hpp"
#include <rocksdb/merge_operator.h>
#include <set>


class BalanceMergeOperator: public rocksdb::AssociativeMergeOperator {
public:
  virtual bool Merge(const rocksdb::Slice&,
                     const rocksdb::Slice *existingValue,
                     const rocksdb::Slice &newValue,
                     std::string *result,
                     rocksdb::Logger*) const override {
    result->assign(newValue.data(), newValue.size());

    if (existingValue) {
      auto *resultPtr = const_cast<BC::DB::BalanceDb::Value*>(reinterpret_cast<const BC::DB::BalanceDb::Value*>(result->data()));
      auto *existingPtr = reinterpret_cast<const BC::DB::BalanceDb::Value*>(existingValue->data());
      resultPtr->Balance += existingPtr->Balance;
    }

    return true;
  }

  virtual const char* Name() const override {
    return "BC::DB::BalanceDb";
  }
};


namespace BC {
namespace DB {

BalanceDb::~BalanceDb()
{
  flush();
}

void BalanceDb::getConfiguration(config4cpp::Configuration *cfg)
{
  Enabled_ = cfg->lookupBoolean("balancedb", "enabled", false);
  if (!Enabled_)
    return;

  Cfg_.ShardsNum = static_cast<unsigned>(cfg->lookupInt("balancedb", "shardsNum", 1));
  Cfg_.StoreFullAddress = cfg->lookupBoolean("balancedb", "storeFullAddress", true);
}

bool BalanceDb::initialize(BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, BC::DB::Archive &archive, BC::Common::BlockIndex **forConnect, IndexDbMap &forDisconnect)
{
  if (!Enabled_)
    return true;

  BlockIndex_ = &blockIndex;
  BlockDb_ = &blockDb;
  TxDb_ = &archive.txdb();

  // Open all shards
  BC::Common::BlockIndex *bestIndex = blockIndex.best();
  std::set<BC::Proto::BlockHashTy> knownStamp;
  std::vector<BC::Common::BlockIndex*> forDisconnectLocal;
  std::vector<BC::Common::BlockIndex*> forConnectLocal;
  Databases_.resize(Cfg_.ShardsNum);
  ShardData_.resize(Cfg_.ShardsNum);
  for (unsigned i = 0; i < Cfg_.ShardsNum; i++) {
    auto shardPath = blockDb.dataDir() / "balancedb" / std::to_string(i);
    std::filesystem::create_directories(shardPath);

    rocksdb::DB *db;
    rocksdb::Options options;
    options.create_if_missing = true;
    options.merge_operator.reset(new BalanceMergeOperator);
    rocksdb::Status status = rocksdb::DB::Open(options, shardPath.u8string().c_str(), &db);
    if (!status.ok()) {
      LOG_F(ERROR, "Can't open or create balancedb database at %s", shardPath.u8string().c_str());
      return false;
    }

    Databases_[i].reset(db);

    Shard &shardData = ShardData_[i];

    if (i == 0) {
      // Check configuration
      rocksdb::Slice key("config");
      std::string value;
      if (db->Get(rocksdb::ReadOptions(), key, &value).ok() && value.size() >= Configuration::Size[0]) {
        const Configuration *storedCfg = reinterpret_cast<const Configuration*>(value.data());
        if (storedCfg->ShardsNum != Cfg_.ShardsNum ||
            storedCfg->StoreFullAddress != Cfg_.StoreFullAddress) {
          LOG_F(ERROR, "transaction db configuration not compatible with requested configuration, rerun with --reindex=txdb");
          return false;
        }
      } else {
        // DB not have configuration, write current to it
        rocksdb::Slice value(reinterpret_cast<char*>(&Cfg_), sizeof(Cfg_));
        db->Put(rocksdb::WriteOptions(), key, value);
      }
    }

    // Check stamp (last known block)
    std::string stampData;
    if (db->Get(rocksdb::ReadOptions(), rocksdb::Slice("stamp"), &stampData).ok()) {
      if (stampData.size() != sizeof(Stamp)) {
        LOG_F(ERROR, "txdb is corrupted: invalid stamp size (%s)", shardPath.u8string().c_str());
        return false;
      }

      const Stamp *stamp = reinterpret_cast<const Stamp*>(stampData.data());
      auto It = blockIndex.blockIndex().find(stamp->Hash);
      if (It == blockIndex.blockIndex().end()) {
        LOG_F(ERROR, "txdb is corrupted: stamp not exists in block index (%s)", shardPath.u8string().c_str());
        return false;
      }

      // Build connect and disconnect block set if need
      if (It->second != bestIndex && knownStamp.insert(stamp->Hash).second) {
        BC::Common::BlockIndex *first = rebaseChain(bestIndex, It->second, forDisconnectLocal);
        if (first && (!*forConnect || first->Height < (*forConnect)->Height))
          *forConnect = first;
        for (auto index: forDisconnectLocal)
          forDisconnect[index].Affected[DbTransactions] = true;
      }

      shardData.BatchId = stamp->BatchId + 1;
    } else {
      // database is empty, run full rescanning
      *forConnect = blockIndex.genesis();
      shardData.BatchId = 1;
    }
  }

  return true;
}

void BalanceDb::add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy actionType, bool doFlush)
{
  if (block.vtx.empty())
    return;

  unsigned coinbaseShard = 0;
  BC::Proto::AddressTy coinbaseAddress;
  coinbaseAddress.SetNull();

  // Process coinbase transaction
  const auto &coinbaseTx = block.vtx[0];
  if (coinbaseTx.txIn.size() == 1 && coinbaseTx.txOut.size() == 1) {
    BC::Proto::AddressTy address;
    int64_t delta = coinbaseTx.txOut[0].value;
    if (actionType == Disconnect)
      delta = -delta;

    if (BC::Script::decodeStandardOutput(coinbaseTx.txOut[0], address)) {
      unsigned shardNum = address.GetUint64(0) % Cfg_.ShardsNum;
      Shard &shardData = ShardData_[shardNum];

      decltype (shardData.Cache)::accessor accessor;
      if (shardData.Cache.find(accessor, address)) {
        accessor->second.Balance += delta;
      } else {
        Value value;
        value.BatchId = shardData.BatchId;
        value.Balance = delta;
        shardData.Cache.insert(std::make_pair(address, value));
      }

      coinbaseShard = shardNum;
      coinbaseAddress = address;
    }
  }

  for (size_t i = 1, ie = block.vtx.size(); i != ie; i++) {
    int64_t totalInput = 0;
    int64_t totalOutput = 0;
    auto tx = block.vtx[i];

    for (const auto &input: tx.txIn) {
      // Here we need find referenced output
      // Best way: use UTXO cache
      // Now UTXO cache not implemented yet, use txdb database (SLOW!)
      BC::DB::TxDb::QueryResult result;
      if (TxDb_->find(input.previousOutputHash, *BlockIndex_, *BlockDb_, result) && input.previousOutputIndex < result.Tx.txOut.size()) {
        BC::Proto::AddressTy address;
        int64_t delta = result.Tx.txOut[input.previousOutputIndex].value;
        if (actionType == Connect)
          delta = -delta;

        if (BC::Script::decodeStandardOutput(result.Tx.txOut[input.previousOutputIndex], address)) {
          unsigned shardNum = address.GetUint64(0) % Cfg_.ShardsNum;
          Shard &shardData = ShardData_[shardNum];

          decltype (shardData.Cache)::accessor accessor;
          if (shardData.Cache.find(accessor, address)) {
            accessor->second.Balance += delta;
          } else {
            Value value;
            value.BatchId = shardData.BatchId;
            value.Balance = delta;
            shardData.Cache.insert(std::make_pair(address, value));
          }

          totalInput += delta;
        }
      }
    }

    for (size_t j = 0, je = tx.txOut.size(); j != je; ++j) {
      const auto &output = tx.txOut[j];
      BC::Proto::AddressTy address;
      int64_t delta = output.value;
      if (actionType == Disconnect)
        delta = -delta;

      if (BC::Script::decodeStandardOutput(output, address)) {
        unsigned shardNum = address.GetUint64(0) % Cfg_.ShardsNum;
        Shard &shardData = ShardData_[shardNum];

        decltype (shardData.Cache)::accessor accessor;
        if (shardData.Cache.find(accessor, address)) {
          accessor->second.Balance += delta;
        } else {
          Value value;
          value.BatchId = shardData.BatchId;
          value.Balance = delta;
          shardData.Cache.insert(std::make_pair(address, value));
        }

        totalOutput += delta;
        if (i == 0 && j == 0) {
          coinbaseShard = shardNum;
          coinbaseAddress = address;
        }
      }
    }

    // Process transaction fee
    int64_t txFee = - (totalInput + totalOutput);
    if (txFee && !coinbaseAddress.IsNull()) {
      Shard &shard = ShardData_[coinbaseShard];
      decltype (shard.Cache)::accessor accessor;
      if (shard.Cache.find(accessor, coinbaseAddress))
        accessor->second.Balance += txFee;
    }
  }

  LastAdded_ = index;

  if (doFlush) {
    for (unsigned i = 0; i < Cfg_.ShardsNum; i++) {
      if (ShardData_[i].Cache.size() >= MinimalBatchSize)
        flush(i);
    }
  }
}

bool BalanceDb::find(const BC::Proto::AddressTy &address, int64_t *result)
{
  unsigned shardNum = address.GetUint64(0) % Cfg_.ShardsNum;
  Shard &shardData = ShardData_[shardNum];
  bool hasCachedValue = false;
  bool hasStoredValue = false;
  Value balance = {0, shardData.BatchId};

  if (Cfg_.StoreFullAddress) {
    {
      // Get cached delta
      decltype (shardData.Cache)::const_accessor accessor;
      if (shardData.Cache.find(accessor, address)) {
        balance = accessor->second;
        hasCachedValue = true;
      }
    }

    // Get stored balance
    rocksdb::DB *db = Databases_[shardNum].get();
    rocksdb::Slice key(reinterpret_cast<const char*>(address.begin()), sizeof(BC::Proto::AddressTy));
    std::string data;
    if (db->Get(rocksdb::ReadOptions(), key, &data).ok() && data.size() == sizeof(Value)) {
      const Value *value = reinterpret_cast<const Value*>(data.data());
      if (balance.BatchId - value->BatchId >= 0)
        balance.Balance += value->Balance;
      hasStoredValue = true;
    }
  } else {
    LOG_F(ERROR, "Not supported now");
    abort();
  }

  *result = balance.Balance;
  return hasCachedValue | hasStoredValue;
}

void BalanceDb::flush(unsigned shardNum)
{
  if (!LastAdded_)
    return;

  Shard &shardData = ShardData_[shardNum];
  std::vector<std::pair<BC::Proto::AddressTy, Value>> allValues(shardData.Cache.begin(), shardData.Cache.end());
  if (Cfg_.StoreFullAddress) {
    rocksdb::DB *db = Databases_[shardNum].get();
    rocksdb::WriteBatch batch;

    {
      Stamp stamp;
      stamp.Hash = LastAdded_->Header.GetHash();
      stamp.BatchId = shardData.BatchId;
      batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(&stamp), sizeof(stamp)));
    }

    for (const auto &addrDeltaPair: allValues) {
      auto key = rocksdb::Slice(reinterpret_cast<const char*>(addrDeltaPair.first.begin()), sizeof(BC::Proto::AddressTy));
      auto value = rocksdb::Slice(reinterpret_cast<const char*>(&addrDeltaPair.second), sizeof(Value));
      batch.Merge(key, value);
    }

    db->Write(rocksdb::WriteOptions(), &batch);
  } else {
    LOG_F(ERROR, "Not supported now");
    abort();
  }

  shardData.Cache.clear();
  shardData.BatchId++;
}

}
}
