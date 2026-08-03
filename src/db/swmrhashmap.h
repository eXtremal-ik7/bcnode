#pragma once

// Single-writer / multi-reader open-addressing map for the database window
// (the CLog layer): replaces the tbb::concurrent_hash_map + std::unordered_map
// pair with one flat table. Generation-based reset makes clearing O(1), so
// after a background flush the frozen map needs no drain at all - it stays
// readable until the log is recycled, and recycling is a counter bump.
//
// The map stores non-owning value pointers into the log arena; a null result
// means "not in this window, look deeper" (frozen log / disk), a tombstone
// entry answers "known absent in this window" the same way. Keys are never
// removed within a window (a pop() restore writes a tombstone instead), so
// linear-probe chains never break.
//
// Slot life cycle within one window ("gen" is the map generation, even):
//     stale (Gen != gen)  --claim: CAS Gen -> gen+1-->  mid-insert
//     mid-insert          --fill Key/Ptr, store Gen = gen (release)--> live
//     live                --update: store Ptr (release)--> live
// reset() bumps the generation by 2: every slot turns stale at once, the
// arena pointers inside die with the arena, nothing is walked or freed.
//
// The generation word doubles as the claim word and the publication word:
//  - a claim can only move a STALE slot to mid-insert, so a live slot can
//    never be stolen (the CAS expects the stale value it sampled);
//  - the release store of the even generation publishes the key and the
//    initial value together; a reader that acquire-loads Gen == gen may
//    read the key plainly - the key is immutable until the next reset;
//  - a mid-insert slot is skipped by everyone (readers may cut the probe
//    short here only at the cost of a legal "not yet inserted" answer, so
//    they must keep probing instead).
//
// Concurrency contract:
//  - any number of find() readers, wait-free, at any time;
//  - one writer thread (update/restore/reserve/reset) by default, OR several
//    writer threads with DISJOINT KEY SETS running between two quiescent
//    points after setConcurrentMutators(true): claims are arbitrated by the
//    generation CAS, Used_ is a relaxed atomic add, and a published slot is
//    only ever mutated by the thread that owns its key. The mode is off by
//    default because it is not free: it puts a lock cmpxchg and a lock xadd
//    on every insert. No eviction, no hazard pointers: entries live to reset().
//  - growth is single-writer only: a concurrent wave must pre-size the
//    table with reserve() so update() never grows mid-wave (update aborts
//    on an unexpectedly full table rather than corrupt it);
//  - forEachCurrent()/rebuild run on a quiescent map (the frozen log during
//    a background flush, or the owner between waves);
//  - a reader overlapping reset() is the same residual epoch race the
//    arena already has (bug-A class): the caller keeps logs out of reuse
//    for a full window cycle, a proper EBR closes it later in aggkv.
//
// The header is deliberately interpretable by GenMC (the model checker runs
// the real class, not a model): no std::vector, no calloc, no variable
// length memory intrinsics; stale slots never have their Key/Ptr read, so
// the uninitialized-memory rule is checked by the tool as well.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#ifdef _MSC_VER
#include <intrin.h>
#endif

template<typename CKey, typename CHasher = std::hash<CKey>>
class CSwmrHashMap {
public:
  struct SUpdateResult {
    void *Prev;         // previous value, nullptr if the key was absent
    bool FirstInRev;    // first touch of this key in this revision (undo point)
  };

private:
  struct SSlot {
    std::atomic<uint64_t> Gen;
    std::atomic<void*> Ptr;   // value, TOMB, or garbage while Gen is stale
    uint32_t LastRev;         // written only by the slot owner, never read concurrently
    CKey Key;                 // immutable from publication to reset
  };

  struct STable {
    size_t Capacity;
    size_t Mask;
    STable *NextGrave;
    SSlot *Slots;
  };

  std::atomic<STable*> Table_{nullptr};
  std::atomic<uint64_t> Gen_{2};  // even; slot values gen+1 mean mid-insert
  uint64_t Used_ = 0;             // claimed slots this window (atomicAdd from mutators)
  STable *Graveyard_ = nullptr;   // outgrown tables, freed at reset()
  CHasher Hasher_;
  bool ConcurrentMutators_ = false;

  static void *tombstone() { return reinterpret_cast<void*>(uintptr_t(1)); }

  template<typename T>
  static void atomicAdd(T &v, long long delta) {
#ifdef _MSC_VER
    _InterlockedExchangeAdd64(reinterpret_cast<volatile __int64*>(&v), static_cast<__int64>(delta));
#else
    __atomic_fetch_add(&v, static_cast<T>(delta), __ATOMIC_RELAXED);
#endif
  }

