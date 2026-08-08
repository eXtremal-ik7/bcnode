// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include "common/blockPipeline.h"
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

BC::Common::BlockIndex *rebaseChain(BC::Common::BlockIndex *newBest,
                                    BC::Common::BlockIndex *previousBest,
                                    std::vector<BC::Common::BlockIndex*> &forDisconnect);

BC::Common::BlockIndex *AddHeader(BlockInMemoryIndex &blockIndex,
                                  BC::Common::ChainParams &chainParams,
                                  const BC::Proto::BlockHeader &header,
                                  BC::Common::CheckConsensusCtx &ccCtx);

// attachBlockData - reserve an index for block data: the index, the header chain and nothing
// else (the data is not parsed here). *checkWork is set when the header work was never
// verified. Returns nullptr when the block can't get an index of its own (already have it)
BC::Common::BlockIndex *attachBlockData(BlockInMemoryIndex &blockIndex,
                                        BC::Common::ChainParams &chainParams,
                                        const BC::Proto::BlockHeader &header,
                                        const BC::Proto::BlockHashTy &hash,
                                        bool *checkWork);

// Everything a segment needs before the chain state matters: proof of work, unpack, standalone
// and contextual checks in parallel waves, then the linking of its inputs - what a block of the
// segment answers for itself, what an earlier block of the segment answers, and what only the
// state right before the segment can answer (the residual list). A block that fails its own
// checks cuts the segment short. False when nothing of it is left
bool prepareSegment(BlockInMemoryIndex &blockIndex,
                    BC::Common::ChainParams &chainParams,
                    BC::DB::Storage &storage,
                    CParallelRunner &runner,
                    CSegment &segment,
                    CPipelineCounters &counters,
                    bool prefetch);

// Connect of a prepared segment: rebase if needed, the existence wave over the residual inputs,
// then apply block by block. Every verdict is made before the first block lands - a segment
// reaches the chain whole or not at all. On failure *failedAt is the position of the block the
// chain stops at
bool connectSegment(BlockInMemoryIndex &blockIndex,
                    BC::Common::ChainParams &chainParams,
                    BC::DB::Storage &storage,
                    CParallelRunner &runner,
                    CSegment &segment,
                    size_t *failedAt);

bool loadingBlockIndex(BlockInMemoryIndex &blockIndex,
                       const std::filesystem::path &blockPath,
                       const std::filesystem::path &indexPath);

bool reindex(BlockInMemoryIndex &blockIndex,
             const std::filesystem::path &blockPath,
             BC::Common::ChainParams &chainParams,
             BC::DB::Storage &storage,
             CBlockPipeline &pipeline);


class BlockInMemoryIndex {
public:
  BlockInMemoryIndex() {}

  BC::Common::BlockIndex *best() { return BestIndex_.load(std::memory_order_relaxed); }
  BC::Common::BlockIndex *genesis() { return GenesisIndex_; }
  BC::Proto::Block &genesisBlock() { return GenesisBlock_; }
  void setBest(BC::Common::BlockIndex *index) { BestIndex_.store(index); }
  void setGenesis(BC::Common::BlockIndex *index, const BC::Proto::Block &block) {
    GenesisIndex_ = index;
    GenesisBlock_ = block;
  }

  BC::Common::BlockIndex *indexByHash(const BC::Proto::BlockHashTy &hash) {
    auto It = BlockIndex_.find(hash);
    return It != BlockIndex_.end() ? It->second : nullptr;
  }

  BC::Common::BlockIndex *indexByHeight(uint32_t height) {
    auto It = BlockHeightIndex_.find(height);
    return It != BlockHeightIndex_.end() ? It->second : nullptr;
  }

  // Old-style accessors
  auto &blockIndex() { return BlockIndex_; }
  auto &blockHeightIndex() { return BlockHeightIndex_; }
  CCandidateTracker &candidateTracker() { return CandidateTracker_; }

private:
  CCandidateTracker CandidateTracker_;
  tbb::concurrent_unordered_map<BC::Proto::BlockHashTy, BC::Common::BlockIndex*, std::hash<BC::Proto::BlockHashTy>> BlockIndex_;
  tbb::concurrent_unordered_map<uint32_t, BC::Common::BlockIndex*, std::hash<uint32_t>> BlockHeightIndex_;
  std::atomic<BC::Common::BlockIndex*> BestIndex_ = nullptr;
  BC::Common::BlockIndex *GenesisIndex_ = nullptr;
  BC::Proto::Block GenesisBlock_;
};

