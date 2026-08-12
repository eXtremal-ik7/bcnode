// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "db/sync.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

class BlockDatabase;

namespace BC {
namespace DB {

class Archive {
public:
  // One batch on its way to every database. The refs belong to the submitter:
  // a worker only reads them and drops Pending when done, so the submitter may
  // reuse the storage once wait() has returned. Segment, when set, is the data
  // behind those refs: each worker owns a share and drops it as it finishes
  struct CConnectTask {
    CBlockBatch Batch;
    BlockInMemoryIndex *BlockIndex = nullptr;
    BlockDatabase *BlockDb = nullptr;
    CSegment *Segment = nullptr;
    uint32_t FirstHeight = 0;
    size_t Pending = 0;   // ConnectMutex_
  };

  ~Archive();

  bool init(BlockInMemoryIndex &blockIndex,
            BC::Common::ChainParams &chainParams,
            BC::DB::Storage &storage,
            CBlockPipeline &pipeline,
            const CBlockPipeline::CParams &params,
            const std::filesystem::path &dataDir,
            const std::filesystem::path &utxoPath,
            config4cpp::Configuration *cfg);

  bool purge(config4cpp::Configuration *cfg, std::filesystem::path &dataDir);

  // The databases are independent over one read-only batch (each writes only
  // its own engine), so the batch fans out to a thread per database. Post and
  // wait: the caller has nothing else to do, and the batch dies with it
  void connect(CBlockBatch batch, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb);

  // The same fan-out for a feeder that has work of its own while the databases
  // chew - the catch-up prepares the next batch between these two calls
  void submit(CConnectTask &task);
  void wait(CConnectTask &task);

  // Where a database wakes up: everything below is already in it, so it gets
  // the tail of the batch and nothing else. UINT32_MAX skips it entirely, and
  // zero - the state once everyone is caught up - takes every batch whole
  void setConnectFrom(size_t index, uint32_t height) { ConnectFromHeight_[index] = height; }
  size_t databasesNum() const { return AllDb_.size(); }

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

  // What a database put off until it had the data - rank indexes today. In
  // parallel: each one scans only its own shards
  bool finishInitialBuild() {
    std::atomic<bool> ok = true;
    std::vector<std::thread> workers;
    for (auto &db: AllDb_) {
      BC::DB::BaseInterface *base = db.get();
      workers.emplace_back([base, &ok]() {
        if (!base->finishInitialBuild())
          ok = false;
      });
    }
    for (auto &worker: workers)
      worker.join();
    return ok;
  }

  // Databases settle in parallel - each one waits on its own debt, and they
  // share the background pool anyway
  void settle() {
    std::vector<std::thread> workers;
    for (auto &db: AllDb_) {
      BC::DB::BaseInterface *base = db.get();
      workers.emplace_back([base]() { base->flush(); base->settle(); });
    }
    for (auto &worker: workers)
      worker.join();
  }

  // Off by default: a node a few blocks behind must not compact its whole
  // archive at startup. Benchmarks and rescans turn it on to see honest numbers
  bool compactAfterSync() const { return CompactAfterSync_; }

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
  void startConnectWorkers();
  void connectWorker(size_t slot);
  void connectSlot(size_t slot, CConnectTask &task);

private:
  std::vector<std::unique_ptr<BC::DB::BaseInterface>> AllDb_;

  // A worker per database, alive from init() to the destructor. Nobody else
  // ever connects: a database is always mutated by the same thread, which is
  // what the engines expect of a single mutator. Everything the storage thread
  // still does itself (disconnect, flush) is ordered after a wait()
  std::vector<std::thread> ConnectWorkers_;
  std::vector<std::deque<CConnectTask*>> ConnectQueues_;
  std::vector<uint32_t> ConnectFromHeight_;
  std::mutex ConnectMutex_;
  std::condition_variable ConnectStartCv_;
  std::condition_variable ConnectDoneCv_;
  bool ConnectStop_ = false;
  bool CompactAfterSync_ = false;

public:
  // Handlers
  ITransactionDb *TransactionDb_ = nullptr;
  IAddrHistoryDb *AddrHistoryDb_ = nullptr;
  IAddrDb *AddrDb_ = nullptr;
  ISpentDb *SpentDb_ = nullptr;
};

}
}
