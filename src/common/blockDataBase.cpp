// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockDataBase.h"
#include "dbengine/keyHash.h"
#include "db/storage.h"
#include "common/fopen.h"
#include "common/parallelRunner.h"
#include "common/serializeUtils.h"
#include "common/smallStream.h"
#include "common/utils.h"
#include <asyncio/asyncio.h>
#include <p2putils/coreTypes.h>
#include <p2putils/xmstream.h>
#include "loguru.hpp"
#include "thirdparty/ankerl/unordered_dense.h"
#include <deque>
#include <thread>

struct BlockPosition {
  uint32_t Offset;
  uint32_t Size;
};

// Every input still empty: from the database, else from an output of this very block. A worker
// runs it on a block outside a run, where the state it needs may not exist yet - what it could
// not answer InputsResolved reports. The connect thread runs it on exactly those, and there the
// state is the one the block connects to
static bool resolveBlockInputs(BC::Proto::CBlockLinkedOutputs &linkedOutputs, BC::Proto::CBlockValidationData &validationData, BC::Proto::Block &block, const BC::DB::UTXODb &db)
{
  ankerl::unordered_dense::set<CUnspentOutputKey> removed;

  assert(validationData.TxIds.size() == block.vtx.size());
  linkedOutputs.Tx.resize(block.vtx.size());

  bool resolved = true;
  size_t inOrdinal = 0;
  for (size_t txIdx = 1; txIdx < block.vtx.size(); txIdx++) {
    BC::Proto::Transaction &tx = block.vtx[txIdx];
    auto &txLinked = linkedOutputs.Tx[txIdx];

    txLinked.TxIn.resize(tx.txIn.size());
    for (size_t txinIdx = 0; txinIdx < tx.txIn.size(); txinIdx++, inOrdinal++) {
      const auto &txin = tx.txIn[txinIdx];
      auto &txinLinked = txLinked.TxIn[txinIdx];

      // Answered by the run: from the same block, or from an earlier block of it
      if (!txinLinked.empty())
        continue;

      if (db.query(txin.previousOutputHash, txin.previousOutputIndex, txinLinked)) {
        // Unspent output found
      } else {
        // Try find in local block (topology precomputed in validation data)
        uint32_t localTxIdx = validationData.InputLocalTx[inOrdinal];
        if (localTxIdx != BC::Proto::CBlockValidationData::NoLocalTx) {
          BC::Proto::Transaction &localReferencedTx = block.vtx[localTxIdx];
          if (txin.previousOutputIndex >= localReferencedTx.txOut.size()) {
            validationData.InputsResolved = false;
            return false;
          }
          CUnspentOutputKey key;
          key.Tx = txin.previousOutputHash;
          key.Index = txin.previousOutputIndex;
          if (!removed.insert(key).second) {
            validationData.InputsResolved = false;
            return false;
          }

          xmstream s;
          BC::Script::parseTransactionOutput(localReferencedTx.txOut[txin.previousOutputIndex], s);
          BTC::Script::UnspentOutputInfo *info = s.data<BTC::Script::UnspentOutputInfo>();
          info->IsLocalTx = 1;
          xvectorFromStream(std::move(s), txinLinked);
        } else {
          resolved = false;
        }
      }
    }
  }
  assert(inOrdinal == validationData.InputLocalTx.size());

  validationData.InputsResolved = resolved;
  return resolved;
}

// The one point where the database holds the state the segment was built on. Everything the
// preparation could not answer is looked up here in one wave; an input that finds nothing makes
// its block invalid. The wave is the critical path, so it takes the pool before preparation
static void resolveSegmentResidual(CSegment &segment, const BC::DB::UTXODb &db, CParallelRunner &runner)
{
  const std::vector<CSegment::CInput> &inputs = segment.Inputs;

  runner.run(inputs.size(), [&inputs, &segment, &db](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      const CSegment::CInput &input = inputs[i];
      BC::Common::CIndexCacheObject *object = segment.Objects[input.Object].Object.get();
      const auto &txin = object->block()->vtx[input.TxIdx].txIn[input.InIdx];
      auto &slot = object->linkedOutputs().Tx[input.TxIdx].TxIn[input.InIdx];
      // Emptied first: an empty slot is the only thing that means "the coin is not there", and a
      // block prepared twice still carries what an earlier wave found
      slot.resize(0);
      db.query(txin.previousOutputHash, txin.previousOutputIndex, slot);
    }
  }, /*priority=*/true);

  for (const CSegment::CInput &input: inputs) {
    if (segment.Objects[input.Object].Object.get()->linkedOutputs().Tx[input.TxIdx].TxIn[input.InIdx].empty())
      segment.Objects[input.Object].Completable = false;
  }

  for (CSegment::CObject &entry: segment.Objects) {
    entry.Object.get()->validationData().InputsResolved = entry.Completable;
    entry.Object.get()->validationData().InputsInvalid = !entry.Completable;
  }
}

