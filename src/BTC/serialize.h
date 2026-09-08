#pragma once

// Bitcoin's wire format: compact sizes, base 128 varints, little endian scalars. The machinery
// that walks a shape - the passes, the arena, the failure latch - is format-neutral and lives
// in common/serialize.h; this file is only the bytes, and the same bytes serve bcnode's own
// on-disk structures (the block index, the linked-outputs file, utxodb values) because that is
// what they were written in, not because a format is inherited from anywhere.

#include "common/baseBlob.h"
#include "common/serialize.h"
#include "common/uint.h"
#include "common/xvector.h"
#include "p2putils/xmstream.h"
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>

namespace BTC {

// The leaves of the format. A leaf that needs arena space of its own declares
// read(op, src, data) instead of unserialize and takes it from op.arena().
template<typename T, typename Enable=void>
struct Io {
  static inline size_t getSerializedSize(const T &data);
  static inline void serialize(xmstream &src, const T &data);
  static inline void unserialize(xmstream &dst, T &data);
};

template<typename T> static inline size_t getSerializedSize(const T &data) { return Io<T>::getSerializedSize(data); }
template<typename T> static inline void serialize(xmstream &src, const T &data) { Io<T>::serialize(src, data); }
template<typename T> static inline void unserialize(xmstream &src, T &data);

// variable size
// Upper bound Core puts on a compact size
static constexpr uint64_t MaxSerializedSize = 0x02000000;

static inline size_t getSerializedVarSizeSize(uint64_t value)
{
  if (value < 0xFD) {
    return 1;
  } else if (value <= 0xFFFF) {
    return 3;
  } else if (value <= 0xFFFFFFFF) {
    return 5;
  } else {
    return 9;
  }
}

static inline void serializeVarSize(xmstream &stream, uint64_t value)
{
  if (value < 0xFD) {
    stream.write<uint8_t>(static_cast<uint8_t>(value));
  } else if (value <= 0xFFFF) {
    stream.write<uint8_t>(0xFD);
    stream.writele<uint16_t>(static_cast<uint16_t>(value));
  } else if (value <= 0xFFFFFFFF) {
    stream.write<uint8_t>(0xFE);
    stream.writele<uint32_t>(static_cast<uint32_t>(value));
  } else {
    stream.write<uint8_t>(0xFF);
    stream.writele<uint64_t>(value);
  }
}

// Rejects what Core's ReadCompactSize rejects: a value that would have fit in a shorter
// encoding, and a value above MaxSerializedSize. Failure sets eof, which unserializeAndCheck
// and unpack2 already treat as a parse error; out is zeroed so nothing loops on a bad count.
static inline void unserializeVarSize(xmstream &stream, uint64_t &out)
{
  uint8_t type = stream.read<uint8_t>();
  if (type < 0xFD) {
    out = type;
    return;
  }

  uint64_t shortestForm;
  if (type == 0xFD) {
    out = stream.readle<uint16_t>();
    shortestForm = 0xFD;
  } else if (type == 0xFE) {
    out = stream.readle<uint32_t>();
    shortestForm = 0x10000;
  } else {
    out = stream.readle<uint64_t>();
    shortestForm = 0x100000000;
  }

  if (out < shortestForm || out > MaxSerializedSize) {
    out = 0;
    stream.seekEnd(0, true);
  }
}

// Core's other variable size integer, unrelated to the compact size above: base 128, most
// significant group first, every group after the first biased by one so that each value has
// exactly one encoding. MWEB uses it for amounts, heights and MMR sizes.
template<typename T> static inline size_t getSerializedVarIntSize(T value)
{
  size_t size = 0;
  while (true) {
    size++;
    if (value <= 0x7F)
      break;
    value = (value >> 7) - 1;
  }

  return size;
}

template<typename T> static inline void serializeVarInt(xmstream &stream, T value)
{
  uint8_t data[(sizeof(T)*8 + 6) / 7];
  int len = 0;
  while (true) {
    data[len] = static_cast<uint8_t>((value & 0x7F) | (len ? 0x80 : 0x00));
    if (value <= 0x7F)
      break;
    value = (value >> 7) - 1;
    len++;
  }

  do {
    stream.write<uint8_t>(data[len]);
  } while (len--);
}

// Non minimal encodings are unrepresentable by construction, so the only failures left are
// running out of bytes and overflowing the target type; both set eof, as unserializeVarSize
template<typename T> static inline void unserializeVarInt(xmstream &stream, T &out)
{
  T value = 0;
  while (true) {
    uint8_t data = stream.read<uint8_t>();
    if (stream.eof()) {
      out = 0;
      return;
    }

    if (value > (std::numeric_limits<T>::max() >> 7)) {
      out = 0;
      stream.seekEnd(0, true);
      return;
    }

    value = static_cast<T>((value << 7) | (data & 0x7F));
    if (data & 0x80) {
      if (value == std::numeric_limits<T>::max()) {
        out = 0;
        stream.seekEnd(0, true);
        return;
      }
      value++;
    } else {
      out = value;
      return;
    }
  }
}

// A varint as an ordinary field, for types whose wire form carries one by value:
// the headers message entry stores a transaction count, always 0 there
struct VarSize {
  uint64_t Value = 0;
};

template<> struct Io<VarSize> {
  static inline size_t getSerializedSize(const VarSize &data) { return getSerializedVarSizeSize(data.Value); }
  static inline void serialize(xmstream &dst, const VarSize &data) { serializeVarSize(dst, data.Value); }
  static inline void unserialize(xmstream &src, VarSize &data) { unserializeVarSize(src, data.Value); }
};

}

