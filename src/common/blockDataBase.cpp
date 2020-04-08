// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockDataBase.h"

#include "bcnode.h"
#include "merkleTree.h"
#include "p2putils/coreTypes.h"
#include <p2putils/xmstream.h>
#include "loguru.hpp"
#include <chrono>
#include <deque>
#include <future>
#include <thread>

struct LoadingIndexContext {
  std::vector<BC::Common::BlockIndex*> allIndexes;
  BC::Common::BlockIndex *bestIndex = nullptr;
};

struct BlockPosition {
  uint32_t offset;
  uint32_t size;
};

static inline void QueueNextHeaders(std::deque<BC::Common::BlockIndex*> &queue, BC::Common::BlockIndex *start)
{
  auto it = start->SuccessorHeaders.exchange(nullptr, 1);
  while (auto ptr = it.pointer()) {
    queue.push_back(ptr);

    while (ptr->ConcurrentHeaderNext.data() == WaitPtr<BC::Common::BlockIndex>())
      continue;
    it = ptr->ConcurrentHeaderNext.load();
  }
}

static inline void QueueNextBlocks(std::deque<BC::Common::BlockIndex*> &queue, BC::Common::BlockIndex *start)
{
  auto it = start->SuccessorBlocks.exchange(nullptr, 1);
  while (auto ptr = it.pointer()) {
    queue.push_back(ptr);
    while (ptr->ConcurrentBlockNext.data() == WaitPtr<BC::Common::BlockIndex>())
      continue;
    it = ptr->ConcurrentBlockNext.load();
  }
}

static void ConnectBlock(BCNodeContext &context, BC::Common::BlockIndex *index, bool silent = true)
{
  if (!silent)
    LOG_F(INFO, "Connect block %s (%u)", index->Header.GetHash().ToString().c_str(), index->Height);
  index->Prev->Next = index;
  context.blockHeightIndex[index->Height] = index;
}

static void DisconnectBlock(BCNodeContext &context, BC::Common::BlockIndex *index, bool silent = true)
{
  if (!silent)
    LOG_F(INFO, "Disconnect block %s (%u)", index->Header.GetHash().ToString().c_str(), index->Height);
  index->Prev->Next = nullptr;
  context.blockHeightIndex[index->Height] = nullptr;
}

static void BuildHeaderChain(BCNodeContext &context, BC::Common::BlockIndex *start)
{
  BC::Common::BlockIndex *currentStart = start;
  std::deque<BC::Common::BlockIndex*> queue;

  QueueNextHeaders(queue, currentStart);

  while (!queue.empty()) {
    BC::Common::BlockIndex *current = queue.front();
    BC::Common::BlockIndex *prev = current->Prev;

    if (current->Height == std::numeric_limits<uint32_t>::max()) {
      current->Height = prev->Height + 1;
      current->ChainWork = prev->ChainWork + BC::Common::GetBlockProof(current->Header, context.chainParams);
    }

    QueueNextHeaders(queue, current);
    queue.pop_front();
  }
}

