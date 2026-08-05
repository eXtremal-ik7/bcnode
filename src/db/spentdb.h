// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "db/outpointKey.h"

namespace config4cpp {
class Configuration;
}

namespace BC {
namespace DB {

// The back reference a transaction does not carry: which input of which
// transaction took a given output. Complement of utxodb over the same key
// space - an output the chain created is either there (unspent) or here
// (spent). Pure append on the write path: connect writes marks from data it
// already holds and reads nothing back
class SpentDb : public CBaseKV<COutpointKey>, public ISpentDb {
public:
  SpentDb() : CBaseKV<COutpointKey>("spentdb") {}
  virtual ~SpentDb() {}

  void *interface(int interface) final {
    switch (interface) {
      case EIQuerySpent : return static_cast<ISpentDb*>(this);
      default: return nullptr;
    }
  }

  bool querySpent(const BC::Proto::TxHashTy &txid, uint32_t index, CQuerySpentResult &result) final;
  bool querySpentOutputs(const BC::Proto::TxHashTy &txid, uint32_t count, std::vector<CQuerySpentResult> &result) final;

private:
  uint32_t version() final { return 1; }
  bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage) final;

  void connectImpl(CBlockBatch batch,
                   BlockInMemoryIndex &blockIndex,
                   BlockDatabase &blockDb) final;

  void disconnectImpl(const BC::Common::BlockIndex *index,
                      const BC::Proto::Block &block,
                      const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                      const BC::Proto::CBlockValidationData &validationData,
                      BlockInMemoryIndex &blockIndex,
                      BlockDatabase &blockDb) final;
};

}
}
