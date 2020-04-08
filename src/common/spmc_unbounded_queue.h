// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "dcas.h"
#include <stddef.h>
#include <atomic>
#include <vector>

template<typename T> bool lfAdapterIsNull(const T &data);
template<typename T> void lfAdapterSetNull(T &data);

template<typename T>
class alignas(16) spmc_unbounded_queue {
private:
  struct PositionPair {
    size_t ReadPos;
    size_t KnownWritePos;

    std::atomic<size_t> &atomicReadPos() { return *reinterpret_cast<std::atomic<size_t>*>(&ReadPos); }
    std::atomic<size_t> &atomicKnownWritePos() { return *reinterpret_cast<std::atomic<size_t>*>(&KnownWritePos); }
  };

  T *getPtr(size_t dataIdx, size_t pos) {
    return &Data_[dataIdx][pos % (InitialSize_ << dataIdx)];
  }

  size_t getSize(size_t dataidx) {
    return InitialSize_ << dataidx;
  }

private:
  PositionPair ConsumerPart_;
  size_t WritePos_ = 0;
  size_t InitialSize_ = 0;

  T *Data_[64] = {nullptr};
  std::atomic<size_t> DataIdx_ = 0;
  std::atomic<size_t> DataIdxNext_ = 0;

  void grow(size_t newDataIdx) {
    size_t oldSize = InitialSize_ << DataIdx_;
    size_t newSize = InitialSize_ << newDataIdx;
    T *data = new T[newSize];
    Data_[newDataIdx] = data;
    DataIdxNext_.store(newDataIdx);
    for (size_t i = WritePos_ - oldSize; i != WritePos_; i++)
      data[i % newSize] = Data_[DataIdx_][i % oldSize];
    for (size_t i = WritePos_; i != WritePos_ + newSize - oldSize; i++)
      lfAdapterSetNull(data[i % newSize]);
    DataIdx_.store(newDataIdx);
  }

public:
  spmc_unbounded_queue(size_t initialSize) {
    InitialSize_ = initialSize;

    T *data = new T[initialSize];
    for (size_t i = 0; i < initialSize; i++)
      lfAdapterSetNull(data[i]);
    Data_[0] = data;

    ConsumerPart_.ReadPos = 0;
    ConsumerPart_.KnownWritePos = 0;
  }

  void enqueue(const T &data) {
    T &ptr = getPtr(WritePos_);
    if (lfAdapterIsNull(ptr)) {
      ptr = data;
      std::atomic_thread_fence(std::memory_order_seq_cst);
      WritePos_++;
    } else {
      // Do reallocation
      grow(DataIdx_ + 1);
      getPtr(WritePos_) = data;
      std::atomic_thread_fence(std::memory_order_seq_cst);
      WritePos_++;
    }
  }

  void enqueue(const std::vector<T> &data) {
    size_t newDataIdx = 0;

    {
      size_t dataIdx = DataIdx_;
      T *ptr = Data_[dataIdx];
      size_t size = InitialSize_ << dataIdx;

      if (data.size() <= size) {
        // Check N queue cells
        for (size_t i = WritePos_, ie = WritePos_ + data.size(); i != ie; i++) {
          if (!lfAdapterIsNull(ptr[i % size])) {
            newDataIdx = DataIdx_ + 1;
            break;
          }
        }
      } else {
        newDataIdx = DataIdx_ + 1;
        while ((InitialSize_ << newDataIdx) < data.size())
          newDataIdx++;
      }
    }

    if (newDataIdx)
      grow(newDataIdx);

    {
      size_t dataIdx = DataIdx_;
      T *ptr = Data_[dataIdx];
      size_t size = InitialSize_ << dataIdx;
      size_t pos = WritePos_;
      for (const auto &element: data)
        ptr[pos++ % size] = element;
      std::atomic_thread_fence(std::memory_order_seq_cst);
      WritePos_ += data.size();
    }
  }

