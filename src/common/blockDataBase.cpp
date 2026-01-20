// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockDataBase.h"
#include "db/storage.h"
#include "common/fopen.h"
#include "common/serializeUtils.h"
#include <asyncio/asyncio.h>
#include <p2putils/coreTypes.h>
#include <p2putils/xmstream.h>
#include "loguru.hpp"
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

bool initializeLinkedOutputs(BC::Proto::CBlockLinkedOutputs &linkedOutputs, BC::Proto::Block &block, const BC::DB::UTXODb &db)
{
  std::unordered_map<BC::Proto::TxHashTy, uint32_t> txIndexMap;
  std::unordered_set<CUnspentOutputKey> removed;

  linkedOutputs.Tx.resize(block.vtx.size());

  if (!block.vtx.empty())
    txIndexMap[block.vtx[0].getTxId()] = 0;

  bool allOutputsFound = true;
  for (size_t txIdx = 1; txIdx < block.vtx.size(); txIdx++) {
    BC::Proto::Transaction &tx = block.vtx[txIdx];
    auto &txLinked = linkedOutputs.Tx[txIdx];

    txLinked.TxIn.resize(tx.txIn.size());
    for (size_t txinIdx = 0; txinIdx < tx.txIn.size(); txinIdx++) {
      const auto &txin = tx.txIn[txinIdx];
      auto &txinLinked = txLinked.TxIn[txinIdx];

      if (db.queryCache(txin.previousOutputHash, txin.previousOutputIndex, txinLinked)) {
        // Unspent output found in cache
      } else {
        // Try find in local block
        auto It = txIndexMap.find(txin.previousOutputHash);
        if (It != txIndexMap.end()) {
          BC::Proto::Transaction &localReferencedTx = block.vtx[It->second];
          if (txin.previousOutputIndex >= localReferencedTx.txOut.size())
            return false;
          CUnspentOutputKey key;
          key.Tx = txin.previousOutputHash;
          key.Index = txin.previousOutputIndex;
          if (!removed.insert(key).second)
            return false;

          xmstream s;
          BC::Script::parseTransactionOutput(localReferencedTx.txOut[txin.previousOutputIndex], s);
          BTC::Script::UnspentOutputInfo *info = s.data<BTC::Script::UnspentOutputInfo>();
          info->IsLocalTx = 1;
          xvectorFromStream(std::move(s), txinLinked);
        } else {
          allOutputsFound = false;
        }
      }
    }

    txIndexMap[tx.getTxId()] = txIdx;
  }

  linkedOutputs.AllOutputsFound = allOutputsFound;
  return true;
}

bool initializeLinkedOutputsContextual(BC::Proto::CBlockLinkedOutputs &linkedOutputs, BC::Proto::Block &block, const BC::DB::UTXODb &db)
{
  linkedOutputs.Tx.resize(block.vtx.size());

  for (size_t txIdx = 1; txIdx < block.vtx.size(); txIdx++) {
    BC::Proto::Transaction &tx = block.vtx[txIdx];
    auto &txLinked = linkedOutputs.Tx[txIdx];

    txLinked.TxIn.resize(tx.txIn.size());

    for (size_t txinIdx = 0; txinIdx < tx.txIn.size(); txinIdx++) {
      const auto &txin = tx.txIn[txinIdx];
      auto &txinLinked = txLinked.TxIn[txinIdx];
      if (!txinLinked.empty())
        continue;

      if (!db.query(txin.previousOutputHash, txin.previousOutputIndex, txinLinked)) {
        LOG_F(ERROR,
              " * Transaction %s refers non-existing utxo %s:%u",
              tx.getTxId().getHexLE().c_str(),
              txin.previousOutputHash.getHexLE().c_str(),
              txin.previousOutputIndex);
        // exit(1);
        return false;
      }
    }
  }

  return true;
}

BC::Common::BlockIndex *rebaseChain(BC::Common::BlockIndex *newBest,
                                    BC::Common::BlockIndex *previousBest,
                                    std::vector<BC::Common::BlockIndex*> &forDisconnect)
{
  // New best block found
  if (newBest->Prev == previousBest) {
    return newBest;
  } else {
    // Rebuild chain from least common ancestor
    BC::Common::BlockIndex *lb;
    BC::Common::BlockIndex *sb;
    if (newBest->Height >= previousBest->Height) {
      lb = newBest;
      sb = previousBest;
      uint32_t sbHeight = sb->Height;
      while (lb->Height > sbHeight) {
        lb = lb->Prev;
      }
      while (sb != lb) {
        forDisconnect.push_back(sb);
        sb = sb->Prev;
        lb = lb->Prev;
      }

    } else {
      lb = previousBest;
      sb = newBest;
      uint32_t sbHeight = sb->Height;
      while (lb->Height > sbHeight) {
        forDisconnect.push_back(lb);
        lb = lb->Prev;
      }
      while (sb != lb) {
        forDisconnect.push_back(lb);
        sb = sb->Prev;
        lb = lb->Prev;
      }
    }

    return sb;
  }
}

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

