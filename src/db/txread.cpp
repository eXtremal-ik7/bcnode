// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "db/queries.h"
#include "common/smallStream.h"

namespace BC {
namespace DB {

bool readTransactionAt(BC::Common::BlockIndex *index,
                       uint32_t txIndex,
                       uint32_t txOffset,
                       uint32_t txSize,
                       BlockDatabase &blockDb,
                       CQueryTransactionResult &result)
{
  result.Block = index->Header.GetHash();
  result.TxNum = txIndex;

  // Still in memory: the parsed block is there, no file read and no unserialize
  intrusive_ptr<BC::Common::CIndexCacheObject> serializedPtr(index->Serialized);
  if (serializedPtr.get()) {
    BC::Proto::Block *block = serializedPtr.get()->block();
    result.Tx = block->vtx[txIndex];
    result.LinkedOutputs = serializedPtr.get()->linkedOutputs().Tx[txIndex];
    return true;
  }

  if (!index->indexStored()) {
    result.DataCorrupted = true;
    return false;
  }

  // The 8 bytes are the magic and the length in front of every stored block
  SmallStream<16384> stream;
  if (!blockDb.blockReader().read(index->FileNo, index->FileOffset + txOffset + 8,
                                  stream.reserve(txSize), txSize)) {
    result.DataCorrupted = true;
    return false;
  }

  stream.seekSet(0);
  if (!BC::unserializeAndCheck(stream, result.Tx)) {
    result.DataCorrupted = true;
    return false;
  }

  // Linked outputs are one blob per block, so this one is read whole and indexed
  BC::Proto::CBlockLinkedOutputs linkedOutputs;
  stream.reset();
  if (!blockDb.linkedOutputsReader().read(index->LinkedOutputsFileNo,
                                          index->LinkedOutputsFileOffset + 4,
                                          stream.reserve(index->LinkedOutputsSerializedSize),
                                          index->LinkedOutputsSerializedSize)) {
    result.DataCorrupted = true;
    return false;
  }

  stream.seekSet(0);
  if (!BC::unserializeAndCheck(stream, linkedOutputs)) {
    result.DataCorrupted = true;
    return false;
  }

  result.LinkedOutputs = linkedOutputs.Tx[txIndex];
  return true;
}

}
}
