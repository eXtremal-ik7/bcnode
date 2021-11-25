#pragma once

#include <algorithm>
#include <atomic>
#include <stddef.h>
#include <string.h>

namespace HashMap {
namespace SPMC {

template<typename KeyTy, typename ValueTy, typename StorageTy>
class HashMap {
public:
  HashMap(const StorageTy &storage, size_t initialTableSize = 0) : Storage_(storage) {
    std::fill(AllocatedMemory_, AllocatedMemory_+64, nullptr);

    InitialSize_ = initialTableSize ? initialTableSize : DefaultTableSize;
    std::atomic<uint64_t> *table = new std::atomic<uint64_t>[InitialSize_];
    std::fill(table, table+InitialSize_, 0xFFFFFFFFFFFFFFFFULL);

    AllocatedMemory_[0] = table;
    AllocationCount_ = 1;
  }

  ~HashMap() {
    for (unsigned i = 0; i < AllocationCount_; i++)
      delete[] AllocatedMemory_[i];
  }

  ValueTy *find(const KeyTy &key) {
    CBuffer table = getHashTable();
    for (unsigned i = 0; i < MaxCollisionCount; i++) {
      size_t index = ValueTy::getHash(key, i) % table.Size;
      uint64_t ref = table.Table[index];
      uint32_t generationId = ref >> 32;
      uint32_t allocationId = ref;
      if (allocationId == EmptyValue)
        continue;

      ValueTy *value = ValueTy::locate(Storage_, allocationId);
      if (value &&
          value->loadGenerationId(Storage_, std::memory_order_relaxed) == generationId &&
          value->key() == key)
        return value;
    }

    return 0;
  }

  bool extract(const KeyTy &key, ValueTy &result) {
    CBuffer table = getHashTable();
    for (unsigned i = 0; i < MaxCollisionCount; i++) {
      size_t index = ValueTy::getHash(key, i) % table.Size;
      uint64_t ref = table.Table[index];
      uint32_t generationId = ref >> 32;
      uint32_t allocationId = ref;
      if (allocationId == EmptyValue)
        continue;

      ValueTy *value = ValueTy::locate(Storage_, allocationId);
      if (!value ||
          value->loadGenerationId(Storage_, std::memory_order_relaxed) != generationId &&
          value->extractKey() != key)
        continue;

      // Deep copy of value
      result = *value;

      if (value->loadGenerationId(Storage_, std::memory_order_seq_cst) != generationId)
        continue;
      return true;
    }

    return false;
  }

  bool insert(const ValueTy *value, uint32_t allocationId) {
    do {
      CBuffer table = getHashTable();
      for (unsigned i = 0; i < MaxCollisionCount; i++) {
        size_t index = ValueTy::getHash(value->extractKey(), i) % table.Size;
        uint32_t storedAllocationId = table.Table[index] & 0xFFFFFFFF;
        if (storedAllocationId != EmptyValue)
          continue;

        uint64_t newRef = (static_cast<uint64_t>(value->loadGenerationId(Storage_, std::memory_order_relaxed)) << 32) | allocationId;
        table.Table[index].store(newRef, std::memory_order_relaxed);
        return true;
      }
    } while (grow());

    return false;
  }

  void erase(const KeyTy &key) {
    CBuffer table = getHashTable();
    for (unsigned i = 0; i < MaxCollisionCount; i++) {
      size_t index = ValueTy::getHash(key, i) % table.Size;
      uint64_t ref = table.Table[index];
      uint32_t generationId = ref >> 32;
      uint32_t allocationId = ref;
      ValueTy *value = ValueTy::locate(Storage_, allocationId);
      if (value && value->extractKey() == key) {
        value->atomicUpdateGeneration();
        table.Table[index].store((static_cast<uint64_t>(generationId) << 32) | EmptyValue, std::memory_order_relaxed);
        break;
      }
    }
  }

  size_t tableSize() const { return InitialSize_ << (AllocationCount_ - 1); }

private:
  struct CBuffer {
    std::atomic<uint64_t> *Table;
    size_t Size;
  };

private:
  const StorageTy &Storage_;
  std::atomic<uint64_t> *AllocatedMemory_[64];
  std::atomic<size_t> AllocationCount_;
  size_t InitialSize_ = 0;

private:
  CBuffer getHashTable() {
    CBuffer table;
    size_t tableIdx = AllocationCount_.load(std::memory_order_relaxed) - 1;
    table.Table = AllocatedMemory_[tableIdx];
    table.Size = InitialSize_ << tableIdx;
    return table;
  }

  bool grow() {
    for (size_t count = AllocationCount_+1; count <= 64; count++) {
      if (tryGrow(count))
        return true;
    }

    return false;
  }

  bool tryGrow(size_t newAllocationCount) {
    CBuffer oldTable = getHashTable();

    size_t newSize = InitialSize_ << (newAllocationCount - 1);
    std::atomic<uint64_t> *newTable = new std::atomic<uint64_t>[newSize];
    std::fill(newTable, newTable+newSize, 0xFFFFFFFFFFFFFFFFULL);

    // Move all values to new hash table
    for (size_t i = 0; i < oldTable.Size; i++) {
      uint64_t ref = oldTable.Table[i];
      uint32_t generationId = ref >> 32;
      uint32_t allocationId = ref;
      if (allocationId == EmptyValue)
        continue;

      ValueTy *value = ValueTy::locate(Storage_, allocationId);
      if (!value)
        continue;

      bool insertionSuccessful = false;
      KeyTy key = value->extractKey();
      for (unsigned j = 0; j < MaxCollisionCount; j++) {
        size_t newIndex = ValueTy::getHash(key, j) % newSize;
        uint32_t newAllocationId = newTable[newIndex] & 0xFFFFFFFF;
        if (newAllocationId == EmptyValue) {
          uint64_t newRef = (static_cast<uint64_t>(generationId) << 32) | allocationId;
          newTable[newIndex].store(newRef, std::memory_order_relaxed);
          insertionSuccessful = true;
          break;
        }
      }

      if (!insertionSuccessful) {
        delete[] newTable;
        return false;
      }
    }

    AllocatedMemory_[newAllocationCount-1] = newTable;
    AllocationCount_.store(newAllocationCount, std::memory_order_seq_cst);
    return true;
  }

private:
  static constexpr size_t DefaultTableSize = 16;
  static constexpr unsigned MaxCollisionCount = 16;
  static constexpr uint32_t EmptyValue = 0xFFFFFFFFU;
};

}
}
