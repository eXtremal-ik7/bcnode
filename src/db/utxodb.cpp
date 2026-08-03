// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "utxodb.h"
#include "db/swmrdump.h"
#include "common/smallStream.h"
#include "loguru.hpp"
#include <chrono>
#include <thread>

namespace BC {
namespace DB {

static const char *CacheDumpFileName = "cache.dat";

// the raw on-disk key is Tx immediately followed by Index; the field-wise
// copy in warmupFromDb() relies on it
static_assert(sizeof(CUnspentOutputKey) == sizeof(BC::Proto::TxHashTy) + sizeof(uint32_t),
              "unexpected padding in CUnspentOutputKey");

// (creationHeight << 1) | isCoinbase, appended to every on-disk value
static inline uint32_t packHeight(uint32_t height, bool isCoinbase)
{
  return (height << 1) | (isCoinbase ? 1 : 0);
}

// The cache is mutated synchronously with every connect/disconnect
// (including the fast log-pop paths), so it never holds a spent output: a
// positive needs no cross-check against the shard log
bool UTXODb::query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xvector<uint8_t> &result, bool cacheOnly) const
{
  if (Cache_.enabled()) {
    if (Cache_.lookupConcurrent(txid.begin(), txoutIdx, [&result](const CUtxoCacheValue &value) {
          result.resize(sizeof(value.Data));
          memcpy(result.begin(), value.Data, sizeof(value.Data));
        }))
      return true;
    if (cacheOnly)
      return false; // the miss is resolved by the serial contextual pass
  }

  CUnspentOutputKey key;
  key.Tx = txid;
  key.Index = txoutIdx;
  return this->find(key, [&result](const void *d, size_t s) {
    // strip the packed height suffix, consumers expect pure UnspentOutputInfo
    result.resize(s - sizeof(uint32_t));
    memcpy(result.begin(), d, s - sizeof(uint32_t));
  });
}

bool UTXODb::initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage&)
{
  // Threshold flushes leave the connect path to a background thread; the
  // escape hatch exists for A/B runs on the bench stand
  if (cfg->lookupBoolean("utxo", "asyncFlush", true))
    enableAsyncFlush();

  int cacheSizeMb = cfg->lookupInt("utxo", "cacheSizeMb", 0);
  if (cacheSizeMb <= 0)
    return true;

  unsigned hwThreads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;
  CacheDumpThreads_ = static_cast<unsigned>(cfg->lookupInt("utxo", "cacheDumpThreads", hwThreads));
  if (CacheDumpThreads_ == 0)
    CacheDumpThreads_ = 1;

  Cache_.init(CSwmrCache<CUtxoCacheValue>::limitForMemory(static_cast<size_t>(cacheSizeMb) << 20));
  LOG_F(INFO, "utxo cache: limit %zu entries, table %zu MB (faulted lazily)", Cache_.limit(), Cache_.memoryBytes() >> 20);

  if (CurrentBlock_.isNull())
    return true;

  // Warm start: the dump is accepted only at the exact database position
  std::filesystem::path dumpPath = CacheDir_ / CacheDumpFileName;
  if (CacheBlockIndex_ && std::filesystem::exists(dumpPath)) {
    // The stamp always resolves: initialize() has already rejected a
    // database whose stamp is not in the block index
    auto It = CacheBlockIndex_->blockIndex().find(CurrentBlock_);
    if (It != CacheBlockIndex_->blockIndex().end()) {
      SSwmrDumpStamp stamp;
      stamp.Height = It->second->Height;
      memcpy(stamp.BlockHash, CurrentBlock_.begin(), sizeof(stamp.BlockHash));

      SSwmrDumpLoadOptions options;
      options.ValueVersion = version();
      options.Threads = CacheDumpThreads_;

      std::string error;
      auto startTime = std::chrono::steady_clock::now();
      if (swmrDumpLoad(Cache_, dumpPath, stamp, options, &error)) {
        double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() / 1000.0;
        LOG_F(INFO, "utxo cache: %zu entries loaded from dump (%.2lf seconds)", Cache_.size(), elapsed);
      } else {
        LOG_F(WARNING, "utxo cache: dump rejected: %s", error.c_str());
      }
    }
  }

  // No dump or a rejected one: warm up by scanning the database
  if (Cache_.size() == 0)
    warmupFromDb();

  return true;
}

