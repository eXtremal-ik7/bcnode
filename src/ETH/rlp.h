#pragma once

#include "common/baseBlob.h"
#include "common/serialize.h"
#include "common/uint.h"
#include "common/xvector.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace ETH {

// One decoded RLP item. Raw is the whole encoding including the prefix (what a hash is taken
// over), Payload is just the contents. Nothing is copied: both point into the caller's buffer.
struct CRlpValue {
  const uint8_t *Raw = nullptr;
  size_t RawSize = 0;
  const uint8_t *Payload = nullptr;
  size_t Size = 0;
  bool IsList = false;
};

// Decodes the item at the start of [data, data+size). Canonicality is checked - a non-minimal
// length or a single byte wrapped in a 0x81 prefix is an error, because on this data any such
// thing means our parser drifted, not that the chain is unusual.
inline bool rlpDecode(const uint8_t *data, size_t size, CRlpValue &out)
{
  if (!size)
    return false;

  const uint8_t prefix = data[0];

  if (prefix <= 0x7F) {
    out = {data, 1, data, 1, false};
    return true;
  }

  auto readLength = [&](size_t lengthSize, size_t headerSize, uint64_t &length) {
    if (size < headerSize)
      return false;
    if (data[1] == 0)                       // leading zero in the length
      return false;
    length = 0;
    for (size_t i = 0; i < lengthSize; i++)
      length = (length << 8) | data[1 + i];
    if (length <= 55)                       // would fit the short form
      return false;
    return true;
  };

  if (prefix <= 0xBF) {
    const bool shortForm = prefix <= 0xB7;
    uint64_t length;
    size_t headerSize;
    if (shortForm) {
      length = prefix - 0x80u;
      headerSize = 1;
      // A single byte below 0x80 encodes itself, never as 0x81 <byte>
      if (length == 1 && size >= 2 && data[1] <= 0x7F)
        return false;
    } else {
      const size_t lengthSize = prefix - 0xB7u;
      headerSize = 1 + lengthSize;
      if (!readLength(lengthSize, headerSize, length))
        return false;
    }
    if (length > size - headerSize)
      return false;
    out = {data, headerSize + length, data + headerSize, static_cast<size_t>(length), false};
    return true;
  }

  const bool shortForm = prefix <= 0xF7;
  uint64_t length;
  size_t headerSize;
  if (shortForm) {
    length = prefix - 0xC0u;
    headerSize = 1;
  } else {
    const size_t lengthSize = prefix - 0xF7u;
    headerSize = 1 + lengthSize;
    if (!readLength(lengthSize, headerSize, length))
      return false;
  }
  if (length > size - headerSize)
    return false;
  out = {data, headerSize + length, data + headerSize, static_cast<size_t>(length), true};
  return true;
}

// Sequential walk over the elements of a list
class CRlpList {
public:
  CRlpList() {}
  explicit CRlpList(const CRlpValue &list) : Ptr_(list.Payload), End_(list.Payload + list.Size) {}

  bool atEnd() const { return Ptr_ >= End_; }

  bool next(CRlpValue &out) {
    if (atEnd())
      return false;
    if (!rlpDecode(Ptr_, static_cast<size_t>(End_ - Ptr_), out))
      return false;
    Ptr_ += out.RawSize;
    return true;
  }

  // Number of elements, or SIZE_MAX if the list does not parse
  size_t count() const {
    const uint8_t *p = Ptr_;
    size_t n = 0;
    while (p < End_) {
      CRlpValue value;
      if (!rlpDecode(p, static_cast<size_t>(End_ - p), value))
        return SIZE_MAX;
      p += value.RawSize;
      n++;
    }
    return n;
  }

private:
  const uint8_t *Ptr_ = nullptr;
  const uint8_t *End_ = nullptr;
};

// Scalars are big-endian and minimally encoded: no leading zero byte, and zero is the empty string
inline bool rlpToU64(const CRlpValue &value, uint64_t &out)
{
  if (value.IsList || value.Size > 8)
    return false;
  if (value.Size && value.Payload[0] == 0)
    return false;
  out = 0;
  for (size_t i = 0; i < value.Size; i++)
    out = (out << 8) | value.Payload[i];
  return true;
}

inline bool rlpToBytes(const CRlpValue &value, void *out, size_t size)
{
  if (value.IsList || value.Size != size)
    return false;
  memcpy(out, value.Payload, size);
  return true;
}

