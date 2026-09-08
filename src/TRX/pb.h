#pragma once

#include "common/baseBlob.h"
#include "common/serialize.h"
#include "common/xvector.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <memory>
#include <new>
#include <string>
#include <type_traits>

namespace TRX {

// Wire types. 3 and 4 (groups) left the language long before TRON existed, so meeting one
// means the reader drifted rather than that the chain is unusual.
enum EPbWire {
  PbVarint  = 0,
  PbFixed64 = 1,
  PbBytes   = 2,
  PbFixed32 = 5
};

// One field as it sits on the wire. A varint or fixed field arrives decoded in Value; a
// length-delimited one keeps Payload pointing into the caller's buffer, nothing is copied.
struct CPbField {
  uint32_t Number = 0;
  uint32_t Wire = PbVarint;
  uint64_t Value = 0;
  const uint8_t *Payload = nullptr;
  size_t Size = 0;
};

// Minimal encoding is not something the protobuf spec demands, but protobuf-java never emits
// padding, and on this data a padded varint means the reader drifted. A negative int64 does
// legitimately fill all ten bytes - it is sign-extended, not zigzagged - so the tenth byte
// carrying exactly the one remaining bit is normal and only a zero there is padding.
inline bool pbVarint(const uint8_t *&p, const uint8_t *end, uint64_t &out)
{
  uint64_t value = 0;
  unsigned shift = 0;
  const uint8_t *start = p;
  while (p < end) {
    const uint8_t byte = *p++;
    if (shift == 63) {
      if (byte != 1)
        return false;
      out = value | (static_cast<uint64_t>(1) << 63);
      return true;
    }
    value |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if (!(byte & 0x80)) {
      if (p - start > 1 && byte == 0)
        return false;
      out = value;
      return true;
    }
    shift += 7;
  }
  return false;
}

// Sequential walk over the fields of one message
class CPbMessage {
public:
  CPbMessage() {}
  CPbMessage(const uint8_t *data, size_t size) : Ptr_(data), End_(data + size) {}

  bool atEnd() const { return Ptr_ >= End_; }
  bool bad() const { return Bad_; }

  // Next field, or false at the end of the message and on anything malformed
  bool next(CPbField &out) {
    if (Bad_ || atEnd())
      return false;

    uint64_t key;
    if (!pbVarint(Ptr_, End_, key))
      return setBad();
    // A tag has a nonzero 29-bit field number and 3 wire bits; check before narrowing.
    if (key < 8 || key > UINT32_MAX)
      return setBad();
    out = CPbField();
    out.Number = static_cast<uint32_t>(key >> 3);
    out.Wire = static_cast<uint32_t>(key & 7);

    switch (out.Wire) {
      case PbVarint:
        if (!pbVarint(Ptr_, End_, out.Value))
          return setBad();
        return true;
      case PbFixed64:
        if (End_ - Ptr_ < 8)
          return setBad();
        out.Value = load(Ptr_, 8);
        Ptr_ += 8;
        return true;
      case PbFixed32:
        if (End_ - Ptr_ < 4)
          return setBad();
        out.Value = load(Ptr_, 4);
        Ptr_ += 4;
        return true;
      case PbBytes: {
        uint64_t length;
        if (!pbVarint(Ptr_, End_, length))
          return setBad();
        if (length > static_cast<uint64_t>(End_ - Ptr_))
          return setBad();
        out.Payload = Ptr_;
        out.Size = static_cast<size_t>(length);
        Ptr_ += length;
        return true;
      }
      default:
        return setBad();
    }
  }

private:
  bool setBad() { Bad_ = true; return false; }

  static uint64_t load(const uint8_t *p, size_t size) {
    uint64_t v = 0;
    for (size_t i = 0; i < size; i++)
      v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
  }