namespace BTC {
// TODO: use C++20 and concepts
template<class T>
struct is_simple_numeric : std::integral_constant<bool,
        std::is_same<T, int8_t>::value ||
        std::is_same<T, uint8_t>::value ||
        std::is_same<T, int16_t>::value ||
        std::is_same<T, uint16_t>::value ||
        std::is_same<T, int32_t>::value ||
        std::is_same<T, uint32_t>::value ||
        std::is_same<T, int64_t>::value ||
        std::is_same<T, uint64_t>::value> {};

// Serialization for simple integer types
template<typename T>
struct Io<T, typename std::enable_if<is_simple_numeric<T>::value, void>::type> {
  static inline size_t getSerializedSize(const T&) { return sizeof(T); }
  static inline void serialize(xmstream &stream, const T &data) { stream.writele<T>(data); }
  static inline void unserialize(xmstream &stream, T &data) { data = stream.readle<T>(); }
};

// Serialization for bool
template<> struct Io<bool> {
  static inline size_t getSerializedSize(const bool&) { return 1; }
  static inline void serialize(xmstream &stream, const bool &data) { stream.writele(static_cast<uint8_t>(data)); }
  static inline void unserialize(xmstream &stream, bool &data) { data = stream.readle<uint8_t>(); }
};

// Serialization for base_blob (including uint256) types
template<unsigned Bits> struct Io<BaseBlob<Bits>> {
  static inline size_t getSerializedSize(const BaseBlob<Bits>&) { return Bits/8; }
  static inline void serialize(xmstream &stream, const BaseBlob<Bits> &data) { stream.write(data.begin(), data.size()); }
  static inline void unserialize(xmstream &stream, BaseBlob<Bits> &data) { stream.read(data.begin(), data.size()); }
};

template<unsigned Bits> struct Io<UInt<Bits>> {
  static inline size_t getSerializedSize(const UInt<Bits>&) { return Bits/8; }
  static inline void serialize(xmstream &stream, const UInt<Bits> &data) { stream.write(data.data(), Bits / 8); }
  static inline void unserialize(xmstream &stream, UInt<Bits> &data) { stream.read(data.data(), Bits / 8); }
};

// string
// Serialization for std::string
// NOTE: a string keeps its own heap, so a type holding one cannot live in an unpacked object
template<> struct Io<std::string> {
  static inline size_t getSerializedSize(const std::string &data) {
    return getSerializedVarSizeSize(data.size()) + data.size();
  }

  static inline void serialize(xmstream &dst, const std::string &data) {
    serializeVarSize(dst, data.size());
    dst.write(data.data(), data.size());
  }
  static inline void unserialize(xmstream &src, std::string &data) {
    uint64_t length;
    unserializeVarSize(src, length);
    data.assign(src.seek<const char>(length), length);
  }
};

// array
template<size_t Size> struct Io<std::array<uint8_t, Size>> {
  static inline size_t getSerializedSize(const std::array<uint8_t, Size>&) {
    return Size;
  }

  static inline void serialize(xmstream &dst, const std::array<uint8_t, Size> &data) {
    dst.write(data.data(), Size);
  }

  static inline void unserialize(xmstream &src, std::array<uint8_t, Size> &data) {
    src.read(data.data(), Size);
  }
};

// xvector: only the writing side is a leaf. Reading a vector is where the three passes differ,
// so it belongs to the reader, which is the one place that knows which pass is running.
template<typename T> struct Io<xvector<T>> {
  template<typename... Ctx>
  static inline size_t getSerializedSize(const xvector<T> &data, Ctx... ctx) {
    size_t size = getSerializedVarSizeSize(data.size());
    for (const auto &v: data) {
      if constexpr (requires { Io<T>::getSerializedSize(v, ctx...); })
        size += Io<T>::getSerializedSize(v, ctx...);
      else
        size += Io<T>::getSerializedSize(v);
    }
    return size;
  }

  template<typename... Ctx>
  static inline void serialize(xmstream &dst, const xvector<T> &data, Ctx... ctx) {
    serializeVarSize(dst, data.size());
    for (const auto &v: data) {
      if constexpr (requires { Io<T>::serialize(dst, v, ctx...); })
        Io<T>::serialize(dst, v, ctx...);
      else
        Io<T>::serialize(dst, v);
    }
  }
};

// Special case: xvector<uint8_t> is a byte string, not a list
template<> struct Io<xvector<uint8_t>> {
  static inline size_t getSerializedSize(const xvector<uint8_t> &data) {
    return getSerializedVarSizeSize(data.size()) + data.size();
  }

  static inline void serialize(xmstream &dst, const xvector<uint8_t> &data) {
    serializeVarSize(dst, data.size());
    dst.write(data.data(), data.size());
  }
};

// Self-described types: the wire format is written once as a single template procedure
//
//   template<typename Op, typename Self>
//   static void io(Op &op, Self &d, ...context...) { op.io(d.field); ... }
//
// and all five operations are that procedure walked by one of the operation classes below.
// Members left out are not on the wire; a group present under a condition is an ordinary if;
// genuinely asymmetric formats (the segwit marker) branch on Op::Writing. A type serving as
// the base of a format-changing heir (BTC::Proto::CMessageVersion, whose XPM heir drops the
// relay field) opens its io with
//
//   static_assert(std::is_same_v<std::remove_cv_t<Self>, X>);
//
// so an heir that forgot its own io fails loudly instead of picking up the base format;
// heirs that keep the format inherit io as is.
//
// The op interface: io(member [, context]) — a field, another self-described type or a leaf,
// with a vector member routed to vec(); vec(member [, context]) — a vector field, returns the
// element count on every pass, so counts read from the stream can drive later gates even while
// measuring, when the vector itself is not filled; raw(member) — unions and paddingless
// aggregates written as bytes; varint(member) — an integer in Core's base 128 form rather than
// fixed width little endian; check(ok [, what]) — a format rule, makes the readers fail;
// put(v)/get(v) — direction-specific scalars for wire words that are not members (packed
// version bits, prefix bytes, the segwit marker); element(vec, i, fn) — per-element access on
// the reading side (the measuring pass has no elements and hands fn a throwaway).

// Probes only the call shape, so the io body is not instantiated: a concept usable before
// the operation classes exist
struct IoProbe {
  static constexpr bool Writing = true;
};

template<typename T> concept SelfIo =
  requires(IoProbe &op, const T &d) { T::io(op, d); } ||
  requires(IoProbe &op, const T &d) { T::io(op, d, true); };

struct SizeOf {
  size_t Size = 0;
  static constexpr bool Writing = true;

