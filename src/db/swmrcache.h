#pragma once

// Single-writer / multi-reader fixed-capacity cache map: prototype of the
// production utxo ReadCache. 2-choice set-associative buckets of 8 inline
// slots, per-slot seqlock publication, eviction is a height floor enforced
// by an incremental sweep. Insertion order never materializes as a queue:
// the age of an entry is its Height field, condemned entries keep serving
// hits until the sweep reclaims their slots.
//
// Concurrency contract: any number of lookupConcurrent() readers, and
// mutators (insert/spend) may run from several threads concurrently PROVIDED
// their key sets are disjoint - no two threads ever touch the same key at
// the same time (shard-partitioned flushers satisfy this by construction) -
// AND the owner enabled the mode with setConcurrentMutators(true) at a
// quiescent point. In that mode slot ownership is arbitrated by a CAS claim
// on the per-slot seqlock, tag bytes are replaced with a CAS loop on the tag
// word, the found slot is re-verified under the claim (it may have been
// displaced meanwhile) and accounting uses relaxed atomic adds. With the
// mode off (default) the mutator path keeps the plain single-writer stores:
// the locked instructions are full barriers on x86 and cost a DRAM latency
// each on cold slot lines, which is measurable at reindex rates. A reader
// never spins either way: a slot caught mid-mutation is reported as a miss,
// which the cache contract allows ("absence is always legal, a positive
// must be exact").
//
// Owner-side only (quiescent, no concurrent mutators): maintain(), init(),
// ensureHeightCapacity() and the whole persistence API (exportBucket/
// importBucket/finalizeImport/clear/debugCheck). Before a concurrent
// mutator wave the owner must pre-size the height accounting via
// ensureHeightCapacity(maxInsertHeight): insert() grows it only when it
// runs as the single mutator.
//
// ValueT must be trivially copyable and expose a public uint32_t Height.

#include "db/keyHash.h"
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#endif

template<typename ValueT>
class CSwmrCache {
  static_assert(std::is_trivially_copyable_v<ValueT>, "seqlock publication requires a trivially copyable value");

public:
  static constexpr size_t SLOTS_PER_BUCKET = 8;

private:
  struct SSlot {
    std::atomic<uint32_t> Seq; // seqlock: odd = mutation in progress
    uint32_t Vout;
    uint8_t TxId[32];
    ValueT Value;
  };

  struct SRef {
    uint64_t H1;
    uint64_t H2;
    uint8_t Tag;
  };

  struct SFind {
    SSlot *Slot = nullptr;
    size_t Bucket = 0;
    unsigned Index = 0;
  };

  static constexpr uint64_t LOW_BYTES = 0x0101010101010101ull;
  static constexpr uint64_t HIGH_BITS = 0x8080808080808080ull;
  static constexpr size_t SWEEP_CHUNK = 4096;

  std::atomic<uint64_t> *Tags_ = nullptr; // 8 tags per bucket in one word, 0 = empty slot
  SSlot *Slots_ = nullptr;
  size_t NumBuckets_ = 0;
  size_t Limit_ = 0;
  size_t CondemnedSlack_ = 0;

  uint32_t Floor_ = 0;     // entries with Height < Floor_ are condemned
  size_t LiveCount_ = 0;   // occupied slots, condemned included
  size_t Condemned_ = 0;   // occupied slots below the floor, not swept yet
  size_t SweepBucket_ = 0; // circular sweep cursor
  std::vector<uint32_t> AliveByHeight_;

  uint64_t SweptCount_ = 0;
  uint64_t DisplacedCount_ = 0;
  uint64_t DupOverwriteCount_ = 0;
  uint64_t SweptBucketCount_ = 0;

  // false (default) = classic single-writer mutator path with plain stores;
  // true = disjoint-key concurrent mutators (CAS claims, atomic counters)
  bool ConcurrentMutators_ = false;