  template<typename T>
  static T atomicLoad(const T &v) {
#ifdef _MSC_VER
    return *reinterpret_cast<const volatile T*>(&v);
#else
    return __atomic_load_n(&v, __ATOMIC_RELAXED);
#endif
  }

  static STable *allocTable(size_t capacity) {
    STable *t = static_cast<STable*>(malloc(sizeof(STable) + capacity * sizeof(SSlot)));
    if (!t)
      abort();
    t->Capacity = capacity;
    t->Mask = capacity - 1;
    t->NextGrave = nullptr;
    t->Slots = reinterpret_cast<SSlot*>(t + 1);
    // only the generations need a defined value: Key/Ptr/LastRev of a stale
    // slot are never read, the claim writes them before the publication
    for (size_t i = 0; i < capacity; i++)
      t->Slots[i].Gen.store(0, std::memory_order_relaxed);
    return t;
  }

  static size_t roundUpPow2(size_t n) {
    size_t c = 1;
    while (c < n)
      c <<= 1;
    return c;
  }

  // single-writer growth; readers keep using the old table until the
  // release store below, the old table stays valid until reset()
  void grow(STable *t, size_t newCapacity) {
    const uint64_t gen = Gen_.load(std::memory_order_relaxed);
    STable *fresh = allocTable(newCapacity);
    uint64_t used = 0;
    for (size_t i = 0; i < t->Capacity; i++) {
      SSlot &s = t->Slots[i];
      if (s.Gen.load(std::memory_order_relaxed) != gen)
        continue;
      void *p = s.Ptr.load(std::memory_order_relaxed);
      if (p == tombstone())
        continue; // a tombstone means "absent": fresh chains don't need it
      size_t idx = Hasher_(s.Key) & fresh->Mask;
      for (;;) {
        SSlot &d = fresh->Slots[idx];
        if (d.Gen.load(std::memory_order_relaxed) != gen) {
          d.Key = s.Key;
          d.LastRev = s.LastRev;
          d.Ptr.store(p, std::memory_order_relaxed);
          d.Gen.store(gen, std::memory_order_relaxed);
          break;
        }
        idx = (idx + 1) & fresh->Mask;
      }
      used++;
    }
    Used_ = used;
    t->NextGrave = Graveyard_;
    Graveyard_ = t;
    Table_.store(fresh, std::memory_order_release);
  }

public:
  explicit CSwmrHashMap(size_t initialCapacity = 65536) {
    Table_.store(allocTable(roundUpPow2(initialCapacity < 4 ? 4 : initialCapacity)), std::memory_order_relaxed);
  }

  ~CSwmrHashMap() {
    freeGraveyard();
    free(Table_.load(std::memory_order_relaxed));
  }

  CSwmrHashMap(const CSwmrHashMap&) = delete;
  CSwmrHashMap &operator=(const CSwmrHashMap&) = delete;

  // reader side: wait-free, legal at any time. nullptr = not in this window
  // (a tombstone reports the same: the caller falls through to the frozen
  // log / disk, where the key is guaranteed older than this window)
  void *find(const CKey &key) const {
    const STable *t = Table_.load(std::memory_order_acquire);
    const uint64_t gen = Gen_.load(std::memory_order_relaxed);
    size_t idx = Hasher_(key) & t->Mask;
    for (size_t probe = 0; probe <= t->Mask; probe++, idx = (idx + 1) & t->Mask) {
      const SSlot &s = t->Slots[idx];
      const uint64_t g = s.Gen.load(std::memory_order_acquire);
      if (g == gen) {
        // published: the key is immutable, the acquire above pairs with the
        // publication release, so reading it plainly is race-free
        if (s.Key == key) {
          void *p = s.Ptr.load(std::memory_order_acquire);
          return p == tombstone() ? nullptr : p;
        }
        continue;
      }
      if (g == gen + 1)
        continue; // mid-insert of another key: the probe must not stop here
      return nullptr; // stale slot: end of the cluster
    }
    return nullptr;
  }

