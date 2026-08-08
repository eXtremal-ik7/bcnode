// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "db/sync.h"

#include <condition_variable>
#include <mutex>
#include <thread>

class BlockDatabase;

namespace BC {
namespace DB {

class Archive {
public:
  ~Archive();

  bool init(BlockInMemoryIndex &blockIndex,
            BC::Common::ChainParams &chainParams,
            BC::DB::Storage &storage,
            const std::filesystem::path &dataDir,
            const std::filesystem::path &utxoPath,
            config4cpp::Configuration *cfg);

  bool purge(config4cpp::Configuration *cfg, std::filesystem::path &dataDir);

  // The databases are independent over one read-only batch (each writes only
  // its own engine), so the batch fans out to a thread per database. The
  // barrier before returning keeps per-database unit order and lets the batch
  // die with the caller
  void connect(CBlockBatch batch, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb);

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
  void connectWorker(size_t slot);

private:
  std::vector<std::unique_ptr<BC::DB::BaseInterface>> AllDb_;

  // One worker per database except AllDb_[0], which the storage thread runs
  // itself. A database is always mutated by the same thread - the engines
  // expect a single mutator
  std::vector<std::thread> ConnectWorkers_;
  std::mutex ConnectMutex_;
  std::condition_variable ConnectStartCv_;
  std::condition_variable ConnectDoneCv_;
  CBlockBatch ConnectBatch_;
  BlockInMemoryIndex *ConnectBlockIndex_ = nullptr;
  BlockDatabase *ConnectBlockDb_ = nullptr;
  uint64_t ConnectGeneration_ = 0;
  size_t ConnectDone_ = 0;
  bool ConnectStop_ = false;

public:
  // Handlers
  ITransactionDb *TransactionDb_ = nullptr;
  IAddrHistoryDb *AddrHistoryDb_ = nullptr;
  IAddrDb *AddrDb_ = nullptr;
  ISpentDb *SpentDb_ = nullptr;
};

}
}
