// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// The arena of a layer: chunks mapped as they are needed, never moved, never
// released before the layer dies. Nothing is ever copied, so an address is
// fixed from the moment it is handed out - which is what lets a reader chase a
// record while the writer keeps allocating.
//
// The offset space is flat and independent of where the chunks landed: one slot
// of the table owns one fixed 128 MB window of it, so an offset resolves in a
// single lookup no matter how it was mapped. An allocation too big for a chunk
// takes a mapping of its own and as many slots as it spans - what runs out at
// 32 GB is the offset space, and an era is three orders of magnitude below it.
//
// Slots_ is read without a lock: the writer fills a slot before it publishes
// anything living in it, and a reader reaches an offset only through the map
// slot that published it, so the release/acquire pair that carries the record
// carries the chunk pointer too.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace dbengine {

class CArena {
public:
  // 8-byte units, so a 32-bit reference reaches 32 GB. Records are aligned to a
  // unit anyway - their headers need it
  using CRef = uint32_t;
  static constexpr size_t UnitBits = 3;
  static constexpr size_t ChunkBits = 27;                    // 128 MB
  static constexpr size_t ChunkSize = size_t(1) << ChunkBits;
  static constexpr size_t SlotsNum = 256;                    // 32 GB of offset space
  static constexpr CRef NullRef = 0;

  // The size a caller expects the arena to hold picks nothing: chunks are the
  // same either way. Kept for the callers that know their era
  explicit CArena(size_t = 0) {
    for (size_t i = 0; i < SlotsNum; i++)
      Slots_[i] = nullptr;
    openChunk();
    // Unit zero is never handed out, so a null reference stays distinguishable
    Cursor_ += size_t(1) << UnitBits;
  }

  CArena(const CArena&) = delete;
  CArena &operator=(const CArena&) = delete;

  ~CArena() {
    for (const CMapping &mapping: Mappings_)
      release(mapping.Base, mapping.Bytes);
  }

  void *alloc(size_t size) { return resolve(allocRef(size)); }

  // The allocation as an offset. Out of offset space or out of memory ends the
  // process: neither has an answer the caller could act on
  CRef allocRef(size_t size) {
    const size_t bytes = (size + (size_t(1) << UnitBits) - 1) & ~((size_t(1) << UnitBits) - 1);
    if (bytes > ChunkSize)
      return allocOversized(bytes);

    // The tail of a chunk is left behind rather than split: an allocation lives
    // inside one mapping, and the next chunk opens on a slot of its own
    if (Cursor_ + bytes > ChunkEnd_)
      openChunk();

    const size_t offset = Cursor_;
    Cursor_ += bytes;
    Used_ += bytes;
    return static_cast<CRef>(offset >> UnitBits);
  }

  void *resolve(CRef ref) const {
    const size_t offset = static_cast<size_t>(ref) << UnitBits;
    return Slots_[offset >> ChunkBits] + (offset & (ChunkSize - 1));
  }

  // What the records took, and what the mappings under them cost
  size_t size() const { return Used_; }
  size_t capacity() const { return Mapped_; }

private:
  struct CMapping {
    uint8_t *Base;
    size_t Bytes;
  };

  void openChunk() {
    const size_t slot = NextSlot_++;
    if (slot >= SlotsNum)
      fatal("arena out of offset space");
    uint8_t *base = map(ChunkSize);
    Slots_[slot] = base;
    Mappings_.push_back({base, ChunkSize});
    Cursor_ = slot << ChunkBits;
    ChunkEnd_ = Cursor_ + ChunkSize;
  }

  // One mapping, with the slots it spans pointing into it consecutively, so
  // resolve() cannot tell it from an ordinary chunk. The small cursor is left
  // where it was: only offset space is spent here, never mapped bytes
  CRef allocOversized(size_t bytes) {
    const size_t slots = (bytes + ChunkSize - 1) >> ChunkBits;
    const size_t first = NextSlot_;
    if (first + slots > SlotsNum)
      fatal("arena out of offset space");
    NextSlot_ += slots;

    uint8_t *base = map(slots << ChunkBits);
    for (size_t i = 0; i < slots; i++)
      Slots_[first + i] = base + (i << ChunkBits);
    Mappings_.push_back({base, slots << ChunkBits});
    Used_ += bytes;
    return static_cast<CRef>((first << ChunkBits) >> UnitBits);
  }

  uint8_t *map(size_t bytes) {
#ifdef _WIN32
    void *p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    // Pages arrive on first touch, so an era frozen half way through a chunk
    // costs the half it wrote and not the chunk
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
      p = nullptr;
#endif
    if (!p)
      fatal("arena out of memory");
    Mapped_ += bytes;
    return static_cast<uint8_t*>(p);
  }

  static void release(uint8_t *base, size_t bytes) {
#ifdef _WIN32
    (void)bytes;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, bytes);
#endif
  }

  [[noreturn]] static void fatal(const char *what) {
    fprintf(stderr, "fatal: %s\n", what);
    abort();
  }

private:
  uint8_t *Slots_[SlotsNum];
  std::vector<CMapping> Mappings_;
  size_t NextSlot_ = 0;
  size_t Cursor_ = 0;      // byte offset of the next small allocation
  size_t ChunkEnd_ = 0;    // where the current chunk's window of offsets ends
  size_t Used_ = 0;
  size_t Mapped_ = 0;
};

}
