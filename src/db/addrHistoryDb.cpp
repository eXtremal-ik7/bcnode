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

bool AddrHistoryDb::initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage)
{
  UTXODb_ = &storage.utxodb();
  TxDb_ = storage.archive().TransactionDb_;
  if (!TxDb_) {
    LOG_F(ERROR, "addrhistory depends on transaction db, need to enable it first and setup 'tx' handler");
    return false;
  }

  return true;
}

void AddrHistoryDb::connectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb)
{
  const auto blockId = index->Header.GetHash();
  if (block.vtx.empty())
    return;

  std::unordered_map<BC::Proto::AddressTy, std::vector<BC::Proto::TxHashTy>> txMap;
  std::unordered_map<BC::Proto::TxHashTy, size_t> internalTxMap;

  // Coinbase
  {
    const auto &coinbaseTx = block.vtx[0];
    // TODO: check kind on txid (segwit or normal)
    BC::Proto::TxHashTy hash = coinbaseTx.getTxId();

    BC::Proto::AddressTy address;
    for (const auto &txout: coinbaseTx.txOut) {
      if (BC::Script::decodeStandardOutput(txout, address))
        txMap[address].push_back(hash);

      internalTxMap[hash] = 0;
    }
  }

  // Other transactions
  for (size_t i = 1; i < block.vtx.size(); i++) {
    std::unordered_set<BC::Proto::AddressTy> affectedAddresses;
    const auto &tx = block.vtx[i];
    BC::Proto::TxHashTy hash = tx.getTxId();

    BC::Proto::AddressTy address;
    for (size_t i = 0; i < tx.txIn.size(); i++) {
      const auto &txin = tx.txIn[i];
      xmstream output;
      if (!searchUnspentOutput(txin.previousOutputHash,
                               txin.previousOutputIndex,
                               block,
                               internalTxMap,
                               blockIndex,
                               blockDb,
                               UTXODb_,
                               TxDb_,
                               output)) {
        // Corrupted database
        LOG_F(ERROR, "tx %s input %zu refer non-existing utxo %s:%u at %u",
              hash.GetHex().c_str(), i, txin.previousOutputHash.GetHex().c_str(), txin.previousOutputIndex,
              index->Height);
        exit(1);
      }

      BC::Script::UnspentOutputInfo *outputInfo = (BC::Script::UnspentOutputInfo*)output.data();
      if (outputInfo->Type == BC::Script::UnspentOutputInfo::EPubKeyHash ||
          outputInfo->Type == BC::Script::UnspentOutputInfo::EScriptHash) {
        if (affectedAddresses.insert(outputInfo->PubKeyHash).second)
          txMap[outputInfo->PubKeyHash].push_back(hash);
      }
    }

    for (const auto &txout: tx.txOut) {
      if (BC::Script::decodeStandardOutput(txout, address)) {
        if (affectedAddresses.insert(address).second)
          txMap[address].push_back(hash);
      }
    }

    internalTxMap[hash] = i;
  }

  for (const auto &addr: txMap)
    this->add(blockId, addr.first, &addr.second[0], addr.second.size() * sizeof(BC::Proto::TxHashTy), addr.second.size());
}

void AddrHistoryDb::disconnectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb)
{
  const auto blockId = index->Header.GetHash();
  if (block.vtx.empty())
    return;

  std::unordered_map<BC::Proto::AddressTy, size_t> txMap;
  std::unordered_map<BC::Proto::TxHashTy, size_t> internalTxMap;

  // Coinbase
  {
    const auto &coinbaseTx = block.vtx[0];
    BC::Proto::AddressTy address;
    for (const auto &txout: coinbaseTx.txOut) {
      if (BC::Script::decodeStandardOutput(txout, address))
        txMap[address]++;
    }
  }

  // Other transactions
  for (size_t i = 1; i < block.vtx.size(); i++) {
    std::unordered_set<BC::Proto::AddressTy> affectedAddresses;
    const auto &tx = block.vtx[i];
    BC::Proto::TxHashTy hash = tx.getTxId();

    BC::Proto::AddressTy address;
    for (const auto &txin: tx.txIn) {
      xmstream output;
      if (!searchUnspentOutput(txin.previousOutputHash,
                               txin.previousOutputIndex,
                               block,
                               internalTxMap,
                               blockIndex,
                               blockDb,
                               UTXODb_,
                               TxDb_,
                               output)) {
        // Corrupted database
        LOG_F(ERROR, "tx %s input %zu refer non-existing utxo %s:%u at %u",
              hash.GetHex().c_str(), i, txin.previousOutputHash.GetHex().c_str(), txin.previousOutputIndex,
              index->Height);
        exit(1);
      }

      BC::Script::UnspentOutputInfo *outputInfo = (BC::Script::UnspentOutputInfo*)output.data();
      if (outputInfo->Type == BC::Script::UnspentOutputInfo::EPubKeyHash ||
          outputInfo->Type == BC::Script::UnspentOutputInfo::EScriptHash) {
        if (affectedAddresses.insert(outputInfo->PubKeyHash).second)
        txMap[outputInfo->PubKeyHash]++;
      }
    }

    for (const auto &txout: tx.txOut) {
      if (BC::Script::decodeStandardOutput(txout, address)) {
        if (affectedAddresses.insert(address).second)
          txMap[address]++;
      }
    }

    internalTxMap[hash] = i;
  }

  for (const auto &addr: txMap)
    this->remove(blockId, addr.first, addr.second);
}

}
}
