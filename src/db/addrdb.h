// Copyright (c) 2026 Ivan K.
// Copyright (c) 2026 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"

namespace BC {
namespace DB {

class AddrDb :
  public CBaseMerge<BC::Script::CAddress, CAddrValue>,
  public IAddrDb {

public:
  AddrDb() : CBaseMerge<BC::Script::CAddress, CAddrValue>("addrdb") {
    registerIndex("balance", [](const CAddrValue &value) -> uint64_t { return value.Received - value.Sent; });
    registerIndex("tx_count", [](const CAddrValue &value) -> uint64_t { return value.TxCount; });
  }
  virtual ~AddrDb() {}

  void *interface(int interface) {
    switch (interface) {
      case EIQueryAddr : return static_cast<IAddrDb*>(this);
      default: return nullptr;
    }
  }

  bool queryAddr(const BC::Script::CAddress &address, CAddrValue &result) final;
  bool queryTop(const std::string &index, size_t offset, size_t limit,
                std::vector<std::pair<BC::Script::CAddress, CAddrValue>> &result) final;

  uint32_t version() final { return 1; }

  void connectImpl(const BC::Common::BlockIndex *index,
                   const BC::Proto::Block &block,
                   const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                   BlockInMemoryIndex &blockIndex,
                   BlockDatabase &blockDb);

  void disconnectImpl(const BC::Common::BlockIndex *index,
                      const BC::Proto::Block &block,
                      const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                      BlockInMemoryIndex &blockIndex,
                      BlockDatabase &blockDb);
};

}
}