static void buildBlockChain(BCNodeContext &context, BC::Common::BlockIndex *start, std::vector<BC::Common::BlockIndex*> &acceptedBlocks)
{
  BC::Common::BlockIndex *currentStart = start;
  std::deque<BC::Common::BlockIndex*> queue;

  QueueNextBlocks(queue, currentStart);
  while (!queue.empty()) {
    BC::Common::BlockIndex *current = queue.front();
    BC::Common::BlockIndex *prev = current->Prev;

    current->OnChain = prev->OnChain;
    if (current->Height == std::numeric_limits<uint32_t>::max()) {
      current->Height = prev->Height + 1;
      current->ChainWork = prev->ChainWork + BC::Common::GetBlockProof(current->Header, context.chainParams);
    }

    if (current->ChainWork > context.BestIndex.load()->ChainWork) {
      // New best block found
      BC::Common::BlockIndex *previousBest = context.BestIndex.load();
      if (current->Prev == previousBest) {
        ConnectBlock(context, current);
      } else {
        // Rebuild chain from least common ancestor
        std::vector<BC::Common::BlockIndex*> newPath;
        BC::Common::BlockIndex *lb;
        BC::Common::BlockIndex *sb;
        if (current->Height >= previousBest->Height) {
          lb = current;
          sb = previousBest;
          uint32_t sbHeight = sb->Height;
          while (lb->Height > sbHeight) {
            newPath.push_back(lb);
            lb = lb->Prev;
          }
          while (sb != lb) {
            newPath.push_back(lb);
            DisconnectBlock(context, sb, false);
            sb = sb->Prev;
            lb = lb->Prev;
          }

        } else {
          lb = previousBest;
          sb = current;
          uint32_t sbHeight = sb->Height;
          while (lb->Height > sbHeight) {
            DisconnectBlock(context, lb, false);
            lb = lb->Prev;
          }
          while (sb != lb) {
            newPath.push_back(sb);
            DisconnectBlock(context, lb, false);
            sb = sb->Prev;
            lb = lb->Prev;
          }
        }

        // Connect blocks from new path
        for (auto I = newPath.rbegin(), IE = newPath.rend(); I != IE; ++I)
          ConnectBlock(context, *I, false);
      }

      context.BestIndex = current;
    }

    // drop block data cache for connected block
    StoreTask *task = new StoreTask;
    task->id = context.storageTaskId++;
    task->index = current;
    acceptedBlocks.push_back(current);
    context.storeQueue.push(task);
    QueueNextBlocks(queue, current);
    queue.pop_front();
  }
}

BC::Common::BlockIndex *AddHeader(BCNodeContext &context, const BC::Proto::BlockHeader &header, BC::Common::CheckConsensusCtx &ccCtx)
{
  // Check presence of this block
  BlockStatus empty = BSEmpty;
  BC::Proto::BlockHashTy hash = header.GetHash();
  BC::Common::BlockIndex *index = nullptr;

  {
    auto It = context.blockIndex.find(hash);
    if (It != context.blockIndex.end()) {
      // Found BlockIndex structure can describe:
      //  1. Block
      //  2. Stub for previous block (not have predecessor block)
      index = It->second;
      if (!index->IndexState.compare_exchange_strong(empty, BSHeader)) {
        return index;
      }
    }
  }

  // Check consensus (such as PoW)
  if (!BC::Common::checkConsensus(header, ccCtx, context.chainParams)) {
    LOG_F(ERROR, "Check Proof-Of-Work failed for block %s", hash.ToString().c_str());
    return nullptr;
  }

  // Prepare block index structure for predecessor block
  auto prevIndex = BC::Common::BlockIndex::create(BSEmpty, nullptr);

  auto prevIt = context.blockIndex.insert(std::pair(header.hashPrevBlock, prevIndex));
  if (!prevIt.second) {
    delete prevIndex;
    prevIndex = prevIt.first->second;
  }

  // Try insert incoming block to index
  if (!index) {
    index = BC::Common::BlockIndex::create(BSHeader, prevIndex);
    auto It = context.blockIndex.insert(std::pair(hash, index));
    if (!It.second) {
      // Already have index for current block
      delete index;
      index = It.first->second;
      if (!index->IndexState.compare_exchange_strong(empty, BSHeader)) {
        return index;
      }
    }
  } else {
    // Already have index for current block; state checked before
    index->Prev = prevIndex;
  }

  index->Header = header;

  // Try to continue chain
  index->ConcurrentHeaderNext = WaitPtr<BC::Common::BlockIndex>();
  index->ConcurrentHeaderNext = prevIndex->SuccessorHeaders.exchange(index, 0);
  if (index->ConcurrentHeaderNext.tag() == 1)
    BuildHeaderChain(context, prevIndex);

  return index;
}

