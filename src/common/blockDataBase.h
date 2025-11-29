// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include "common/blockIndexCombiner.h"
#include "common/linearDataStorage.h"
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>
#include <filesystem>
#include <functional>
#include <thread>

struct BCNodeContext;
struct asyncBase;
struct aioUserEvent;

namespace BC {
namespace DB {
class Storage;
}
}

class BlockInMemoryIndex;
class BlockDatabase;

typedef std::function<void(const std::vector<BC::Common::BlockIndex*>&)> newBestCallback;

BC::Common::BlockIndex *rebaseChain(BC::Common::BlockIndex *newBest,
                                    BC::Common::BlockIndex *previousBest,
                                    std::vector<BC::Common::BlockIndex*> &forDisconnect);

BC::Common::BlockIndex *AddHeader(BlockInMemoryIndex &blockIndex,
                                  BC::Common::ChainParams &chainParams,
                                  const BC::Proto::BlockHeader &header,
                                  BC::Common::CheckConsensusCtx &ccCtx);

BC::Common::BlockIndex *AddBlock(BlockInMemoryIndex &blockIndex,
                                 BC::Common::ChainParams &chainParams,
                                 BC::DB::Storage &storage,
                                 SerializedDataObject *serialized,
                                 BC::Common::CheckConsensusCtx &ccCtx,
                                 newBestCallback callback,
                                 uint32_t fileNo=std::numeric_limits<uint32_t>::max(),
                                 uint32_t fileOffset=std::numeric_limits<uint32_t>::max());

bool loadingBlockIndex(BlockInMemoryIndex &blockIndex, std::filesystem::path &dataDir);
bool reindex(BlockInMemoryIndex &blockIndex, std::filesystem::path &dataDir, BC::Common::ChainParams &chainParams, BC::DB::Storage &storage);


class BlockInMemoryIndex {
public:
  BlockInMemoryIndex() {}

  BC::Common::BlockIndex *best() { return BestIndex_.load(std::memory_order::memory_order_relaxed); }
  BC::Common::BlockIndex *genesis() { return GenesisIndex_; }
  BC::Proto::Block &genesisBlock() { return GenesisBlock_; }
  void setBest(BC::Common::BlockIndex *index) { BestIndex_.store(index); }
  void setGenesis(BC::Common::BlockIndex *index, const BC::Proto::Block &block) {
    GenesisIndex_ = index;
    GenesisBlock_ = block;
  }

  // Old-style accessors
  auto &blockIndex() { return BlockIndex_; }
  auto &blockHeightIndex() { return BlockHeightIndex_; }
  auto &combiner() { return ChainStateCombiner_; }

private:
  Combiner<BlockProcessingTask> ChainStateCombiner_;
  tbb::concurrent_unordered_map<BC::Proto::BlockHashTy, BC::Common::BlockIndex*, std::hash<BC::Proto::BlockHashTy>> BlockIndex_;
  tbb::concurrent_unordered_map<uint32_t, BC::Common::BlockIndex*, std::hash<uint32_t>> BlockHeightIndex_;
  std::atomic<BC::Common::BlockIndex*> BestIndex_ = nullptr;
  BC::Common::BlockIndex *GenesisIndex_ = nullptr;
  BC::Proto::Block GenesisBlock_;
};

class BlockDatabase {
public:
  BlockDatabase() {}
  bool init(std::filesystem::path &dataDir, BC::Common::ChainParams &chainParams);

  std::filesystem::path &dataDir() { return DataDir_; }
  LinearDataStorage &blockReader() { return BlockStorage_; }

  bool writeBlock(BC::Common::BlockIndex *index);
  bool writeBufferEmpty() { return BlockStorage_.bufferEmpty(); }
  bool flush() { return BlockStorage_.flush() && IndexStorage_.flush(); }
  uint32_t magic() { return Magic_; }

  // TODO: remove
  LinearDataStorage &indexStorage() { return IndexStorage_; }

private:
  LinearDataStorage BlockStorage_;
  LinearDataStorage IndexStorage_;
  std::filesystem::path DataDir_;
  // NOTE: bitcoin core compatibility
  uint32_t Magic_;
};

class BlockSearcher {
private:
  static constexpr unsigned MaxIoSize = 4*1048576;

private:
  BlockDatabase &BlockDb_;
  uint32_t fileNo = std::numeric_limits<uint32_t>::max();
  uint32_t fileOffsetBegin = std::numeric_limits<uint32_t>::max();
  uint32_t fileOffsetCurrent = std::numeric_limits<uint32_t>::max();

  xmstream stream;
  std::filesystem::path blocksDirectory;
  std::function<void(void*, size_t)> Handler_;
  std::function<void()> ErrorHandler_;
  std::vector<uint32_t> ExpectedBlockSizes_;

  void fetchPending();

public:
  BlockSearcher(BlockDatabase &blockDb, std::function<void(void*, size_t)> handler, std::function<void()> errorHandler);
  ~BlockSearcher();
  BC::Common::BlockIndex *add(BC::Common::BlockIndex *index);
  BC::Common::BlockIndex *add(BlockInMemoryIndex &blockIndex, const BC::Proto::BlockHashTy &hash);
};