  const uint8_t *Ptr_ = nullptr;
  const uint8_t *End_ = nullptr;
  bool Bad_ = false;
};

// ---------------------------------------------------------------------------
// io helpers: the wire shape of a type is written once, as on the RLP side, as
//
//   template<typename Op, typename Self> static void io(Op &op, Self &d) {
//     op.io(1, d.Field);
//     op.io(11, d.Repeated);
//     op.end();
//   }
//
// The one thing that differs from RLP is that protobuf fields are tagged rather than
// positional, so every line names its field number and an absent field is reset to its default,
// even when reading into a reused object. Field numbers must be declared in
// ascending order, which costs nothing to write and is what the ordered reader below trades
// on. end() is stricter than it looks: it fails on any field the shape did not read, because
// a field this schema does not know about means the reader drifted, exactly the contract
// op.end() has on the RLP side. Failure latches into the shared error string, first fail()
// wins, so a shape carries no error plumbing of its own.
//
// Two readers walk these shapes, and nothing in a shape says which:
//
//   pbRead / pbUnpack                 any field order, each field found by searching
//   pbReadOrdered / pbUnpackOrdered   one forward cursor, no searching at all
//
// The ordered one is what the .tbf files want: they hold the bytes protobuf-java wrote, and
// protobuf-java emits fields in ascending number order, so a cursor that only moves forward
// never has to look back. It refuses to guess - a message whose fields are out of order fails
// rather than being read wrong. The searching reader stays for protobuf from anywhere else,
// where the order is whatever the encoder felt like.
//
// Reading comes in the same two forms as the RLP side: pbRead puts vectors on the heap,
// pbUnpack does the measuring walk and then builds the object with all its elements in one
// allocation. Both leave a self-contained object - byte strings are copied like every other
// member, bcnode's contract - so a decoded block does not depend on the buffer it came from.

template<typename T, typename Enable = void>
struct Io {
  static bool read(const CPbField &field, T &out);
};

template<> struct Io<uint64_t> {
  static bool read(const CPbField &f, uint64_t &out) {
    if (f.Wire != PbVarint)
      return false;
    out = f.Value;
    return true;
  }
};

template<> struct Io<int64_t> {
  static bool read(const CPbField &f, int64_t &out) {
    if (f.Wire != PbVarint)
      return false;
    out = static_cast<int64_t>(f.Value);
    return true;
  }
};

// int32 and the enums: sign-extended to 64 bits on the wire, so the whole int32 range arrives
// as a value that must fold back into 32 bits
template<> struct Io<int32_t> {
  static bool read(const CPbField &f, int32_t &out) {
    if (f.Wire != PbVarint)
      return false;
    const int64_t wide = static_cast<int64_t>(f.Value);
    if (wide < INT32_MIN || wide > INT32_MAX)
      return false;
    out = static_cast<int32_t>(wide);
    return true;
  }
};

template<> struct Io<bool> {
  static bool read(const CPbField &f, bool &out) {
    if (f.Wire != PbVarint || f.Value > 1)
      return false;
    out = f.Value != 0;
    return true;
  }
};

// Fixed-width byte strings: the roots and hashes that are structurally 32 bytes
template<unsigned Bits> struct Io<BaseBlob<Bits>> {
  static bool read(const CPbField &f, BaseBlob<Bits> &out) {
    if (f.Wire != PbBytes || f.Size != out.size())
      return false;
    memcpy(out.begin(), f.Payload, f.Size);
    return true;
  }
};

// Probes only the call shape, so the io body is not instantiated
struct CPbIoProbe {};

template<typename T> concept PbSelfIo = requires(CPbIoProbe &op, T &d) { T::io(op, d); };

// What tells a repeated field from a singular one: the member's type, exactly as on the RLP
// side. xvector<uint8_t> is one `bytes` field; any other xvector is a field that repeats.
template<typename T> struct IsRepeated : std::false_type {};
template<typename T> struct IsRepeated<xvector<T>> : std::true_type {};
template<> struct IsRepeated<xvector<uint8_t>> : std::false_type {};

// Survey mode: a property of the run rather than of one reader, so both share it. Off by
// default - a schema that claims to be complete has to fail loudly, and a field nobody
// declared is the one signal that the reader and the chain have diverged. Turn it on to walk
// a stretch the schema has not been checked against yet and collect every missing field in
// one pass instead of one per run. Which message a number turned up in is not recorded;
// `protoc --decode_raw` on the block answers that in a second.
struct CPbSurvey {
  static bool Lax;
  static uint64_t Seen;
  static bool SeenHigh;
};

// Everything the two readers share: the failure latch, which of the three passes is running,
// and how one field becomes a value. CRTP so a nested message is walked by the same reader as
// its parent - which reader that is, is the caller's choice and never the shape's.
template<typename TDerived>
class CPbReaderBase : public Ser::CReaderState {
public:
  CPbReaderBase(const uint8_t *data, size_t size, Ser::CIoStatus &status)
    : Ser::CReaderState(status), Data_(data), Size_(size) {}
  CPbReaderBase(const uint8_t *data, size_t size, Ser::CIoStatus &status, size_t *extra)
    : Ser::CReaderState(status, extra), Data_(data), Size_(size) {}
  CPbReaderBase(const uint8_t *data, size_t size, Ser::CIoStatus &status, uint8_t **arena)
    : Ser::CReaderState(status, arena), Data_(data), Size_(size) {}

protected:
  // A sub-walk shares the failure latch and the pass it runs under
  CPbReaderBase(const CPbReaderBase &parent, const uint8_t *data, size_t size)
    : Ser::CReaderState(parent), Data_(data), Size_(size) {}

