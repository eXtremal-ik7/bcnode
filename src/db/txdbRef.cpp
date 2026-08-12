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
  result.Found = false;
  find(txid, [&result, &blockIndex, &blockDb](const void *data, size_t size) {
    // A record of another width belongs to a database built by another version:
    // report the transaction as missing rather than read somebody else's bytes
    if (size != sizeof(CLogData))
      return;
    CLogData logData;
    memcpy(&logData, data, sizeof(logData));
    result.Found = true;

    BC::Common::BlockIndex *index = blockIndex.indexByHeight(logData.Height);
    if (!index) {
      result.DataCorrupted = true;
      return;
    }

    readTransactionAt(index, logData.Index, logData.SerializedDataOffset, logData.SerializedDataSize,
                      blockDb, result);
  });

  return true;
}

bool TxDbRef::initializeImpl(config4cpp::Configuration*)
{
  return true;
}

void TxDbRef::connect(CBlockBatch batch, BlockInMemoryIndex&, BlockDatabase&)
{
  dbengine::CKvWriter<BC::Proto::TxHashTy> writer = liveWriter();
  for (const CBlockRef &ref: batch) {
    const BC::Proto::Block &block = *ref.Block;
    const BC::Proto::CBlockValidationData &validationData = *ref.ValidationData;
    assert(validationData.TxIds.size() == block.vtx.size());

    if (!BTC::txPositionsMatchStored(block, validationData.TxPositions, ref.Index->SerializedBlockSize)) {
      LOG_F(ERROR,
            "TxDbRef: transaction layout of block %s does not add up to its stored size",
            ref.Index->Header.GetHash().getHexLE().c_str());
      continue;
    }

    // A BIP30 repeat brings a coinbase this database already holds; see firstTx
    for (size_t i = firstTx(validationData), ie = block.vtx.size(); i != ie; i++) {
      CLogData data;
      data.Height = ref.Index->Height;
      data.Index = i;
      data.SerializedDataOffset = validationData.TxPositions[i].Offset;
      data.SerializedDataSize = validationData.TxPositions[i].Size;
      writer.putNew(validationData.TxIds[i], &data, sizeof(data));
    }
  }
  commit(writer, batch.back().Index->Header.GetHash());
}

void TxDbRef::disconnect(const BC::Common::BlockIndex *index,
                             const BC::Proto::Block &block,
                             const BC::Proto::CBlockLinkedOutputs&,
                             const BC::Proto::CBlockValidationData &validationData,
                             BlockInMemoryIndex&,
                             BlockDatabase&)
{
  dbengine::CKvWriter<BC::Proto::TxHashTy> writer = liveWriter();
  assert(validationData.TxIds.size() == block.vtx.size());
  for (size_t i = firstTx(validationData), ie = block.vtx.size(); i != ie; i++)
    writer.erase(validationData.TxIds[i]);
  commit(writer, index->Header.hashPrevBlock);
}

}
}
