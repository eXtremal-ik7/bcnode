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

// Feeds the databases the blocks they missed. The chain is settled and holds all of them, so
// nothing is selected, validated or written back: the walk starts at the lowest block anyone
// still needs, the pipeline prepares the batches and returns them in order, and they go straight
// to the archive. Databases wake up at different heights and each takes its own tail of a batch
// (Archive::setConnectFrom); archive may be null - the utxo-only path has none
bool dbConnectBlocks(BC::DB::UTXODb &utxoDb,
                     BC::Common::BlockIndex *utxoBestBlock,
                     std::vector<BaseWithBest> archiveDatabases,
                     BC::DB::Archive *archive,
                     BlockInMemoryIndex &blockIndex,
                     BC::DB::Storage &storage,
                     CBlockPipeline &pipeline,
                     const CBlockPipeline::CParams &params,
                     const char *name);

}
}