  static constexpr uint32_t ClaimLimit = 64;

  // A field no shape ever asked for
  void unclaimed(uint32_t number) {
    if (CPbSurvey::Lax) {
      if (number < ClaimLimit)
        CPbSurvey::Seen |= uint64_t(1) << number;
      else
        CPbSurvey::SeenHigh = true;
      return;
    }
    fail("has an unclaimed field " + std::to_string(number));
  }

  template<typename T> void resetField(T &out) {
    // Unpack passes start with fresh objects; only a plain read can carry old fields.
    if (pass() != Ser::EPass::Read || failed())
      return;
    std::destroy_at(&out);
    std::construct_at(&out);
  }

  template<typename T> void readField(const CPbField &field, T &out) {
    if constexpr (std::is_same_v<T, xvector<uint8_t>>) {
      if (field.Wire != PbBytes) {
        fail("field " + std::to_string(field.Number) + " is not a byte string");
        return;
      }
      byteString(field, out);
    } else if constexpr (PbSelfIo<T>) {
      if (field.Wire != PbBytes) {
        fail("field " + std::to_string(field.Number) + " is not a message");
        return;
      }
      TDerived op(static_cast<TDerived&>(*this), field.Payload, field.Size);
      T::io(op, out);
    } else if (!Io<T>::read(field, out)) {
      fail("field " + std::to_string(field.Number) + " has an unexpected shape");
    }
  }

  // Byte-string contents go where a repeated field's elements do, so either reading form
  // leaves the object standing on its own
  void byteString(const CPbField &field, xvector<uint8_t> &v) {
    if (uint8_t *data = this->prepare(v, field.Size))
      memcpy(data, field.Payload, field.Size);
  }

  const uint8_t *Data_ = nullptr;
  size_t Size_ = 0;
};

// Finds each field by searching the message for its number: a walk per field, which is what
// protobuf of unknown provenance costs. Use it when the encoder is not known to be ordered.
class CPbReader : public CPbReaderBase<CPbReader> {
  using CBase = CPbReaderBase<CPbReader>;
  friend CBase;

public:
  using CBase::CBase;

  template<typename T> void io(uint32_t number, T &v) {
    if constexpr (IsRepeated<T>::value) {
      vec(number, v);
    } else {
      CPbField field;
      if (!findOne(number, field, false))
        return;
      if (field.Number)
        this->readField(field, v);
      else
        this->resetField(v);
    }
  }

