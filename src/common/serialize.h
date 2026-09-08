// Copyright (c) 2026 Ivan K.
// Copyright (c) 2026 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// Everything about deserialization that is not a wire format.
//
// There is no common format and no default one: Bitcoin's compact sizes, Hathor's explicit
// big-endian prefixes, RLP and protobuf have nothing in common at the byte level, and each
// lives in its own header with its own Io<T> leaves and its own reader. What they do share is
// this file. Every one of them describes a type once,
//
//   template<typename Op, typename Self> static void io(Op &op, Self &d, ...context...)
//
// and gets five operations out of that one description - measure the encoded size, write,
// read, size an arena, build into it - by walking it with a different operation class.
//
// Reading comes in two forms. Plain deserialization gives every vector its own heap. Unpacking
// walks the shape twice: the first walk only sizes what the variable-size members need, then
// the object and all of them are built in one allocation, which is how a decoded block becomes
// a single pointer that costs one free. The two walks decode the same bytes and the first one
// throws its values away, so a rule that gates on an already-read field works on both, while a
// rule about a vector's contents has to use the element count vec() returns - during measuring
// the vector itself is not filled.

#include "common/xvector.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <new>
#include <string>
#include <type_traits>

static constexpr size_t UnpackAlignment = 8;
static inline size_t aligned(size_t size, size_t align) { return (size + align - 1) & ~(align-1); }

namespace Ser {

static inline size_t alignUp(size_t size) { return aligned(size, UnpackAlignment); }

template<typename T> struct IsXVector : std::false_type {};
template<typename T> struct IsXVector<xvector<T>> : std::true_type {};

// Which of the three reading passes is running. The formats' readers are one class each, not
// three: the passes disagree about exactly one thing - where the variable-size parts go - and
// keeping that in a single place is what stops the three from drifting apart.
enum class EPass {
  Read,     // ordinary deserialization: every vector gets its own heap
  Measure,  // the sizing walk of an unpack: nothing is built, the arena is only counted
  Build     // the same walk again, placing everything into the arena the first one sized
};

// The failure latch, shared by a walk and every sub-walk under it: the first failure wins and
// everything after it is a no-op, so a shape carries no error plumbing of its own. Error is
// optional and stays null on the hot paths - a failed block on the network path is thrown away
// by the caller either way, and composing a message for it would cost an allocation per block.
struct CIoStatus {
  bool Failed = false;
  std::string *Error = nullptr;
  const char *Name = "";
};

// What a reader is on top of the format it speaks: the latch above, and the arena discipline.
class CReaderState {
public:
  explicit CReaderState(CIoStatus &status) : Status_(&status) {}
  CReaderState(CIoStatus &status, size_t *extra) : Status_(&status), Extra_(extra) {}
  CReaderState(CIoStatus &status, uint8_t **arena) : Status_(&status), Arena_(arena) {}

  EPass pass() const { return Extra_ ? EPass::Measure : (Arena_ ? EPass::Build : EPass::Read); }
  bool building() const { return !Extra_; }
  bool failed() const { return Status_->Failed; }
  CIoStatus &status() const { return *Status_; }

  void fail(const char *what) {
    if (Status_->Failed)
      return;
    Status_->Failed = true;
    if (Status_->Error)
      compose(what);
  }

  // A message composed on the spot; only failures ever build one
  void fail(const std::string &what) { fail(what.c_str()); }

  void check(bool ok, const char *what) {
    if (!ok)
      fail(what);
  }

  // Which element of a repeated field is being read, so a message can say which one
  void setElement(size_t index) { Element_ = index; }
  size_t element() const { return Element_; }

  // Arena space for a leaf with variable-size innards of its own - XPM's big numbers are the
  // only one in the tree. Measuring counts it and hands back nothing; building bumps the
  // pointer and hands back the memory; a plain read gets nothing and keeps its own storage.
  uint8_t *arena(size_t size) {
    if (Extra_) {
      *Extra_ += alignUp(size);
      return nullptr;
    }
    if (Arena_) {
      uint8_t *memory = *Arena_;
      *Arena_ += alignUp(size);
      return memory;
    }
    return nullptr;
  }

  // Where the elements of a vector go. Returns the array to fill, or null when this pass
  // builds nothing - the caller then reads each element into a throwaway, because the walk
  // must consume the same bytes whatever it is doing with them.
  template<typename T> T *prepare(xvector<T> &v, size_t n) {
    static_assert(alignof(T) <= UnpackAlignment);
    if (Extra_) {
      *Extra_ += alignUp(n * sizeof(T));
      return nullptr;
    }
    if (Arena_) {
      T *elements = reinterpret_cast<T*>(*Arena_);
      *Arena_ += alignUp(n * sizeof(T));
      for (size_t i = 0; i < n; i++)
        new (elements + i) T;
      v.set(elements, n, n, false);
      return elements;
    }
    v.resize(n);
    return v.data();
  }

protected:
  // A sub-walk inherits the latch, the pass and the element being read
  CReaderState(const CReaderState &parent)
    : Status_(parent.Status_), Extra_(parent.Extra_), Arena_(parent.Arena_), Element_(parent.Element_) {}

  CIoStatus *Status_;
  size_t *Extra_ = nullptr;
  uint8_t **Arena_ = nullptr;
  size_t Element_ = SIZE_MAX;

private:
  void compose(const char *what) {
    *Status_->Error = Status_->Name;
    if (Element_ != SIZE_MAX)
      *Status_->Error += " #" + std::to_string(Element_);
    *Status_->Error += ": ";
    *Status_->Error += what;
  }
};

// Build an object and everything it contains in one allocation. Walk sizes the arena when it
// is handed 'extra', and fills it when it is handed 'arena'; it is the format's business what
// walking means. objectSize, when asked for, is what the block cache accounts.
//
// The result is freed with a bare operator delete and no destructor - which is what the block
// cache does today (SerializedDataObject::~SerializedDataObject) and what the arena requires:
// members point into the same allocation, and XPM's big numbers point their limbs at it, so
// running their destructors would hand arena memory to free().
template<typename T, typename FWalk>
static inline T *unpackObject(CIoStatus &status, size_t *objectSize, FWalk walk)
{
  const size_t headSize = aligned(sizeof(T), UnpackAlignment);

  size_t extra = 0;
  {
    T throwaway;
    walk(&extra, static_cast<uint8_t**>(nullptr), throwaway);
    if (status.Failed)
      return nullptr;
  }

  const size_t size = headSize + extra;
  uint8_t *memory = static_cast<uint8_t*>(operator new(size));
  T *object = new (memory) T;
  uint8_t *arena = memory + headSize;
  walk(static_cast<size_t*>(nullptr), &arena, *object);
  if (status.Failed) {
    operator delete(memory);
    return nullptr;
  }

  assert(static_cast<size_t>(arena - memory) == size && "unpack drifted from its measure");
  if (objectSize)
    *objectSize = size;
  return object;
}

// No destructor runs: members point into the same allocation, and a leaf that took arena space
// for its innards (XPM's big numbers) would hand it to free(). This is what the block cache
// does with an unpacked block today.
template<typename T> struct CUnpackedFree {
  void operator()(T *object) const { operator delete(object); }
};

template<typename T> using CUnpackedPtr = std::unique_ptr<T, CUnpackedFree<T>>;

}
