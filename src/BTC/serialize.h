#pragma once

#include "common/baseBlob.h"
#include "common/uint.h"
#include "common/xvector.h"
#include "p2putils/xmstream.h"
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <type_traits>

static inline size_t aligned(size_t size, size_t align) { return (size + align - 1) & ~(align-1); }
static constexpr size_t UnpackAlignment = 8;

namespace BTC {

template<typename T, typename Enable=void>
struct Io {
  static inline size_t getSerializedSize(const T &data);
  static inline size_t getUnpackedExtraSize(xmstream &src);
  static inline void serialize(xmstream &src, const T &data);
  static inline void unserialize(xmstream &dst, T &data);
  static inline void unpack2(xmstream &src, T *data, uint8_t **extraData);
};

template<typename T> static inline size_t getSerializedSize(const T &data) { return Io<T>::getSerializedSize(data); }
template<typename T> static inline void serialize(xmstream &src, const T &data) { Io<T>::serialize(src, data); }
template<typename T> static inline void unserialize(xmstream &dst, T &data) { Io<T>::unserialize(dst, data); }
template<typename T> static inline T *unpack2(xmstream &src, size_t *size) {
  size_t dataOnlySize = aligned(sizeof(T), UnpackAlignment);
  {
    xmstream stream(src.ptr<uint8_t>(), src.remaining());
    *size = dataOnlySize + Io<T>::getUnpackedExtraSize(stream);
    if (stream.eof())
      return nullptr;
  }

  uint8_t *data = static_cast<uint8_t*>(operator new(*size));
  uint8_t *extraData = data + dataOnlySize;
  Io<T>::unpack2(src, reinterpret_cast<T*>(data), &extraData);
  assert(static_cast<size_t>(extraData-data) == *size && "Unpack failed");
  if (!src.eof()) {
    return reinterpret_cast<T*>(data);
  } else {
    operator delete(data);
    return nullptr;
  }
}

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
  static inline size_t getUnpackedExtraSize(xmstream &src) {
    uint64_t value;
    unserializeVarSize(src, value);
    return 0;
  }
  static inline void serialize(xmstream &dst, const VarSize &data) { serializeVarSize(dst, data.Value); }
  static inline void unserialize(xmstream &src, VarSize &data) { unserializeVarSize(src, data.Value); }
  static inline void unpack2(xmstream &src, VarSize *data, uint8_t**) { unserialize(src, *data); }
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
  static inline size_t getUnpackedExtraSize(xmstream &src) { src.seek(sizeof(T)); return 0; }
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
  static inline size_t getUnpackedExtraSize(xmstream &src) {
    src.seek(Bits / 8);
    return 0;
  }
  static inline void serialize(xmstream &stream, const BaseBlob<Bits> &data) { stream.write(data.begin(), data.size()); }
  static inline void unserialize(xmstream &stream, BaseBlob<Bits> &data) { stream.read(data.begin(), data.size()); }
  static inline void unpack2(xmstream &src, BaseBlob<Bits> *data, uint8_t**) { unserialize(src, *data); }
};

template<unsigned Bits> struct Io<UInt<Bits>> {
  static inline size_t getSerializedSize(const UInt<Bits>&) { return Bits/8; }
  static inline size_t getUnpackedExtraSize(xmstream &src) {
    src.seek(Bits / 8);
    return 0;
  }
  static inline void serialize(xmstream &stream, const UInt<Bits> &data) { stream.write(data.data(), Bits / 8); }
  static inline void unserialize(xmstream &stream, UInt<Bits> &data) { stream.read(data.data(), Bits / 8); }
  static inline void unpack2(xmstream &src, UInt<Bits> *data, uint8_t**) { unserialize(src, *data); }
};

// string
// Serialization for std::string
// NOTE: unpacking not supported
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

  static inline size_t getUnpackedExtraSize(xmstream &src) {
    src.seek(Size);
    return 0;
  }

  static inline void serialize(xmstream &dst, const std::array<uint8_t, Size> &data) {
    dst.write(data.data(), Size);
  }

  static inline void unserialize(xmstream &src, std::array<uint8_t, Size> &data) {
    src.read(data.data(), Size);
  }

  static inline void unpack2(xmstream &src, std::array<uint8_t, Size> *data, uint8_t**) {
    src.read(data->data(), Size);
  }
};

