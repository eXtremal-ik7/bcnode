#include "addrHistoryDb.h"
#include "storage.h"
#include "archive.h"

namespace BC {
namespace DB {

bool AddrHistoryDb::queryAddrTxid(const BC::Proto::AddressTy &address, size_t from, size_t count, CQueryAddrHistory &result)
{
  xmstream data;
  size_t totalTxCount;
  if (this->query(address, from, count, data, &totalTxCount) && data.sizeOf() % sizeof(BC::Proto::TxHashTy) == 0) {
    BC::Proto::TxHashTy *p = (BC::Proto::TxHashTy*)data.data();
    size_t count = data.sizeOf() / sizeof(BC::Proto::TxHashTy);
    result.Transactions.resize(count);
    for (size_t i = 0; i < count; i++)
      result.Transactions[i] = p[i];
    result.TotalTxCount = totalTxCount;
    return true;
  }

  return false;
}

bool AddrHistoryDb::initializeImpl(config4cpp::Configuration*, BC::DB::Storage&)
{
  return true;
}

void AddrHistoryDb::connectImpl(const BC::Common::BlockIndex *index,
                                const BC::Proto::Block &block,
                                const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                                BlockInMemoryIndex&,
                                BlockDatabase&)
{
  const auto blockId = index->Header.GetHash();
  if (block.vtx.empty())
    return;

  std::unordered_map<BC::Proto::AddressTy, std::vector<BC::Proto::TxHashTy>> txMap;

  // Coinbase
  {
    const auto &coinbaseTx = block.vtx[0];
    // TODO: check kind on txid (segwit or normal)
    BC::Proto::TxHashTy hash = coinbaseTx.getTxId();

    BC::Proto::AddressTy address;
    for (const auto &txout: coinbaseTx.txOut) {
      if (BC::Script::extractSingleAddress(txout, address) != BC::Script::UnspentOutputInfo::EInvalid)
        txMap[address].push_back(hash);
    }
  }

  // Other transactions
  assert(linkedOutputs.Tx.size() == block.vtx.size());

  for (size_t i = 1; i < block.vtx.size(); i++) {
    std::unordered_set<BC::Proto::AddressTy> affectedAddresses;
    const auto &tx = block.vtx[i];
    const auto &linkedTx = linkedOutputs.Tx[i];
    BC::Proto::TxHashTy hash = tx.getTxId();

    assert(linkedTx.TxIn.size() == tx.txIn.size());

    BC::Proto::AddressTy address;
    for (size_t j = 0; j < tx.txIn.size(); j++) {
      const auto &linkedTxin = linkedTx.TxIn[j];
      assert(linkedTxin.size() >= sizeof(BC::Script::UnspentOutputInfo));

      BC::Script::UnspentOutputInfo *outputInfo = (BC::Script::UnspentOutputInfo*)linkedTxin.data();
      if (BC::Script::extractSingleAddress(*outputInfo, address) && affectedAddresses.insert(address).second)
        txMap[address].push_back(hash);
    }

    for (const auto &txout: tx.txOut) {
      if (BC::Script::extractSingleAddress(txout, address) != BC::Script::UnspentOutputInfo::EInvalid) {
        if (affectedAddresses.insert(address).second)
          txMap[address].push_back(hash);
      }
    }
  }

  for (const auto &addr: txMap)
    this->add(blockId, addr.first, &addr.second[0], addr.second.size() * sizeof(BC::Proto::TxHashTy), addr.second.size());
}

void AddrHistoryDb::disconnectImpl(const BC::Common::BlockIndex *index,
                                   const BC::Proto::Block &block,
                                   const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                                   BlockInMemoryIndex&,
                                   BlockDatabase&)
{
  const auto blockId = index->Header.GetHash();
  if (block.vtx.empty())
    return;

  std::unordered_map<BC::Proto::AddressTy, size_t> txMap;

  // Coinbase
  {
    const auto &coinbaseTx = block.vtx[0];
    BC::Proto::AddressTy address;
    for (const auto &txout: coinbaseTx.txOut) {
      if (BC::Script::extractSingleAddress(txout, address) != BC::Script::UnspentOutputInfo::EInvalid)
        txMap[address]++;
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

      BC::Script::UnspentOutputInfo *outputInfo = (BC::Script::UnspentOutputInfo*)linkedTxin.data();
      if (BC::Script::extractSingleAddress(*outputInfo, address) && affectedAddresses.insert(address).second)
        txMap[address]++;
    }

    for (const auto &txout: tx.txOut) {
      if (BC::Script::extractSingleAddress(txout, address) != BC::Script::UnspentOutputInfo::EInvalid) {
        if (affectedAddresses.insert(address).second)
          txMap[address]++;
      }
    }
  }

  for (const auto &addr: txMap)
    this->remove(blockId, addr.first, addr.second);
}

}
}
