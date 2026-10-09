// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txRelay.h"
#include "network.h"
#include "../loguru.hpp"
#include <algorithm>
#include <cassert>
#include <string>

namespace BC {
namespace Network {

using BC::Mempool::CVerdict;
using BC::Mempool::steadySeconds;

namespace {

// Core's numbers (net_processing.cpp, txorphanage.h)
constexpr size_t MaxOrphans = 100;
constexpr int64_t OrphanLifetime = 20 * 60;
constexpr int64_t RequestTimeout = 60;
// A threshold, not a cap: past it the peer's announcements are asked for later
constexpr uint32_t MaxPeerRequestsInFlight = 100;
constexpr int64_t OverloadedPeerDelay = 2;
constexpr uint32_t MaxPeerAnnouncements = 5000;
// Stale marks beyond twice the requests and this are compacted away
constexpr size_t MarksSlack = 1024;
// Blocks whose txids are still recognized as confirmed (§5.7)
constexpr size_t ConfirmedBlocks = 10;
// A rolling filter in Core; here a set dropped whole when it gets this big
constexpr size_t MaxRejected = 120000;
// Messages waiting for the writer, by the memory they hold: a peer's share and everyone's
constexpr size_t MaxPeerQueuedBytes = 8 << 20;
constexpr size_t MaxQueuedBytes = 64 << 20;

// A witness can be swapped under the same txid, and a refusal may be the witness's fault: such a
// transaction is refused by its wtxid, as Core does for peers relaying by txid
template<typename TxTy>
CTxRelay::TxId rejectKey(const TxTy &tx, const CTxRelay::TxId &txid)
{
  if constexpr (requires { tx.hasWitness(); })
    return tx.hasWitness() ? tx.getWTxid() : txid;
  else
    return txid;
}

}

struct CTxRelay::CMessage {
  CMessage *Next = nullptr;
  EMessage Type;
  CPeerRef Source;
  xvector<TxId> TxIds;
  // One unpack2 allocation, owned until the writer takes it
  BC::Proto::CTransaction *Tx = nullptr;
  TxId Id;
  // Counted against the queue shares; none for PeerGone, which is never dropped
  size_t Bytes = 0;