  template<typename U, typename... Ctx> inline void io(const U &v, Ctx... ctx) {
    if constexpr (Ser::IsXVector<U>::value) {
      vec(v, ctx...);
    } else if constexpr (requires { U::io(*this, v, ctx...); }) {
      U::io(*this, v, ctx...);
    } else if constexpr (requires { U::io(*this, v); }) {
      // a self-described member that does not take the context: drop it, as a leaf would
      U::io(*this, v);
    } else {
      static_assert(!SelfIo<U>, "self-described type reached the leaf path: missing io context?");
      Size += Io<U>::getSerializedSize(v);
    }
  }

  template<typename U, typename... Ctx> inline size_t vec(const xvector<U> &v, Ctx... ctx) {
    Size += Io<xvector<U>>::getSerializedSize(v, ctx...);
    return v.size();
  }

  template<typename U> inline void raw(const U&) { Size += sizeof(U); }
  template<typename U> inline void varint(const U &v) { Size += getSerializedVarIntSize(v); }
  template<typename U> inline void put(U) { Size += sizeof(U); }
  inline void check(bool) {}
  inline void check(bool, const char*) {}
};

struct Writer {
  xmstream &Dst;
  static constexpr bool Writing = true;

  template<typename U, typename... Ctx> inline void io(const U &v, Ctx... ctx) {
    if constexpr (Ser::IsXVector<U>::value) {
      vec(v, ctx...);
    } else if constexpr (requires { U::io(*this, v, ctx...); }) {
      U::io(*this, v, ctx...);
    } else if constexpr (requires { U::io(*this, v); }) {
      U::io(*this, v);
    } else {
      static_assert(!SelfIo<U>, "self-described type reached the leaf path: missing io context?");
      Io<U>::serialize(Dst, v);
    }
  }