BC::Common::BlockIndex *AddBlock(BCNodeContext &context, SerializedDataObject *serialized, BC::Common::CheckConsensusCtx &ccCtx, newBestCallback callback, uint32_t fileNo, uint32_t fileOffset)
{
  BC::Proto::Block *block = static_cast<BC::Proto::Block*>(serialized->unpackedData());

  // Check presence of this block
  BC::Proto::BlockHashTy hash = block->header.GetHash();
  BC::Common::BlockIndex *index = nullptr;
  bool alreadyHaveHeader = false;

  {
    auto It = context.blockIndex.find(hash);
    if (It != context.blockIndex.end()) {
      index = It->second;
      uint32_t prevIndexState = index->IndexState.exchange(BSBlock);
      if (prevIndexState == BSEmpty) {
        // Locked index stub
      } else if (prevIndexState == BSHeader) {
        // Locked header
        alreadyHaveHeader = true;
      } else {
        LOG_F(WARNING, "Already have block %s (%u)", hash.ToString().c_str(), index->Height);
        return index;
      }
    }
  }

  // Do all off-chain block checking here
  // Don't check PoW if we already have header
  if (!alreadyHaveHeader) {
    // Check consensus (such as PoW)
    if (!BC::Common::checkConsensus(block->header, ccCtx, context.chainParams)) {
      LOG_F(ERROR, "Check Proof-Of-Work failed for block %s", hash.ToString().c_str());
      return nullptr;
    }
  }

  {
    // Other checks
    if (calculateMerkleRoot(block->vtx) != block->header.hashMerkleRoot) {
      LOG_F(ERROR, "Merkle root invalid for block %s", hash.ToString().c_str());
      return nullptr;
    }
  }

  // Prepare block index structure for predecessor block
  auto prevIndex = BC::Common::BlockIndex::create(BSEmpty, nullptr);

  auto prevIt = context.blockIndex.insert(std::pair(block->header.hashPrevBlock, prevIndex));
  if (!prevIt.second) {
    delete prevIndex;
    prevIndex = prevIt.first->second;
  }

  // Try insert incoming block to index
  if (!index) {
    index = BC::Common::BlockIndex::create(BSBlock, prevIndex);
    auto It = context.blockIndex.insert(std::pair(hash, index));
    if (!It.second) {
      // Already have index for current block
      delete index;
      index = It.first->second;
      uint32_t prevIndexState = index->IndexState.exchange(BSBlock);
      if (prevIndexState == BSEmpty) {
        // Locked index stub
        // Delete recently allocated index
        index->Prev = prevIndex;
        index->Header = block->header;
      } else if (prevIndexState == BSHeader) {
        // Locked header
        alreadyHaveHeader = true;
      } else {
        LOG_F(WARNING, "Already have block %s (%u)", hash.ToString().c_str(), index->Height);
        return index;
      }
    } else {
      // New index created for current block; prev index already initialized
      index->Header = block->header;
    }
  } else {
    // Already have index for current block; state checked before
    if (!alreadyHaveHeader) {
      index->Prev = prevIndex;
      index->Header = block->header;
    }
  }

  // Continue header chain if we see header first time
  if (!alreadyHaveHeader) {
    index->ConcurrentHeaderNext = WaitPtr<BC::Common::BlockIndex>();
    index->ConcurrentHeaderNext = prevIndex->SuccessorHeaders.exchange(index, 0);
    if (index->ConcurrentHeaderNext.tag() == 1)
      BuildHeaderChain(context, prevIndex);
  }

  // Try to continue chain
  index->Serialized.reset(serialized);
  index->FileNo = fileNo;
  index->FileOffset = fileOffset;
  index->SerializedBlockSize = static_cast<uint32_t>(serialized->size());
  index->ConcurrentBlockNext = WaitPtr<BC::Common::BlockIndex>();
  index->ConcurrentBlockNext = prevIndex->SuccessorBlocks.exchange(index, 0);
  if (index->ConcurrentBlockNext.tag() == 1) {
    std::vector<BC::Common::BlockIndex*> acceptedBlocks;
    context.ChainStateCombiner.call(static_cast<BlockProcessingTask*>(prevIndex), [&context, &acceptedBlocks](BC::Common::BlockIndex *index) {
      buildBlockChain(context, index, acceptedBlocks);
    });

    if (!acceptedBlocks.empty() && context.storageEvent) {
      userEventActivate(context.storageEvent);
      if (callback)
        callback(acceptedBlocks);
    }
  }

  return index;
}

