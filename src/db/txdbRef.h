// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "db/queries.h"
#include "db/chaindb.h"
#include "dbengine/kvbase.h"

namespace config4cpp {
class Configuration;
}

namespace BC {
namespace DB {

class Archive;

class TxDbRef :
  public CChainDb<dbengine::CKvBase<BC::Proto::TxHashTy>>,
  public ITransactionDb {
public:
  static constexpr unsigned MinimalBatchSize = 8192;

public:
  TxDbRef() : CChainDb<dbengine::CKvBase<BC::Proto::TxHashTy>>("txdb.ref") {}
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

  void connect(CBlockBatch batch,
               BlockInMemoryIndex &blockIndex,
               BlockDatabase &blockDb) final;

  void disconnect(const BC::Common::BlockIndex *index,
                  const BC::Proto::Block &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  const BC::Proto::CBlockValidationData &validationData,
                  BlockInMemoryIndex &blockIndex,
                  BlockDatabase &blockDb) final;

private:
  // The height and not the block hash: this database only ever holds rows of the
  // connected chain (disconnect erases them), so a height names one block
  struct CLogData {
    uint32_t Height;
    uint32_t Index;
    uint32_t SerializedDataOffset;
    uint32_t SerializedDataSize;
  };

  uint32_t version() final { return 1; }
  bool initializeImpl(config4cpp::Configuration *cfg);
};

}
}