  template<typename U, typename... Ctx> inline size_t vec(const xvector<U> &v, Ctx... ctx) {
    Io<xvector<U>>::serialize(Dst, v, ctx...);
    return v.size();
  }

  template<typename U> inline void raw(const U &v) { Dst.write(&v, sizeof(U)); }
  template<typename U> inline void varint(const U &v) { serializeVarInt(Dst, v); }
  template<typename U> inline void put(U v) { Dst.writele<U>(v); }
  inline void check(bool) {}
  inline void check(bool, const char*) {}
};

// The reading side, one class for all three passes. What they disagree about is where a
// vector's elements go, and that lives in Ser::CReaderState::prepare - so the guards a vector
// read needs are written once and cannot drift between passes.
//
// A failure both latches into the status and sets eof on the stream: the latch carries the
// message, eof is what unserializeAndCheck and unpack2 have always answered on.
struct Reader : public Ser::CReaderState {
  xmstream &Src;
  static constexpr bool Writing = false;

  Reader(xmstream &src, Ser::CIoStatus &status) : Ser::CReaderState(status), Src(src) {}
  Reader(xmstream &src, Ser::CIoStatus &status, size_t *extra) : Ser::CReaderState(status, extra), Src(src) {}
  Reader(xmstream &src, Ser::CIoStatus &status, uint8_t **arena) : Ser::CReaderState(status, arena), Src(src) {}

  inline void fail(const char *what) {
    Ser::CReaderState::fail(what);
    Src.seekEnd(0, true);
  }

  inline void check(bool ok) { if (!ok) fail("format check failed"); }
  inline void check(bool ok, const char *what) { if (!ok) fail(what); }

  template<typename U, typename... Ctx> inline void io(U &v, Ctx... ctx) {
    if constexpr (Ser::IsXVector<U>::value)
      vec(v, ctx...);
    else
      leaf(v, ctx...);
  }

  template<typename U, typename... Ctx> inline size_t vec(xvector<U> &v, Ctx... ctx) {
    uint64_t count = 0;
    unserializeVarSize(Src, count);
    // Every element takes at least one byte: a count past the end is a broken record, and
    // without this the measuring pass would walk millions of elements that are not there
    if (count > Src.remaining()) {
      fail("vector is longer than the data");
      return 0;
    }

    U *elements = prepare(v, count);
    for (uint64_t i = 0; i < count; i++) {
      if (Src.eof() || failed())
        break;
      // io, not leaf: an element can itself be a vector - the witness stacks are
      if (elements) {
        io(elements[i], ctx...);
      } else {
        U throwaway;
        io(throwaway, ctx...);
      }
    }

    return count;
  }

