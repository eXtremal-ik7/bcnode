// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "db/common.h"
#include "db/kvarray.h"

#include "thirdparty/ankerl/unordered_dense.h"

namespace BC {
namespace DB {

class AddrHistoryDb :
  public CKvArrayBase<BC::Script::CAddress, CAddrHistoryItem>,
  public IAddrHistoryDb {

public:
  AddrHistoryDb() : CKvArrayBase<BC::Script::CAddress, CAddrHistoryItem>("addrhistorydb", 64) {}
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
                   CKvWriter<BC::Script::CAddress> &writer,
                   BlockInMemoryIndex &blockIndex,
                   BlockDatabase &blockDb);

  void disconnectImpl(const BC::Common::BlockIndex *index,
                      const BC::Proto::Block &block,
                      const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                      const BC::Proto::CBlockValidationData &validationData,
                      CKvWriter<BC::Script::CAddress> &writer,
                      BlockInMemoryIndex &blockIndex,
                      BlockDatabase &blockDb);

private:
  struct CTouch {
    uint32_t KeyId;
    CAddrHistoryItem Item;
  };

  // Net balance delta of one transaction per affected address
  struct CTxTouch {
    uint32_t KeyId;
    BC::Proto::BalanceType Delta;
  };

  // The scratch of one connect, kept between calls: the database is mutated by
  // one thread, so clearing beats rebuilding - the capacity and the pages of
  // the previous batch stay, and a batch of a segment is millions of touches
  ankerl::unordered_dense::map<BC::Script::CAddress, uint32_t> KeyIds_;
  std::vector<BC::Script::CAddress> KeyById_;
  std::vector<uint32_t> Counts_;
  std::vector<CTouch> Touches_;
  std::vector<uint64_t> TouchEpoch_;  // per id: serial of the tx that touched it last
  std::vector<uint32_t> TxSlot_;      // per id: the touch's slot within that tx
  std::vector<CTxTouch> TxTouches_;   // current tx, insertion order
  std::vector<CTailWriter> Cursors_;
  // Never restarts: an epoch of a fresh id is zero, so any live serial differs
  uint64_t TxSerial_ = 0;
};

}
}
