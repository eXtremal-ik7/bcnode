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

// The connect walk. Honours the run pair marks: an output spent inside the
// run is invisible outside it, so neither the log nor the cache ever sees it
void UTXODb::connectImpl(CBlockBatch batch, BlockInMemoryIndex&, BlockDatabase&)
{
  for (const CBlockRef &ref: batch) {
    const BC::Proto::Block &block = *ref.Block;
    const BC::Proto::CBlockValidationData &validationData = *ref.ValidationData;
    assert(validationData.TxIds.size() == block.vtx.size());
    const uint32_t height = ref.Index->Height;

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
    for (size_t i = 0; i < block.vtx.size(); i++) {
      const auto &tx = block.vtx[i];
      const bool isCoinbase = i == 0;

      // txin in coinbase can't spent anything
      if (!isCoinbase) {
        for (size_t j = 0; j < tx.txIn.size(); j++, inOrdinal++) {
          if (validationData.InputLocalTx[inOrdinal] != BC::Proto::CBlockValidationData::NoLocalTx ||
              validationData.inputSpendsInBatch(inOrdinal))
            continue;
          const auto &txIn = tx.txIn[j];
          key.Tx = txIn.previousOutputHash;
          key.Index = txIn.previousOutputIndex;
          this->erase(key);
          cacheRemove(key);
        }
      }

      const uint32_t packed = packHeight(height, isCoinbase);
      // A coinbase below BIP34 may repeat an earlier one and land on its live coin.
      // Such a write forfeits window annihilation: the key may already exist below, and
      // a later spend annihilated inside the window would leave the older value there
      // as a live coin nobody can spend
      const bool mayRepeat = isCoinbase && (validationData.CoinbaseRepeat || validationData.CoinbaseMayRepeat);
      key.Tx = validationData.TxIds[i];
      for (size_t j = 0; j < tx.txOut.size(); j++, outOrdinal++) {
        if (validationData.outputSpentLocally(outOrdinal) || validationData.outputSpentInBatch(outOrdinal))
          continue;
        size_t infoSize;
        const void *info = validationData.outputData(outOrdinal, infoSize);
        if (infoSize) {
          key.Index = static_cast<uint32_t>(j);
          if (mayRepeat)
            this->putRestore(key, info, infoSize, &packed, sizeof(packed));
          else
            this->putNew(key, info, infoSize, &packed, sizeof(packed));
          cacheAdd(key, info, infoSize, height, isCoinbase);
        }
      }
    }
    assert(inOrdinal == validationData.InputLocalTx.size());
    assert((outOrdinal + 63) / 64 == validationData.OutputSpentLocally.size());
  }
}

// The disconnect walk. It does not honour the run pair marks: a same-block
// pair was never connected, so neither side is undone, but a run pair is where
// the hiding ends - the input puts the output back although the connect never
// took it away, and the marks are dropped right after, so from here both
// blocks are plain
void UTXODb::disconnectImpl(const BC::Common::BlockIndex *index,
                            const BC::Proto::Block &block,
                            const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            const BC::Proto::CBlockValidationData &validationData,
                            BlockInMemoryIndex&,
                            BlockDatabase&)
{
  assert(validationData.TxIds.size() == block.vtx.size());
  assert(linkedOutputs.Tx.size() == block.vtx.size());
  // The creation height of a restored output is unknown here; the height of
  // the disconnected block is an upper bound (and its coinbase flag is
  // unknowable, but a coinbase spend sits 100+ blocks below any reorg). It
  // skews eviction aging and maturity metadata of reorged spends only
  const uint32_t height = index->Height;
  const uint32_t packed = packHeight(height, false);

  if (Cache_.enabled())
    Cache_.maintain();

  CUnspentOutputKey key;
  size_t outOrdinal = 0;
  size_t inOrdinal = 0;
  for (size_t i = 0; i < block.vtx.size(); i++) {
    const auto &tx = block.vtx[i];

    // txin in coinbase can't spent anything
    if (i != 0) {
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
        // The coin this input spent was created by a block below and may well
        // be there on disk: a later spend of it must leave a real tombstone
        this->putRestore(key, linkedTxin.data(), linkedTxin.size(), &packed, sizeof(packed));
        cacheAdd(key, linkedTxin.data(), linkedTxin.size(), height, false);
      }
    }

    key.Tx = validationData.TxIds[i];
    for (size_t j = 0; j < tx.txOut.size(); j++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal))
        continue;
      size_t infoSize;
      validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(j);
        this->erase(key);
        cacheRemove(key);
      }
    }
  }
  assert(inOrdinal == validationData.InputLocalTx.size());
  assert((outOrdinal + 63) / 64 == validationData.OutputSpentLocally.size());
}

}
}
