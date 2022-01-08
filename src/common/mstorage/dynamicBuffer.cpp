#include "dynamicBuffer.h"
#include <string.h>
#include <algorithm>

namespace MStorage {

DynamicBuffer::DynamicBuffer(size_t initialSize)
{
  std::fill(AllocatedMemory_, AllocatedMemory_+64, nullptr);

  InitialSize_ = initialSize;
  uint8_t *data = new uint8_t[InitialSize_];
  AllocatedMemory_[0] = data;
  AllocationCount_ = 1;
}

DynamicBuffer::~DynamicBuffer()
{
  for (unsigned i = 0; i < AllocationCount_; i++)
    delete[] AllocatedMemory_[i];
}

DynamicBuffer::Pointer DynamicBuffer::alloc(size_t size)
{
  CBuffer buffer = getBuffer();
  size_t requiredSize = Offset_ + size;
  if (requiredSize > buffer.Size) {
    // Calculate new size
    unsigned newAllocationCount = AllocationCount_.load(std::memory_order_relaxed) + 1;
    size_t newSize = buffer.Size * 2;
    while (requiredSize > newSize) {
      newAllocationCount++;
      newSize *= 2;
    }

    uint8_t *newData = new uint8_t[newSize];
    memcpy(newData, buffer.Data, Offset_);
    buffer.Data = newData;
    AllocatedMemory_[newAllocationCount-1] = newData;
    AllocationCount_.store(newAllocationCount, std::memory_order_seq_cst);
  }

  Pointer pointer;
  pointer.AllocId = Offset_;
  pointer.Data = buffer.Data + Offset_;
  Offset_ += size;
  return pointer;
}

void *DynamicBuffer::get(uint32_t allocId) const
{
  CBuffer buffer = getBuffer();
  return buffer.Data + allocId;
}

}