static bool ConnectBlock(BC::Common::BlockIndex *index,
                         BC::Proto::Block &block,
                         BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                         BC::Proto::CBlockValidationData &validationData,
                         BC::Common::ChainParams &chainParams,
                         BlockInMemoryIndex &blockIndex,
                         BC::DB::Storage &storage,
                         bool silent = true)
{
  // Locate linked transaction outputs
  if (!linkedOutputs.AllOutputsFound && !initializeLinkedOutputsContextual(linkedOutputs, block, storage.utxodb())) {
    LOG_F(ERROR,
          "Block %s validation failed (non-existent utxo)",
          block.header.GetHash().getHexLE().c_str());
    return false;
  }

  std::string error;
  if (!BC::Common::checkBlockContextual(*index, block, validationData, linkedOutputs, chainParams, error))
    return false;

  if (!silent)
    LOG_F(INFO, "Connect block %s (%u)", index->Header.GetHash().getHexLE().c_str(), index->Height);
  index->Prev->Next = index;
  blockIndex.blockHeightIndex()[index->Height] = index;
  storage.add(BC::DB::Connect, index, block, linkedOutputs, blockIndex);
  blockIndex.setBest(index);
  return true;
}

static void DisconnectBlock(BlockInMemoryIndex &blockIndex,
                            BC::Proto::Block &block,
                            BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            BC::DB::Storage &storage,
                            BC::Common::BlockIndex *index,
    bool silent = true)
{
  if (!silent)
    LOG_F(INFO, "Disconnect block %s (%u)", index->Header.GetHash().getHexLE().c_str(), index->Height);
  index->Prev->Next = nullptr;
  blockIndex.blockHeightIndex()[index->Height] = nullptr;
  storage.add(BC::DB::Disconnect, index, block, linkedOutputs, blockIndex);
}

static void BuildHeaderChain(BC::Common::ChainParams &chainParams, BC::Common::BlockIndex *start)
{
  BC::Common::BlockIndex *currentStart = start;
  std::deque<BC::Common::BlockIndex*> queue;

  QueueNextHeaders(queue, currentStart);

  while (!queue.empty()) {
    BC::Common::BlockIndex *current = queue.front();
    BC::Common::BlockIndex *prev = current->Prev;

    if (current->Height == std::numeric_limits<uint32_t>::max()) {
      current->Height = prev->Height + 1;
      current->ChainWork = prev->ChainWork + BC::Common::GetBlockProof(current->Header, chainParams);
    }

    QueueNextHeaders(queue, current);
    queue.pop_front();
  }
}

intrusive_ptr<BC::Common::CIndexCacheObject> objectByIndex(BC::Common::BlockIndex *index, BlockDatabase &blockDb)
{
  {
    intrusive_ptr<BC::Common::CIndexCacheObject> object(index->Serialized);
    if (object.get())
      return object;
  }

  // Load block data
  std::unique_ptr<uint8_t[]> serialized(new uint8_t[index->SerializedBlockSize]);
  std::unique_ptr<uint8_t[]> linkedOutputsData(new uint8_t[index->LinkedOutputsSerializedSize]);

  assert(index->blockStored());
  if (!blockDb.blockReader().read(index->FileNo,
                                  index->FileOffset + 8,
                                  serialized.get(),
                                  index->SerializedBlockSize))
    return nullptr;

  assert(index->indexStored());
  if (!blockDb.linkedOutputsReader().read(index->LinkedOutputsFileNo,
                                          index->LinkedOutputsFileOffset + 4,
                                          linkedOutputsData.get(),
                                          index->LinkedOutputsSerializedSize))
    return nullptr;



  size_t unpackedSize = 0;
  xmstream stream(serialized.get(), index->SerializedBlockSize);
  BC::Proto::Block *block = BTC::unpack2<BC::Proto::Block>(stream, &unpackedSize);
  if (!block)
    return nullptr;

  // Create block object
  intrusive_ptr<BC::Common::CIndexCacheObject> object(new BC::Common::CIndexCacheObject(nullptr,
                                                                                        nullptr,
                                                                                        index->SerializedBlockSize,
                                                                                        0,
                                                                                        block,
                                                                                        unpackedSize));

  {
    xmstream stream(linkedOutputsData.get(), index->LinkedOutputsSerializedSize);
    if (!BTC::unserializeAndCheck(stream, object.get()->linkedOutputs()))
      return nullptr;
    object.get()->linkedOutputs().AllOutputsFound = true;
  }

  return object;
}

static intrusive_ptr<BC::Common::CIndexCacheObject> objectByIndexChecked(BC::Common::BlockIndex *index, BlockDatabase &blockDb)
{
  auto object = objectByIndex(index, blockDb);
  if (!object.get()) {
    LOG_F(ERROR, "Block index corrupted, failed to load block [%u]%s", index->Height, index->Header.GetHash().getHexLE().c_str());
    abort();
  }

  return object;
}

