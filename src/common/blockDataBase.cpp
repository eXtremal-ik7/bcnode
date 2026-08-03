// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockDataBase.h"
#include "db/keyHash.h"
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

// Every input still empty: from the database, else from an output of this very block. A worker
// runs it on a block outside a run, where the state it needs may not exist yet - what it could
// not answer InputsResolved reports. The connect thread runs it on exactly those, and there the
// state is the one the block connects to
static bool resolveBlockInputs(BC::Proto::CBlockLinkedOutputs &linkedOutputs, BC::Proto::CBlockValidationData &validationData, BC::Proto::Block &block, const BC::DB::UTXODb &db)
{
  std::unordered_set<CUnspentOutputKey> removed;

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
  index->Prev->Next = index;
  index->Prepared.store(true, std::memory_order_relaxed);
  blockIndex.blockHeightIndex()[index->Height] = index;
  storage.add(BC::DB::Connect, index, block, linkedOutputs, validationData, blockIndex);
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
  // Free to be bitten into a segment again: the block is not below the frontier any more
  index->Prepared.store(false, std::memory_order_relaxed);
  blockIndex.blockHeightIndex()[index->Height] = nullptr;
  storage.add(BC::DB::Disconnect, index, block, linkedOutputs, validationData, blockIndex);
  // The segment this block came in is broken from here: the disconnect put the hidden outputs
  // back, and every later connect of it must be plain. Only utxodb reads the marks, and it took
  // its half of the disconnect above, on this thread
  validationData.dropPairs();
}

