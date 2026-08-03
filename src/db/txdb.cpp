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
                            BlockInMemoryIndex&,
                            BlockDatabase&,
                            CQueryTransactionResult &result)
{
  result.DataCorrupted = false;
  result.Found = this->find(txid, [&result](const void *data, size_t size) {
    CLogData *p = (CLogData*)data;
    result.Block = p->Hash;
    result.TxNum = p->Index;
    xmstream s(p+1, size-sizeof(CLogData));
    result.DataCorrupted |= !BC::unserializeAndCheck(s, result.Tx);
    result.DataCorrupted |= !BC::unserializeAndCheck(s, result.LinkedOutputs);
  });

  return true;
}

bool TxDb::initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage&)
{
  // See TxDbRef::initializeImpl
  if (cfg->lookupBoolean(name().c_str(), "asyncFlush", true))
    enableAsyncFlush();

  return true;
}

void TxDb::connectImpl(const BC::Common::BlockIndex *index,
                       const BC::Proto::Block &block,
                       const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                       const BC::Proto::CBlockValidationData &validationData,
                       BlockInMemoryIndex&,
                       BlockDatabase&)
{
  assert(validationData.TxIds.size() == block.vtx.size());
  const auto blockId = index->Header.GetHash();
  SmallStream<4096> stream;
  // A BIP30 repeat carries a coinbase this database already holds, byte for byte
  // the same one: leaving the twin's record alone keeps the key write-once, and a
  // query answers with both inclusions from the chain params
  for (size_t i = firstTx(validationData), ie = block.vtx.size(); i != ie; i++) {
    const auto &tx = block.vtx[i];

    stream.reset();
    CLogData *data = stream.reserve<CLogData>(1);
    data->Hash = blockId;
    data->Index = i;
    BC::serialize(stream, tx);
    BC::serialize(stream, linkedOutputs.Tx[i]);
    this->putNew(validationData.TxIds[i], stream.data(), stream.sizeOf());
  }
}

void TxDb::disconnectImpl(const BC::Common::BlockIndex*,
                          const BC::Proto::Block &block,
                          const BC::Proto::CBlockLinkedOutputs&,
                          const BC::Proto::CBlockValidationData &validationData,
                          BlockInMemoryIndex&,
                          BlockDatabase&)
{
  assert(validationData.TxIds.size() == block.vtx.size());
  for (size_t i = firstTx(validationData), ie = block.vtx.size(); i != ie; i++)
    erase(validationData.TxIds[i]);
}

}
}