static bool switchTo(BC::Common::BlockIndex *newBest,
                     BC::Common::ChainParams &chainParams,
                     BlockInMemoryIndex &blockIndex,
                     BC::DB::Storage &storage)
{
  BC::Common::BlockIndex *currentBest = blockIndex.best();

  // Rebuild chain from least common ancestor
  std::vector<BC::Common::BlockIndex*> newPath;
  BC::Common::BlockIndex *lb;
  BC::Common::BlockIndex *sb;
  if (newBest->Height >= currentBest->Height) {
    lb = newBest;
    sb = currentBest;
    uint32_t sbHeight = sb->Height;
    while (lb->Height > sbHeight) {
      newPath.push_back(lb);
      lb = lb->Prev;
    }
    while (sb != lb) {
      newPath.push_back(lb);
      auto object = objectByIndexChecked(sb, storage.blockDb());
      DisconnectBlock(blockIndex, *object.get()->block(), object.get()->linkedOutputs(), storage, sb, false);
      sb = sb->Prev;
      lb = lb->Prev;
    }

  } else {
    lb = currentBest;
    sb = newBest;
    uint32_t sbHeight = sb->Height;
    while (lb->Height > sbHeight) {
      BC::Proto::Block diskBlock;
      auto object = objectByIndexChecked(lb, storage.blockDb());
      DisconnectBlock(blockIndex, *object.get()->block(), object.get()->linkedOutputs(), storage, lb, false);
      lb = lb->Prev;
    }
    while (sb != lb) {
      BC::Proto::Block diskBlock;
      newPath.push_back(sb);
      auto object = objectByIndexChecked(lb, storage.blockDb());
      DisconnectBlock(blockIndex, *object.get()->block(), object.get()->linkedOutputs(), storage, lb, false);
      sb = sb->Prev;
      lb = lb->Prev;
    }
  }

  // Connect blocks from new path
  for (auto I = newPath.rbegin(), IE = newPath.rend(); I != IE; ++I) {
    auto object = objectByIndexChecked(*I, storage.blockDb());
    if (!ConnectBlock(*I, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationData(), chainParams, blockIndex, storage, false)) {
      (*I)->IndexState = BSInvalid;
      return false;
    }
  }

  return true;
}

static void buildBlockChain(BlockInMemoryIndex &blockIndex, BC::Common::ChainParams &chainParams, BC::DB::Storage &storage, BC::Common::BlockIndex *start, std::vector<BC::Common::BlockIndex*> &acceptedBlocks)
{
  BC::Common::BlockIndex *currentStart = start;
  std::deque<BC::Common::BlockIndex*> queue;

  QueueNextBlocks(queue, currentStart);
  while (!queue.empty()) {
    BC::Common::BlockIndex *current = queue.front();
    BC::Common::BlockIndex *prev = current->Prev;
    BC::Common::CIndexCacheObject *object = current->Serialized.get();
    BC::Proto::Block *block = object->block();
    BC::Proto::CBlockValidationData &validationData = object->validationData();
    BC::Proto::CBlockLinkedOutputs &linkedOutputs = object->linkedOutputs();

    current->OnChain = prev->OnChain;
    if (current->Height == std::numeric_limits<uint32_t>::max()) {
      current->Height = prev->Height + 1;
      current->ChainWork = prev->ChainWork + BC::Common::GetBlockProof(current->Header, chainParams);
    }

    BC::Common::BlockIndex *currentBest = blockIndex.best();
    if (current->ChainWork > currentBest->ChainWork) {
      // New best block found
      if (current->Prev == currentBest) {
        // Connect
        if (!ConnectBlock(current, *block, linkedOutputs, validationData, chainParams, blockIndex, storage)) {
          current->IndexState = BSInvalid;
          QueueNextBlocks(queue, current);
          queue.pop_front();
          continue;
        }
      } else {
        // Rebuild chain from least common ancestor
        if (!switchTo(current, chainParams, blockIndex, storage)) {
          if (!switchTo(currentBest, chainParams, blockIndex, storage)) {
            // Abnormal situation
            LOG_F(ERROR, "Block database corrupted");
            abort();
          }

          current->IndexState = BSInvalid;
          QueueNextBlocks(queue, current);
          queue.pop_front();
          continue;
        }
      }
    } else if (linkedOutputs.AllOutputsFound) {
      storage.add(BC::DB::WriteData, current, *block, linkedOutputs, blockIndex, false);
    }

    // drop block data cache for connected block
    acceptedBlocks.push_back(current);
    QueueNextBlocks(queue, current);
    queue.pop_front();
  }
}

