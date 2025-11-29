#pragma once

#include "common/blockDataBase.h"

namespace BC {
namespace DB {

class BaseInterface;

struct BaseWithBest {
  BaseInterface *Base;
  BC::Common::BlockIndex *BestBlock;
  BaseWithBest() {}
  BaseWithBest(BaseInterface *base, BC::Common::BlockIndex *block) : Base(base), BestBlock(block) {}
};

bool dbDisconnectBlocks(BC::DB::BaseInterface &db,
                        BlockInMemoryIndex &blockIndex,
                        BC::DB::Storage &storage,
                        std::vector<BC::Common::BlockIndex *> &forDisconnect);

bool dbConnectBlocks(BC::DB::UTXODb &utxoDb,
                     BC::Common::BlockIndex *utxoBestBlock,
                     std::vector<BaseWithBest> archiveDatabases,
                     BlockInMemoryIndex &blockIndex,
                     BC::DB::Storage &storage,
                     const char *name);

}
}
