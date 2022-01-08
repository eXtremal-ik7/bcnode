// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "utxodb.h"
#include "common/smallStream.h"
#include <set>

namespace BC {
namespace DB {

UTXODb::~UTXODb()
{
  flush();
}

bool UTXODb::initialize(BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, BC::Common::BlockIndex **forConnect, IndexDbMap &forDisconnect)
{
  auto shardPath = blockDb.dataDir() / "utxo";
  std::filesystem::create_directories(shardPath);

  BC::Common::BlockIndex *bestIndex = blockIndex.best();
  std::set<BC::Proto::BlockHashTy> knownStamp;
  std::vector<BC::Common::BlockIndex*> forDisconnectLocal;

  rocksdb::DB *db;
  rocksdb::Options options;
  options.create_if_missing = true;
  rocksdb::Status status = rocksdb::DB::Open(options, shardPath.u8string().c_str(), &db);
  if (!status.ok()) {
    LOG_F(ERROR, "Can't open of create utxodb database at %s", shardPath.u8string().c_str());
    return false;
  }

  Database_.reset(db);

  // Check stamp (last known block)
  std::string stampData;
  if (db->Get(rocksdb::ReadOptions(), rocksdb::Slice("stamp"), &stampData).ok()) {
    if (stampData.size() != sizeof(BC::Proto::BlockHashTy)) {
      LOG_F(ERROR, "utxodb is corrupted: invalid stamp size (%s)", shardPath.u8string().c_str());
      return false;
    }

    BC::Proto::BlockHashTy stamp;
    memcpy(stamp.begin(), stampData.data(), sizeof(BC::Proto::BlockHashTy));
    auto It = blockIndex.blockIndex().find(stamp);
    if (It == blockIndex.blockIndex().end()) {
      LOG_F(ERROR, "utxodb is corrupted: stamp not exists in block index (%s)", shardPath.u8string().c_str());
      return false;
    }

    // Build connect and disconnect block set if need
    if (It->second != bestIndex && knownStamp.insert(stamp).second) {
      BC::Common::BlockIndex *first = rebaseChain(bestIndex, It->second, forDisconnectLocal);
      if (first && (!*forConnect || first->Height < (*forConnect)->Height))
        *forConnect = first;
      for (auto index: forDisconnectLocal)
        forDisconnect[index].Affected[DbUTXO] = true;
    }
  } else {
    // database is empty, run full rescanning
    *forConnect = blockIndex.genesis();
  }

  return true;
}

void UTXODb::add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy actionType, bool doFlush)
{
  SmallStream<1024> serialized;
  for (const auto &tx: block.vtx) {
    for (size_t i = 0, ie = tx.txIn.size(); i != ie; ++i) {
      const auto &txIn = tx.txIn[i];

      if (actionType == Connect) {
        serialized.reset();
        serialized.write(txIn.previousOutputHash.begin(), sizeof(txIn.previousOutputHash));
        serialized.write<uint8_t>(i);
        serialized.reserve<BC::Script::UnspentOutputInfo>(1)->Type = BTC::Script::UnspentOutputInfo::EInvalid;
      } else if (actionType == Disconnect) {
        // SLOW!
        // Load transaction data from disk
      }

      MStorage::DynamicBuffer::Pointer pointer = Data_.alloc(serialized.sizeOf());
      memcpy(pointer.Data, serialized.data(), serialized.sizeOf());
      MLog_.insert(serialized.data<const UnspentOutputValue>(), pointer.AllocId);
    }

    BC::Proto::BlockHashTy txid = tx.getTxId();
    for (size_t i = 0, ie = tx.txOut.size(); i != ie; ++i) {
      const auto &txOut = tx.txOut[i];

      serialized.reset();
      serialized.write(txid.begin(), sizeof(txid));
      serialized.write<uint8_t>(i);

      if (actionType == Connect)
        BTC::Script::parseTransactionOutput(txOut, serialized);
      else if (actionType == Disconnect)
        serialized.reserve<BC::Script::UnspentOutputInfo>(1)->Type = BTC::Script::UnspentOutputInfo::EInvalid;

      MStorage::DynamicBuffer::Pointer pointer = Data_.alloc(serialized.sizeOf());
      memcpy(pointer.Data, serialized.data(), serialized.sizeOf());
      MLog_.insert(serialized.data<const UnspentOutputValue>(), pointer.AllocId);
    }
  }
}

bool UTXODb::query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xmstream &data)
{
  BC::Proto::BlockHashTy key(txid);
  *reinterpret_cast<uint64_t*>(key.begin()) ^= txoutIdx;

}

bool UTXODb::queryFast(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xmstream &data)
{
  return false;
}

void UTXODb::flush()
{
  if (!LastAdded_)
    return;

  rocksdb::WriteBatch batch;
  BC::Proto::BlockHashTy stamp = LastAdded_->Header.GetHash();
  batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(stamp.begin()), sizeof(BC::Proto::BlockHashTy)));
//  for (const auto &tx: Queue_) {

//  }

  Database_->Write(rocksdb::WriteOptions(), &batch);
  Data_.updateGenerationId(BatchId_);
}

}
}