  bool dequeue(T &data) {
    for (;;) {
      size_t pos = ConsumerPart_.atomicReadPos().fetch_add(1);
      size_t knownWritePos = ConsumerPart_.atomicKnownWritePos().load();
      if (pos < knownWritePos) {
        unsigned dataIdx = DataIdx_;
        T *ptr = getPtr(dataIdx, pos);
        data = *ptr;
        for (;;) {
          lfAdapterSetNull(ptr);
          unsigned dataIdxNext = DataIdxNext_.load();
          if (dataIdxNext == dataIdx)
            break;
          dataIdx = dataIdxNext;
          ptr = getPtr(dataIdx, pos);
        }

        return true;
      } else if (knownWritePos != WritePos_) {
        // ReadPos_ <- KnownWritePos_
        // KnownWritePos_ <- WritePos_
        PositionPair oldPos = {pos+1, knownWritePos};
        PositionPair newPos = {knownWritePos+1, WritePos_};
        if (dblCompareAndExchange(&ConsumerPart_, &oldPos, &newPos)) {
          unsigned dataIdx = DataIdx_;
          T *ptr = getPtr(dataIdx, pos);

          data = *ptr;
          for (;;) {
            lfAdapterSetNull(ptr);
            unsigned dataIdxNext = DataIdxNext_.load();
            if (dataIdxNext == dataIdx)
              break;
            dataIdx = dataIdxNext;
            ptr = getPtr(dataIdx, pos);
          }

          return true;
        }
      } else {
        return false;
      }
    }
  }

  bool dequeue(std::vector<T> &out, size_t size) {
    for (;;) {
      size_t pos = ConsumerPart_.atomicReadPos().fetch_add(size);
      size_t knownWritePos = ConsumerPart_.atomicKnownWritePos().load();
      if (pos < knownWritePos) {
        size_t dataIdx = DataIdx_;
        T *dataPtr = getPtr(dataIdx, 0);
        size_t dataSize = getSize(dataIdx);
        size_t copySize = std::min(knownWritePos - pos, size);

        out.resize(copySize);
        for (size_t i = 0; i < copySize; i++)
          out[i] = dataPtr[(pos+i) % dataSize];

        for (;;) {
          for (size_t i = 0; i < copySize; i++)
            lfAdapterSetNull(dataPtr[(pos+i) % dataSize]);
          size_t dataIdxNext = DataIdxNext_.load();
          if (dataIdxNext == dataIdx)
            break;
          dataIdx = dataIdxNext;
          dataPtr = getPtr(dataIdx, 0);
          dataSize = getSize(dataIdx);
        }

        return true;
      } else if (knownWritePos != WritePos_) {
        // ReadPos_ <- KnownWritePos_
        // KnownWritePos_ <- WritePos_
        size_t writePos = WritePos_;
        size_t newReadPos = knownWritePos;
        PositionPair oldPos = {pos+size, knownWritePos};
        PositionPair newPos = {knownWritePos+size, writePos};
        if (dblCompareAndExchange(&ConsumerPart_, &oldPos, &newPos)) {
          size_t dataIdx = DataIdx_;
          T *dataPtr = getPtr(dataIdx, 0);
          size_t dataSize = getSize(dataIdx);
          size_t copySize = std::min(writePos - newReadPos, size);

          out.resize(copySize);
          for (size_t i = 0; i < copySize; i++)
            out[i] = dataPtr[(newReadPos+i) % dataSize];

          for (;;) {
            for (size_t i = 0; i < copySize; i++)
              lfAdapterSetNull(dataPtr[(newReadPos+i) % dataSize]);
            size_t dataIdxNext = DataIdxNext_.load();
            if (dataIdxNext == dataIdx)
              break;
            dataIdx = dataIdxNext;
            dataPtr = getPtr(dataIdx, 0);
            dataSize = getSize(dataIdx);
          }

          return true;
        }
      } else {
        return false;
      }
    }
  }

  bool empty() {
    return ConsumerPart_.atomicReadPos().load(std::memory_order_relaxed) >= WritePos_;
  }
};
