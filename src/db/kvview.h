// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// Engine on published views. Per shard a revision is one stack of layers over
// the disk at its snapshot: the live era last (fold at insert, consecutive
// units annihilate in memory; tearOff() per unit hands readers a watermark,
// records above it do not exist for them), frozen eras below, drained by the
// flusher in freeze order, one batch per layer. Nothing refuses or waits -
// the backpressure is admission only (isPipelineFull).
//
// No layer is ever reset or reused, and an uncommitted era generation is
// above every pinned watermark by construction: readers take no locks.
//
// The shards belong to the database class, so the shutdown order is the
// owner's: stop admitting, commit the last unit, flushAll(), stop serving
// reads, shutdown(), close the shards. shutdown() waits out readers still
// inside a call, whose revisions hold snapshots.

#include "common/blockDataBase.h"
#include "common/intrusive_ptr.h"
#include "db/keyHash.h"
#include "db/kvlayer.h"

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

// Shards are fixed when the database is created, so the per-revision array is
// too
static constexpr size_t MaxShards = 16;

// Admission closes when a shard owes the disk this many layers: the queue
// absorbs flusher jitter, the cap is what bounds memory
static constexpr size_t MaxUnflushedLayers = 4;

// The disk half of a revision: a snapshot and how much of the freeze order is
// inside it, published as ONE object by the flusher right after its write - a
// layer is either wholly above the snapshot or wholly inside it. A layer
// counted twice is fatal to the folding families (merge sums it, array
// appends it)
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
  // before Shards - members die in reverse order, so the death is announced
  // only after every reference the view held is gone
  struct CLiveTicket {
    std::atomic<size_t> *Live = nullptr;
    ~CLiveTicket() {
      if (Live) {
        Live->fetch_sub(1, std::memory_order_release);
        Live->notify_all();
      }
    }
  } Ticket;

  struct CShardView {
    // Everything above the disk, oldest first: frozen layers - Seq set, the
    // flusher writes them in order - and, only ever last, the live era with
    // Seq still zero. A lookup walks the vector newest first
    std::vector<intrusive_ptr<const CLayer<CKey>>> Layers;

    // Visibility cut of the LAST layer in THIS revision: the watermark its
    // commit tore off when that layer is the live era, UINT32_MAX when it is
    // frozen - every generation of a frozen layer is committed
    uint32_t Watermark = UINT32_MAX;

    // The disk state this revision reads, kept alive by it
    intrusive_ptr<const CDiskState> Disk;
  };

  // Inline: a lookup reaches its shard straight from the pinned view
  std::array<CShardView, MaxShards> Shards;
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

// One unit of connect writes through this: a reference to the engine's active
// eras, borrowed for exactly one unit - each key routed to its shard's era by
// one hash, no engine in the way on the hottest path there is. Per-shard byte
// baselines are what "this unit wrote" means for an era that already holds
// the units before it
template<typename CKey>
class CKvWriter {
public:
  CKvWriter(CKvWriter&&) = default;
  CKvWriter(const CKvWriter&) = delete;
  CKvWriter &operator=(const CKvWriter&) = delete;

  size_t shardsNum() const { return ShardsNum_; }

  // Nothing below can hold this key: a brand new output, a transaction of a
  // block connected for the first time.
  // One hash per operation: shard from its high bits, map slot from its low
  void putNew(const CKey &key, const void *data, size_t size, const void *suffix = nullptr, size_t suffixSize = 0) {
    const size_t hash = std::hash<CKey>()(key);
    layerFor(hash).put(key, hash, data, size, suffix, suffixSize, false);
  }

  // May land on an existing value: an output restored by a disconnect, a BIP30
  // coinbase repeating an earlier one. Conservative - a false alarm costs one
  // Put plus one Delete instead of nothing
  void putRestore(const CKey &key, const void *data, size_t size, const void *suffix = nullptr, size_t suffixSize = 0) {
    const size_t hash = std::hash<CKey>()(key);
    layerFor(hash).put(key, hash, data, size, suffix, suffixSize, true);
  }

  void erase(const CKey &key) {
    const size_t hash = std::hash<CKey>()(key);
    layerFor(hash).erase(key, hash);
  }

