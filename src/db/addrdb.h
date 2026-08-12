// Copyright (c) 2026 Ivan K.
// Copyright (c) 2026 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "db/queries.h"
#include "db/chaindb.h"
#include "dbengine/kvmerge.h"

namespace BC {
namespace DB {

class AddrDb :
  public CChainDb<dbengine::CKvMergeBase<BC::Script::CAddress, CAddrValue>>,
  public IAddrDb {

public:
  AddrDb() : CChainDb<dbengine::CKvMergeBase<BC::Script::CAddress, CAddrValue>>("addrdb") {
    // The metric width is the column's own - per-coin for the balance. New
    // indexes register at the end: the order is the stored id, and shifting
    // it degenerates the xcfg diff to drop-all+rebuild
    registerIndex("balance", sizeof(BC::Proto::BalanceType),
                  [](const CAddrValue &value) -> UInt<128> { return value.Received - value.Sent; });
    registerIndex("tx_count", sizeof(uint64_t),
                  [](const CAddrValue &value) -> UInt<128> { return UInt<128>(value.TxCount); });
    registerIndex("total_received", sizeof(UInt<128>),
                  [](const CAddrValue &value) -> UInt<128> { return value.Received; });
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

  void connect(CBlockBatch batch,
               BlockInMemoryIndex &blockIndex,
               BlockDatabase &blockDb) final;

  void disconnect(const BC::Common::BlockIndex *index,
                  const BC::Proto::Block &block,
                  const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                  const BC::Proto::CBlockValidationData &validationData,
                  BlockInMemoryIndex &blockIndex,
                  BlockDatabase &blockDb) final;
};

}
}