BC::Common::BlockIndex *rebaseChain(BC::Common::BlockIndex *newBest,
                                    BC::Common::BlockIndex *previousBest,
                                    std::vector<BC::Common::BlockIndex*> &forDisconnect)
{
  // New best block found
  if (newBest->Prev == previousBest) {
    return newBest;
  } else {
    // Rebuild chain from least common ancestor. The return is the first block
    // to CONNECT - the child of the ancestor on the new chain, never the
    // ancestor itself: previousBest is already applied, and replaying it
    // double-counts every delta-folded database
    BC::Common::BlockIndex *lb;
    BC::Common::BlockIndex *sb;
    if (newBest->Height >= previousBest->Height) {
      lb = newBest;
      sb = previousBest;
      BC::Common::BlockIndex *lbChild = nullptr;
      uint32_t sbHeight = sb->Height;
      while (lb->Height > sbHeight) {
        lbChild = lb;
        lb = lb->Prev;
      }
      while (sb != lb) {
        forDisconnect.push_back(sb);
        sb = sb->Prev;
        lbChild = lb;
        lb = lb->Prev;
      }

      return lbChild;
    } else {
      lb = previousBest;
      sb = newBest;
      BC::Common::BlockIndex *sbChild = nullptr;
      uint32_t sbHeight = sb->Height;
      while (lb->Height > sbHeight) {
        forDisconnect.push_back(lb);
        lb = lb->Prev;
      }
      while (sb != lb) {
        forDisconnect.push_back(lb);
        sbChild = sb;
        sb = sb->Prev;
        lb = lb->Prev;
      }

      // Null when newBest is an ancestor of previousBest: disconnect only
      return sbChild;
    }
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

static inline void QueueNextBlocks(std::deque<BC::Common::BlockIndex*> &queue,
                                   BC::Common::BlockIndex *start)
{
  auto it = start->SuccessorBlocks.exchange(nullptr, 1);
  while (auto ptr = it.pointer()) {
    queue.push_back(ptr);
    while (ptr->ConcurrentBlockNext.data() == WaitPtr<BC::Common::BlockIndex>())
      continue;
    it = ptr->ConcurrentBlockNext.load();
  }
}

// Index bookkeeping of a connected block: the chain, the height index and the flags. The
// databases hear about it separately - one call per batch, not per block
static void markConnected(BC::Common::BlockIndex *index, BlockInMemoryIndex &blockIndex)
{
  index->Prev->Next = index;
  index->OnChain.store(true, std::memory_order_relaxed);
  blockIndex.blockHeightIndex()[index->Height] = index;
}

// Everything a connect changes; checks belong to the caller, so a segment can make them for all
// of its blocks before the first one lands
static void applyConnect(BC::Common::BlockIndex *index,
                         BC::Proto::Block &block,
                         BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                         BC::Proto::CBlockValidationData &validationData,
                         BlockInMemoryIndex &blockIndex,
                         BC::DB::Storage &storage,
                         bool silent)
{
  if (!silent)
    LOG_F(INFO, "Connect block %s (%u)", index->Header.GetHash().getHexLE().c_str(), index->Height);
  markConnected(index, blockIndex);
  BC::DB::CBlockRef ref{index, &block, &linkedOutputs, &validationData};
  storage.connect(BC::DB::CBlockBatch(&ref, 1), blockIndex);
  blockIndex.setBest(index);
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
  // Inputs are resolved before the block gets here whenever the state to resolve them against
  // exists that early: by the run, or by the single block path. What was left - a block outside
  // any run, a run that could not open where it was built - is looked up now, against the state
  // the block connects to. An input still empty means the coin does not exist or is taken
  if (!validationData.InputsResolved &&
      (validationData.InputsInvalid ||
       !resolveBlockInputs(linkedOutputs, validationData, block, storage.utxodb()))) {
    LOG_F(ERROR,
          "Block %s validation failed (non-existent utxo)",
          block.header.GetHash().getHexLE().c_str());
    return false;
  }

  std::string error;
  if (!BC::Common::checkBlockContextual(*index, block, validationData, linkedOutputs, chainParams, error)) {
    LOG_F(ERROR,
          "Block %s (%u) contextual check failed, error: %s",
          index->Header.GetHash().getHexLE().c_str(),
          index->Height,
          error.c_str());
    return false;
  }

  applyConnect(index, block, linkedOutputs, validationData, blockIndex, storage, silent);
  return true;
}

static void DisconnectBlock(BlockInMemoryIndex &blockIndex,
                            BC::Proto::Block &block,
                            BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                            BC::Proto::CBlockValidationData &validationData,
                            BC::DB::Storage &storage,
                            BC::Common::BlockIndex *index,
    bool silent = true)
{
  if (!silent)
    LOG_F(INFO, "Disconnect block %s (%u)", index->Header.GetHash().getHexLE().c_str(), index->Height);
  index->Prev->Next = nullptr;
  index->OnChain.store(false, std::memory_order_relaxed);
  blockIndex.blockHeightIndex()[index->Height] = nullptr;
  storage.disconnect(index, block, linkedOutputs, validationData, blockIndex);
  // The segment this block came in is broken from here: the disconnect put the hidden outputs
  // back, and every later connect of it must be plain. Only utxodb reads the marks, and it took
  // its half of the disconnect above, on this thread
  validationData.dropPairs();
}

// A height is what makes a block a candidate: until the header chain reaches it, nothing knows
// where it stands. Data may have been waiting for this for a whole block file
static void BuildHeaderChain(BlockInMemoryIndex &blockIndex,
                             BC::Common::ChainParams &chainParams,
                             BC::Common::BlockIndex *start)
{
  std::deque<BC::Common::BlockIndex*> queue;
  BC::Common::BlockIndex *best = nullptr;

  QueueNextHeaders(queue, start);

  while (!queue.empty()) {
    BC::Common::BlockIndex *current = queue.front();
    BC::Common::BlockIndex *prev = current->Prev;

    if (current->Height == std::numeric_limits<uint32_t>::max()) {
      current->Height = prev->Height + 1;
      current->ChainWork = prev->ChainWork + BC::Common::GetBlockProof(current->Header, chainParams);
    }

    // These two flags form a sequentially consistent handshake: whichever topology walk
    // finishes second sees the first and publishes a candidate with both work and data visible.
    current->HeaderReady.store(true);
    if (current->DataReady.load() && (!best || current->ChainWork > best->ChainWork))
      best = current;

    QueueNextHeaders(queue, current);
    queue.pop_front();
  }

  if (best)
    blockIndex.notifyReady(best);
}

// The data graph is released only through a predecessor already released itself. Therefore every
// block visited here has a complete decoded path to genesis, independent of arrival order.
static BC::Common::BlockIndex *BuildReadyChain(BC::Common::BlockIndex *start)
{
  std::deque<BC::Common::BlockIndex*> queue;
  QueueNextBlocks(queue, start);
  BC::Common::BlockIndex *best = nullptr;

  while (!queue.empty()) {
    BC::Common::BlockIndex *current = queue.front();
    queue.pop_front();

    // A rejected block takes its descendants with it: the walk stops here, so their successor
    // lists stay unreleased and nothing below them is ever offered as a candidate
    if (current->IndexState.load(std::memory_order_relaxed) == BSInvalid ||
        current->Prev->IndexState.load(std::memory_order_relaxed) == BSInvalid)
      continue;

    current->DataReady.store(true);
    if (current->HeaderReady.load() && (!best || current->ChainWork > best->ChainWork))
      best = current;
    QueueNextBlocks(queue, current);
  }

  return best;
}

intrusive_ptr<BC::Common::CIndexCacheObject> objectFromStoredBytes(BC::Common::BlockIndex *index,
                                                                   BC::Common::ChainParams &chainParams,
                                                                   const void *blockData,
                                                                   size_t blockSize,
                                                                   const void *linkedOutputsData,
                                                                   size_t linkedOutputsSize,
                                                                   CAllocationInfo *info)
{
  size_t unpackedSize = 0;
  xmstream blockStream(const_cast<void*>(blockData), blockSize);
  BC::Proto::Block *block = BTC::unpack2<BC::Proto::Block>(blockStream, &unpackedSize);
  if (!block || blockStream.remaining() != 0) {
    operator delete(block);
    return nullptr;
  }

  // The serialized bytes are not kept: the block is on disk, and the caller owns the copy
  intrusive_ptr<BC::Common::CIndexCacheObject> object(
    new BC::Common::CIndexCacheObject(info, nullptr, blockSize, 0, block, unpackedSize));

  {
    xmstream stream(const_cast<void*>(linkedOutputsData), linkedOutputsSize);
    if (!BTC::unserializeAndCheck(stream, object.get()->linkedOutputs()))
      return nullptr;
  }

  // Same invariant as a fresh block: validation data is filled before any connect/disconnect.
  // No contextual check runs here - the block passed one on its way into the block database -
  // so the exemptions come straight from the pinned list
  BC::Common::initializeValidationContext(*block, object.get()->validationData());
  BTC::Common::fillBIP30Context(*index, chainParams, object.get()->validationData());
  object.get()->validationData().InputsResolved = true;
  object.get()->reaccount();

  return object;
}

intrusive_ptr<BC::Common::CIndexCacheObject> objectByIndex(BC::Common::BlockIndex *index,
                                                           BC::Common::ChainParams &chainParams,
                                                           BlockDatabase &blockDb)
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

  return objectFromStoredBytes(index,
                               chainParams,
                               serialized.get(),
                               index->SerializedBlockSize,
                               linkedOutputsData.get(),
                               index->LinkedOutputsSerializedSize,
                               nullptr);
}

static intrusive_ptr<BC::Common::CIndexCacheObject> objectByIndexChecked(BC::Common::BlockIndex *index,
                                                                        BC::Common::ChainParams &chainParams,
                                                                        BlockDatabase &blockDb)
{
  auto object = objectByIndex(index, chainParams, blockDb);
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
      auto object = objectByIndexChecked(sb, chainParams, storage.blockDb());
      DisconnectBlock(blockIndex, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationData(), storage, sb, false);
      sb = sb->Prev;
      lb = lb->Prev;
    }

  } else {
    lb = currentBest;
    sb = newBest;
    uint32_t sbHeight = sb->Height;
    while (lb->Height > sbHeight) {
      BC::Proto::Block diskBlock;
      auto object = objectByIndexChecked(lb, chainParams, storage.blockDb());
      DisconnectBlock(blockIndex, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationData(), storage, lb, false);
      lb = lb->Prev;
    }
    while (sb != lb) {
      BC::Proto::Block diskBlock;
      newPath.push_back(sb);
      auto object = objectByIndexChecked(lb, chainParams, storage.blockDb());
      DisconnectBlock(blockIndex, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationData(), storage, lb, false);
      sb = sb->Prev;
      lb = lb->Prev;
    }
  }

  // Connect blocks from new path
  for (auto I = newPath.rbegin(), IE = newPath.rend(); I != IE; ++I) {
    auto object = objectByIndexChecked(*I, chainParams, storage.blockDb());
    if (!ConnectBlock(*I, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationData(), chainParams, blockIndex, storage, false)) {
      (*I)->IndexState = BSInvalid;
      return false;
    }
  }

  return true;
}