  // Writes of the value families the engine does not interpret: merge folds a
  // delta into the key, array appends to its tail - the fold against the
  // unit's own previous write happens in fill(dst, prev). One hash per
  // operation, as in put/erase - the caller computes it once and passes it on
  size_t hashOf(const CKey &key) const { return std::hash<CKey>()(key); }

  template<typename F>
  void putWith(const CKey &key, size_t hash, size_t size, F &&fill) {
    layerFor(hash).putWith(key, hash, size, fill);
  }

  // The unit's own write for the key, if any: the era's uncommitted
  // generation - an older head belongs to a committed unit and is not the
  // caller's to touch
  const CGenRecord *findOwn(const CKey &key, size_t hash) const {
    return layerFor(hash).findOwn(key, hash);
  }

private:
  friend class CKvEngine<CKey>;

  explicit CKvWriter(size_t shardsNum) : ShardsNum_(shardsNum) {}

  // Did this unit write the shard - against the baseline, so an era full of
  // earlier units still answers for this unit alone
  bool wrote(size_t i) const { return Layers_[i].get()->bytes() != Baselines_[i]; }

  bool empty() const {
    for (size_t i = 0; i < ShardsNum_; i++) {
      if (wrote(i))
        return false;
    }
    return true;
  }

  CLayer<CKey> *layer(size_t i) const { return Layers_[i].get(); }
  CLayer<CKey> &layerFor(size_t hash) const { return *Layers_[fastrange(hash, ShardsNum_)].get(); }

  std::array<intrusive_ptr<CLayer<CKey>>, MaxShards> Layers_;
  std::array<size_t, MaxShards> Baselines_{};
  size_t ShardsNum_;
};

// How a layer becomes rocksdb rows: the family's business, not the engine's.
// KV replaces the key, merge sums deltas into it, array concatenates tails -
// all the engine knows is which layers are still owed to the disk
template<typename CKey>
class IKvSegmentWriter {
public:
  virtual ~IKvSegmentWriter() {}

  // One sealed layer, one batch: a frozen era is folded at insert - there are
  // no runs to merge across, and the flusher hands layers over one at a time
  virtual void writeLayer(rocksdb::DB *db,
                          size_t shardIndex,
                          const CLayer<CKey> *layer,
                          const BC::Proto::BlockHashTy &stamp) = 0;
};

template<typename CKey>
class CKvEngine {
public:
  struct CConfig {
    std::string Name;

    // Freeze threshold of a live era, bytes of its arena - the only flush
    // policy there is: an era leaves when it fills, a checkpoint freezes it
    // regardless
    size_t EraBytes = 256u << 20;
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
    // watermarks and hide every layer from it
    assert(!FlushThread_.joinable());
    assert(LiveViews_.load(std::memory_order_relaxed) == 0);
    Published_.store(0, std::memory_order_relaxed);
    Flushed_.store(0, std::memory_order_relaxed);
    NextSeq_ = 1;
    for (size_t i = 0; i < MaxShards; i++) {
      DurableThrough_[i].store(0, std::memory_order_relaxed);
      Unflushed_[i].store(0, std::memory_order_relaxed);
      ActiveEra_[i] = nullptr;
      Disk_[i].reset(nullptr);
    }

    Name_ = cfg.Name;
    ShardsNum_ = shards.size();
    EraBytes_ = cfg.EraBytes;

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

  // Stops the flusher and drops the published view. A unit committed but not
  // flushed is lost unless flushAll() ran first - same contract as today
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
    for (size_t i = 0; i < ShardsNum_; i++) {
      Disk_[i].reset(nullptr);
      ActiveEra_[i] = nullptr;
    }
    for (size_t live = LiveViews_.load(std::memory_order_acquire); live; live = LiveViews_.load(std::memory_order_acquire))
      LiveViews_.wait(live, std::memory_order_acquire);
  }

  // Writer side ---------------------------------------------------------