bool loadBlocks(BCNodeContext &context, const char *path, uint8_t *data, BlockPosition *position, size_t blocksNum, uint32_t fileNo)
{
  BC::Common::CheckConsensusCtx ccCtx;
  BC::Common::checkConsensusInitialize(ccCtx);

  for (size_t i = 0; i < blocksNum; i++) {
    xmstream stream(data + position[i].offset + 8, position[i].size);
    xmstream unpacked(stream.sizeOf()*2);
    BC::unpack<BC::Proto::Block>(stream, unpacked);
    if (stream.eof() || stream.remaining() != 0) {
      LOG_F(ERROR, "Can't parse block file %s (invalid block structure)", path);
      return false;
    }

    size_t unpackedMemorySize = unpacked.capacity();
    auto object = context.BlockCache.add(nullptr, stream.sizeOf(), 0, unpacked.capture(), unpackedMemorySize);
    if (!AddBlock(context, object.get(), ccCtx, nullptr, fileNo, position[i].offset)) {
      LOG_F(ERROR, "Can't parse block file %s (invalid block structure)", path);
      return false;
    }
  }

  return true;
}

static bool loadBlockIndexDeserializer(BCNodeContext &context, LoadingIndexContext &loadingIndexContext, std::vector<size_t> &blockFileSizes, RawData *data, size_t indexesNum, const char *path)
{
  for (size_t i = 0; i < indexesNum; i++) {
    BC::Common::BlockIndex *index = BC::Common::BlockIndex::create(BSBlock, nullptr);
    xmstream stream(data[i].data, data[i].size);
    if (!BC::unserializeAndCheck(stream, *index)) {
      LOG_F(ERROR, "Can't read index data from %s", path);
      return false;
    }

    index->OnChain = true;
    index->SuccessorHeaders.set(nullptr, 1);
    index->SuccessorBlocks.set(nullptr, 1);

    // Quick check of presence block on disk
    if (!(index->FileNo <= blockFileSizes.size() &&
          index->FileOffset < blockFileSizes[index->FileNo])) {
      LOG_F(ERROR, "Index loader: no block data on disk for %s", index->Header.GetHash().ToString().c_str());
      return false;
    }

    // Check proof of work if need

    context.blockIndex.insert(std::pair(index->Header.GetHash(), index));
    loadingIndexContext.allIndexes.push_back(index);
    if (loadingIndexContext.bestIndex == nullptr || index->ChainWork > loadingIndexContext.bestIndex->ChainWork)
      loadingIndexContext.bestIndex = index;
  }

  return true;
}

static bool loadBlockIndexBuilder(BCNodeContext &context, LoadingIndexContext *loadingIndexContext)
{
  for (auto &index: loadingIndexContext->allIndexes) {
    auto It = context.blockIndex.find(index->Header.hashPrevBlock);
    if (It == context.blockIndex.end()) {
      LOG_F(ERROR, "Can't find previous block %s for %s (%u)", index->Header.hashPrevBlock.ToString().c_str(), index->Header.GetHash().ToString().c_str(), index->Height);
      return false;
    }

    index->Prev = It->second;
  }

  return true;
}