// Open addressing tables for the linking of one segment. A slot holds the hash and where the key
// already lives (validation data txids, the outpoint of an input) instead of the key itself: one
// array, no allocation per entry. Linking is the serial stretch of the preparation, and
// std::unordered_* cost it a malloc per insert and a pointer chase per lookup
namespace {
struct CTxSlot {
  uint64_t Hash;      // 0 - empty slot
  uint32_t Block;
  uint32_t TxIdx;
  uint32_t OutBase;   // block-wide ordinal of the first output of the transaction
  uint32_t Reserved;
};

struct CSpendSlot {
  uint64_t Hash;      // 0 - empty slot
  uint32_t Input;     // position in the residual list, where the outpoint is
  uint32_t Reserved;
};

// Hashing is db/keyHash.h; the only thing added here is the sentinel: a slot with hash 0 is the
// free slot of these tables. A txid is a hash already, so a word of it is the hash of a txid
inline uint64_t occupied(uint64_t hash) { return hash ? hash : 1; }

inline uint64_t txidHash(const BC::Proto::TxHashTy &txid) { return occupied(txid.get64(0)); }

inline uint64_t outpointHash(const BC::Proto::TxHashTy &txid, uint32_t index)
{
  return occupied(dbengine::hashOutpoint(txid.begin(), index).H1);
}

// Load factor 1/2
size_t tableSize(size_t expected)
{
  size_t size = 1024;
  while (size < expected * 2)
    size <<= 1;
  return size;
}

void markOrdinal(xvector<uint64_t> &bits, size_t ordinal, size_t count)
{
  if (bits.empty()) {
    bits.resize((count + 63) / 64);
    memset(bits.begin(), 0, bits.size() * sizeof(uint64_t));
  }

  bits[ordinal >> 6] |= 1ull << (ordinal & 63);
}
}

// Linking of a whole segment: an input is answered by its own block, then by an earlier block of
// the segment, and anything older goes on the residual list. Nothing is looked up - that state
// does not exist yet. A pair inside the segment is marked on both sides and skipped by the
// databases, which holds only because the segment connects as one operation
static void resolveSegmentInputs(CSegment &segment)
{
  const size_t count = segment.Objects.size();

  segment.Inputs.clear();

  // Kept between segments (same size every time, warm memory); one set per preparation lane
  static thread_local std::vector<CTxSlot> txTable;
  static thread_local std::vector<CSpendSlot> spendTable;
  static thread_local std::vector<const BC::Proto::TxHashTy*> txIds;

  size_t txCount = 0;
  size_t inputCount = 0;
  txIds.resize(count);
  for (size_t pos = 0; pos < count; pos++) {
    const BC::Proto::CBlockValidationData &validationData = segment.Objects[pos].Object.get()->validationDataConst();
    txIds[pos] = validationData.TxIds.begin();
    txCount += validationData.TxIds.size();
    inputCount += validationData.InputLocalTx.size();
  }

  const size_t txMask = tableSize(txCount) - 1;
  const size_t spendMask = tableSize(inputCount) - 1;
  txTable.assign(txMask + 1, CTxSlot{});
  spendTable.assign(spendMask + 1, CSpendSlot{});

  // Where a transaction of the segment lives, nullptr when this txid is seen the first time
  auto txInsert = [&](uint64_t hash, const BC::Proto::TxHashTy &txid, uint32_t block, uint32_t txIdx, uint32_t outBase) -> CTxSlot* {
    size_t pos = hash & txMask;
    for (;;) {
      CTxSlot &slot = txTable[pos];
      if (!slot.Hash) {
        slot = CTxSlot{hash, block, txIdx, outBase, 0};
        return nullptr;
      }
      if (slot.Hash == hash && txIds[slot.Block][slot.TxIdx] == txid)
        return &slot;
      pos = (pos + 1) & txMask;
    }
  };

  auto txFind = [&](uint64_t hash, const BC::Proto::TxHashTy &txid) -> const CTxSlot* {
    size_t pos = hash & txMask;
    for (;;) {
      const CTxSlot &slot = txTable[pos];
      if (!slot.Hash)
        return nullptr;
      if (slot.Hash == hash && txIds[slot.Block][slot.TxIdx] == txid)
        return &slot;
      pos = (pos + 1) & txMask;
    }
  };

  // The outpoint of a residual input, read back from where it lives
  auto outpointOf = [&](uint32_t inputIdx, uint32_t &index) -> const BC::Proto::TxHashTy& {
    const CSegment::CInput &input = segment.Inputs[inputIdx];
    const auto &txin = segment.Objects[input.Object].Object.get()->block()->vtx[input.TxIdx].txIn[input.InIdx];
    index = txin.previousOutputIndex;
    return txin.previousOutputHash;
  };

  // False when the segment already spends this coin: the residual wave would find it for both,
  // and that is a double spend
  auto spendInsert = [&](uint64_t hash, uint32_t inputIdx) -> bool {
    size_t pos = hash & spendMask;
    for (;;) {
      CSpendSlot &slot = spendTable[pos];
      if (!slot.Hash) {
        slot = CSpendSlot{hash, inputIdx, 0};
        return true;
      }
      if (slot.Hash == hash) {
        uint32_t indexA, indexB;
        const BC::Proto::TxHashTy &a = outpointOf(slot.Input, indexA);
        const BC::Proto::TxHashTy &b = outpointOf(inputIdx, indexB);
        if (indexA == indexB && a == b)
          return false;
      }
      pos = (pos + 1) & spendMask;
    }
  };

  for (size_t pos = 0; pos < count; pos++) {
    BC::Common::CIndexCacheObject *object = segment.Objects[pos].Object.get();
    BC::Proto::Block &block = *object->block();
    BC::Proto::CBlockValidationData &validationData = object->validationData();
    BC::Proto::CBlockLinkedOutputs &linkedOutputs = object->linkedOutputs();

    assert(validationData.TxIds.size() == block.vtx.size());

    // Nothing to link: the same-block topology already proved the block invalid, and the segment
    // is cut here
    if (validationData.LocalSpendInvalid) {
      segment.Objects[pos].Completable = false;
      validationData.InputsInvalid = true;
      continue;
    }

    linkedOutputs.Tx.resize(block.vtx.size());

    // An input left unresolved keeps its block out of the chain; the truncation happens after
    // the linking, so nothing else has to stop here
    bool completable = true;
    size_t inOrdinal = 0;

    for (size_t txIdx = 1; txIdx < block.vtx.size(); txIdx++) {
      BC::Proto::Transaction &tx = block.vtx[txIdx];
      auto &txLinked = linkedOutputs.Tx[txIdx];
      txLinked.TxIn.resize(tx.txIn.size());

      for (size_t j = 0; j < tx.txIn.size(); j++, inOrdinal++) {
        const auto &txin = tx.txIn[j];
        auto &txinLinked = txLinked.TxIn[j];

        // Spend of an output of this very block; the topology pass checked it
        uint32_t localTxIdx = validationData.InputLocalTx[inOrdinal];
        if (localTxIdx != BC::Proto::CBlockValidationData::NoLocalTx) {
          xmstream s;
          BC::Script::parseTransactionOutput(block.vtx[localTxIdx].txOut[txin.previousOutputIndex], s);
          BTC::Script::UnspentOutputInfo *info = s.data<BTC::Script::UnspentOutputInfo>();
          info->IsLocalTx = 1;
          xvectorFromStream(std::move(s), txinLinked);
          continue;
        }

        // Spend of an output created by an earlier block of the segment
        uint64_t hash = txidHash(txin.previousOutputHash);
        if (const CTxSlot *slot = txFind(hash, txin.previousOutputHash)) {
          BC::Common::CIndexCacheObject *creator = segment.Objects[slot->Block].Object.get();
          BC::Proto::CBlockValidationData &creatorData = creator->validationData();
          const BC::Proto::Transaction &creatorTx = creator->block()->vtx[slot->TxIdx];
          if (txin.previousOutputIndex >= creatorTx.txOut.size()) {
            completable = false;
            continue;
          }

          // Spent once already inside the segment, or not a utxo at all
          size_t ordinal = slot->OutBase + txin.previousOutputIndex;
          if (creatorData.outputSpentLocally(ordinal) || creatorData.outputSpentInBatch(ordinal)) {
            completable = false;
            continue;
          }
          size_t infoSize;
          const void *info = creatorData.outputData(ordinal, infoSize);
          if (!infoSize) {
            completable = false;
            continue;
          }

          txinLinked.resize(infoSize);
          memcpy(txinLinked.begin(), info, infoSize);
          markOrdinal(creatorData.OutputSpentInBatch, ordinal, creatorData.OutputSpentLocally.size() * 64);
          markOrdinal(validationData.InputSpendsInBatch, inOrdinal, validationData.InputLocalTx.size());
          continue;
        }

        // A coin older than the segment: only the serial stage sees the state it lives in
        uint32_t inputIdx = static_cast<uint32_t>(segment.Inputs.size());
        segment.Inputs.push_back(CSegment::CInput{static_cast<uint32_t>(pos),
                                                  static_cast<uint32_t>(txIdx),
                                                  static_cast<uint32_t>(j)});
        if (!spendInsert(outpointHash(txin.previousOutputHash, txin.previousOutputIndex), inputIdx)) {
          segment.Inputs.pop_back();
          completable = false;
        }
      }
    }
    assert(inOrdinal == validationData.InputLocalTx.size());

    uint32_t outBase = 0;
    for (size_t txIdx = 0; txIdx < block.vtx.size(); txIdx++) {
      uint32_t outputsNum = static_cast<uint32_t>(block.vtx[txIdx].txOut.size());
      const BC::Proto::TxHashTy &txid = validationData.TxIds[txIdx];
      CTxSlot *twin = txInsert(txidHash(txid), txid, static_cast<uint32_t>(pos), static_cast<uint32_t>(txIdx), outBase);
      if (twin) {
        // The same txid twice inside the segment: legal only while every output of the earlier
        // one is spent (BIP30), and impossible since BIP34. Otherwise the block hides a live coin
        BC::Common::CIndexCacheObject *twinObject = segment.Objects[twin->Block].Object.get();
        const BC::Proto::CBlockValidationData &twinData = twinObject->validationDataConst();
        uint32_t twinOutputs = static_cast<uint32_t>(twinObject->block()->vtx[twin->TxIdx].txOut.size());

        bool spent = true;
        for (uint32_t i = 0; i < twinOutputs; i++) {
          size_t ordinal = twin->OutBase + i;
          if (!twinData.outputSpentLocally(ordinal) && !twinData.outputSpentInBatch(ordinal)) {
            spent = false;
            break;
          }
        }

        // A BIP30 repeat is valid with the twin's coins still live and overwrites them. Either
        // way the newer transaction becomes the creator a later spend links to
        if (spent || validationData.CoinbaseRepeat) {
          twin->Block = static_cast<uint32_t>(pos);
          twin->TxIdx = static_cast<uint32_t>(txIdx);
          twin->OutBase = outBase;
        } else {
          completable = false;
        }
      }
      outBase += outputsNum;
    }

    // A verdict of the segment, not a lookup that came too early
    segment.Objects[pos].Completable = completable;
    validationData.InputsInvalid = !completable;
  }
}


