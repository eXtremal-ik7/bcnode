// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "utxodb.h"
#include "common/smallStream.h"

namespace BC {
namespace DB {

bool UTXODb::query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xmstream &data) const
{
  // TODO: search in cache

  CUnspentOutputKey key;
  key.Tx = txid;
  key.Index = txoutIdx;
  data.reset();
  return this->find(key, [&data](const void *d, size_t s) {
    data.write(d, s);
  });
}

bool UTXODb::queryFast(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xmstream &data)
{
  return false;
}

bool UTXODb::initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage)
{
  return true;
}

void UTXODb::connectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb)
{ 
  SmallStream<1024> serialized;
  const auto blockId = index->Header.GetHash();

  CUnspentOutputKey key;
  // Coinbase
  {
    // txin in coinbase can't spent anything
    const auto &coinbaseTx = block.vtx[0];
    key.Tx = coinbaseTx.getTxId();
    for (size_t i = 0; i < coinbaseTx.txOut.size(); i++) {
      const auto &txOut = coinbaseTx.txOut[i];
      key.Index = i;

      serialized.reset();
      BTC::Script::parseTransactionOutput(txOut, serialized);
      this->add(blockId, key, serialized.data(), serialized.sizeOf());
    }
  }

  // Other transactions
  for (size_t i = 1; i < block.vtx.size(); i++) {
    const auto &tx = block.vtx[i];

    for (size_t j = 0; j < tx.txIn.size(); j++) {
      const auto &txIn = tx.txIn[j];
      key.Tx = txIn.previousOutputHash;
      key.Index = txIn.previousOutputIndex;
      this->remove(blockId, key);
    }

    key.Tx = tx.getTxId();
    for (size_t j = 0; j < tx.txOut.size(); j++) {
      const auto &txOut = tx.txOut[j];
      key.Index = j;

      serialized.reset();
      BTC::Script::parseTransactionOutput(txOut, serialized);
      this->add(blockId, key, serialized.data(), serialized.sizeOf());
    }
  }
}

void UTXODb::disconnectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb)
{
  SmallStream<1024> serialized;
  const auto blockId = index->Header.GetHash();

  CUnspentOutputKey key;
  // Coinbase
  {
    // txin in coinbase can't spent anything
    const auto &coinbaseTx = block.vtx[0];
    key.Tx = coinbaseTx.getTxId();
    for (size_t i = 0; i < coinbaseTx.txOut.size(); i++) {
      key.Index = i;
      this->remove(blockId, key);
    }
  }

  // Other transactions
  for (size_t i = 1; i < block.vtx.size(); i++) {
    const auto &tx = block.vtx[i];

    for (size_t j = 0; j < tx.txIn.size(); j++) {
      const auto &txIn = tx.txIn[j];
      // TODO: use block index for transaction search
      serialized.reset();
      if (!TxDb_->searchUnspentOutput(txIn.previousOutputHash, txIn.previousOutputIndex, blockIndex, blockDb, serialized)) {
        LOG_F(ERROR,
              "can't disconnect block [%u]%s unspent output %s:%u not found",
              index->Height,
              index->Header.GetHash().GetHex().c_str(),
              txIn.previousOutputHash.GetHex().c_str(),
              txIn.previousOutputIndex);
        exit(1);
      }

      this->add(blockId, key, serialized.data(), serialized.sizeOf());
    }

    key.Tx = tx.getTxId();
    for (size_t j = 0; j < tx.txOut.size(); j++) {
      key.Index = j;
      this->remove(blockId, key);
    }
  }
}

bool searchUnspentOutput(const BC::Proto::TxHashTy &tx,
                         uint32_t index,
                         const BC::Proto::Block &block,
                         std::unordered_map<BC::Proto::TxHashTy, size_t> &localTxMap,
                         BlockInMemoryIndex &blockIndex,
                         BlockDatabase &blockDb,
                         UTXODb *db,
                         ITransactionDb *txdb,
                         xmstream &result)
{
  result.reset();

  // 1. Search in UTXO cache

  // 2. Search in current block
  {
    auto It = localTxMap.find(tx);
    if (It != localTxMap.end()) {
      const BC::Proto::Transaction &localTx = block.vtx[It->second];
      if (index >= localTx.txOut.size())
        return false;

      const BC::Proto::TxOut &txOut = localTx.txOut[index];

      // Parse transaction output and return
      BC::Script::parseTransactionOutput(txOut, result);
      return true;
    }
  }

  // 3. Search in whole transaction base
  return txdb->searchUnspentOutput(tx, index, blockIndex, blockDb, result);
}

}
}
