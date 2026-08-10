// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// One in-memory layer of the engine: one growing arena plus one hash map, an
// era absorbing many units of connect. tearOff() per unit publishes a
// watermark, and that integer IS an O(1) snapshot: a reader pinning W accepts
// a record iff Gen <= W; versions hang off record headers and die whole with
// the arena. Single mutator, reads wait-free under it - a reader left on an
// outgrown table misses only keys inserted after the swap, all above any
// watermark it can hold.

#include "common/blockDataBase.h"
#include "common/mlog.h"
#include "db/swmrhashmap.h"

#include <p2putils/strExtras.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>

namespace BC {
namespace DB {

// One version of one key: payload follows the header, the previous version's
// pointer - when one exists - precedes it
struct CGenRecord {
  static constexpr uint32_t MayExistBelowFlag = 0x80000000u;
  static constexpr uint32_t TombstoneFlag = 0x40000000u;
  static constexpr uint32_t HasPrevFlag = 0x20000000u;
  static constexpr uint32_t SizeMask = 0x1FFFFFFFu;

  uint32_t Gen;
  uint32_t SizeAndFlags;

  size_t size() const { return SizeAndFlags & SizeMask; }
  bool mayExistBelow() const { return (SizeAndFlags & MayExistBelowFlag) != 0; }
  bool tombstone() const { return (SizeAndFlags & TombstoneFlag) != 0; }
  const void *payload() const { return this + 1; }

  const CGenRecord *prev() const {
    if (!(SizeAndFlags & HasPrevFlag))
      return nullptr;
    const CGenRecord *p;
    memcpy(&p, reinterpret_cast<const char*>(this) - sizeof(void*), sizeof(p));
    return p;
  }
};

// A layer record for the flush fold: key prefix for cheap ordering, key and
// entry living in the layer itself
template<typename CKey>
struct CKvSortedRef {
  uint64_t Prefix;      // big-endian head of the key
  const CKey *Key;      // points into the sealed map slot
  const void *Entry;    // CGenRecord*, the newest version of the key
};

// Scatter geometry shared by the layers and the folds: buckets by the top
// bits of the big-endian prefix, so bucket order is memcmp order of keys.
// Key heads are uniform (txids), and one bucket of a whole flush stays small
// enough to sort cache-hot
static constexpr size_t KvScatterBucketBits = 11;
static constexpr size_t KvScatterBuckets = size_t(1) << KvScatterBucketBits;

// Sort of one scatter bucket into memcmp order of keys, the flush-side half:
// bucket order itself already follows the top bits of the prefix
template<typename CKey>
inline void kvSortBucket(std::vector<CKvSortedRef<CKey>> &scattered, const std::vector<uint32_t> &bounds, size_t b) {
  const uint32_t begin = b ? bounds[b - 1] : 0;
  std::sort(scattered.begin() + begin, scattered.begin() + bounds[b],
            [](const CKvSortedRef<CKey> &l, const CKvSortedRef<CKey> &r) {
    if (l.Prefix != r.Prefix)
      return l.Prefix < r.Prefix;
    return memcmp(l.Key, r.Key, sizeof(CKey)) < 0;
  });
}

// Counting sort of scattered refs into the prefix buckets, in place: after it
// Bounds[i] is the end of bucket i - the ranges the fold reads
template<typename CKey>
inline void kvScatterSort(std::vector<CKvSortedRef<CKey>> &scattered, std::vector<uint32_t> &bounds) {
  static thread_local std::vector<CKvSortedRef<CKey>> scratch;
  scratch.resize(scattered.size());
  bounds.assign(KvScatterBuckets, 0);
  for (const auto &ref: scattered)
    bounds[ref.Prefix >> (64 - KvScatterBucketBits)]++;
  uint32_t sum = 0;
  for (size_t i = 0; i < KvScatterBuckets; i++) {
    const uint32_t n = bounds[i];
    bounds[i] = sum;
    sum += n;
  }
  for (const auto &ref: scattered)
    scratch[bounds[ref.Prefix >> (64 - KvScatterBucketBits)]++] = ref;
  scattered.swap(scratch);
}

// The fold walks the refs in key order while the records they point at lie in
// the arena in insertion order: every entry is a miss to memory, and the next
// addresses are known a dozen iterations ahead
inline void kvPrefetch(const void *p) {
#ifdef _MSC_VER
  _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#else
  __builtin_prefetch(p);
#endif
}

static constexpr uint32_t KvPrefetchDistance = 8;

// Never reset, never reused - a layer dies whole, when the last view or
// reader lets go of it, so a reader chasing an arena pointer can never meet a
// writer rewinding that arena
template<typename CKey>
class CLayer {
public:
  CLayer(size_t arenaBytes, size_t mapCapacity) : Arena_(arenaBytes), Map_(mapCapacity) {}

