#pragma once

// Single-writer / multi-reader array over a dense 32-bit index (the block height index). Readers
// are wait-free, the writer never moves what it has published.
//
// Partition i holds 2^(i + InitialLog2) slots, doubling as in asyncio's ConcurrentQueue. The ladder
// covers every 32-bit index, so it cannot be exhausted by construction. The writer allocates a
// partition on its first store and publishes it with a release store; a reader that sees no
// partition sees no element. Slots are atomic: a reader may race the writer on the same index and
// gets the old value or the new one, never a torn one.

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

template<typename T, unsigned InitialLog2 = 12>
class CSwmrArray {
public:
  CSwmrArray() = default;
  CSwmrArray(const CSwmrArray&) = delete;
  CSwmrArray &operator=(const CSwmrArray&) = delete;

  ~CSwmrArray() {
    for (auto &partition: Partitions_)
      delete[] partition.load(std::memory_order_relaxed);
  }

  // Any thread: T{} where nothing was stored
  T get(uint32_t index) const {
    unsigned partition;
    size_t offset;
    locate(index, partition, offset);
    const std::atomic<T> *slots = Partitions_[partition].load(std::memory_order_acquire);
    return slots ? slots[offset].load(std::memory_order_acquire) : T{};
  }

  // The writer only
  void set(uint32_t index, T value) {
    unsigned partition;
    size_t offset;
    locate(index, partition, offset);
    std::atomic<T> *slots = Partitions_[partition].load(std::memory_order_relaxed);
    if (!slots) {
      slots = new std::atomic<T>[size_t(1) << (partition + InitialLog2)]();
      Partitions_[partition].store(slots, std::memory_order_release);
    }
    slots[offset].store(value, std::memory_order_release);
  }

private:
  // Index + 2^InitialLog2 is below 2^(32 + 1): its top bit names the partition
  static constexpr unsigned PartitionsNum = 33 - InitialLog2;

  static void locate(uint32_t index, unsigned &partition, size_t &offset) {
    const uint64_t x = uint64_t(index) + (uint64_t(1) << InitialLog2);
    partition = static_cast<unsigned>(std::bit_width(x)) - 1 - InitialLog2;
    offset = static_cast<size_t>(x - (uint64_t(1) << (partition + InitialLog2)));
  }

  std::atomic<std::atomic<T>*> Partitions_[PartitionsNum] = {};
};
