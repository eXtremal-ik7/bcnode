// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "db/sync.h"

class BlockDatabase;

struct aioObject;

namespace BC {
namespace DB {

class Archive {
public:      
  bool init(BlockInMemoryIndex &blockIndex,
            BC::DB::Storage &storage,
            config4cpp::Configuration *cfg);

  bool purge(config4cpp::Configuration *cfg, std::filesystem::path &dataDir);

  void connect(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) {
    for (auto &db: AllDb_)
      db->connect(index, block, blockIndex, blockDb);
  }

  void disconnect(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) {
    for (auto &db: AllDb_)
      db->disconnect(index, block, blockIndex, blockDb);
  }

  void flush() {
    for (auto &db: AllDb_)
      db->flush();
  }

private:
  std::vector<std::unique_ptr<BC::DB::BaseInterface>> AllDb_;

public:
  // Handlers
  ITransactionDb *TransactionDb_ = nullptr;
  IAddrHistoryDb *AddrHistoryDb_ = nullptr;
};

}
}