  // A byte string, not a list of elements
  inline size_t vec(xvector<uint8_t> &v) {
    uint64_t count = 0;
    unserializeVarSize(Src, count);
    if (count > Src.remaining()) {
      fail("byte string is longer than the data");
      return 0;
    }

    uint8_t *data = prepare(v, count);
    const void *source = Src.seek(count);
    if (data && source)
      memcpy(data, source, count);
    return count;
  }

  template<typename U> inline void raw(U &v) { Src.read(&v, sizeof(U)); }
  template<typename U> inline void varint(U &v) { unserializeVarInt(Src, v); }
  template<typename U> inline void get(U &v) { v = Src.readle<U>(); }

  // Elements exist on every pass but the measuring one, which has nothing to point at
  template<typename U, typename F> inline void element(xvector<U> &v, size_t i, F body) {
    if (pass() != Ser::EPass::Measure) {
      body(v[i]);
    } else {
      U throwaway;
      body(throwaway);
    }
  }

private:
  template<typename U, typename... Ctx> inline void leaf(U &v, Ctx... ctx) {
    if constexpr (requires { U::io(*this, v, ctx...); }) {
      U::io(*this, v, ctx...);
    } else if constexpr (requires { U::io(*this, v); }) {
      U::io(*this, v);
    } else if constexpr (requires { Io<U>::read(*this, Src, v); }) {
      // a leaf with variable-size innards of its own, taking them from the arena
      static_assert(!SelfIo<U>, "self-described type reached the leaf path: missing io context?");
      Io<U>::read(*this, Src, v);
    } else {
      static_assert(!SelfIo<U>, "self-described type reached the leaf path: missing io context?");
      Io<U>::unserialize(Src, v);
    }
  }
};

// The operations of a self-described type are thin drivers over its io
template<typename T> requires SelfIo<T>
struct Io<T, void> {
  template<typename... Ctx>
  static inline size_t getSerializedSize(const T &data, Ctx... ctx) {
    SizeOf op;
    op.io(data, ctx...);
    return op.Size;
  }

  template<typename... Ctx>
  static inline void serialize(xmstream &dst, const T &data, Ctx... ctx) {
    Writer op{dst};
    op.io(data, ctx...);
  }

  template<typename... Ctx>
  static inline void unserialize(xmstream &src, T &data, Ctx... ctx) {
    Ser::CIoStatus status;
    Reader op(src, status);
    op.io(data, ctx...);
  }
};

// Public reads use the reader too: a root can be a vector, not only a leaf or a shape.
template<typename T>
static inline void unserialize(xmstream &src, T &data) {
  Ser::CIoStatus status;
  Reader op(src, status);
  op.io(data);
}

// unserialize & check
template<typename T>
static inline bool unserializeAndCheck(xmstream &stream, T &data) {
  BTC::unserialize(stream, data);
  return !stream.eof();
}

// The object and everything it contains in one allocation: the shape is walked once to size
// the arena, then again to fill it. The measuring walk runs on a copy of the cursor, so the
// caller's stream advances exactly once. 'error', when given, is filled only on failure.
template<typename T> static inline T *unpack2(xmstream &src, size_t *size, std::string *error = nullptr)
{
  Ser::CIoStatus status;
  status.Name = "block";
  status.Error = error;

  uint8_t *begin = src.ptr<uint8_t>();
  const size_t remaining = src.remaining();

  return Ser::unpackObject<T>(status, size, [&](size_t *extra, uint8_t **arena, T &target) {
    if (extra) {
      xmstream probe(begin, remaining);
      Reader op(probe, status, extra);
      op.io(target);
      if (probe.eof())
        op.fail("unexpected end of data");
    } else {
      Reader op(src, status, arena);
      op.io(target);
      if (src.eof())
        op.fail("unexpected end of data");
    }
  });
}

}
