// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// The peer side of transactions (mempool-impl-plan.md §5.7, §5.8, §7.3): who announced what, what
// is asked of whom, orphans and the filters of what was already decided. Core keeps all of it in
// net_processing; here it is the mempool writer's client and runs on its thread, so the mempool
// knows no peers. Network threads only push messages.

#include "common/inbox.h"
#include "common/intrusive_ptr.h"
#include "common/mempool.h"
#include "thirdparty/ankerl/unordered_dense.h"
#include <deque>
#include <string>

namespace BC {
namespace Network {

class Peer;
class PeerEmptyDeleter;

class CTxRelay : public BC::Mempool::CMempoolClient {
public:
  using TxId = BC::Proto::TxHashTy;

  // Relaxed counters for the API
  struct CStats {
    std::atomic<uint64_t> Messages = 0;
    std::atomic<uint64_t> Requested = 0;
    std::atomic<uint64_t> Duplicates = 0;
    std::atomic<uint64_t> Orphans = 0;
    // Over a share of the writer's queue, or longer than the policy passes
    std::atomic<uint64_t> Dropped = 0;
  };

  explicit CTxRelay(BC::Mempool::CMempool &mempool);
  // After the writer has stopped
  ~CTxRelay();

  // Network threads. The transaction is one unpack2 allocation of the given size, the relay takes it
  void receiveInv(Peer *peer, xvector<TxId> &&txids);
  void receiveTx(Peer *peer, BC::Proto::CTransaction *tx, size_t size);
  void receiveNotFound(Peer *peer, xvector<TxId> &&txids);
  void peerGone(Peer *peer);
  // A tx message left unparsed: longer than the policy passes
  void dropUnparsed() { Stats_.Dropped.fetch_add(1, std::memory_order_relaxed); }

  const CStats &stats() const { return Stats_; }

  // The writer thread
  void onWake() override;
  int64_t deadline() const override;
  void transactionAdded(const TxId &txid) override;
  void blockConnected(const BC::Common::BlockIndex *index, const BC::Proto::CBlock &block, const std::vector<TxId> &txids) override;
  void baseChanged(bool disconnected) override;

private:
  using CPeerRef = intrusive_ptr<Peer, PeerEmptyDeleter>;
  template<typename V> using CTxMap = ankerl::unordered_dense::map<TxId, V, BC::Mempool::CTxIdHash>;

  enum class EMessage {
    Inv,
    Tx,
    NotFound,
    PeerGone
  };

  struct CMessage;

  struct COrphan {
    BC::Proto::CTransaction *Tx;
    CPeerRef Source;
    int64_t Expire;
    std::vector<TxId> MissingParents;
  };

  // Asked of one announcer, or waiting to be asked: all of them were overloaded
  struct CRequest {
    std::vector<Peer*> Announcers;
    Peer *Requested = nullptr;
    // The one live mark of the request, in RequestOrder_ or Delayed_
    uint64_t Mark = 0;
  };

  struct CMark {
    int64_t Time;
    TxId Id;
    uint64_t Seq;
  };

  struct CPeerState {
    CPeerRef Ref;
    uint32_t InFlight = 0;
    uint32_t Announced = 0;
    xvector<TxId> Batch;
  };

  bool admit(Peer *peer, size_t bytes);
  void send(CMessage *message);
  void handle(CMessage &message);
  void release(const CMessage &message);

  // Acceptance through the mempool. orphanExpire: an orphan tried again keeps its lifetime
  void onTx(CMessage &message);
  void accept(BC::Proto::CTransaction *tx, const TxId &txid, const TxId &key, const CPeerRef &source, int64_t orphanExpire);
  void onVerdict(BC::Mempool::CVerdict &verdict, const TxId &key, const CPeerRef &source, int64_t orphanExpire);
  void reject(const TxId &txid, const std::string &reason);
  void retryOrphans();
  void expireOrphans();

  // Requests (§7.3)
  bool known(const TxId &txid) const;
  CPeerState *peerState(Peer *peer);
  void announce(Peer *peer, const TxId &txid);
  void request(CRequest &entry, const TxId &txid, Peer *peer);
  void requestNext(CRequest &entry, const TxId &txid, bool waited);
  void delay(CRequest &entry, const TxId &txid);
  void mark(std::deque<CMark> &marks, CRequest &entry, const TxId &txid, int64_t time);
  void dropRequest(const TxId &txid, Peer *peer);
  void forget(const TxId &txid);
  void onPeerGone(Peer *peer);
  void timeouts();
  void delayed();
  void sendRequests();

private:
  BC::Mempool::CMempool &Mempool_;
  // Network threads push and wake the writer, which takes
  CInbox<CMessage> Inbox_;
  // Memory held by every peer's messages in the queue
  std::atomic<size_t> QueuedBytes_ = 0;
  CStats Stats_;

  // Everything below is the writer thread's
  CTxMap<COrphan> Orphans_;
  // Accepted since the orphans were last tried (Core's orphan work set)
  std::vector<TxId> Added_;
  bool RetryAllOrphans_ = false;

  // Filters (§5.7): confirmed by the last blocks, refused at this base (a transaction with a
  // witness by its wtxid)
  std::deque<std::vector<TxId>> ConfirmedBlocks_;
  CTxMap<uint32_t> Confirmed_;
  CTxMap<std::string> Rejected_;

  // txid -> who announced it and who is asked. Marks in time order, each timeout and the delay one
  // constant: the request deadlines and the delayed asks
  CTxMap<CRequest> Requests_;
  std::deque<CMark> RequestOrder_;
  std::deque<CMark> Delayed_;
  uint64_t MarkSeq_ = 0;
  ankerl::unordered_dense::map<Peer*, CPeerState> Peers_;
};

}
}