// xvector
template<typename T> struct Io<xvector<T>> {
  static inline size_t getSerializedSize(const xvector<T> &data) {
    size_t size = getSerializedVarSizeSize(data.size());
    for (const auto &v: data)
      size += Io<T>::getSerializedSize(v);
    return size;
  }

  static inline size_t getUnpackedExtraSize(xmstream &src, uint64_t *count) {
    unserializeVarSize(src, *count);

    size_t result = 0;
    for (size_t i = 0; i < *count; i++)
      result += Io<T>::getUnpackedExtraSize(src);
    return *count*sizeof(T) + result;
  }

  static inline size_t getUnpackedExtraSize(xmstream &src) {
    uint64_t size;
    return getUnpackedExtraSize(src, &size);
  }

  static inline void serialize(xmstream &dst, const xvector<T> &data) {
    serializeVarSize(dst, data.size());
    for (const auto &v: data)
      BTC::serialize(dst, v);
  }

  static inline void unserialize(xmstream &src, xvector<T> &data) {
    uint64_t size = 0;
    unserializeVarSize(src, size);
    if (size > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.resize(size);
    for (uint64_t i = 0; i < size; i++)
      BTC::unserialize(src, data[i]);
  }

  static inline void unpack2(xmstream &src, xvector<T> *data, uint8_t **extraData) {
    uint64_t size;
    unserializeVarSize(src, size);

    T *elementsData = reinterpret_cast<T*>(*extraData);
    new (data) xvector<T>(elementsData, size);
    (*extraData) += sizeof(T)*size;
    for (size_t i = 0; i < size; i++)
      BTC::Io<T>::unpack2(src, &elementsData[i], extraData);
  }
};

// Context-dependend xvector
// The element type takes the context either in its own Io specialization or as trailing
// arguments of the plain one (the self-described drivers below accept any), hence the fallback
template<typename T, typename ContextTy> struct Io<xvector<T>, ContextTy> {
  static inline size_t getSerializedSize(const xvector<T> &data, ContextTy context) {
    size_t size = getSerializedVarSizeSize(data.size());
    for (const auto &v: data) {
      if constexpr (requires { Io<T, ContextTy>::getSerializedSize(v, context); })
        size += Io<T, ContextTy>::getSerializedSize(v, context);
      else if constexpr (requires { Io<T>::getSerializedSize(v, context); })
        size += Io<T>::getSerializedSize(v, context);
      else
        size += Io<T>::getSerializedSize(v);
    }
    return size;
  }

  static inline size_t getUnpackedExtraSize(xmstream &src, uint64_t *count, ContextTy context) {
    unserializeVarSize(src, *count);

    size_t result = 0;
    for (size_t i = 0; i < *count; i++) {
      if constexpr (requires { Io<T, ContextTy>::getUnpackedExtraSize(src, context); })
        result += Io<T, ContextTy>::getUnpackedExtraSize(src, context);
      else if constexpr (requires { Io<T>::getUnpackedExtraSize(src, context); })
        result += Io<T>::getUnpackedExtraSize(src, context);
      else
        result += Io<T>::getUnpackedExtraSize(src);
    }
    return *count*sizeof(T) + result;
  }

  static inline size_t getUnpackedExtraSize(xmstream &src, ContextTy context) {
    uint64_t size;
    return getUnpackedExtraSize(src, &size, context);
  }

  static inline void serialize(xmstream &dst, const xvector<T> &data, ContextTy context) {
    serializeVarSize(dst, data.size());
    for (const auto &v: data) {
      if constexpr (requires { Io<T, ContextTy>::serialize(dst, v, context); })
        Io<T, ContextTy>::serialize(dst, v, context);
      else
        Io<T>::serialize(dst, v, context);
    }
  }

  static inline void unserialize(xmstream &src, xvector<T> &data, ContextTy context) {
    uint64_t size = 0;
    unserializeVarSize(src, size);
    if (size > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.resize(size);
    for (uint64_t i = 0; i < size; i++) {
      if constexpr (requires { Io<T, ContextTy>::unserialize(src, data[i], context); })
        Io<T, ContextTy>::unserialize(src, data[i], context);
      else if constexpr (requires { Io<T>::unserialize(src, data[i], context); })
        Io<T>::unserialize(src, data[i], context);
      else
        Io<T>::unserialize(src, data[i]);
    }
  }

  static inline void unpack2(xmstream &src, xvector<T> *data, uint8_t **extraData, ContextTy context) {
    uint64_t size;
    unserializeVarSize(src, size);

    T *elementsData = reinterpret_cast<T*>(*extraData);
    new (data) xvector<T>(elementsData, size);
    (*extraData) += sizeof(T)*size;
    for (size_t i = 0; i < size; i++) {
      if constexpr (requires { Io<T, ContextTy>::unpack2(src, &elementsData[i], extraData, context); })
        Io<T, ContextTy>::unpack2(src, &elementsData[i], extraData, context);
      else if constexpr (requires { Io<T>::unpack2(src, &elementsData[i], extraData, context); })
        Io<T>::unpack2(src, &elementsData[i], extraData, context);
      else
        Io<T>::unpack2(src, &elementsData[i], extraData);
    }
  }
};

// Special case: xvector<uint8_t>
template<> struct Io<xvector<uint8_t>> {
  static inline size_t getSerializedSize(const xvector<uint8_t> &data) {
    return getSerializedVarSizeSize(data.size()) + data.size();
  }

  static inline size_t getUnpackedExtraSize(xmstream &src, uint64_t *count) {
    unserializeVarSize(src, *count);
    src.seek(*count);
    return aligned(*count, UnpackAlignment);
  }

  static inline size_t getUnpackedExtraSize(xmstream &src) {
    uint64_t size;
    return getUnpackedExtraSize(src, &size);
  }

  static inline void serialize(xmstream &dst, const xvector<uint8_t> &data) {
    serializeVarSize(dst, data.size());
    dst.write(data.data(), data.size());
  }

  static inline void unserialize(xmstream &src, xvector<uint8_t> &data) {
    uint64_t size = 0;
    unserializeVarSize(src, size);
    if (size > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.resize(size);
    src.read(data.data(), size);
  }

  static inline void unpack2(xmstream &src, xvector<uint8_t> *data, uint8_t **extraData) {
    uint64_t size;
    unserializeVarSize(src, size);

    new (data) xvector<uint8_t>(*extraData, size);
    void *srcData = src.seek(size);
    if (srcData)
      memcpy(*extraData, srcData, size);
    (*extraData) += aligned(size, UnpackAlignment);
  }
};

// unserialize & check
template<typename T>
static inline bool unserializeAndCheck(xmstream &stream, T &data) {
  BTC::Io<T>::unserialize(stream, data);
  return !stream.eof();
}

// Self-described types: the wire format is written once as a single template procedure
//
//   template<typename Op, typename Self>
//   static void io(Op &op, Self &d, ...context...) { op.io(d.field); ... }
//
// and all five operations are that procedure walked by one of the operation classes below.
// Members left out are not on the wire; a group present under a condition is an ordinary if;
// genuinely asymmetric formats (the segwit marker) branch on Op::Writing. A type serving as
// the base of a format-changing heir (BTC::Proto::MessageVersion, whose XPM heir drops the
// relay field) opens its io with
//
//   static_assert(std::is_same_v<std::remove_cv_t<Self>, X>);
//
// so an heir that forgot its own io fails loudly instead of picking up the base format;
// heirs that keep the format inherit io as is.
//
// The op interface: io(member [, context]) — a field, another self-described type or a leaf;
// vec(member [, context]) — a vector field, returns the element count on both directions, so
// counts read from the stream can drive later gates on every pass; raw(member) — unions and
// paddingless aggregates written as bytes; varint(member) — an integer in Core's base 128 form
// rather than fixed width little endian; check(ok) — a format rule, makes the readers fail;
// put(v)/get(v) — direction-specific scalars for wire words that are not members (packed
// version bits, prefix bytes, the segwit marker); element(vec, i, fn) — per-element access
// on the reading passes (the measuring pass has no elements and hands fn a throwaway).

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
    if constexpr (requires { U::io(*this, v, ctx...); }) {
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
    if constexpr (sizeof...(Ctx) != 0)
      Size += Io<xvector<U>, Ctx...>::getSerializedSize(v, ctx...);
    else
      Size += Io<xvector<U>>::getSerializedSize(v);
    return v.size();
  }

  template<typename U> inline void raw(const U&) { Size += sizeof(U); }
  template<typename U> inline void varint(const U &v) { Size += getSerializedVarIntSize(v); }
  template<typename U> inline void put(U) { Size += sizeof(U); }
  inline void check(bool) {}
};

struct Writer {
  xmstream &Dst;
  static constexpr bool Writing = true;

  template<typename U, typename... Ctx> inline void io(const U &v, Ctx... ctx) {
    if constexpr (requires { U::io(*this, v, ctx...); }) {
      U::io(*this, v, ctx...);
    } else if constexpr (requires { U::io(*this, v); }) {
      U::io(*this, v);
    } else {
      static_assert(!SelfIo<U>, "self-described type reached the leaf path: missing io context?");
      Io<U>::serialize(Dst, v);
    }
  }

  template<typename U, typename... Ctx> inline size_t vec(const xvector<U> &v, Ctx... ctx) {
    if constexpr (sizeof...(Ctx) != 0)
      Io<xvector<U>, Ctx...>::serialize(Dst, v, ctx...);
    else
      Io<xvector<U>>::serialize(Dst, v);
    return v.size();
  }

  template<typename U> inline void raw(const U &v) { Dst.write(&v, sizeof(U)); }
  template<typename U> inline void varint(const U &v) { serializeVarInt(Dst, v); }
  template<typename U> inline void put(U v) { Dst.writele<U>(v); }
  inline void check(bool) {}
};

struct Reader {
  xmstream &Src;
  static constexpr bool Writing = false;

  template<typename U, typename... Ctx> inline void io(U &v, Ctx... ctx) {
    if constexpr (requires { U::io(*this, v, ctx...); }) {
      U::io(*this, v, ctx...);
    } else if constexpr (requires { U::io(*this, v); }) {
      U::io(*this, v);
    } else {
      static_assert(!SelfIo<U>, "self-described type reached the leaf path: missing io context?");
      Io<U>::unserialize(Src, v);
    }
  }

  template<typename U, typename... Ctx> inline size_t vec(xvector<U> &v, Ctx... ctx) {
    if constexpr (sizeof...(Ctx) != 0)
      Io<xvector<U>, Ctx...>::unserialize(Src, v, ctx...);
    else
      Io<xvector<U>>::unserialize(Src, v);
    return v.size();
  }

  template<typename U> inline void raw(U &v) { Src.read(&v, sizeof(U)); }
  template<typename U> inline void varint(U &v) { unserializeVarInt(Src, v); }
  template<typename U> inline void get(U &v) { v = Src.readle<U>(); }
  inline void check(bool ok) { if (!ok) Src.seekEnd(0, true); }
  template<typename U, typename F> inline void element(xvector<U> &v, size_t i, F body) { body(v[i]); }
};

// The unpack measuring pass: nothing is being built, vector contents only move the stream.
// Scalar members are read into the throwaway object the driver provides, so a gate on an
// already read integer field (the auxpow bit, a message version) works here as well; gates on
// vector contents must use the count vec() returns instead.
struct Measurer {
  xmstream &Src;
  size_t Extra = 0;
  static constexpr bool Writing = false;

  template<typename U, typename... Ctx> inline void io(U &v, Ctx... ctx) {
    if constexpr (requires { U::io(*this, v, ctx...); }) {
      U::io(*this, v, ctx...);
    } else if constexpr (requires { U::io(*this, v); }) {
      U::io(*this, v);
    } else if constexpr (is_simple_numeric<U>::value || std::is_same_v<U, bool>) {
      Io<U>::unserialize(Src, v);
    } else {
      static_assert(!SelfIo<U>, "self-described type reached the leaf path: missing io context?");
      Extra += Io<U>::getUnpackedExtraSize(Src);
    }
  }

  template<typename U, typename... Ctx> inline size_t vec(xvector<U>&, Ctx... ctx) {
    uint64_t count = 0;
    if constexpr (sizeof...(Ctx) != 0)
      Extra += Io<xvector<U>, Ctx...>::getUnpackedExtraSize(Src, &count, ctx...);
    else
      Extra += Io<xvector<U>>::getUnpackedExtraSize(Src, &count);
    return count;
  }

  template<typename U> inline void raw(U&) { Src.seek(sizeof(U)); }
  // Read, not skipped: a varint has no fixed width, and a later gate may depend on the value
  template<typename U> inline void varint(U &v) { unserializeVarInt(Src, v); }
  template<typename U> inline void get(U &v) { v = Src.readle<U>(); }
  inline void check(bool ok) { if (!ok) Src.seekEnd(0, true); }
  template<typename U, typename F> inline void element(xvector<U>&, size_t, F body) {
    U dummy;
    body(dummy);
  }
};

// Building into raw memory the measuring pass sized. The driver placement-constructs the
// whole object first; unpack2 of an arena leaf then rebuilds the member in place — what the
// constructor made is destroyed right before, or an allocating default constructor
// (mpz_class under MPIR) would leak.
struct Unpacker {
  xmstream &Src;
  uint8_t **Extra;
  static constexpr bool Writing = false;

  template<typename U, typename... Ctx> inline void io(U &v, Ctx... ctx) {
    if constexpr (requires { U::io(*this, v, ctx...); }) {
      U::io(*this, v, ctx...);
    } else if constexpr (requires { U::io(*this, v); }) {
      U::io(*this, v);
    } else if constexpr (requires { Io<U>::unpack2(Src, &v, Extra); }) {
      static_assert(!SelfIo<U>, "self-described type reached the leaf path: missing io context?");
      std::destroy_at(&v);
      Io<U>::unpack2(Src, &v, Extra);
    } else {
      // leaves needing no arena space of their own have no unpack2
      static_assert(!SelfIo<U>, "self-described type reached the leaf path: missing io context?");
      Io<U>::unserialize(Src, v);
    }
  }

  template<typename U, typename... Ctx> inline size_t vec(xvector<U> &v, Ctx... ctx) {
    std::destroy_at(&v);
    if constexpr (sizeof...(Ctx) != 0)
      Io<xvector<U>, Ctx...>::unpack2(Src, &v, Extra, ctx...);
    else
      Io<xvector<U>>::unpack2(Src, &v, Extra);
    return v.size();
  }

  template<typename U> inline void raw(U &v) { Src.read(&v, sizeof(U)); }
  template<typename U> inline void varint(U &v) { unserializeVarInt(Src, v); }
  template<typename U> inline void get(U &v) { v = Src.readle<U>(); }
  inline void check(bool ok) { if (!ok) Src.seekEnd(0, true); }
  template<typename U, typename F> inline void element(xvector<U> &v, size_t i, F body) { body(v[i]); }
};

// The five operations of a self-described type are thin drivers over its io
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
    Reader op{src};
    op.io(data, ctx...);
  }

  template<typename... Ctx>
  static inline size_t getUnpackedExtraSize(xmstream &src, Ctx... ctx) {
    T tmp;
    Measurer op{src};
    op.io(tmp, ctx...);
    return op.Extra;
  }

  template<typename... Ctx>
  static inline void unpack2(xmstream &src, T *data, uint8_t **extraData, Ctx... ctx) {
    // Default initialized, not value initialized: members that are not on the wire have no
    // other chance to be constructed, the rest is overwritten from the stream anyway
    new (data) T;
    Unpacker op{src, extraData};
    op.io(*data, ctx...);
  }
};

}
