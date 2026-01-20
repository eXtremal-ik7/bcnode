#pragma once

#include "common/baseBlob.h"
#include "common/uint.h"
#include "common/xvector.h"
#include "p2putils/xmstream.h"
#include <array>
#include <string>

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

static inline void unserializeVarSize(xmstream &stream, uint64_t &out)
{
  uint8_t type = stream.read<uint8_t>();
  if (type < 0xFD)
    out = type;
  else if (type == 0xFD)
    out = stream.readle<uint16_t>();
  else if (type == 0xFE)
    out = stream.readle<uint32_t>();
  else
    out = stream.readle<uint64_t>();
}

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
  static inline size_t getSerializedSize(const std::array<uint8_t, Size> &data) {
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

  static inline void unpack2(xmstream &src, std::array<uint8_t, Size> *data, uint8_t **extraData) {
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
template<typename T, typename ContextTy> struct Io<xvector<T>, ContextTy> {
  static inline size_t getSerializedSize(const xvector<T> &data, ContextTy context) {
    size_t size = getSerializedVarSizeSize(data.size());
    for (const auto &v: data)
      size += Io<T, ContextTy>::getSerializedSize(v, context);
    return size;
  }

  static inline size_t getUnpackedExtraSize(xmstream &src, uint64_t *count, ContextTy context) {
    unserializeVarSize(src, *count);

    size_t result = 0;
    for (size_t i = 0; i < *count; i++)
      result += Io<T, ContextTy>::getUnpackedExtraSize(src, context);
    return *count*sizeof(T) + result;
  }

  static inline size_t getUnpackedExtraSize(xmstream &src, ContextTy context) {
    uint64_t size;
    return getUnpackedExtraSize(src, &size, context);
  }

  static inline void serialize(xmstream &dst, const xvector<T> &data, ContextTy context) {
    serializeVarSize(dst, data.size());
    for (const auto &v: data)
      Io<T, ContextTy>::serialize(dst, v, context);
  }

  static inline void unserialize(xmstream &src, xvector<T> &data, ContextTy context) {
    uint64_t size = 0;
    unserializeVarSize(src, size);
    if (size > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.resize(size);
    for (uint64_t i = 0; i < size; i++)
      Io<T, ContextTy>::unserialize(src, data[i], context);
  }

  static inline void unpack2(xmstream &src, xvector<T> *data, uint8_t **extraData, ContextTy context) {
    uint64_t size;
    unserializeVarSize(src, size);

    T *elementsData = reinterpret_cast<T*>(*extraData);
    new (data) xvector<T>(elementsData, size);
    (*extraData) += sizeof(T)*size;
    for (size_t i = 0; i < size; i++)
      BTC::Io<T, ContextTy>::unpack2(src, &elementsData[i], extraData, context);
  }
};

// Special case: xvector<uint8_t>
template<> struct Io<xvector<uint8_t>> {
  static inline size_t getSerializedSize(const xvector<uint8_t> &data) {
    return getSerializedVarSizeSize(data.size()) + data.size();
  }

  static inline size_t getUnpackedExtraSize(xmstream &src) {
    uint64_t size;
    unserializeVarSize(src, size);
    src.seek(size);
    return aligned(size, UnpackAlignment);
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

}