// A height is what makes a block a candidate: until the header chain reaches it, nothing knows
// where it stands. Data may have been waiting for this for a whole block file
static void BuildHeaderChain(BlockInMemoryIndex &blockIndex, BC::Common::ChainParams &chainParams, BC::Common::BlockIndex *start)
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
      if (current->Raw.load(std::memory_order_acquire) || current->Serialized.get())
        blockIndex.candidateTracker().update(current);
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
  }

  // A disk-reloaded object must satisfy the same invariant as a fresh one:
  // validation data (txids included) is filled before any connect/disconnect
  BC::Common::initializeValidationContext(*block, object.get()->validationData());
  object.get()->validationData().InputsResolved = true;

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
      auto object = objectByIndexChecked(lb, storage.blockDb());
      DisconnectBlock(blockIndex, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationData(), storage, lb, false);
      lb = lb->Prev;
    }
    while (sb != lb) {
      BC::Proto::Block diskBlock;
      newPath.push_back(sb);
      auto object = objectByIndexChecked(lb, storage.blockDb());
      DisconnectBlock(blockIndex, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationData(), storage, lb, false);
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
  return occupied(hashOutpoint(txid.begin(), index).H1);
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
        if (!results[i]) {
          segment.Objects[positions[i]].Valid = false;
          LOG_F(ERROR,
                "Check Proof-Of-Work failed for block %s",
                segment.Objects[positions[i]].Index->Header.GetHash().getHexLE().c_str());
        }
      }
      num = 0;
    };

    for (size_t i = begin; i < end; i++) {
      CSegment::CObject &entry = segment.Objects[i];
      CBlockRawData *raw = entry.Index->Raw.load(std::memory_order_acquire);
      // No raw data left means the block was prepared before and checked then; a header that
      // came through AddHeader paid for its work already
      if (!raw || !raw->CheckWork)
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

// Unpack and everything that needs no chain state, block by block over the whole pool. Heights
// are known from the index in a pull pipeline, so the contextual check is paid here too
static void prepareSegmentBlocks(BC::Common::ChainParams &chainParams,
                                 BC::DB::Storage &storage,
                                 CSegment &segment,
                                 CParallelRunner &runner,
                                 CPipelineCounters &counters)
{
  std::atomic<uint64_t> parseErrors = 0;
  std::atomic<size_t> consumed = 0;

  runner.run(segment.Objects.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      CSegment::CObject &entry = segment.Objects[i];
      BC::Common::BlockIndex *index = entry.Index;

      // Taken even from a block the work check rejected: until it is released the reader
      // counts it as read ahead
      std::unique_ptr<CBlockRawData> raw(index->Raw.exchange(nullptr, std::memory_order_acq_rel));
      if (raw)
        consumed += raw->Size;
      if (!entry.Valid)
        continue;

      intrusive_ptr<BC::Common::CIndexCacheObject> object(index->Serialized);
      if (object.get()) {
        // A segment that had to be cut and bitten again: the block keeps everything but its
        // links
      } else if (raw) {
        size_t unpackedSize = 0;
        xmstream stream(raw->data(), raw->Size);
        BC::Proto::Block *block = BTC::unpack2<BC::Proto::Block>(stream, &unpackedSize);
        if (!block || stream.remaining() != 0) {
          LOG_F(ERROR, "Can't parse block %s (invalid block structure)", index->Header.GetHash().getHexLE().c_str());
          if (raw->FileNo != std::numeric_limits<uint32_t>::max())
            parseErrors++;
          entry.Valid = false;
          continue;
        }

        // Data owning its buffer alone (from the network) hands the bytes to the block object:
        // writeBlock needs them. Data inside a block file is on disk already
        void *serializedData = nullptr;
        size_t serializedMemorySize = 0;
        if (raw->Exclusive) {
          serializedMemorySize = raw->Buffer.get()->memorySize();
          serializedData = raw->Buffer.get()->detach();
        }

        object = intrusive_ptr<BC::Common::CIndexCacheObject>(
          new BC::Common::CIndexCacheObject(&storage.cache(), serializedData, raw->Size, serializedMemorySize, block, unpackedSize));
        index->FileNo = raw->FileNo;
        index->FileOffset = raw->FileOffset;
        index->SerializedBlockSize = raw->Size;
        entry.Relay = raw->Relay;
      } else {
        // Written to disk by an earlier connect and asked for again by a reorg
        object = objectByIndexChecked(index, storage.blockDb());
      }

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

  counters.RawBytes -= consumed.load();
  counters.FileParseErrors += parseErrors.load();
}

// Everything above the block the chain stops at is a descendant of it, so it goes with it
static void cutSegment(CSegment &segment, size_t keep, const char *reason)
{
  const size_t count = segment.Objects.size();
  LOG_F(ERROR,
        "Pull pipeline: block %s (%u) %s, %zu blocks above it dropped",
        segment.Objects[keep].Index->Header.GetHash().getHexLE().c_str(),
        segment.Objects[keep].Index->Height,
        reason,
        count - keep - 1);

  for (size_t i = keep; i < count; i++) {
    segment.Objects[i].Index->IndexState.store(BSInvalid);
    segment.Objects[i].Index->Prepared.store(false, std::memory_order_relaxed);
  }

  segment.Objects.resize(keep);
}

bool prepareSegment(BlockInMemoryIndex&,
                    BC::Common::ChainParams &chainParams,
                    BC::DB::Storage &storage,
                    CParallelRunner &runner,
                    CSegment &segment,
                    CPipelineCounters &counters,
                    bool prefetch)
{
  if (segment.Objects.empty())
    return false;

  checkSegmentWork(segment, chainParams, runner);
  prepareSegmentBlocks(chainParams, storage, segment, runner, counters);

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

  // The chain may stand elsewhere than where the selector cut: only disconnects are needed here,
  // the new path is the segment itself
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
    for (CSegment::CObject &entry: segment.Objects) {
      BC::Common::CIndexCacheObject *object = entry.Object.get();
      applyConnect(entry.Index, *object->block(), object->linkedOutputs(), object->validationData(),
                   blockIndex, storage, true);
    }
  }


  return result;
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
    BuildHeaderChain(blockIndex, chainParams, prevIndex);

  return index;
}


// BSEmpty (stub) and BSHeader are the states block data may take over
static bool reserveIndexForData(BC::Common::BlockIndex *index, bool *alreadyHaveHeader)
{
  BlockStatus state = index->IndexState.load(std::memory_order_relaxed);
  for (;;) {
    if (state != BSEmpty && state != BSHeader)
      return false;
    if (index->IndexState.compare_exchange_weak(state, BSData))
      break;
  }

  *alreadyHaveHeader = (state == BSHeader);
  return true;
}

BC::Common::BlockIndex *attachBlockData(BlockInMemoryIndex &blockIndex,
                                        BC::Common::ChainParams &chainParams,
                                        const BC::Proto::BlockHeader &header,
                                        const BC::Proto::BlockHashTy &hash,
                                        bool *checkWork)
{
  BC::Common::BlockIndex *index = nullptr;
  bool alreadyHaveHeader = false;

  {
    auto It = blockIndex.blockIndex().find(hash);
    if (It != blockIndex.blockIndex().end()) {
      // Header (headers-first path) or a stub for predecessor of a known block
      index = It->second;
      if (!reserveIndexForData(index, &alreadyHaveHeader))
        return nullptr;
    }
  }

  // Predecessor index; unlike AddBlock don't allocate a stub for a predecessor we know
  BC::Common::BlockIndex *prevIndex = nullptr;
  {
    auto It = blockIndex.blockIndex().find(header.hashPrevBlock);
    if (It != blockIndex.blockIndex().end()) {
      prevIndex = It->second;
    } else {
      prevIndex = BC::Common::BlockIndex::create(BSEmpty, nullptr);
      auto prevIt = blockIndex.blockIndex().insert(std::pair(header.hashPrevBlock, prevIndex));
      if (!prevIt.second) {
        delete prevIndex;
        prevIndex = prevIt.first->second;
      }
    }
  }

  // Try insert incoming block to index
  if (!index) {
    index = BC::Common::BlockIndex::create(BSData, prevIndex);
    auto It = blockIndex.blockIndex().insert(std::pair(hash, index));
    if (!It.second) {
      // Already have index for current block
      delete index;
      index = It.first->second;
      if (!reserveIndexForData(index, &alreadyHaveHeader))
        return nullptr;
      if (!alreadyHaveHeader) {
        index->Prev = prevIndex;
        index->Header = header;
      }
    } else {
      // New index created for current block; prev index already initialized
      index->Header = header;
    }
  } else if (!alreadyHaveHeader) {
    index->Prev = prevIndex;
    index->Header = header;
  }

  // Continue header chain if we see header first time
  if (!alreadyHaveHeader) {
    index->ConcurrentHeaderNext = WaitPtr<BC::Common::BlockIndex>();
    index->ConcurrentHeaderNext = prevIndex->SuccessorHeaders.exchange(index, 0);
    if (index->ConcurrentHeaderNext.tag() == 1)
      BuildHeaderChain(blockIndex, chainParams, prevIndex);
  }

  // A header that came through AddHeader has its consensus check done; one seen first time here
  // still owes it, and a worker pays it
  *checkWork = !alreadyHaveHeader;
  return index;
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

    // outlives the workers below, which keep the pointer while they run
    std::string pathUtf8 = pathToUtf8(path);

    size_t indexFileSize = std::filesystem::file_size(path);
    std::unique_ptr<uint8_t[]> data(new uint8_t[indexFileSize]);

    if (indexFileSize) {
      // Read index file
      std::unique_ptr<FILE, std::function<void(FILE*)>> hFile(fopen_path(path, "rb"), [](FILE *f) { fclose(f); });
      if (!hFile.get()) {
        LOG_F(ERROR, "Can't open index file %s", pathUtf8.c_str());
        return false;
      }

      if (fread(data.get(), indexFileSize, 1, hFile.get()) != 1) {
        LOG_F(ERROR, "Can't read index file %s", pathUtf8.c_str());
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
          LOG_F(ERROR, "Invalid index size %u detected in file %s", size, pathUtf8.c_str());
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
      workers[i] = std::async(std::launch::async, loadBlockIndexDeserializer, std::ref(blockIndex), std::ref(loadingIndexContext[i]), std::ref(blockFileSizes), &offsets[0] + off, size, pathUtf8.c_str());
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
    // Below the preparation frontier from the start: a walk down from a candidate stops at the
    // connected chain, not at the genesis block
    index->Prepared.store(true, std::memory_order_relaxed);
    index = index->Prev;
  }

  index->Prepared.store(true, std::memory_order_relaxed);
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
             BC::DB::Storage &storage,
             CBlockPipeline &pipeline)
{
  char blockFileName[64];
  unsigned blkFileIndex = 0;
  size_t totalBlockCount = 0;
  uint64_t totalBytesRead = 0;
  auto startTime = std::chrono::steady_clock::now();

  std::vector<BlockPosition> blockOffsets;

  for (;;) {
    snprintf(blockFileName, sizeof(blockFileName), "blk%05u.dat", blkFileIndex);
    std::filesystem::path path = blockPath / blockFileName;
    if (!std::filesystem::exists(path))
      break;

    // Read ahead limit: raw block data attached but not prepared yet lives in memory. Nothing
    // else here - the pipeline decides what to connect from the index, so a reader that waits
    // holds nobody up. The one case it would is a pipeline with nothing to do: the block it
    // needs is in a file not read yet, and only the reader can bring it
    {
      constexpr auto nap = std::chrono::milliseconds(1);
      while (pipeline.throttled())
        std::this_thread::sleep_for(nap);
    }

    std::string pathUtf8 = pathToUtf8(path);
    LOG_F(INFO, "Loading block file %s ...", pathUtf8.c_str());

    // Attached blocks point into this buffer: it lives until the last block of it is prepared
    size_t blockFileSize = std::filesystem::file_size(path);
    intrusive_ptr<CRawBlockData> buffer(new CRawBlockData(operator new(blockFileSize), blockFileSize, nullptr));
    uint8_t *data = static_cast<uint8_t*>(buffer.get()->data());

    {
      // Read block file
      std::unique_ptr<FILE, std::function<void(FILE*)>> hFile(fopen_path(path, "rb"), [](FILE *f) { fclose(f); });
      if (!hFile.get()) {
        LOG_F(ERROR, "Can't open block file %s", pathUtf8.c_str());
        return false;
      }

      if (fread(data, 1, blockFileSize, hFile.get()) != blockFileSize) {
        LOG_F(ERROR, "Can't read block file %s", pathUtf8.c_str());
        return false;
      }
    }

    {
      blockOffsets.clear();
      xmstream stream(data, blockFileSize);
      while (stream.remaining()) {
        size_t headerOffset = stream.offsetOf();
        if (data[headerOffset] == 0) {
          // Zero padding between records is legal; resync at the first non-zero byte
          size_t scan = headerOffset;
          while (scan < blockFileSize && data[scan] == 0)
            scan++;
          if (scan == blockFileSize)
            break;
          stream.seekSet(scan);
          continue;
        }

        uint32_t magic;
        uint32_t blockSize;
        BC::unserialize(stream, magic);
        BC::unserialize(stream, blockSize);

        if (magic != chainParams.magic) {
          LOG_F(ERROR, "Can't parse block file %s (invalid magic)", pathUtf8.c_str());
          return false;
        }

        BlockPosition data;
        data.offset = static_cast<uint32_t>(stream.offsetOf() - 8);
        data.size = blockSize;
        blockOffsets.push_back(data);
        stream.seek<uint8_t>(blockSize);
      }
    }

    for (const auto &position: blockOffsets) {
      // Serialized block starts right after the <magic>:4 <blockSize>:4 record prefix
      if (pipeline.attachFromFile(buffer, position.offset + 8, position.size, blkFileIndex, position.offset) == CBlockPipeline::Invalid) {
        LOG_F(ERROR, "Can't parse block file %s (invalid block structure)", pathUtf8.c_str());
        return false;
      }
    }

    if (pipeline.failed())
      return false;

    LOG_F(INFO,
          "%u blocks read from %s; read ahead: %.3lfM cache: %.3lfM best: [%u]%s",
          static_cast<unsigned>(blockOffsets.size()),
          pathUtf8.c_str(),
          pipeline.rawBytes() / 1048576.0f,
          storage.cache().size() / 1048576.0f,
          blockIndex.best()->Height,
          blockIndex.best()->Header.GetHash().getHexLE().c_str());

    pipeline.rotateFile();

    totalBlockCount += blockOffsets.size();
    totalBytesRead += blockFileSize;
    blkFileIndex++;
  }

  // Pipeline and write queue tails are part of reindex work: without draining them the speed
  // number would exclude a cache worth of pending writes
  pipeline.waitDrained();
  if (pipeline.failed())
    return false;

  while (!storage.queue().empty())
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  auto best = blockIndex.best();
  LOG_F(INFO, "%zu blocks loaded from disk", totalBlockCount);
  LOG_F(INFO, "Best block is %s (%u)", best->Header.GetHash().getHexLE().c_str(), best->Height);

  double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() / 1000.0;
  double megabytes = totalBytesRead / 1048576.0;
  LOG_F(INFO, "Reindex speed: %.2lf MB/s (%.1lf MB in %.1lf seconds)", elapsed > 0.0 ? megabytes / elapsed : 0.0, megabytes, elapsed);
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

    // The handler connects synchronously and the databases copy what they keep into their
    // shard logs, so the arena can go right after
    Handler_(Indexes_[i++], *block, linkedOutputs);
    operator delete(block);
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
