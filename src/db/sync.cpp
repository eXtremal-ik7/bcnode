#include "sync.h"
#include "storage.h"

namespace BC {
namespace DB {

bool dbDisconnectBlocks(BC::DB::BaseInterface &db,
                        BlockInMemoryIndex &blockIndex,
                        BC::DB::Storage &storage,
                        std::vector<BC::Common::BlockIndex*> &forDisconnect)
{
  for (BC::Common::BlockIndex *index: forDisconnect) {
    auto object = objectByIndex(index, storage.blockDb());
    if (!object.get())
      return false;
    db.disconnect(index, *object.get()->block(), object.get()->linkedOutputs(), blockIndex, storage.blockDb());
  }

  return true;


  // bool noError = true;
  // auto It = forDisconnect.begin();
  // auto handler = [&db, &It, &blockIndex, &storage](void *data, size_t size) {
  //   xmstream stream(data, size);
  //   BC::Proto::Block block;
  //   BC::unserialize(stream, block);
  //   // TODO: load linked outputs from disk
  //   db.disconnect(*It, block, BC::Proto::CBlockLinkedOutputs(), blockIndex, storage.blockDb());
  //   ++It;
  // };

  // BlockSearcher searcher(storage.blockDb(), handler, [&noError]() { noError = false; });
  // for (const auto &element: forDisconnect) {
  //   searcher.add(element);
  //   if (!noError)
  //     return false;
  // }

  // return true;
}

bool dbConnectBlocks(BC::DB::UTXODb &utxoDb,
                     BC::Common::BlockIndex *utxoBestBlock,
                     std::vector<BaseWithBest> archiveDatabases,
                     BlockInMemoryIndex &blockIndex,
                     BC::DB::Storage &storage,
                     const char *name)
{
  BC::Common::BlockIndex *firstCommon = utxoBestBlock;
  for (size_t i = 0; i < archiveDatabases.size(); i++) {
    BC::Common::BlockIndex *best = archiveDatabases[i].BestBlock;
    if (best->Height < firstCommon->Height)
      firstCommon = best;
  }

  firstCommon = firstCommon->Next;
  if (!firstCommon) {
    LOG_F(INFO, "%s is up to date", name);
    return true;
  }

  bool noError = true;
  BC::Common::BlockIndex *best = blockIndex.best();
  uint32_t count = best->Height - firstCommon->Height;
  LOG_F(INFO, "Update %s: connecting %u blocks", name, count);

  auto handler = [&utxoDb, utxoBestBlock, &archiveDatabases, &blockIndex, &storage](BC::Common::BlockIndex *index, const BC::Proto::Block &block, const BC::Proto::CBlockLinkedOutputs &linkedOutputs) {
    // Connect archive
    for (size_t i = 0; i < archiveDatabases.size(); i++) {
      BC::Common::BlockIndex *best = archiveDatabases[i].BestBlock;
      uint32_t connectHeight = best ? best->Height : std::numeric_limits<uint32_t>::max();
      if (index->Height >= connectHeight)
        archiveDatabases[i].Base->connect(index, block, linkedOutputs, blockIndex, storage.blockDb());
    }

    // Connect utxo
    if (index->Height >= utxoBestBlock->Height)
      utxoDb.connect(index, block, linkedOutputs, blockIndex, storage.blockDb());
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