  template<typename T> void required(uint32_t number, T &v) {
    CPbField field;
    if (!findOne(number, field, true))
      return;
    this->readField(field, v);
  }

private:
  template<typename T> void vec(uint32_t number, xvector<T> &v) {
    if (!claim(number))
      return;

    CPbMessage scan(this->Data_, this->Size_);
    CPbField field;
    size_t n = 0;
    while (scan.next(field)) {
      if (field.Number == number)
        n++;
    }
    if (scan.bad()) {
      this->fail("does not parse");
      return;
    }

    T *elements = this->prepare(v, n);
    scan = CPbMessage(this->Data_, this->Size_);
    size_t index = 0;
    const size_t saved = this->element();
    while (scan.next(field) && !this->failed()) {
      if (field.Number != number)
        continue;
      this->setElement(index);
      if (elements) {
        this->readField(field, elements[index]);
      } else {
        T throwaway;
        this->readField(field, throwaway);
      }
      index++;
    }
    this->setElement(saved);
  }

public:
  void end() {
    if (this->failed())
      return;
    CPbMessage scan(this->Data_, this->Size_);
    CPbField field;
    while (scan.next(field)) {
      if (field.Number < ClaimLimit && (Claimed_ & (uint64_t(1) << field.Number)))
        continue;
      this->unclaimed(field.Number);
      if (this->failed())
        return;
    }
    if (scan.bad())
      this->fail("does not parse");
  }

private:
  bool claim(uint32_t number) {
    if (this->failed())
      return false;
    if (number == 0 || number >= ClaimLimit) {
      this->fail("shape declares field " + std::to_string(number) +
                 ", which this reader cannot track");
      return false;
    }
    Claimed_ |= uint64_t(1) << number;
    return true;
  }

  // The single occurrence of a field. out.Number stays 0 when the field is absent and that is
  // allowed; two occurrences of a singular field always fail.
  bool findOne(uint32_t number, CPbField &out, bool mustExist) {
    if (!claim(number))
      return false;
    CPbMessage scan(this->Data_, this->Size_);
    CPbField field;
    bool found = false;
    while (scan.next(field)) {
      if (field.Number != number)
        continue;
      if (found) {
        this->fail("carries field " + std::to_string(number) + " more than once");
        return false;
      }
      out = field;
      found = true;
    }
    if (scan.bad()) {
      this->fail("does not parse");
      return false;
    }
    if (!found) {
      if (mustExist) {
        this->fail("has no field " + std::to_string(number));
        return false;
      }
      out = CPbField();
    }
    return true;
  }

  uint64_t Claimed_ = 0;
};

// One cursor, forward only. Every message protobuf-java writes carries its fields in ascending
// number order, and a shape declares them in that same order, so the cursor either sits on the
// field being asked for or has already passed it - and having passed it is exactly what
// absence looks like. Nothing is ever searched for, and a repeated field's run is counted by
// peeking over just that run. A message whose fields are not ascending is refused rather than
// read wrong, and a shape that declares them out of order is a bug this says so about.
class CPbOrderedReader : public CPbReaderBase<CPbOrderedReader> {
  using CBase = CPbReaderBase<CPbOrderedReader>;
  friend CBase;

public:
  CPbOrderedReader(const uint8_t *data, size_t size, Ser::CIoStatus &status)
    : CBase(data, size, status) { start(); }
  CPbOrderedReader(const uint8_t *data, size_t size, Ser::CIoStatus &status, size_t *extra)
    : CBase(data, size, status, extra) { start(); }
  CPbOrderedReader(const uint8_t *data, size_t size, Ser::CIoStatus &status, uint8_t **arena)
    : CBase(data, size, status, arena) { start(); }

  template<typename T> void io(uint32_t number, T &v) {
    if constexpr (IsRepeated<T>::value) {
      vec(number, v);
    } else if (seek(number)) {
      take(number, v);
    } else {
      this->resetField(v);
    }
  }

  template<typename T> void required(uint32_t number, T &v) {
    if (!seek(number)) {
      if (!this->failed())
        this->fail("has no field " + std::to_string(number));
      return;
    }
    take(number, v);
  }

  void end() {
    while (Have_ && !this->failed()) {
      this->unclaimed(Current_.Number);
      advance();
    }
    if (Scan_.bad())
      this->fail("does not parse");
  }

private:
  template<typename T> void vec(uint32_t number, xvector<T> &v) {
    if (!seek(number)) {
      this->prepare(v, 0);
      return;
    }

    const size_t n = runLength(number);
    T *elements = this->prepare(v, n);
    const size_t saved = this->element();
    for (size_t i = 0; i < n && !this->failed(); i++) {
      this->setElement(i);
      if (elements) {
        this->readField(Current_, elements[i]);
      } else {
        T throwaway;
        this->readField(Current_, throwaway);
      }
      advance();
    }
    this->setElement(saved);
  }

