// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "dbengine/keyHash.h"
#include "db/chaindb.h"
#include "dbengine/kvbase.h"
#include "db/outpointKey.h"
#include "dbengine/swmrcache.h"
#include "BC/script.h"

// The key is the outpoint shared with spentdb; the name says what it means here
using CUnspentOutputKey = COutpointKey;

namespace BC {
namespace DB {

// Coin metadata beside every output: maturity, BIP68 and the age rule of
// readers that lag behind the database
enum EUtxoFlag : uint8_t {
  EUtxoCoinbase = 1,
  // An MWEB peg-out: a HogEx output past the first (the HogAddr)
  EUtxoPegout = 2,
  // Put back by a disconnect, so it may be spent on the branch a lagging reader stands on
  EUtxoRestored = 4
};

// MWEB peg-out maturity; the flag exists only on chains with an extension block
static constexpr uint32_t UtxoPegoutMaturity = 6;

// The coin word: (creationHeight << 3) | EUtxoFlag. The on-disk suffix and the
// linked outputs carry it as is
static constexpr unsigned UtxoFlagBits = 3;
static inline uint32_t utxoMeta(uint32_t height, unsigned flags) { return (height << UtxoFlagBits) | flags; }
static inline uint32_t utxoMetaHeight(uint32_t meta) { return meta >> UtxoFlagBits; }
static inline uint8_t utxoMetaFlags(uint32_t meta) { return static_cast<uint8_t>(meta & ((1u << UtxoFlagBits) - 1)); }

// Flags an output of tx (txIdx in its block) is created with
template<typename CTransactionTy>
static inline unsigned utxoCreationFlags(const CTransactionTy &tx, size_t txIdx, size_t outIdx)
{
  unsigned flags = txIdx == 0 ? EUtxoCoinbase : 0;
  if constexpr (requires { tx.HogEx; }) {
    // The HogAddr is spent by the next HogEx and is no peg-out
    if (tx.HogEx && outIdx > 0)
      flags |= EUtxoPegout;
  }
  return flags;
}

// Read-cache entry: serialized CUnspentOutputInfo bytes stored inline. Every
// fixed-layout output type (incl. the bare multisig identity) is exactly
// sizeof(CUnspentOutputInfo); longer values (uncompressed P2PK, non-standard
// scripts) are not cached at all - a miss is always legal, a positive must
// be exact
struct CUtxoCacheValue {
  uint32_t Height; // creation height, drives the eviction floor
  uint8_t Flags;   // EUtxoFlag, mirrors the on-disk suffix; rides in former padding
  uint8_t Data[sizeof(BC::Script::CUnspentOutputInfo)];
};

// A value is CUnspentOutputInfo bytes followed by the uint32 coin word
// (utxoMeta). On disk the record goes in the stored form of linked outputs
// (BTC/proto.h), the word as it is; everything above the disk - the layers,
// the cache, query() - sees the record whole. query() hands the word out apart
struct CUtxoValueCodec {
  static void pack(const void *data, size_t size, xmstream &out);
  static bool unpack(const void *data, size_t size, xvector<uint8_t> &out);
};

class UTXODb : public CChainDb<dbengine::CKvBase<CUnspentOutputKey, CUtxoValueCodec>> {
public:
  UTXODb() : CChainDb<dbengine::CKvBase<CUnspentOutputKey, CUtxoValueCodec>>("utxo") {}
  virtual ~UTXODb() {}
  void *interface(int) final { return nullptr; }
  // Seqlock cache probe, then shard logs and RocksDB. Concurrent loaders
  // running ahead of connect pass cacheOnly: the miss (usually an output of
  // a still-unconnected block) is legal and resolved by the serial
  // contextual pass; a full search from workers measured 2-3% of reindex
  // wall in negative RocksDB gets. Cache disabled: falls back to the db.
  // meta receives the coin word
  bool query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xvector<uint8_t> &result, uint32_t &meta, bool cacheOnly = false) const;
  bool cacheEnabled() const { return Cache_.enabled(); }

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

  void connect(CBlockBatch batch,
               BlockInMemoryIndex &blockIndex,
               BlockDatabase &blockDb) final;

  void disconnect(const BC::Common::BlockIndex *index,
                  const BC::Proto::CBlock &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  const BC::Proto::CBlockValidationData &validationData,
                  BlockInMemoryIndex &blockIndex,
                  BlockDatabase &blockDb) final;

  // The change sequence: blocks connected and disconnected so far, stored once at the start of
  // each operation, before its first change. Whoever saw any change of operation k - in the
  // cache or in a revision - and loads the sequence after that gets k at least: a reader that
  // lags behind (the mempool) learns how far the database may have moved under its probes
  uint64_t changeSeq() const {
    std::atomic_thread_fence(std::memory_order_acquire);
    return ChangeSeq_.load(std::memory_order_relaxed);
  }

  // The published revision: the block it stands at and its number in the change sequence, from
  // one view, so the two always belong together
  struct CRevision {
    BaseBlob<256> Stamp;
    uint64_t Seq;
  };

  CRevision revision() const {
    dbengine::CKvGuard<CUnspentOutputKey> guard = Engine_.guard();
    return {guard.view()->Stamp, guard.view()->Seq};
  }

private:
  // An operation of 'blocks' blocks begins: the store goes before any change it makes
  uint64_t beginChange(uint64_t blocks) {
    const uint64_t seq = ChangeSeq_.load(std::memory_order_relaxed) + blocks;
    ChangeSeq_.store(seq, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    return seq;
  }

  uint32_t version() final { return 1; }
  bool initializeImpl(config4cpp::Configuration *cfg) override;

  // No-dump warmup: one streaming pass over the shards, inserting every
  // record with its true creation height; the floor eviction keeps roughly
  // the newest limit() entries whatever the iteration order is
  void warmupFromDb();

  // Only exact-width values are admitted, so both copies (into the claimed
  // slot here and out of it in query) run at constant size
  void cacheAdd(const CUnspentOutputKey &key, const void *data, size_t size, uint32_t meta) {
    if (!Cache_.enabled() || size != sizeof(CUtxoCacheValue::Data))
      return;
    const uint32_t height = utxoMetaHeight(meta);
    Cache_.insertWith(key.Tx.begin(), key.Index, height, [data, height, meta](CUtxoCacheValue &value) {
      value.Height = height;
      value.Flags = utxoMetaFlags(meta);
      memcpy(value.Data, data, sizeof(value.Data));
    });
  }

  void cacheRemove(const CUnspentOutputKey &key) {
    if (Cache_.enabled())
      Cache_.spend(key.Tx.begin(), key.Index, [](const CUtxoCacheValue&) {});
  }

private:
  dbengine::CSwmrCache<CUtxoCacheValue> Cache_;
  // One writer, the thread connecting and disconnecting blocks
  std::atomic<uint64_t> ChangeSeq_{0};
  std::filesystem::path CacheDir_;
  BlockInMemoryIndex *CacheBlockIndex_ = nullptr;
  unsigned CacheDumpThreads_ = 1;
};

}
}