void UTXODb::warmupFromDb()
{
  auto startTime = std::chrono::steady_clock::now();
  uint64_t scanned = 0;
  unsigned sinceMaintain = 0;

  for (size_t shardIdx = 0; shardIdx < BaseCfg_.ShardsNum; shardIdx++) {
    std::unique_ptr<rocksdb::Iterator> It(OnDiskStorage_[shardIdx]->NewIterator(rocksdb::ReadOptions()));
    for (It->SeekToFirst(); It->Valid(); It->Next()) {
      rocksdb::Slice keySlice = It->key();
      rocksdb::Slice valueSlice = It->value();
      // service records (stamp, base configuration) have short keys
      if (keySlice.size() != sizeof(CUnspentOutputKey) ||
          valueSlice.size() < sizeof(BC::Script::UnspentOutputInfo) + sizeof(uint32_t))
        continue;

      // field-wise copy: the key type is not trivially copyable, but the
      // on-disk layout is exactly Tx followed by Index (no padding)
      CUnspentOutputKey key;
      memcpy(key.Tx.begin(), keySlice.data(), sizeof(BC::Proto::TxHashTy));
      memcpy(&key.Index, keySlice.data() + sizeof(BC::Proto::TxHashTy), sizeof(uint32_t));
      uint32_t packed;
      memcpy(&packed, valueSlice.data() + valueSlice.size() - sizeof(uint32_t), sizeof(uint32_t));
      cacheAdd(key, valueSlice.data(), valueSlice.size() - sizeof(uint32_t), packed >> 1, packed & 1);
      scanned++;

      // the floor eviction keeps the newest entries as the scan streams by
      if (++sinceMaintain == 4096) {
        Cache_.maintain();
        sinceMaintain = 0;
      }
    }
  }

  Cache_.maintain();
  double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() / 1000.0;
  LOG_F(INFO,
        "utxo cache: warmup scanned %llu utxos, cached %zu with height floor %u (%.2lf seconds)",
        static_cast<unsigned long long>(scanned),
        Cache_.size(),
        Cache_.floorHeight(),
        elapsed);
}

void UTXODb::saveCache()
{
  if (!Cache_.enabled() || !CacheBlockIndex_ || CurrentBlock_.isNull())
    return;

  auto It = CacheBlockIndex_->blockIndex().find(CurrentBlock_);
  if (It == CacheBlockIndex_->blockIndex().end())
    return;

  SSwmrDumpStamp stamp;
  stamp.Height = It->second->Height;
  memcpy(stamp.BlockHash, CurrentBlock_.begin(), sizeof(stamp.BlockHash));

  SSwmrDumpSaveOptions options;
  options.ValueVersion = version();
  options.Threads = CacheDumpThreads_;

  std::string error;
  auto startTime = std::chrono::steady_clock::now();
  if (swmrDumpSave(Cache_, CacheDir_ / CacheDumpFileName, stamp, options, &error)) {
    double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() / 1000.0;
    LOG_F(INFO, "utxo cache: %zu entries saved to dump (%.2lf seconds)", Cache_.size(), elapsed);
  } else {
    LOG_F(WARNING, "utxo cache: dump not saved: %s", error.c_str());
  }
}

void UTXODb::connectImpl(const BC::Common::BlockIndex *index,
                         const BC::Proto::Block &block,
                         const BC::Proto::CBlockLinkedOutputs&,
                         const BC::Proto::CBlockValidationData &validationData,
                         BlockInMemoryIndex&,
                         BlockDatabase&)
{
  const auto blockId = index->Header.GetHash();
  const uint32_t height = index->Height;
  assert(validationData.TxIds.size() == block.vtx.size());

  if (Cache_.enabled())
    Cache_.maintain();

  CUnspentOutputKey key;
  // An output spent by a later tx of the same block (and that input itself)
  // is skipped entirely: the pair is invisible outside its block, so neither
  // the log nor the cache ever sees it. A pair spanning two blocks of one run
  // is skipped the same way - the run connects as one operation, and the
  // disconnect that splits it puts the output back
  size_t outOrdinal = 0;
  size_t inOrdinal = 0;
  // Coinbase
  {
    // txin in coinbase can't spent anything
    const auto &coinbaseTx = block.vtx[0];
    const uint32_t packed = packHeight(height, true);
    key.Tx = validationData.TxIds[0];
    for (size_t i = 0; i < coinbaseTx.txOut.size(); i++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal) || validationData.outputSpentInBatch(outOrdinal))
        continue;
      size_t infoSize;
      const void *info = validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(i);
        this->add(blockId, key, info, infoSize, &packed, sizeof(packed));
        cacheAdd(key, info, infoSize, height, true);
      }
    }
  }

  // Other transactions
  const uint32_t packed = packHeight(height, false);
  for (size_t i = 1; i < block.vtx.size(); i++) {
    const auto &tx = block.vtx[i];

    for (size_t j = 0; j < tx.txIn.size(); j++, inOrdinal++) {
      if (validationData.InputLocalTx[inOrdinal] != BC::Proto::CBlockValidationData::NoLocalTx ||
          validationData.inputSpendsInBatch(inOrdinal))
        continue;
      const auto &txIn = tx.txIn[j];
      key.Tx = txIn.previousOutputHash;
      key.Index = txIn.previousOutputIndex;
      this->remove(blockId, key);
      cacheRemove(key);
    }

    key.Tx = validationData.TxIds[i];
    for (size_t j = 0; j < tx.txOut.size(); j++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal) || validationData.outputSpentInBatch(outOrdinal))
        continue;
      size_t infoSize;
      const void *info = validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(j);
        this->add(blockId, key, info, infoSize, &packed, sizeof(packed));
        cacheAdd(key, info, infoSize, height, false);
      }
    }
  }
  assert(inOrdinal == validationData.InputLocalTx.size());
  assert((outOrdinal + 63) / 64 == validationData.OutputSpentLocally.size());
}

