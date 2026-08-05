// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"

namespace BC {
namespace DB {

class AddrHistoryDb :
  public CBaseArrayAggregated<BC::Script::CAddress, CAddrHistoryItem>,
  public IAddrHistoryDb {

public:
  AddrHistoryDb() : CBaseArrayAggregated<BC::Script::CAddress, CAddrHistoryItem>("addrhistorydb", 64) {}
  virtual ~AddrHistoryDb() {}

  void *interface(int interface) {
    switch (interface) {
      case EIQueryAddrHistory : return static_cast<IAddrHistoryDb*>(this);
      default: return nullptr;
    }
  }

  bool queryAddrHistory(const BC::Script::CAddress &address, size_t from, size_t count, CQueryAddrHistory &result) final;

  uint32_t version() final { return 1; }
  bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage);

  void connectImpl(CBlockBatch batch,
                   BlockInMemoryIndex &blockIndex,
                   BlockDatabase &blockDb);

  void disconnectImpl(const BC::Common::BlockIndex *index,
                      const BC::Proto::Block &block,
                      const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                      const BC::Proto::CBlockValidationData &validationData,
                      BlockInMemoryIndex &blockIndex,
                      BlockDatabase &blockDb);
};

}
}
