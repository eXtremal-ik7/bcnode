#pragma once

#include "common/blockDataBase.h"

namespace BC {
namespace DB {

class BaseInterface;
class Archive;

struct BaseWithBest {
  BaseInterface *Base;
  BC::Common::BlockIndex *BestBlock;
  BaseWithBest() {}
  BaseWithBest(BaseInterface *base, BC::Common::BlockIndex *block) : Base(base), BestBlock(block) {}
};

bool dbDisconnectBlocks(BC::DB::BaseInterface &db,
                        BlockInMemoryIndex &blockIndex,
                        BC::Common::ChainParams &chainParams,
                        BC::DB::Storage &storage,
                        std::vector<BC::Common::BlockIndex *> &forDisconnect);

// archive may be null - the utxo-only path has no archive at all. When it is
// set, the archive databases are fed through its connect workers and this
// thread keeps a batch ahead of them
bool dbConnectBlocks(BC::DB::UTXODb &utxoDb,
                     BC::Common::BlockIndex *utxoBestBlock,
                     std::vector<BaseWithBest> archiveDatabases,
                     BC::DB::Archive *archive,
                     BlockInMemoryIndex &blockIndex,
                     BC::Common::ChainParams &chainParams,
                     BC::DB::Storage &storage,
                     size_t batchSizeLimit,
                     const char *name);

}
}
