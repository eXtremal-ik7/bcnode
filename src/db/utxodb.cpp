// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "utxodb.h"
#include "common/smallStream.h"

namespace BC {
namespace DB {

bool UTXODb::query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xvector<uint8_t> &result) const
{
  // TODO: search in cache

  CUnspentOutputKey key;
  key.Tx = txid;
  key.Index = txoutIdx;
  return this->find(key, [&result](const void *d, size_t s) {
    result.resize(s);
    memcpy(result.begin(), d, s);
  });
}

bool UTXODb::queryCache(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xvector<uint8_t> &result) const
{
  // TODO: search in cache
  // Now cache not implemented, search in entire database
  CUnspentOutputKey key;
  key.Tx = txid;
  key.Index = txoutIdx;
  return this->find(key, [&result](const void *d, size_t s) {
    result.resize(s);
    memcpy(result.begin(), d, s);
  });
}

bool UTXODb::initializeImpl(config4cpp::Configuration* , BC::DB::Storage&)
{
  return true;
}

void UTXODb::connectImpl(const BC::Common::BlockIndex *index,
                         const BC::Proto::Block &block,
                         const BC::Proto::CBlockLinkedOutputs&,
                         BlockInMemoryIndex&,
                         BlockDatabase&)
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
      const BC::Script::UnspentOutputInfo *info = serialized.data<const BC::Script::UnspentOutputInfo>();
      if (info->Type != BC::Script::UnspentOutputInfo::EOpReturn)
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
      const BC::Script::UnspentOutputInfo *info = serialized.data<const BC::Script::UnspentOutputInfo>();
      if (info->Type != BC::Script::UnspentOutputInfo::EOpReturn)
        this->add(blockId, key, serialized.data(), serialized.sizeOf());
    }
  }
}

void UTXODb::disconnectImpl(const BC::Common::BlockIndex *index,
                            const BC::Proto::Block &block,
                            const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            BlockInMemoryIndex&,
                            BlockDatabase&)
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
      serialized.reset();
      BTC::Script::parseTransactionOutput(coinbaseTx.txOut[i], serialized);
      const BC::Script::UnspentOutputInfo *info = serialized.data<const BC::Script::UnspentOutputInfo>();
      if (info->Type != BC::Script::UnspentOutputInfo::EOpReturn) {
        key.Index = i;
        this->remove(blockId, key);
      }
    }
  }

  // Other transactions
  assert(linkedOutputs.Tx.size() == block.vtx.size());

  for (size_t i = 1; i < block.vtx.size(); i++) {
    const auto &tx = block.vtx[i];
    const auto &linkedTx = linkedOutputs.Tx[i];
    assert(linkedTx.TxIn.size() == tx.txIn.size());

    for (size_t j = 0; j < tx.txIn.size(); j++) {
      const auto &txIn = tx.txIn[j];
      const auto &linkedTxin = linkedTx.TxIn[j];

      assert(linkedTxin.size() >= sizeof(BC::Script::UnspentOutputInfo));

      key.Tx = txIn.previousOutputHash;
      key.Index = txIn.previousOutputIndex;
      this->add(blockId, key, linkedTxin.data(), linkedTxin.size());
    }

    key.Tx = tx.getTxId();
    for (size_t j = 0; j < tx.txOut.size(); j++) {
      serialized.reset();
      BTC::Script::parseTransactionOutput(tx.txOut[j], serialized);
      const BC::Script::UnspentOutputInfo *info = serialized.data<const BC::Script::UnspentOutputInfo>();
      if (info->Type != BC::Script::UnspentOutputInfo::EOpReturn) {
        key.Index = j;
        this->remove(blockId, key);
      }
    }
  }
}

}
}
