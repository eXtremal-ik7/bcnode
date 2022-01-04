// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "balancedb.h"
#include "config4cpp/Configuration.h"
#include "BC/bc.h"
#include "BC/script.h"
#include "db/archive.h"
#include "db/storage.h"
#include "../loguru.hpp"
#include <rocksdb/merge_operator.h>
#include <set>

template<typename T>
static T subNoOverflow(T a, T b) {
  T result = a - b;
  return result <= a ? result : 0;
}

namespace BC {
namespace DB {

class AddrDbMergeOperator: public rocksdb::MergeOperator {
public:

  virtual bool FullMerge(const rocksdb::Slice&,
                         const rocksdb::Slice *existing_value,
                         const std::deque<std::string> &operand_list,
                         std::string *new_value,
                         rocksdb::Logger*) const override {
    new_value->resize(sizeof(BalanceDb::TxValue));
    BalanceDb::TxValue *txValue = reinterpret_cast<BalanceDb::TxValue*>(new_value->data());
    txValue->offset = 0;
    uint32_t minOffset = std::numeric_limits<uint32_t>::max();
    uint32_t maxTxNum = 0;

    if (existing_value) {
      const BalanceDb::TxValue *op = reinterpret_cast<const BalanceDb::TxValue*>(existing_value->data());
      assert(op->offset == 0);

      doMerge(txValue, existing_value->data(), existing_value->size());
      minOffset = op->offset;
      maxTxNum = op->txNum(static_cast<uint32_t>(existing_value->size()));
    }

    for (const auto &operand: operand_list) {
      const BalanceDb::TxValue *op = reinterpret_cast<const BalanceDb::TxValue*>(operand.data());
      doMerge(txValue, operand.data(), operand.size());
      minOffset = std::min(minOffset, op->offset);
      maxTxNum = std::max(maxTxNum, op->offset + op->txNum(static_cast<uint32_t>(operand.size())));
    }

    assert(minOffset == 0);
    assert(maxTxNum <= BalanceDb::TransactionRowSize);
    new_value->resize(txValue->dataSize(maxTxNum));
    return true;
  }



  virtual bool PartialMerge(const rocksdb::Slice&,
                            const rocksdb::Slice &left,
                            const rocksdb::Slice &right,
                            std::string *out,
                            rocksdb::Logger*) const override {
    return doMerge(out, left.data(), static_cast<uint32_t>(left.size()), right.data(), static_cast<uint32_t>(right.size()));
  }

  virtual const char* Name() const override {
    return "BC::DB::AddrDb";
  }

private:
  bool doMerge(std::string *out, const char *leftOperand, uint32_t leftSize, const char *rightOperand, uint32_t rightSize) const {
    const BalanceDb::TxValue *left = reinterpret_cast<const BalanceDb::TxValue*>(leftOperand);
    const BalanceDb::TxValue *right = reinterpret_cast<const BalanceDb::TxValue*>(rightOperand);
    uint32_t leftTxNum = left->txNum(leftSize);
    uint32_t rightTxNum = right->txNum(rightSize);
    uint32_t leftEnd = left->offset + leftTxNum;
    uint32_t rightEnd = right->offset + rightTxNum;
    assert(leftEnd <= BalanceDb::TransactionRowSize);
    assert(rightEnd <= BalanceDb::TransactionRowSize);

    if (left->offset <= right->offset) {
      if (left->offset + leftTxNum < right->offset)
        return false;
      out->assign(leftOperand, leftSize);
      out->replace(BalanceDb::TxValue::dataSize(right->offset - left->offset),
                   rightTxNum*sizeof(BC::Proto::TxHashTy),
                   reinterpret_cast<const char*>(right->hashes),
                   rightTxNum*sizeof(BC::Proto::TxHashTy));
    } else {
      if (right->offset + rightTxNum < left->offset)
        return false;
      out->assign(rightOperand, rightSize);
      if (leftEnd > rightEnd) {
        size_t leftTail = leftEnd - rightEnd;
        out->append(leftOperand + leftSize - (leftTail*sizeof(BC::Proto::TxHashTy)), leftTail*sizeof(BC::Proto::TxHashTy));
      }
    }

    return true;
  }

