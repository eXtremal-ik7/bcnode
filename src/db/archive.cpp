// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "archive.h"

namespace BC {
namespace DB {

bool Archive::sync(BlockInMemoryIndex &blockIndex,
                   BC::Common::ChainParams &chainParams,
                   BlockDatabase &blockDb,
                   BC::Common::BlockIndex **connectPoints,
                   BC::DB::IndexDbMap &disconnectQueue)
{
  uint32_t forConnectHeights[DbCount];
  BC::Common::BlockIndex *firstCommon = nullptr;
  for (unsigned i = 0; i < BC::DB::DbCount; i++) {
    if (connectPoints[i] && (!firstCommon || connectPoints[i]->Height < firstCommon->Height))
      firstCommon = connectPoints[i];
    forConnectHeights[i] = connectPoints[i] ? connectPoints[i]->Height : std::numeric_limits<uint32_t>::max();
  }

  // disconnect blocks
  {
    bool noError = true;
    auto It = disconnectQueue.begin();
    auto handler = [this, &It](void *data, size_t size) {
      xmstream stream(data, size);
      BC::Proto::Block block;
      BC::unserialize(stream, block);
      BitMap bitMap = It->second;
      if (bitMap.Affected[DbTransactions] && TxDb_.enabled())
        TxDb_.add(It->first, block, Disconnect, true);
      if (bitMap.Affected[DbAddrBalance] && BalanceDb_.enabled())
        BalanceDb_.add(It->first, block, Disconnect, true);
      ++It;
    };

    BlockSearcher searcher(blockDb, handler, [&noError]() { noError = false; });
    for (const auto &element: disconnectQueue) {
      searcher.add(element.first);
      if (!noError)
        return false;
    }
  }

  // connect blocks
  if (firstCommon) {
    bool noError = true;
    BC::Common::BlockIndex *best = blockIndex.best();
    uint32_t count = best->Height - firstCommon->Height + 1;
    LOG_F(INFO, "Update databases: connecting %u blocks", count);

    BC::Common::BlockIndex *indexIt = firstCommon;
    auto handler = [this, &forConnectHeights, &indexIt](void *data, size_t size) {
      xmstream stream(data, size);
      BC::Proto::Block block;
      BC::unserialize(stream, block);
      if (indexIt->Height >= forConnectHeights[DbTransactions] && TxDb_.enabled())
        TxDb_.add(indexIt, block, Connect, true);
      if (indexIt->Height >= forConnectHeights[DbAddrBalance] && BalanceDb_.enabled())
        BalanceDb_.add(indexIt, block, Connect, true);
      indexIt = indexIt->Next;
    };

    BC::Common::BlockIndex *index = firstCommon;
    BlockSearcher searcher(blockDb, handler, [&noError]() { noError = false; });
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
  }

  if (TxDb_.enabled())
    TxDb_.flush();

  LOG_F(INFO, "100%% done");
  return true;
}

}
}