// A wave helper pays the proof of work of the blocks it takes; the context is per thread and
// lives as long as the pool does
static BC::Common::CheckConsensusCtx &waveConsensusCtx()
{
  static thread_local BC::Common::CheckConsensusCtx ctx;
  static thread_local bool initialized = false;
  if (!initialized) {
    BC::Common::checkConsensusInitialize(ctx);
    initialized = true;
  }

  return ctx;
}

// Proof of work of the whole segment: the check needs only the header, costs the same for every
// block, and here it spreads over the pool in groups a multi-way hash can take
static void checkSegmentWork(CSegment &segment,
                             BC::Common::ChainParams &chainParams,
                             CParallelRunner &runner)
{
  // Headers checked in one call: a multiple of the width a multi-way hash takes
  static constexpr size_t CheckWorkGroup = 64;

  runner.run(segment.Objects.size(), [&segment, &chainParams](size_t begin, size_t end) {
    BC::Common::CheckConsensusCtx &ccCtx = waveConsensusCtx();
    const BC::Proto::BlockHeader *headers[CheckWorkGroup];
    size_t positions[CheckWorkGroup];
    bool results[CheckWorkGroup];
    size_t num = 0;

    auto verify = [&]() {
      BC::Common::checkConsensusMulti(headers, num, ccCtx, chainParams, results);
      for (size_t i = 0; i < num; i++) {
        BC::Common::BlockIndex *index = segment.Objects[positions[i]].Index;
        if (results[i]) {
          index->WorkChecked.store(true, std::memory_order_release);
        } else {
          segment.Objects[positions[i]].Valid = false;
          LOG_F(ERROR,
                "Check Proof-Of-Work failed for block %s",
                index->Header.GetHash().getHexLE().c_str());
        }
      }
      num = 0;
    };

    for (size_t i = begin; i < end; i++) {
      CSegment::CObject &entry = segment.Objects[i];
      if (entry.Index->WorkChecked.load(std::memory_order_acquire))
        continue;

      headers[num] = &entry.Index->Header;
      positions[num] = i;
      if (++num == CheckWorkGroup)
        verify();
    }

    if (num)
      verify();
  });
}

// Everything that needs no mutable chain state, block by block over the whole pool. Heights are
// already published by the index, so the contextual check is paid here too.
static void prepareSegmentBlocks(BC::Common::ChainParams &chainParams,
                                 BC::DB::Storage &storage,
                                 CSegment &segment,
                                 CParallelRunner &runner)
{
  runner.run(segment.Objects.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      CSegment::CObject &entry = segment.Objects[i];
      BC::Common::BlockIndex *index = entry.Index;

      if (!entry.Valid)
        continue;

      intrusive_ptr<BC::Common::CIndexCacheObject> object(index->Serialized);
      if (!object.get()) {
        // Written to disk by an earlier connect and asked for again by a reorg
        object = objectByIndexChecked(index, chainParams, storage.blockDb());
      }
      entry.Relay = object.get()->relay();

      BC::Proto::Block *block = object.get()->block();
      BC::Proto::CBlockValidationData &validationData = object.get()->validationData();
      BC::Proto::CBlockLinkedOutputs &linkedOutputs = object.get()->linkedOutputs();

      std::string error;
      if (validationData.TxIds.size() != block->vtx.size()) {
        BC::Common::initializeValidationContext(*block, validationData);
        if (!BC::Common::checkBlockStandalone(*block, validationData, chainParams, error)) {
          LOG_F(WARNING, "block %s check failed, error: %s", block->header.GetHash().getHexLE().c_str(), error.c_str());
          entry.Valid = false;
          continue;
        }
      }

      if (!BC::Common::checkBlockContextual(*index, *block, validationData, linkedOutputs, chainParams, error)) {
        LOG_F(ERROR,
              "Block %s (%u) contextual check failed, error: %s",
              index->Header.GetHash().getHexLE().c_str(),
              index->Height,
              error.c_str());
        entry.Valid = false;
        continue;
      }

      validationData.InputsResolved = false;
      validationData.InputsInvalid = false;
      validationData.dropPairs();

      index->IndexState.store(BSBlock);
      index->Serialized.reset(object.get());
      entry.Object = object;
    }
  });
}

// Everything above the block the chain stops at is a descendant of it, so it goes with it
static void cutSegment(CSegment &segment, size_t keep, const char *reason)
{
  const size_t count = segment.Objects.size();
  LOG_F(ERROR,
        "Block pipeline: block %s (%u) %s, %zu blocks above it dropped",
        segment.Objects[keep].Index->Header.GetHash().getHexLE().c_str(),
        segment.Objects[keep].Index->Height,
        reason,
        count - keep - 1);

  for (size_t i = keep; i < count; i++) {
    segment.Objects[i].Index->IndexState.store(BSInvalid);
  }

  segment.Objects.resize(keep);
}