bool loadingBlockIndex(BCNodeContext &context)
{
  LOG_F(INFO, "Loading block index...");

  char fileName[64];
  uint32_t indexFileNo = 0;
  std::filesystem::path indexPath = context.dataDir / "index";
  std::filesystem::path blocksPath = context.dataDir / "blocks";
  std::vector<size_t> blockFileSizes;

  // Collect block data file sizes
  for (;;) {
    snprintf(fileName, sizeof(fileName), "blk%05u.dat", indexFileNo++);
    std::filesystem::path path = blocksPath / fileName;
    if (!std::filesystem::exists(path))
      break;

    blockFileSizes.push_back(std::filesystem::file_size(path));
  }

  indexFileNo = 0;
  unsigned threadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;
  std::unique_ptr<LoadingIndexContext[]> loadingIndexContext(new LoadingIndexContext[threadsNum]);
  std::unique_ptr<std::future<bool>[]> workers(new std::future<bool>[threadsNum]);

  std::vector<RawData> offsets;
  for (;;) {
    snprintf(fileName, sizeof(fileName), "index%05u.dat", indexFileNo++);
    std::filesystem::path path = indexPath / fileName;
    if (!std::filesystem::exists(path))
      break;

    size_t indexFileSize = std::filesystem::file_size(path);
    std::unique_ptr<uint8_t[]> data(new uint8_t[indexFileSize]);

    if (indexFileSize) {
      // Read index file
      std::unique_ptr<FILE, std::function<void(FILE*)>> hFile(fopen(path.u8string().c_str(), "rb"), [](FILE *f) { fclose(f); });
      if (!hFile.get()) {
        LOG_F(ERROR, "Can't open index file %s", path.u8string().c_str());
        return false;
      }

      if (fread(data.get(), indexFileSize, 1, hFile.get()) != 1) {
        LOG_F(ERROR, "Can't read index file %s", path.u8string().c_str());
        return false;
      }
    }

    {
      offsets.clear();
      xmstream stream(data.get(), indexFileSize);
      while (stream.remaining()) {
        uint32_t size;
        BC::unserialize(stream, size);
        if (!size || size > stream.remaining()) {
          LOG_F(ERROR, "Invalid index size %u detected in file %s", size, path.u8string().c_str());
          return false;
        }

        RawData data;
        data.data = stream.seek<uint8_t>(size);
        data.size = size;
        offsets.push_back(data);
      }
    }

    if (offsets.empty())
      continue;

    size_t workLoad = offsets.size() / threadsNum;
    size_t workLoadExtra = offsets.size() % threadsNum;
    size_t offset = 0;
    for (unsigned i = 0; i < threadsNum; i++) {
      size_t off = offset;
      size_t size = workLoad + (i < workLoadExtra);
      offset += size;
      workers[i] = std::async(std::launch::async, loadBlockIndexDeserializer, std::ref(context), std::ref(loadingIndexContext[i]), std::ref(blockFileSizes), &offsets[0] + off, size, path.u8string().c_str());
    }

    for (unsigned i = 0; i < threadsNum; i++) {
      if (!workers[i].get()) {
        return false;
      }
    }
  }

  // Make links to previous blocks
  for (unsigned i = 0; i < threadsNum; i++)
    workers[i] = std::async(loadBlockIndexBuilder, std::ref(context), &loadingIndexContext[i]);
  for (unsigned i = 0; i < threadsNum; i++) {
    if (!workers[i].get())
      return false;
  }

  uint64_t blocksNum = 0;
  BC::Common::BlockIndex *bestIndex = nullptr;
  for (unsigned i = 0; i < threadsNum; i++) {
    blocksNum += loadingIndexContext[i].allIndexes.size();
    if (bestIndex == nullptr || (loadingIndexContext[i].bestIndex && loadingIndexContext[i].bestIndex->ChainWork > bestIndex->ChainWork))
      bestIndex = loadingIndexContext[i].bestIndex;
  }

  LOG_F(INFO, "Loaded %zu blocks", static_cast<size_t>(blocksNum));

  if (blocksNum == 0)
    bestIndex = context.genesisIndex;

  LOG_F(INFO, "Found best index: %s (%u)", bestIndex->Header.GetHash().ToString().c_str(), bestIndex->Height);
  LOG_F(INFO, "Restore best chain...");

  BC::Common::BlockIndex *index = bestIndex;
  while (index) {
    context.blockHeightIndex[index->Height] = index;

    BC::Common::BlockIndex *prevIndex = index->Prev;
    if (prevIndex) {
      if (prevIndex->Height != index->Height-1) {
        LOG_F(ERROR, "Index loader: block %s (%u) have invalid previous block %s with height %u", index->Header.GetHash().ToString().c_str(), index->Height, prevIndex->Header.GetHash().ToString().c_str(), prevIndex->Height);
        return false;
      }

      prevIndex->Next = index;
    }

    index = prevIndex;
  }

  context.BestIndex = bestIndex;
  LOG_F(INFO, "Loading index done");
  return true;
}

