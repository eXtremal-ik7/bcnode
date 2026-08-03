// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdbRef.h"
#include "common/smallStream.h"

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

bool TxDbRef::initializeImpl(config4cpp::Configuration*, BC::DB::Storage&)
{
  return true;
}

void TxDbRef::connectImpl(const BC::Common::BlockIndex *index,
                          const BC::Proto::Block &block,
                          const BC::Proto::CBlockLinkedOutputs&,
                          const BC::Proto::CBlockValidationData &validationData,
                          BlockInMemoryIndex&,
                          BlockDatabase&)
{
  const auto blockId = index->Header.GetHash();
  std::vector<BTC::CTxPosition> positions;
  if (!BTC::enumerateTransactions(block, index->SerializedBlockSize, positions)) {
    LOG_F(ERROR,
          "TxDbRef: transaction layout of block %s does not add up to its stored size",
          blockId.getHexLE().c_str());
    return;
  }

  // A BIP30 repeat brings a coinbase this database already holds; see firstTx
  for (size_t i = firstTx(validationData), ie = block.vtx.size(); i != ie; i++) {
    CLogData data;
    data.Hash = blockId;
    data.Index = i;
    data.SerializedDataOffset = positions[i].Offset;
    data.SerializedDataSize = positions[i].Size;
    this->add(blockId, block.vtx[i].getTxId(), &data, sizeof(data));
  }
}

void TxDbRef::disconnectImpl(const BC::Common::BlockIndex *index,
                             const BC::Proto::Block &block,
                             const BC::Proto::CBlockLinkedOutputs&,
                             const BC::Proto::CBlockValidationData &validationData,
                             BlockInMemoryIndex&,
                             BlockDatabase&)
{
  const auto blockId = index->Header.GetHash();
  for (size_t i = firstTx(validationData), ie = block.vtx.size(); i != ie; i++)
    this->remove(blockId, block.vtx[i].getTxId());
}

}
}