  void doMerge(BalanceDb::TxValue *dst, const char *srcData, size_t srcSize) const {
    const BalanceDb::TxValue *src = reinterpret_cast<const BalanceDb::TxValue*>(srcData);
    memcpy(dst->hashes + src->offset, src->hashes, srcSize - sizeof(uint32_t));
  }
};

BalanceDb::~BalanceDb()
{
  flush();
}

void BalanceDb::getConfiguration(config4cpp::Configuration *cfg)
{
  Enabled_ = cfg->lookupBoolean("balancedb", "enabled", false);
  if (!Enabled_)
    return;

  Cfg_.ShardsNum = static_cast<unsigned>(cfg->lookupInt("balancedb", "shardsNum", 1));
  Cfg_.StoreFullTx = cfg->lookupBoolean("balancedb", "storeFullTx", false);
}

bool BalanceDb::initialize(BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, BC::DB::Archive &archive, BC::Common::BlockIndex **forConnect, IndexDbMap &forDisconnect)
{
  if (!Enabled_)
    return true;

  BlockIndex_ = &blockIndex;
  BlockDb_ = &blockDb;
  TxDb_ = &archive.txdb();

  // Open all shards
  BC::Common::BlockIndex *bestIndex = blockIndex.best();
  std::set<BC::Proto::BlockHashTy> knownStamp;
  std::vector<BC::Common::BlockIndex*> forDisconnectLocal;
  std::vector<BC::Common::BlockIndex*> forConnectLocal;
  Databases_.resize(Cfg_.ShardsNum);
  ShardData_.reset(new Shard[Cfg_.ShardsNum]);
  for (unsigned i = 0; i < Cfg_.ShardsNum; i++) {
    auto shardPath = blockDb.dataDir() / "balancedb" / std::to_string(i);
    std::filesystem::create_directories(shardPath);

    rocksdb::DB *db;
    rocksdb::Options options;
    options.create_if_missing = true;
    options.merge_operator.reset(new AddrDbMergeOperator);
    rocksdb::Status status = rocksdb::DB::Open(options, shardPath.u8string().c_str(), &db);
    if (!status.ok()) {
      LOG_F(ERROR, "Can't open or create balancedb database at %s", shardPath.u8string().c_str());
      return false;
    }

    Databases_[i].reset(db);

    Shard &shardData = ShardData_[i];

    if (i == 0) {
      // Check configuration
      rocksdb::Slice key("config");
      std::string value;
      if (db->Get(rocksdb::ReadOptions(), key, &value).ok() && value.size() >= Configuration::Size[0]) {
        const Configuration *storedCfg = reinterpret_cast<const Configuration*>(value.data());
        if (storedCfg->ShardsNum != Cfg_.ShardsNum ||
            storedCfg->StoreFullTx != Cfg_.StoreFullTx) {
          LOG_F(ERROR, "transaction db configuration not compatible with requested configuration, rerun with --reindex=addrdb");
          return false;
        }
      } else {
        // DB not have configuration, write current to it
        rocksdb::Slice value(reinterpret_cast<char*>(&Cfg_), sizeof(Cfg_));
        db->Put(rocksdb::WriteOptions(), key, value);
      }
    }

    // Check stamp (last known block)
    std::string stampData;
    if (db->Get(rocksdb::ReadOptions(), rocksdb::Slice("stamp"), &stampData).ok()) {
      if (stampData.size() != sizeof(Stamp)) {
        LOG_F(ERROR, "txdb is corrupted: invalid stamp size (%s)", shardPath.u8string().c_str());
        return false;
      }

      const Stamp *stamp = reinterpret_cast<const Stamp*>(stampData.data());
      auto It = blockIndex.blockIndex().find(stamp->Hash);
      if (It == blockIndex.blockIndex().end()) {
        LOG_F(ERROR, "txdb is corrupted: stamp not exists in block index (%s)", shardPath.u8string().c_str());
        return false;
      }

      // Build connect and disconnect block set if need
      if (It->second != bestIndex && knownStamp.insert(stamp->Hash).second) {
        BC::Common::BlockIndex *first = rebaseChain(bestIndex, It->second, forDisconnectLocal);
        if (first && (!*forConnect || first->Height < (*forConnect)->Height))
          *forConnect = first;
        for (auto index: forDisconnectLocal)
          forDisconnect[index].Affected[DbTransactions] = true;
      }

      shardData.BatchId = stamp->BatchId + 1;
    } else {
      // database is empty, run full rescanning
      *forConnect = blockIndex.genesis();
      shardData.BatchId = 1;
    }
  }

  return true;
}

void BalanceDb::modify(Shard &shard, const BC::Proto::AddressTy &address, int64_t sent, int64_t received, int64_t txNum, BC::Proto::TxHashTy *transactions)
{
  auto It = shard.Cache.find(address);
  if (It != shard.Cache.end()) {
    CachedValue &value = It->second;

    std::lock_guard lock(shard.Mutex);
    value.TotalSent += sent;
    value.TotalReceived += received;
    value.TransactionsNum += txNum;
    if (txNum < 0) {
      value.TxData.seek(txNum*sizeof(BC::Proto::BlockHashTy));
      value.TxData.truncate();
    } else {
      value.TxData.write(transactions, txNum*sizeof(BC::Proto::BlockHashTy));
    }
  } else {
    std::lock_guard lock(shard.Mutex);
    CachedValue &value = shard.Cache[address];
    value.BatchId = shard.BatchId;
    value.TotalSent = sent;
    value.TotalReceived = received;
    value.TransactionsNum = txNum;
    if (txNum > 0) {
      value.TxData.write(transactions, txNum*sizeof(BC::Proto::BlockHashTy));
      value.TxData.truncate();
    }
  }
}

void BalanceDb::add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy actionType, bool doFlush)
{
  if (block.vtx.empty())
    return;

  // Process coinbase transaction
  const auto &coinbaseTx = block.vtx[0];
  int32_t txDelta = actionType == Connect ? 1 : -1;
  if (coinbaseTx.txIn.size() == 1 && coinbaseTx.txOut.size() == 1) {
    BC::Proto::AddressTy address;
    int64_t delta = coinbaseTx.txOut[0].value;
    if (actionType == Disconnect)
      delta = -delta;

    if (BC::Script::decodeStandardOutput(coinbaseTx.txOut[0], address)) {
      unsigned shardNum = address.GetUint64(0) % Cfg_.ShardsNum;
      auto hash = coinbaseTx.getTxId();
      modify(ShardData_[shardNum], address, 0, delta, txDelta, &hash);
    }
  }

  for (size_t i = 1, ie = block.vtx.size(); i != ie; i++) {
    auto tx = block.vtx[i];
    auto txHash = tx.getTxId();
    std::set<BC::Proto::AddressTy> knownAddresses;
    for (const auto &input: tx.txIn) {
      // Here we need find referenced output
      // Best way: use UTXO cache
      // Now UTXO cache not implemented yet, use txdb database (SLOW!)
      BC::DB::TxDb::QueryResult result;
      TxDb_->find(input.previousOutputHash, *BlockIndex_, *BlockDb_, result);
      if (result.Found && input.previousOutputIndex < result.Tx.txOut.size()) {
        BC::Proto::AddressTy address;
        int64_t delta = result.Tx.txOut[input.previousOutputIndex].value;
        if (actionType == Connect)
          delta = -delta;

        if (BC::Script::decodeStandardOutput(result.Tx.txOut[input.previousOutputIndex], address)) {
          unsigned shardNum = address.GetUint64(0) % Cfg_.ShardsNum;

          modify(ShardData_[shardNum], address, -delta, 0, (knownAddresses.insert(address).second ? txDelta : 0), &txHash);
          knownAddresses.insert(address);
        }
      }
    }

    for (size_t j = 0, je = tx.txOut.size(); j != je; ++j) {
      const auto &output = tx.txOut[j];
      BC::Proto::AddressTy address;
      int64_t delta = output.value;
      if (actionType == Disconnect)
        delta = -delta;

      if (BC::Script::decodeStandardOutput(output, address)) {
        unsigned shardNum = address.GetUint64(0) % Cfg_.ShardsNum;
        modify(ShardData_[shardNum], address, 0, delta, knownAddresses.insert(address).second ? txDelta : 0, &txHash);
      }
    }
  }

  LastAdded_ = index;

  if (doFlush) {
    for (unsigned i = 0; i < Cfg_.ShardsNum; i++) {
      if (ShardData_[i].Cache.size() >= MinimalBatchSize)
        flush(i);
    }
  }
}

bool BalanceDb::find(const BC::Proto::AddressTy &address, QueryResult *result)
{
  unsigned shardNum = address.GetUint64(0) % Cfg_.ShardsNum;
  Shard &shardData = ShardData_[shardNum];
  bool hasCachedValue = false;
  bool hasStoredValue = false;
  Value cachedValue;
  cachedValue.BatchId = shardData.BatchId;

  {
    // Get cached delta
    std::shared_lock lock(shardData.Mutex);
    auto It = shardData.Cache.find(address);

    if (It != shardData.Cache.end()) {
      cachedValue = It->second;
      hasCachedValue = true;
    }
  }

  // Get stored balance
  rocksdb::DB *db = Databases_[shardNum].get();
  rocksdb::Slice key(reinterpret_cast<const char*>(address.begin()), sizeof(BC::Proto::AddressTy));
  std::string data;
  if (db->Get(rocksdb::ReadOptions(), key, &data).ok() && data.size() >= sizeof(Value)) {
    const Value *dbValue = reinterpret_cast<const Value*>(data.data());
    if (cachedValue.BatchId != dbValue->BatchId) {
      cachedValue.TotalSent += dbValue->TotalSent;
      cachedValue.TotalReceived += dbValue->TotalReceived;
      cachedValue.TransactionsNum += dbValue->TransactionsNum;
    }
    hasStoredValue = true;
  }

  result->Balance = cachedValue.TotalReceived - cachedValue.TotalSent;
  result->TotalSent = cachedValue.TotalSent;
  result->TotalReceived = cachedValue.TotalReceived;
  result->TransactionsNum = cachedValue.TransactionsNum;
  return hasCachedValue | hasStoredValue;
}


void BalanceDb::findTxidForAddr(const BC::Proto::AddressTy &address, uint64_t from, uint32_t count, std::vector<BC::Proto::BlockHashTy> &result)
{
  unsigned shardNum = address.GetUint64(0) % Cfg_.ShardsNum;
  Shard &shardData = ShardData_[shardNum];

  rocksdb::DB *db = Databases_[shardNum].get();
  rocksdb::Slice key(reinterpret_cast<const char*>(address.begin()), sizeof(BC::Proto::AddressTy));

  uint64_t current = from;
  uint64_t dbTxCount = 0;
  uint64_t dbBatch = shardData.BatchId - 1;

  uint64_t startRow = from / TransactionRowSize;
  uint64_t endRow = (from + count - 1) / TransactionRowSize;
  uint64_t rowsCount = endRow - startRow + 1;

  std::vector<TxKey> keyData(rowsCount + 1);
  std::vector<rocksdb::Slice> keySlices(rowsCount + 1);
  std::vector<std::string> queryResult(rowsCount + 1);

  keySlices[0] = rocksdb::Slice(reinterpret_cast<const char*>(address.begin()), sizeof(BC::Proto::AddressTy));
  for (unsigned i = 1; i <= rowsCount; i++) {
    keyData[i].setRow(startRow + i - 1);
    keyData[i].Hash = address;
    keySlices[i] = rocksdb::Slice(reinterpret_cast<const char*>(&keyData[i]), sizeof(TxKey));
  }

  auto queryStatus = db->MultiGet(rocksdb::ReadOptions(), keySlices, &queryResult);

  if (queryStatus[0].ok()) {
    const Value *value = reinterpret_cast<const Value*>(queryResult[0].data());
    dbBatch = value->BatchId;
    dbTxCount = value->TransactionsNum;
  }

  if (shardData.BatchId > dbBatch) {
    std::shared_lock lock(shardData.Mutex);
    auto It = shardData.Cache.find(address);
    if (It != shardData.Cache.end()) {
      CachedValue &value = It->second;
      const BC::Proto::TxHashTy *cachedHashes = value.TxData.data<BC::Proto::TxHashTy>();
      size_t cachedTxNum = value.TxData.sizeOf() / sizeof(BC::Proto::TxHashTy);
      size_t cachedTxOffset = value.TransactionsNum - cachedTxNum;
      dbTxCount -= cachedTxOffset;

      // Copy txid from disk storage
      current += extractTx(result, queryResult, queryStatus, from, count, dbTxCount);

      if (current >= dbTxCount) {
        // Copy txid from cache
        size_t limit = std::min(from+count, dbTxCount + cachedTxNum);
        while (current < limit) {
          result.push_back(cachedHashes[current - dbTxCount]);
          current++;
        }
      }
    } else {
      // Use tx from disk storage only
      extractTx(result, queryResult, queryStatus, from, count, dbTxCount);
    }
  } else {
    // Use tx from disk storage only
    extractTx(result, queryResult, queryStatus, from, count, dbTxCount);
  }
}

size_t BalanceDb::extractTx(std::vector<BC::Proto::TxHashTy> &out,
                            const std::vector<std::string> &data,
                            const std::vector<rocksdb::Status> &statuses,
                            uint64_t from,
                            uint32_t count,
                            uint64_t limit)
{
  size_t extractedTxNum = 0;
  uint64_t end = std::min(from+count, limit);
  uint64_t remaining = subNoOverflow(end, from);
  uint32_t offset = from % TransactionRowSize;
  for (size_t i = 1, ie = data.size(); i != ie; i++) {
    if (!statuses[i].ok())
      break;

    const TxValue *value = reinterpret_cast<const TxValue*>(data[i].data());
    uint32_t availableTx = subNoOverflow(value->txNum(data[i].size()), offset);
    uint32_t copySize = static_cast<uint32_t>(std::min(remaining, static_cast<uint64_t>(availableTx)));

    for (uint32_t j = 0; j < copySize; j++)
      out.push_back(value->hashes[offset+j]);

    offset = 0;
    remaining -= copySize;
    extractedTxNum += copySize;
  }

  return extractedTxNum;
}

void BalanceDb::mergeTx(rocksdb::WriteBatch &out, const char *address, uint64_t offset, const BC::Proto::TxHashTy *data, size_t txNum)
{
  TxKey key;
  TxValue value;
  const BC::Proto::TxHashTy *ptr = data;

  memcpy(key.Hash.begin(), address, sizeof(BC::Proto::AddressTy));

  uint64_t end = offset + txNum;
  uint64_t row = offset / TransactionRowSize;
  value.offset = offset % TransactionRowSize;
  while (offset < end) {
    uint32_t currentTxNum;
    currentTxNum = static_cast<uint32_t>(std::min(end-offset, static_cast<uint64_t>(TransactionRowSize)));
    currentTxNum = std::min(currentTxNum, TransactionRowSize - value.offset);

    key.setRow(row);

    memcpy(value.hashes, ptr, currentTxNum*sizeof(BC::Proto::TxHashTy));

    out.Merge(rocksdb::Slice(reinterpret_cast<const char*>(&key), sizeof(key)),
              rocksdb::Slice(reinterpret_cast<const char*>(&value), TxValue::dataSize(currentTxNum)));

    ptr += currentTxNum;
    offset += currentTxNum;
    value.offset = 0;
    row++;
  }
}

void BalanceDb::flush(unsigned shardNum)
{
  if (!LastAdded_)
    return;

  Shard &shardData = ShardData_[shardNum];

  rocksdb::DB *db = Databases_[shardNum].get();

  size_t keysNum = shardData.Cache.size();
  std::vector<rocksdb::Slice> keys;
  std::vector<const CachedValue*> cachedValues;
  std::vector<std::string> values;
  keys.resize(keysNum);
  cachedValues.resize(keysNum);
  size_t i = 0;
  for (const auto &addrDeltaPair: shardData.Cache) {
    keys[i] = rocksdb::Slice(reinterpret_cast<const char*>(addrDeltaPair.first.begin()), sizeof(BC::Proto::AddressTy));
    cachedValues[i] = &addrDeltaPair.second;
    i++;
  }

  auto status = db->MultiGet(rocksdb::ReadOptions(), keys, &values);

  rocksdb::WriteBatch batch;

  for (size_t i = 0; i < keysNum; i++) {
    size_t incomingTxNum = cachedValues[i]->TxData.sizeOf() / sizeof(BC::Proto::TxHashTy);
    const BC::Proto::TxHashTy *incomingTxData = cachedValues[i]->TxData.data<BC::Proto::TxHashTy>();

    if (status[i].ok()) {
      Value *dbValue = reinterpret_cast<Value*>(values[i].data());
      assert(shardData.BatchId > dbValue->BatchId);

      dbValue->BatchId = shardData.BatchId;
      dbValue->TransactionsNum += cachedValues[i]->TransactionsNum;
      dbValue->TotalSent += cachedValues[i]->TotalSent;
      dbValue->TotalReceived += cachedValues[i]->TotalReceived;

      batch.Put(keys[i], rocksdb::Slice(reinterpret_cast<const char*>(dbValue), sizeof(Value)));
      mergeTx(batch, keys[i].data(), dbValue->TransactionsNum - incomingTxNum, incomingTxData, incomingTxNum);
    } else {
      // First occurence of address
      assert(cachedValues[i]->BatchId == shardData.BatchId);
      batch.Put(keys[i], rocksdb::Slice(reinterpret_cast<const char*>(cachedValues[i]), sizeof(Value)));
      mergeTx(batch, keys[i].data(), 0, incomingTxData, incomingTxNum);
    }
  }

  {
    Stamp stamp;
    stamp.Hash = LastAdded_->Header.GetHash();
    stamp.BatchId = shardData.BatchId;
    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(&stamp), sizeof(stamp)));
  }

  db->Write(rocksdb::WriteOptions(), &batch);

  {
    std::lock_guard lock(shardData.Mutex);
    shardData.Cache.clear();
    shardData.BatchId++;
  }
}

}
}
