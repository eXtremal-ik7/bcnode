#include "sync.h"
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

bool dbConnectBlocks(BC::DB::UTXODb &utxoDb,
                     BC::Common::BlockIndex *utxoBestBlock,
                     std::vector<BaseWithBest> archiveDatabases,
                     BlockInMemoryIndex &blockIndex,
                     BC::Common::ChainParams &chainParams,
                     BC::DB::Storage &storage,
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
  uint32_t count = best->Height - firstCommon->Height;
  LOG_F(INFO, "Update %s: connecting %u blocks", name, count);

  auto handler = [&utxoDb, utxoBestHeight, &archiveDatabases, &blockIndex, &chainParams, &storage](BC::Common::BlockIndex *index, const BC::Proto::Block &block, const BC::Proto::CBlockLinkedOutputs &linkedOutputs) {
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

    // The bulk reader hands out raw disk data; rebuild the validation context
    // to keep the connect invariant (txids and consensus exemptions precomputed on
    // every path). No contextual check runs here - a database catching up connects
    // blocks the chain accepted long ago
    BC::Proto::CBlockValidationData validationData;
    BC::Common::initializeValidationContext(block, validationData);
    BTC::Common::fillBIP30Context(*index, chainParams, validationData);
    validationData.InputsResolved = true;

    // A batch of one: the bulk reader hands out block by block, and the validation
    // context above lives no longer than this call
    CBlockRef ref{index, &block, &linkedOutputs, &validationData};
    CBlockBatch batch(&ref, 1);

    // Connect archive
    for (size_t i = 0; i < archiveDatabases.size(); i++) {
      BC::Common::BlockIndex *best = archiveDatabases[i].BestBlock;
      uint32_t connectHeight = best ? best->Height : std::numeric_limits<uint32_t>::max();
      if (index->Height >= connectHeight)
        archiveDatabases[i].Base->connect(batch, blockIndex, storage.blockDb());
    }

    // Connect utxo
    if (index->Height >= utxoBestHeight)
      utxoDb.connect(batch, blockIndex, storage.blockDb());
  };

  BC::Common::BlockIndex *index = firstCommon;
  BlockBulkReader searcher(storage.blockDb(), handler, [&noError]() { noError = false; });
  unsigned portionNum = 0;
  unsigned portionSize = count / 20 + 1;
  unsigned i = 0;
  while (index) {
    searcher.add(index);
    if (!noError)
      return false;
    index = index->Next;
    if (++i == portionSize) {
      portionNum++;
      LOG_F(INFO, "%u%% done", portionNum*5);
      i = 0;
    }
  }

  LOG_F(INFO, "100%% done");
  return true;
}

}
}