bool reindex(BCNodeContext &context)
{
  size_t bufferSize = 0;
  std::unique_ptr<uint8_t[]> data;
  char blockFileName[64];
  unsigned blkFileIndex = 0;
  std::filesystem::path indexPath = context.dataDir / "index";
  std::filesystem::path blocksPath = context.dataDir / "blocks";
  size_t totalBlockCount = 0;

  std::vector<BlockPosition> blockOffsets;
  unsigned threadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;

  // Cleanup index directory
  for (std::filesystem::directory_iterator I(indexPath), IE; I != IE; ++I)
    std::filesystem::remove_all(I->path());

  // Initialize local index storage
  LinearDataWriter indexStorageWriter;
  indexStorageWriter.init(indexPath, "index%05u.dat", BC::Common::BlocksFileLimit);

  auto indexWriter = std::async(std::launch::async, [&context, &indexStorageWriter]() -> bool {
    xmstream stream;
    for (;;) {
      StoreTask *task;
      if (!context.storeQueue.try_pop(task)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }

      if (!task)
        break;

      std::pair<uint32_t, uint32_t> position;

      stream.reset();
      BC::serialize(stream, *task->index);
      uint32_t serializedSize = static_cast<uint32_t>(stream.sizeOf());
      if (!indexStorageWriter.write(&serializedSize, sizeof(serializedSize), stream.data(), static_cast<uint32_t>(stream.sizeOf()), position))
        return false;
      task->index->Serialized.reset();
      delete task;
    }

    return true;
  });

  for (;;) {
    snprintf(blockFileName, sizeof(blockFileName), "blk%05u.dat", blkFileIndex);
    std::filesystem::path path = blocksPath / blockFileName;
    if (!std::filesystem::exists(path))
      break;

    LOG_F(INFO, "Loading block file %s ...", path.c_str());

    // Allocate memory for block file
    size_t blockFileSize = std::filesystem::file_size(path);
    if (blockFileSize > bufferSize) {
      data.reset(new uint8_t[blockFileSize]);
      bufferSize = blockFileSize;
    }

    {
      // Read block file
      std::unique_ptr<FILE, std::function<void(FILE*)>> hFile(fopen(path.u8string().c_str(), "rb"), [](FILE *f) { fclose(f); });
      if (!hFile.get()) {
        LOG_F(ERROR, "Can't open block file %s", path.c_str());
        context.storeQueue.push(nullptr);
        return false;
      }

      if (fread(data.get(), 1, blockFileSize, hFile.get()) != blockFileSize) {
        LOG_F(ERROR, "Can't read block file %s", path.c_str());
        context.storeQueue.push(nullptr);
        return false;
      }
    }

    {
      blockOffsets.clear();
      xmstream stream(data.get(), blockFileSize);
      while (stream.remaining()) {
        uint32_t magic;
        uint32_t blockSize;
        BC::unserialize(stream, magic);
        BC::unserialize(stream, blockSize);
        if (!magic && !blockSize)
          continue;

        if (magic != context.chainParams. magic) {
          LOG_F(ERROR, "Can't parse block file %s (invalid magic)", path.c_str());
          context.storeQueue.push(nullptr);
          return false;
        }

        BlockPosition data;
        data.offset = static_cast<uint32_t>(stream.offsetOf() - 8);
        data.size = blockSize;
        blockOffsets.push_back(data);
        stream.seek<uint8_t>(blockSize);
      }
    }

    size_t workLoad = blockOffsets.size() / threadsNum;
    size_t workLoadExtra = blockOffsets.size() % threadsNum;
    size_t blockOffset = 0;
    std::unique_ptr<std::future<bool>[]> workers(new std::future<bool>[threadsNum]);
    for (unsigned i = 0; i < threadsNum; i++) {
      size_t off = blockOffset;
      size_t size = workLoad + (i < workLoadExtra);
      blockOffset += size;
      workers[i] = std::async(std::launch::async, loadBlocks, std::ref(context), path.u8string().c_str(), data.get(), &blockOffsets[0] + off, size, blkFileIndex);
    }

    for (unsigned i = 0; i < threadsNum; i++) {
      if (!workers[i].get()) {
        context.storeQueue.push(nullptr);
        return false;
      }
    }

    LOG_F(INFO, "%u blocks loaded from %s; cache size: %.3lfM", static_cast<unsigned>(blockOffsets.size()), path.c_str(), context.BlockCache.size() / 1048576.0f);
    totalBlockCount += blockOffsets.size();
    blkFileIndex++;
  }

  context.storeQueue.push(nullptr);
  if (!indexWriter.get())
    return false;
  if (!indexStorageWriter.flush())
    return false;

  auto best = context.BestIndex.load();
  LOG_F(INFO, "%zu blocks loaded from disk", totalBlockCount);
  LOG_F(INFO, "Best block is %s (%u)", best->Header.GetHash().ToString().c_str(), best->Height);
  return true;
}