  // Writer side: one mutator thread ---------------------------------------

  // A generation record; the committed previous version stays on the chain
  // for its readers, an uncommitted one is replaced. The fold keeps
  // may-exist-below monotonic - what the replaced version knew about the
  // layers below cannot be taken back
  void put(const CKey &key, size_t hash, const void *data, size_t size, bool mayExistBelow) {
    putRecord(key, hash, data, size, nullptr, 0, mayExistBelow);
  }

  // Two pieces glued straight in the arena - a "payload plus a few bytes of
  // metadata" caller would otherwise stage and copy the record twice
  void put(const CKey &key, size_t hash, const void *data, size_t size, const void *suffix, size_t suffixSize, bool mayExistBelow) {
    putRecord(key, hash, data, size, suffix, suffixSize, mayExistBelow);
  }

  // The family fold at insert: the new version's payload is computed from the
  // one it replaces (merge sums its delta into the replaced cumulative
  // there). fill(dst, prev) writes exactly size bytes; prev may be null.
  // Legal because the generation is uncommitted: no reader sees the gap
  template<typename F>
  void putWith(const CKey &key, size_t hash, size_t size, F &&fill) {
    Map_.updateWith(key, hash, [&](void *prevRaw) -> void* {
      const CGenRecord *prev = static_cast<const CGenRecord*>(prevRaw);
      CGenRecord *rec = writeRecord(chainOf(prev), 0, nullptr, size);
      fill(rec + 1, prev);
      return rec;
    });
  }

  void erase(const CKey &key, size_t hash) {
    Map_.updateWith(key, hash, [&](void *prevRaw) -> void* {
      const CGenRecord *prev = static_cast<const CGenRecord*>(prevRaw);
      // No version in this layer, or one that admits layers below: the disk
      // may hold the key and the tombstone must survive the flush. A value
      // born here dies with its pair - a tombstone without the flag
      const bool meb = !prev || prev->mayExistBelow();
      return writeRecord(chainOf(prev), CGenRecord::TombstoneFlag | (meb ? CGenRecord::MayExistBelowFlag : 0), nullptr, 0);
    });
  }

  // Raw arena bytes with the layer's lifetime, for family payloads that
  // outgrow one record (array tail buffers). 8-aligned like the records
  void *allocRaw(size_t size) {
    return Arena_.alloc((size + 7) & ~static_cast<size_t>(7));
  }

  // The abort path of a unit: every head its generation wrote is popped back
  // to the committed version - out of the map when the key was born here.
  // The records stay behind as arena garbage; the generation was never torn
  // off, so no reader ever met them. Live era only, mutator thread
  void discardUncommitted() {
    Map_.replaceEach([this](void *value) -> void* {
      const CGenRecord *rec = static_cast<const CGenRecord*>(value);
      if (!rec || rec->Gen != Gen_)
        return value;
      return const_cast<void*>(static_cast<const void*>(rec->prev()));
    });
  }

  // The snapshot: the generation being filled becomes visible to whoever is
  // handed this number. In the engine the revision swap is what publishes it;
  // the release here only keeps standalone readers safe
  uint32_t tearOff() {
    const uint32_t gen = Gen_++;
    Watermark_.store(gen, std::memory_order_release);
    return gen;
  }

  uint32_t watermark() const { return Watermark_.load(std::memory_order_acquire); }

  // Reader side -------------------------------------------------------------

  // Wait-free at any time: the newest version of the key at watermark W.
  // nullptr = nothing this layer knew at W - fall through to the layer below.
  // A tombstone is a result like any other
  const CGenRecord *find(const CKey &key, size_t hash, uint32_t watermark) const {
    const CGenRecord *rec = static_cast<const CGenRecord*>(Map_.find(key, hash));
    while (rec && rec->Gen > watermark)
      rec = rec->prev();
    return rec;
  }

  // The uncommitted generation's own write for the key, mutator side: the
  // head only while it is this generation - an older head belongs to a
  // committed unit below and is not the caller's to touch
  const CGenRecord *findOwn(const CKey &key, size_t hash) const {
    const CGenRecord *rec = static_cast<const CGenRecord*>(Map_.find(key, hash));
    return rec && rec->Gen == Gen_ ? rec : nullptr;
  }

  // Live scan at a watermark, legal under the mutator (rank index folds read
  // the active era this way): per key, the newest record at or below W. A key
  // inserted during the walk is missed - it carries a generation above any
  // watermark the caller can hold, so the answer is exact for W
  template<typename F>
  void forEachAt(uint32_t watermark, F &&fn) const {
    Map_.forEachConcurrent([watermark, &fn](const CKey &key, void *value) {
      const CGenRecord *rec = static_cast<const CGenRecord*>(value);
      while (rec && rec->Gen > watermark)
        rec = rec->prev();
      if (rec)
        fn(key, *rec);
    });
  }

