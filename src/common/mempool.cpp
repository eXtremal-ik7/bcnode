// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "common/mempool.h"
#include "BC/script.h"
#include "BTC/policy.h"
#include "common/blockDataBase.h"
#include "common/smallStream.h"
#include "db/storage.h"
#include "config4cpp/Configuration.h"
#include "loguru.hpp"
#include <algorithm>
#include <ctime>
#include <random>

namespace BC {
namespace Mempool {

using TxId = BC::Proto::TxHashTy;

namespace {

// Core's numbers (validation.h, net_processing.cpp)
constexpr size_t AncestorLimit = 25;
constexpr size_t DescendantLimit = 25;
constexpr uint64_t AncestorSizeLimit = 101000;
constexpr uint64_t DescendantSizeLimit = 101000;
constexpr int64_t Expiry = 336 * 3600;
// Core 31 bounds the cluster a transaction joins (Bitcoin); the other coins' nodes, its ancestors
// and descendants
struct CClusterLimits {
  size_t Count = 0;
  uint64_t Size = 0;
};

template<typename C> constexpr CClusterLimits clusterLimits()
{
  if constexpr (requires { C::MempoolClusterLimit; })
    return {C::MempoolClusterLimit, C::MempoolClusterSizeLimit};
  else
    return {};
}

constexpr CClusterLimits ClusterLimits = clusterLimits<BC::Configuration>();

// BIP431 (TRUC) holds where the policy takes version 3 transactions
constexpr int32_t TrucVersion = 3;
constexpr uint32_t TrucMaxVSize = 10000;
constexpr uint32_t TrucChildMaxVSize = 1000;

template<typename C> constexpr bool trucPolicy()
{
  if constexpr (requires { C::MaxStandardTxVersion; })
    return C::MaxStandardTxVersion >= TrucVersion;
  else
    return false;
}

constexpr bool TrucPolicy = trucPolicy<BC::Configuration>();

// A longer way from the base to the utxo revision is not walked block by block (§6.3)
constexpr size_t MaxSyncBlocks = 100;
// What the client hears of after a jump or a reset: as deep as its filter of the confirmed
constexpr size_t RecentBlocks = 10;
constexpr size_t MinGenerationCapacity = 1024;
// The API's transactions waiting for the writer, by the memory they hold
constexpr size_t MaxSubmitQueuedBytes = 16 << 20;
// Transactions waiting for the base to catch up (§4.4)
constexpr size_t MaxWaiting = 5000;
constexpr uint64_t MaxWaitingBytes = 32 << 20;

template<typename T> constexpr bool HasHogEx = requires(const T &tx) { tx.HogEx; };
template<typename T> constexpr bool HasMweb = requires(const T &tx) { tx.MwebTx; };
template<typename T> constexpr bool HasShielded = requires(const T &tx) { tx.JoinSplits; };

// MWEB's HogEx closes the extension block: a block's own transaction, as the coinbase
template<typename T> bool isHogEx(const T &tx)
{
  if constexpr (HasHogEx<T>)
    return tx.HogEx;
  else
    return false;
}

// Parts the mempool cannot hold: what they spend is not in utxo, what they conflict with is not
// tracked, and a shielded value balance is not in the fee
template<typename T> bool hasMweb(const T &tx)
{
  if constexpr (HasMweb<T>)
    return tx.hasMweb();
  else
    return false;
}

template<typename T> bool hasShielded(const T &tx)
{
  if constexpr (HasShielded<T>)
    return !tx.JoinSplits.empty() || !tx.ShieldedSpends.empty() || !tx.ShieldedOutputs.empty();
  else
    return false;
}

// The utxo type an output would be stored with
uint8_t outputType(const BC::Proto::CTxOut &out)
{
  SmallStream<256> stream;
  BC::Script::parseTransactionOutput(out, stream);
  return stream.data<BC::Script::CUnspentOutputInfo>()->Type;
}

using CTxSet = ankerl::unordered_dense::set<TxId, CTxIdHash>;

// The writer and every view on a generation hold a reference
void release(CGeneration *generation)
{
  if (--generation->Refs == 0)
    delete generation;
}

// Core's GetMedianTimePast: the median of the last 11 block times
int64_t medianTimePast(const BC::Common::BlockIndex *index)
{
  std::vector<int64_t> times;
  for (; index && times.size() < 11; index = index->Prev)
    times.push_back(index->Header.Time);
  std::sort(times.begin(), times.end());
  return times.empty() ? 0 : times[times.size() / 2];
}

}

struct CMempool::CMessage {
  CMessage *Next = nullptr;
  EMessage Type;
  // One unpack2 allocation, owned until the writer takes it
  BC::Proto::CTransaction *Tx = nullptr;
  size_t Bytes = 0;
  CReply Reply;

