// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "utxodb.h"
#include "dbengine/swmrdump.h"
#include "common/smallStream.h"
#include "loguru.hpp"
#include <chrono>
#include <thread>

namespace BC {
namespace DB {

static const char *CacheDumpFileName = "cache.dat";

void CUtxoValueCodec::pack(const void *data, size_t size, xmstream &out)
{
  const uint8_t *bytes = static_cast<const uint8_t*>(data);
  const size_t recordSize = size - sizeof(uint32_t);
  uint32_t meta;
  memcpy(&meta, bytes + recordSize, sizeof(meta));

  BTC::Writer op{out};
  BC::Proto::CTxLinkedOutputs::writeOutput(op, bytes, recordSize);
  op.put(meta);
}

bool CUtxoValueCodec::unpack(const void *data, size_t size, xvector<uint8_t> &out)
{
  xmstream in(const_cast<void*>(data), size);
  Ser::CIoStatus status;
  BTC::Reader op(in, status);
  BC::Proto::CTxLinkedOutputs::readOutput(op, &out);
  uint32_t meta = 0;
  op.get(meta);
  if (status.Failed || in.eof() || in.remaining())
    return false;

  const size_t recordSize = out.size();
  out.resize(recordSize + sizeof(meta));
  memcpy(out.data() + recordSize, &meta, sizeof(meta));
  return true;
}

// The cache is mutated synchronously with every connect/disconnect
// (including the fast log-pop paths), so it never holds a spent output: a
// positive needs no cross-check against the shard log
bool UTXODb::query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xvector<uint8_t> &result, uint32_t &meta, bool cacheOnly) const
{
  if (Cache_.enabled()) {
    if (Cache_.lookupConcurrent(txid.begin(), txoutIdx, [&result, &meta](const CUtxoCacheValue &value) {
          result.resize(sizeof(value.Data));
          memcpy(result.begin(), value.Data, sizeof(value.Data));
          meta = utxoMeta(value.Height, value.Flags);
        }))
      return true;
    if (cacheOnly)
      return false; // the miss is resolved by the serial contextual pass
  }

  CUnspentOutputKey key;
  key.Tx = txid;
  key.Index = txoutIdx;
  return this->find(key, [&result, &meta](const void *d, size_t s) {
    // strip the coin word suffix, consumers expect pure CUnspentOutputInfo
    result.resize(s - sizeof(uint32_t));
    memcpy(result.begin(), d, s - sizeof(uint32_t));
    memcpy(&meta, static_cast<const uint8_t*>(d) + s - sizeof(uint32_t), sizeof(uint32_t));
  });
}

bool UTXODb::initializeImpl(config4cpp::Configuration *cfg)
{
  int cacheSizeMb = cfg->lookupInt("utxo", "cacheSizeMb", 0);
  if (cacheSizeMb <= 0)
    return true;

  unsigned hwThreads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;
  CacheDumpThreads_ = static_cast<unsigned>(cfg->lookupInt("utxo", "cacheDumpThreads", hwThreads));
  if (CacheDumpThreads_ == 0)
    CacheDumpThreads_ = 1;

  Cache_.init(dbengine::CSwmrCache<CUtxoCacheValue>::limitForMemory(static_cast<size_t>(cacheSizeMb) << 20));
  LOG_F(INFO, "utxo cache: limit %zu entries, table %zu MB (faulted lazily)", Cache_.limit(), Cache_.memoryBytes() >> 20);

  if (Stamp_.isNull())
    return true;

  // Warm start: the dump is accepted only at the exact database position
  std::filesystem::path dumpPath = CacheDir_ / CacheDumpFileName;
  if (CacheBlockIndex_ && std::filesystem::exists(dumpPath)) {
    // The stamp always resolves: initialize() has already rejected a
    // database whose stamp is not in the block index
    auto It = CacheBlockIndex_->blockIndex().find(Stamp_);
    if (It != CacheBlockIndex_->blockIndex().end()) {
      dbengine::SSwmrDumpStamp stamp;
      stamp.Height = It->second->Height;
      memcpy(stamp.BlockHash, Stamp_.begin(), sizeof(stamp.BlockHash));

      dbengine::SSwmrDumpLoadOptions options;
      options.ValueVersion = version();
      options.Threads = CacheDumpThreads_;

      std::string error;
      auto startTime = std::chrono::steady_clock::now();
      if (dbengine::swmrDumpLoad(Cache_, dumpPath, stamp, options, &error)) {
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

  xvector<uint8_t> value;
  for (size_t shardIdx = 0; shardIdx < BaseCfg_.ShardsNum; shardIdx++) {
    std::unique_ptr<rocksdb::Iterator> It(OnDiskStorage_[shardIdx]->NewIterator(rocksdb::ReadOptions()));
    for (It->SeekToFirst(); It->Valid(); It->Next()) {
      rocksdb::Slice keySlice = It->key();
      rocksdb::Slice valueSlice = It->value();
      // service records (stamp, base configuration) have short keys
      if (keySlice.size() != sizeof(CUnspentOutputKey) ||
          !CUtxoValueCodec::unpack(valueSlice.data(), valueSlice.size(), value))
        continue;

      // field-wise copy: the key type is not trivially copyable, but the
      // on-disk layout is exactly Tx followed by Index (no padding)
      CUnspentOutputKey key;
      memcpy(key.Tx.begin(), keySlice.data(), sizeof(BC::Proto::TxHashTy));
      memcpy(&key.Index, keySlice.data() + sizeof(BC::Proto::TxHashTy), sizeof(uint32_t));
      uint32_t meta;
      memcpy(&meta, value.data() + value.size() - sizeof(uint32_t), sizeof(uint32_t));
      cacheAdd(key, value.data(), value.size() - sizeof(uint32_t), meta);
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
  if (!Cache_.enabled() || !CacheBlockIndex_ || Stamp_.isNull())
    return;

  auto It = CacheBlockIndex_->blockIndex().find(Stamp_);
  if (It == CacheBlockIndex_->blockIndex().end())
    return;

  dbengine::SSwmrDumpStamp stamp;
  stamp.Height = It->second->Height;
  memcpy(stamp.BlockHash, Stamp_.begin(), sizeof(stamp.BlockHash));

  dbengine::SSwmrDumpSaveOptions options;
  options.ValueVersion = version();
  options.Threads = CacheDumpThreads_;

  std::string error;
  auto startTime = std::chrono::steady_clock::now();
  if (dbengine::swmrDumpSave(Cache_, CacheDir_ / CacheDumpFileName, stamp, options, &error)) {
    double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() / 1000.0;
    LOG_F(INFO, "utxo cache: %zu entries saved to dump (%.2lf seconds)", Cache_.size(), elapsed);
  } else {
    LOG_F(WARNING, "utxo cache: dump not saved: %s", error.c_str());
  }
}

// The connect walk. Honours the run pair marks: an output spent inside the
// run is invisible outside it, so neither the log nor the cache ever sees it
void UTXODb::connect(CBlockBatch batch, BlockInMemoryIndex&, BlockDatabase&)
{
  const uint64_t seq = beginChange(batch.size());
  dbengine::CKvWriter<CUnspentOutputKey> writer = liveWriter();
  for (const CBlockRef &ref: batch) {
    const BC::Proto::CBlock &block = *ref.Block;
    const BC::Proto::CBlockValidationData &validationData = *ref.ValidationData;
    assert(validationData.TxIds.size() == block.Vtx.size());
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
    for (size_t i = 0; i < block.Vtx.size(); i++) {
      const auto &tx = block.Vtx[i];
      const bool isCoinbase = i == 0;

      // txin in coinbase can't spent anything
      if (!isCoinbase) {
        for (size_t j = 0; j < tx.TxIn.size(); j++, inOrdinal++) {
          if (validationData.InputLocalTx[inOrdinal] != BC::Proto::CBlockValidationData::NoLocalTx ||
              validationData.inputSpendsInBatch(inOrdinal))
            continue;
          const auto &txIn = tx.TxIn[j];
          key.Tx = txIn.PreviousOutputHash;
          key.Index = txIn.PreviousOutputIndex;
          writer.erase(key);
          cacheRemove(key);
        }
      }

      // A coinbase below BIP34 may repeat an earlier one and land on its live coin.
      // Such a write forfeits window annihilation: the key may already exist below, and
      // a later spend annihilated inside the window would leave the older value there
      // as a live coin nobody can spend
      const bool mayRepeat = isCoinbase && (validationData.CoinbaseRepeat || validationData.CoinbaseMayRepeat);
      key.Tx = validationData.TxIds[i];
      for (size_t j = 0; j < tx.TxOut.size(); j++, outOrdinal++) {
        if (validationData.outputSpentLocally(outOrdinal) || validationData.outputSpentInBatch(outOrdinal))
          continue;
        size_t infoSize;
        const void *info = validationData.outputData(outOrdinal, infoSize);
        if (infoSize) {
          key.Index = static_cast<uint32_t>(j);
          const uint32_t meta = utxoMeta(height, utxoCreationFlags(tx, i, j));
          if (mayRepeat)
            writer.putRestore(key, info, infoSize, &meta, sizeof(meta));
          else
            writer.putNew(key, info, infoSize, &meta, sizeof(meta));
          cacheAdd(key, info, infoSize, meta);
        }
      }
    }
    assert(inOrdinal == validationData.InputLocalTx.size());
    assert((outOrdinal + 63) / 64 == validationData.OutputSpentLocally.size());
  }
  commit(writer, batch.back().Index->Header.GetHash(), seq);
}

// The disconnect walk. It does not honour the run pair marks: a same-block
// pair was never connected, so neither side is undone, but a run pair is where
// the hiding ends - the input puts the output back although the connect never
// took it away, and the marks are dropped right after, so from here both
// blocks are plain
void UTXODb::disconnect(const BC::Common::BlockIndex *index,
                            const BC::Proto::CBlock &block,
                            const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            const BC::Proto::CBlockValidationData &validationData,
                            BlockInMemoryIndex&,
                            BlockDatabase&)
{
  const uint64_t seq = beginChange(1);
  dbengine::CKvWriter<CUnspentOutputKey> writer = liveWriter();
  assert(validationData.TxIds.size() == block.Vtx.size());
  assert(linkedOutputs.Tx.size() == block.Vtx.size());
  assert(linkedOutputs.Meta.size() == validationData.InputLocalTx.size());

  if (Cache_.enabled())
    Cache_.maintain();

  CUnspentOutputKey key;
  size_t outOrdinal = 0;
  size_t inOrdinal = 0;
  for (size_t i = 0; i < block.Vtx.size(); i++) {
    const auto &tx = block.Vtx[i];

    // txin in coinbase can't spent anything
    if (i != 0) {
      const auto &linkedTx = linkedOutputs.Tx[i];
      assert(linkedTx.TxIn.size() == tx.TxIn.size());

      for (size_t j = 0; j < tx.TxIn.size(); j++, inOrdinal++) {
        if (validationData.InputLocalTx[inOrdinal] != BC::Proto::CBlockValidationData::NoLocalTx)
          continue;
        const auto &txIn = tx.TxIn[j];
        const auto &linkedTxin = linkedTx.TxIn[j];

        assert(linkedTxin.size() >= sizeof(BC::Script::CUnspentOutputInfo));

        key.Tx = txIn.PreviousOutputHash;
        key.Index = txIn.PreviousOutputIndex;
        // The coin this input spent was created by a block below and may well
        // be there on disk: a later spend of it must leave a real tombstone.
        // It goes back with its own height and flags, marked restored
        const uint32_t meta = linkedOutputs.Meta[inOrdinal] | EUtxoRestored;
        writer.putRestore(key, linkedTxin.data(), linkedTxin.size(), &meta, sizeof(meta));
        cacheAdd(key, linkedTxin.data(), linkedTxin.size(), meta);
      }
    }

    key.Tx = validationData.TxIds[i];
    for (size_t j = 0; j < tx.TxOut.size(); j++, outOrdinal++) {
      if (validationData.outputSpentLocally(outOrdinal))
        continue;
      size_t infoSize;
      validationData.outputData(outOrdinal, infoSize);
      if (infoSize) {
        key.Index = static_cast<uint32_t>(j);
        writer.erase(key);
        cacheRemove(key);
      }
    }
  }
  assert(inOrdinal == validationData.InputLocalTx.size());
  assert((outOrdinal + 63) / 64 == validationData.OutputSpentLocally.size());
  commit(writer, index->Header.HashPrevBlock, seq);
}

}
}