static void storageTimerCb(aioUserEvent*, void *arg)
{
  BCNodeContext *context = static_cast<BCNodeContext*>(arg);
  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::seconds>(now - context->blockStorageWriter.lastWriteTime()).count() >= 10) {
    if (!context->blockStorageWriter.flush())
      shutdown(*context);
    std::for_each(context->cachedBlocks.begin(), context->cachedBlocks.end(), [](BC::Common::BlockIndex *index) {
      index->Serialized.reset();
    });
    context->cachedBlocks.clear();
  }
  if (std::chrono::duration_cast<std::chrono::seconds>(now - context->indexStorageWriter.lastWriteTime()).count() >= 10) {
    if (!context->indexStorageWriter.flush())
      shutdown(*context);
  }
  userEventStartTimer(context->storageTimer, 4*100000, 1);
}

static void storageEventCb(aioUserEvent*, void *arg)
{
  BCNodeContext *context = static_cast<BCNodeContext*>(arg);

  StoreTask *task;
  while (context->storeQueue.try_pop(task)) {
    context->cachedBlocks.push_back(task->index);

    std::pair<uint32_t, uint32_t> position;
    const SerializedDataObject *serialized = task->index->Serialized.get();
    uint32_t prefix[2] = { context->chainParams.magic, task->index->SerializedBlockSize };
    if (!context->blockStorageWriter.write(prefix, sizeof(prefix), serialized->data(), static_cast<uint32_t>(serialized->size()), position)) {
      shutdown(*context);
      return;
    }

    if (context->blockStorageWriter.bufferEmpty()) {
      std::for_each(context->cachedBlocks.begin(), context->cachedBlocks.end(), [](BC::Common::BlockIndex *index) {
        index->Serialized.reset();
      });
      context->cachedBlocks.clear();
    }

    {
      // Serialize index for storage
      uint8_t buffer[1024];
      xmstream indexData(buffer, sizeof(buffer));
      task->index->FileNo = position.first;
      task->index->FileOffset = position.second;
      indexData.reset();
      BC::serialize(indexData, *task->index);
      uint32_t serializedSize = static_cast<uint32_t>(indexData.sizeOf());
      if (!context->indexStorageWriter.write(&serializedSize, sizeof(serializedSize), indexData.data(), static_cast<uint32_t>(indexData.sizeOf()), position)) {
        // TODO: shutdown
        shutdown(*context);
        return;
      }
    }

    delete task;
  }
}

bool initializeStorage(BCNodeContext &context, std::thread &storageThread)
{
  context.blockStorageReader.init(context.dataDir / "blocks", "blk%05u.dat");
  if (!context.blockStorageWriter.init(context.dataDir / "blocks", "blk%05u.dat", BC::Common::BlocksFileLimit))
    return false;
  if (!context.indexStorageWriter.init(context.dataDir / "index", "index%05u.dat", BC::Common::BlocksFileLimit))
    return false;

  if (context.blockStorageWriter.empty()) {
    // Store genesis block to disk (for compatibility with core clients)
    // Serialize block using storage format:
    //   <magic>:4 <blockSize>:4 <block>
    std::pair<uint32_t, uint32_t> position;
    xmstream stream;
    BC::serialize(stream, context.chainParams.GenesisBlock);
    uint32_t prefix[2] = {context.chainParams.magic, static_cast<uint32_t>(stream.sizeOf())};
    if (!context.blockStorageWriter.write(prefix, sizeof(prefix), stream.data(), static_cast<uint32_t>(stream.sizeOf()), position))
      shutdown(context);
  }

  context.storageBase = createAsyncBase(amOSDefault);
  context.storageEvent = newUserEvent(context.storageBase, 0, storageEventCb, &context);
  context.storageTimer = newUserEvent(context.storageBase, 0, storageTimerCb, &context);
  std::thread thread([](asyncBase *base) {
    loguru::set_thread_name("storage");
    asyncLoop(base);
  }, context.storageBase);

  userEventStartTimer(context.storageTimer, 10*100000, 1);
  storageThread = std::move(thread);
  return true;
}