bool prepareSegment(BC::Common::ChainParams &chainParams,
                    BC::DB::Storage &storage,
                    CParallelRunner &runner,
                    CSegment &segment,
                    bool prefetch)
{
  if (segment.Objects.empty())
    return false;

  checkSegmentWork(segment, chainParams, runner);
  prepareSegmentBlocks(chainParams, storage, segment, runner);

  // Prepared whole or not at all: a failing block cuts the chain there, and segments bitten after
  // this one continue a chain that will not happen. The caller takes them all back - the rejected
  // block is BSInvalid now, so the next bite stops before it and the good part comes back
  {
    size_t valid = 0;
    while (valid < segment.Objects.size() && segment.Objects[valid].Valid)
      valid++;
    if (valid < segment.Objects.size()) {
      cutSegment(segment, valid, "rejected by its own checks");
      return false;
    }
  }

  resolveSegmentInputs(segment);

  // Everything the blocks of this segment will hold until they connect is built now
  for (const CSegment::CObject &entry: segment.Objects) {
    if (entry.Object.get())
      entry.Object.get()->reaccount();
  }

  {
    size_t completable = 0;
    while (completable < segment.Objects.size() && segment.Objects[completable].Completable)
      completable++;
    if (completable < segment.Objects.size()) {
      cutSegment(segment, completable, "spends what it may not");
      return false;
    }
  }

  // Warms the cache for the existence wave on the critical path. A probe here has no authority:
  // it may be stale by the time the segment connects, only the wave decides existence
  if (prefetch && !segment.Inputs.empty()) {
    const BC::DB::UTXODb &db = storage.utxodb();
    runner.run(segment.Inputs.size(), [&segment, &db](size_t begin, size_t end) {
      xvector<uint8_t> value;
      for (size_t i = begin; i < end; i++) {
        const CSegment::CInput &input = segment.Inputs[i];
        BC::Common::CIndexCacheObject *object = segment.Objects[input.Object].Object.get();
        const auto &txin = object->block()->vtx[input.TxIdx].txIn[input.InIdx];
        db.query(txin.previousOutputHash, txin.previousOutputIndex, value, /*cacheOnly=*/true);
      }
    });
  }

  return true;
}

bool connectSegment(BlockInMemoryIndex &blockIndex,
                    BC::Common::ChainParams &chainParams,
                    BC::DB::Storage &storage,
                    CParallelRunner &runner,
                    CSegment &segment,
                    size_t *failedAt)
{

  *failedAt = 0;
  bool result = true;
  BC::Common::BlockIndex *head = segment.Objects.front().Index;

  // The chain may stand elsewhere than when the segment was admitted: only disconnects are
  // needed here; the new path is the segment itself.
  if (head->Prev != blockIndex.best()) {
    if (!switchTo(head->Prev, chainParams, blockIndex, storage)) {
      LOG_F(ERROR, "Block database corrupted");
      abort();
    }
  }

  // Only now does the database hold the state the segment was built on
  resolveSegmentResidual(segment, storage.utxodb(), runner);

  for (size_t i = 0; i < segment.Objects.size(); i++) {
    if (!segment.Objects[i].Completable) {
      *failedAt = i;
      result = false;
      break;
    }
  }

  if (result) {
    // The segment reaches the databases as one batch - the same unit it was prepared and
    // judged as. The index bookkeeping stays per block and goes first: it is what makes
    // these blocks the chain the databases are about to be told about
    std::vector<BC::DB::CBlockRef> batch;
    batch.reserve(segment.Objects.size());
    for (CSegment::CObject &entry: segment.Objects) {
      BC::Common::CIndexCacheObject *object = entry.Object.get();
      markConnected(entry.Index, blockIndex);
      batch.push_back(BC::DB::CBlockRef{entry.Index, object->block(), &object->linkedOutputs(), &object->validationData()});
    }

    storage.connect(batch, blockIndex);
    blockIndex.setBest(segment.Objects.back().Index);
  }


  return result;
}


// The predecessor is already known for every block of a continuous run, and that is the whole
// reindex: look before allocating, or the stub costs an allocation per block
static BC::Common::BlockIndex *findOrCreateStub(BlockInMemoryIndex &blockIndex,
                                                 const BC::Proto::BlockHashTy &hash)
{
  auto existing = blockIndex.blockIndex().find(hash);
  if (existing != blockIndex.blockIndex().end())
    return existing->second;

  BC::Common::BlockIndex *stub = BC::Common::BlockIndex::create(BSEmpty, nullptr);
  auto [it, inserted] = blockIndex.blockIndex().insert(std::pair(hash, stub));
  if (inserted)
    return stub;

  delete stub;
  return it->second;
}

