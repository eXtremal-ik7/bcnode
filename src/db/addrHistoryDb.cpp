#include "addrHistoryDb.h"
#include "storage.h"
#include "archive.h"

namespace BC {
namespace DB {

bool AddrHistoryDb::queryAddrHistory(const BC::Script::CAddress &address, size_t from, size_t count, CQueryAddrHistory &result)
{
  xmstream data;
  size_t totalTxCount;
  if (this->query(address, from, count, data, &totalTxCount) && data.sizeOf() % sizeof(CAddrHistoryItem) == 0) {
    const CAddrHistoryItem *p = (const CAddrHistoryItem*)data.data();
    size_t itemsNum = data.sizeOf() / sizeof(CAddrHistoryItem);
    result.Items.assign(p, p + itemsNum);
    result.TotalTxCount = totalTxCount;
    return true;
  }

  return false;
}

bool AddrHistoryDb::initializeImpl(config4cpp::Configuration*, BC::DB::Storage&)
{
  return true;
}

void AddrHistoryDb::connectImpl(CBlockBatch batch, CKvWriter<BC::Script::CAddress> &writer, BlockInMemoryIndex&, BlockDatabase&)
{
  for (const CBlockRef &ref: batch) {
    const BC::Proto::Block &block = *ref.Block;
    const BC::Proto::CBlockLinkedOutputs &linkedOutputs = *ref.LinkedOutputs;
    const BC::Proto::CBlockValidationData &validationData = *ref.ValidationData;
    assert(validationData.TxIds.size() == block.vtx.size());
    if (block.vtx.empty())
      continue;

    const uint32_t height = ref.Index->Height;
    const uint32_t time = ref.Index->Header.nTime;

    std::unordered_map<BC::Script::CAddress, std::vector<CAddrHistoryItem>> historyMap;

    // Net balance delta of the transaction for each affected address; an address
    // touched with a zero net change (self-send) still gets a history element
    std::unordered_map<BC::Script::CAddress, uint64_t> txDelta;
    auto flushTx = [&](const BC::Proto::TxHashTy &hash) {
      for (const auto &d: txDelta) {
        CAddrHistoryItem &item = historyMap[d.first].emplace_back();
        item.TxId = hash;
        item.Height = height;
        item.Time = time;
        // The delta; CKvArrayBase folds it into the running balance
        item.Aggregate = d.second;
      }
      txDelta.clear();
    };

    // Coinbase
    {
      const auto &coinbaseTx = block.vtx[0];
      BC::Script::CAddress address;
      for (const auto &txout: coinbaseTx.txOut) {
        if (BC::Script::extractAddress(txout, address)) {
          // A BIP30 repeat replaces the twin's coins with identical ones and only
          // one of the two can ever be spent: the address gets the history element
          // (the block did pay it) with a zero delta, so the running balance stays
          // equal to what the utxo set holds
          if (validationData.CoinbaseRepeat)
            txDelta[address];
          else
            txDelta[address] += txout.value;
        }
      }

      // TODO: check kind on txid (segwit or normal)
      flushTx(validationData.TxIds[0]);
    }

    // Other transactions
    assert(linkedOutputs.Tx.size() == block.vtx.size());

    for (size_t i = 1; i < block.vtx.size(); i++) {
      const auto &tx = block.vtx[i];
      const auto &linkedTx = linkedOutputs.Tx[i];

      assert(linkedTx.TxIn.size() == tx.txIn.size());

      BC::Script::CAddress address;
      for (size_t j = 0; j < tx.txIn.size(); j++) {
        const auto &linkedTxin = linkedTx.TxIn[j];
        assert(linkedTxin.size() >= sizeof(BC::Script::UnspentOutputInfo));

        const BC::Script::UnspentOutputInfo *outputInfo = (const BC::Script::UnspentOutputInfo*)linkedTxin.data();
        if (BC::Script::extractAddress(*outputInfo, address))
          txDelta[address] -= outputInfo->Value;
      }

      for (const auto &txout: tx.txOut) {
        if (BC::Script::extractAddress(txout, address))
          txDelta[address] += txout.value;
      }

      flushTx(validationData.TxIds[i]);
    }

    for (const auto &addr: historyMap)
      this->add(writer, addr.first, addr.second.data(), addr.second.size());
  }
}

void AddrHistoryDb::disconnectImpl(const BC::Common::BlockIndex*,
                                   const BC::Proto::Block &block,
                                   const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                                   const BC::Proto::CBlockValidationData&,
                                   CKvWriter<BC::Script::CAddress> &writer,
                                   BlockInMemoryIndex&,
                                   BlockDatabase&)
{
  if (block.vtx.empty())
    return;

  std::unordered_map<BC::Script::CAddress, size_t> txMap;

  // Coinbase
  {
    const auto &coinbaseTx = block.vtx[0];
    std::unordered_set<BC::Script::CAddress> affectedAddresses;
    BC::Script::CAddress address;
    for (const auto &txout: coinbaseTx.txOut) {
      if (BC::Script::extractAddress(txout, address)) {
        if (affectedAddresses.insert(address).second)
          txMap[address]++;
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

      BC::Script::UnspentOutputInfo *outputInfo = (BC::Script::UnspentOutputInfo*)linkedTxin.data();
      if (BC::Script::extractAddress(*outputInfo, address) && affectedAddresses.insert(address).second)
        txMap[address]++;
    }

    for (const auto &txout: tx.txOut) {
      if (BC::Script::extractAddress(txout, address)) {
        if (affectedAddresses.insert(address).second)
          txMap[address]++;
      }
    }
  }

  for (const auto &addr: txMap)
    this->truncate(writer, addr.first, addr.second);
}

}
}
