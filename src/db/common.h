// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/blockDataBase.h"

#include "config4cpp/Configuration.h"
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace BC {
namespace DB {

#pragma pack(push, 1)
struct CBaseCfg {
  uint32_t Version;
  uint32_t ShardsNum;
};
#pragma pack(pop)

// One block as a database sees it: the index and the three parsed pieces the
// walk reads. Pointers, because these live in an array. The index is not const
// only because the storage queue writes back where the block landed on disk -
// a database reads it and nothing more
struct CBlockRef {
  BC::Common::BlockIndex *Index = nullptr;
  const BC::Proto::Block *Block = nullptr;
  const BC::Proto::CBlockLinkedOutputs *LinkedOutputs = nullptr;
  const BC::Proto::CBlockValidationData *ValidationData = nullptr;
};

// The unit of a connect: blocks in chain order, applied as one operation, so
// the database ends up at the last of them. A block connected on its own is a
// batch of one. A disconnect has no such unit and takes a single block: it
// walks down a fork of unknown depth, a block at a time, off the hot path
using CBlockBatch = std::span<const CBlockRef>;

// What a database is to the chain: open it, tell it where the chain went, ask
// it to write. How it holds the window in between - published views, see
// kvbase.h - is none of the caller's business
class BaseInterface {
public:
  virtual ~BaseInterface() {}

  virtual bool initialize(BlockInMemoryIndex &blockIndex,
                          const std::filesystem::path &dbPath,
                          BC::DB::Storage &storage,
                          config4cpp::Configuration *cfg,
                          BC::Common::BlockIndex **forConnect,
                          std::vector<BC::Common::BlockIndex*> &forDisconnect) = 0;

  virtual void connect(CBlockBatch batch,
                       BlockInMemoryIndex &blockIndex,
                       BlockDatabase &blockDb) = 0;

  virtual void disconnect(const BC::Common::BlockIndex *index,
                          const BC::Proto::Block &block,
                          const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                          const BC::Proto::CBlockValidationData &validationData,
                          BlockInMemoryIndex &blockIndex,
                          BlockDatabase &blockDb) = 0;

  virtual void *interface(int interface) = 0;

  // Windows attached but not flushed yet, over the admission limit: attach
  // cannot refuse, so whoever feeds connects must ask this before starting work
  virtual bool pipelineFull() const { return false; }

  const std::string &name() const { return Name_; }
  BC::Proto::BlockHashTy currentBlock() { return CurrentBlock_; }

  virtual void flush() = 0;

protected:
  BC::Proto::BlockHashTy CurrentBlock_;
  std::string Name_;
};

// Where a database keyed by txid starts its walk over a block. A BIP30 repeat
// brings a coinbase already stored under that key - the same transaction byte
// for byte - so rewriting it would only move the record to the newer block, and
// the remove that undoes it would take the twin's record with it while the twin
// is still connected. The chain params carry both inclusions for the query side
static inline size_t firstTx(const BC::Proto::CBlockValidationData &validationData) {
  return validationData.CoinbaseRepeat ? 1 : 0;
}

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

// History element of addrhistorydb: the fact (txid) plus scalars denormalized
// for the list UI and the balance chart; Aggregate is the running balance
// maintained by CKvArrayBase - the address balance right after this tx
#pragma pack(push, 1)
struct CAddrHistoryItem {
  BC::Proto::TxHashTy TxId;
  uint32_t Height;
  uint32_t Time;
  uint64_t Aggregate;
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
// (all fields are additive, negative deltas use two's complement wrap)
#pragma pack(push, 1)
struct CAddrValue {
  uint64_t Received = 0;
  uint64_t Sent = 0;
  uint64_t Mined = 0;
  uint32_t TxCount = 0;
  uint32_t TxInCount = 0;
  uint32_t TxOutCount = 0;
  uint32_t MinedTxCount = 0;

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
    Received = 0 - Received;
    Sent = 0 - Sent;
    Mined = 0 - Mined;
    TxCount = 0 - TxCount;
    TxInCount = 0 - TxInCount;
    TxOutCount = 0 - TxOutCount;
    MinedTxCount = 0 - MinedTxCount;
  }

  bool isNull() const {
    return Received == 0 && Sent == 0 && Mined == 0 &&
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