void UTXODb::disconnectImpl(const BC::Common::BlockIndex *index,
                            const BC::Proto::Block &block,
                            const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            const BC::Proto::CBlockValidationData &validationData,
                            BlockInMemoryIndex&,
                            BlockDatabase&)
{
  const auto blockId = index->Header.GetHash();
  assert(validationData.TxIds.size() == block.vtx.size());
  // The creation height of a restored output is unknown here; the height of
  // the disconnected block is an upper bound (and its coinbase flag is
  // unknowable, but a coinbase spend sits 100+ blocks below any reorg). It
  // skews eviction aging and maturity metadata of reorged spends only
  const uint32_t height = index->Height;
  const uint32_t packed = packHeight(height, false);

  if (Cache_.enabled())
    Cache_.maintain();

  CUnspentOutputKey key;
  // A same-block pair was never connected, so neither side is undone. A run pair is where the
  // hiding ends: the input puts the output back although the connect never took it away, and the
  // marks are dropped right after, so from here both blocks are plain
  size_t outOrdinal = 0;
  size_t inOrdinal = 0;
  // Coinbase
  {
    // txin in coinbase can't spent anything
    const auto &coinbaseTx = block.vtx[0];
    key.Tx = validationData.TxIds[0];
    for (size_t i = 0; i < coinbaseTx.txOut.size(); i++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal))
        continue;
      size_t infoSize;
      validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(i);
        this->remove(blockId, key);
        cacheRemove(key);
      }
    }
  }

  // Other transactions
  assert(linkedOutputs.Tx.size() == block.vtx.size());

  for (size_t i = 1; i < block.vtx.size(); i++) {
    const auto &tx = block.vtx[i];
    const auto &linkedTx = linkedOutputs.Tx[i];
    assert(linkedTx.TxIn.size() == tx.txIn.size());

    for (size_t j = 0; j < tx.txIn.size(); j++, inOrdinal++) {
      if (validationData.InputLocalTx[inOrdinal] != BC::Proto::CBlockValidationData::NoLocalTx)
        continue;
      const auto &txIn = tx.txIn[j];
      const auto &linkedTxin = linkedTx.TxIn[j];

      assert(linkedTxin.size() >= sizeof(BC::Script::UnspentOutputInfo));

      key.Tx = txIn.previousOutputHash;
      key.Index = txIn.previousOutputIndex;
      this->add(blockId, key, linkedTxin.data(), linkedTxin.size(), &packed, sizeof(packed));
      cacheAdd(key, linkedTxin.data(), linkedTxin.size(), height, false);
    }

    key.Tx = validationData.TxIds[i];
    for (size_t j = 0; j < tx.txOut.size(); j++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal))
        continue;
      size_t infoSize;
      validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(j);
        this->remove(blockId, key);
        cacheRemove(key);
      }
    }
  }
  assert(inOrdinal == validationData.InputLocalTx.size());
  assert((outOrdinal + 63) / 64 == validationData.OutputSpentLocally.size());
}

