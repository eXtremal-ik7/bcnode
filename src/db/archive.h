// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "balancedb.h"
#include "txdb.h"

class BlockDatabase;

namespace BC {
namespace DB {

class Archive {
public:
  bool init(BlockInMemoryIndex &blockIndex,
            BlockDatabase &blockDb,
            BC::Common::BlockIndex **connectPoints,
            BC::DB::IndexDbMap &disconnectQueue) {
    if (!TxDb_.initialize(blockIndex, blockDb, *this, &connectPoints[BC::DB::DbTransactions], disconnectQueue) ||
        !BalanceDb_.initialize(blockIndex, blockDb, *this, &connectPoints[BC::DB::DbAddrBalance], disconnectQueue))
      return false;
    return true;
  }

  bool sync(BlockInMemoryIndex &blockIndex,
            BlockDatabase &blockDb,
            BC::Common::BlockIndex **connectPoints,
            BC::DB::IndexDbMap &disconnectQueue);

  void add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy action) {
    if (TxDb_.enabled())
      TxDb_.add(index, block, action);
    if (BalanceDb_.enabled())
      BalanceDb_.add(index, block, action);
  }

  void flush() {
    if (TxDb_.enabled())
      TxDb_.flush();
    if (BalanceDb_.enabled())
      BalanceDb_.flush();
  }

  BC::DB::BalanceDb &balancedb() { return BalanceDb_; }
  BC::DB::TxDb &txdb() { return TxDb_; }

private:
  BC::DB::BalanceDb BalanceDb_;
  BC::DB::TxDb TxDb_;
};

}
}
