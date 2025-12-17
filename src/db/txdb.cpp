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

bool TxDb::initializeImpl(config4cpp::Configuration*, BC::DB::Storage&)
{
  return true;
}

void TxDb::connectImpl(const BC::Common::BlockIndex *index,
                       const BC::Proto::Block &block,
                       const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                       BlockInMemoryIndex&,
                       BlockDatabase&)
{
  const auto blockId = index->Header.GetHash();
  SmallStream<4096> stream;
  for (size_t i = 0, ie = block.vtx.size(); i != ie; i++) {
    auto tx = block.vtx[i];
    BC::Proto::BlockHashTy hash = tx.getTxId();

    stream.reset();
    CLogData *data = stream.reserve<CLogData>(1);
    data->Hash = blockId;
    data->Index = i;
    BC::serialize(stream, tx);
    BC::serialize(stream, linkedOutputs.Tx[i]);
    this->add(blockId, hash, stream.data(), stream.sizeOf());
  }
}

void TxDb::disconnectImpl(const BC::Common::BlockIndex *index,
                          const BC::Proto::Block &block,
                          const BC::Proto::CBlockLinkedOutputs&,
                          BlockInMemoryIndex&,
                          BlockDatabase&)
{
  const auto blockId = index->Header.GetHash();
  for (size_t i = 0, ie = block.vtx.size(); i != ie; i++)
    remove(blockId, block.vtx[i].getTxId());
}

}
}
