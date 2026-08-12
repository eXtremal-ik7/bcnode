// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// Where a store becomes a database of this chain. Everything below - eras,
// layers, shards, folds - takes the position as opaque bytes; here and only
// here those bytes are a block hash, resolved against the block index into
// what has to be connected and what rolled back.
//
// The rest of the class is forwarding: the lifetime calls of BaseInterface are
// answered by the store, and a chain-typed method cannot be overridden from a
// sibling base, so the bridges have to exist. Once, here, rather than in every
// database.

#include "db/common.h"
#include "dbengine/kvstore.h"

namespace BC {
namespace DB {

template<typename TStore>
class CChainDb : public TStore, public BaseInterface {
public:
  using TStore::TStore;

  bool initialize(BlockInMemoryIndex &blockIndex,
                  const std::filesystem::path &dbPath,
                  config4cpp::Configuration *cfg,
                  BC::Common::BlockIndex **forConnect,
                  std::vector<BC::Common::BlockIndex*> &forDisconnect) final {
    typename TStore::COpenResult opened;
    *forConnect = nullptr;
    if (!this->open(dbPath, cfg, opened))
      return false;

    // Nothing stored yet: the whole chain has to be walked
    if (opened.Fresh) {
      *forConnect = blockIndex.genesis();
      return true;
    }

    auto It = blockIndex.blockIndex().find(opened.Stamp);
    if (It == blockIndex.blockIndex().end()) {
      LOG_F(ERROR, "%s is corrupted: stamp %s not exists in block index",
            this->name().c_str(), opened.Stamp.getHexLE().c_str());
      return false;
    }

    // Build connect and disconnect block set if need
    BC::Common::BlockIndex *bestIndex = blockIndex.best();
    if (It->second != bestIndex)
      *forConnect = rebaseChain(bestIndex, It->second, forDisconnect);
    return true;
  }

  bool pipelineFull() const final { return TStore::pipelineFull(); }
  void flush() final { TStore::flush(); }
  void settle() final { TStore::settle(); }
  bool finishInitialBuild() final { return TStore::finishInitialBuild(); }
  const std::string &name() const final { return TStore::name(); }
};

}
}
