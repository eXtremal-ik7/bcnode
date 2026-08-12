// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"
#include "common/smallStream.h"
#include "config4cpp/Configuration.h"
#include "BC/bc.h"
#include "../loguru.hpp"

namespace BC {
namespace DB {

bool TxDb::queryTransaction(const BC::Proto::TxHashTy &txid,
                            BlockInMemoryIndex &blockIndex,
                            BlockDatabase&,
                            CQueryTransactionResult &result)
{
  result.DataCorrupted = false;
  result.Found = this->find(txid, [&result, &blockIndex](const void *data, size_t size) {
    CLogData logData;
    memcpy(&logData, data, sizeof(logData));
    result.TxNum = logData.Index;

    // The row keeps a height, and the reply a hash: the header is in memory anyway
    BC::Common::BlockIndex *index = blockIndex.indexByHeight(logData.Height);
    if (index)
      result.Block = index->Header.GetHash();
    else
      result.DataCorrupted = true;

    xmstream s(static_cast<uint8_t*>(const_cast<void*>(data)) + sizeof(CLogData), size - sizeof(CLogData));
    result.DataCorrupted |= !BC::unserializeAndCheck(s, result.Tx);
    result.DataCorrupted |= !BC::unserializeAndCheck(s, result.LinkedOutputs);
  });

  return true;
}

bool TxDb::initializeImpl(config4cpp::Configuration*)
{
  return true;
}

void TxDb::connect(CBlockBatch batch, BlockInMemoryIndex&, BlockDatabase&)
{
  dbengine::CKvWriter<BC::Proto::TxHashTy> writer = liveWriter();
  SmallStream<4096> stream;
  for (const CBlockRef &ref: batch) {
    const BC::Proto::Block &block = *ref.Block;
    const BC::Proto::CBlockValidationData &validationData = *ref.ValidationData;
    assert(validationData.TxIds.size() == block.vtx.size());

    // A BIP30 repeat carries a coinbase this database already holds, byte for byte
    // the same one: leaving the twin's record alone keeps the key write-once, and a
    // query answers with both inclusions from the chain params
    for (size_t i = firstTx(validationData), ie = block.vtx.size(); i != ie; i++) {
      const auto &tx = block.vtx[i];

      stream.reset();
      CLogData *data = stream.reserve<CLogData>(1);
      data->Height = ref.Index->Height;
      data->Index = i;
      BC::serialize(stream, tx);
      BC::serialize(stream, ref.LinkedOutputs->Tx[i]);
      writer.putNew(validationData.TxIds[i], stream.data(), stream.sizeOf());
    }
  }
  commit(writer, batch.back().Index->Header.GetHash());
}

void TxDb::disconnect(const BC::Common::BlockIndex *index,
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