// Claims an empty stub, or a completed header when block data is being attached. BSClaimed keeps
// the index private until Header, Prev and the decoded-object fields match its published state.
// Losers spin here, so nothing that can block may run before the claim is released.
static bool claimIndex(BC::Common::BlockIndex *index,
                       bool acceptHeader,
                       bool *hadHeader = nullptr)
{
  BlockStatus state = index->IndexState.load(std::memory_order_acquire);
  for (;;) {
    if (state == BSClaimed) {
      std::this_thread::yield();
      state = index->IndexState.load(std::memory_order_acquire);
      continue;
    }
    if (state != BSEmpty && !(acceptHeader && state == BSHeader))
      return false;
    if (index->IndexState.compare_exchange_weak(state,
                                                BSClaimed,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
      if (hadHeader)
        *hadHeader = state == BSHeader;
      return true;
    }
  }
}

// The caller verifies the whole received header message with checkConsensusMulti() first.
BC::Common::BlockIndex *AddHeader(BlockInMemoryIndex &blockIndex,
                                  BC::Common::ChainParams &chainParams,
                                  const BC::Proto::BlockHeader &header)
{
  const BC::Proto::BlockHashTy hash = header.GetHash();
  BC::Common::BlockIndex *index = nullptr;

  auto existing = blockIndex.blockIndex().find(hash);
  if (existing != blockIndex.blockIndex().end()) {
    index = existing->second;
    if (!claimIndex(index, false)) {
      // WorkChecked is monotonic: a checked headers message may race with block publication.
      index->WorkChecked.store(true, std::memory_order_release);
      return index;
    }
  }

  BC::Common::BlockIndex *prevIndex = findOrCreateStub(blockIndex, header.hashPrevBlock);

  if (!index) {
    index = BC::Common::BlockIndex::create(BSClaimed, nullptr);
    auto inserted = blockIndex.blockIndex().insert(std::pair(hash, index));
    if (!inserted.second) {
      delete index;
      index = inserted.first->second;
      if (!claimIndex(index, false)) {
        index->WorkChecked.store(true, std::memory_order_release);
        return index;
      }
    }
  }

  index->Prev = prevIndex;
  index->Header = header;
  index->WorkChecked.store(true, std::memory_order_release);
  index->IndexState.store(BSHeader, std::memory_order_release);

  index->ConcurrentHeaderNext = WaitPtr<BC::Common::BlockIndex>();
  index->ConcurrentHeaderNext = prevIndex->SuccessorHeaders.exchange(index, 0);
  if (index->ConcurrentHeaderNext.tag() == 1)
    BuildHeaderChain(blockIndex, chainParams, prevIndex);

  return index;
}


EBlockDataResult acceptBlockData(BlockInMemoryIndex &blockIndex,
                                 BC::Common::ChainParams &chainParams,
                                 const intrusive_ptr<BC::Common::CIndexCacheObject> &object,
                                 uint32_t fileNo,
                                 uint32_t fileOffset,
                                 BC::Common::BlockIndex **accepted)
{
  BC::Proto::Block *block = object.get() ? object.get()->block() : nullptr;
  if (!block)
    return EBlockDataResult::Invalid;

  const BC::Proto::BlockHeader &header = block->header;
  const BC::Proto::BlockHashTy hash = header.GetHash();
  BC::Common::BlockIndex *index = nullptr;
  bool alreadyHaveHeader = false;

  auto existing = blockIndex.blockIndex().find(hash);
  if (existing != blockIndex.blockIndex().end()) {
    index = existing->second;
    if (!claimIndex(index, true, &alreadyHaveHeader)) {
      if (accepted)
        *accepted = index;
      return EBlockDataResult::Duplicate;
    }
  }

  BC::Common::BlockIndex *prevIndex = findOrCreateStub(blockIndex, header.hashPrevBlock);

  if (!index) {
    index = BC::Common::BlockIndex::create(BSClaimed, nullptr);
    auto inserted = blockIndex.blockIndex().insert(std::pair(hash, index));
    if (!inserted.second) {
      delete index;
      index = inserted.first->second;
      if (!claimIndex(index, true, &alreadyHaveHeader)) {
        if (accepted)
          *accepted = index;
        return EBlockDataResult::Duplicate;
      }
    }
  }

  if (!alreadyHaveHeader) {
    index->Prev = prevIndex;
    index->Header = header;
  }

  index->FileNo = fileNo;
  index->FileOffset = fileOffset;
  index->SerializedBlockSize = static_cast<uint32_t>(object.get()->blockData().size());
  index->Serialized.reset(object.get());
  index->IndexState.store(BSData, std::memory_order_release);

  if (!alreadyHaveHeader) {
    index->ConcurrentHeaderNext = WaitPtr<BC::Common::BlockIndex>();
    index->ConcurrentHeaderNext = prevIndex->SuccessorHeaders.exchange(index, 0);
    if (index->ConcurrentHeaderNext.tag() == 1)
      BuildHeaderChain(blockIndex, chainParams, prevIndex);
  }

  // Publish only after the decoded object and disk coordinates are visible. If the predecessor
  // was published already, this caller becomes (or joins) the flat combiner that releases every
  // descendant whose former hole has just disappeared.
  index->ConcurrentBlockNext = WaitPtr<BC::Common::BlockIndex>();
  index->ConcurrentBlockNext = prevIndex->SuccessorBlocks.exchange(index, 0);
  if (index->ConcurrentBlockNext.tag() == 1) {
    BC::Common::BlockIndex *candidate = nullptr;
    // The accepted block is the combiner task because it is published exactly once. Several
    // children may release the same predecessor concurrently, so the predecessor itself is not
    // a unique task key; each task still drains the data graph from that predecessor.
    blockIndex.combiner().call(index, [&candidate](BC::Common::BlockIndex *acceptedIndex) {
      BC::Common::BlockIndex *tip = BuildReadyChain(acceptedIndex->Prev);
      if (tip && (!candidate || tip->ChainWork > candidate->ChainWork))
        candidate = tip;
    });
    if (candidate)
      blockIndex.notifyReady(candidate);
  }

  if (accepted)
    *accepted = index;
  return EBlockDataResult::Accepted;
}

EBlockDataResult acceptNetworkBlock(BlockInMemoryIndex &blockIndex,
                                    BC::Common::ChainParams &chainParams,
                                    BC::DB::Storage &storage,
                                    void *data,
                                    size_t size,
                                    size_t memorySize,
                                    bool relay,
                                    BC::Common::BlockIndex **accepted)
{
  size_t unpackedSize = 0;
  xmstream stream(data, size);
  BC::Proto::Block *block = BTC::unpack2<BC::Proto::Block>(stream, &unpackedSize);
  if (!block || stream.remaining() != 0) {
    operator delete(block);
    operator delete(data);
    return EBlockDataResult::Invalid;
  }

  intrusive_ptr<BC::Common::CIndexCacheObject> object(
    new BC::Common::CIndexCacheObject(&storage.cache(), data, size, memorySize,
                                      block, unpackedSize, relay));
  return acceptBlockData(blockIndex,
                         chainParams,
                         object,
                         std::numeric_limits<uint32_t>::max(),
                         std::numeric_limits<uint32_t>::max(),
                         accepted);
}

BC::Common::BlockIndex *bestReadyBlock(BlockInMemoryIndex &blockIndex)
{
  BC::Common::BlockIndex *best = blockIndex.best();
  for (const auto &entry: blockIndex.blockIndex()) {
    BC::Common::BlockIndex *index = entry.second;
    if (!index->ready() ||
        index->IndexState.load(std::memory_order_relaxed) == BSInvalid ||
        (best && !(index->ChainWork > best->ChainWork)))
      continue;

    BC::Common::BlockIndex *ancestor = index;
    while (ancestor && ancestor != blockIndex.genesis()) {
      if (!ancestor->ready() ||
          ancestor->IndexState.load(std::memory_order_relaxed) == BSInvalid)
        break;
      ancestor = ancestor->Prev;
    }
    if (ancestor == blockIndex.genesis())
      best = index;
  }
  return best;
}


static bool decodeIndexRange(BlockInMemoryIndex &blockIndex,
                             const std::vector<size_t> &blockFileSizes,
                             const uint8_t *fileData,
                             const BlockPosition *positions,
                             size_t count,
                             BC::Common::BlockIndex **output,
                             const char *path)
{
  for (size_t i = 0; i < count; i++) {
    BC::Common::BlockIndex *index = BC::Common::BlockIndex::create(BSBlock, nullptr);
    xmstream stream(const_cast<uint8_t*>(fileData) + positions[i].Offset, positions[i].Size);
    if (!BC::unserializeAndCheck(stream, *index) || stream.remaining() != 0) {
      delete index;
      LOG_F(ERROR, "Can't read index data from %s", path);
      return false;
    }

    index->SuccessorHeaders.set(nullptr, 1);
    index->SuccessorBlocks.set(nullptr, 1);
    index->WorkChecked.store(true, std::memory_order_relaxed);
    index->HeaderReady.store(true, std::memory_order_relaxed);
    index->DataReady.store(true, std::memory_order_relaxed);

    bool blockPresent = index->FileNo < blockFileSizes.size();
    if (blockPresent) {
      const size_t fileSize = blockFileSizes[index->FileNo];
      const size_t offset = index->FileOffset;
      blockPresent = offset <= fileSize && fileSize - offset >= 8 &&
                     index->SerializedBlockSize <= fileSize - offset - 8;
    }
    if (!blockPresent) {
      LOG_F(ERROR,
            "Index loader: no block data on disk for %s",
            index->Header.GetHash().getHexLE().c_str());
      delete index;
      return false;
    }

    const BC::Proto::BlockHashTy hash = index->Header.GetHash();
    auto [it, inserted] = blockIndex.blockIndex().insert(std::pair(hash, index));
    if (!inserted) {
      LOG_F(ERROR, "Duplicate block %s in %s", hash.getHexLE().c_str(), path);
      delete index;
      return false;
    }
    output[i] = index;
  }

  return true;
}

bool loadingBlockIndex(BlockInMemoryIndex &blockIndex,
                       const std::filesystem::path &blockPath,
                       const std::filesystem::path &indexPath)
{
  LOG_F(INFO, "Loading block index...");

  char fileName[64];
  std::vector<size_t> blockFileSizes;

  for (uint32_t fileNo = 0; ; fileNo++) {
    snprintf(fileName, sizeof(fileName), "blk%05u.dat", fileNo);
    std::filesystem::path path = blockPath / fileName;
    if (!std::filesystem::exists(path))
      break;
    blockFileSizes.push_back(std::filesystem::file_size(path));
  }

  const unsigned threadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;
  CParallelRunner runner;
  runner.start(threadsNum > 1 ? threadsNum - 1 : 0);

  std::vector<BlockPosition> positions;
  std::vector<BC::Common::BlockIndex*> allIndexes;
  for (uint32_t fileNo = 0; ; fileNo++) {
    snprintf(fileName, sizeof(fileName), "index%05u.dat", fileNo);
    std::filesystem::path path = indexPath / fileName;
    if (!std::filesystem::exists(path))
      break;

    const std::string pathUtf8 = pathToUtf8(path);
    const size_t indexFileSize = std::filesystem::file_size(path);
    if (indexFileSize > std::numeric_limits<uint32_t>::max()) {
      LOG_F(ERROR, "Index file %s is too large", pathUtf8.c_str());
      return false;
    }
    std::unique_ptr<uint8_t[]> data(new uint8_t[indexFileSize]);

    if (indexFileSize) {
      std::unique_ptr<FILE, std::function<void(FILE*)>> hFile(fopen_path(path, "rb"), [](FILE *f) { fclose(f); });
      if (!hFile) {
        LOG_F(ERROR, "Can't open index file %s", pathUtf8.c_str());
        return false;
      }
      if (fread(data.get(), 1, indexFileSize, hFile.get()) != indexFileSize) {
        LOG_F(ERROR, "Can't read index file %s", pathUtf8.c_str());
        return false;
      }
    }

    positions.clear();
    xmstream stream(data.get(), indexFileSize);
    while (stream.remaining()) {
      if (stream.remaining() < sizeof(uint32_t)) {
        LOG_F(ERROR, "Truncated index record in %s", pathUtf8.c_str());
        return false;
      }
      uint32_t size = 0;
      BC::unserialize(stream, size);
      if (!size || size > stream.remaining()) {
        LOG_F(ERROR, "Invalid index size %u detected in file %s", size, pathUtf8.c_str());
        return false;
      }

      positions.push_back(BlockPosition{static_cast<uint32_t>(stream.offsetOf()), size});
      stream.seek<uint8_t>(size);
    }

    const size_t first = allIndexes.size();
    allIndexes.resize(first + positions.size());
    std::atomic<bool> decodeFailed = false;
    runner.run(positions.size(), [&](size_t begin, size_t end) {
      if (!decodeIndexRange(blockIndex,
                            blockFileSizes,
                            data.get(),
                            positions.data() + begin,
                            end - begin,
                            allIndexes.data() + first + begin,
                            pathUtf8.c_str()))
        decodeFailed.store(true, std::memory_order_relaxed);
    });
    if (decodeFailed.load(std::memory_order_relaxed))
      return false;
  }

  std::atomic<bool> linkFailed = false;
  runner.run(allIndexes.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      BC::Common::BlockIndex *index = allIndexes[i];
      auto prev = blockIndex.blockIndex().find(index->Header.hashPrevBlock);
      if (prev == blockIndex.blockIndex().end()) {
        LOG_F(ERROR,
              "Index loader: previous block is missing for %s",
              index->Header.GetHash().getHexLE().c_str());
        linkFailed.store(true, std::memory_order_relaxed);
      } else {
        index->Prev = prev->second;
      }
    }
  });
  if (linkFailed.load(std::memory_order_relaxed))
    return false;

  BC::Common::BlockIndex *bestIndex = blockIndex.genesis();
  for (BC::Common::BlockIndex *index: allIndexes) {
    if (index->ChainWork > bestIndex->ChainWork)
      bestIndex = index;
  }

  LOG_F(INFO, "Loaded %zu blocks", allIndexes.size());

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
    // Loaded indexes hold the whole tree; on chain are those the restore walk passes
    index->OnChain.store(true, std::memory_order_relaxed);
    index = index->Prev;
  }

  index->OnChain.store(true, std::memory_order_relaxed);
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


