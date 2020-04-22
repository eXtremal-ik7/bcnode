// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"
#include "config4cpp/Configuration.h"
#include "BC/bc.h"
#include "../loguru.hpp"
#include <set>

namespace BC {
namespace DB {

TxDb::~TxDb()
{
  flush();
}

void TxDb::getConfiguration(config4cpp::Configuration *cfg)
{
  Enabled_ = cfg->lookupBoolean("txdb", "enabled", false);
  if (!Enabled_)
    return;

  Cfg_.ShardsNum = static_cast<unsigned>(cfg->lookupInt("txdb", "shardsNum", 1));
  Cfg_.StoreFullTxHash = cfg->lookupBoolean("txdb", "storeFullTxHash", true);
  Cfg_.StoreFullTx = cfg->lookupBoolean("txdb", "storeFullTx", false);
}

bool TxDb::initialize(BlockInMemoryIndex &blockIndex, std::filesystem::path &dataDir, BC::Common::BlockIndex **forConnect, IndexDbMap &forDisconnect)
{
  if (!Enabled_)
    return true;

  // Open all shards
  BC::Common::BlockIndex *bestIndex = blockIndex.best();
  std::set<BC::Proto::BlockHashTy> knownStamp;
  std::vector<BC::Common::BlockIndex*> forDisconnectLocal;
  std::vector<BC::Common::BlockIndex*> forConnectLocal;
  Databases_.resize(Cfg_.ShardsNum);
  ShardData_.resize(Cfg_.ShardsNum);
  for (unsigned i = 0; i < Cfg_.ShardsNum; i++) {
    auto shardPath = dataDir / "txdb" / std::to_string(i);
    std::filesystem::create_directories(shardPath);

    rocksdb::DB *db;
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::Status status = rocksdb::DB::Open(options, shardPath.u8string().c_str(), &db);
    if (!status.ok()) {
      LOG_F(ERROR, "Can't open or create txdb database at %s", shardPath.u8string().c_str());
      return false;
    }

    Databases_[i].reset(db);

    if (i == 0) {
      // Check configuration
      rocksdb::Slice key("config");
      std::string value;
      if (db->Get(rocksdb::ReadOptions(), key, &value).ok() && value.size() >= Configuration::Size[0]) {
        const Configuration *storedCfg = reinterpret_cast<const Configuration*>(value.data());
        if (storedCfg->ShardsNum != Cfg_.ShardsNum ||
            storedCfg->StoreFullTx != Cfg_.StoreFullTx) {
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
      if (stampData.size() != sizeof(BC::Proto::BlockHashTy)) {
        LOG_F(ERROR, "txdb is corrupted: invalid stamp size (%s)", shardPath.u8string().c_str());
        return false;
      }

      BC::Proto::BlockHashTy stamp;
      memcpy(stamp.begin(), stampData.data(), sizeof(BC::Proto::BlockHashTy));
      auto It = blockIndex.blockIndex().find(stamp);
      if (It == blockIndex.blockIndex().end()) {
        LOG_F(ERROR, "txdb is corrupted: stamp not exists in block index (%s)", shardPath.u8string().c_str());
        return false;
      }

      // Build connect and disconnect block set if need
      if (It->second != bestIndex && knownStamp.insert(stamp).second) {
        BC::Common::BlockIndex *first = rebaseChain(bestIndex, It->second, forDisconnectLocal);
        if (first && (!*forConnect || first->Height < (*forConnect)->Height))
          *forConnect = first;
        for (auto index: forDisconnectLocal)
          forDisconnect[index].Affected[DbTransactions] = true;
      }
    } else {
      // database is empty, run full rescanning
      *forConnect = blockIndex.genesis();
    }
  }

  return true;
}

void TxDb::add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy actionType, bool doFlush)
{
  BC::Proto::BlockHashTy blockHash = index->Header.GetHash();
  for (size_t i = 0, ie = block.vtx.size(); i != ie; i++) {
    auto tx = block.vtx[i];

    BC::Proto::BlockHashTy hash = tx.GetHash();
    unsigned shardNum = hash.GetUint64(0) % Cfg_.ShardsNum;
    Shard &shardData = ShardData_[shardNum];

    TxData txData;
    txData.Hash = tx.GetHash();
    if (actionType == Connect) {
      TxLink link;
      link.block = index;
      link.txIndex = static_cast<uint32_t>(i);
      link.SerializedDataOffset = tx.SerializedDataOffset;
      link.SerializedDataSize = tx.SerializedDataSize;
      shardData.Cache.insert(std::make_pair(txData.Hash, link));

      txData.dataOffset = shardData.Data.offsetOf();
      BC::serialize(shardData.Data, blockHash);
      BC::serialize(shardData.Data, static_cast<uint32_t>(i));
      if (Cfg_.StoreFullTx) {
        BC::serialize(shardData.Data, tx);
      } else {
        BC::serialize(shardData.Data, tx.SerializedDataOffset);
        BC::serialize(shardData.Data, tx.SerializedDataSize);
      }

      txData.dataSize = shardData.Data.offsetOf() - txData.dataOffset;
    } else {
      shardData.Cache.erase(txData.Hash);
    }

    shardData.Queue.push_back(txData);
  }

  LastAdded_ = index;

  if (doFlush) {
    for (unsigned i = 0; i < Cfg_.ShardsNum; i++) {
      if (ShardData_[i].Queue.size() >= MinimalBatchSize)
        flush(i);
    }
  }
}

bool TxDb::find(const BC::Proto::BlockHashTy &hash, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, QueryResult &result)
{
  unsigned shardNum = hash.GetUint64(0) % Cfg_.ShardsNum;
  Shard &shardData = ShardData_[shardNum];
  BC::Common::BlockIndex *index = nullptr;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
  if (Cfg_.StoreFullTxHash) {
    decltype (shardData.Cache)::const_accessor accessor;
    if (shardData.Cache.find(accessor, hash)) {
      index = accessor->second.block;
      result.Block = index->Header.GetHash();
      result.TxNum = accessor->second.txIndex;
      dataOffset = accessor->second.SerializedDataOffset;
      dataSize = accessor->second.SerializedDataSize;
    } else {
      rocksdb::DB *db = Databases_[shardNum].get();
      rocksdb::Slice key(reinterpret_cast<const char*>(hash.begin()), sizeof(BC::Proto::BlockHashTy));
      std::string value;
      if (db->Get(rocksdb::ReadOptions(), key, &value).ok()) {
        xmstream src(value.data(), value.size());
        BC::unserialize(src, result.Block);
        BC::unserialize(src, result.TxNum);
        if (Cfg_.StoreFullTx) {
          result.DataCorrupted = !BC::unserializeAndCheck(src, result.Tx);
          return !result.DataCorrupted;
        }

        BC::unserialize(src, dataOffset);
        BC::unserialize(src, dataSize);
        if (src.remaining() || src.eof()) {
          result.DataCorrupted = true;
          return false;
        }

        auto It = blockIndex.blockIndex().find(result.Block);
        if (It == blockIndex.blockIndex().end()) {
          result.DataCorrupted = true;
          return false;
        }

        index = It->second;
      } else {
        return false;
      }
    }
  } else {
    LOG_F(ERROR, "Not supported now");
    abort();
  }

  intrusive_ptr<const SerializedDataObject> serializedPtr(index->Serialized);
  if (serializedPtr.get()) {
    BC::Proto::Block *block = static_cast<BC::Proto::Block*>(serializedPtr.get()->unpackedData());
    if (result.TxNum <= block->vtx.size()) {
      result.Tx = block->vtx[result.TxNum];
      return true;
    } else {
      result.DataCorrupted = true;
      return false;
    }
  }

  xmstream stream(dataSize);
  stream.reserve(dataSize);
  stream.seekSet(0);
  if (blockDb.blockReader().read(index->FileNo, index->FileOffset + dataOffset + 8, stream.data(), dataSize)) {
    result.DataCorrupted = !unserializeAndCheck(stream, result.Tx);
    return !result.DataCorrupted;
  } else {
    result.DataCorrupted = true;
    return false;
  }
}

void TxDb::flush(unsigned shardNum)
{
  if (!LastAdded_)
    return;

  Shard &shardData = ShardData_[shardNum];
  const char *txData = shardData.Data.data<const char>();
  if (Cfg_.StoreFullTxHash) {
    rocksdb::DB *db = Databases_[shardNum].get();
    rocksdb::WriteBatch batch;
    BC::Proto::BlockHashTy stamp = LastAdded_->Header.GetHash();
    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(stamp.begin()), sizeof(BC::Proto::BlockHashTy)));
    for (const auto &tx: shardData.Queue) {
      auto key = rocksdb::Slice(reinterpret_cast<const char*>(tx.Hash.begin()), sizeof(BC::Proto::BlockHashTy));
      if (tx.dataSize) {
        // Connecting block
        auto value = rocksdb::Slice(txData + tx.dataOffset, tx.dataSize);
        batch.Put(key, value);
      } else {
        // Disconnecting block
        batch.Delete(key);
      }
    }

    db->Write(rocksdb::WriteOptions(), &batch);
  } else {
    LOG_F(ERROR, "Not supported now");
    abort();
  }

  shardData.Queue.clear();
  shardData.Cache.clear();
}

}
}
