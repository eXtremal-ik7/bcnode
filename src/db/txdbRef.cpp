// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdbRef.h"

namespace BC {
namespace DB {

bool TxDbRef::queryTransaction(const BC::Proto::TxHashTy &txid,
                               BlockInMemoryIndex &blockIndex,
                               BlockDatabase &blockDb,
                               CQueryTransactionResult &result)
{
  result.Found = find(txid, [&result, &blockIndex, &blockDb](const void *data, size_t size) {
    CLogData *logData = (CLogData*)data;
    result.Block = logData->Hash;
    result.TxNum = logData->Index;

    auto It = blockIndex.blockIndex().find(logData->Hash);
    if (It == blockIndex.blockIndex().end())
      return;
    BC::Common::BlockIndex *index = It->second;
    intrusive_ptr<const SerializedDataObject> serializedPtr(index->Serialized);
    if (serializedPtr.get()) {
      BC::Proto::Block *block = static_cast<BC::Proto::Block*>(serializedPtr.get()->unpackedData());
      result.Tx = block->vtx[logData->Index];
      result.DataCorrupted = false;
      return;
    }

    xmstream txData;
    BC::Proto::Transaction txFromDisk;
    txData.reserve(logData->SerializedDataSize);
    if (blockDb.blockReader().read(index->FileNo, index->FileOffset + logData->SerializedDataOffset + 8, txData.data(), logData->SerializedDataSize)) {
      txData.seekSet(0);
      if (unserializeAndCheck(txData, result.Tx)) {
        result.DataCorrupted = false;
      }
    }
  });

  return true;
}

bool TxDbRef::searchUnspentOutput(const BC::Proto::TxHashTy &tx,
                                  uint32_t outputIndex,
                                  BlockInMemoryIndex &blockIndex,
                                  BlockDatabase &blockDb,
                                  xmstream &result)
{
  bool found = find(tx, [&blockIndex, &blockDb, &result, outputIndex](const void *data, size_t size) {
    // Load transaction
    const CLogData *logData = static_cast<const CLogData*>(data);

    auto It = blockIndex.blockIndex().find(logData->Hash);
    if (It == blockIndex.blockIndex().end())
      return;
    BC::Common::BlockIndex *index = It->second;

    intrusive_ptr<const SerializedDataObject> serializedPtr(index->Serialized);
    if (serializedPtr.get()) {
      BC::Proto::Block *block = static_cast<BC::Proto::Block*>(serializedPtr.get()->unpackedData());
      if (logData->Index < block->vtx.size()) {
        const auto &tx = block->vtx[logData->Index];
        if (outputIndex < tx.txOut.size()) {
          const auto &txOut = tx.txOut[outputIndex];
          BC::Script::parseTransactionOutput(txOut, result);
        }
      }

      return;
    }

    xmstream txData;
    BC::Proto::Transaction txFromDisk;
    txData.reserve(logData->SerializedDataSize);
    if (blockDb.blockReader().read(index->FileNo, index->FileOffset + logData->SerializedDataOffset + 8, txData.data(), logData->SerializedDataSize)) {
      txData.seekSet(0);
      if (unserializeAndCheck(txData, txFromDisk)) {
        if (outputIndex < txFromDisk.txOut.size()) {
          const auto &txOut = txFromDisk.txOut[outputIndex];
          BC::Script::parseTransactionOutput(txOut, result);
        }
      }
    }
  });

  return found && result.sizeOf();
}

bool TxDbRef::initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage)
{
  return true;
}

void TxDbRef::connectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb)
{
  const auto blockId = index->Header.GetHash();
  for (size_t i = 0, ie = block.vtx.size(); i != ie; i++) {
    auto tx = block.vtx[i];
    BC::Proto::BlockHashTy hash = tx.getTxId();
    CLogData data;
    data.Hash = blockId;
    data.Index = i;
    data.SerializedDataOffset = tx.SerializedDataOffset;
    data.SerializedDataSize = tx.SerializedDataSize;
    this->add(blockId, hash, &data, sizeof(data));
  }
}

void TxDbRef::disconnectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb)
{
  const auto blockId = index->Header.GetHash();
  for (size_t i = 0, ie = block.vtx.size(); i != ie; i++)
    this->remove(blockId, block.vtx[i].getTxId());
}


}
}