BC::Common::BlockIndex *AddHeader(BlockInMemoryIndex &blockIndex, BC::Common::ChainParams &chainParams, const BC::Proto::BlockHeader &header, BC::Common::CheckConsensusCtx &ccCtx)
{
  // Check presence of this block
  BlockStatus empty = BSEmpty;
  BC::Proto::BlockHashTy hash = header.GetHash();
  BC::Common::BlockIndex *index = nullptr;

  {
    auto It = blockIndex.blockIndex().find(hash);
    if (It != blockIndex.blockIndex().end()) {
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
  if (!BC::Common::checkConsensus(header, ccCtx, chainParams)) {
    LOG_F(ERROR, "Check Proof-Of-Work failed for block %s", hash.getHexLE().c_str());
    return nullptr;
  }

  // Prepare block index structure for predecessor block
  auto prevIndex = BC::Common::BlockIndex::create(BSEmpty, nullptr);

  auto prevIt = blockIndex.blockIndex().insert(std::pair(header.hashPrevBlock, prevIndex));
  if (!prevIt.second) {
    delete prevIndex;
    prevIndex = prevIt.first->second;
  }

  // Try insert incoming block to index
  if (!index) {
    index = BC::Common::BlockIndex::create(BSHeader, prevIndex);
    auto It = blockIndex.blockIndex().insert(std::pair(hash, index));
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
    BuildHeaderChain(chainParams, prevIndex);

  return index;
}

BC::Common::BlockIndex *AddBlock(BlockInMemoryIndex &blockIndex,
                                 BC::Common::ChainParams &chainParams,
                                 BC::DB::Storage &storage,
                                 BC::Common::CIndexCacheObject *serialized,
                                 BC::Common::CheckConsensusCtx &ccCtx,
                                 newBestCallback callback,
                                 uint32_t fileNo,
                                 uint32_t fileOffset)
{
  BC::Proto::Block *block = serialized->block();

  // Check presence of this block
  BC::Proto::BlockHashTy hash = block->header.GetHash();
  BC::Common::BlockIndex *index = nullptr;
  bool alreadyHaveHeader = false;

  {
    auto It = blockIndex.blockIndex().find(hash);
    if (It != blockIndex.blockIndex().end()) {
      index = It->second;
      uint32_t prevIndexState = index->IndexState.exchange(BSBlock);
      if (prevIndexState == BSEmpty) {
        // Locked index stub
      } else if (prevIndexState == BSHeader) {
        // Locked header
        alreadyHaveHeader = true;
      } else {
        LOG_F(WARNING, "Already have block %s (%u)", hash.getHexLE().c_str(), index->Height);
        return index;
      }
    }
  }

  // Do all off-chain block checking here
  // Don't check PoW if we already have header
  if (!alreadyHaveHeader) {
    // Check consensus (such as PoW)
    if (!BC::Common::checkConsensus(block->header, ccCtx, chainParams)) {
      LOG_F(ERROR, "Check Proof-Of-Work failed for block %s", hash.getHexLE().c_str());
      return nullptr;
    }
  }

  // Validate block
  initializeLinkedOutputs(serialized->linkedOutputs(), *block, storage.utxodb());
  BC::Common::initializeValidationContext(*block, serialized->validationData());

  std::string error;
  unsigned blockGeneration = BC::Common::checkBlockStandalone(*block, serialized->validationData(), chainParams, error);
  if (blockGeneration == 0) {
    LOG_F(WARNING, "block %s check failed, error: %s", block->header.GetHash().getHexLE().c_str(), error.c_str());
    return nullptr;
  }

  // Prepare block index structure for predecessor block
  auto prevIndex = BC::Common::BlockIndex::create(BSEmpty, nullptr);

  auto prevIt = blockIndex.blockIndex().insert(std::pair(block->header.hashPrevBlock, prevIndex));
  if (!prevIt.second) {
    delete prevIndex;
    prevIndex = prevIt.first->second;
  }

  // Try insert incoming block to index
  if (!index) {
    index = BC::Common::BlockIndex::create(BSBlock, prevIndex);
    auto It = blockIndex.blockIndex().insert(std::pair(hash, index));
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
        LOG_F(WARNING, "Already have block %s (%u)", hash.getHexLE().c_str(), index->Height);
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
      BuildHeaderChain(chainParams, prevIndex);
  }

  // Try to continue chain
  index->Serialized.reset(serialized);
  index->FileNo = fileNo;
  index->FileOffset = fileOffset;
  index->SerializedBlockSize = static_cast<uint32_t>(serialized->blockData().size());
  index->ConcurrentBlockNext = WaitPtr<BC::Common::BlockIndex>();
  index->ConcurrentBlockNext = prevIndex->SuccessorBlocks.exchange(index, 0);
  if (index->ConcurrentBlockNext.tag() == 1) {
    std::vector<BC::Common::BlockIndex*> acceptedBlocks;
    blockIndex.combiner().call(static_cast<BlockProcessingTask*>(prevIndex), [&blockIndex, &chainParams, &storage, &acceptedBlocks](BC::Common::BlockIndex *index) {
      buildBlockChain(blockIndex, chainParams, storage, index, acceptedBlocks);
    });

    if (!acceptedBlocks.empty()) {
      storage.wakeUp();
      if (callback)
        callback(acceptedBlocks);
    }
  }

  return index;
}

bool loadBlocks(BlockInMemoryIndex &blockIndex, BC::Common::ChainParams &chainParams, BC::DB::Storage &storage, const char *path, uint8_t *data, BlockPosition *position, size_t blocksNum, uint32_t fileNo)
{
  BC::Common::CheckConsensusCtx ccCtx;
  BC::Common::checkConsensusInitialize(ccCtx);

  for (size_t i = 0; i < blocksNum; i++) {
    size_t unpackedSize = 0;
    xmstream stream(data + position[i].offset + 8, position[i].size);
    BC::Proto::Block *block = BTC::unpack2<BC::Proto::Block>(stream, &unpackedSize);
    if (!block || stream.remaining() != 0) {
      LOG_F(ERROR, "Can't parse block file %s (invalid block structure)", path);
      return false;
    }

    intrusive_ptr<BC::Common::CIndexCacheObject> object(new BC::Common::CIndexCacheObject(&storage.cache(), nullptr, stream.sizeOf(), 0, block, unpackedSize));

    if (!AddBlock(blockIndex, chainParams, storage, object.get(), ccCtx, nullptr, fileNo, position[i].offset)) {
      LOG_F(ERROR, "Can't parse block file %s (invalid block structure)", path);
      return false;
    }
  }

  return true;
}

static bool loadBlockIndexDeserializer(BlockInMemoryIndex &blockIndex, LoadingIndexContext &loadingIndexContext, std::vector<size_t> &blockFileSizes, RawData *data, size_t indexesNum, const char *path)
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
      LOG_F(ERROR, "Index loader: no block data on disk for %s", index->Header.GetHash().getHexLE().c_str());
      return false;
    }

    // Check proof of work if need

    blockIndex.blockIndex().insert(std::pair(index->Header.GetHash(), index));
    loadingIndexContext.allIndexes.push_back(index);
    if (loadingIndexContext.bestIndex == nullptr || index->ChainWork > loadingIndexContext.bestIndex->ChainWork)
      loadingIndexContext.bestIndex = index;
  }

  return true;
}