// ---------------------------------------------------------------------------
// io helpers: the wire shape of a type is written once, the way bcnode does it, as
//
//   template<typename Op, typename Self> static void io(Op &op, Self &d) {
//     op.io(d.Field);
//     op.io(d.List);
//     op.end();
//   }
//
// walked over the elements of one RLP list. A vector member is an ordinary field: op.io of an
// xvector<T> reads the next element as a list of T, and op.io of an xvector<uint8_t> as a
// byte string. vec() is the same read returning the count - elements of a list, bytes of a
// string - for the rare shape where a later rule needs it (a blob transaction must carry
// blobs, extraData is capped): on the measuring pass of the unpack path the vector itself is
// not filled, so such gates must use the count, exactly BTC::Io's contract. Failure latches
// into the error string shared by every reader of the walk - the first fail() wins and
// everything after it is a no-op - so a shape carries no error plumbing of its own, the same
// contract xmstream's eof gives the BTC::Io readers.
//
// Reading comes in the two forms of BTC's reading side: rlpRead is unserialize, vectors on
// the heap; rlpUnpack is unpack2, a measuring walk sizes every vector and the object is then
// built with all its elements in one allocation. Both leave a self-contained object - byte
// strings are copied like every other member, bcnode's contract. A writer, when one is
// needed, walks the same shapes.

// Leaf codecs: how a single RLP item becomes a C++ value. Specializations for project types
// live next to the type, as bcnode keeps Io<BlockIndex> in blockIndex.h; a leaf that needs
// the reader itself (the transaction envelope) declares read(op, value, out) instead
template<typename T, typename Enable = void>
struct Io {
  static bool read(const CRlpValue &value, T &out);
};

template<> struct Io<uint64_t> {
  static bool read(const CRlpValue &value, uint64_t &out) { return rlpToU64(value, out); }
};

// Fixed-width byte strings: hashes and addresses (BaseBlob<256>/<160>), blooms
template<unsigned Bits> struct Io<BaseBlob<Bits>> {
  static bool read(const CRlpValue &value, BaseBlob<Bits> &out) {
    return rlpToBytes(value, out.begin(), out.size());
  }
};

// A minimally-encoded scalar up to the type's width: the wide integers (wei amounts, r, s)
template<unsigned Bits> struct Io<UInt<Bits>> {
  static bool read(const CRlpValue &value, UInt<Bits> &out) {
    if (value.IsList || value.Size > Bits / 8 || (value.Size && value.Payload[0] == 0))
      return false;
    out = UInt<Bits>::zero();
    // Big-endian wire into the little-endian limb array
    uint8_t *data = reinterpret_cast<uint8_t*>(out.data());
    for (size_t i = 0; i < value.Size; i++)
      data[i] = value.Payload[value.Size - 1 - i];
    return true;
  }
};

// Probes only the call shape, so the io body is not instantiated
struct CRlpIoProbe {};

template<typename T> concept RlpSelfIo = requires(CRlpIoProbe &op, T &d) { T::io(op, d); };

// The reading operation over the elements of one list. Fields are consumed in order; an
// element that does not parse always fails loudly - on this data it means the reader is
// wrong, never that the chain is unusual. One class serves all three passes of the reading
// side - BTC's Reader, Measurer and Unpacker - which differ only in where a vector's
// elements go: the heap, nowhere (only the size is kept), or the object's single allocation
class CRlpReader : public Ser::CReaderState {
public:
  CRlpReader(const CRlpValue &list, Ser::CIoStatus &status)
    : Ser::CReaderState(status), List_(list) {}
  CRlpReader(const CRlpValue &list, Ser::CIoStatus &status, size_t *extra)
    : Ser::CReaderState(status, extra), List_(list) {}
  CRlpReader(const CRlpValue &list, Ser::CIoStatus &status, uint8_t **arena)
    : Ser::CReaderState(status, arena), List_(list) {}

  bool atEnd() const { return failed() || List_.atEnd(); }

  // Every field of the entity must be consumed: the closing bracket of a shape
  void end() { check(atEnd(), "has extra fields"); }

  // Next field: a leaf through Io<T>, a vector as a list of its element type (a byte string
  // for xvector<uint8_t>), a self-described type through its own io over the sub-list
  template<typename T> void io(T &v) {
    if constexpr (Ser::IsXVector<T>::value) {
      vec(v);
    } else {
      CRlpValue field;
      if (!require(field))
        return;
      if constexpr (requires { Io<T>::read(*this, field, v); }) {
        Io<T>::read(*this, field, v);
      } else if constexpr (RlpSelfIo<T>) {
        read(field, v);
      } else if (!Io<T>::read(field, v)) {
        fail(fieldMsg("has an unexpected shape"));
      }
    }
  }