void UTXODb::connectFastImpl(const BC::Common::BlockIndex *index,
                             const BC::Proto::Block &block,
                             const BC::Proto::CBlockLinkedOutputs&,
                             const BC::Proto::CBlockValidationData &validationData)
{
  assert(validationData.TxIds.size() == block.vtx.size());
  if (!Cache_.enabled())
    return;

  // The log connect happened as a pop of the pending disconnect; mirror it
  // for the cache, which has no pop
  const uint32_t height = index->Height;

  Cache_.maintain();

  CUnspentOutputKey key;
  // Mirrors connectImpl: the cache never saw either pair kind. Reached only with the marks
  // already dropped - a block that hides pairs never takes the pop path (see CBaseDb::disconnect)
  size_t outOrdinal = 0;
  size_t inOrdinal = 0;
  {
    const auto &coinbaseTx = block.vtx[0];
    key.Tx = validationData.TxIds[0];
    for (size_t i = 0; i < coinbaseTx.txOut.size(); i++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal) || validationData.outputSpentInBatch(outOrdinal))
        continue;
      size_t infoSize;
      const void *info = validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(i);
        cacheAdd(key, info, infoSize, height, true);
      }
    }
  }

  for (size_t i = 1; i < block.vtx.size(); i++) {
    const auto &tx = block.vtx[i];

    for (size_t j = 0; j < tx.txIn.size(); j++, inOrdinal++) {
      if (validationData.InputLocalTx[inOrdinal] != BC::Proto::CBlockValidationData::NoLocalTx)
        continue;
      key.Tx = tx.txIn[j].previousOutputHash;
      key.Index = tx.txIn[j].previousOutputIndex;
      cacheRemove(key);
    }

    key.Tx = validationData.TxIds[i];
    for (size_t j = 0; j < tx.txOut.size(); j++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal) || validationData.outputSpentInBatch(outOrdinal))
        continue;
      size_t infoSize;
      const void *info = validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(j);
        cacheAdd(key, info, infoSize, height, false);
      }
    }
  }
  assert(inOrdinal == validationData.InputLocalTx.size());
  assert((outOrdinal + 63) / 64 == validationData.OutputSpentLocally.size());
}

void UTXODb::disconnectFastImpl(const BC::Common::BlockIndex *index,
                                const BC::Proto::Block &block,
                                const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                                const BC::Proto::CBlockValidationData &validationData)
{
  assert(validationData.TxIds.size() == block.vtx.size());
  if (!Cache_.enabled())
    return;

  // The log disconnect happened as a pop of the pending connect; mirror it
  // for the cache, which has no pop
  const uint32_t height = index->Height;

  Cache_.maintain();

  CUnspentOutputKey key;
  // Exact inverse of connectImpl: what the connect never wrote must not be undone
  size_t outOrdinal = 0;
  size_t inOrdinal = 0;
  {
    const auto &coinbaseTx = block.vtx[0];
    key.Tx = validationData.TxIds[0];
    for (size_t i = 0; i < coinbaseTx.txOut.size(); i++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal) || validationData.outputSpentInBatch(outOrdinal))
        continue;
      size_t infoSize;
      validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(i);
        cacheRemove(key);
      }
    }
  }

  assert(linkedOutputs.Tx.size() == block.vtx.size());

  for (size_t i = 1; i < block.vtx.size(); i++) {
    const auto &tx = block.vtx[i];
    const auto &linkedTx = linkedOutputs.Tx[i];
    assert(linkedTx.TxIn.size() == tx.txIn.size());

    for (size_t j = 0; j < tx.txIn.size(); j++, inOrdinal++) {
      if (validationData.InputLocalTx[inOrdinal] != BC::Proto::CBlockValidationData::NoLocalTx ||
          validationData.inputSpendsInBatch(inOrdinal))
        continue;
      const auto &linkedTxin = linkedTx.TxIn[j];
      key.Tx = tx.txIn[j].previousOutputHash;
      key.Index = tx.txIn[j].previousOutputIndex;
      cacheAdd(key, linkedTxin.data(), linkedTxin.size(), height, false);
    }

    key.Tx = validationData.TxIds[i];
    for (size_t j = 0; j < tx.txOut.size(); j++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal) || validationData.outputSpentInBatch(outOrdinal))
        continue;
      size_t infoSize;
      validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(j);
        cacheRemove(key);
      }
    }
  }
  assert(inOrdinal == validationData.InputLocalTx.size());
  assert((outOrdinal + 63) / 64 == validationData.OutputSpentLocally.size());
}

}
}
