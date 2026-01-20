// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"

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

class UTXODb : public CBaseKV<CUnspentOutputKey> {
public:
  UTXODb() : CBaseKV<CUnspentOutputKey>("utxo") {}
  virtual ~UTXODb() {}
  void *interface(int) final { return nullptr; }
  // Thread safe, search utxo in cache; if it not found - look disk storage
  bool query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xvector<uint8_t> &result) const;
  // Thread safe, search utxo in cache only
  bool queryCache(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xvector<uint8_t> &result) const;

private:
  uint32_t version() final { return 1; }
  bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage);

  void connectImpl(const BC::Common::BlockIndex *index,
                   const BC::Proto::Block &block,
                   const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                   BlockInMemoryIndex &blockIndex,
                   BlockDatabase &blockDb);

  void disconnectImpl(const BC::Common::BlockIndex *index,
                      const BC::Proto::Block &block,
                      const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                      BlockInMemoryIndex &blockIndex,
                      BlockDatabase &blockDb);
};

}
}