void shutdownStorage(BCNodeContext &context)
{
  deleteUserEvent(context.storageEvent);
  deleteUserEvent(context.storageTimer);
  postQuitOperation(context.storageBase);
}

BlockSearcher::BlockSearcher(BCNodeContext &context) : context(&context) {
  blocksDirectory = context.dataDir / "blocks";
}

BC::Common::BlockIndex *BlockSearcher::add(const BC::Proto::BlockHashTy &hash)
{
  auto It = context->blockIndex.find(hash);
  if (It != context->blockIndex.end()) {
    BC::Common::BlockIndex *index = It->second;
    intrusive_ptr<const SerializedDataObject> serializedPtr(index->Serialized);
    if (const SerializedDataObject *serialized = serializedPtr.get()) {
      fileNo = std::numeric_limits<uint32_t>::max();
      stream.reset();
      // Serialize block using storage format:
      //   <magic>:4 <blockSize>:4 <block>
      // TODO: remove memory copying from here
      BC::serialize(stream, context->chainParams.magic);
      BC::serialize(stream, static_cast<uint32_t>(0));
      stream.write(serialized->data(), serialized->size());
      stream.data<uint32_t>()[1] = static_cast<uint32_t>(stream.sizeOf()) - 8;
      stream.seekSet(0);
      return index;
    } else if (index->FileNo != std::numeric_limits<uint32_t>::max() &&
               index->FileOffset != std::numeric_limits<uint32_t>::max() &&
               index->SerializedBlockSize != std::numeric_limits<uint32_t>::max()) {

      if (fileNo == std::numeric_limits<uint32_t>::max()) {
        fileNo = index->FileNo;
        fileOffsetBegin = index->FileOffset;
        fileOffsetCurrent = fileOffsetBegin + index->SerializedBlockSize + 8;
      } else if (fileNo == index->FileNo && fileOffsetCurrent == index->FileOffset) {
        fileOffsetCurrent = index->FileOffset + index->SerializedBlockSize + 8;
      } else {
        fetchPending();
        fileNo = index->FileNo;
        fileOffsetBegin = index->FileOffset;
        fileOffsetCurrent = fileOffsetBegin + index->SerializedBlockSize + 8;
      }

      if (fileOffsetCurrent - fileOffsetBegin >= MaxIoSize)
        fetchPending();

      return index;
    }
  }

  return nullptr;
}

void *BlockSearcher::next(size_t *size)
{
  if (!stream.remaining())
    return nullptr;

  uint32_t magic;
  uint32_t blockSize;
  BC::unserialize(stream, magic);
  BC::unserialize(stream, blockSize);
  void *data = stream.seek<uint8_t>(blockSize);
  if (magic != context->chainParams.magic || stream.eof()) {
    char fileName[64];
    snprintf(fileName, sizeof(fileName), "blk%05u.dat", fileNo);
    std::filesystem::path path = blocksDirectory / fileName;
    LOG_F(ERROR, "Invalid block data in file %s", path.c_str());
    shutdown(*context);
    return nullptr;
  }

  *size = blockSize;
  return data;
}

void BlockSearcher::fetchPending()
{
  stream.reset();
  uint32_t size = fileOffsetCurrent - fileOffsetBegin;
  if (fileNo != std::numeric_limits<uint32_t>::max() && size && !context->blockStorageReader.read(fileNo, fileOffsetBegin, stream.reserve(size), size)) {
    LOG_F(ERROR, "Can't read data from %s (offset = %u, size = %u)", context->blockStorageReader.getFilePath(fileNo).c_str(), fileOffsetBegin, size);
    shutdown(*context);
  }
  stream.seekSet(0);
}