  // mutator side: insert or update, disjoint-key concurrent safe.
  // rev deduplicates undo records: the first touch of a key in a revision
  // reports FirstInRev and the caller records {key, Prev} for pop()
  SUpdateResult update(const CKey &key, void *value, uint32_t rev) {
    STable *t = Table_.load(std::memory_order_relaxed);
    const uint64_t gen = Gen_.load(std::memory_order_relaxed);
    size_t idx = Hasher_(key) & t->Mask;
    for (size_t probe = 0; probe <= t->Mask; probe++, idx = (idx + 1) & t->Mask) {
      SSlot &s = t->Slots[idx];
      uint64_t g = s.Gen.load(std::memory_order_acquire);
      if (g == gen) {
        if (!(s.Key == key))
          continue;
        // our own published slot: nobody else touches this key
        void *prev = s.Ptr.load(std::memory_order_relaxed);
        bool first = s.LastRev != rev;
        s.LastRev = rev;
        s.Ptr.store(value, std::memory_order_release);
        return {prev == tombstone() ? nullptr : prev, first};
      }
      if (g == gen + 1)
        continue; // another mutator mid-insert (a different key by contract)
      // stale slot: our key is absent, so it is claimed right here. With one
      // mutator the slot is ours by definition - fill it and publish with a
      // single release store. The claim CAS and the atomic counter exist only
      // to arbitrate between mutators, and both are locked read-modify-writes
      // on the insert path: the fast path skips them. Readers are unaffected
      // either way - a slot they see stale ends their probe, and nothing of
      // this cluster lives past it until the publication below
      if (ConcurrentMutators_) {
        // losing the CAS means another mutator extended the cluster - keep probing
        if (!s.Gen.compare_exchange_strong(g, gen + 1, std::memory_order_acquire, std::memory_order_relaxed))
          continue;
        s.Key = key;
        s.LastRev = rev;
        s.Ptr.store(value, std::memory_order_relaxed);
        s.Gen.store(gen, std::memory_order_release); // publish key + value together
        atomicAdd(Used_, 1);
        // single-writer growth; a concurrent wave must have reserve()d, so
        // this trigger never fires there (see the contract above)
        if (atomicLoad(Used_) > t->Capacity - t->Capacity / 4)
          grow(t, t->Capacity * 4);
        return {nullptr, true};
      }

      s.Key = key;
      s.LastRev = rev;
      s.Ptr.store(value, std::memory_order_relaxed);
      s.Gen.store(gen, std::memory_order_release); // publish key + value together
      Used_++;
      if (Used_ > t->Capacity - t->Capacity / 4)
        grow(t, t->Capacity * 4);
      return {nullptr, true};
    }
    // no stale slot in a full circle: only a concurrent wave that broke the
    // reserve() contract can fill the table this far, no correct answer left
    abort();
  }

  // pop() restore path (single mutator, readers allowed): rewrite the value
  // of an existing key, nullptr restores "absent" as a tombstone so the
  // probe chains never break
  void restore(const CKey &key, void *prev) {
    STable *t = Table_.load(std::memory_order_relaxed);
    const uint64_t gen = Gen_.load(std::memory_order_relaxed);
    size_t idx = Hasher_(key) & t->Mask;
    for (size_t probe = 0; probe <= t->Mask; probe++, idx = (idx + 1) & t->Mask) {
      SSlot &s = t->Slots[idx];
      if (s.Gen.load(std::memory_order_relaxed) == gen && s.Key == key) {
        s.Ptr.store(prev ? prev : tombstone(), std::memory_order_release);
        return;
      }
    }
    abort(); // the undo list referenced a key this window does not hold
  }

  // quiescent scan (frozen map in the flusher, or the owner between waves):
  // fn(key, value) for every live entry, tombstones skipped
  template<typename F>
  void forEachCurrent(F &&fn) const {
    const STable *t = Table_.load(std::memory_order_relaxed);
    const uint64_t gen = Gen_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < t->Capacity; i++) {
      const SSlot &s = t->Slots[i];
      if (s.Gen.load(std::memory_order_relaxed) != gen)
        continue;
      void *p = s.Ptr.load(std::memory_order_relaxed);
      if (p == tombstone())
        continue;
      fn(static_cast<const CKey&>(s.Key), p);
    }
  }

  // owner-side: make sure a concurrent wave of up to n new keys never
  // needs to grow (the 3/4 load threshold stays unreachable)
  void reserve(size_t n) {
    STable *t = Table_.load(std::memory_order_relaxed);
    size_t needed = roundUpPow2((Used_ + n) * 2);
    if (needed > t->Capacity)
      grow(t, needed);
  }

  // owner-side O(1) window reset: every slot turns stale at once, the
  // outgrown tables die with the window that outgrew them
  void reset() {
    Gen_.store(Gen_.load(std::memory_order_relaxed) + 2, std::memory_order_relaxed);
    Used_ = 0;
    freeGraveyard();
  }

  // Several mutators with disjoint key sets between two quiescent points: the
  // claim is arbitrated by a CAS and Used_ becomes an atomic counter. Nothing
  // turns it on today (the window is mutated by the serial thread alone), and
  // the two locked instructions per insert are what it costs to leave on
  void setConcurrentMutators(bool on) { ConcurrentMutators_ = on; }

  size_t used() const { return Used_; }
  size_t capacity() const { return Table_.load(std::memory_order_relaxed)->Capacity; }

private:
  void freeGraveyard() {
    while (Graveyard_) {
      STable *next = Graveyard_->NextGrave;
      free(Graveyard_);
      Graveyard_ = next;
    }
  }
};
