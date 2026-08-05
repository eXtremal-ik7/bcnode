// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <atomic>
#include <cstdint>

template<typename T>
static inline T *WaitPtr() { return reinterpret_cast<T*>(0ULL - 1); }

template<typename T, unsigned tagSize> class atomic_tagged_ptr;

template<typename T, unsigned tagSize>
class tagged_ptr {
private:
  T *_data;

private:
  static constexpr uintptr_t DataMask = (static_cast<uintptr_t>(1) << tagSize) - 1;
  static constexpr uintptr_t PointerMask = ~DataMask;

public:
  static T *make(T *data, uintptr_t tag) { return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(data) | tag); }
  static T *extract_ptr(T *data) { return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(data) & PointerMask); }
  static uintptr_t extract_tag(T *data) { return reinterpret_cast<uintptr_t>(data) & DataMask; }


public:
  tagged_ptr() : _data(nullptr) {}
  tagged_ptr(T *data) : _data(make(data, 0)) {}
  tagged_ptr(T *data, uintptr_t tag) : _data(make(data, tag)) {}

  // accessors
  T *data() const { return _data; }
  T *pointer() const { return extract_ptr(_data); }
  uintptr_t tag() { return extract_tag(_data); }
  void set(T *ptr, uintptr_t data) { _data = make(ptr, data); }

  friend class atomic_tagged_ptr<T, tagSize>;
};

template<typename T, unsigned tagSize>
class atomic_tagged_ptr {
private:
  // Keep the tagged representation as an integer. Accessing an atomic<T*>
  // through atomic<uintptr_t> in fetch_add() violates the atomic object's type
  // and lifetime; using the integer type directly makes the RMW well-defined.
  std::atomic<uintptr_t> _data;

public:
  atomic_tagged_ptr() : _data(0) {}
  atomic_tagged_ptr(T *ptr) : _data(reinterpret_cast<uintptr_t>(ptr)) {}
  atomic_tagged_ptr(T *ptr, uintptr_t data) :
    _data(reinterpret_cast<uintptr_t>(tagged_ptr<T, tagSize>::make(ptr, data))) {}
  atomic_tagged_ptr(const tagged_ptr<T, tagSize> &ptr) :
    _data(reinterpret_cast<uintptr_t>(ptr._data)) {}
  atomic_tagged_ptr<T, tagSize> &operator=(const atomic_tagged_ptr &data) {
    _data.store(data._data.load());
    return *this;
  }
  atomic_tagged_ptr<T, tagSize> &operator=(atomic_tagged_ptr &&data) {
    _data.store(data._data.load());
    return *this;
  }

  // accessors
  tagged_ptr<T, tagSize> load() { return tagged_ptr<T, tagSize>(reinterpret_cast<T*>(_data.load())); }
  T *data() const { return reinterpret_cast<T*>(_data.load()); }
  T *pointer() const { return tagged_ptr<T, tagSize>::extract_ptr(reinterpret_cast<T*>(_data.load())); }
  uintptr_t tag() { return tagged_ptr<T, tagSize>::extract_tag(reinterpret_cast<T*>(_data.load())); }
  void set(T *ptr, uintptr_t data) {
    _data.store(reinterpret_cast<uintptr_t>(tagged_ptr<T, tagSize>::make(ptr, data)));
  }

  // atomic accessors
  tagged_ptr<T, tagSize> exchange(const tagged_ptr<T, tagSize> &data) {
    return tagged_ptr<T, tagSize>(reinterpret_cast<T*>(_data.exchange(reinterpret_cast<uintptr_t>(data._data))));
  }
  tagged_ptr<T, tagSize> exchange(T *ptr, uintptr_t data) {
    return tagged_ptr<T, tagSize>(reinterpret_cast<T*>(_data.exchange(
      reinterpret_cast<uintptr_t>(tagged_ptr<T, tagSize>::make(ptr, data)))));
  }
  tagged_ptr<T, tagSize> fetch_add(uintptr_t data) {
    T *result = reinterpret_cast<T*>(_data.fetch_add(data));
    return tagged_ptr<T, tagSize>(result);
  }

  bool compare_and_exchange(tagged_ptr<T, tagSize> &oldValue, tagged_ptr<T, tagSize> newValue) {
    uintptr_t oldData = reinterpret_cast<uintptr_t>(oldValue._data);
    if (_data.compare_exchange_strong(oldData, reinterpret_cast<uintptr_t>(newValue._data)))
      return true;
    oldValue._data = reinterpret_cast<T*>(oldData);
    return false;
  }
};
