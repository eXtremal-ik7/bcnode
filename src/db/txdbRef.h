// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"

namespace config4cpp {
class Configuration;
}

namespace BC {
namespace DB {

class Archive;

class TxDbRef :
  public CBaseKV<BC::Proto::TxHashTy>,
  public ITransactionDb {
public:
  static constexpr unsigned MinimalBatchSize = 8192;

public:
  TxDbRef() : CBaseKV<BC::Proto::TxHashTy>("txdb.ref") {}
  virtual ~TxDbRef() {}

  void *interface(int interface) {
    switch (interface) {
      case EIQueryTransaction : return static_cast<ITransactionDb*>(this);
      default: return nullptr;
    }
  }

  bool queryTransaction(const BC::Proto::TxHashTy &txid,
                        BlockInMemoryIndex &blockIndex,
                        BlockDatabase &blockDb,
                        CQueryTransactionResult &result);

  // TODO: remove it, use full block index instead
  bool searchUnspentOutput(const BC::Proto::TxHashTy &tx,
                           uint32_t index,
                           BlockInMemoryIndex &blockIndex,
                           BlockDatabase &blockDb,
                           xmstream &result);

private:
  struct CLogData {
    BC::Proto::BlockHashTy Hash;
    uint32_t Index;
    uint32_t SerializedDataOffset;
    uint32_t SerializedDataSize;
  };

  uint32_t version() final { return 1; }
  bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage);
  void connectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb);
  void disconnectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb);
};

}
}
