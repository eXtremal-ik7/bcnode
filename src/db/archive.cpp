// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "archive.h"
#include "addrHistoryDb.h"
#include "txdbRef.h"
#include "txdb.h"
#include "storage.h"

namespace BC {
namespace DB {

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
                   BC::DB::Storage &storage,
                   const std::filesystem::path &dataDir,
                   const std::filesystem::path &utxoPath,
                   config4cpp::Configuration *cfg)
{
  std::unordered_map<std::string, uint32_t> dbIndexMap;
  config4cpp::StringVector enabledDatabases;
  cfg->lookupList("archive", "databases", enabledDatabases, config4cpp::StringVector());

  for (int i = 0; i < enabledDatabases.length(); i++) {
    if (!dbIndexMap.insert(std::make_pair(enabledDatabases[i], i)).second) {
      LOG_F(ERROR, "Duplicate database type: %s", enabledDatabases[i]);
      return false;
    }

    if (strcmp(enabledDatabases[i], "addrhistorydb") == 0) {
      AllDb_.emplace_back(new AddrHistoryDb());
    } else if (strcmp(enabledDatabases[i], "txdb.ref") == 0) {
      AllDb_.emplace_back(new TxDbRef());
    } else if (strcmp(enabledDatabases[i], "txdb.full") == 0) {
      AllDb_.emplace_back(new TxDb());
    } else {
      LOG_F(ERROR, "Unknown database type: %s", enabledDatabases[i]);
      return false;
    }
  }

  // Route queries
  TransactionDb_ = setupHandler<ITransactionDb>(cfg, "tx", EIQueryTransaction, dbIndexMap, AllDb_);
  AddrHistoryDb_ = setupHandler<IAddrHistoryDb>(cfg, "addrhistory", EIQueryAddrHistory, dbIndexMap, AllDb_);

  BC::Common::BlockIndex *utxoBestBlock;
  std::vector<BC::Common::BlockIndex*> utxoDisconnect;
  std::vector<std::vector<BC::Common::BlockIndex*>> archiveDisconnect;
  std::vector<BaseWithBest> archiveDatabases;
  archiveDisconnect.resize(AllDb_.size());
  archiveDatabases.resize(AllDb_.size());

  // Initialize all databases
  if (!storage.utxodb().initialize(blockIndex, utxoPath, storage, cfg, &utxoBestBlock, utxoDisconnect))
    return false;
  for (size_t i = 0; i < AllDb_.size(); i++) {
    BaseWithBest &current = archiveDatabases[i];
    current.Base = AllDb_[i].get();

    // Get custom database path from config
    std::string scope = "archive." + AllDb_[i]->name();
    const char *p = cfg->lookupString(scope.c_str(), "path", nullptr);
    std::filesystem::path dbPath = p ? p : dataDir / AllDb_[i]->name();

    if (!AllDb_[i]->initialize(blockIndex, dbPath, storage, cfg, &current.BestBlock, archiveDisconnect[i]))
      return false;
  }

  // Disconnect UTXO
  if (!dbDisconnectBlocks(storage.utxodb(), blockIndex, storage, utxoDisconnect))
    return false;

  // Disconnect archive
  for (size_t i = 0; i < AllDb_.size(); i++) {
    if (!dbDisconnectBlocks(*AllDb_[i], blockIndex, storage, archiveDisconnect[i]))
      return false;
  }

  // Connect
  if (!dbConnectBlocks(storage.utxodb(), utxoBestBlock, archiveDatabases, blockIndex, storage, "utxo & archive databases"))
    return false;

  return true;
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