  // The only throttle there is: the pipeline asks before starting work and
  // stops admitting on yes; units already in preparation still commit without
  // waiting. Plain loads - no guard, no RMW on the slot readers pin
  bool isPipelineFull() const {
    assert(!stopped());
    for (size_t i = 0; i < ShardsNum_; i++) {
      if (Unflushed_[i].load(std::memory_order_relaxed) >= MaxUnflushedLayers)
        return true;
    }

    return false;
  }

  size_t shardsNum() const { return ShardsNum_; }

  // The writer of one unit of connect - a batch, a run, a single block.
  // Mutator thread only; a missing era is created here and enters a view only
  // when the unit that wrote it commits
  CKvWriter<CKey> liveWriter() {
    assert(!stopped());
    CKvWriter<CKey> writer(ShardsNum_);
    for (size_t i = 0; i < ShardsNum_; i++) {
      CLayer<CKey> *era = ActiveEra_[i].get();
      if (!era) {
        // Sized once for the whole era: the arena to its freeze threshold, the
        // map to the keys such an arena can hold (~48 bytes per record) - a
        // growth under readers is legal but copies the table
        era = new CLayer<CKey>(EraBytes_, EraBytes_ / 48);
        ActiveEra_[i] = era;
      }
      writer.Layers_[i] = era;
      writer.Baselines_[i] = era->bytes();
    }
    return writer;
  }

  // The unit's publication: tearOff per shard the unit touched, the new
  // watermark rides the revision swap; an era at its byte threshold freezes
  // right here. NOT thread-safe: one external mutator serializes every
  // commitLive() - readers and the flusher may run concurrently
  void commitLive(CKvWriter<CKey> &writer, const BC::Proto::BlockHashTy &stamp) {
    assert(writer.shardsNum() == ShardsNum_);
    assert(!stopped());

    if (writer.empty())
      return;

    CKvView<CKey> *next = cloneView();
    for (size_t i = 0; i < ShardsNum_; i++) {
      if (!writer.wrote(i))
        continue;
      typename CKvView<CKey>::CShardView &dst = next->Shards[i];

      CLayer<CKey> *era = ActiveEra_[i].get();
      assert(era == writer.layer(i) && "the writer outlived a freeze of its era");

      // A fresh era enters the stack with the first unit that wrote it
      if (dst.Layers.empty() || dst.Layers.back().get() != era)
        dst.Layers.push_back(intrusive_ptr<const CLayer<CKey>>(era));

      era->Stamp = stamp;
      era->tearOff();
      dst.Watermark = era->watermark();

      if (era->bytes() >= EraBytes_)
        freezeEra(next, i);
    }

    // The linearization point: one atomic swap by a single mutator - no CAS
    // loop, no ABA, no lock. Everyone sees the new revision from here on, the
    // old one dies with the last guard holding it
    Current_.reset(next);

    // The ticket after the revision, always: whoever acquires this bump sees
    // the view too, which is what keeps the flusher's wait free of lost wakeups
    Published_.fetch_add(1, std::memory_order_release);
    Published_.notify_all();
  }

