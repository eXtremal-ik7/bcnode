// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "txdb.h"

namespace BC {
namespace DB {

class Archive {
public:
  bool init(BlockInMemoryIndex &blockIndex,
            std::filesystem::path &dataDir,
            BC::Common::BlockIndex **connectPoints,
            BC::DB::IndexDbMap &disconnectQueue) {
    if (!TxDb_.initialize(blockIndex, dataDir, &connectPoints[BC::DB::DbTransactions], disconnectQueue))
      return false;
    return true;
  }

  bool sync(BlockInMemoryIndex &blockIndex,
            BC::Common::ChainParams &chainParams,
            BlockDatabase &blockDb,
            BC::Common::BlockIndex **connectPoints,
            BC::DB::IndexDbMap &disconnectQueue);

  void add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy action) {
//    const SerializedDataObject *serialized = index->Serialized.get();
//    const BC::Proto::Block *block = static_cast<BC::Proto::Block*>(serialized->unpackedData());

    if (TxDb_.enabled())
      TxDb_.add(index, block, action);
  }

  void flush() {
    if (TxDb_.enabled())
      TxDb_.flush();
  }

  BC::DB::TxDb &txdb() { return TxDb_; }

private:
  BC::DB::TxDb TxDb_;
};

}
}
