// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdbRef.h"
#include "common/smallStream.h"
#include "config4cpp/Configuration.h"

namespace BC {
namespace DB {

bool TxDbRef::queryTransaction(const BC::Proto::TxHashTy &txid,
                               BlockInMemoryIndex &blockIndex,
                               BlockDatabase &blockDb,
                               CQueryTransactionResult &result)
{
  result.DataCorrupted = false;
  result.Found = find(txid, [&result, &blockIndex, &blockDb](const void *data, size_t) {
    CLogData *logData = (CLogData*)data;
    result.Block = logData->Hash;
    result.TxNum = logData->Index;

    auto It = blockIndex.blockIndex().find(logData->Hash);
    if (It == blockIndex.blockIndex().end())
      return;
    BC::Common::BlockIndex *index = It->second;
    intrusive_ptr<BC::Common::CIndexCacheObject> serializedPtr(index->Serialized);
    if (serializedPtr.get()) {
      BC::Proto::Block *block = serializedPtr.get()->block();
      result.Tx = block->vtx[logData->Index];
      result.LinkedOutputs = serializedPtr.get()->linkedOutputs().Tx[logData->Index];
      return;
    }

    if (!index->indexStored()) {
      result.DataCorrupted = true;
      return;
    }

    SmallStream<16384> stream;
    BC::Proto::Transaction txFromDisk;
    BC::Proto::CBlockLinkedOutputs linkedOutputs;
    if (blockDb.blockReader().read(index->FileNo, index->FileOffset + logData->SerializedDataOffset + 8, stream.reserve(logData->SerializedDataSize), logData->SerializedDataSize)) {
      stream.seekSet(0);
      if (!unserializeAndCheck(stream, result.Tx)) {
        result.DataCorrupted = true;
        return;
      }
    }

    // Load linked outputs
    stream.reset();
    if (!blockDb.linkedOutputsReader().read(index->LinkedOutputsFileNo,
                                            index->LinkedOutputsFileOffset + 4,
                                            stream.reserve(index->LinkedOutputsSerializedSize),
                                            index->LinkedOutputsSerializedSize)) {
      result.DataCorrupted = true;
      return;
    }

    stream.seekSet(0);
    if (!BTC::unserializeAndCheck(stream, linkedOutputs)) {
      result.DataCorrupted = true;
      return;
    }

    result.LinkedOutputs = linkedOutputs.Tx[logData->Index];
  });

  return true;
}

bool TxDbRef::initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage&)
{
  // A write-once key never needs its own previous value, so a threshold flush
  // has nothing to hand back to the connect path: it goes to a background
  // thread. The escape hatch exists for A/B runs on the bench stand
  if (cfg->lookupBoolean(name().c_str(), "asyncFlush", true))
    enableAsyncFlush();

  return true;
}

void TxDbRef::connectImpl(CBlockBatch batch, BlockInMemoryIndex&, BlockDatabase&)
{
  std::vector<BTC::CTxPosition> positions;
  for (const CBlockRef &ref: batch) {
    const BC::Proto::Block &block = *ref.Block;
    const BC::Proto::CBlockValidationData &validationData = *ref.ValidationData;
    assert(validationData.TxIds.size() == block.vtx.size());
    const auto blockId = ref.Index->Header.GetHash();

    positions.clear();
    if (!BTC::enumerateTransactions(block, ref.Index->SerializedBlockSize, positions)) {
      LOG_F(ERROR,
            "TxDbRef: transaction layout of block %s does not add up to its stored size",
            blockId.getHexLE().c_str());
      continue;
    }

    // A BIP30 repeat brings a coinbase this database already holds; see firstTx
    for (size_t i = firstTx(validationData), ie = block.vtx.size(); i != ie; i++) {
      CLogData data;
      data.Hash = blockId;
      data.Index = i;
      data.SerializedDataOffset = positions[i].Offset;
      data.SerializedDataSize = positions[i].Size;
      this->putNew(validationData.TxIds[i], &data, sizeof(data));
    }
  }
}

void TxDbRef::disconnectImpl(const BC::Common::BlockIndex*,
                             const BC::Proto::Block &block,
                             const BC::Proto::CBlockLinkedOutputs&,
                             const BC::Proto::CBlockValidationData &validationData,
                             BlockInMemoryIndex&,
                             BlockDatabase&)
{
  assert(validationData.TxIds.size() == block.vtx.size());
  for (size_t i = firstTx(validationData), ie = block.vtx.size(); i != ie; i++)
    this->erase(validationData.TxIds[i]);
}

}
}