  CPbOrderedReader(const CPbOrderedReader &parent, const uint8_t *data, size_t size)
    : CBase(parent, data, size) { start(); }

  void start() {
    Scan_ = CPbMessage(this->Data_, this->Size_);
    Have_ = Scan_.next(Current_);
    if (!Have_ && Scan_.bad())
      this->fail("does not parse");
  }

  void advance() {
    const uint32_t previous = Current_.Number;
    Have_ = Scan_.next(Current_);
    if (!Have_) {
      if (Scan_.bad())
        this->fail("does not parse");
      return;
    }
    if (Current_.Number < previous)
      this->fail("has field " + std::to_string(Current_.Number) + " after field " +
                 std::to_string(previous) + ", so its fields are not ascending");
  }

  template<typename T> void take(uint32_t number, T &v) {
    this->readField(Current_, v);
    advance();
    if (Have_ && Current_.Number == number)
      this->fail("carries field " + std::to_string(number) + " more than once");
  }

  // Leaves the cursor on the first field numbered at least 'number'; true when that is the
  // field itself. Anything stepped over on the way is a field no shape asked for.
  bool seek(uint32_t number) {
    if (this->failed())
      return false;
    if (number <= Requested_) {
      this->fail("shape asks for field " + std::to_string(number) + " after field " +
                 std::to_string(Requested_) + ", but this reader needs them ascending");
      return false;
    }
    Requested_ = number;

    while (Have_ && Current_.Number < number) {
      this->unclaimed(Current_.Number);
      if (this->failed())
        return false;
      advance();
    }
    return !this->failed() && Have_ && Current_.Number == number;
  }

  // The occurrences of a repeated field sit together in an ordered message, so the run is
  // counted by peeking over just those and costs nothing beyond them
  size_t runLength(uint32_t number) const {
    CPbMessage peek = Scan_;
    CPbField field;
    size_t n = 1;                     // the cursor is already on the first one
    while (peek.next(field) && field.Number == number)
      n++;
    return n;
  }

  CPbMessage Scan_;
  CPbField Current_;
  bool Have_ = false;
  uint32_t Requested_ = 0;
};

// The whole decode of one entity, vectors on the heap
template<typename TReader, typename T>
inline bool pbReadWith(const uint8_t *data, size_t size, T &out, std::string &error)
{
  error.clear();
  Ser::CIoStatus status;
  status.Name = T::Name;
  status.Error = &error;
  TReader op(data, size, status);
  T::io(op, out);
  return !op.failed();
}

// The measuring walk sizes every repeated field, then the object and all its elements are
// built in one allocation. The measuring walk decodes into throwaways, so everything is
// already validated when building starts.
template<typename TReader, typename T>
inline T *pbUnpackWith(const uint8_t *data, size_t size, std::string &error, size_t *objectSize)
{
  error.clear();
  Ser::CIoStatus status;
  status.Name = T::Name;
  status.Error = &error;
  return Ser::unpackObject<T>(status, objectSize, [&](size_t *extra, uint8_t **arena, T &target) {
    if (extra) {
      TReader op(data, size, status, extra);
      T::io(op, target);
    } else {
      TReader op(data, size, status, arena);
      T::io(op, target);
    }
  });
}

template<typename T>
inline bool pbRead(const uint8_t *data, size_t size, T &out, std::string &error)
{
  return pbReadWith<CPbReader, T>(data, size, out, error);
}

template<typename T>
inline bool pbReadOrdered(const uint8_t *data, size_t size, T &out, std::string &error)
{
  return pbReadWith<CPbOrderedReader, T>(data, size, out, error);
}

template<typename T>
inline T *pbUnpack(const uint8_t *data, size_t size, std::string &error, size_t *objectSize = nullptr)
{
  return pbUnpackWith<CPbReader, T>(data, size, error, objectSize);
}

template<typename T>
inline T *pbUnpackOrdered(const uint8_t *data, size_t size, std::string &error, size_t *objectSize = nullptr)
{
  return pbUnpackWith<CPbOrderedReader, T>(data, size, error, objectSize);
}

// Freeing an unpacked object is the skeleton's business: nothing here owns anything of its
// own, every vector - byte strings included - points back into the same allocation
using Ser::CUnpackedPtr;

}
