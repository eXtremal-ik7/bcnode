// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once 

#include "common/intrusive_ptr.h"
#include <stddef.h>
#include <atomic>
#include <limits>
#include <memory>

class SerializedDataCache;

class alignas(512) SerializedDataObject {
private:
  mutable std::atomic<uintptr_t> Refs_ = 0;
  SerializedDataCache *Parent_;
  void *Data_ = nullptr;
  size_t DataSize_ = 0;
  size_t DataMemSize_ = 0;
  void *UnpackedData_ = nullptr;
  size_t UnpackedMemSize_ = 0;

public:
  uintptr_t ref_fetch_add(uintptr_t count) const { return Refs_.fetch_add(count); }
  uintptr_t ref_fetch_sub(uintptr_t count) const { return Refs_.fetch_sub(count); }

  SerializedDataObject(SerializedDataCache *parent,
                       void *data,
                       size_t dataSize,
                       size_t memorySize,
                       void *unpackedData,
                       size_t unpackedMemorySize) :
    Parent_(parent), Data_(data), DataSize_(dataSize), DataMemSize_(memorySize), UnpackedData_(unpackedData), UnpackedMemSize_(unpackedMemorySize) {}

  ~SerializedDataObject();

  void *data() const { return Data_; }
  void *unpackedData() const { return UnpackedData_; }
  size_t size() const { return DataSize_; }
  size_t memorySize() const { return DataMemSize_ + UnpackedMemSize_ + 512; }
};

class SerializedDataCache {
private:
  std::atomic<size_t> Size_ = 0;
  size_t Limit_ = std::numeric_limits<size_t>::max();

public:
  void setLimit(size_t limit) { Limit_ = limit; }
  size_t size() { return Size_.load(std::memory_order_relaxed); }
  bool overflow() { return size() >= Limit_; }

  intrusive_ptr<SerializedDataObject> add(void *data,
                                          size_t dataSize,
                                          size_t memorySize,
                                          void *unpackedData,
                                          size_t unpackedMemorySize) {
    SerializedDataObject *object = new SerializedDataObject(this, data, dataSize, memorySize, unpackedData, unpackedMemorySize);
    Size_.fetch_add(object->memorySize());
    return intrusive_ptr<SerializedDataObject>(object);
  }

  void remove(SerializedDataObject *object) {
    Size_.fetch_sub(object->memorySize(), std::memory_order_relaxed);
  }
};
