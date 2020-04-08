// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include "BC/http.h"
#include "BC/nativeApi.h"
#include "BC/network.h"

#include <tbb/concurrent_unordered_map.h>

#include "blockIndexCombiner.h"
#include "linearDataStorage.h"
#include "serializedDataCache.h"

#include <filesystem>

class BCPeer;
struct StoreTask;

struct BCNodeContext {
public:
  std::filesystem::path dataDir;
  int networkId;

  BC::Common::ChainParams chainParams;
  BC::Common::BlockIndex *genesisIndex;

  // Blockchain
  Combiner<BlockProcessingTask> ChainStateCombiner;
  std::atomic<BC::Common::BlockIndex*> BestIndex;

  tbb::concurrent_unordered_map<uint32_t, BC::Common::BlockIndex*, std::hash<uint32_t>> blockHeightIndex;
  tbb::concurrent_unordered_map<BC::Proto::BlockHashTy, BC::Common::BlockIndex*, std::hash<BC::Proto::BlockHashTy>> blockIndex;

  // Network
  asyncBase *networkBase;
  BC::Network::Node PeerManager;
  BC::Network::HttpApiNode httpApiNode;
  BC::Network::NativeApiNode nativeApiNode;

  // Storage
  SerializedDataCache BlockCache;
  uint64_t storageTaskId = 0;
  asyncBase *storageBase = nullptr;
  aioUserEvent *storageEvent = nullptr;
  aioUserEvent *storageTimer = nullptr;
  tbb::concurrent_queue<StoreTask*> storeQueue;
  std::vector<BC::Common::BlockIndex*> cachedBlocks;
  LinearDataReader blockStorageReader;
  LinearDataWriter blockStorageWriter;
  LinearDataWriter indexStorageWriter;

public:
  BCNodeContext() : BestIndex(nullptr) {}
};

void shutdown(BCNodeContext &context);
