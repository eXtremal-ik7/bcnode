#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <atomic>

// "Infinite life time" - this stream don't free memory on grow, so all pointers to
// data inside stream still valid until object will be destroyed
class MLog {
public:
  MLog(size_t initialSize) {
    std::fill(AllocatedMemory_, AllocatedMemory_+64, nullptr);

    InitialSize_ = initialSize;
    uint8_t *data = new uint8_t[InitialSize_];
    AllocatedMemory_[0] = data;
    AllocationCount_ = 1;
    GenerationId_ = 0;
  }

  MLog() : MLog(1u << 20) {}

  ~MLog() {
    for (unsigned i = 0; i < AllocationCount_; i++)
      delete[] AllocatedMemory_[i];
  }

  void *begin() { return AllocatedMemory_[AllocationCount_ - 1]; }
  void *ptr() { return AllocatedMemory_[AllocationCount_ - 1] + Offset_; }

  void *alloc(size_t size) {
    // get current buffer size
    size_t currentSize = InitialSize_ << (AllocationCount_ - 1);
    size_t requiredSize = Offset_ + size;
    if (requiredSize > currentSize) {
      // count from begin of new buffer
      requiredSize = size;

     // Calculate new size
      unsigned newAllocationCount = AllocationCount_ + 1;
      size_t newSize = currentSize * 2;
      while (requiredSize > newSize) {
        newAllocationCount++;
        newSize *= 2;
      }

      uint8_t *newData = new uint8_t[newSize];
      memcpy(newData, AllocatedMemory_[AllocationCount_ - 1], Offset_);

      AllocatedMemory_[newAllocationCount-1] = newData;
      AllocationCount_ = newAllocationCount;
    }

    size_t offset = Offset_;
    Offset_ += size;
    return AllocatedMemory_[AllocationCount_ - 1] + offset;
  }

  template<typename T> T *alloc() { return static_cast<T*>(alloc(sizeof(T))); }

  void rollback(size_t size) {
    // Saturation substract
    Offset_ -= std::min(Offset_, size);
  }

  uint64_t generationId() { return GenerationId_.load(std::memory_order_seq_cst); }

  void reset() {
    GenerationId_.fetch_add(1, std::memory_order_seq_cst);
    Offset_ = 0;
  }

  size_t size() { return Offset_; }

private:
  uint8_t *AllocatedMemory_[64];
  uint32_t AllocationCount_ = 0;
  size_t InitialSize_ = 0;
  size_t Offset_ = 0;
  std::atomic<uint64_t> GenerationId_;
};

