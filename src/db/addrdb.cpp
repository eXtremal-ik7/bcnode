// Copyright (c) 2026 Ivan K.
// Copyright (c) 2026 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "addrdb.h"
#include "storage.h"

#include "thirdparty/ankerl/unordered_dense.h"

namespace BC {
namespace DB {

// Net per-block delta for each affected address; connect merges it as is,
// disconnect merges it negated
static void buildBlockDelta(const BC::Proto::CBlock &block,
                            const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            const BC::Proto::CBlockValidationData &validationData,
                            ankerl::unordered_dense::map<BC::Script::CAddress, CAddrValue> &deltaMap)
{
  const bool coinbaseRepeat = validationData.CoinbaseRepeat;
  // Outputs come from the records parsed once per block; an empty one is not a utxo.
  // The ordinal runs over every output of the block in walk order, as it does in utxodb
  size_t outOrdinal = 0;
  auto outputInfoAt = [&validationData](size_t ordinal) -> const BC::Script::CUnspentOutputInfo* {
    size_t size;
    const void *data = validationData.outputData(ordinal, size);
    return size ? static_cast<const BC::Script::CUnspentOutputInfo*>(data) : nullptr;
  };

  // Coinbase
  {
    const auto &coinbaseTx = block.Vtx[0];
    ankerl::unordered_dense::set<BC::Script::CAddress> affectedAddresses;
    BC::Script::CAddress address;
    for (size_t j = 0; j < coinbaseTx.TxOut.size(); j++, outOrdinal++) {
      const BC::Script::CUnspentOutputInfo *outputInfo = outputInfoAt(outOrdinal);
      if (outputInfo && BC::Script::extractAddress(*outputInfo, address)) {
        CAddrValue &delta = deltaMap[address];
        // A BIP30 repeat pays no one twice: its outputs replace the twin's coins
        // with identical ones, and only one of the two can ever be spent. The
        // transaction is counted, the money and the outputs are not - otherwise
        // the balance and the utxo count of the address stay above what the utxo
        // set holds forever
        if (!coinbaseRepeat) {
          delta.Received += unsignedAmount(outputInfo->Value);
          delta.Mined += unsignedAmount(outputInfo->Value);
          delta.TxOutCount++;
        }
        if (affectedAddresses.insert(address).second) {
          delta.TxCount++;
          delta.MinedTxCount++;
        }
      }
    }
  }

  // Other transactions
  assert(linkedOutputs.Tx.size() == block.Vtx.size());

  for (size_t i = 1; i < block.Vtx.size(); i++) {
    ankerl::unordered_dense::set<BC::Script::CAddress> affectedAddresses;
    const auto &tx = block.Vtx[i];
    const auto &linkedTx = linkedOutputs.Tx[i];

    assert(linkedTx.TxIn.size() == tx.TxIn.size());

    BC::Script::CAddress address;
    for (size_t j = 0; j < tx.TxIn.size(); j++) {
      const auto &linkedTxin = linkedTx.TxIn[j];
      assert(linkedTxin.size() >= sizeof(BC::Script::CUnspentOutputInfo));

      const BC::Script::CUnspentOutputInfo *outputInfo = (const BC::Script::CUnspentOutputInfo*)linkedTxin.data();
      if (BC::Script::extractAddress(*outputInfo, address)) {
        CAddrValue &delta = deltaMap[address];
        delta.Sent += unsignedAmount(outputInfo->Value);
        delta.TxInCount++;
        if (affectedAddresses.insert(address).second)
          delta.TxCount++;
      }
    }

    for (size_t j = 0; j < tx.TxOut.size(); j++, outOrdinal++) {
      const BC::Script::CUnspentOutputInfo *outputInfo = outputInfoAt(outOrdinal);
      if (outputInfo && BC::Script::extractAddress(*outputInfo, address)) {
        CAddrValue &delta = deltaMap[address];
        delta.Received += unsignedAmount(outputInfo->Value);
        delta.TxOutCount++;
        if (affectedAddresses.insert(address).second)
          delta.TxCount++;
      }
    }
  }
}

bool AddrDb::queryAddr(const BC::Script::CAddress &address, CAddrValue &result)
{
  return this->find(address, result);
}

bool AddrDb::queryTop(const std::string &index, size_t offset, size_t limit,
                      std::vector<std::pair<BC::Script::CAddress, CAddrValue>> &result)
{
  return this->top(index, offset, limit, result);
}

void AddrDb::connect(CBlockBatch batch, BlockInMemoryIndex&, BlockDatabase&)
{
  dbengine::CKvWriter<BC::Script::CAddress> writer = liveWriter();
  ankerl::unordered_dense::map<BC::Script::CAddress, CAddrValue> deltaMap;
  for (const CBlockRef &ref: batch) {
    if (ref.Block->Vtx.empty())
      continue;

    deltaMap.clear();
    buildBlockDelta(*ref.Block, *ref.LinkedOutputs, *ref.ValidationData, deltaMap);

    for (const auto &addr: deltaMap)
      this->merge(writer, addr.first, addr.second);
  }
  commit(writer, batch.back().Index->Header.GetHash());
}

void AddrDb::disconnect(const BC::Common::BlockIndex *index,
                            const BC::Proto::CBlock &block,
                            const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            const BC::Proto::CBlockValidationData &validationData,
                            BlockInMemoryIndex&,
                            BlockDatabase&)
{
  dbengine::CKvWriter<BC::Script::CAddress> writer = liveWriter();
  // Nothing to undo, but the position still moves off this block
  if (block.Vtx.empty()) {
    commit(writer, index->Header.HashPrevBlock);
    return;
  }

  ankerl::unordered_dense::map<BC::Script::CAddress, CAddrValue> deltaMap;
  buildBlockDelta(block, linkedOutputs, validationData, deltaMap);

  for (auto &addr: deltaMap) {
    addr.second.negate();
    this->merge(writer, addr.first, addr.second);
  }
  commit(writer, index->Header.HashPrevBlock);
}

}
}
