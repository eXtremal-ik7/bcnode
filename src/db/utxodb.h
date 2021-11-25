// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/blockDataBase.h"
#include "common/hashmap/spmc/hashmap.h"
#include "common/mstorage/dynamicBuffer.h"
#include "db/common.h"
#include <rocksdb/db.h>

namespace BC {
namespace DB {

class UTXODb {
public:
  ~UTXODb();
  bool initialize(BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb, BC::Common::BlockIndex **forConnect, IndexDbMap &forDisconnect);
  void add(BC::Common::BlockIndex *index, const BC::Proto::Block &block, ActionTy actionType, bool doFlush);
  // Not thread safe, search utxo in cache; if it not found - look disk storage
  bool query(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xmstream &data);
  // Thread safe, search utxo in cache only
  bool queryFast(const BC::Proto::BlockHashTy &txid, unsigned txoutIdx, xmstream &data);
  void flush();

private:
  struct RowData {
    BC::Proto::BlockHashTy Hash;
    size_t DataOffset = 0;
    size_t DataSize = 0;
  };

private:
  std::unique_ptr<rocksdb::DB> Database_;
  const BC::Common::BlockIndex *LastAdded_ = nullptr;
  xmstream Data_;
  std::vector<RowData> Queue_;


//  xmstream Data_;
//  std::vector<TxData> Queue_;
//  tbb::concurrent_hash_map<BC::Proto::BlockHashTy, TxLink, TbbHash<256>> Cache_;
};

}
}