  ~CMessage() { operator delete(Tx); }
};

CTxRelay::CTxRelay(BC::Mempool::CMempool &mempool) : Mempool_(mempool) {}

CTxRelay::~CTxRelay()
{
  for (CMessage *message = Inbox_.take(); message; ) {
    CMessage *next = message->Next;
    delete message;
    message = next;
  }
  for (auto &orphan: Orphans_)
    operator delete(orphan.second.Tx);
}

// Network threads

void CTxRelay::receiveInv(Peer *peer, xvector<TxId> &&txids)
{
  // Not active yet, or already in the mempool: the writer never hears of it
  BC::Mempool::CViewRef current = Mempool_.view();
  if (!current.get())
    return;

  size_t unknown = 0;
  for (size_t i = 0; i < txids.size(); i++) {
    if (!current.get()->find(txids[i]))
      txids[unknown++] = txids[i];
  }
  if (!unknown)
    return;
  txids.resize(unknown);
  const size_t bytes = sizeof(CMessage) + txids.memoryBytes();
  if (!admit(peer, bytes))
    return;

  CMessage *message = new CMessage;
  message->Type = EMessage::Inv;
  message->Source = CPeerRef(peer);
  message->TxIds = std::move(txids);
  message->Bytes = bytes;
  send(message);
}

void CTxRelay::receiveTx(Peer *peer, BC::Proto::CTransaction *tx, size_t size)
{
  BC::Mempool::CViewRef current = Mempool_.view();
  const TxId txid = tx->getTxId();
  if (!current.get() || current.get()->find(txid)) {
    if (current.get())
      Stats_.Duplicates.fetch_add(1, std::memory_order_relaxed);
    operator delete(tx);
    return;
  }
  const size_t bytes = sizeof(CMessage) + size;
  if (!admit(peer, bytes)) {
    operator delete(tx);
    return;
  }

  CMessage *message = new CMessage;
  message->Type = EMessage::Tx;
  message->Source = CPeerRef(peer);
  message->Tx = tx;
  message->Id = txid;
  message->Bytes = bytes;
  send(message);
}

// Nothing is asked and no peer has state before activation
void CTxRelay::receiveNotFound(Peer *peer, xvector<TxId> &&txids)
{
  if (!Mempool_.view().get())
    return;
  const size_t bytes = sizeof(CMessage) + txids.memoryBytes();
  if (!admit(peer, bytes))
    return;
  CMessage *message = new CMessage;
  message->Type = EMessage::NotFound;
  message->Source = CPeerRef(peer);
  message->TxIds = std::move(txids);
  message->Bytes = bytes;
  send(message);
}

void CTxRelay::peerGone(Peer *peer)
{
  if (!Mempool_.view().get())
    return;
  CMessage *message = new CMessage;
  message->Type = EMessage::PeerGone;
  message->Source = CPeerRef(peer);
  send(message);
}

// Over the peer's share or everyone's, a message is dropped: relay survives a loss (a request
// times out to the next announcer), while pausing the socket as Core does would stall its blocks
bool CTxRelay::admit(Peer *peer, size_t bytes)
{
  if (peer->RelayQueuedBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes <= MaxPeerQueuedBytes) {
    if (QueuedBytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes <= MaxQueuedBytes)
      return true;
    QueuedBytes_.fetch_sub(bytes, std::memory_order_relaxed);
  }
  peer->RelayQueuedBytes.fetch_sub(bytes, std::memory_order_relaxed);
  Stats_.Dropped.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void CTxRelay::send(CMessage *message)
{
  Inbox_.push(message);
  Mempool_.wake();
}

// The writer thread

void CTxRelay::onWake()
{
  for (CMessage *message = Inbox_.take(); message; ) {
    Stats_.Messages.fetch_add(1, std::memory_order_relaxed);
    handle(*message);
    release(*message);
    CMessage *next = message->Next;
    delete message;
    message = next;
  }

  retryOrphans();
  expireOrphans();
  timeouts();
  delayed();
  sendRequests();
}

int64_t CTxRelay::deadline() const
{
  int64_t deadline = RequestOrder_.empty() ? 0 : RequestOrder_.front().Time;
  if (!Delayed_.empty() && (!deadline || Delayed_.front().Time < deadline))
    deadline = Delayed_.front().Time;
  for (const auto &orphan: Orphans_) {
    if (!deadline || orphan.second.Expire < deadline)
      deadline = orphan.second.Expire;
  }
  return deadline;
}

void CTxRelay::transactionAdded(const TxId &txid)
{
  Added_.push_back(txid);
  forget(txid);
}

// Core's recent confirmed filter, its orphans included in the block or spending what it spent, and
// what was refused but mined: each of those is a case to look into on the stand
void CTxRelay::blockConnected(const BC::Common::BlockIndex *index, const BC::Proto::CBlock &block, const std::vector<TxId> &txids)
{
  size_t rejected = 0;
  size_t orphans = 0;
  size_t orphanConflicts = 0;
  ankerl::unordered_dense::map<std::string, size_t> reasons;
  for (size_t i = 0; i < txids.size(); i++) {
    const TxId &txid = txids[i];
    auto refused = Rejected_.find(txid);
    // A transaction with a witness is refused by its wtxid
    if (refused == Rejected_.end() && !Rejected_.empty())
      refused = Rejected_.find(rejectKey(block.Vtx[i + 1], txid));
    if (refused != Rejected_.end()) {
      rejected++;
      reasons[refused->second]++;
    } else if (auto orphan = Orphans_.find(txid); orphan != Orphans_.end()) {
      orphans++;
      operator delete(orphan->second.Tx);
      Orphans_.erase(orphan);
    }
    Confirmed_[txid]++;
    forget(txid);
  }

  // An orphan spending what the block spent can never enter, all of them on one outpoint (Core's
  // EraseForBlock)
  if (!Orphans_.empty()) {
    using COutpointSet = ankerl::unordered_dense::set<BC::Mempool::COutpoint, BC::Mempool::COutpointHash>;
    COutpointSet orphanInputs;
    COutpointSet spent;
    for (const auto &orphan: Orphans_) {
      for (const auto &in: orphan.second.Tx->TxIn)
        orphanInputs.insert({in.PreviousOutputHash, in.PreviousOutputIndex});
    }
    for (const auto &tx: block.Vtx) {
      for (const auto &in: tx.TxIn) {
        const BC::Mempool::COutpoint outpoint{in.PreviousOutputHash, in.PreviousOutputIndex};
        if (orphanInputs.contains(outpoint))
          spent.insert(outpoint);
      }
    }
    for (auto it = Orphans_.begin(); !spent.empty() && it != Orphans_.end(); ) {
      const auto &inputs = it->second.Tx->TxIn;
      if (std::any_of(inputs.begin(), inputs.end(), [&spent](const auto &in) { return spent.contains({in.PreviousOutputHash, in.PreviousOutputIndex}); })) {
        orphanConflicts++;
        operator delete(it->second.Tx);
        it = Orphans_.erase(it);
      } else {
        ++it;
      }
    }
  }

  ConfirmedBlocks_.push_back(txids);
  if (ConfirmedBlocks_.size() > ConfirmedBlocks) {
    for (const TxId &txid: ConfirmedBlocks_.front()) {
      auto it = Confirmed_.find(txid);
      if (it != Confirmed_.end() && --it->second == 0)
        Confirmed_.erase(it);
    }
    ConfirmedBlocks_.pop_front();
  }

  if (!rejected && !orphans && !orphanConflicts)
    return;
  std::string list;
  for (const auto &reason: reasons) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%s%s: %zu", list.empty() ? " (" : ", ", reason.first.c_str(), reason.second);
    list.append(buffer);
  }
  if (!list.empty())
    list.push_back(')');
  LOG_F(INFO, "txrelay: block %u: rejected %zu%s, orphan %zu, orphan conflicts %zu", index->Height, rejected, list.c_str(), orphans, orphanConflicts);
}

// A refusal could depend on the chain state (§5.7), a missing parent may be in the chain now
void CTxRelay::baseChanged(bool disconnected)
{
  Rejected_.clear();
  if (disconnected) {
    Confirmed_.clear();
    ConfirmedBlocks_.clear();
  }
  RetryAllOrphans_ = true;
}

void CTxRelay::handle(CMessage &message)
{
  switch (message.Type) {
    case EMessage::Inv :
      for (const TxId &txid: message.TxIds)
        announce(message.Source.get(), txid);
      break;
    case EMessage::Tx :
      onTx(message);
      break;
    case EMessage::NotFound :
      for (const TxId &txid: message.TxIds)
        dropRequest(txid, message.Source.get());
      break;
    case EMessage::PeerGone :
      onPeerGone(message.Source.get());
      break;
  }
}

void CTxRelay::release(const CMessage &message)
{
  if (!message.Bytes)
    return;
  message.Source.get()->RelayQueuedBytes.fetch_sub(message.Bytes, std::memory_order_relaxed);
  QueuedBytes_.fetch_sub(message.Bytes, std::memory_order_relaxed);
}

// Acceptance

void CTxRelay::onTx(CMessage &message)
{
  const TxId txid = message.Id;
  BC::Proto::CTransaction *tx = message.Tx;
  message.Tx = nullptr;
  forget(txid);

  // Already decided (§5.7). A txid refused stands for every witness: a child of refused parents
  const TxId key = rejectKey(*tx, txid);
  if (Orphans_.contains(txid) || Confirmed_.contains(txid) || Rejected_.contains(key) || Rejected_.contains(txid) || Mempool_.contains(txid)) {
    Stats_.Duplicates.fetch_add(1, std::memory_order_relaxed);
    operator delete(tx);
    return;
  }

  accept(tx, txid, key, message.Source, 0);
}

void CTxRelay::accept(BC::Proto::CTransaction *tx, const TxId &txid, const TxId &key, const CPeerRef &source, int64_t orphanExpire)
{
  Mempool_.accept(tx, txid, [this, key, source, orphanExpire](CVerdict &verdict) { onVerdict(verdict, key, source, orphanExpire); });
}

void CTxRelay::onVerdict(CVerdict &verdict, const TxId &key, const CPeerRef &source, int64_t orphanExpire)
{
  if (verdict.Result == CVerdict::EAccepted)
    return;
  if (verdict.Result == CVerdict::ERejected) {
    reject(key, verdict.Reason);
    return;
  }

  // A parent refused, or confirmed with this output already spent: nothing to wait for (Core's
  // rejected parents, plus the recently confirmed)
  for (const TxId &parent: verdict.MissingParents) {
    if (Rejected_.contains(parent) || Confirmed_.contains(parent)) {
      reject(verdict.TxId, verdict.Reason);
      return;
    }
  }

  // No one to ask its parents of: a waiting transaction's verdict can come after its peer left
  if (source.get()->deleted())
    return;

  if (Orphans_.size() >= MaxOrphans) {
    auto oldest = std::min_element(Orphans_.begin(), Orphans_.end(), [](const auto &l, const auto &r) { return l.second.Expire < r.second.Expire; });
    operator delete(oldest->second.Tx);
    Orphans_.erase(oldest);
  }

  // A retry still missing a parent goes back as it was: its parents were asked for already
  const bool retry = orphanExpire != 0;
  if (!retry)
    Stats_.Orphans.fetch_add(1, std::memory_order_relaxed);
  Orphans_[verdict.TxId] = COrphan{verdict.Tx, source, retry ? orphanExpire : steadySeconds() + OrphanLifetime, verdict.MissingParents};
  verdict.Tx = nullptr;
  // The parents are asked of whoever sent the child (§5.8)
  if (!retry) {
    for (const TxId &parent: verdict.MissingParents)
      announce(source.get(), parent);
  }
}

void CTxRelay::reject(const TxId &txid, const std::string &reason)
{
  if (Rejected_.size() >= MaxRejected)
    Rejected_.clear();
  Rejected_[txid] = reason;
}

// Orphans whose parent was accepted, or all of them after the base moved. A retry accepted makes
// another parent: the loop runs until nothing new comes
void CTxRelay::retryOrphans()
{
  while (RetryAllOrphans_ || !Added_.empty()) {
    std::vector<TxId> ready;
    for (const auto &orphan: Orphans_) {
      const std::vector<TxId> &missing = orphan.second.MissingParents;
      if (RetryAllOrphans_ ||
          std::any_of(missing.begin(), missing.end(), [this](const TxId &parent) { return std::find(Added_.begin(), Added_.end(), parent) != Added_.end(); }))
        ready.push_back(orphan.first);
    }
    RetryAllOrphans_ = false;
    Added_.clear();

    for (const TxId &txid: ready) {
      auto it = Orphans_.find(txid);
      if (it == Orphans_.end())
        continue;
      COrphan orphan = std::move(it->second);
      Orphans_.erase(it);
      accept(orphan.Tx, txid, rejectKey(*orphan.Tx, txid), orphan.Source, orphan.Expire);
    }
  }
}

void CTxRelay::expireOrphans()
{
  const int64_t now = steadySeconds();
  for (auto it = Orphans_.begin(); it != Orphans_.end(); ) {
    if (it->second.Expire <= now) {
      operator delete(it->second.Tx);
      it = Orphans_.erase(it);
    } else {
      ++it;
    }
  }
}

// Requests

bool CTxRelay::known(const TxId &txid) const
{
  return Mempool_.contains(txid) ||
         Orphans_.contains(txid) ||
         Confirmed_.contains(txid) ||
         Rejected_.contains(txid);
}

// A peer's messages may outlive its disconnect notice (§7.5): a dead peer gets no state
CTxRelay::CPeerState *CTxRelay::peerState(Peer *peer)
{
  if (!peer)
    return nullptr;
  auto it = Peers_.find(peer);
  if (it != Peers_.end())
    return &it->second;
  if (peer->deleted())
    return nullptr;
  CPeerState &state = Peers_[peer];
  state.Ref = CPeerRef(peer);
  return &state;
}

void CTxRelay::announce(Peer *peer, const TxId &txid)
{
  if (known(txid))
    return;
  CPeerState *state = peerState(peer);
  if (!state || state->Announced >= MaxPeerAnnouncements)
    return;

  CRequest &entry = Requests_[txid];
  if (std::find(entry.Announcers.begin(), entry.Announcers.end(), peer) != entry.Announcers.end())
    return;
  entry.Announcers.push_back(peer);
  state->Announced++;
  if (entry.Requested)
    return;
  if (state->InFlight < MaxPeerRequestsInFlight)
    request(entry, txid, peer);
  // A new request; an older one has its delayed ask pending
  else if (entry.Announcers.size() == 1)
    delay(entry, txid);
}

void CTxRelay::request(CRequest &entry, const TxId &txid, Peer *peer)
{
  // Announcers have state: a peer gone leaves every request before its state goes
  auto state = Peers_.find(peer);
  assert(state != Peers_.end());
  entry.Requested = peer;
  mark(RequestOrder_, entry, txid, steadySeconds() + RequestTimeout);
  state->second.InFlight++;
  state->second.Batch.emplace_back(txid);
  Stats_.Requested.fetch_add(1, std::memory_order_relaxed);
}

// The first announcer not overloaded. All of them overloaded: the first one later, or now if the
// request has waited already - no announcement waits for good, as in Core
void CTxRelay::requestNext(CRequest &entry, const TxId &txid, bool waited)
{
  for (Peer *peer: entry.Announcers) {
    auto state = Peers_.find(peer);
    if (state != Peers_.end() && state->second.InFlight < MaxPeerRequestsInFlight) {
      request(entry, txid, peer);
      return;
    }
  }
  if (entry.Announcers.empty())
    return;
  if (waited)
    request(entry, txid, entry.Announcers.front());
  else
    delay(entry, txid);
}

void CTxRelay::delay(CRequest &entry, const TxId &txid)
{
  mark(Delayed_, entry, txid, steadySeconds() + OverloadedPeerDelay);
}

// A new mark makes the request's earlier one stale. A stale mark waits for its time: compacted
// before such marks outnumber the requests, inv and notfound in a loop cannot pile them up
void CTxRelay::mark(std::deque<CMark> &marks, CRequest &entry, const TxId &txid, int64_t time)
{
  entry.Mark = ++MarkSeq_;
  marks.push_back({time, txid, entry.Mark});
  if (marks.size() > 2 * Requests_.size() + MarksSlack) {
    std::erase_if(marks, [this](const CMark &item) {
      auto it = Requests_.find(item.Id);
      return it == Requests_.end() || it->second.Mark != item.Seq;
    });
  }
}

// The peer will not deliver: not found, timed out or gone. The next announcer is asked
void CTxRelay::dropRequest(const TxId &txid, Peer *peer)
{
  auto it = Requests_.find(txid);
  if (it == Requests_.end())
    return;

  CRequest &entry = it->second;
  auto announcer = std::find(entry.Announcers.begin(), entry.Announcers.end(), peer);
  if (announcer == entry.Announcers.end())
    return;
  entry.Announcers.erase(announcer);

  auto state = Peers_.find(peer);
  if (state != Peers_.end())
    state->second.Announced--;
  if (entry.Requested == peer) {
    if (state != Peers_.end())
      state->second.InFlight--;
    entry.Requested = nullptr;
    requestNext(entry, txid, false);
  }

  if (!entry.Requested && entry.Announcers.empty())
    Requests_.erase(it);
}

// Known now: received, accepted or confirmed (Core's ForgetTxHash)
void CTxRelay::forget(const TxId &txid)
{
  auto it = Requests_.find(txid);
  if (it == Requests_.end())
    return;
  for (Peer *peer: it->second.Announcers) {
    auto state = Peers_.find(peer);
    if (state == Peers_.end())
      continue;
    state->second.Announced--;
    if (peer == it->second.Requested)
      state->second.InFlight--;
  }
  Requests_.erase(it);
}

void CTxRelay::onPeerGone(Peer *peer)
{
  // Its orphans go with it (Core's EraseForPeer)
  for (auto it = Orphans_.begin(); it != Orphans_.end(); ) {
    if (it->second.Source.get() == peer) {
      operator delete(it->second.Tx);
      it = Orphans_.erase(it);
    } else {
      ++it;
    }
  }

  if (!Peers_.contains(peer))
    return;

  std::vector<TxId> announced;
  for (const auto &entry: Requests_) {
    if (std::find(entry.second.Announcers.begin(), entry.second.Announcers.end(), peer) != entry.second.Announcers.end())
      announced.push_back(entry.first);
  }
  for (const TxId &txid: announced)
    dropRequest(txid, peer);
  Peers_.erase(peer);
}

void CTxRelay::timeouts()
{
  const int64_t now = steadySeconds();
  while (!RequestOrder_.empty() && RequestOrder_.front().Time <= now) {
    const CMark item = RequestOrder_.front();
    RequestOrder_.pop_front();
    auto it = Requests_.find(item.Id);
    // A stale mark: the request was answered or moved on since
    if (it != Requests_.end() && it->second.Mark == item.Seq)
      dropRequest(item.Id, it->second.Requested);
  }
}

void CTxRelay::delayed()
{
  const int64_t now = steadySeconds();
  while (!Delayed_.empty() && Delayed_.front().Time <= now) {
    const CMark item = Delayed_.front();
    Delayed_.pop_front();
    auto it = Requests_.find(item.Id);
    // A stale mark: asked for or answered since
    if (it != Requests_.end() && it->second.Mark == item.Seq)
      requestNext(it->second, item.Id, true);
  }
}

void CTxRelay::sendRequests()
{
  for (auto &peer: Peers_) {
    if (peer.second.Batch.empty())
      continue;
    if (!peer.first->deleted())
      peer.first->requestTransactions(peer.second.Batch);
    peer.second.Batch.resize(0);
  }
}

}
}