static bool loadBlockIndexBuilder(BlockInMemoryIndex &blockIndex, LoadingIndexContext *loadingIndexContext)
{
  for (auto &index: loadingIndexContext->allIndexes) {
    auto It = blockIndex.blockIndex().find(index->Header.hashPrevBlock);
    if (It == blockIndex.blockIndex().end())
      continue;
    index->Prev = It->second;
  }

  return true;
}

bool loadingBlockIndex(BlockInMemoryIndex &blockIndex,
                       const std::filesystem::path &blockPath,
                       const std::filesystem::path &indexPath)
{
  LOG_F(INFO, "Loading block index...");

  char fileName[64];
  uint32_t indexFileNo = 0;
  std::vector<size_t> blockFileSizes;

  // Collect block data file sizes
  for (;;) {
    snprintf(fileName, sizeof(fileName), "blk%05u.dat", indexFileNo++);
    std::filesystem::path path = blockPath / fileName;
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
      std::unique_ptr<FILE, std::function<void(FILE*)>> hFile(fopen_path(path, "rb"), [](FILE *f) { fclose(f); });
      if (!hFile.get()) {
        LOG_F(ERROR, "Can't open index file %s", path.c_str());
        return false;
      }

      if (fread(data.get(), indexFileSize, 1, hFile.get()) != 1) {
        LOG_F(ERROR, "Can't read index file %s", path.c_str());
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
          LOG_F(ERROR, "Invalid index size %u detected in file %s", size, path.c_str());
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
      workers[i] = std::async(std::launch::async, loadBlockIndexDeserializer, std::ref(blockIndex), std::ref(loadingIndexContext[i]), std::ref(blockFileSizes), &offsets[0] + off, size, path.c_str());
    }

    for (unsigned i = 0; i < threadsNum; i++) {
      if (!workers[i].get()) {
        return false;
      }
    }
  }

  // Make links to previous blocks
  for (unsigned i = 0; i < threadsNum; i++)
    workers[i] = std::async(loadBlockIndexBuilder, std::ref(blockIndex), &loadingIndexContext[i]);
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
    bestIndex = blockIndex.genesis();

  LOG_F(INFO, "Found best index: %s (%u)", bestIndex->Header.GetHash().getHexLE().c_str(), bestIndex->Height);
  LOG_F(INFO, "Restore best chain...");

  BC::Common::BlockIndex *index = bestIndex;
  while (index->Prev) {
    if (index->Prev->Height != index->Height-1) {
      LOG_F(ERROR,
            "Index loader: block %s (%u) have invalid previous block %s with height %u",
            index->Header.GetHash().getHexLE().c_str(),
            index->Height,
            index->Prev->Header.GetHash().getHexLE().c_str(),
            index->Prev->Height);
      return false;
    }

    blockIndex.blockHeightIndex()[index->Height] = index;
    index->Prev->Next = index;
    index = index->Prev;
  }

  blockIndex.blockHeightIndex()[index->Height] = index;
  if (index != blockIndex.genesis()) {
    LOG_F(ERROR, "Index for [%u]%s is broken (breaks at [%u]%s",
          bestIndex->Height,
          bestIndex->Header.GetHash().getHexLE().c_str(),
          index->Height,
          index->Header.GetHash().getHexLE().c_str());
    return false;
  }

  blockIndex.setBest(bestIndex);
  LOG_F(INFO, "Loading index done");
  return true;
}