class BlockDatabase {
public:
  BlockDatabase() {}
  bool init(const std::filesystem::path &blocksDir,
            const std::filesystem::path &indexDir,
            BC::Common::ChainParams &chainParams);

  const std::filesystem::path &blocksDir() const { return BlocksDir_; }
  const std::filesystem::path &indexDir() const { return IndexDir_; }
  LinearDataStorage &blockReader() { return BlockStorage_; }
  LinearDataStorage &linkedOutputsReader() { return LinkedOutputsStorage_; }

  bool writeBlock(BC::Common::BlockIndex *index, bool *needFlush);
  bool flush() { return BlockStorage_.flush() && IndexStorage_.flush() && LinkedOutputsStorage_.flush(); }
  uint32_t magic() { return Magic_; }

  // TODO: remove
  LinearDataStorage &indexStorage() { return IndexStorage_; }
  LinearDataStorage &linkedOutputsStorage() { return LinkedOutputsStorage_; }

private:
  LinearDataStorage BlockStorage_;
  LinearDataStorage IndexStorage_;
  LinearDataStorage LinkedOutputsStorage_;
  std::filesystem::path BlocksDir_;
  std::filesystem::path IndexDir_;
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

intrusive_ptr<BC::Common::CIndexCacheObject> objectByIndex(BC::Common::BlockIndex *index,
                                                           BC::Common::ChainParams &chainParams,
                                                           BlockDatabase &blockDb);

// The single definition of what a stored block must become before a database may connect or
// disconnect it: linked outputs and validation data (txids, consensus exemptions) filled in.
// objectByIndex reads the bytes itself, the catch-up preparation brings them from a combined
// read. 'info' accounts the result, null for a block nobody budgets for
intrusive_ptr<BC::Common::CIndexCacheObject> objectFromStoredBytes(BC::Common::BlockIndex *index,
                                                                   BC::Common::ChainParams &chainParams,
                                                                   const void *blockData,
                                                                   size_t blockSize,
                                                                   const void *linkedOutputsData,
                                                                   size_t linkedOutputsSize,
                                                                   CAllocationInfo *info);

// Reads blocks back in chain order for a database that is behind, a batch at a time: a block
// and its linked outputs come from one combined read, and the batch goes on as a catch-up
// segment for the pipeline to prepare
class CCatchUpReader {
public:
  // batchSizeLimit/batchBlocksLimit cut the batch, same meaning as the segment limits of the
  // pipeline; the disk read stays combined across whatever piece of it is contiguous
  CCatchUpReader(BlockDatabase &blockDb,
                 BC::Common::BlockIndex *first,
                 size_t batchSizeLimit,
                 size_t batchBlocksLimit) :
    BlockDb_(blockDb), Cursor_(first), BatchSizeLimit_(batchSizeLimit), BatchBlocksLimit_(batchBlocksLimit) {}

  // Null at the end of the chain, and when the block database does not hold what the index
  // says it holds - failed() tells the two apart
  std::unique_ptr<CSegment> next();
  bool failed() const { return Failed_; }

private:
  // One stored record with its prefix: what a single read covers
  struct CRecord {
    uint32_t FileNo;
    uint32_t Offset;
    uint32_t Size;
  };

  static bool readRecords(LinearDataStorage &storage, const std::vector<CRecord> &records, void *destination);

private:
  BlockDatabase &BlockDb_;
  BC::Common::BlockIndex *Cursor_ = nullptr;
  size_t BatchSizeLimit_ = 0;
  size_t BatchBlocksLimit_ = 0;
  bool Failed_ = false;

  std::vector<BC::Common::BlockIndex*> Indexes_;
  std::vector<CRecord> BlockRecords_;
  std::vector<CRecord> LinkedOutputsRecords_;
};
