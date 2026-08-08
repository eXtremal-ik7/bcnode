#include "sync.h"
#include "archive.h"
#include "storage.h"
#include <chrono>
#include <thread>

namespace BC {
namespace DB {

bool dbDisconnectBlocks(BC::DB::BaseInterface &db,
                        BlockInMemoryIndex &blockIndex,
                        BC::Common::ChainParams &chainParams,
                        BC::DB::Storage &storage,
                        std::vector<BC::Common::BlockIndex*> &forDisconnect)
{
  for (BC::Common::BlockIndex *index: forDisconnect) {
    // attach cannot refuse: hold the walk while the engine is over its
    // admission limit, the flusher drains it on its own
    while (db.pipelineFull())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto object = objectByIndex(index, chainParams, storage.blockDb());
    if (!object.get())
      return false;
    db.disconnect(index, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationDataConst(), blockIndex, storage.blockDb());
  }

  return true;
}

// One batch on its way to the databases, with everything its refs point at.
// Two of these alternate: while the archive workers chew one, the reader fills
// the other, and a slot is refilled only after its previous batch is done
struct CFeedSlot {
  std::vector<BlockBulkReader::CBulkBlock> Blocks;
  std::vector<BC::Proto::CBlockValidationData> ValidationData;
  std::vector<CBlockRef> Refs;
  BC::DB::Archive::CConnectTask Task;
};

// A database wakes up at its own height; heights inside a batch are contiguous
static size_t tailFrom(uint32_t connectHeight, uint32_t firstHeight)
{
  return connectHeight > firstHeight ? connectHeight - firstHeight : 0;
}

bool dbConnectBlocks(BC::DB::UTXODb &utxoDb,
                     BC::Common::BlockIndex *utxoBestBlock,
                     std::vector<BaseWithBest> archiveDatabases,
                     BC::DB::Archive *archive,
                     BlockInMemoryIndex &blockIndex,
                     BC::Common::ChainParams &chainParams,
                     BC::DB::Storage &storage,
                     size_t batchSizeLimit,
                     const char *name)
{
  uint32_t utxoBestHeight = utxoBestBlock ? utxoBestBlock->Height : std::numeric_limits<uint32_t>::max();

  BC::Common::BlockIndex *firstCommon = utxoBestBlock;
  uint32_t firstCommonHeight = utxoBestHeight;

  for (size_t i = 0; i < archiveDatabases.size(); i++) {
    BC::Common::BlockIndex *best = archiveDatabases[i].BestBlock;
    if (best && best->Height < firstCommonHeight) {
      firstCommon = best;
      firstCommonHeight = firstCommon->Height;
    }
  }

  if (!firstCommon) {
    LOG_F(INFO, "%s is up to date", name);
    return true;
  }

  bool noError = true;
  BC::Common::BlockIndex *best = blockIndex.best();
  // firstCommon is the first block to connect, not the one already applied
  uint32_t count = best->Height - firstCommon->Height + 1;
  LOG_F(INFO, "Update %s: connecting %u blocks", name, count);

  CFeedSlot slots[2];
  size_t slotIndex = 0;
  uint32_t fed = 0;
  unsigned portionNum = 0;
  unsigned portionSize = count / 20 + 1;

  auto handler = [&](std::vector<BlockBulkReader::CBulkBlock> &batchBlocks) {
    CFeedSlot &slot = slots[slotIndex];
    slotIndex ^= 1;

    // The slot's previous batch has to be out of every database before its
    // storage is reused: the refs of that batch point straight into it
    if (archive)
      archive->wait(slot.Task);

    slot.Blocks = std::move(batchBlocks);
    slot.ValidationData.clear();
    slot.ValidationData.resize(slot.Blocks.size());
    slot.Refs.clear();
    slot.Refs.reserve(slot.Blocks.size());

    for (size_t i = 0; i < slot.Blocks.size(); i++) {
      // The bulk reader hands out raw disk data; rebuild the validation context
      // to keep the connect invariant (txids and consensus exemptions precomputed on
      // every path). No contextual check runs here - a database catching up connects
      // blocks the chain accepted long ago
      const BC::Proto::Block &block = slot.Blocks[i].block();
      BC::Proto::CBlockValidationData &validationData = slot.ValidationData[i];
      BC::Common::initializeValidationContext(block, validationData);
      BTC::Common::fillBIP30Context(*slot.Blocks[i].Index, chainParams, validationData);
      validationData.InputsResolved = true;
      slot.Refs.push_back(CBlockRef{slot.Blocks[i].Index, &block,
                                    &slot.Blocks[i].linkedOutputs(), &validationData});
    }

    // attach cannot refuse: hold the bulk reader while any engine is over its
    // admission limit, the flushers drain them on their own
    for (;;) {
      bool full = utxoDb.pipelineFull();
      for (const auto &db: archiveDatabases)
        full |= db.Base->pipelineFull();
      if (!full)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const uint32_t firstHeight = slot.Refs.front().Index->Height;

    // Archive first and without waiting: its workers run while this thread
    // takes the utxo below and then reads the next batch
    if (archive) {
      slot.Task.Batch = slot.Refs;
      slot.Task.BlockIndex = &blockIndex;
      slot.Task.BlockDb = &storage.blockDb();
      slot.Task.FirstHeight = firstHeight;
      archive->submit(slot.Task);
    }

    // utxo is not one of the archive databases, so this thread owns it
    const size_t utxoSkip = tailFrom(utxoBestHeight, firstHeight);
    if (utxoSkip < slot.Refs.size())
      utxoDb.connect(CBlockBatch(slot.Refs).subspan(utxoSkip), blockIndex, storage.blockDb());

    fed += static_cast<uint32_t>(slot.Blocks.size());
    while (portionNum < 20 && fed >= (portionNum + 1) * portionSize) {
      portionNum++;
      LOG_F(INFO, "%u%% done", portionNum*5);
    }
  };

  {
    BlockBulkReader searcher(storage.blockDb(), batchSizeLimit, 262144,
                             handler, [&noError]() { noError = false; });
    for (BC::Common::BlockIndex *index = firstCommon; index; index = index->Next) {
      searcher.add(index);
      if (!noError)
        break;
    }
  }

  // Nothing may reach the databases from another thread while a batch is still
  // inside them: the caller goes on to flush and stamp
  if (archive) {
    archive->wait(slots[0].Task);
    archive->wait(slots[1].Task);
  }

  if (!noError)
    return false;

  LOG_F(INFO, "100%% done");
  return true;
}

}
}