  // Next field as a string of exactly this many bytes
  void bytes(void *out, size_t size) {
    CRlpValue field;
    if (!require(field))
      return;
    if (!rlpToBytes(field, out, size))
      fail(fieldMsg("has an unexpected shape"));
  }

  // Next field as an exactly-8-byte big-endian integer: the header nonce, a fixed-width
  // string on the wire whatever its value, unlike the minimally-encoded scalars
  void fixedU64(uint64_t &v) {
    uint8_t buf[8] = {};
    bytes(buf, sizeof(buf));
    v = 0;
    for (size_t i = 0; i < sizeof(buf); i++)
      v = (v << 8) | buf[i];
  }

  // Walk a list value already in hand as an entity, with io context if the shape takes one
  template<typename T, typename... Ctx> void read(const CRlpValue &value, T &out, Ctx... ctx) {
    if (failed())
      return;
    if (!value.IsList) {
      fail("is not a list");
      return;
    }
    CRlpReader op(*this, value);
    T::io(op, out, ctx...);
  }

  // Next field as a list of T with the element count returned - BTC::Io's vec
  template<typename T> size_t vec(xvector<T> &v) {
    CRlpValue field;
    if (!enterList(field))
      return 0;
    CRlpReader op(*this, field);
    return op.items(v);
  }

  // Next field as a byte string - calldata, log data, extraData - with its size returned
  size_t vec(xvector<uint8_t> &v) {
    CRlpValue field;
    if (!require(field))
      return 0;
    if (field.IsList) {
      fail(fieldMsg("has an unexpected shape"));
      return 0;
    }
    byteString(field, v);
    return field.Size;
  }

  // The elements of this reader itself: for entities that are a bare list of T (receipts)
  template<typename T> size_t items(xvector<T> &v) {
    if (failed())
      return 0;
    const size_t n = List_.count();
    if (n == SIZE_MAX) {
      fail("has an element that does not parse");
      return 0;
    }
    T *elements = prepare(v, n);
    CRlpValue value;
    for (size_t i = 0; i < n && next(value); i++) {
      setElement(i);
      if (elements) {
        readValue(value, elements[i]);
      } else {
        T throwaway;
        readValue(value, throwaway);
      }
    }
    return failed() ? 0 : n;
  }

  // Consume whatever remains; the total field count is what tells forks apart
  size_t drain() {
    CRlpValue value;
    while (next(value))
      ;
    return Index_;
  }

private:
  // A sub-walk shares the failure latch and the pass it runs under
  CRlpReader(const CRlpReader &parent, const CRlpValue &list)
    : Ser::CReaderState(parent), List_(list) {}

  // Byte-string contents go where a vector's elements do, so what either reading form
  // returns is self-contained, as bcnode's deserialization leaves it
  void byteString(const CRlpValue &value, xvector<uint8_t> &v) {
    if (uint8_t *data = prepare(v, value.Size))
      memcpy(data, value.Payload, value.Size);
  }

  // Next element if there is one: false at the end; an element that does not parse fails
  bool next(CRlpValue &out) {
    if (atEnd())
      return false;
    if (!List_.next(out)) {
      fail("element " + std::to_string(Index_) + " does not parse");
      return false;
    }
    Index_++;
    return true;
  }

  // Next element that must exist
  bool require(CRlpValue &out) {
    if (failed())
      return false;
    if (!next(out)) {
      if (!failed())
        fail("has too few fields");
      return false;
    }
    return true;
  }

  template<typename T> void readValue(const CRlpValue &value, T &out) {
    if constexpr (requires { Io<T>::read(*this, value, out); }) {
      Io<T>::read(*this, value, out);
    } else if constexpr (RlpSelfIo<T>) {
      read(value, out);
    } else if (!Io<T>::read(value, out)) {
      fail("has an element of unexpected shape");
    }
  }

  bool enterList(CRlpValue &field) {
    if (!require(field))
      return false;
    if (!field.IsList) {
      fail(fieldMsg("is not a list"));
      return false;
    }
    return true;
  }

  std::string fieldMsg(const char *what) const {
    return "field " + std::to_string(Index_ - 1) + " " + what;
  }

  CRlpList List_;
  size_t Index_ = 0;
};

// The root of an entity must be a list covering the buffer exactly
inline bool rlpRoot(const uint8_t *data, size_t size, const char *name, CRlpValue &root,
                    std::string &error)
{
  error.clear();
  if (!rlpDecode(data, size, root) || !root.IsList || root.RawSize != size) {
    error = std::string(name) + ": not a well-formed RLP list";
    return false;
  }
  return true;
}

