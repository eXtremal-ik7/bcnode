// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Bounded lock-free MPMC queue (Vyukov ring): FIFO, one CAS per push and per pop, no
// allocation outside init(). The sequence word of a cell says whose turn it is, and it
// publishes the value: a reader that saw its own turn also sees the value written before it.
// malloc plus an explicit init loop, like the SWMR structures: the header is model-checked
template<typename T>
class CMpmcRing {
public:
  ~CMpmcRing() { free(Cells_); }

  // Capacity is rounded up to a power of two
  void init(size_t capacity) {
    size_t size = 1;
    while (size < capacity)
      size *= 2;

    Cells_ = static_cast<SCell*>(malloc(sizeof(SCell) * size));
    if (!Cells_)
      abort();
    for (size_t i = 0; i < size; i++) {
      Cells_[i].Sequence.store(i, std::memory_order_relaxed);
      Cells_[i].Value = nullptr;
    }

    Mask_ = size - 1;
  }

  // false when the ring is full
  bool push(T *value) {
    uint64_t pos = EnqueuePos_.load(std::memory_order_relaxed);
    for (;;) {
      SCell &cell = Cells_[pos & Mask_];
      int64_t diff = static_cast<int64_t>(cell.Sequence.load(std::memory_order_acquire)) - static_cast<int64_t>(pos);
      if (diff == 0) {
        if (EnqueuePos_.compare_exchange_weak(pos, pos+1, std::memory_order_relaxed)) {
          cell.Value = value;
          cell.Sequence.store(pos+1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = EnqueuePos_.load(std::memory_order_relaxed);
      }
    }
  }

  // nullptr when the ring is empty, or when a concurrent push has claimed a cell but has
  // not published it yet: the caller retries
  T *pop() {
    uint64_t pos = DequeuePos_.load(std::memory_order_relaxed);
    for (;;) {
      SCell &cell = Cells_[pos & Mask_];
      int64_t diff = static_cast<int64_t>(cell.Sequence.load(std::memory_order_acquire)) - static_cast<int64_t>(pos+1);
      if (diff == 0) {
        if (DequeuePos_.compare_exchange_weak(pos, pos+1, std::memory_order_relaxed)) {
          T *value = cell.Value;
          cell.Sequence.store(pos + Mask_ + 1, std::memory_order_release);
          return value;
        }
      } else if (diff < 0) {
        return nullptr;
      } else {
        pos = DequeuePos_.load(std::memory_order_relaxed);
      }
    }
  }

  // No cell is claimed for push: a failed pop with this true means the ring is really empty
  bool empty() const {
    return DequeuePos_.load(std::memory_order_relaxed) >= EnqueuePos_.load(std::memory_order_relaxed);
  }

private:
  struct SCell {
    std::atomic<uint64_t> Sequence;
    T *Value;
  };

private:
  SCell *Cells_ = nullptr;
  size_t Mask_ = 0;
  alignas(64) std::atomic<uint64_t> EnqueuePos_ = 0;
  alignas(64) std::atomic<uint64_t> DequeuePos_ = 0;
};
