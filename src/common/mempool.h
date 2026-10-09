// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// The mempool writer: one thread owns every change, the rest of the node sends it messages and
// reads what it published. The storage wakes it after every utxo change; the wake carries nothing,
// the writer follows the published utxo revision itself and is consistent with the block it stands
// at (mempool-impl-plan.md §3, §4).
//
// The core is the same for every coin; what a transaction must satisfy is the coin's consensus and
// mempool policy (checkTransaction*, checkPolicy*). Peers are not its business: the coin's network side runs on the writer
// thread as its client and gives it transactions, as Core's net_processing gives them to
// AcceptToMemoryPool.

#include "BC/bc.h"
#include "common/inbox.h"
#include "common/mempoolStorage.h"
#include "thirdparty/ankerl/unordered_dense.h"
#include <asyncio/asyncio.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class BlockInMemoryIndex;

namespace BTC {
struct CTxCost;
}

namespace config4cpp {
class Configuration;
}

namespace BC {
namespace DB {
class Storage;
class UTXODb;
}

namespace Mempool {

// The clock of the writer's deadlines
inline int64_t steadySeconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// What became of a transaction given to the mempool. Reason is one of Core's reject strings, empty
// when accepted
struct CVerdict {
  enum EResult {
    EAccepted,
    // An input is neither in the mempool nor in utxo at the base: an orphan or a spent output, the
    // caller decides
    EMissingInputs,
    ERejected
  };

  EResult Result;
  std::string Reason;
  BC::Proto::TxHashTy TxId;
  // EMissingInputs: the parents not found, and the transaction itself, which the reply may take
  // by nulling the field
  std::vector<BC::Proto::TxHashTy> MissingParents;
  BC::Proto::CTransaction *Tx = nullptr;
};

struct COutpoint {
  BC::Proto::TxHashTy Tx;
  uint32_t Index;
  bool operator==(const COutpoint &other) const { return Index == other.Index && Tx == other.Tx; }
};

struct COutpointHash {
  using is_avalanching = void;
  uint64_t operator()(const COutpoint &o) const { return CTxIdHash()(o.Tx) ^ (o.Index * 0xC2B2AE3D27D4EB4FULL); }
};

// Runs on the writer thread beside the mempool: the coin's network side. The writer calls it as
// Core's validation signals call net_processing, and it gives transactions back through accept()
class CMempoolClient {
public:
  virtual ~CMempoolClient() = default;

  // Every pass of the writer, after the chain: the client's own messages and deadlines
  virtual void onWake() = 0;
  // The nearest own deadline in steadySeconds(), 0 for none: the writer wakes up for it
  virtual int64_t deadline() const = 0;

  virtual void transactionAdded(const BC::Proto::TxHashTy &txid) = 0;
  // A block the base moved over (§6.1): its transactions have left the mempool. After a jump or a
  // reset, only the last few blocks, for the client's filters. txids[i] is block.Vtx[i + 1]'s
  virtual void blockConnected(const BC::Common::BlockIndex *index,
                              const BC::Proto::CBlock &block,
                              const std::vector<BC::Proto::TxHashTy> &txids) = 0;
  // The base moved; 'disconnected': blocks were taken off it, what they confirmed is not confirmed
  // any more
  virtual void baseChanged(bool disconnected) = 0;
};

class CMempool {
public:
  using TxId = BC::Proto::TxHashTy;

  // Writer counters for the API: relaxed, nothing reads them to decide anything
  struct CStats {
    std::atomic<uint64_t> Wakeups = 0;
    std::atomic<uint64_t> Messages = 0;
    std::atomic<uint64_t> Jumps = 0;
    std::atomic<uint64_t> Resets = 0;
    std::atomic<uint64_t> Accepted = 0;
    std::atomic<uint64_t> Rejected = 0;
    std::atomic<uint64_t> MissingInputs = 0;
    std::atomic<uint64_t> Waiting = 0;
    std::atomic<uint64_t> Confirmed = 0;
    std::atomic<uint64_t> Conflicted = 0;
    std::atomic<uint64_t> Expired = 0;
  };

  // Runs once on the writer thread with the final verdict: at once, or when the base reached what
  // an input waits for (§4.4)
  using CReply = std::function<void(CVerdict&)>;

  CMempool();
  ~CMempool();

  // The writer thread; the mempool stays empty and still until activate(). The chain must stand
  // still: the storage gets the writer's wake event here. The client lives longer than the writer
  bool start(BlockInMemoryIndex &blockIndex,
             BC::Common::ChainParams &chainParams,
             BC::DB::Storage &storage,
             config4cpp::Configuration *cfg,
             CMempoolClient *client);
  // The chain must stand still, as for start()
  void stop();
  bool started() const { return Loop_ != nullptr; }

  // Once the node has caught up with its peers, and for good: later calls do nothing
  void activate();

  // Any thread: a transaction from the API, one unpack2 allocation of the given size the mempool
  // takes. False, with no reply, when such transactions already fill their share of the queue
  bool submit(BC::Proto::CTransaction *tx, size_t size, CReply reply);
  // Any thread, after start(): a pass of the writer, for the client's messages
  void wake() { userEventActivate(WakeEvent_); }

  // The writer thread only, for the client. accept() takes the transaction
  void accept(BC::Proto::CTransaction *tx, const TxId &txid, CReply reply);
  // In the mempool or waiting for the chain
  bool contains(const TxId &txid) const;