// The whole decode of one entity, BTC::unserializeAndCheck. 'error' is filled only on failure;
// on this data any failure means the reader is wrong
template<typename T>
inline bool rlpRead(const uint8_t *data, size_t size, T &out, std::string &error)
{
  error.clear();
  CRlpValue root;
  if (!rlpRoot(data, size, T::Name, root, error))
    return false;

  Ser::CIoStatus status;
  status.Name = T::Name;
  status.Error = &error;
  CRlpReader op(root, status);
  T::io(op, out);
  return !op.failed();
}

// BTC::unpack2: a measuring walk of the shape sizes every vector, then the object and all
// its elements are built in one allocation. The measuring walk decodes into a throwaway, so
// everything is already validated when building starts.
template<typename T>
inline T *rlpUnpack(const uint8_t *data, size_t size, std::string &error, size_t *objectSize = nullptr)
{
  error.clear();
  CRlpValue root;
  if (!rlpRoot(data, size, T::Name, root, error))
    return nullptr;

  Ser::CIoStatus status;
  status.Name = T::Name;
  status.Error = &error;
  return Ser::unpackObject<T>(status, objectSize, [&](size_t *extra, uint8_t **arena, T &target) {
    if (extra) {
      CRlpReader op(root, status, extra);
      T::io(op, target);
    } else {
      CRlpReader op(root, status, arena);
      T::io(op, target);
    }
  });
}

// Freeing an unpacked object is the skeleton's business: nothing here owns anything of its
// own, every vector points back into the same allocation
using Ser::CUnpackedPtr;

// ---------------------------------------------------------------------------
// RLP the other way round. The decoder above never needed this: it reads what geth wrote. An
// encoder is needed for three things that are all derived rather than stored - the payload a
// transaction signature covers, a transaction hash, and the transactions/receipts tries.
//
// A list is written payload first and its header inserted afterwards. That costs one move of the
// payload; the alternative is measuring every item twice, which costs more and is easy to get
// subtly wrong.
class CRlpWriter {
public:
  const std::vector<uint8_t> &data() const { return Buffer_; }
  std::vector<uint8_t> &data() { return Buffer_; }
  void clear() { Buffer_.clear(); }

  void bytes(const uint8_t *data, size_t size) {
    if (size == 1 && data[0] <= 0x7F) {
      Buffer_.push_back(data[0]);
      return;
    }
    header(0x80, size);
    Buffer_.insert(Buffer_.end(), data, data + size);
  }

  // Every integer on the wire is a minimal big-endian string, and zero is the empty one
  void u64(uint64_t value) {
    uint8_t be[8];
    for (int i = 0; i < 8; i++)
      be[i] = static_cast<uint8_t>(value >> (56 - 8 * i));
    size_t first = 0;
    while (first < 8 && be[first] == 0)
      first++;
    bytes(be + first, 8 - first);
  }

  template<unsigned Bits>
  void uint(const UInt<Bits> &value) {
    uint8_t be[Bits / 8];
    value.exportBE(be);
    size_t first = 0;
    while (first < sizeof(be) && be[first] == 0)
      first++;
    bytes(be + first, sizeof(be) - first);
  }

  template<typename Blob>
  void blob(const Blob &value) { bytes(value.begin(), value.size()); }

  void empty() { Buffer_.push_back(0x80); }

  size_t beginList() { return Buffer_.size(); }

  void endList(size_t marker) {
    const size_t size = Buffer_.size() - marker;
    uint8_t head[9];
    size_t headSize = 0;
    if (size <= 55) {
      head[headSize++] = static_cast<uint8_t>(0xC0 + size);
    } else {
      const size_t lengthSize = lengthBytes(size);
      head[headSize++] = static_cast<uint8_t>(0xF7 + lengthSize);
      for (size_t i = 0; i < lengthSize; i++)
        head[headSize++] = static_cast<uint8_t>(size >> (8 * (lengthSize - 1 - i)));
    }
    Buffer_.insert(Buffer_.begin() + marker, head, head + headSize);
  }

private:
  static size_t lengthBytes(size_t size) {
    size_t count = 0;
    while (size) {
      count++;
      size >>= 8;
    }
    return count;
  }

  void header(uint8_t base, size_t size) {
    if (size <= 55) {
      Buffer_.push_back(static_cast<uint8_t>(base + size));
      return;
    }
    const size_t lengthSize = lengthBytes(size);
    Buffer_.push_back(static_cast<uint8_t>(base + 55 + lengthSize));
    for (size_t i = 0; i < lengthSize; i++)
      Buffer_.push_back(static_cast<uint8_t>(size >> (8 * (lengthSize - 1 - i))));
  }

  std::vector<uint8_t> Buffer_;
};

}
