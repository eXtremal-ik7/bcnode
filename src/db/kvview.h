// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// KV window engine on published views. Rationale and proofs:
// kv-view-migration-plan.md
//
// A writer fills a window set of its own - anywhere, including prepare, off the
// serial thread - and attaches it as one unit of connect (a batch, a run, a
// single live block). That makes it one revision: windows plus a RocksDB
// snapshot per shard, installed with one atomic swap. Readers pin a revision,
// walk it newest to oldest, then read the disk at its snapshot. The flusher
// folds unflushed windows into one batch per shard.
//
// No window is ever reset or reused, so a reader chasing an arena pointer can
// no longer meet a writer rewinding that arena. Execution and validation never
// look up an unpublished write-set; CKvWriter itself may still fold repeated
// operations on one key inside its private map.
//
// The shards are not owned here: the database class keeps initialize/stamps/
// rebaseChain and hands over opened rocksdb::DB*. So the shutdown order is the
// owner's - stop admitting work, attach the last unit, flushAll(), stop serving
// reads, shutdown(), close the shards - asserted here, and shutdown() waits out
// the reads already inside a call, whose revisions still hold snapshots.

#include "common/blockDataBase.h"
#include "common/intrusive_ptr.h"
#include "common/mlog.h"
#include "db/keyHash.h"
#include "swmrhashmap.h"