  // The published state, pinned for one request; null until activation
  CViewRef view() const { return CViewRef(Current_); }
  const CStats &stats() const { return Stats_; }

private:
  enum class EMessage {
    Activate,
    Submit
  };

  struct CMessage;

  struct CDecision;

  struct CWaiting {
    BC::Proto::CTransaction *Tx;
    TxId Id;
    CReply Reply;
    uint64_t Seq;
    uint32_t Size;
  };

  struct CBlockStats {
    size_t Txs = 0;
    size_t InMempool = 0;
    size_t Waiting = 0;
    size_t Conflicts = 0;
  };

  static void wakeCb(aioUserEvent*, void *arg) { static_cast<CMempool*>(arg)->onWake(); }
  // On IOCP a timer fires on a pool thread, even after stop(): it only wakes the writer, through a
  // reference to the wake event of its own, never through the mempool
  static void timerCb(aioUserEvent*, void *arg) { userEventActivate(static_cast<aioUserEvent*>(arg)); }
  static void timerGone(aioUserEvent*, void *arg) { eventDecrementReference(static_cast<aioUserEvent*>(arg), 1); }

  // Any thread
  void send(CMessage *message);

  // The writer thread from here on
  void onWake();
  void onActivate();

  // Acceptance (§5.2): decide() only reads, apply() makes every change
  CDecision decide(const BC::Proto::CTransaction &tx, const TxId &txid) const;
  // The coin's bounds on what the transaction joins in the mempool; the error is the reason
  bool limitsPass(const BC::Proto::CTransaction &tx, uint32_t vsize, std::string &error) const;
  bool chainPass(const BC::Proto::CTransaction &tx, uint32_t vsize) const;
  bool clusterPass(const BC::Proto::CTransaction &tx, uint32_t vsize) const;
  bool trucPass(const BC::Proto::CTransaction &tx, uint32_t vsize) const;
  void apply(BC::Proto::CTransaction *tx, const TxId &txid, CDecision &decision, CReply &&reply);
  void insert(BC::Proto::CTransaction *tx, const TxId &txid, int64_t fee, const BTC::CTxCost &cost);
  void retryWaiting();
  bool waiting(const TxId &txid) const;
  const BC::Common::BlockIndex *baseAncestor(uint32_t height) const;

  // Removal
  void collectDescendants(const TxId &txid, std::vector<TxId> &result) const;
  void erase(const TxId &txid);
  void eraseWithDescendants(const TxId &txid);

  // The chain (§4.3, §6.1)
  void sync();
  void connectBlock(BC::Common::BlockIndex *index);
  void recentBlocks(BC::Common::BlockIndex *position);
  void reset(const char *why);
  void expire();
  void armTimer();

  // Publication (§8)
  void publish();
  void compact(size_t capacity);
  void replaceGeneration(CGeneration *next);
  void recycle();

private:
  // Shared with other threads
  asyncBase *Loop_ = nullptr;
  // Activated by message senders and by the storage after a utxo change
  aioUserEvent *WakeEvent_ = nullptr;
  // One-shot, armed by the writer for the client's nearest deadline
  aioUserEvent *TimerEvent_ = nullptr;
  std::thread Thread_;
  // Producers push and wake; the writer thread is the only one that ever takes
  CInbox<CMessage> Inbox_;
  // Memory held by the API's transactions in the queue
  std::atomic<size_t> SubmitBytes_ = 0;
  std::atomic<bool> ActivationSent_ = false;
  mutable atomic_intrusive_ptr<CView, CViewDeleter> Current_;
  // Views whose last reader is gone, for the writer to free
  CReturnList<CView> RetiredViews_;
  CStats Stats_;

  // Set by start()
  BlockInMemoryIndex *BlockIndex_ = nullptr;
  BC::Common::ChainParams *ChainParams_ = nullptr;
  BC::DB::Storage *Storage_ = nullptr;
  BC::DB::UTXODb *Utxo_ = nullptr;
  CMempoolClient *Client_ = nullptr;
  uint64_t MaxBytes_ = 0;

  // Everything below is the writer thread's (§3): plain structures, no atomics
  bool Active_ = false;
  // The base (B, M): the block the mempool stands at and the utxo change sequence it matches
  BC::Common::BlockIndex *BaseIndex_ = nullptr;
  uint64_t BaseSeq_ = 0;

  // Published side: changes made now are seen from revision Revision_ + 1
  CGeneration *Generation_ = nullptr;
  uint64_t Revision_ = 0;
  uint64_t Sequence_ = 0;
  bool Dirty_ = false;

  // The mempool: txid -> body (the active generation holds it), outpoint -> the entry spending it
  ankerl::unordered_dense::map<TxId, CTxBody*, CTxIdHash> Entries_;
  ankerl::unordered_dense::map<COutpoint, TxId, COutpointHash> Spent_;
  uint64_t Bytes_ = 0;
  int64_t Fees_ = 0;

  // Waiting for the base (§4.4): in arrival order, and txid -> the sequence it waits for
  std::vector<CWaiting> Waiting_;
  ankerl::unordered_dense::map<TxId, uint64_t, CTxIdHash> WaitingSeq_;
  uint64_t WaitingBytes_ = 0;
  int64_t TimerDeadline_ = 0;
};

}
}
