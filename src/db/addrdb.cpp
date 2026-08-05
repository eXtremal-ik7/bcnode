// Copyright (c) 2026 Ivan K.
// Copyright (c) 2026 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "addrdb.h"
#include "storage.h"

namespace BC {
namespace DB {

// Net per-block delta for each affected address; connect merges it as is,
// disconnect merges it negated
static void buildBlockDelta(const BC::Proto::Block &block,
                            const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            bool coinbaseRepeat,
                            std::unordered_map<BC::Script::CAddress, CAddrValue> &deltaMap)
{
  // Coinbase
  {
    const auto &coinbaseTx = block.vtx[0];
    std::unordered_set<BC::Script::CAddress> affectedAddresses;
    BC::Script::CAddress address;
    for (const auto &txout: coinbaseTx.txOut) {
      if (BC::Script::extractAddress(txout, address)) {
        CAddrValue &delta = deltaMap[address];
        // A BIP30 repeat pays no one twice: its outputs replace the twin's coins
        // with identical ones, and only one of the two can ever be spent. The
        // transaction is counted, the money and the outputs are not - otherwise
        // the balance and the utxo count of the address stay above what the utxo
        // set holds forever
        if (!coinbaseRepeat) {
          delta.Received += txout.value;
          delta.Mined += txout.value;
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
  assert(linkedOutputs.Tx.size() == block.vtx.size());

  for (size_t i = 1; i < block.vtx.size(); i++) {
    std::unordered_set<BC::Script::CAddress> affectedAddresses;
    const auto &tx = block.vtx[i];
    const auto &linkedTx = linkedOutputs.Tx[i];

    assert(linkedTx.TxIn.size() == tx.txIn.size());

    BC::Script::CAddress address;
    for (size_t j = 0; j < tx.txIn.size(); j++) {
      const auto &linkedTxin = linkedTx.TxIn[j];
      assert(linkedTxin.size() >= sizeof(BC::Script::UnspentOutputInfo));

      const BC::Script::UnspentOutputInfo *outputInfo = (const BC::Script::UnspentOutputInfo*)linkedTxin.data();
      if (BC::Script::extractAddress(*outputInfo, address)) {
        CAddrValue &delta = deltaMap[address];
        delta.Sent += outputInfo->Value;
        delta.TxInCount++;
        if (affectedAddresses.insert(address).second)
          delta.TxCount++;
      }
    }

    for (const auto &txout: tx.txOut) {
      if (BC::Script::extractAddress(txout, address)) {
        CAddrValue &delta = deltaMap[address];
        delta.Received += txout.value;
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

void AddrDb::connectImpl(CBlockBatch batch, CKvWriter<BC::Script::CAddress> &writer, BlockInMemoryIndex&, BlockDatabase&)
{
  std::unordered_map<BC::Script::CAddress, CAddrValue> deltaMap;
  for (const CBlockRef &ref: batch) {
    if (ref.Block->vtx.empty())
      continue;

    deltaMap.clear();
    buildBlockDelta(*ref.Block, *ref.LinkedOutputs, ref.ValidationData->CoinbaseRepeat, deltaMap);

    for (const auto &addr: deltaMap)
      this->merge(writer, addr.first, addr.second);
  }
}

void AddrDb::disconnectImpl(const BC::Common::BlockIndex*,
                            const BC::Proto::Block &block,
                            const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            const BC::Proto::CBlockValidationData &validationData,
                            CKvWriter<BC::Script::CAddress> &writer,
                            BlockInMemoryIndex&,
                            BlockDatabase&)
{
  if (block.vtx.empty())
    return;

  std::unordered_map<BC::Script::CAddress, CAddrValue> deltaMap;
  buildBlockDelta(block, linkedOutputs, validationData.CoinbaseRepeat, deltaMap);

  for (auto &addr: deltaMap) {
    addr.second.negate();
    this->merge(writer, addr.first, addr.second);
  }
}

}
}
