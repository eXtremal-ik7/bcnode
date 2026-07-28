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
                            std::unordered_map<BC::Proto::AddressTy, CAddrValue> &deltaMap)
{
  // Coinbase
  {
    const auto &coinbaseTx = block.vtx[0];
    std::unordered_set<BC::Proto::AddressTy> affectedAddresses;
    BC::Proto::AddressTy address;
    for (const auto &txout: coinbaseTx.txOut) {
      auto type = BC::Script::extractSingleAddress(txout, address);
      if (type != BC::Script::UnspentOutputInfo::EInvalid) {
        CAddrValue &delta = deltaMap[address];
        delta.AddressType = type;
        delta.Received += txout.value;
        delta.Mined += txout.value;
        delta.TxOutCount++;
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
    std::unordered_set<BC::Proto::AddressTy> affectedAddresses;
    const auto &tx = block.vtx[i];
    const auto &linkedTx = linkedOutputs.Tx[i];

    assert(linkedTx.TxIn.size() == tx.txIn.size());

    BC::Proto::AddressTy address;
    for (size_t j = 0; j < tx.txIn.size(); j++) {
      const auto &linkedTxin = linkedTx.TxIn[j];
      assert(linkedTxin.size() >= sizeof(BC::Script::UnspentOutputInfo));

      const BC::Script::UnspentOutputInfo *outputInfo = (const BC::Script::UnspentOutputInfo*)linkedTxin.data();
      if (BC::Script::extractSingleAddress(*outputInfo, address)) {
        CAddrValue &delta = deltaMap[address];
        delta.AddressType = outputInfo->Type;
        delta.Sent += outputInfo->Value;
        delta.TxInCount++;
        if (affectedAddresses.insert(address).second)
          delta.TxCount++;
      }
    }

    for (const auto &txout: tx.txOut) {
      auto type = BC::Script::extractSingleAddress(txout, address);
      if (type != BC::Script::UnspentOutputInfo::EInvalid) {
        CAddrValue &delta = deltaMap[address];
        delta.AddressType = type;
        delta.Received += txout.value;
        delta.TxOutCount++;
        if (affectedAddresses.insert(address).second)
          delta.TxCount++;
      }
    }
  }
}

bool AddrDb::queryAddr(const BC::Proto::AddressTy &address, CAddrValue &result)
{
  return this->find(address, result);
}

bool AddrDb::queryTop(const std::string &index, size_t offset, size_t limit,
                      std::vector<std::pair<BC::Proto::AddressTy, CAddrValue>> &result)
{
  return this->top(index, offset, limit, result);
}

void AddrDb::connectImpl(const BC::Common::BlockIndex *index,
                         const BC::Proto::Block &block,
                         const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                         BlockInMemoryIndex&,
                         BlockDatabase&)
{
  if (block.vtx.empty())
    return;

  const auto blockId = index->Header.GetHash();
  std::unordered_map<BC::Proto::AddressTy, CAddrValue> deltaMap;
  buildBlockDelta(block, linkedOutputs, deltaMap);

  for (const auto &addr: deltaMap)
    this->merge(blockId, addr.first, addr.second);
}

void AddrDb::disconnectImpl(const BC::Common::BlockIndex *index,
                            const BC::Proto::Block &block,
                            const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            BlockInMemoryIndex&,
                            BlockDatabase&)
{
  if (block.vtx.empty())
    return;

  const auto blockId = index->Header.GetHash();
  std::unordered_map<BC::Proto::AddressTy, CAddrValue> deltaMap;
  buildBlockDelta(block, linkedOutputs, deltaMap);

  for (auto &addr: deltaMap) {
    addr.second.negate();
    this->merge(blockId, addr.first, addr.second);
  }
}

}
}
