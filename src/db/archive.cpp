// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "archive.h"
#include "addrdb.h"
#include "addrHistoryDb.h"
#include "spentdb.h"
#include "txdbRef.h"
#include "txdb.h"
#include "storage.h"

namespace BC {
namespace DB {

Archive::~Archive()
{
  if (!ConnectWorkers_.empty()) {
    {
      std::lock_guard lock(ConnectMutex_);
      ConnectStop_ = true;
    }
    ConnectStartCv_.notify_all();
    for (std::thread &thread: ConnectWorkers_)
      thread.join();
  }
}

void Archive::startConnectWorkers()
{
  for (size_t i = 0; i < AllDb_.size(); i++)
    ConnectWorkers_.emplace_back([this, i]() { connectWorker(i); });
}

void Archive::connect(CBlockBatch batch, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb)
{
  if (batch.empty())
    return;

  CConnectTask task;
  task.Batch = batch;
  task.BlockIndex = &blockIndex;
  task.BlockDb = &blockDb;
  task.FirstHeight = batch.front().Index->Height;
  submit(task);
  wait(task);
}

void Archive::submit(CConnectTask &task)
{
  // Before init() started them: same work, this thread doing all of it
  if (ConnectWorkers_.empty()) {
    for (size_t i = 0; i < AllDb_.size(); i++)
      connectSlot(i, task);
    return;
  }

  {
    std::lock_guard lock(ConnectMutex_);
    task.Pending = AllDb_.size();
    // A share of the data for every worker, taken before they can see the task
    if (task.Segment)
      task.Segment->shareAdd(AllDb_.size());
    for (auto &queue: ConnectQueues_)
      queue.push_back(&task);
  }
  ConnectStartCv_.notify_all();
}

void Archive::wait(CConnectTask &task)
{
  std::unique_lock lock(ConnectMutex_);
  ConnectDoneCv_.wait(lock, [&task]() { return task.Pending == 0; });
}

// The database's own tail of the batch. Heights inside a batch are contiguous,
// so the wake-up height turns straight into the number of blocks to skip
void Archive::connectSlot(size_t slot, CConnectTask &task)
{
  const uint32_t from = ConnectFromHeight_[slot];
  const size_t skip = from > task.FirstHeight ? from - task.FirstHeight : 0;
  if (skip >= task.Batch.size())
    return;
  AllDb_[slot]->connect(task.Batch.subspan(skip), *task.BlockIndex, *task.BlockDb);
}

void Archive::connectWorker(size_t slot)
{
  loguru::set_thread_name((AllDb_[slot]->name() + ".connect").c_str());

  for (;;) {
    CConnectTask *task = nullptr;
    {
      std::unique_lock lock(ConnectMutex_);
      ConnectStartCv_.wait(lock, [this, slot]() { return ConnectStop_ || !ConnectQueues_[slot].empty(); });
      if (ConnectQueues_[slot].empty())
        return;
      task = ConnectQueues_[slot].front();
      ConnectQueues_[slot].pop_front();
    }

    connectSlot(slot, *task);

    // Done reading the batch: nothing below may touch Batch or Segment again
    if (task->Segment)
      CSegment::shareRelease(task->Segment);

    bool done;
    {
      std::lock_guard lock(ConnectMutex_);
      done = (--task->Pending == 0);
    }
    if (done)
      ConnectDoneCv_.notify_all();
  }
}

template<typename IInterface>
IInterface* setupHandler(config4cpp::Configuration *cfg,
                         const char *name,
                         EInterfaceTy type,
                         const std::unordered_map<std::string, uint32_t> &dbMap,
                         const std::vector<std::unique_ptr<BC::DB::BaseInterface>> &allDb)
{
  const char *handler = cfg->lookupString("archive.queries", name, nullptr);
  if (handler == nullptr)
    return nullptr;

  auto It = dbMap.find(handler);
  if (It == dbMap.end()) {
    LOG_F(ERROR, "Database %s is handler for %s, but it not present", handler, name);
    exit(1);
  }

  IInterface *result = static_cast<IInterface*>(allDb[It->second]->interface(type));
  if (!result) {
    LOG_F(ERROR, "Invalid database for query type %s", name);
    exit(1);
  }

  return result;
}

bool Archive::init(BlockInMemoryIndex &blockIndex,
                   BC::Common::ChainParams &chainParams,
                   BC::DB::Storage &storage,
                   CBlockPipeline &pipeline,
                   const CBlockPipeline::CParams &params,
                   const std::filesystem::path &dataDir,
                   const std::filesystem::path &utxoPath,
                   config4cpp::Configuration *cfg)
{
  std::unordered_map<std::string, uint32_t> dbIndexMap;
  config4cpp::StringVector enabledDatabases;
  cfg->lookupList("archive", "databases", enabledDatabases, config4cpp::StringVector());
  CompactAfterSync_ = cfg->lookupBoolean("archive", "compactAfterSync", false);

  for (int i = 0; i < enabledDatabases.length(); i++) {
    if (!dbIndexMap.insert(std::make_pair(enabledDatabases[i], i)).second) {
      LOG_F(ERROR, "Duplicate database type: %s", enabledDatabases[i]);
      return false;
    }

    if (strcmp(enabledDatabases[i], "addrhistorydb") == 0) {
      AllDb_.emplace_back(new AddrHistoryDb());
    } else if (strcmp(enabledDatabases[i], "addrdb") == 0) {
      AllDb_.emplace_back(new AddrDb());
    } else if (strcmp(enabledDatabases[i], "txdb.ref") == 0) {
      AllDb_.emplace_back(new TxDbRef());
    } else if (strcmp(enabledDatabases[i], "txdb.full") == 0) {
      AllDb_.emplace_back(new TxDb());
    } else if (strcmp(enabledDatabases[i], "spentdb") == 0) {
      AllDb_.emplace_back(new SpentDb());
    } else {
      LOG_F(ERROR, "Unknown database type: %s", enabledDatabases[i]);
      return false;
    }
  }

  // Sized before anything can connect; the workers themselves start below
  ConnectQueues_.resize(AllDb_.size());
  ConnectFromHeight_.assign(AllDb_.size(), 0);

  // Route queries
  TransactionDb_ = setupHandler<ITransactionDb>(cfg, "tx", EIQueryTransaction, dbIndexMap, AllDb_);
  AddrHistoryDb_ = setupHandler<IAddrHistoryDb>(cfg, "addrhistory", EIQueryAddrHistory, dbIndexMap, AllDb_);
  AddrDb_ = setupHandler<IAddrDb>(cfg, "addr", EIQueryAddr, dbIndexMap, AllDb_);
  SpentDb_ = setupHandler<ISpentDb>(cfg, "spent", EIQuerySpent, dbIndexMap, AllDb_);

  BC::Common::BlockIndex *utxoFirstBlock = nullptr;
  std::vector<BC::Common::BlockIndex*> utxoDisconnect;
  std::vector<std::vector<BC::Common::BlockIndex*>> archiveDisconnect;
  std::vector<BC::Common::BlockIndex*> archiveFirstBlocks;
  archiveDisconnect.resize(AllDb_.size());
  archiveFirstBlocks.resize(AllDb_.size());

  // Initialize all databases
  if (!storage.utxodb().initialize(blockIndex, utxoPath, storage, cfg, &utxoFirstBlock, utxoDisconnect))
    return false;
  for (size_t i = 0; i < AllDb_.size(); i++) {
    // Get custom database path from config
    std::string scope = "archive." + AllDb_[i]->name();
    const char *p = cfg->lookupString(scope.c_str(), "path", nullptr);
    std::filesystem::path dbPath = p ? p : dataDir / AllDb_[i]->name();

    if (!AllDb_[i]->initialize(blockIndex, dbPath, storage, cfg, &archiveFirstBlocks[i], archiveDisconnect[i]))
      return false;
  }

  // The catch-up below already fans out over these; the disconnect walks above
  // it run on this thread while every queue is empty
  startConnectWorkers();

  // Disconnect UTXO
  if (!dbDisconnectBlocks(storage.utxodb(), blockIndex, chainParams, storage, utxoDisconnect))
    return false;

  // Disconnect archive
  for (size_t i = 0; i < AllDb_.size(); i++) {
    if (!dbDisconnectBlocks(*AllDb_[i], blockIndex, chainParams, storage, archiveDisconnect[i]))
      return false;
  }

  // Connect. The databases wake up at different heights, so the catch-up feeds
  // one batch to all of them and each takes the tail that is new to it
  for (size_t i = 0; i < AllDb_.size(); i++) {
    setConnectFrom(i, archiveFirstBlocks[i] ? archiveFirstBlocks[i]->Height
                                            : std::numeric_limits<uint32_t>::max());
  }

  bool connected = dbConnectBlocks(storage.utxodb(), utxoFirstBlock, archiveFirstBlocks, this,
                                   blockIndex, chainParams, storage, pipeline, params,
                                   "utxo & archive databases");

  // Everyone is level from here on: a batch goes to every database whole
  for (size_t i = 0; i < AllDb_.size(); i++)
    setConnectFrom(i, 0);

  return connected;
}

bool Archive::purge(config4cpp::Configuration *cfg, std::filesystem::path &dataDir)
{
  config4cpp::StringVector enabledDatabases;
  cfg->lookupList("archive", "databases", enabledDatabases, config4cpp::StringVector());

  for (int i = 0; i < enabledDatabases.length(); i++) {
    std::string scope = "archive.";
    scope.append(enabledDatabases[i]);
    const char *p = cfg->lookupString(scope.c_str(), "path", nullptr);

    std::filesystem::path dbPath = p ? p : dataDir / enabledDatabases[i];
    std::error_code ec;
    std::filesystem::remove_all(dbPath, ec);
    if (ec) {
      LOG_F(ERROR, "Failed to remove database %s", enabledDatabases[i]);
      return false;
    }
  }

  return true;
}

}
}
