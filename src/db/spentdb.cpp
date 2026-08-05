// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "spentdb.h"
#include "config4cpp/Configuration.h"

namespace BC {
namespace DB {

bool SpentDb::querySpent(const BC::Proto::TxHashTy &txid, uint32_t index, CQuerySpentResult &result)
{
  COutpointKey key;
  key.Tx = txid;
  key.Index = index;

  result.Found = false;
  this->find(key, [&result](const void *data, size_t size) {
    // A record of another width belongs to a database built by another
    // version: report the output as unspent rather than decode garbage
    if (size != sizeof(CSpentValue))
      return;
    // the record is the struct byte for byte, but a blob member makes the type
    // non-trivially-copyable for the compiler
    memcpy(static_cast<void*>(&result.Value), data, sizeof(CSpentValue));
    result.Found = true;
  });

  return true;
}

bool SpentDb::querySpentOutputs(const BC::Proto::TxHashTy &txid, uint32_t count, std::vector<CQuerySpentResult> &result)
{
  result.clear();
  result.resize(count);
  for (uint32_t i = 0; i < count; i++)
    querySpent(txid, i, result[i]);

  return true;
}

bool SpentDb::initializeImpl(config4cpp::Configuration*, BC::DB::Storage&)
{
  return true;
}

void SpentDb::connectImpl(CBlockBatch batch, CKvWriter<COutpointKey> &writer, BlockInMemoryIndex&, BlockDatabase&)
{
  COutpointKey key;
  CSpentValue value;

  for (const CBlockRef &ref: batch) {
    const BC::Proto::Block &block = *ref.Block;
    const BC::Proto::CBlockValidationData &validationData = *ref.ValidationData;
    assert(validationData.TxIds.size() == block.vtx.size());
    value.Height = ref.Index->Height;

    // vtx[0] is the coinbase and spends nothing. Same-block and same-run pairs
    // get their mark like any other spend: the pair skip of the utxo db hides an
    // output that was still created and still spent, and this database is the
    // only place that says so
    for (size_t i = 1, ie = block.vtx.size(); i != ie; i++) {
      const auto &tx = block.vtx[i];
      value.SpentBy = validationData.TxIds[i];
      for (size_t j = 0, je = tx.txIn.size(); j != je; j++) {
        key.Tx = tx.txIn[j].previousOutputHash;
        key.Index = tx.txIn[j].previousOutputIndex;
        value.InputIndex = static_cast<uint32_t>(j);
        // An outpoint is spent once on the chain this database follows, so
        // nothing below can hold a mark for it. The exception is a BIP30 twin
        // re-spent and reorged away inside one window, which would leave the
        // stale mark of the first spend on disk: reachable only below the BIP30
        // activation, and not worth a read on the write path
        writer.putNew(key, &value, sizeof(value));
      }
    }
  }
}

void SpentDb::disconnectImpl(const BC::Common::BlockIndex*,
                             const BC::Proto::Block &block,
                             const BC::Proto::CBlockLinkedOutputs&,
                             const BC::Proto::CBlockValidationData&,
                             CKvWriter<COutpointKey> &writer,
                             BlockInMemoryIndex&,
                             BlockDatabase&)
{
  COutpointKey key;

  // Only the marks this block wrote go away. Its own outputs cannot be spent
  // while it is being disconnected - whoever spent them was disconnected first
  for (size_t i = 1, ie = block.vtx.size(); i != ie; i++) {
    const auto &tx = block.vtx[i];
    for (size_t j = 0, je = tx.txIn.size(); j != je; j++) {
      key.Tx = tx.txIn[j].previousOutputHash;
      key.Index = tx.txIn[j].previousOutputIndex;
      writer.erase(key);
    }
  }
}

}
}
