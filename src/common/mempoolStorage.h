// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// What readers of the mempool see (mempool-impl-plan.md §8). A generation is an append-only array
// of records in insertion order plus a map from txid to the newest record of that txid; a record
// is visible to a view whose revision falls into [Born, Dead). A reader pins a view once per
// request and walks the generation it names. The writer never moves a record a view can see:
// compaction copies the live records into a fresh generation in their order, and the old one is
// freed after its last view. Everything here is created and freed on the writer thread only.

#include "BC/bc.h"
#include "common/intrusive_ptr.h"
#include "dbengine/swmrhashmap.h"
#include <atomic>
#include <cstring>
#include <memory>

namespace BC {
namespace Mempool {

// Any thread pushes, the writer takes the whole list
template<typename T>
class CReturnList {
public:
  void push(T *node) {
    T *head = Head_.load(std::memory_order_relaxed);
    do {
      node->Next = head;
    } while (!Head_.compare_exchange_weak(head, node, std::memory_order_release, std::memory_order_relaxed));
  }

  T *take() { return Head_.exchange(nullptr, std::memory_order_acquire); }

private:
  std::atomic<T*> Head_ = nullptr;
};

// The low bits of a txid are chosen by whoever builds the transaction: a salt keeps a flood of
// ground txids from sharing one probe chain. Mixed already, so ankerl maps take it as it is
struct CTxIdHash {
  using is_avalanching = void;
  static inline uint64_t Salt = 0;
  size_t operator()(const BC::Proto::TxHashTy &txid) const {
    uint64_t h = (txid.get64(0) ^ Salt) * 0x9E3779B97F4A7C15ULL;
    return static_cast<size_t>(h ^ (h >> 29));
  }
};

enum ERecordFlags : uint8_t {
  // Own scripts checked under the policy flags; nothing sets it until the checker exists (§5.5)
  ERecordVerified = 1,
  // Verified, and so is every unconfirmed ancestor: what may be relayed, served and mined
  ERecordEligible = 2
};

// One accepted transaction, immutable once published. Every generation listing it holds a
// reference; the count is the writer's alone, readers never touch it
struct CTxBody {
  uint32_t Refs = 0;
  uint32_t Size = 0;
  uint32_t VSize = 0;
  uint32_t SigOpsCost = 0;
  int64_t Fee = 0;
  // Seconds, when the transaction entered the mempool
  int64_t Time = 0;
  // The writer's insertion counter: the order HTTP pages by
  uint64_t Sequence = 0;
  BC::Proto::TxHashTy TxId;
  BC::Proto::TxHashTy WTxId;
  // One allocation from unpack2
  BC::Proto::CTransaction *Tx = nullptr;

  ~CTxBody() { operator delete(Tx); }

  void release() {
    if (--Refs == 0)
      delete this;
  }
};

static constexpr uint64_t RevisionInfinity = UINT64_MAX;

struct CRecord {
  CTxBody *Body = nullptr;
  uint64_t Born = 0;
  std::atomic<uint64_t> Dead = RevisionInfinity;
  // One-based reference of the earlier incarnation of the same txid in this generation
  uint32_t Previous = 0;
  uint8_t Flags = 0;
};

class CGeneration {
public:
  explicit CGeneration(size_t capacity) :
    Map_(capacity * 2), Records_(new CRecord[capacity]), Capacity_(capacity) {}

  ~CGeneration() {
    for (size_t i = 0; i < Used_; i++)
      Records_[i].Body->release();
  }

  // Reader side
  const CRecord *find(const BC::Proto::TxHashTy &txid, uint64_t revision) const {
    uint32_t ref = Map_.find(txid);
    while (ref) {
      const CRecord &record = Records_[ref - 1];
      if (record.Born <= revision)
        return revision < record.Dead.load(std::memory_order_acquire) ? &record : nullptr;
      ref = record.Previous;
    }
    return nullptr;
  }

  const CRecord &record(size_t index) const { return Records_[index]; }

  // Writer side
  size_t capacity() const { return Capacity_; }
  size_t used() const { return Used_; }
  bool full() const { return Used_ == Capacity_; }

  // Takes a reference to the body
  void put(CTxBody *body, uint64_t revision, uint8_t flags) {
    body->Refs++;
    CRecord &record = Records_[Used_];
    record.Body = body;
    record.Born = revision;
    record.Flags = flags;
    // Born and the body go out with the map's release store, Previous before it
    Map_.updateWith(body->TxId, [&](uint32_t previous) {
      record.Previous = previous;
      return static_cast<uint32_t>(++Used_);
    });
  }

  void erase(const BC::Proto::TxHashTy &txid, uint64_t revision) {
    uint32_t ref = Map_.find(txid);
    if (ref)
      Records_[ref - 1].Dead.store(revision, std::memory_order_release);
  }

  // Generation lifetime: the writer and every view on it hold one reference
  uint32_t Refs = 1;

private:
  dbengine::CSwmrHashMap<BC::Proto::TxHashTy, CTxIdHash> Map_;
  std::unique_ptr<CRecord[]> Records_;
  size_t Capacity_ = 0;
  size_t Used_ = 0;
};

// A publication: the base the mempool is consistent with, the totals of its transactions and the
// bounds that make a walk of the generation show exactly this state
struct alignas(512) CView {
  // 512: atomic_intrusive_ptr caches references in the low 9 pointer bits
  mutable std::atomic<uintptr_t> Refs_{0};
  uintptr_t ref_fetch_add(uintptr_t n) const { return Refs_.fetch_add(n, std::memory_order_relaxed); }
  uintptr_t ref_fetch_sub(uintptr_t n) const { return Refs_.fetch_sub(n, std::memory_order_acq_rel); }

  CView *Next = nullptr;
  // Where the last reference sends the view: the writer frees it, not the reader
  CReturnList<CView> *Retired = nullptr;

  CGeneration *Generation = nullptr;
  uint64_t Revision = 0;
  size_t RecordEnd = 0;

  BC::Proto::BlockHashTy BaseHash;
  uint32_t BaseHeight = 0;
  uint64_t TxCount = 0;
  uint64_t Bytes = 0;
  int64_t Fees = 0;
  uint64_t Unverified = 0;
  int64_t PublishedTime = 0;

  bool visible(const CRecord &record) const {
    return record.Born <= Revision && Revision < record.Dead.load(std::memory_order_acquire);
  }

  const CRecord *find(const BC::Proto::TxHashTy &txid) const { return Generation ? Generation->find(txid, Revision) : nullptr; }

  // The first record with a sequence above 'sequence', or RecordEnd: insertion order is the
  // record order, so a page cursor is found by bisection
  size_t upperBound(uint64_t sequence) const {
    size_t begin = 0;
    size_t end = RecordEnd;
    while (begin < end) {
      size_t middle = begin + (end - begin) / 2;
      if (Generation->record(middle).Body->Sequence <= sequence)
        begin = middle + 1;
      else
        end = middle;
    }
    return begin;
  }
};

struct CViewDeleter {
  void operator()(CView *view) const { view->Retired->push(view); }
};

using CViewRef = intrusive_ptr<CView, CViewDeleter>;

}
}
