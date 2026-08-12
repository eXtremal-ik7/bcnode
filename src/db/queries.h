// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// What the databases of this chain answer, and what they answer it with. A
// menu and not a base class: a database picks the interfaces it serves and
// hands them out through BaseInterface::interface(), so a caller that only
// reads never sees the connect side - and a database that only writes never
// sees this file

#include "common/blockDataBase.h"
#include "common/uint.h"

#include <string>
#include <utility>
#include <vector>

namespace BC {
namespace DB {

// Interfaces
enum EInterfaceTy {
  EIQueryTransaction = 0,
  EIQueryAddrHistory,
  EIQueryAddr,
  EIQuerySpent
};

struct CQueryTransactionResult {
  BC::Proto::Transaction Tx;
  BC::Proto::CTxLinkedOutputs LinkedOutputs;
  BC::Proto::BlockHashTy Block;
  uint32_t TxNum;
  bool Found = false;
  bool DataCorrupted = false;
};

// Reads what a stored position points at: the transaction out of the block still
// held in memory, or exactly its bytes out of the block file, plus the linked
// outputs of that block. Everything the databases keep besides the position is
// in the block index already, so this is the whole read path behind a position
bool readTransactionAt(BC::Common::BlockIndex *index,
                       uint32_t txIndex,
                       uint32_t txOffset,
                       uint32_t txSize,
                       BlockDatabase &blockDb,
                       CQueryTransactionResult &result);

// History element of addrhistorydb: where the transaction lies plus the running
// balance maintained by CKvArrayBase - the address balance right after this tx.
// A position and not a txid: the reader takes the transaction straight out of
// the block file, so the transaction database drops out of this path entirely,
// and the txid is recomputed for the reply anyway (serializeTx). TxIndex stays
// because the linked outputs of a block are one blob indexed by it
#pragma pack(push, 1)
struct CAddrHistoryItem {
  uint32_t Height;
  uint32_t TxIndex;
  uint32_t TxOffset;   // from the start of the serialized block
  uint32_t TxSize;
  BC::Proto::BalanceType Aggregate;
};
#pragma pack(pop)

struct CQueryAddrHistory {
  std::vector<CAddrHistoryItem> Items;
  size_t TotalTxCount;
};

class ITransactionDb {
public:
  virtual bool queryTransaction(const BC::Proto::TxHashTy &txid,
                                BlockInMemoryIndex &blockIndex,
                                BlockDatabase &blockDb,
                                CQueryTransactionResult &result) = 0;
};

class IAddrHistoryDb {
public:
  virtual bool queryAddrHistory(const BC::Script::CAddress &address, size_t from, size_t count, CQueryAddrHistory &result) = 0;
};

// Cumulative per-address counters; also serves as the in-memory delta
// (all fields are additive, negative deltas use two's complement wrap).
// Received/Sent are 128-bit - lifetime turnover is not bounded by the supply,
// a busy DOGE wallet passes 2^64 satoshi within a few years. Mined counts
// created coins only, so it is supply-bounded and takes the per-coin width
#pragma pack(push, 1)
struct CAddrValue {
  UInt<128> Received;
  UInt<128> Sent;
  BC::Proto::BalanceType Mined{};
  // The chain-wide transaction count outgrows uint32 in the foreseeable
  // future, and the input/output counters run ahead of it
  uint64_t TxCount = 0;
  uint64_t TxInCount = 0;
  uint64_t TxOutCount = 0;
  uint64_t MinedTxCount = 0;

  void merge(const CAddrValue &delta) {
    Received += delta.Received;
    Sent += delta.Sent;
    Mined += delta.Mined;
    TxCount += delta.TxCount;
    TxInCount += delta.TxInCount;
    TxOutCount += delta.TxOutCount;
    MinedTxCount += delta.MinedTxCount;
  }

  void negate() {
    Received = -Received;
    Sent = -Sent;
    Mined = -Mined;
    TxCount = 0 - TxCount;
    TxInCount = 0 - TxInCount;
    TxOutCount = 0 - TxOutCount;
    MinedTxCount = 0 - MinedTxCount;
  }

  bool isNull() const {
    return Received.isZero() && Sent.isZero() && Mined == 0u &&
           TxCount == 0 && TxInCount == 0 && TxOutCount == 0 && MinedTxCount == 0;
  }
};
#pragma pack(pop)

class IAddrDb {
public:
  virtual bool queryAddr(const BC::Script::CAddress &address, CAddrValue &result) = 0;
  virtual bool queryTop(const std::string &index, size_t offset, size_t limit,
                        std::vector<std::pair<BC::Script::CAddress, CAddrValue>> &result) = 0;
};

// What spentdb stores per spent outpoint: the input that took it. The height
// bounds a walk down the spend graph without reading the spending transaction
#pragma pack(push, 1)
struct CSpentValue {
  BC::Proto::TxHashTy SpentBy;  // txid of the spending transaction
  uint32_t InputIndex;          // its input taking this output
  uint32_t Height;              // height of the block holding the spender
};
#pragma pack(pop)

static_assert(sizeof(CSpentValue) == sizeof(BC::Proto::TxHashTy) + 2 * sizeof(uint32_t),
              "unexpected padding in CSpentValue");

struct CQuerySpentResult {
  CSpentValue Value;
  bool Found = false;
};

class ISpentDb {
public:
  virtual bool querySpent(const BC::Proto::TxHashTy &txid, uint32_t index, CQuerySpentResult &result) = 0;
  // Every output of one transaction, result sized to count: what a transaction
  // page needs, one call instead of a lookup per output
  virtual bool querySpentOutputs(const BC::Proto::TxHashTy &txid, uint32_t count, std::vector<CQuerySpentResult> &result) = 0;
};

}
}