static bool decodeBlockRange(BlockInMemoryIndex &blockIndex,
                             BC::Common::ChainParams &chainParams,
                             BC::DB::Storage &storage,
                             const uint8_t *fileData,
                             const BlockPosition *positions,
                             size_t count,
                             uint32_t fileNo)
{
  for (size_t i = 0; i < count; i++) {
    const BlockPosition &position = positions[i];
    size_t unpackedSize = 0;
    xmstream stream(const_cast<uint8_t*>(fileData) + position.Offset + 8, position.Size);
    BC::Proto::Block *block = BTC::unpack2<BC::Proto::Block>(stream, &unpackedSize);
    if (!block || stream.remaining() != 0) {
      operator delete(block);
      return false;
    }

    // The serialized bytes remain in the block file and are released with fileData after this
    // wave. Only the decoded block is cached; its index is the durable reference back to disk.
    intrusive_ptr<BC::Common::CIndexCacheObject> object(
      new BC::Common::CIndexCacheObject(&storage.cache(), nullptr, position.Size, 0,
                                        block, unpackedSize));
    if (acceptBlockData(blockIndex,
                        chainParams,
                        object,
                        fileNo,
                        position.Offset,
                        nullptr) == EBlockDataResult::Invalid)
      return false;
  }
  return true;
}

bool reindex(BlockInMemoryIndex &blockIndex,
             const std::filesystem::path &blockPath,
             BC::Common::ChainParams &chainParams,
             BC::DB::Storage &storage,
             CBlockPipeline &pipeline)
{
  char blockFileName[64];
  uint32_t fileNo = 0;
  size_t totalBlockCount = 0;
  uint64_t totalBytesRead = 0;
  const auto startTime = std::chrono::steady_clock::now();
  const unsigned hardwareThreads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;
  std::vector<BlockPosition> positions;

  // One pool for the whole run: a fresh thread per block file costs more than the decode of a
  // small file, and there are thousands of files
  CParallelRunner runner;
  runner.start(hardwareThreads > 1 ? hardwareThreads - 1 : 0);

  pipeline.setBulkFeed(true);
  for (;;) {
    snprintf(blockFileName, sizeof(blockFileName), "blk%05u.dat", fileNo);
    const std::filesystem::path path = blockPath / blockFileName;
    if (!std::filesystem::exists(path))
      break;

    while (pipeline.throttled())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const std::string pathUtf8 = pathToUtf8(path);
    const size_t fileSize = std::filesystem::file_size(path);
    std::unique_ptr<uint8_t[]> fileData(new uint8_t[fileSize]);
    LOG_F(INFO, "Loading block file %s ...", pathUtf8.c_str());

    std::unique_ptr<FILE, std::function<void(FILE*)>> file(fopen_path(path, "rb"), [](FILE *f) { fclose(f); });
    if (!file || (fileSize && fread(fileData.get(), 1, fileSize, file.get()) != fileSize)) {
      LOG_F(ERROR, "Can't read block file %s", pathUtf8.c_str());
      return false;
    }

    // First pass is deliberately serial: it only identifies flat record boundaries. The second
    // pass below splits these independent slices over all hardware threads.
    positions.clear();
    xmstream stream(fileData.get(), fileSize);
    while (stream.remaining()) {
      size_t recordOffset = stream.offsetOf();
      if (fileData[recordOffset] == 0) {
        while (recordOffset < fileSize && fileData[recordOffset] == 0)
          recordOffset++;
        if (recordOffset == fileSize)
          break;
        stream.seekSet(recordOffset);
      }

      if (stream.remaining() < 8) {
        LOG_F(ERROR, "Can't parse block file %s (truncated record header)", pathUtf8.c_str());
        return false;
      }

      uint32_t magic = 0;
      uint32_t blockSize = 0;
      BC::unserialize(stream, magic);
      BC::unserialize(stream, blockSize);
      if (magic != chainParams.magic || !blockSize || blockSize > stream.remaining()) {
        LOG_F(ERROR, "Can't parse block file %s (invalid record)", pathUtf8.c_str());
        return false;
      }

      positions.push_back(BlockPosition{static_cast<uint32_t>(stream.offsetOf() - 8), blockSize});
      stream.seek<uint8_t>(blockSize);
    }

    std::atomic<bool> decodeFailed = false;
    runner.run(positions.size(), [&](size_t begin, size_t end) {
      if (!decodeBlockRange(blockIndex, chainParams, storage, fileData.get(),
                            positions.data() + begin, end - begin, fileNo))
        decodeFailed.store(true, std::memory_order_relaxed);
    });

    if (decodeFailed.load(std::memory_order_relaxed)) {
      LOG_F(ERROR, "Can't parse block file %s (invalid block structure)", pathUtf8.c_str());
      return false;
    }

    LOG_F(INFO,
          "%u blocks decoded from %s; cache: %.3lfM best: [%u]%s",
          static_cast<unsigned>(positions.size()),
          pathUtf8.c_str(),
          storage.cache().size() / 1048576.0f,
          blockIndex.best()->Height,
          blockIndex.best()->Header.GetHash().getHexLE().c_str());

    totalBlockCount += positions.size();
    totalBytesRead += fileSize;
    fileNo++;
  }

  // Ending bulk mode cuts the only deliberately short segment: the final tail.
  pipeline.setBulkFeed(false);
  pipeline.waitDrained();
  while (!storage.queue().empty())
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  BC::Common::BlockIndex *best = blockIndex.best();
  LOG_F(INFO, "%zu blocks loaded from disk", totalBlockCount);
  LOG_F(INFO, "Best block is %s (%u)", best->Header.GetHash().getHexLE().c_str(), best->Height);

  const double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - startTime).count() / 1000.0;
  const double megabytes = totalBytesRead / 1048576.0;
  LOG_F(INFO, "Reindex speed: %.2lf MB/s (%.1lf MB in %.1lf seconds)",
        elapsed > 0.0 ? megabytes / elapsed : 0.0, megabytes, elapsed);
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
  SmallStream<1024> data;
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
      LOG_F(ERROR, "Invalid block data in file %s", pathToUtf8(path).c_str());
      ErrorHandler_();
      return;
    }

    if (blockSize != ExpectedBlockSizes_[expectedIndex]) {
      char fileName[64];
      snprintf(fileName, sizeof(fileName), "blk%05u.dat", fileNo);
      std::filesystem::path path = blocksDirectory / fileName;
      LOG_F(ERROR, "Invalid block data in file %s: mismatch block size in index(%u) and data file(%u)", pathToUtf8(path).c_str(), ExpectedBlockSizes_[expectedIndex], blockSize);
      ErrorHandler_();
      return;
    }

    expectedIndex++;
    Handler_(data, blockSize);
  }

  ExpectedBlockSizes_.clear();
}