  static SRef refOf(const uint8_t *txid, uint32_t vout) {
    const COutpointHash h = hashOutpoint(txid, vout);
    SRef ref;
    ref.H1 = h.H1;
    ref.H2 = h.H2;
    // fastrange consumes the high bits, so the tag must come from the low ones
    ref.Tag = static_cast<uint8_t>(ref.H1);
    if (!ref.Tag)
      ref.Tag = 0x4D;
    return ref;
  }

  size_t bucketOf(uint64_t h) const { return fastrange(h, NumBuckets_); }

  static unsigned ctz64(uint64_t v) {
#ifdef _MSC_VER
    unsigned long index;
    _BitScanForward64(&index, v);
    return static_cast<unsigned>(index);
#else
    return static_cast<unsigned>(__builtin_ctzll(v));
#endif
  }

  // counter add: plain in single-writer mode, relaxed atomic under
  // concurrent mutators (the owner reads totals only in quiescent phases)
  template<typename T>
  void addCounter(T &v, long long delta) {
    if (!ConcurrentMutators_) {
      v = static_cast<T>(v + delta);
      return;
    }
#ifdef _MSC_VER
    if constexpr (sizeof(T) == 8)
      _InterlockedExchangeAdd64(reinterpret_cast<volatile __int64*>(&v), static_cast<__int64>(delta));
    else
      _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(&v), static_cast<long>(delta));
#else
    __atomic_fetch_add(&v, static_cast<T>(delta), __ATOMIC_RELAXED);
#endif
  }

  SSlot &slotAt(size_t bucket, unsigned i) { return Slots_[bucket * SLOTS_PER_BUCKET + i]; }
  const SSlot &slotAt(size_t bucket, unsigned i) const { return Slots_[bucket * SLOTS_PER_BUCKET + i]; }

  static uint8_t tagAt(uint64_t w, unsigned i) { return static_cast<uint8_t>(w >> (i * 8)); }

  // SWAR candidate filter; may flag extra bytes (borrow artifacts), callers
  // must re-check the exact tag byte, false negatives are impossible
  static uint64_t matchMask(uint64_t w, uint8_t tag) {
    uint64_t x = w ^ (LOW_BYTES * tag);
    return (x - LOW_BYTES) & ~x & HIGH_BITS;
  }

  static int findFree(uint64_t w) {
    for (unsigned i = 0; i < SLOTS_PER_BUCKET; i++) {
      if (!tagAt(w, i))
        return static_cast<int>(i);
    }
    return -1;
  }

  static int freeCount(uint64_t w) {
    int n = 0;
    for (unsigned i = 0; i < SLOTS_PER_BUCKET; i++)
      n += !tagAt(w, i);
    return n;
  }

  // seqlock writer protocol; the release fence orders the odd store before
  // the field stores on ARM/x86 (kernel-style seqlock, not pure-C++ formal).
  // Under concurrent mutators the claim is a CAS even->odd arbitrating slot
  // ownership: false = the slot is mid-mutation elsewhere, re-evaluate; the
  // acquire on success pairs with the loser's writeEnd release, so the
  // payload re-verified under the claim is the latest committed one. In
  // single-writer mode the claim is the plain odd store of the original
  // seqlock: a locked CAS is a full barrier that stalls for the whole DRAM
  // miss of a cold slot line, the plain store hides behind the store buffer
  bool tryClaim(SSlot &s) {
    uint32_t seq = s.Seq.load(std::memory_order_relaxed);
    if (!ConcurrentMutators_) {
      s.Seq.store(seq + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      return true;
    }
    if (seq & 1)
      return false;
    if (!s.Seq.compare_exchange_strong(seq, seq + 1, std::memory_order_acquire, std::memory_order_relaxed))
      return false;
    std::atomic_thread_fence(std::memory_order_release);
    return true;
  }

  // payload was written: advance to the next even value, readers that
  // sampled the old sequence discard their copy
  static void writeEnd(SSlot &s) {
    s.Seq.store(s.Seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  // nothing was written: restore the pre-claim even value
  static void unclaimClean(SSlot &s) {
    s.Seq.store(s.Seq.load(std::memory_order_relaxed) - 1, std::memory_order_release);
  }

  // plain read-modify-write in single-writer mode; a CAS loop under
  // concurrent mutators, which may be replacing other bytes of the word
  void setTag(size_t bucket, unsigned slot, uint8_t tag) {
    const uint64_t mask = 0xFFull << (slot * 8);
    const uint64_t val = static_cast<uint64_t>(tag) << (slot * 8);
    uint64_t w = Tags_[bucket].load(std::memory_order_relaxed);
    if (!ConcurrentMutators_) {
      Tags_[bucket].store((w & ~mask) | val, std::memory_order_release);
      return;
    }
    while (!Tags_[bucket].compare_exchange_weak(w, (w & ~mask) | val, std::memory_order_release, std::memory_order_relaxed))
      ;
  }

  void accountRemoval(uint32_t height) {
    addCounter(LiveCount_, -1);
    addCounter(AliveByHeight_[height], -1);
    if (height < Floor_)
      addCounter(Condemned_, -1);
  }

  // mutator-side search with plain reads. With concurrent mutators a slot
  // mid-rewrite can be read torn here, so a match is only a candidate:
  // every mutation path re-verifies the key under the slot claim before
  // acting on it (a torn mismatch just skips - the own key of this thread
  // is never mutated elsewhere, its slot is either stable or being
  // displaced, and displacement is caught by the re-verify)
  SFind findOwn(size_t b1, size_t b2, uint8_t tag, const uint8_t *txid, uint32_t vout) {
    for (int pass = 0; pass < 2; pass++) {
      if (pass && b2 == b1)
        break;
      size_t b = pass ? b2 : b1;
      uint64_t w = Tags_[b].load(std::memory_order_relaxed);
      uint64_t mask = matchMask(w, tag);
      while (mask) {
        unsigned i = ctz64(mask) >> 3;
        mask &= mask - 1;
        if (tagAt(w, i) != tag)
          continue;
        SSlot &s = slotAt(b, i);
        if (s.Vout == vout && memcmp(s.TxId, txid, 32) == 0)
          return {&s, b, i};
      }
    }
    return {};
  }

  void sweepChunk(size_t buckets) {
    for (size_t n = 0; n < buckets; n++) {
      size_t b = SweepBucket_;
      if (++SweepBucket_ == NumBuckets_)
        SweepBucket_ = 0;
      SweptBucketCount_++;
      uint64_t w = Tags_[b].load(std::memory_order_relaxed);
      if (!w)
        continue;
      for (unsigned i = 0; i < SLOTS_PER_BUCKET; i++) {
        if (!tagAt(w, i))
          continue;
        SSlot &s = slotAt(b, i);
        uint32_t height = s.Value.Height;
        if (height >= Floor_)
          continue;
        if (!tryClaim(s))
          continue; // impossible under the owner-quiescence contract; skip
        setTag(b, i, 0);
        writeEnd(s);
        accountRemoval(height);
        SweptCount_++;
      }
    }
  }

public:
  // Two-phase initialization: a default-constructed cache is a valid
  // "disabled" state, every access must be gated on enabled()
  CSwmrCache() {}
  explicit CSwmrCache(size_t limit, size_t slotCap = 400000000) { init(limit, slotCap); }

  void init(size_t limit, size_t slotCap = 400000000) {
    Limit_ = std::max<size_t>(limit, 1024);
    size_t slots = Limit_ + Limit_ / 4; // load factor 0.8 at the limit
    if (slots > slotCap)
      slots = slotCap; // BTC UTXO peak (174M) sits far below the cap
    NumBuckets_ = std::max<size_t>(slots / SLOTS_PER_BUCKET, 64);
    size_t capacity = NumBuckets_ * SLOTS_PER_BUCKET;
    size_t headroom = capacity > Limit_ ? capacity - Limit_ : capacity / 8;
    CondemnedSlack_ = std::max<size_t>(std::min(Limit_ / 32, headroom / 2), 1024);
    // calloc: zeroed pages are a valid initial state (Seq even, tags empty)
    // and fault in lazily, so RSS tracks occupancy instead of capacity
    Tags_ = static_cast<std::atomic<uint64_t>*>(calloc(NumBuckets_, sizeof(std::atomic<uint64_t>)));
    Slots_ = static_cast<SSlot*>(calloc(capacity, sizeof(SSlot)));
    if (!Tags_ || !Slots_) {
      fprintf(stderr, "ERROR: swmr cache: can't allocate %zu buckets\n", NumBuckets_);
      exit(1);
    }
    AliveByHeight_.reserve(1u << 20);
  }

  bool enabled() const { return Tags_ != nullptr; }

  ~CSwmrCache() {
    free(Tags_);
    free(Slots_);
  }

  CSwmrCache(const CSwmrCache&) = delete;
  CSwmrCache &operator=(const CSwmrCache&) = delete;

  // returns false if the key was already present (BIP30 duplicate coinbase
  // txid) - the value is overwritten in place
  bool insert(const uint8_t *txid, uint32_t vout, const ValueT &value) {
    return insertWith(txid, vout, value.Height, [&value](ValueT &slot) { slot = value; });
  }

  // In-place variant: fill() writes the payload directly into the claimed
  // slot, sparing the caller's staging copy. height must match the Height
  // the callback stores - the eviction accounting is driven by it
  template<typename FillF>
  bool insertWith(const uint8_t *txid, uint32_t vout, uint32_t height, FillF &&fill) {
    const SRef ref = refOf(txid, vout);
    const size_t b1 = bucketOf(ref.H1);
    const size_t b2 = bucketOf(ref.H2);

    // growth is single-mutator only: a concurrent wave pre-sizes the height
    // accounting via ensureHeightCapacity() before it starts
    if (height >= AliveByHeight_.size())
      AliveByHeight_.resize(height + 1, 0);

    for (;;) {
      if (SFind dup = findOwn(b1, b2, ref.Tag, txid, vout); dup.Slot) {
        SSlot &s = *dup.Slot;
        if (!tryClaim(s))
          continue;
        // findOwn ran unclaimed: the slot may have been displaced by a
        // concurrent mutator between the find and the claim, re-verify
        if (tagAt(Tags_[dup.Bucket].load(std::memory_order_relaxed), dup.Index) != ref.Tag ||
            s.Vout != vout || memcmp(s.TxId, txid, 32) != 0) {
          unclaimClean(s);
          continue;
        }
        uint32_t oldHeight = s.Value.Height;
        fill(s.Value);
        assert(s.Value.Height == height);
        writeEnd(s);
        addCounter(AliveByHeight_[oldHeight], -1);
        if (oldHeight < Floor_)
          addCounter(Condemned_, -1);
        addCounter(AliveByHeight_[height], 1);
        if (height < Floor_)
          addCounter(Condemned_, 1);
        addCounter(DupOverwriteCount_, 1);
        return false;
      }

      uint64_t w1 = Tags_[b1].load(std::memory_order_relaxed);
      uint64_t w2 = b2 == b1 ? w1 : Tags_[b2].load(std::memory_order_relaxed);
      int free1 = freeCount(w1);
      int free2 = b2 == b1 ? 0 : freeCount(w2);

      size_t bucket;
      unsigned slot;
      bool displacing = false;
      if (free1 || free2) {
        bucket = free1 >= free2 ? b1 : b2;
        slot = static_cast<unsigned>(findFree(bucket == b1 ? w1 : w2));
      } else {
        // both buckets full: displace the weakest entry, condemned first,
        // then the oldest; losing a live entry costs one future miss, which
        // the cache contract allows. The heights here are heuristic plain
        // reads, the victim is re-read under the claim for exact accounting
        displacing = true;
        bucket = b1;
        slot = 0;
        uint32_t victimHeight = UINT32_MAX;
        bool victimCondemned = false;
        auto consider = [&](size_t b) {
          for (unsigned i = 0; i < SLOTS_PER_BUCKET; i++) {
            const SSlot &cs = slotAt(b, i);
            uint32_t h = cs.Value.Height;
            bool cond = h < Floor_;
            if ((cond && !victimCondemned) || (cond == victimCondemned && h < victimHeight)) {
              bucket = b;
              slot = i;
              victimHeight = h;
              victimCondemned = cond;
            }
          }
        };
        consider(b1);
        if (b2 != b1)
          consider(b2);
      }

      SSlot &s = slotAt(bucket, slot);
      if (!tryClaim(s))
        continue;
      uint8_t claimedTag = tagAt(Tags_[bucket].load(std::memory_order_relaxed), slot);
      if (!displacing && claimedTag != 0) {
        // the free slot was taken by a concurrent mutator: re-evaluate
        unclaimClean(s);
        continue;
      }
      if (claimedTag != 0) {
        // the victim payload is stable under the claim; a slot picked as a
        // victim but freed meanwhile falls through as a plain free insert
        accountRemoval(s.Value.Height);
        addCounter(DisplacedCount_, 1);
      }
      s.Vout = vout;
      memcpy(s.TxId, txid, 32);
      fill(s.Value);
      assert(s.Value.Height == height);
      // publish the tag before releasing the seqlock: a claimed slot must
      // never look free to a concurrent mutator scanning for empty slots
      setTag(bucket, slot, ref.Tag);
      writeEnd(s);
      addCounter(LiveCount_, 1);
      addCounter(AliveByHeight_[height], 1);
      if (height < Floor_)
        addCounter(Condemned_, 1);
      return true;
    }
  }

  // mutator-side find-and-erase; fn(const ValueT&) is called before removal
  template<typename F>
  bool spend(const uint8_t *txid, uint32_t vout, F &&fn) {
    const SRef ref = refOf(txid, vout);
    const size_t b1 = bucketOf(ref.H1);
    const size_t b2 = bucketOf(ref.H2);
    for (;;) {
      SFind f = findOwn(b1, b2, ref.Tag, txid, vout);
      if (!f.Slot)
        return false;
      SSlot &s = *f.Slot;
      if (!tryClaim(s))
        continue;
      if (tagAt(Tags_[f.Bucket].load(std::memory_order_relaxed), f.Index) != ref.Tag ||
          s.Vout != vout || memcmp(s.TxId, txid, 32) != 0) {
        // displaced between the find and the claim: rescan (usually a miss)
        unclaimClean(s);
        continue;
      }
      fn(static_cast<const ValueT&>(s.Value));
      uint32_t height = s.Value.Height;
      setTag(f.Bucket, f.Index, 0);
      writeEnd(s);
      accountRemoval(height);
      return true;
    }
  }

  // reader path: wait-free, no retry loops - any conflict degrades to a miss
  template<typename F>
  bool lookupConcurrent(const uint8_t *txid, uint32_t vout, F &&fn) const {
    const SRef ref = refOf(txid, vout);
    const size_t b1 = bucketOf(ref.H1);
    const size_t b2 = bucketOf(ref.H2);
    for (int pass = 0; pass < 2; pass++) {
      if (pass && b2 == b1)
        break;
      size_t b = pass ? b2 : b1;
      uint64_t w = Tags_[b].load(std::memory_order_relaxed);
      uint64_t mask = matchMask(w, ref.Tag);
      while (mask) {
        unsigned i = ctz64(mask) >> 3;
        mask &= mask - 1;
        if (tagAt(w, i) != ref.Tag)
          continue;
        const SSlot &s = slotAt(b, i);
        uint32_t s1 = s.Seq.load(std::memory_order_acquire);
        if (s1 & 1)
          continue;
        uint32_t vout2 = s.Vout;
        uint8_t key[32];
        ValueT value;
        memcpy(key, s.TxId, 32);
        memcpy(&value, &s.Value, sizeof(ValueT));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.Seq.load(std::memory_order_relaxed) != s1)
          continue; // torn by a concurrent mutation: report a miss
        if (vout2 != vout || memcmp(key, txid, 32) != 0)
          continue;
        fn(static_cast<const ValueT&>(value));
        return true;
      }
    }
    return false;
  }

  // owner-side, quiescent (no concurrent mutators): pre-size the height
  // accounting before a wave of concurrent inserts; insert() itself grows
  // it only when it runs as the single mutator
  void ensureHeightCapacity(uint32_t maxHeight) {
    if (maxHeight >= AliveByHeight_.size())
      AliveByHeight_.resize(maxHeight + 1, 0);
  }

  // owner-side, quiescent: toggle the disjoint-key concurrent mutator mode
  // around a wave (see the contract at the top of the file)
  void setConcurrentMutators(bool on) { ConcurrentMutators_ = on; }

  // advance the floor until the live population fits the limit, then reclaim
  // condemned slots under pressure; sweeping is pure memory bandwidth and
  // self-tunes to the condemn inflow, condemned entries hit until swept.
  // Owner-side: requires no concurrent mutators (reads the counters plainly
  // and moves the floor, which mutators read unsynchronized)
  void maintain() {
    while (LiveCount_ - Condemned_ >= Limit_ && Floor_ < AliveByHeight_.size()) {
      Condemned_ += AliveByHeight_[Floor_];
      Floor_++;
    }
    size_t budget = NumBuckets_; // hard cap: one full pass per call
    while (Condemned_ > CondemnedSlack_ && budget) {
      size_t chunk = std::min(budget, SWEEP_CHUNK);
      sweepChunk(chunk);
      budget -= chunk;
    }
  }

  // --- owner-side persistence API (quiescent state only: no concurrent
  // readers or writer; the file format lives in swmrdump.h) ---

  struct SExportEntry {
    uint8_t TxId[32];
    uint32_t Vout;
    ValueT Value;
  };

  size_t limit() const { return Limit_; }

  // entry limit fitting a memory budget: the table allocates limit * 1.25
  // slots plus one tag byte per slot
  static size_t limitForMemory(size_t bytes) {
    return bytes / (sizeof(SSlot) + 1) * 4 / 5;
  }

  // copies every entry of the bucket into out[], returns their number
  unsigned exportBucket(size_t bucket, SExportEntry out[SLOTS_PER_BUCKET]) const {
    uint64_t w = Tags_[bucket].load(std::memory_order_relaxed);
    unsigned n = 0;
    for (unsigned i = 0; i < SLOTS_PER_BUCKET; i++) {
      if (!tagAt(w, i))
        continue;
      const SSlot &s = slotAt(bucket, i);
      memcpy(out[n].TxId, s.TxId, 32);
      out[n].Vout = s.Vout;
      memcpy(&out[n].Value, &s.Value, sizeof(ValueT));
      n++;
    }
    return n;
  }

  // direct placement into an empty bucket, bypassing 2-choice insertion;
  // touches only this bucket's memory, so disjoint bucket ranges can be
  // imported from parallel threads; global counters are rebuilt afterwards
  // by finalizeImport(). Every key is re-hashed and must map to the bucket:
  // a corrupt record or a different table geometry rejects the bucket, and
  // since the tag word is published last, a rejected bucket stays empty.
  bool importBucket(size_t bucket, const SExportEntry *entries, unsigned count) {
    if (bucket >= NumBuckets_ || count == 0 || count > SLOTS_PER_BUCKET)
      return false;
    if (Tags_[bucket].load(std::memory_order_relaxed) != 0)
      return false;
    uint64_t w = 0;
    for (unsigned i = 0; i < count; i++) {
      const SRef ref = refOf(entries[i].TxId, entries[i].Vout);
      if (bucketOf(ref.H1) != bucket && bucketOf(ref.H2) != bucket)
        return false;
      SSlot &s = slotAt(bucket, i);
      s.Vout = entries[i].Vout;
      memcpy(s.TxId, entries[i].TxId, 32);
      memcpy(&s.Value, &entries[i].Value, sizeof(ValueT));
      w |= static_cast<uint64_t>(ref.Tag) << (i * 8);
    }
    Tags_[bucket].store(w, std::memory_order_relaxed);
    return true;
  }

  // single-threaded scan rebuilding the incremental accounting after a
  // series of importBucket() calls; the floor restarts from zero and the
  // next maintain() re-derives it from the same population
  void finalizeImport() {
    Floor_ = 0;
    Condemned_ = 0;
    SweepBucket_ = 0;
    LiveCount_ = 0;
    AliveByHeight_.clear();
    for (size_t b = 0; b < NumBuckets_; b++) {
      uint64_t w = Tags_[b].load(std::memory_order_relaxed);
      if (!w)
        continue;
      for (unsigned i = 0; i < SLOTS_PER_BUCKET; i++) {
        if (!tagAt(w, i))
          continue;
        uint32_t height = slotAt(b, i).Value.Height;
        if (height >= AliveByHeight_.size())
          AliveByHeight_.resize(height + 1, 0);
        AliveByHeight_[height]++;
        LiveCount_++;
      }
    }
  }

  // drops every entry (failed-import recovery); slot payloads stay in
  // place, the zeroed tag words mark all slots free
  void clear() {
    for (size_t b = 0; b < NumBuckets_; b++)
      Tags_[b].store(0, std::memory_order_relaxed);
    LiveCount_ = 0;
    Condemned_ = 0;
    Floor_ = 0;
    SweepBucket_ = 0;
    AliveByHeight_.clear();
  }

  // owner-side full scan validating the incremental accounting
  bool debugCheck() const {
    size_t occupied = 0;
    size_t condemned = 0;
    for (size_t b = 0; b < NumBuckets_; b++) {
      uint64_t w = Tags_[b].load(std::memory_order_relaxed);
      for (unsigned i = 0; i < SLOTS_PER_BUCKET; i++) {
        if (!tagAt(w, i))
          continue;
        occupied++;
        if (slotAt(b, i).Value.Height < Floor_)
          condemned++;
      }
    }
    uint64_t aliveSum = 0;
    for (uint32_t n: AliveByHeight_)
      aliveSum += n;
    bool ok = occupied == LiveCount_ && condemned == Condemned_ && aliveSum == LiveCount_;
    if (!ok)
      fprintf(stderr, "swmr debugCheck FAILED: occupied %zu vs %zu, condemned %zu vs %zu, aliveSum %llu\n",
              occupied, LiveCount_, condemned, Condemned_, static_cast<unsigned long long>(aliveSum));
    return ok;
  }

  size_t size() const { return LiveCount_; }
  size_t condemned() const { return Condemned_; }
  uint32_t floorHeight() const { return Floor_; }
  size_t capacity() const { return NumBuckets_ * SLOTS_PER_BUCKET; }
  size_t numBuckets() const { return NumBuckets_; }
  size_t memoryBytes() const { return NumBuckets_ * sizeof(uint64_t) + capacity() * sizeof(SSlot); }
  uint64_t sweptCount() const { return SweptCount_; }
  uint64_t displacedCount() const { return DisplacedCount_; }
  uint64_t dupOverwriteCount() const { return DupOverwriteCount_; }
  double sweepPasses() const { return NumBuckets_ ? static_cast<double>(SweptBucketCount_) / NumBuckets_ : 0.0; }
  uint64_t sweptBytes() const { return SweptBucketCount_ * (sizeof(uint64_t) + SLOTS_PER_BUCKET * sizeof(SSlot)); }
};