  size_t used() const { return Map_.used(); }

  // What this layer holds - the backpressure currency
  size_t bytes() { return Arena_.size(); }

  // The layer in the engine -------------------------------------------------

  // intrusive_ptr contract, then what the commit and the freeze fill in: the
  // tip this layer ends at and its sealed size. The flusher meets a layer
  // only through a published revision, so none of it is atomic
  mutable std::atomic<uintptr_t> Refs_{0};
  uintptr_t ref_fetch_add(uintptr_t n) const { return Refs_.fetch_add(n, std::memory_order_relaxed); }
  uintptr_t ref_fetch_sub(uintptr_t n) const { return Refs_.fetch_sub(n, std::memory_order_acq_rel); }

  BC::Proto::BlockHashTy Stamp;
  size_t Bytes = 0;

  // The freeze handshake: zero while the layer is the live era, its place in
  // the freeze order once sealed. The flusher walks published stacks that
  // still hold the era, so the store is the release its contents ride on
  std::atomic<uint64_t> Seq{0};

  // The layer's records scattered into prefix buckets, built once by the
  // flusher on a sealed layer. The fold sorts bucket by bucket, so the whole
  // batch never sorts as one pile; BatchBytesBound sizes the write batch
  // without a headers walk
  void buildScattered() const {
    if (!Scattered.empty() || Map_.used() == 0)
      return;
    Scattered.reserve(Map_.used());
    Map_.forEachCurrent([this](const CKey &key, void *value) {
      // a discarded unit popped the key out of the map
      if (!value)
        return;
      uint64_t prefix = 0;
      memcpy(&prefix, &key, std::min(sizeof(prefix), sizeof(CKey)));
      Scattered.push_back({xhtobe(prefix), &key, value});
    });

    // Key + record framing per entry, values at most what the arena holds:
    // a bound, not a headers walk
    BatchBytesBound = Map_.used() * (sizeof(CKey) + 12) + Bytes;
    kvScatterSort(Scattered, Bounds);
  }

  mutable std::vector<CKvSortedRef<CKey>> Scattered;
  mutable std::vector<uint32_t> Bounds;
  mutable size_t BatchBytesBound = 0;

private:
  // A committed previous version may still have a reader and goes on the
  // chain; an uncommitted one is replaced and links past itself
  const CGenRecord *chainOf(const CGenRecord *prev) const {
    if (!prev)
      return nullptr;
    return prev->Gen != Gen_ ? prev : prev->prev();
  }

  CGenRecord *writeRecord(const CGenRecord *chain, uint32_t flags, const void *data, size_t size) {
    // 8-aligned records, so the back pointer sits whole right before its header
    const size_t prefix = chain ? sizeof(void*) : 0;
    uint8_t *base = static_cast<uint8_t*>(Arena_.alloc((prefix + sizeof(CGenRecord) + size + 7) & ~static_cast<size_t>(7)));
    if (chain) {
      memcpy(base, &chain, sizeof(void*));
      base += sizeof(void*);
      flags |= CGenRecord::HasPrevFlag;
    }
    CGenRecord *rec = reinterpret_cast<CGenRecord*>(base);
    rec->Gen = Gen_;
    rec->SizeAndFlags = static_cast<uint32_t>(size) | flags;
    // null data = the caller fills the payload itself (putWith): legal while
    // the generation is uncommitted, no reader can hold a watermark this high
    if (size && data)
      memcpy(rec + 1, data, size);
    return rec;
  }

  void putRecord(const CKey &key, size_t hash, const void *data, size_t size, const void *suffix, size_t suffixSize, bool mayExistBelow) {
    Map_.updateWith(key, hash, [&](void *prevRaw) -> void* {
      const CGenRecord *prev = static_cast<const CGenRecord*>(prevRaw);
      // A born-dead tombstone adds nothing - that pair lived and died here
      const bool meb = mayExistBelow || (prev && prev->mayExistBelow());
      CGenRecord *rec = writeRecord(chainOf(prev), meb ? CGenRecord::MayExistBelowFlag : 0, nullptr, size + suffixSize);
      uint8_t *value = reinterpret_cast<uint8_t*>(rec + 1);
      memcpy(value, data, size);
      if (suffixSize)
        memcpy(value + size, suffix, suffixSize);
      return rec;
    });
  }

  MLog Arena_;
  CSwmrHashMap<CKey> Map_;

  // The mutator's own copy of "the generation being filled": the watermark is
  // what readers pair with, the writer never pays an atomic load
  uint32_t Gen_ = 1;
  std::atomic<uint32_t> Watermark_{0};
};

}
}