// One read per contiguous run: the walk and the block database are both in chain order, so a
// whole batch is usually one or two reads
CCatchUpReader::CCatchUpReader(BlockDatabase &blockDb,
                               BC::Common::ChainParams &chainParams,
                               CAllocationInfo &allocationInfo,
                               BC::Common::BlockIndex *first,
                               size_t batchSizeLimit,
                               size_t batchBlocksLimit,
                               unsigned waveThreads) :
  BlockDb_(blockDb),
  ChainParams_(chainParams),
  AllocationInfo_(allocationInfo),
  Cursor_(first),
  BatchSizeLimit_(batchSizeLimit),
  BatchBlocksLimit_(batchBlocksLimit)
{
  if (!waveThreads)
    waveThreads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;
  Runner_.start(waveThreads > 1 ? waveThreads - 1 : 0);
}

bool CCatchUpReader::readRecords(LinearDataStorage &storage, const std::vector<CRecord> &records, void *destination)
{
  uint8_t *out = static_cast<uint8_t*>(destination);
  size_t i = 0;

  while (i < records.size()) {
    uint32_t size = records[i].Size;
    size_t j = i + 1;
    while (j < records.size() &&
           records[j].FileNo == records[i].FileNo &&
           records[j].Offset == records[i].Offset + size) {
      size += records[j].Size;
      j++;
    }

    if (!storage.read(records[i].FileNo, records[i].Offset, out, size)) {
      LOG_F(ERROR,
            "Can't read data from %s (offset = %u, size = %u)",
            storage.getFilePath(records[i].FileNo).c_str(),
            records[i].Offset,
            size);
      return false;
    }

    out += size;
    i = j;
  }

  return true;
}

std::unique_ptr<CSegment> CCatchUpReader::next()
{
  if (Failed_)
    return nullptr;

  Indexes_.clear();
  BlockRecords_.clear();
  LinkedOutputsRecords_.clear();
  size_t blockBytes = 0;
  size_t linkedOutputsBytes = 0;
  size_t rawBytes = 0;

  for (; Cursor_; Cursor_ = Cursor_->Next) {
    BC::Common::BlockIndex *index = Cursor_;
    if (!index->blockStored() || !index->indexStored()) {
      LOG_F(ERROR,
            "Block %s (%u) is on the chain but not in the block database",
            index->Header.GetHash().getHexLE().c_str(),
            index->Height);
      Failed_ = true;
      return nullptr;
    }

    // Records are read whole, with their <magic>:4 <size>:4 (blocks) and <size>:4 (linked
    // outputs) prefixes, so that neighbours stay contiguous
    const uint32_t blockRecord = index->SerializedBlockSize + 8;
    const uint32_t linkedOutputsRecord = index->LinkedOutputsSerializedSize + 4;

    // Cut before the block that would overflow the batch, never after it
    if (!BlockRecords_.empty() &&
        (rawBytes + index->SerializedBlockSize > BatchSizeLimit_ ||
         BlockRecords_.size() >= BatchBlocksLimit_))
      break;

    BlockRecords_.push_back(CRecord{index->FileNo, index->FileOffset, blockRecord});
    LinkedOutputsRecords_.push_back(CRecord{index->LinkedOutputsFileNo, index->LinkedOutputsFileOffset, linkedOutputsRecord});
    Indexes_.push_back(index);
    blockBytes += blockRecord;
    linkedOutputsBytes += linkedOutputsRecord;
    rawBytes += index->SerializedBlockSize;
  }

  if (Indexes_.empty())
    return nullptr;

  std::unique_ptr<uint8_t[]> blockBlob(new uint8_t[blockBytes]);
  std::unique_ptr<uint8_t[]> linkedOutputsBlob(new uint8_t[linkedOutputsBytes]);
  if (!readRecords(BlockDb_.blockReader(), BlockRecords_, blockBlob.get()) ||
      !readRecords(BlockDb_.linkedOutputsReader(), LinkedOutputsRecords_, linkedOutputsBlob.get())) {
    Failed_ = true;
    return nullptr;
  }

  struct CStoredView {
    const void *BlockData;
    const void *LinkedOutputsData;
    uint32_t BlockSize;
    uint32_t LinkedOutputsSize;
  };

  auto segment = std::make_unique<CSegment>();
  segment->Size = rawBytes;
  segment->Objects.resize(Indexes_.size());
  std::vector<CStoredView> views(Indexes_.size());
  uint8_t *blockData = blockBlob.get();
  uint8_t *linkedOutputsData = linkedOutputsBlob.get();

  for (size_t i = 0; i < Indexes_.size(); i++) {
    BC::Common::BlockIndex *index = Indexes_[i];
    CSegment::CObject &entry = segment->Objects[i];
    entry.Index = index;

    uint32_t magic = 0;
    uint32_t blockSize = 0;
    uint32_t linkedOutputsSize = 0;
    {
      xmstream stream(blockData, 8);
      BC::unserialize(stream, magic);
      BC::unserialize(stream, blockSize);
    }
    {
      xmstream stream(linkedOutputsData, 4);
      BC::unserialize(stream, linkedOutputsSize);
    }

    // What the index says the record is must be what the record says it is
    if (magic != BlockDb_.magic() ||
        blockSize != index->SerializedBlockSize ||
        linkedOutputsSize != index->LinkedOutputsSerializedSize) {
      LOG_F(ERROR,
            "Stored block %s (%u) does not match its index entry",
            index->Header.GetHash().getHexLE().c_str(),
            index->Height);
      Failed_ = true;
      return nullptr;
    }

    views[i] = CStoredView{blockData + 8, linkedOutputsData + 4, blockSize, linkedOutputsSize};
    blockData += blockSize + 8;
    linkedOutputsData += linkedOutputsSize + 4;
  }

  std::atomic<bool> decodeFailed = false;
  Runner_.run(segment->Objects.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      CSegment::CObject &entry = segment->Objects[i];
      const CStoredView &view = views[i];
      entry.Object = objectFromStoredBytes(entry.Index,
                                           ChainParams_,
                                           view.BlockData,
                                           view.BlockSize,
                                           view.LinkedOutputsData,
                                           view.LinkedOutputsSize,
                                           &AllocationInfo_);
      if (!entry.Object.get()) {
        LOG_F(ERROR,
              "Can't rebuild stored block %s (%u), block database is damaged",
              entry.Index->Header.GetHash().getHexLE().c_str(),
              entry.Index->Height);
        decodeFailed.store(true, std::memory_order_relaxed);
      }
    }
  });

  if (decodeFailed.load(std::memory_order_relaxed)) {
    Failed_ = true;
    return nullptr;
  }

  Indexes_.clear();
  return segment;
}
