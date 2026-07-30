// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "db/swmrcache.h"
#include "BTC/script.h"

struct CUnspentOutputKey {
  BC::Proto::TxHashTy Tx;
  uint32_t Index;

  friend bool operator==(const CUnspentOutputKey& a, const CUnspentOutputKey& b) { return a.Tx == b.Tx && a.Index == b.Index; }
};

template<>
class std::hash<CUnspentOutputKey> {
public:
  size_t operator()(const CUnspentOutputKey &key) const noexcept {
    return key.Tx.get64(0) + key.Tx.get64(1) * key.Index + key.Tx.get64(2) * key.Index + key.Tx.get64(3) * key.Index;
  }
};

namespace BC {
namespace DB {

// Read-cache entry: serialized UnspentOutputInfo bytes stored inline. Every
// fixed-layout output type (incl. the bare multisig identity) is exactly
// sizeof(UnspentOutputInfo); longer values (uncompressed P2PK, non-standard
// scripts) are not cached at all - a miss is always legal, a positive must
// be exact
struct CUtxoCacheValue {
  uint32_t Height;    // creation height, drives the eviction floor
  uint8_t IsCoinbase; // maturity metadata, mirrors the on-disk suffix bit; rides in former padding
  uint8_t Data[sizeof(BTC::Script::UnspentOutputInfo)];
};

// On-disk value: serialized UnspentOutputInfo followed by a uint32 suffix
// packing (creationHeight << 1) | isCoinbase, the Core-style coin metadata
// (coinbase maturity, BIP68, warmup scan by height). query() strips the
// suffix: every consumer above sees pure UnspentOutputInfo bytes
class UTXODb : public CBaseKV<CUnspentOutputKey> {
public:
  UTXODb() : CBaseKV<CUnspentOutputKey>("utxo") {}
  virtual ~UTXODb() {}
  void *interface(int) final { return nullptr; }
  // Seqlock cache probe, then shard logs and RocksDB. Concurrent loaders
  // running ahead of connect pass cacheOnly: the miss (usually an output of
  // a still-unconnected block) is legal and resolved by the serial
  // contextual pass; a full search from workers measured 2-3% of reindex
  // wall in negative RocksDB gets. Cache disabled: falls back to the db
  bool query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xvector<uint8_t> &result, bool cacheOnly = false) const;

  // Cache dump location and the block index resolving the stamp height;
  // must be called before initialize()
  void setupCache(const std::filesystem::path &dbPath, BlockInMemoryIndex &blockIndex) {
    CacheDir_ = dbPath;
    CacheBlockIndex_ = &blockIndex;
  }

  // Write the cache dump beside the shards; the call must follow the final
  // flush() of a quiescent database - the dump stamp is valid only while it
  // matches the database stamp
  void saveCache();

private:
  uint32_t version() final { return 1; }
  bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage);

  void connectImpl(const BC::Common::BlockIndex *index,
                   const BC::Proto::Block &block,
                   const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                   const BC::Proto::CBlockValidationData &validationData,
                   BlockInMemoryIndex &blockIndex,
                   BlockDatabase &blockDb);

  void disconnectImpl(const BC::Common::BlockIndex *index,
                      const BC::Proto::Block &block,
                      const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                      const BC::Proto::CBlockValidationData &validationData,
                      BlockInMemoryIndex &blockIndex,
                      BlockDatabase &blockDb);

  void connectFastImpl(const BC::Common::BlockIndex *index,
                       const BC::Proto::Block &block,
                       const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                       const BC::Proto::CBlockValidationData &validationData) final;

  void disconnectFastImpl(const BC::Common::BlockIndex *index,
                          const BC::Proto::Block &block,
                          const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                          const BC::Proto::CBlockValidationData &validationData) final;

  // No-dump warmup: one streaming pass over the shards, inserting every
  // record with its true creation height; the floor eviction keeps roughly
  // the newest limit() entries whatever the iteration order is
  void warmupFromDb();

  // Only exact-width values are admitted, so both copies (into the claimed
  // slot here and out of it in query) run at constant size
  void cacheAdd(const CUnspentOutputKey &key, const void *data, size_t size, uint32_t height, bool isCoinbase) {
    if (!Cache_.enabled() || size != sizeof(CUtxoCacheValue::Data))
      return;
    Cache_.insertWith(key.Tx.begin(), key.Index, height, [data, height, isCoinbase](CUtxoCacheValue &value) {
      value.Height = height;
      value.IsCoinbase = isCoinbase;
      memcpy(value.Data, data, sizeof(value.Data));
    });
  }

  void cacheRemove(const CUnspentOutputKey &key) {
    if (Cache_.enabled())
      Cache_.spend(key.Tx.begin(), key.Index, [](const CUtxoCacheValue&) {});
  }

private:
  CSwmrCache<CUtxoCacheValue> Cache_;
  std::filesystem::path CacheDir_;
  BlockInMemoryIndex *CacheBlockIndex_ = nullptr;
  unsigned CacheDumpThreads_ = 1;
};

}
}
