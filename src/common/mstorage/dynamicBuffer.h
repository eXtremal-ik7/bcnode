#pragma once

#include <stddef.h>
#include <stdint.h>
#include <atomic>

namespace MStorage {

// "Infinite life time" - this stream don't free memory on grow, so all pointers to
// data inside stream still valid until object will be destroyed
class DynamicBuffer {
public:
  struct Pointer {
    void *Data;
    uint32_t AllocId;
  };

public:
  DynamicBuffer(size_t initialSize);
  ~DynamicBuffer();

  Pointer alloc(size_t size);
  void *get(uint32_t allocId) const;
  uint32_t loadGenerationId(std::memory_order order) const { return GlobalGenerationId_.load(order); }
  void updateGenerationId(uint32_t generationId) { GlobalGenerationId_.store(generationId, std::memory_order_seq_cst); }

private:
  struct CBuffer {
    uint8_t *Data;
    size_t Size;
  };

private:
  uint8_t *AllocatedMemory_[64];
  std::atomic<size_t> AllocationCount_;
  size_t InitialSize_ = 0;
  size_t Offset_ = 0;
  std::atomic<uint32_t> GlobalGenerationId_;

private:
  void grow();
  CBuffer getBuffer() const {
    CBuffer buffer;
    size_t bufferIdx = AllocationCount_.load(std::memory_order_relaxed) - 1;
    buffer.Data = AllocatedMemory_[bufferIdx];
    buffer.Size = InitialSize_ << bufferIdx;
    return buffer;
  }
};

}