  ~CMessage() { operator delete(Tx); }
};

// What acceptance found, nothing changed yet: apply() carries it out
struct CMempool::CDecision {
  enum EOutcome {
    EAccept,
    EReject,
    EMissingInputs,
    EWait
  };

  EOutcome Outcome = EReject;
  std::string Reason;
  // Accept: what the entry is made of
  int64_t Fee = 0;
  BTC::CTxCost Cost;
  // Wait: the utxo change sequence the base has to reach
  uint64_t Seq = 0;
  std::vector<TxId> MissingParents;
};

// Any thread

CMempool::CMempool()
{
  std::random_device random;
  CTxIdHash::Salt = (static_cast<uint64_t>(random()) << 32) | random();
}

CMempool::~CMempool()
{
  stop();
}

bool CMempool::start(BlockInMemoryIndex &blockIndex,
                     BC::Common::ChainParams &chainParams,
                     BC::DB::Storage &storage,
                     config4cpp::Configuration *cfg,
                     CMempoolClient *client)
{
  Loop_ = createAsyncBase(amOSDefault, 1);
  if (!Loop_) {
    LOG_F(ERROR, "Can't create asyncio base for mempool");
    return false;
  }

  BlockIndex_ = &blockIndex;
  ChainParams_ = &chainParams;
  Storage_ = &storage;
  Utxo_ = &storage.utxodb();
  Client_ = client;
  MaxBytes_ = static_cast<uint64_t>(cfg->lookupInt("mempool", "maxSizeMb", 300)) << 20;
  Generation_ = new CGeneration(MinGenerationCapacity);

  WakeEvent_ = newUserEvent(Loop_, 0, wakeCb, this);
  eventIncrementReference(WakeEvent_, 1);
  TimerEvent_ = newUserEvent(Loop_, 0, timerCb, WakeEvent_);
  eventSetDestructorCb(TimerEvent_, timerGone, WakeEvent_);
  storage.setUtxoEvent(WakeEvent_);
  Thread_ = std::thread([](asyncBase *base) {
    loguru::set_thread_name("mempool");
    asyncLoop(base);
  }, Loop_);
  return true;
}

void CMempool::stop()
{
  if (!Loop_)
    return;

  Storage_->setUtxoEvent(nullptr);
  postQuitOperation(Loop_);
  Thread_.join();
  // After the writer: it arms the timer up to its last pass
  deleteUserEvent(TimerEvent_);
  deleteUserEvent(WakeEvent_);
  TimerEvent_ = nullptr;
  WakeEvent_ = nullptr;
  Loop_ = nullptr;

  // Sent after the writer stopped: nobody took them
  for (CMessage *message = Inbox_.take(); message; ) {
    CMessage *next = message->Next;
    delete message;
    message = next;
  }

  // The readers are gone by now: the last view comes back to be freed, then the generations go
  Current_.reset();
  recycle();
  release(Generation_);
  Generation_ = nullptr;
  Entries_.clear();
  Spent_.clear();
  for (auto &waiting: Waiting_)
    operator delete(waiting.Tx);
  Waiting_.clear();
  WaitingSeq_.clear();
  WaitingBytes_ = 0;
}

void CMempool::activate()
{
  if (!Loop_ || ActivationSent_.exchange(true))
    return;
  CMessage *message = new CMessage;
  message->Type = EMessage::Activate;
  send(message);
}

bool CMempool::submit(BC::Proto::CTransaction *tx, size_t size, CReply reply)
{
  const size_t bytes = sizeof(CMessage) + size;
  if (SubmitBytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes > MaxSubmitQueuedBytes) {
    SubmitBytes_.fetch_sub(bytes, std::memory_order_relaxed);
    operator delete(tx);
    return false;
  }

  CMessage *message = new CMessage;
  message->Type = EMessage::Submit;
  message->Tx = tx;
  message->Bytes = bytes;
  message->Reply = std::move(reply);
  send(message);
  return true;
}

void CMempool::send(CMessage *message)
{
  Inbox_.push(message);
  userEventActivate(WakeEvent_);
}

// The writer thread

// One pass: the chain, the messages, the client. Messages and utxo changes that came while the
// writer was busy coalesce into one pass
void CMempool::onWake()
{
  Stats_.Wakeups.fetch_add(1, std::memory_order_relaxed);
  recycle();
  // The probes of acceptance are compared against the base: it goes first
  if (Active_)
    sync();

  for (CMessage *message = Inbox_.take(); message; ) {
    Stats_.Messages.fetch_add(1, std::memory_order_relaxed);
    switch (message->Type) {
      case EMessage::Activate :
        onActivate();
        break;
      case EMessage::Submit : {
        SubmitBytes_.fetch_sub(message->Bytes, std::memory_order_relaxed);
        BC::Proto::CTransaction *tx = message->Tx;
        message->Tx = nullptr;
        accept(tx, tx->getTxId(), std::move(message->Reply));
        break;
      }
    }
    CMessage *next = message->Next;
    delete message;
    message = next;
  }

  if (Client_)
    Client_->onWake();
  armTimer();
  if (Active_ && Dirty_)
    publish();
}

void CMempool::onActivate()
{
  sync();
  // No base: the next activate() tries again
  if (!BaseIndex_) {
    ActivationSent_.store(false);
    return;
  }
  Active_ = true;
  publish();
  LOG_F(INFO, "mempool: active at %s (%u)", BaseIndex_->Header.GetHash().getHexLE().c_str(), BaseIndex_->Height);
}

// Acceptance (§5.2)

void CMempool::accept(BC::Proto::CTransaction *tx, const TxId &txid, CReply reply)
{
  CDecision decision;
  if (!Active_)
    decision.Reason = "mempool-inactive";
  else
    decision = decide(*tx, txid);
  apply(tx, txid, decision, std::move(reply));
}

bool CMempool::contains(const TxId &txid) const
{
  return Entries_.contains(txid) || waiting(txid);
}

// Carries a decision out: the entry, the waiting list, the counters, the verdict, the client. The
// transaction is ours from here
void CMempool::apply(BC::Proto::CTransaction *tx, const TxId &txid, CDecision &decision, CReply &&reply)
{
  CVerdict verdict;
  verdict.TxId = txid;
  switch (decision.Outcome) {
    case CDecision::EAccept :
      insert(tx, txid, decision.Fee, decision.Cost);
      Stats_.Accepted.fetch_add(1, std::memory_order_relaxed);
      verdict.Result = CVerdict::EAccepted;
      reply(verdict);
      if (Client_)
        Client_->transactionAdded(txid);
      break;

    case CDecision::EReject :
      Stats_.Rejected.fetch_add(1, std::memory_order_relaxed);
      verdict.Result = CVerdict::ERejected;
      verdict.Reason = decision.Reason;
      reply(verdict);
      operator delete(tx);
      break;

    case CDecision::EMissingInputs :
      Stats_.MissingInputs.fetch_add(1, std::memory_order_relaxed);
      verdict.Result = CVerdict::EMissingInputs;
      verdict.Reason = "bad-txns-inputs-missingorspent";
      verdict.MissingParents = std::move(decision.MissingParents);
      verdict.Tx = tx;
      reply(verdict);
      operator delete(verdict.Tx);
      break;

    case CDecision::EWait :
      Stats_.Waiting.fetch_add(1, std::memory_order_relaxed);
      Waiting_.push_back({tx, txid, std::move(reply), decision.Seq, decision.Cost.Size});
      WaitingSeq_[txid] = decision.Seq;
      WaitingBytes_ += decision.Cost.Size;
      break;
  }
}

// Every check of §5.2: the module's own - duplicates, a block's own transactions, parts it cannot
// hold, the inputs and the age rule of §4.4 - and around them the coin's consensus and policy
CMempool::CDecision CMempool::decide(const BC::Proto::CTransaction &tx, const TxId &txid) const
{
  CDecision decision;
  auto reject = [&decision](std::string reason) {
    decision.Outcome = CDecision::EReject;
    decision.Reason = std::move(reason);
    return decision;
  };
  // The waiting list is bounded: full, it refuses, and the base moving on clears the refusal
  auto wait = [this, &decision, &reject](uint64_t seq) {
    if (Waiting_.size() >= MaxWaiting || WaitingBytes_ + decision.Cost.Size > MaxWaitingBytes)
      return reject("mempool-waiting-full");
    decision.Outcome = CDecision::EWait;
    decision.Seq = seq;
    return decision;
  };

  if (contains(txid))
    return reject("txn-already-in-mempool");
  if (BTC::isCoinbase(tx))
    return reject("coinbase");
  if (isHogEx(tx))
    return reject("hogex");
  if (hasMweb(tx))
    return reject("mweb-unsupported");
  if (hasShielded(tx))
    return reject("shielded-unsupported");

  // The transaction alone
  std::string error;
  if (!BC::Common::checkTransactionStandalone(tx, *ChainParams_, error))
    return reject(error);
  BTC::measureTx(tx, decision.Cost);
  if (!BC::Common::checkPolicyStandalone(tx, decision.Cost, error))
    return reject(error);

  // Inputs: a parent in the mempool, otherwise the utxo database read as it is (§4.4). A parent in
  // the mempool counts as the next block
  const uint32_t spendHeight = BaseIndex_->Height + 1;
  const size_t inputs = tx.TxIn.size();
  std::vector<BTC::CPrevout> prevouts(inputs);
  std::vector<uint8_t> probed(inputs, 0);
  std::vector<uint8_t> found(inputs, 0);
  std::vector<uint8_t> restored(inputs, 0);
  xvector<uint8_t> data;
  for (size_t i = 0; i < inputs; i++) {
    const COutpoint outpoint{tx.TxIn[i].PreviousOutputHash, tx.TxIn[i].PreviousOutputIndex};
    // First seen wins: no replacement
    if (Spent_.contains(outpoint))
      return reject("txn-mempool-conflict");

    if (auto parent = Entries_.find(outpoint.Tx); parent != Entries_.end()) {
      const BC::Proto::CTransaction &parentTx = *parent->second->Tx;
      if (outpoint.Index >= parentTx.TxOut.size())
        return reject("bad-txns-inputs-missingorspent");
      const BC::Proto::CTxOut &out = parentTx.TxOut[outpoint.Index];
      prevouts[i] = {out.Value, outputType(out), spendHeight, false, false};
      found[i] = 1;
      continue;
    }

    // Behind a parent that waits for the chain (§5.9)
    if (auto parent = WaitingSeq_.find(outpoint.Tx); parent != WaitingSeq_.end())
      return wait(parent->second);

    probed[i] = 1;
    uint32_t meta = 0;
    if (Utxo_->query(outpoint.Tx, outpoint.Index, data, meta)) {
      const BC::Script::CUnspentOutputInfo *info = reinterpret_cast<const BC::Script::CUnspentOutputInfo*>(data.data());
      const uint8_t flags = BC::DB::utxoMetaFlags(meta);
      prevouts[i] = {info->Value, info->Type, BC::DB::utxoMetaHeight(meta), (flags & BC::DB::EUtxoCoinbase) != 0, (flags & BC::DB::EUtxoPegout) != 0};
      restored[i] = (flags & BC::DB::EUtxoRestored) != 0;
      found[i] = 1;
    }
  }

  // One load after all probes: how many steps utxo may have moved from the base under them. A
  // found output without the Restored mark and no higher than r - L was there at the base (§4.4)
  const uint64_t lag = Utxo_->changeSeq() - BaseSeq_;
  const int64_t oldestSafe = static_cast<int64_t>(BaseIndex_->Height) - static_cast<int64_t>(lag);
  bool allFound = true;
  bool unresolved = false;
  for (size_t i = 0; i < inputs; i++) {
    if (!probed[i])
      continue;
    if (!found[i]) {
      allFound = false;
      unresolved |= lag != 0;
    } else if (restored[i]) {
      unresolved |= lag != 0;
    } else {
      unresolved |= static_cast<int64_t>(prevouts[i].Height) > oldestSafe;
    }
  }

  if (unresolved)
    return wait(BaseSeq_ + lag);

  if (!allFound) {
    // Missing at the base and not in the mempool (§5.2 p.3). Outputs of its own in utxo: it is in
    // the chain already. The cache only, as Core's HaveCoinInCache: no read per output, none
    // without the cache
    for (uint32_t i = 0; Utxo_->cacheEnabled() && i < tx.TxOut.size(); i++) {
      uint32_t meta = 0;
      if (Utxo_->query(txid, i, data, meta, true))
        return reject("txn-already-known");
    }
    for (size_t i = 0; i < inputs; i++) {
      if (!probed[i] || found[i])
        continue;
      const TxId &parent = tx.TxIn[i].PreviousOutputHash;
      if (std::find(decision.MissingParents.begin(), decision.MissingParents.end(), parent) == decision.MissingParents.end())
        decision.MissingParents.push_back(parent);
    }
    decision.Outcome = CDecision::EMissingInputs;
    return decision;
  }

  // With the inputs, for the block the transaction would enter
  BTC::CTxContext context;
  context.Height = spendHeight;
  context.BlockTime = time(nullptr);
  context.MedianTimePast = medianTimePast(BaseIndex_);
  context.MedianTimePastAt = [this](uint32_t height) { return medianTimePast(baseAncestor(height)); };
  if (!BC::Common::checkTransactionContextual(tx, prevouts.data(), context, *ChainParams_, decision.Fee, error))
    return reject(error);
  BTC::measureInputs(tx, prevouts.data(), decision.Cost);
  if (!BC::Common::checkPolicyContextual(tx, prevouts.data(), context, decision.Cost, decision.Fee, error))
    return reject(error);

  // The mempool's own limits
  if (!limitsPass(tx, decision.Cost.vsize(), error))
    return reject(error);
  if (Bytes_ + decision.Cost.Size > MaxBytes_)
    return reject("mempool full");

  decision.Outcome = CDecision::EAccept;
  return decision;
}

// The base's block at a height. Deep below the base the best chain's height index has it: another
// block in the slot means B - height + 1 disconnects first, each step begun before that slot store,
// so the sequence loaded after the slot counts them (§4.4); a cleared slot reads null. Near the
// base, a short walk
const BC::Common::BlockIndex *CMempool::baseAncestor(uint32_t height) const
{
  const BC::Common::BlockIndex *index = BlockIndex_->indexByHeight(height);
  const uint64_t lag = Utxo_->changeSeq() - BaseSeq_;
  if (index && static_cast<int64_t>(height) < static_cast<int64_t>(BaseIndex_->Height) - static_cast<int64_t>(lag))
    return index;

  index = BaseIndex_;
  while (index && index->Height > height)
    index = index->Prev;
  return index;
}

// Ancestors and descendants by walking the links (§5.9): the union of the ancestors, then for
// each of them the descendants it would have with the new transaction
bool CMempool::limitsPass(const BC::Proto::CTransaction &tx, uint32_t vsize, std::string &error) const
{
  if (ClusterLimits.Count) {
    if (!clusterPass(tx, vsize)) {
      error = "too-large-cluster";
      return false;
    }
  } else if (!chainPass(tx, vsize)) {
    error = "too-long-mempool-chain";
    return false;
  }
  if (TrucPolicy && !trucPass(tx, vsize)) {
    error = "TRUC-violation";
    return false;
  }
  return true;
}

bool CMempool::chainPass(const BC::Proto::CTransaction &tx, uint32_t vsize) const
{
  CTxSet ancestors;
  std::vector<TxId> stack;
  for (const auto &in: tx.TxIn) {
    if (Entries_.contains(in.PreviousOutputHash) && ancestors.insert(in.PreviousOutputHash).second)
      stack.push_back(in.PreviousOutputHash);
  }

  uint64_t ancestorSize = vsize;
  while (!stack.empty()) {
    TxId txid = stack.back();
    stack.pop_back();
    const CTxBody *body = Entries_.at(txid);
    ancestorSize += body->VSize;
    if (ancestors.size() + 1 > AncestorLimit || ancestorSize > AncestorSizeLimit)
      return false;
    for (const auto &in: body->Tx->TxIn) {
      if (Entries_.contains(in.PreviousOutputHash) && ancestors.insert(in.PreviousOutputHash).second)
        stack.push_back(in.PreviousOutputHash);
    }
  }

  std::vector<TxId> descendants;
  for (const TxId &ancestor: ancestors) {
    descendants.clear();
    collectDescendants(ancestor, descendants);
    uint64_t size = Entries_.at(ancestor)->VSize + vsize;
    for (const TxId &txid: descendants)
      size += Entries_.at(txid)->VSize;
    // The ancestor, its descendants and the new one
    if (descendants.size() + 2 > DescendantLimit || size > DescendantSizeLimit)
      return false;
  }

  return true;
}

// The clusters of the parents merge with the new transaction: everything linked to them, parents
// and children alike
bool CMempool::clusterPass(const BC::Proto::CTransaction &tx, uint32_t vsize) const
{
  CTxSet cluster;
  std::vector<TxId> stack;
  for (const auto &in: tx.TxIn) {
    if (Entries_.contains(in.PreviousOutputHash) && cluster.insert(in.PreviousOutputHash).second)
      stack.push_back(in.PreviousOutputHash);
  }

  uint64_t size = vsize;
  while (!stack.empty()) {
    const TxId txid = stack.back();
    stack.pop_back();
    const CTxBody *body = Entries_.at(txid);
    size += body->VSize;
    if (cluster.size() + 1 > ClusterLimits.Count || size > ClusterLimits.Size)
      return false;
    for (const auto &in: body->Tx->TxIn) {
      if (Entries_.contains(in.PreviousOutputHash) && cluster.insert(in.PreviousOutputHash).second)
        stack.push_back(in.PreviousOutputHash);
    }
    for (uint32_t i = 0; i < body->Tx->TxOut.size(); i++) {
      auto child = Spent_.find(COutpoint{txid, i});
      if (child != Spent_.end() && cluster.insert(child->second).second)
        stack.push_back(child->second);
    }
  }

  return true;
}

// Core's SingleTRUCChecks: unconfirmed, a version 3 transaction is a lone parent or its only child,
// and the two kinds do not spend each other. First seen wins: no sibling eviction
bool CMempool::trucPass(const BC::Proto::CTransaction &tx, uint32_t vsize) const
{
  const bool truc = tx.Version == TrucVersion;
  CTxSet parents;
  for (const auto &in: tx.TxIn) {
    auto parent = Entries_.find(in.PreviousOutputHash);
    if (parent == Entries_.end())
      continue;
    if ((parent->second->Tx->Version == TrucVersion) != truc)
      return false;
    parents.insert(in.PreviousOutputHash);
  }
  if (!truc)
    return true;
  if (vsize > TrucMaxVSize)
    return false;
  if (parents.empty())
    return true;
  if (parents.size() > 1 || vsize > TrucChildMaxVSize)
    return false;

  // The parent has no unconfirmed parent and no other child
  const TxId &parentId = *parents.begin();
  const BC::Proto::CTransaction &parentTx = *Entries_.at(parentId)->Tx;
  for (const auto &in: parentTx.TxIn) {
    if (Entries_.contains(in.PreviousOutputHash))
      return false;
  }
  for (uint32_t i = 0; i < parentTx.TxOut.size(); i++) {
    if (Spent_.contains(COutpoint{parentId, i}))
      return false;
  }
  return true;
}

void CMempool::insert(BC::Proto::CTransaction *tx, const TxId &txid, int64_t fee, const BTC::CTxCost &cost)
{
  CTxBody *body = new CTxBody;
  body->Size = cost.Size;
  body->VSize = cost.vsize();
  body->SigOpsCost = cost.SigOpsCost;
  body->Fee = fee;
  body->Time = time(nullptr);
  body->Sequence = ++Sequence_;
  body->TxId = txid;
  body->WTxId = tx->getWTxid();
  body->Tx = tx;

  if (Generation_->full())
    compact(2 * (Entries_.size() + 1));
  Generation_->put(body, Revision_ + 1, 0);

  Entries_[txid] = body;
  for (const auto &in: body->Tx->TxIn)
    Spent_[COutpoint{in.PreviousOutputHash, in.PreviousOutputIndex}] = txid;
  Bytes_ += body->Size;
  Fees_ += fee;
  Dirty_ = true;
}

// After a synchronization that brought the base to what they waited for (§4.4)
void CMempool::retryWaiting()
{
  std::vector<CWaiting> waitingNow;
  waitingNow.swap(Waiting_);
  WaitingSeq_.clear();
  WaitingBytes_ = 0;
  for (CWaiting &entry: waitingNow) {
    if (entry.Seq > BaseSeq_) {
      WaitingSeq_[entry.Id] = entry.Seq;
      WaitingBytes_ += entry.Size;
      Waiting_.push_back(std::move(entry));
      continue;
    }
    CDecision decision = decide(*entry.Tx, entry.Id);
    apply(entry.Tx, entry.Id, decision, std::move(entry.Reply));
  }
}

bool CMempool::waiting(const TxId &txid) const
{
  return WaitingSeq_.contains(txid);
}

// Removal

void CMempool::collectDescendants(const TxId &txid, std::vector<TxId> &result) const
{
  CTxSet seen;
  std::vector<TxId> stack{txid};
  while (!stack.empty()) {
    TxId current = stack.back();
    stack.pop_back();
    const CTxBody *body = Entries_.at(current);
    for (uint32_t i = 0; i < body->Tx->TxOut.size(); i++) {
      auto child = Spent_.find(COutpoint{current, i});
      if (child != Spent_.end() && seen.insert(child->second).second) {
        result.push_back(child->second);
        stack.push_back(child->second);
      }
    }
  }
}

// This entry alone: its children keep their inputs, now in the chain or gone with them
void CMempool::erase(const TxId &txid)
{
  auto it = Entries_.find(txid);
  if (it == Entries_.end())
    return;
  CTxBody *body = it->second;
  for (const auto &in: body->Tx->TxIn)
    Spent_.erase(COutpoint{in.PreviousOutputHash, in.PreviousOutputIndex});
  Bytes_ -= body->Size;
  Fees_ -= body->Fee;
  Generation_->erase(txid, Revision_ + 1);
  Entries_.erase(it);
  Dirty_ = true;
}

void CMempool::eraseWithDescendants(const TxId &txid)
{
  if (!Entries_.contains(txid))
    return;
  std::vector<TxId> doomed{txid};
  collectDescendants(txid, doomed);
  for (const TxId &id: doomed)
    erase(id);
}

// The chain

// §4.3: the base catches up with the published utxo revision, block by block from the index and
// the block objects; it jumps when there is nothing to apply the blocks to
void CMempool::sync()
{
  const BC::DB::UTXODb::CRevision revision = Utxo_->revision();
  if (BaseIndex_ && revision.Seq == BaseSeq_)
    return;

  BC::Common::BlockIndex *position = BlockIndex_->indexByHash(revision.Stamp);
  if (!position) {
    // A published revision always stands at a block of the index
    LOG_F(ERROR, "mempool: utxo revision %s is not in the block index", revision.Stamp.getHexLE().c_str());
    return;
  }

  // The fork: up from the higher of the two, then from both. Prev and Height never change
  std::vector<BC::Common::BlockIndex*> connects;
  size_t disconnects = 0;
  if (BaseIndex_) {
    BC::Common::BlockIndex *from = BaseIndex_;
    BC::Common::BlockIndex *to = position;
    while (from->Height > to->Height) {
      from = from->Prev;
      disconnects++;
    }
    while (to->Height > from->Height) {
      connects.push_back(to);
      to = to->Prev;
    }
    while (from != to) {
      from = from->Prev;
      disconnects++;
      connects.push_back(to);
      to = to->Prev;
    }
  }

  // Forward and not far: block by block, an empty mempool too, for the client to hear of every
  // block. Otherwise a jump, starting over if there are entries (the waiting stay either way);
  // disconnect is step 7 of the plan (§6.3)
  const bool walk = BaseIndex_ && !disconnects && connects.size() <= MaxSyncBlocks;
  if (walk) {
    for (auto it = connects.rbegin(); it != connects.rend(); ++it)
      connectBlock(*it);
  } else if (!BaseIndex_ || Entries_.empty()) {
    Stats_.Jumps.fetch_add(1, std::memory_order_relaxed);
  } else {
    reset(disconnects ? "disconnect" : "long way");
  }

  BaseIndex_ = position;
  BaseSeq_ = revision.Seq;
  Dirty_ = true;
  if (Client_)
    Client_->baseChanged(disconnects != 0);
  if (!walk)
    recentBlocks(position);

  expire();
  retryWaiting();
  // Removed records pile up in an append-only generation
  if (Generation_->used() > MinGenerationCapacity && Generation_->used() > 2 * Entries_.size())
    compact(2 * Entries_.size());
}

// §6.1: confirmed entries leave and their children stay; entries spending what the block spent
// leave with all their descendants. The statistics are the main check of the stand
void CMempool::connectBlock(BC::Common::BlockIndex *index)
{
  intrusive_ptr<BC::Common::CIndexCacheObject> object = objectByIndex(index, *ChainParams_, Storage_->blockDb());
  if (!object.get()) {
    LOG_F(ERROR, "mempool: can't load block %s (%u)", index->Header.GetHash().getHexLE().c_str(), index->Height);
    reset("unreadable block");
    return;
  }

  const BC::Proto::CBlock &block = *object.get()->block();
  const BC::Proto::CBlockValidationData &validation = object.get()->validationDataConst();
  CBlockStats blockStats;

  std::vector<TxId> confirmed;
  for (size_t i = 1; i < block.Vtx.size(); i++) {
    const TxId &txid = validation.TxIds[i];
    confirmed.push_back(txid);
    if (isHogEx(block.Vtx[i]))
      continue;
    blockStats.Txs++;
    if (Entries_.contains(txid)) {
      blockStats.InMempool++;
      erase(txid);
      Stats_.Confirmed.fetch_add(1, std::memory_order_relaxed);
    } else if (waiting(txid)) {
      blockStats.Waiting++;
    }
  }

  // Whatever still spends an outpoint the block spent conflicts with it
  for (size_t i = 1; i < block.Vtx.size(); i++) {
    for (const auto &in: block.Vtx[i].TxIn) {
      auto spender = Spent_.find(COutpoint{in.PreviousOutputHash, in.PreviousOutputIndex});
      if (spender == Spent_.end())
        continue;
      size_t before = Entries_.size();
      eraseWithDescendants(TxId(spender->second));
      blockStats.Conflicts += before - Entries_.size();
    }
  }
  Stats_.Conflicted.fetch_add(blockStats.Conflicts, std::memory_order_relaxed);

  LOG_F(INFO,
        "mempool: block %u: %zu txs, in mempool %zu, waiting %zu, absent %zu; conflicts %zu; mempool %zu txs",
        index->Height,
        blockStats.Txs,
        blockStats.InMempool,
        blockStats.Waiting,
        blockStats.Txs - blockStats.InMempool - blockStats.Waiting,
        blockStats.Conflicts,
        Entries_.size());

  if (Client_)
    Client_->blockConnected(index, block, confirmed);
}

// After the base change, so that a disconnect clearing the client's filters comes first
void CMempool::recentBlocks(BC::Common::BlockIndex *position)
{
  if (!Client_)
    return;
  std::vector<BC::Common::BlockIndex*> recent;
  for (BC::Common::BlockIndex *index = position; index && recent.size() < RecentBlocks; index = index->Prev)
    recent.push_back(index);

  for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
    intrusive_ptr<BC::Common::CIndexCacheObject> object = objectByIndex(*it, *ChainParams_, Storage_->blockDb());
    if (!object.get())
      continue;
    const BC::Proto::CBlock &block = *object.get()->block();
    const BC::Proto::CBlockValidationData &validation = object.get()->validationDataConst();
    std::vector<TxId> txids;
    for (size_t i = 1; i < block.Vtx.size(); i++)
      txids.push_back(validation.TxIds[i]);
    Client_->blockConnected(*it, block, txids);
  }
}

// The waiting transactions stay: they are tried against the new base right after
void CMempool::reset(const char *why)
{
  LOG_F(WARNING, "mempool: reset (%s): %zu txs dropped", why, Entries_.size());
  Stats_.Resets.fetch_add(1, std::memory_order_relaxed);

  replaceGeneration(new CGeneration(MinGenerationCapacity));
  Entries_.clear();
  Spent_.clear();
  Bytes_ = 0;
  Fees_ = 0;
}

// Core's 336 hours by acceptance time: the generation lists entries in that order
void CMempool::expire()
{
  const int64_t cutoff = time(nullptr) - Expiry;
  std::vector<TxId> expired;
  for (size_t i = 0; i < Generation_->used(); i++) {
    const CRecord &record = Generation_->record(i);
    if (record.Body->Time >= cutoff)
      break;
    if (record.Dead.load(std::memory_order_relaxed) == RevisionInfinity)
      expired.push_back(record.Body->TxId);
  }

  for (const TxId &txid: expired) {
    size_t before = Entries_.size();
    eraseWithDescendants(txid);
    Stats_.Expired.fetch_add(before - Entries_.size(), std::memory_order_relaxed);
  }
}

// One-shot timer for the client's nearest deadline, only while it has one
void CMempool::armTimer()
{
  int64_t deadline = Client_ ? Client_->deadline() : 0;
  if (!deadline || deadline == TimerDeadline_)
    return;
  TimerDeadline_ = deadline;
  int64_t delay = std::max<int64_t>(deadline - steadySeconds(), 1);
  userEventStartTimer(TimerEvent_, delay * 1000000, 1);
}

// Publication

void CMempool::publish()
{
  CView *view = new CView;
  view->Retired = &RetiredViews_;
  view->Generation = Generation_;
  Generation_->Refs++;
  view->Revision = ++Revision_;
  view->RecordEnd = Generation_->used();
  view->BaseHash = BaseIndex_->Header.GetHash();
  view->BaseHeight = BaseIndex_->Height;
  view->TxCount = Entries_.size();
  view->Bytes = Bytes_;
  view->Fees = Fees_;
  // No script checker yet (§5.5): everything accepted is Unverified
  view->Unverified = Entries_.size();
  view->PublishedTime = time(nullptr);
  Current_.reset(view);
  Dirty_ = false;
}

// A fresh generation with the live records in their order
void CMempool::compact(size_t capacity)
{
  CGeneration *next = new CGeneration(std::max({capacity, Entries_.size() + 1, MinGenerationCapacity}));
  for (size_t i = 0; i < Generation_->used(); i++) {
    const CRecord &record = Generation_->record(i);
    if (record.Dead.load(std::memory_order_relaxed) == RevisionInfinity)
      next->put(record.Body, 0, record.Flags);
  }
  replaceGeneration(next);
}

// Views of the old generation keep it until they are gone
void CMempool::replaceGeneration(CGeneration *next)
{
  release(Generation_);
  Generation_ = next;
  Dirty_ = true;
}

// Views whose last reader left: each held its generation
void CMempool::recycle()
{
  for (CView *view = RetiredViews_.take(); view; ) {
    CView *next = view->Next;
    release(view->Generation);
    delete view;
    view = next;
  }
}

}
}
