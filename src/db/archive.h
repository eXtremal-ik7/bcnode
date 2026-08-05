// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "db/sync.h"

class BlockDatabase;

namespace BC {
namespace DB {

class Archive {
public:
  bool init(BlockInMemoryIndex &blockIndex,
            BC::Common::ChainParams &chainParams,
            BC::DB::Storage &storage,
            const std::filesystem::path &dataDir,
            const std::filesystem::path &utxoPath,
            config4cpp::Configuration *cfg);

  bool purge(config4cpp::Configuration *cfg, std::filesystem::path &dataDir);

  void connect(CBlockBatch batch, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) {
    for (auto &db: AllDb_)
      db->connect(batch, blockIndex, blockDb);
  }

  void disconnect(const BC::Common::BlockIndex *index,
                  const BC::Proto::Block &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  const BC::Proto::CBlockValidationData &validationData,
                  BlockInMemoryIndex &blockIndex,
                  BlockDatabase &blockDb) {
    for (auto &db: AllDb_)
      db->disconnect(index, block, linkedOutputs, validationData, blockIndex, blockDb);
  }

  void flush() {
    for (auto &db: AllDb_)
      db->flush();
  }

  // Any engine over its admission limit: the pipeline stops taking work while
  // the storage thread keeps draining its queue and the flushers catch up
  bool pipelineFull() const {
    for (const auto &db: AllDb_) {
      if (db->pipelineFull())
        return true;
    }
    return false;
  }

private:
  std::vector<std::unique_ptr<BC::DB::BaseInterface>> AllDb_;

public:
  // Handlers
  ITransactionDb *TransactionDb_ = nullptr;
  IAddrHistoryDb *AddrHistoryDb_ = nullptr;
  IAddrDb *AddrDb_ = nullptr;
  ISpentDb *SpentDb_ = nullptr;
};

}
}