bool reindex(BlockInMemoryIndex &blockIndex,
             const std::filesystem::path &blockPath,
             BC::Common::ChainParams &chainParams,
             BC::DB::Storage &storage)
{
  size_t bufferSize = 0;
  std::unique_ptr<uint8_t[]> data;
  char blockFileName[64];
  unsigned blkFileIndex = 0;
  size_t totalBlockCount = 0;

  std::vector<BlockPosition> blockOffsets;
  unsigned threadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;

  for (;;) {
    snprintf(blockFileName, sizeof(blockFileName), "blk%05u.dat", blkFileIndex);
    std::filesystem::path path = blockPath / blockFileName;
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
      std::unique_ptr<FILE, std::function<void(FILE*)>> hFile(fopen_path(path, "rb"), [](FILE *f) { fclose(f); });
      if (!hFile.get()) {
        LOG_F(ERROR, "Can't open block file %s", path.c_str());
        // storage.queue().push(BC::DB::Task(BC::DB::WriteData, nullptr));
        return false;
      }

      if (fread(data.get(), 1, blockFileSize, hFile.get()) != blockFileSize) {
        LOG_F(ERROR, "Can't read block file %s", path.c_str());
        // storage.queue().push(BC::DB::Task(BC::DB::WriteData, nullptr));
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

        if (magic != chainParams.magic) {
          LOG_F(ERROR, "Can't parse block file %s (invalid magic)", path.c_str());
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
      workers[i] = std::async(std::launch::async, loadBlocks, std::ref(blockIndex), std::ref(chainParams), std::ref(storage), path.c_str(), data.get(), &blockOffsets[0] + off, size, blkFileIndex);
    }

    for (unsigned i = 0; i < threadsNum; i++) {
      if (!workers[i].get()) {
        return false;
      }
    }

    LOG_F(INFO,
          "%u blocks loaded from %s; cache size: %.3lfM best: [%u]%s",
          static_cast<unsigned>(blockOffsets.size()),
          path.c_str(),
          storage.cache().size() / 1048576.0f,
          blockIndex.best()->Height,
          blockIndex.best()->Header.GetHash().getHexLE().c_str());

    totalBlockCount += blockOffsets.size();
    blkFileIndex++;
  }

  auto best = blockIndex.best();
  LOG_F(INFO, "%zu blocks loaded from disk", totalBlockCount);
  LOG_F(INFO, "Best block is %s (%u)", best->Header.GetHash().getHexLE().c_str(), best->Height);
  return true;
}


bool BlockDatabase::init(const std::filesystem::path &blocksDir,
                         const std::filesystem::path &indexDir,
                         BC::Common::ChainParams &chainParams)
{
  Magic_ = chainParams.magic;
  BlocksDir_ = blocksDir;
  IndexDir_= indexDir;

  if (!BlockStorage_.init(blocksDir, "blk%05u.dat", BC::Configuration::BlocksFileLimit))
    return false;
  if (!IndexStorage_.init(indexDir, "index%05u.dat", BC::Configuration::BlocksFileLimit))
    return false;
  if (!LinkedOutputsStorage_.init(indexDir, "linkedoutput%05u.dat", BC::Configuration::BlocksFileLimit))
    return false;

  if (BlockStorage_.empty()) {
    // Store genesis block to disk (for compatibility with core clients)
    // Serialize block using storage format:
    //   <magic>:4 <blockSize>:4 <block>
    std::pair<uint32_t, uint32_t> position;
    xmstream stream;
    BC::serialize(stream, chainParams.GenesisBlock);
    uint32_t prefix[2] = {chainParams.magic, static_cast<uint32_t>(stream.sizeOf())};
    if (!BlockStorage_.append2(prefix, sizeof(prefix), stream.data(), static_cast<uint32_t>(stream.sizeOf()), position) ||
        !BlockStorage_.flush())
      return false;
  }

  return true;
}

bool BlockDatabase::writeBlock(BC::Common::BlockIndex *index, bool *needFlush)
{
  if (index->indexStored())
    return true;

  std::pair<uint32_t, uint32_t> position;
  BC::Common::CIndexCacheObject *serialized = index->Serialized.get();
  bool blockStored = index->blockStored();

  if (!blockStored) {
    // Skip blocks loaded from disk
    uint32_t prefix[2] = { Magic_, index->SerializedBlockSize };
    if (!BlockStorage_.append2(prefix, sizeof(prefix), serialized->blockData().data(), static_cast<uint32_t>(serialized->blockData().size()), position))
      return false;
    index->FileNo = position.first;
    index->FileOffset = position.second;
  }

  // Serialize index for storage
  uint32_t serializedSize;
  uint8_t buffer[1024];
  xmstream data(buffer, sizeof(buffer));

  data.reset();
  BC::serialize(data, serialized->linkedOutputs());
  serializedSize = static_cast<uint32_t>(data.sizeOf());
  if (!LinkedOutputsStorage_.append2(&serializedSize, sizeof(serializedSize), data.data(), static_cast<uint32_t>(data.sizeOf()), position))
    return false;

  index->LinkedOutputsFileNo = position.first;
  index->LinkedOutputsFileOffset = position.second;
  index->LinkedOutputsSerializedSize = serializedSize;
  data.reset();
  BC::serialize(data, *index);
  serializedSize = static_cast<uint32_t>(data.sizeOf());
  if (!IndexStorage_.append2(&serializedSize, sizeof(serializedSize), data.data(), static_cast<uint32_t>(data.sizeOf()), position))
    return false;

  if ((!blockStored && BlockStorage_.bufferEmpty()) || LinkedOutputsStorage_.bufferEmpty() || IndexStorage_.bufferEmpty())
    *needFlush = true;

  return true;
}


BlockSearcher::BlockSearcher(BlockDatabase &blockDb, std::function<void(void*, size_t)> handler, std::function<void()> errorHandler) :
  BlockDb_(blockDb), Handler_(handler), ErrorHandler_(errorHandler)
{
  blocksDirectory = blockDb.blocksDir();
}

BlockSearcher::~BlockSearcher()
{
  fetchPending();
}

BC::Common::BlockIndex *BlockSearcher::add(BlockInMemoryIndex &blockIndex, const BC::Proto::BlockHashTy &hash)
{
  auto It = blockIndex.blockIndex().find(hash);
  if (It != blockIndex.blockIndex().end()) {
    return add(It->second);
  } else {
    return nullptr;
  }
}

BC::Common::BlockIndex *BlockSearcher::add(BC::Common::BlockIndex *index)
{
  intrusive_ptr<BC::Common::CIndexCacheObject> serializedPtr(index->Serialized);
  if (const BC::Common::CIndexCacheObject *serialized = serializedPtr.get()) {
    fetchPending();
    fileNo = std::numeric_limits<uint32_t>::max();
    Handler_(serialized->blockData().data(), serialized->blockData().size());
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

    ExpectedBlockSizes_.push_back(index->SerializedBlockSize);
  } else {
    return nullptr;
  }

  return index;
}

void BlockSearcher::fetchPending()
{
  stream.reset();
  uint32_t size = fileOffsetCurrent - fileOffsetBegin;
  if (fileNo != std::numeric_limits<uint32_t>::max() && size && !BlockDb_.blockReader().read(fileNo, fileOffsetBegin, stream.reserve(size), size)) {
    LOG_F(ERROR, "Can't read data from %s (offset = %u, size = %u)", BlockDb_.blockReader().getFilePath(fileNo).c_str(), fileOffsetBegin, size);
    ErrorHandler_();
  }

  stream.seekSet(0);
  unsigned expectedIndex = 0;
  while (stream.remaining()) {
    uint32_t magic;
    uint32_t blockSize;
    BC::unserialize(stream, magic);
    BC::unserialize(stream, blockSize);
    void *data = stream.seek<uint8_t>(blockSize);
    if (magic != BlockDb_.magic() || stream.eof()) {
      char fileName[64];
      snprintf(fileName, sizeof(fileName), "blk%05u.dat", fileNo);
      std::filesystem::path path = blocksDirectory / fileName;
      LOG_F(ERROR, "Invalid block data in file %s", path.c_str());
      ErrorHandler_();
      return;
    }

    if (blockSize != ExpectedBlockSizes_[expectedIndex]) {
      char fileName[64];
      snprintf(fileName, sizeof(fileName), "blk%05u.dat", fileNo);
      std::filesystem::path path = blocksDirectory / fileName;
      LOG_F(ERROR, "Invalid block data in file %s: mismatch block size in index(%u) and data file(%u)", path.c_str(), ExpectedBlockSizes_[expectedIndex], blockSize);
      ErrorHandler_();
      return;
    }

    expectedIndex++;
    Handler_(data, blockSize);
  }

  ExpectedBlockSizes_.clear();
}

BC::Common::BlockIndex *BlockBulkReader::add(BlockInMemoryIndex &blockIndex, const BC::Proto::BlockHashTy &hash)
{
  auto It = blockIndex.blockIndex().find(hash);
  if (It != blockIndex.blockIndex().end()) {
    return add(It->second);
  } else {
    return nullptr;
  }
}

BC::Common::BlockIndex *BlockBulkReader::add(BC::Common::BlockIndex *index)
{
  intrusive_ptr<BC::Common::CIndexCacheObject> object(index->Serialized);
  if (object.get()) {
    // Block now in cache, flush queue and call handler
    fetchPending();
    Handler_(index, *object.get()->block(), object.get()->linkedOutputs());
  } else if (index->FileNo != std::numeric_limits<uint32_t>::max() &&
             index->FileOffset != std::numeric_limits<uint32_t>::max() &&
             index->SerializedBlockSize != std::numeric_limits<uint32_t>::max() &&
             index->LinkedOutputsFileNo != std::numeric_limits<uint32_t>::max() &&
             index->LinkedOutputsFileOffset != std::numeric_limits<uint32_t>::max() &&
             index->LinkedOutputsSerializedSize != std::numeric_limits<uint32_t>::max()) {
    // Block not in cache, but present on disk storage
    if (!BlockCursor_.initialized()) {
      // Queue is empty, initialize it with first block
      BlockCursor_.set(index->FileNo, index->FileOffset, index->FileOffset + index->SerializedBlockSize + 8);
      LinkedOutputsCursor_.emplace_back(index->LinkedOutputsFileNo,
                                        index->LinkedOutputsFileOffset,
                                        index->LinkedOutputsFileOffset + index->LinkedOutputsSerializedSize + 4);
      Indexes_.push_back(index);
    } else if (BlockCursor_.FileNo == index->FileNo && BlockCursor_.OffsetCurrent == index->FileOffset) {
      // Read from disk can be combined
      BlockCursor_.OffsetCurrent = index->FileOffset + index->SerializedBlockSize + 8;

      if (LinkedOutputsCursor_.back().FileNo == index->LinkedOutputsFileNo && LinkedOutputsCursor_.back().OffsetCurrent == index->LinkedOutputsFileOffset) {
        LinkedOutputsCursor_.back().OffsetCurrent = index->LinkedOutputsFileOffset + index->LinkedOutputsSerializedSize + 4;
      } else {
        LinkedOutputsCursor_.emplace_back(index->LinkedOutputsFileNo,
                                          index->LinkedOutputsFileOffset,
                                          index->LinkedOutputsFileOffset + index->LinkedOutputsSerializedSize + 4);
      }
      Indexes_.push_back(index);
    } else {
      fetchPending();
      BlockCursor_.set(index->FileNo, index->FileOffset, index->FileOffset + index->SerializedBlockSize + 8);
      LinkedOutputsCursor_.emplace_back(index->LinkedOutputsFileNo,
                                        index->LinkedOutputsFileOffset,
                                        index->LinkedOutputsFileOffset + index->LinkedOutputsSerializedSize + 4);
      Indexes_.push_back(index);
    }
  } else {
    return nullptr;
  }

  return index;
}

void BlockBulkReader::fetchPending()
{
  if (!BlockCursor_.initialized())
    return;

  // Read block data
  BlockStream_.reset();
  uint32_t blockDataSize = BlockCursor_.OffsetCurrent - BlockCursor_.OffsetBegin;
  void *blockData = BlockStream_.reserve(blockDataSize);
  if (!BlockDb_.blockReader().read(BlockCursor_.FileNo, BlockCursor_.OffsetBegin, blockData, blockDataSize)) {
    LOG_F(ERROR,
          "Can't read data from %s (offset = %u, size = %u)",
          BlockDb_.blockReader().getFilePath(BlockCursor_.FileNo).c_str(),
          BlockCursor_.OffsetBegin,
          blockDataSize);
    ErrorHandler_();
    return;
  }

  // Read linked outputs data
  LinkedOutputsStream_.reset();
  uint32_t linkedOutputsDataSize = 0;
  for (const auto &b: LinkedOutputsCursor_)
    linkedOutputsDataSize += b.OffsetCurrent - b.OffsetBegin;
  void *linkedOutputsData = LinkedOutputsStream_.reserve(linkedOutputsDataSize);

  uint8_t *p = (uint8_t*)linkedOutputsData;
  for (const auto &b: LinkedOutputsCursor_) {
    if (!BlockDb_.linkedOutputsReader().read(b.FileNo, b.OffsetBegin, p, b.OffsetCurrent - b.OffsetBegin)) {
      LOG_F(ERROR,
            "Can't read data from %s (offset = %u, size = %u)",
            BlockDb_.linkedOutputsReader().getFilePath(b.FileNo).c_str(),
            b.OffsetBegin,
            b.OffsetCurrent - b.OffsetBegin);
      ErrorHandler_();
      return;
    }
    p += b.OffsetCurrent - b.OffsetBegin;
  }

  size_t i = 0;
  BlockStream_.seekSet(0);
  LinkedOutputsStream_.seekSet(0);
  while (BlockStream_.remaining()) {
    if (i >= Indexes_.size()) {
      ErrorHandler_();
      return;
    }

    uint32_t magic;
    uint32_t bSize;
    uint32_t lSize;
    BC::unserialize(BlockStream_, magic);
    BC::unserialize(BlockStream_, bSize);
    BC::unserialize(LinkedOutputsStream_, lSize);

    if (magic != BlockDb_.magic() || BlockStream_.eof()) {
      LOG_F(ERROR, "Invalid block data in file %s", BlockDb_.blockReader().getFilePath(BlockCursor_.FileNo).c_str());
      ErrorHandler_();
      return;
    }

    // Unserialize block and linked outputs
    size_t size;
    auto *block = BTC::unpack2<BC::Proto::Block>(BlockStream_, &size);
    if (!block) {
      LOG_F(ERROR,
            "BlockBulkReader: can't unserialize block [%u]%s",
            Indexes_[i]->Height,
            Indexes_[i]->Header.GetHash().getHexLE().c_str());
      ErrorHandler_();
      return;
    }

    BC::Proto::CBlockLinkedOutputs linkedOutputs;
    if (!BC::unserializeAndCheck(LinkedOutputsStream_, linkedOutputs)) {
      LOG_F(ERROR,
            "BlockBulkReader: can't unserialize linked outputs for block [%u]%s",
            Indexes_[i]->Height,
            Indexes_[i]->Header.GetHash().getHexLE().c_str());
      operator delete(block);
      ErrorHandler_();
      return;
    }

    Handler_(Indexes_[i++], *block, linkedOutputs);
  }

  if (i != Indexes_.size() ||
      BlockStream_.remaining() ||
      LinkedOutputsStream_.remaining()) {
    LOG_F(ERROR, "BlockBulkReader: inconsistent data");
    ErrorHandler_();
    return;
  }

  BlockCursor_.reset();
  LinkedOutputsCursor_.clear();
  Indexes_.clear();
}