  // Checkpoint and shutdown: everything committed reaches the disk, then every
  // shard - including those that never saw a write - gets the stamp, which
  // initialize() rejects the database for if the shards disagree
  void flushAll(const BC::Proto::BlockHashTy &stamp) {
    assert(!stopped());
    if (stamp.isNull())
      return;

    // The disk cannot be stamped over a live tail: freeze every live era
    bool live = false;
    for (size_t i = 0; i < ShardsNum_; i++)
      live |= ActiveEra_[i].get() != nullptr;
    if (live) {
      CKvView<CKey> *next = cloneView();
      for (size_t i = 0; i < ShardsNum_; i++)
        freezeEra(next, i);
      Current_.reset(next);
      Published_.fetch_add(1, std::memory_order_release);
      Published_.notify_all();
    }

    // The one place that waits the whole pipe out. The flusher is idle
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

  // The pinned view and nothing else: the layer stack newest first, then the
  // disk at its snapshot. Builders are never searched - see the note at the top
  template<typename F>
  bool find(const CKvGuard<CKey> &guard, const CKey &key, F &&callback) const {
    const size_t hash = std::hash<CKey>()(key);
    return lookup(guard, fastrange(hash, ShardsNum_), key, hash, callback);
  }

  // One shard of the pinned revision, for the families that fold every layer
  // themselves: oldest first, the last layer cut at the revision's Watermark,
  // every other one read whole
  const typename CKvView<CKey>::CShardView &shard(const CKvGuard<CKey> &guard, size_t shardIndex) const {
    return guard.view()->Shards[shardIndex];
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

  // One layer probed for a key: 1 = delivered, -1 = deleted here, 0 = miss.
  // Read at the given watermark - a record above it does not exist for this
  // reader, and a miss means "nothing at or below W" exactly
  template<typename F>
  static int probeLayer(const CLayer<CKey> *layer, uint32_t watermark, const CKey &key, size_t hash, F &&callback) {
    if (const CGenRecord *rec = layer->find(key, hash, watermark)) {
      if (rec->tombstone())
        return -1;
      callback(rec->payload(), rec->size());
      return 1;
    }
    return 0;
  }

  template<typename F>
  bool lookup(const CKvGuard<CKey> &guard, size_t shardIndex, const CKey &key, size_t hash, F &&callback) const {
    // Newest first, stopping on the first hit including a tombstone: otherwise
    // a deleted key resurrects from below
    const typename CKvView<CKey>::CShardView &shard = guard.view()->Shards[shardIndex];
    for (size_t j = shard.Layers.size(); j-- > 0; ) {
      const uint32_t watermark = j + 1 == shard.Layers.size() ? shard.Watermark : UINT32_MAX;
      if (int rc = probeLayer(shard.Layers[j].get(), watermark, key, hash, callback))
        return rc > 0;
    }

    // The disk at the revision of this view: layers and snapshot are one pair
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

  // The next revision, cloned from the current one by its single mutator. The
  // disk states refresh to the newest published, and a frozen layer such a
  // state already contains is dropped: layer and snapshot change as one pair,
  // which is what keeps the families that fold from counting a layer twice
  CKvView<CKey> *cloneView() {
    const CKvView<CKey> *prev = Current_.get();
    CKvView<CKey> *next = newView();
    for (size_t i = 0; i < ShardsNum_; i++) {
      typename CKvView<CKey>::CShardView &dst = next->Shards[i];
      const typename CKvView<CKey>::CShardView &src = prev->Shards[i];
      dst.Disk = intrusive_ptr<const CDiskState>(Disk_[i]);
      dst.Watermark = src.Watermark;
      // A durable layer is dropped with the disk state that contains it; the
      // live era (Seq still zero) is always carried
      for (const auto &layer: src.Layers) {
        const uint64_t seq = layer.get()->Seq.load(std::memory_order_relaxed);
        if (!seq || seq > dst.Disk.get()->DurableSeq)
          dst.Layers.push_back(layer);
      }
    }
    return next;
  }

  // Freeze the shard's live era, if there is one: it takes its place in the
  // freeze order and stops mutating - already last in the stack, so nothing
  // moves, the Seq is what hands it to the flusher
  void freezeEra(CKvView<CKey> *next, size_t i) {
    CLayer<CKey> *era = ActiveEra_[i].get();
    if (!era)
      return;
    era->Bytes = era->bytes();
    // release: a flusher that reads this seq through an older view must see
    // every record and the metadata written above
    era->Seq.store(NextSeq_++, std::memory_order_release);
    ActiveEra_[i] = nullptr;
    next->Shards[i].Watermark = UINT32_MAX;
    Unflushed_[i].fetch_add(1, std::memory_order_relaxed);
  }

  // Flusher side --------------------------------------------------------

  // Does the view still owe the disk anything: frozen layers only - a live
  // era (Seq zero) is never the flusher's until frozen
  bool hasWork(const CKvView<CKey> *view) const {
    for (size_t i = 0; i < ShardsNum_; i++) {
      for (const auto &layer: view->Shards[i].Layers) {
        const uint64_t seq = layer.get()->Seq.load(std::memory_order_acquire);
        if (seq && seq > DurableThrough_[i].load(std::memory_order_acquire))
          return true;
      }
    }
    return false;
  }

  void flushLoop() {
    loguru::set_thread_name((Name_ + ".flush").c_str());

    for (;;) {
      // Word before the view it announces, always: a unit this pass misses
      // has already moved the word, so the wait below returns instead of
      // blocking. The whole handshake - "look again", nothing more
      const uint64_t published = Published_.load(std::memory_order_acquire);

      {
        CKvGuard<CKey> guard = this->guard();
        if (flushPending(guard.view()))
          continue;
      }

      // stop, and every frozen layer is on the disk; promoting what is still
      // active is flushAll's business, by the shutdown contract
      if (published & StopBit)
        return;

      Published_.wait(published, std::memory_order_acquire);
    }
  }

  // One layer, one batch, as soon as it is frozen: a frozen era is folded at
  // insert, so there is nothing to wait for and nothing to merge across
  bool flushPending(const CKvView<CKey> *view) {
    bool flushed = false;

    for (size_t i = 0; i < ShardsNum_; i++) {
      const typename CKvView<CKey>::CShardView &shard = view->Shards[i];
      // The stack is oldest first; whatever is already on the disk waits for
      // the next revision to drop it, a live era (Seq zero) is not owed yet
      for (const auto &ref: shard.Layers) {
        const CLayer<CKey> *frozen = ref.get();
        const uint64_t seq = frozen->Seq.load(std::memory_order_acquire);
        if (!seq || seq <= DurableThrough_[i].load(std::memory_order_acquire))
          continue;

        rocksdb::DB *db = shard.Disk.get()->Db;
        SegmentWriter_->writeLayer(db, i, frozen, frozen->Stamp);

        // Only after the write returned, and as one object: whoever picks up
        // this disk state finds the batch inside its snapshot and the layer
        // that carried it already excluded
        Disk_[i].reset(new CDiskState(db, seq));
        DurableThrough_[i].store(seq, std::memory_order_release);
        Unflushed_[i].fetch_sub(1, std::memory_order_relaxed);
        flushed = true;
      }
    }

    if (flushed) {
      // After the watermarks, mirroring the commit: this bump carries them
      // and wakes drain()
      Flushed_.fetch_add(1, std::memory_order_release);
      Flushed_.notify_all();
    }

    return flushed;
  }

  // Waits the flusher out - the wake is the ticket the commit uses, the check
  // is the view itself. No forcing: the flusher writes everything it sees, so
  // "no work in the current revision" is exactly "drained"
  void drain() {
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

  // The active era per shard, mutator thread only: views hold their own
  // references, the flusher meets an era only through a view, once frozen
  std::array<intrusive_ptr<CLayer<CKey>>, MaxShards> ActiveEra_;
  size_t EraBytes_ = 256u << 20;

  // Whose fold turns those layers into rows - the database class
  IKvSegmentWriter<CKey> *SegmentWriter_ = nullptr;

  // The published revision. Mutable: readers are const, and taking a guard
  // touches the reference cache in the slot
  mutable atomic_intrusive_ptr<CKvView<CKey>> Current_;

  // Revisions alive anywhere, including those only a reader still holds
  std::atomic<size_t> LiveViews_{0};

  // The disk state each shard reads at, published by the flusher and picked up
  // by the next revision. Slots, not a view field: the commit must not wait
  // for it
  std::array<atomic_intrusive_ptr<const CDiskState>, MaxShards> Disk_;

  // Next frozen layer's sequence and the newest durable one per shard - what
  // is still owed to the disk. One writer each: the mutator for the counter,
  // the flusher for the watermarks
  uint64_t NextSeq_ = 1;
  std::array<std::atomic<uint64_t>, MaxShards> DurableThrough_{};

  // The admission mirror: how many layers each shard still owes the disk.
  // ++ by the mutator at freeze, -- by the flusher once durable
  std::array<std::atomic<size_t>, MaxShards> Unflushed_{};

  // Tickets, one per direction, bumped after the state they announce. They
  // count nothing - only "look at the view again" - so there is nothing to add
  // up wrong and no mutex between the threads
  static constexpr uint64_t StopBit = 1ull << 63;
  std::atomic<uint64_t> Published_{0};
  std::atomic<uint64_t> Flushed_{0};

  std::thread FlushThread_;
};

}
}