#include <p2putils/strExtras.h>
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace BC {
namespace DB {

// Arena record of a write: payload follows the header. The key is not stored
// here - it lives in the map slot, and the fold reads it there
struct CKvHeader {
  // Top bit of the length: something below this window (an older segment or the
  // disk) may hold this key, so a later erase must tombstone instead of drop
  static constexpr uint32_t MayExistBelowFlag = 0x80000000u;
  uint32_t SizeAndFlag;

  uint32_t size() const { return SizeAndFlag & ~MayExistBelowFlag; }
  bool mayExistBelow() const { return (SizeAndFlag & MayExistBelowFlag) != 0; }
};

// A delete allocates no arena record: the map value is one of these markers,
// told apart by identity. Tombstone kills a key that may lie below, BornDead a
// key born in this same window - the fold drops that pair whole. Shared by all
// instantiations because the fold compares markers across segments
inline CKvHeader KvTombstoneMarker;
inline CKvHeader KvBornDeadMarker;

inline bool isKvMarker(const void *entry) {
  return entry == &KvTombstoneMarker || entry == &KvBornDeadMarker;
}

// One window, two lives: the writer fills it, seal freezes it and fills in the
// fields below. Never reset, never reused - it dies whole, when the last view
// or reader lets go of it
template<typename CKey>
struct CWindow {
  MLog Arena;
  CSwmrHashMap<CKey> Map;

  // Delete markers have no arena footprint, but their map slot is memory all
  // the same: charged here so backpressure sees a delete-heavy window
  size_t PhantomBytes = 0;

  CWindow(size_t arenaBytes, size_t mapCapacity) : Arena(arenaBytes), Map(mapCapacity) {}

  size_t windowSize() { return Arena.size() + PhantomBytes; }

  // From seal on. intrusive_ptr contract first, then the tip this window ends
  // at and its frozen size (MLog::size() is not const)
  mutable std::atomic<uintptr_t> Refs_{0};
  uintptr_t ref_fetch_add(uintptr_t n) const { return Refs_.fetch_add(n, std::memory_order_relaxed); }
  uintptr_t ref_fetch_sub(uintptr_t n) const { return Refs_.fetch_sub(n, std::memory_order_acq_rel); }

  BC::Proto::BlockHashTy Stamp;
  size_t Bytes = 0;

  // Place in the attach order, one number per unit. Durability is a watermark
  // over these, never a flag per window - see CKvEngine::DurableThrough_
  uint64_t Seq = 0;
};

// Shards are fixed when the database is created, so the per-revision array is
// too. Windows per shard have no such bound on purpose: a connected unit has
// nowhere else to go, so that limit is policy above (isPipelineFull)
static constexpr size_t MaxShards = 16;

// Beware when tuning: this floor fires before the byte one when units are
// small, and the batch then carries too little to fold. 4 against 32 cost 10%
// of BTC reindex (2026-08-05, /ram/bcnodebtc-bench)
static constexpr size_t DefaultFlushSegments = 4;

// The disk half of a revision: a snapshot and how much of the attach order is
// inside it, published as ONE object by the flusher right after its write. That
// is what makes the split exact - a window is either above DurableSeq, and then
// nowhere in the snapshot, or at or below it, and then wholly in it. Two
// separate publications cannot do that, and a window counted twice is fatal to
// the families that fold (merge sums it, array appends it) even though KV
// shrugs it off by shadowing the disk with the same value
struct alignas(512) CDiskState {
  // 512: atomic_intrusive_ptr caches references in the low 9 pointer bits
  rocksdb::DB *Db = nullptr;
  const rocksdb::Snapshot *Snapshot = nullptr;
  uint64_t DurableSeq = 0;

  mutable std::atomic<uintptr_t> Refs_{0};
  uintptr_t ref_fetch_add(uintptr_t n) const { return Refs_.fetch_add(n, std::memory_order_relaxed); }
  uintptr_t ref_fetch_sub(uintptr_t n) const { return Refs_.fetch_sub(n, std::memory_order_acq_rel); }

  CDiskState(rocksdb::DB *db, uint64_t durableSeq) : Db(db), Snapshot(db->GetSnapshot()), DurableSeq(durableSeq) {}
  ~CDiskState() { Db->ReleaseSnapshot(Snapshot); }
};

// One published revision: what is in memory and what to read from the disk, per
// shard. Immutable once published - the writer builds the next one and swaps
template<typename CKey>
struct alignas(512) CKvView {
  // 512 above: atomic_intrusive_ptr caches references in the low 9 pointer bits
  mutable std::atomic<uintptr_t> Refs_{0};
  uintptr_t ref_fetch_add(uintptr_t n) const { return Refs_.fetch_add(n, std::memory_order_relaxed); }
  uintptr_t ref_fetch_sub(uintptr_t n) const { return Refs_.fetch_sub(n, std::memory_order_acq_rel); }

  // The engine's live-revision counter: shutdown waits for it, or a snapshot
  // would be released into a shard the owner has already closed. Declared
  // before Shards and Pending - members die in reverse order, so the death is
  // announced only after every reference the view held is gone
  struct SLiveTicket {
    std::atomic<size_t> *Live = nullptr;
    ~SLiveTicket() {
      if (Live) {
        Live->fetch_sub(1, std::memory_order_release);
        Live->notify_all();
      }
    }
  } Ticket;

  struct CShardView {
    // The disk state this revision reads, kept alive by it
    intrusive_ptr<const CDiskState> Disk;
    // window of Pending below, oldest..newest; a lookup walks it backwards
    size_t First = 0;
    size_t Count = 0;
  };

  // Inline: a lookup reaches its shard straight from the pinned view
  std::array<CShardView, MaxShards> Shards;

  // Every shard's windows, shard by shard, sized to what is there: one
  // allocation per revision, and no cap to run into
  std::vector<intrusive_ptr<const CWindow<CKey>>> Pending;
};

// Reader pin: one atomic on the published slot, held while the caller looks at
// anything it found. Every read takes one, so "forgot the guard" is a compile
// error instead of a race
template<typename CKey>
class CKvGuard {
public:
  CKvGuard() = default;
  explicit CKvGuard(atomic_intrusive_ptr<CKvView<CKey>> &current) : View_(current) {}
  const CKvView<CKey> *view() const { return View_.get(); }

private:
  intrusive_ptr<CKvView<CKey>> View_;
};

template<typename CKey> class CKvEngine;

// Everything one unit of connect writes, filled anywhere - including prepare,
// off the serial thread - and handed to the engine whole. Sizes are exact: the
// unit knows its record count, so nothing has to grow. Writes reach the windows
// with no engine in the way, this being the hottest path there is
template<typename CKey>
class CKvWriter {
public:
  CKvWriter(size_t shardsNum, size_t arenaBytes, size_t mapCapacity) : ShardsNum_(shardsNum) {
    for (size_t i = 0; i < ShardsNum_; i++)
      Windows_[i] = new CWindow<CKey>(arenaBytes, mapCapacity);
  }

  // Whatever the engine did not take: the run was cut before it connected
  ~CKvWriter() {
    for (CWindow<CKey> *window: Windows_)
      delete window;
  }

  CKvWriter(CKvWriter &&other) : Windows_(other.Windows_), ShardsNum_(other.ShardsNum_) {
    other.Windows_.fill(nullptr);
  }

  CKvWriter(const CKvWriter&) = delete;
  CKvWriter &operator=(const CKvWriter&) = delete;

  size_t shardsNum() const { return ShardsNum_; }

  // Nothing written yet: attach would make no revision of such a set, so the
  // caller keeps filling this one instead of building the next
  bool empty() { return maxWindowSize() == 0; }

  // What this set turned out to need, for sizing the next one: MLog growth
  // copies the prefix and the map rehashes, while the units of one chain are
  // all alike
  size_t maxWindowSize() {
    size_t max = 0;
    for (size_t i = 0; i < ShardsNum_; i++)
      max = std::max(max, Windows_[i]->windowSize());
    return max;
  }

  size_t maxUsed() const {
    size_t max = 0;
    for (size_t i = 0; i < ShardsNum_; i++)
      max = std::max(max, Windows_[i]->Map.used());
    return max;
  }

  // Nothing below can hold this key: a brand new output, a transaction of a
  // block connected for the first time
  void putNew(const CKey &key, const void *data, size_t size, const void *suffix = nullptr, size_t suffixSize = 0) {
    put(key, data, size, suffix, suffixSize, false);
  }

  // May land on an existing value: an output restored by a disconnect, a BIP30
  // coinbase repeating an earlier one. Conservative - a false alarm costs one
  // Put plus one Delete instead of nothing
  void putRestore(const CKey &key, const void *data, size_t size, const void *suffix = nullptr, size_t suffixSize = 0) {
    put(key, data, size, suffix, suffixSize, true);
  }

  void erase(const CKey &key) {
    const size_t hash = std::hash<CKey>()(key);
    CWindow<CKey> &window = *Windows_[fastrange(hash, ShardsNum_)];

    // no arena record: charge the map slot to the window
    window.PhantomBytes += sizeof(CKey) + 2 * sizeof(void*);

    // One walk picks the marker and writes it; find() first would walk twice
    window.Map.updateWith(key, hash, [](const void *prev) -> void* {
      if (prev == &KvBornDeadMarker)
        return &KvBornDeadMarker;
      // Born in this window: the pair dies whole. A value that admits something
      // below it forfeits that
      const CKvHeader *header = prev && !isKvMarker(prev) ? static_cast<const CKvHeader*>(prev) : nullptr;
      return header && !header->mayExistBelow() ? &KvBornDeadMarker : &KvTombstoneMarker;
    });
  }

  // Window write for the value families the engine does not interpret: merge
  // folds a delta into the key, array appends to its tail. One hash per
  // operation, as in put/erase - the caller computes it once and passes it on
  size_t hashOf(const CKey &key) const { return std::hash<CKey>()(key); }
  const void *findOwn(const CKey &key, size_t hash) const { return windowFor(hash).Map.find(key, hash); }
  void *alloc(size_t hash, size_t bytes) { return windowFor(hash).Arena.alloc(bytes); }
  void update(const CKey &key, size_t hash, void *entry) { windowFor(hash).Map.update(key, hash, entry); }

  // Records with no arena footprint still occupy a map slot: charged so
  // backpressure and the flush floors see them
  void chargePhantom(size_t hash, size_t bytes) { windowFor(hash).PhantomBytes += bytes; }

private:
  friend class CKvEngine<CKey>;

  CWindow<CKey> *window(size_t i) const { return Windows_[i]; }
  CWindow<CKey> &windowFor(size_t hash) const { return *Windows_[fastrange(hash, ShardsNum_)]; }

  // Handed to the engine: the writer stops owning it
  CWindow<CKey> *release(size_t i) {
    CWindow<CKey> *window = Windows_[i];
    Windows_[i] = nullptr;
    return window;
  }

  // Two pieces glued straight in the arena: a "payload plus a few bytes of
  // metadata" caller would otherwise stage and copy the record twice
  void put(const CKey &key, const void *data, size_t size, const void *suffix, size_t suffixSize, bool mayExistBelow) {
    // One hash per operation: shard from its high bits, map slot from its low
    const size_t hash = std::hash<CKey>()(key);
    CWindow<CKey> &window = *Windows_[fastrange(hash, ShardsNum_)];

    // record rounded up so every header lands 4-aligned in the arena
    const size_t total = size + suffixSize;
    CKvHeader *header = static_cast<CKvHeader*>(window.Arena.alloc((sizeof(CKvHeader) + total + 3) & ~static_cast<size_t>(3)));
    header->SizeAndFlag = static_cast<uint32_t>(total) | (mayExistBelow ? CKvHeader::MayExistBelowFlag : 0);
    uint8_t *value = reinterpret_cast<uint8_t*>(header + 1);
    memcpy(value, data, size);
    if (suffixSize)
      memcpy(value + size, suffix, suffixSize);

    // The flag is monotonic inside a window: what the replaced value knew about
    // the layers below cannot be taken back. A tombstone knows it too - it is
    // only ever written for a key that may exist below
    window.Map.updateWith(key, hash, [header](const void *prev) -> void* {
      if (prev == &KvTombstoneMarker ||
          (prev && !isKvMarker(prev) && static_cast<const CKvHeader*>(prev)->mayExistBelow()))
        header->SizeAndFlag |= CKvHeader::MayExistBelowFlag;
      return header;
    });
  }

  std::array<CWindow<CKey>*, MaxShards> Windows_{};
  size_t ShardsNum_;
};

// How a shard's windows become rocksdb rows: the family's business, not the
// engine's. KV replaces the key, merge sums deltas into it, array concatenates
// tails - all the engine knows is which windows are still owed to the disk
template<typename CKey>
class IKvSegmentWriter {
public:
  virtual ~IKvSegmentWriter() {}
  virtual void writeSegments(rocksdb::DB *db,
                             size_t shardIndex,
                             const CWindow<CKey> *const *segments,
                             size_t count,
                             const BC::Proto::BlockHashTy &stamp) = 0;
};

template<typename CKey>
class CKvEngine {
public:
  struct CConfig {
    std::string Name;

    // Floors, per shard, heirs of flushLogSizeMb: while both are unmet the
    // flusher waits, so short-lived keys fold in memory instead of reaching the
    // disk (0 = write on every wakeup). Bytes bound memory, windows the lookup
    // depth - a read walks every window of its shard before the disk.
    //
    // Admission stops at twice the floor, so the writer can build the next
    // batch while the flusher writes this one. A floor of 0 throttles on
    // nothing: that dimension is switched off, not set to zero
    size_t FlushBytesLower = 1u << 24;
    size_t FlushSegmentsLower = DefaultFlushSegments;
  };

  ~CKvEngine() { shutdown(); }

  bool initialize(const CConfig &cfg, const std::vector<rocksdb::DB*> &shards, IKvSegmentWriter<CKey> *segmentWriter) {
    SegmentWriter_ = segmentWriter;
    if (shards.size() > MaxShards) {
      LOG_F(ERROR, "%s: %zu shards configured, %zu is the maximum", cfg.Name.c_str(), shards.size(), MaxShards);
      return false;
    }

    // Reopening is a real path (rebaseChain, reindex): a leftover stop bit
    // would kill the new flusher, leftover sequences would sit above the fresh
    // watermarks and hide every window from it
    assert(!FlushThread_.joinable());
    assert(LiveViews_.load(std::memory_order_relaxed) == 0);
    Published_.store(0, std::memory_order_relaxed);
    Flushed_.store(0, std::memory_order_relaxed);
    DrainRequests_.store(0, std::memory_order_relaxed);
    NextSeq_ = 1;
    for (size_t i = 0; i < MaxShards; i++) {
      DurableThrough_[i].store(0, std::memory_order_relaxed);
      PendingSegments_[i].store(0, std::memory_order_relaxed);
      PendingBytes_[i].store(0, std::memory_order_relaxed);
      Disk_[i].reset(nullptr);
    }

    Name_ = cfg.Name;
    ShardsNum_ = shards.size();
    FlushBytesLower_ = cfg.FlushBytesLower;
    FlushSegmentsLower_ = cfg.FlushSegmentsLower;

    // Derived, never configured apart: a ceiling below its floor would stop
    // admission before the flusher ever fires, and both sides would wait for
    // each other until the storage timer
    PendingBytesLimit_ = FlushBytesLower_ ? 2 * FlushBytesLower_ : SIZE_MAX;
    PendingSegmentsLimit_ = FlushSegmentsLower_ ? 2 * FlushSegmentsLower_ : SIZE_MAX;

    // The first view: nothing in memory, everything on the disk
    CKvView<CKey> *view = newView();
    for (size_t i = 0; i < ShardsNum_; i++) {
      Disk_[i].reset(new CDiskState(shards[i], 0));
      view->Shards[i].Disk = intrusive_ptr<const CDiskState>(Disk_[i]);
    }
    Current_.reset(view);

    FlushThread_ = std::thread([this]() { flushLoop(); });
    return true;
  }

  // Stops the flusher and drops the published view. A writer set that was never
  // attached is lost unless flushAll() ran first - same contract as today
  void shutdown() {
    if (!FlushThread_.joinable())
      return;
    Published_.fetch_or(StopBit, std::memory_order_release);
    Published_.notify_all();
    FlushThread_.join();

    // Dropping our reference is not enough: a reader inside a call holds a
    // revision - not necessarily the last - and its snapshot would be released
    // through a dead DB. Ends because the empty slots hand out no more
    Current_.reset(nullptr);
    for (size_t i = 0; i < ShardsNum_; i++)
      Disk_[i].reset(nullptr);
    for (size_t live = LiveViews_.load(std::memory_order_acquire); live; live = LiveViews_.load(std::memory_order_acquire))
      LiveViews_.wait(live, std::memory_order_acquire);
  }

  // Writer side ---------------------------------------------------------

  // The only throttle, and it lives outside: the pipeline asks before starting
  // work and stops admitting units on yes. The engine never defends itself, so
  // a caller that does not ask grows memory here until the machine dies.
  //
  // Two plain loads per shard - no guard, no RMW on the slot every reader pins,
  // no walk of the view. A probe, not a reservation: units already in
  // preparation still attach and push both limits past themselves, which costs
  // memory and lookup depth until the flusher catches up, nothing more
  bool isPipelineFull() const {
    assert(!stopped());
    for (size_t i = 0; i < ShardsNum_; i++) {
      if (PendingSegments_[i].load(std::memory_order_relaxed) >= PendingSegmentsLimit_ ||
          PendingBytes_[i].load(std::memory_order_relaxed) >= PendingBytesLimit_)
        return true;
    }

    return false;
  }

  // Sugar: a writer already matched to this database - the shard count must be
  // the same one the lookups divide by. Not static for exactly that reason
  CKvWriter<CKey> newWriter(size_t arenaBytes = 1u << 20, size_t mapCapacity = 4096) const {
    return CKvWriter<CKey>(ShardsNum_, arenaBytes, mapCapacity);
  }

  size_t shardsNum() const { return ShardsNum_; }

  // The whole writer side. One unit of connect - a batch, a run, a single live
  // block - arrives as the windows it filled and becomes one revision, all
  // shards in one swap. Valid because a run is atomic already: its own
  // preprocessing annihilates pairs only while the run connects whole, and a
  // run that fails to connect is cut before it gets here.
  //
  // Never refuses, and never has to: by the time a unit is connected it is too
  // late to say no, and a published window cannot be merged into. Pressure
  // belongs where work is admitted - isPipelineFull() - not where it lands.
  //
  // NOT thread-safe: all attach() calls for one engine must be serialized by
  // one external mutator. The writer may be built on another thread, but that
  // work must be complete and handed off with happens-before before this call;
  // no put/erase may overlap it. Readers and the flusher may run concurrently.
  void attach(CKvWriter<CKey> &writer, const BC::Proto::BlockHashTy &stamp) {
    assert(writer.shardsNum() == ShardsNum_);
    // Nothing would write this unit, and the caller would think it published
    assert(!stopped());

    bool wrote = false;
    for (size_t i = 0; i < ShardsNum_; i++)
      wrote |= writer.window(i)->windowSize() != 0;

    // A unit that wrote nothing: no revision, no snapshots, and the writer is
    // untouched, so the caller can keep filling it
    if (!wrote)
      return;

    // The linearization point: one atomic swap by a single mutator - no CAS
    // loop, no ABA, no lock. Everyone sees the new revision from here on, the
    // old one dies with the last guard holding it
    Current_.reset(createView(writer, stamp));

    // The ticket after the revision, always: whoever acquires this bump sees
    // the view too, which is what keeps the flusher's wait free of lost wakeups
    Published_.fetch_add(1, std::memory_order_release);
    Published_.notify_all();
  }

  // Checkpoint and shutdown: everything attached reaches the disk, then every
  // shard - including those that never saw a write - gets the stamp, which
  // initialize() rejects the database for if the shards disagree. A set the
  // caller has not attached is not the engine's business and is not written
  void flushAll(const BC::Proto::BlockHashTy &stamp) {
    assert(!stopped());
    if (stamp.isNull())
      return;

    // The one place that waits, by its own contract. The flusher is idle
    // afterwards, so the stamps below need no handoff
    drain();

    CKvGuard<CKey> guard = this->guard();
    rocksdb::WriteOptions writeOptions;
    writeOptions.disableWAL = true;
    for (size_t i = 0; i < ShardsNum_; i++) {
      rocksdb::WriteBatch batch;
      batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(stamp.begin()), sizeof(BC::Proto::BlockHashTy)));
      guard.view()->Shards[i].Disk.get()->Db->Write(writeOptions, &batch);
    }
  }

  // Reader side ---------------------------------------------------------

  // Null after shutdown: reads stop before it by contract, and the assert is
  // where a caller that ignored the order finds out
  CKvGuard<CKey> guard() const {
    assert(Current_.get());
    return CKvGuard<CKey>(Current_);
  }

  // The pinned view and nothing else: segments newest to oldest, then the disk
  // at its snapshot. Builders are never searched - see the note at the top
  template<typename F>
  bool find(const CKvGuard<CKey> &guard, const CKey &key, F &&callback) const {
    const size_t hash = std::hash<CKey>()(key);
    return lookup(guard, fastrange(hash, ShardsNum_), key, hash, callback);
  }

  // One shard of the pinned revision: its windows oldest to newest, and the disk
  // they sit on. KV stops at the first hit; merge and array have to fold every
  // layer, so they walk this themselves
  struct CLayers {
    rocksdb::DB *Db = nullptr;
    const rocksdb::Snapshot *Snapshot = nullptr;
    const intrusive_ptr<const CWindow<CKey>> *Windows = nullptr;
    size_t Count = 0;
  };

  CLayers layers(const CKvGuard<CKey> &guard, size_t shardIndex) const {
    const auto &shard = guard.view()->Shards[shardIndex];
    return CLayers{shard.Disk.get()->Db, shard.Disk.get()->Snapshot, guard.view()->Pending.data() + shard.First, shard.Count};
  }

  size_t shardOf(const CKey &key) const { return fastrange(std::hash<CKey>()(key), ShardsNum_); }

private:
  bool stopped() const { return (Published_.load(std::memory_order_relaxed) & StopBit) != 0; }

  // Counted before it exists: shutdown waits only for what is already counted
  CKvView<CKey> *newView() {
    LiveViews_.fetch_add(1, std::memory_order_relaxed);
    CKvView<CKey> *view = new CKvView<CKey>();
    view->Ticket.Live = &LiveViews_;
    return view;
  }

  template<typename F>
  bool lookup(const CKvGuard<CKey> &guard, size_t shardIndex, const CKey &key, size_t hash, F &&callback) const {
    // Newest segment first, and the walk stops on the first hit including a
    // marker: otherwise a deleted key resurrects from a layer below
    const auto &shard = guard.view()->Shards[shardIndex];
    for (size_t i = shard.Count; i-- > 0; ) {
      if (const void *entry = guard.view()->Pending[shard.First + i].get()->Map.find(key, hash))
        return deliver(entry, callback);
    }

    // The disk at the revision of this view: pending and snapshot are one pair
    rocksdb::ReadOptions options;
    options.snapshot = shard.Disk.get()->Snapshot;
    rocksdb::Slice keySlice(reinterpret_cast<const char*>(&key), sizeof(CKey));
    std::string value;
    if (shard.Disk.get()->Db->Get(options, keySlice, &value).ok()) {
      callback(value.data(), value.size());
      return true;
    }

    return false;
  }

  template<typename F>
  static bool deliver(const void *entry, F &&callback) {
    if (isKvMarker(entry))
      return false;
    const CKvHeader *header = static_cast<const CKvHeader*>(entry);
    callback(header + 1, header->size());
    return true;
  }

  // The next revision, built from the current one by reading it plainly: this
  // thread is its only mutator, and the flusher never touches it
  CKvView<CKey> *createView(CKvWriter<CKey> &writer, const BC::Proto::BlockHashTy &stamp) {
    const CKvView<CKey> *prev = Current_.get();
    CKvView<CKey> *next = newView();
    const uint64_t seq = NextSeq_++;

    // Upper bound: everything the previous revision holds plus one window per
    // shard. One allocation, and no reallocation while the shards are filled
    next->Pending.reserve(prev->Pending.size() + ShardsNum_);

    for (size_t i = 0; i < ShardsNum_; i++) {
      typename CKvView<CKey>::CShardView &dst = next->Shards[i];
      const typename CKvView<CKey>::CShardView &src = prev->Shards[i];
      dst.First = next->Pending.size();

      // Disk and watermark in one object, one load: written windows leave the
      // view exactly when their data enters the snapshot that replaces them.
      // Never a flag per window - flags are observed one at a time, and
      // dropping a newer window while keeping an older one leaves the older
      // shadowing the value the fold just replaced (§2.3)
      dst.Disk = intrusive_ptr<const CDiskState>(Disk_[i]);
      for (size_t k = 0; k < src.Count; k++) {
        if (prev->Pending[src.First + k].get()->Seq > dst.Disk.get()->DurableSeq)
          next->Pending.push_back(prev->Pending[src.First + k]);
      }

      // Then the unit's own window, frozen as it goes in. An empty one is not a
      // revision: that shard keeps just the survivors
      CWindow<CKey> *window = writer.release(i);
      if (window->windowSize()) {
        window->Stamp = stamp;
        window->Bytes = window->windowSize();
        window->Seq = seq;
        // Charged before the swap below publishes it: the flusher subtracts
        // only what it found in a view, so the counters cannot go negative
        PendingSegments_[i].fetch_add(1, std::memory_order_relaxed);
        PendingBytes_[i].fetch_add(window->Bytes, std::memory_order_relaxed);
        next->Pending.push_back(intrusive_ptr<const CWindow<CKey>>(window));
      } else {
        delete window;
      }

      dst.Count = next->Pending.size() - dst.First;
    }

    return next;
  }

  // Flusher side --------------------------------------------------------

  // Does the view still owe the disk anything, floors aside. The watermark
  // makes this one read per shard: segments carry increasing sequences, so the
  // newest one being durable means all of them are
  bool hasWork(const CKvView<CKey> *view) const {
    for (size_t i = 0; i < ShardsNum_; i++) {
      const auto &shard = view->Shards[i];
      if (shard.Count &&
          view->Pending[shard.First + shard.Count - 1].get()->Seq > DurableThrough_[i].load(std::memory_order_acquire))
        return true;
    }
    return false;
  }

  void flushLoop() {
    loguru::set_thread_name((Name_ + ".flush").c_str());
    uint64_t drainServed = 0;

    for (;;) {
      // Words before the view they announce, always: a unit this pass misses
      // has already moved the word, so the wait below returns instead of
      // blocking. The whole handshake - they say "look again", nothing more
      const uint64_t published = Published_.load(std::memory_order_acquire);
      const uint64_t drainRequested = DrainRequests_.load(std::memory_order_acquire);

      // Neither a drain nor a stop may wait for data that is not coming
      const bool forced = drainRequested != drainServed || (published & StopBit);

      {
        CKvGuard<CKey> guard = this->guard();
        if (flushPending(guard.view(), forced)) {
          drainServed = drainRequested;
          continue;
        }
      }

      // Nothing written: nothing owed, or every shard below its floor. A forced
      // pass reaching here has written all there was
      drainServed = drainRequested;

      // stop, and everything published is on the disk
      if (published & StopBit)
        return;

      Published_.wait(published, std::memory_order_acquire);
    }
  }

  // Take-all per shard, once the shard is worth a batch: a pass costs the same
  // sort and the same WriteBatch whatever it carries, and a pair folded in
  // memory is a record RocksDB never writes and never compacts. Above the floor
  // the batch size stays emergent - as far ahead as the writer got
  bool flushPending(const CKvView<CKey> *view, bool forced) {
    bool flushed = false;

    for (size_t i = 0; i < ShardsNum_; i++) {
      // reused across passes: one thread lives here and keeps its capacity
      Segments_.clear();
      size_t bytes = 0;
      const size_t count = unflushed(view, i, Segments_, bytes);
      if (!count)
        continue;

      if (!forced && bytes < FlushBytesLower_ && count < FlushSegmentsLower_) {
        // TODO: sort these windows instead of idling - a window is immutable
        // from attach on, so the flush then merges k ready runs
        continue;
      }

      rocksdb::DB *db = view->Shards[i].Disk.get()->Db;
      SegmentWriter_->writeSegments(db, i, Segments_.data(), count, Segments_.back()->Stamp);

      // Only after the write returned, and as one object: whoever picks up this
      // disk state finds the batch inside its snapshot and the windows that
      // carried it already excluded. The whole batch changes hands at once,
      // with no state in between
      Disk_[i].reset(new CDiskState(db, Segments_.back()->Seq));
      DurableThrough_[i].store(Segments_.back()->Seq, std::memory_order_release);
      PendingSegments_[i].fetch_sub(count, std::memory_order_relaxed);
      PendingBytes_[i].fetch_sub(bytes, std::memory_order_relaxed);
      flushed = true;
    }

    if (flushed) {
      // After the watermarks, mirroring attach: this bump carries them
      Flushed_.fetch_add(1, std::memory_order_release);
      Flushed_.notify_all();
    }

    return flushed;
  }

  // The shard's unflushed tail and its weight: everything above the watermark.
  // Sequences increase along the view, so that is always a suffix
  size_t unflushed(const CKvView<CKey> *view, size_t shardIndex, std::vector<const CWindow<CKey>*> &out, size_t &bytes) const {
    const uint64_t durable = DurableThrough_[shardIndex].load(std::memory_order_acquire);
    const auto &shard = view->Shards[shardIndex];
    size_t count = 0;
    bytes = 0;
    for (size_t k = 0; k < shard.Count; k++) {
      const CWindow<CKey> *window = view->Pending[shard.First + k].get();
      // already on the disk, waiting for the next attach to drop it
      if (window->Seq <= durable)
        continue;
      out.push_back(window);
      count++;
      bytes += window->Bytes;
    }

    return count;
  }

  // Asks the flusher to ignore its floors, then waits it out - the request is a
  // counter, the wake is the ticket attach uses, so the writer thread stays the
  // only mutator of both. Word before view, mirroring flushLoop
  void drain() {
    DrainRequests_.fetch_add(1, std::memory_order_release);
    Published_.fetch_add(1, std::memory_order_release);
    Published_.notify_all();

    for (;;) {
      const uint64_t flushed = Flushed_.load(std::memory_order_acquire);

      {
        CKvGuard<CKey> guard = this->guard();
        if (!hasWork(guard.view()))
          return;
      }

      Flushed_.wait(flushed, std::memory_order_acquire);
    }
  }

private:
  std::string Name_;
  size_t ShardsNum_ = 0;
  size_t FlushBytesLower_ = 1u << 24;
  size_t FlushSegmentsLower_ = DefaultFlushSegments;
  size_t PendingBytesLimit_ = 2u << 24;
  size_t PendingSegmentsLimit_ = 2 * DefaultFlushSegments;

  // Flusher scratch: the shard's unflushed tail, refilled every pass
  std::vector<const CWindow<CKey>*> Segments_;

  // Whose fold turns those windows into rows - the database class
  IKvSegmentWriter<CKey> *SegmentWriter_ = nullptr;

  // The published revision. Mutable: readers are const, and taking a guard
  // touches the reference cache in the slot
  mutable atomic_intrusive_ptr<CKvView<CKey>> Current_;

  // Revisions alive anywhere, including those only a reader still holds
  std::atomic<size_t> LiveViews_{0};

  // The disk state each shard reads at, published by the flusher and picked up
  // by the next revision. Slots, not a view field: attach must not wait for it
  std::array<atomic_intrusive_ptr<const CDiskState>, MaxShards> Disk_;

  // Next unit's sequence and the newest durable one per shard: together with
  // the view, the whole state of what is still owed to the disk. One writer
  // each - attach for the counter, the flusher for the watermarks
  uint64_t NextSeq_ = 1;
  std::array<std::atomic<uint64_t>, MaxShards> DurableThrough_{};

  // The same tail counted instead of walked, for admission only: attach adds a
  // window before publishing it, the flusher subtracts what it wrote. Never
  // below the truth, so the probe answers early rather than late
  std::array<std::atomic<size_t>, MaxShards> PendingSegments_{};
  std::array<std::atomic<size_t>, MaxShards> PendingBytes_{};

  // Tickets, one per direction, bumped after the state they announce. They
  // count nothing - only "look at the view again" - so there is nothing to add
  // up wrong and no mutex between the threads. The third is drain asking for
  // the floors to be ignored; the flusher keeps the last value it served, so
  // nothing needs clearing
  static constexpr uint64_t StopBit = 1ull << 63;
  std::atomic<uint64_t> Published_{0};
  std::atomic<uint64_t> Flushed_{0};
  std::atomic<uint64_t> DrainRequests_{0};

  std::thread FlushThread_;
};

}
}
