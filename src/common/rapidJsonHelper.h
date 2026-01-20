#pragma once
#include "rapidjson/document.h"
#include "common/baseBlob.h"
#include <optional>

static inline uint8_t hexParse(char c)
{
  uint8_t digit = c - '0';
  if (digit >= 10)
    digit -= ('A' - '0' - 10);
  if (digit >= 16)
    digit -= ('a' - 'A');
  return digit < 16 ? digit : 0xFF;
}

static inline bool hexParse(uint8_t *out, const rapidjson::GenericValue<rapidjson::UTF8<>> &field, size_t size)
{
  auto s = field.GetString();
  auto l = field.GetStringLength();
  if (l != size * 2)
    return false;

  for (size_t i = 0; i < size; i++) {
    uint8_t lo  = hexParse(s[i*2+1]);
    uint8_t hi = hexParse(s[i*2]);
    if (lo == 0xFF || hi == 0xFF)
      return false;

    out[size - i - 1] = (hi << 4) | lo;
  }

  return true;
}

template<unsigned Bits>
static inline void jsonParseBaseBlob(rapidjson::Value &document,
                                     const char *name,
                                     BaseBlob<Bits> &out,
                                     bool *validAcc,
                                     std::string &errorPoint)
{
  if (!document.HasMember(name)) {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
    return;
  }

  const auto &field = document[name];
  if (!field.IsString() || !hexParse(out.begin(), field, Bits/8)) {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
    return;
  }
}

template<unsigned Bits>
static inline void jsonParseBaseBlob(rapidjson::Value &document,
                                     const char *name,
                                     BaseBlob<Bits> &out,
                                     const BaseBlob<Bits> &defaultValue,
                                     bool *validAcc,
                                     std::string &errorPoint)
{
  if (!document.HasMember(name)) {
    out = defaultValue;
    return;
  }

  const auto &field = document[name];
  if (!field.IsString() || !hexParse(out.begin(), field, Bits/8)) {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
    return;
  }
}

template<unsigned Bits>
static inline void jsonParseBaseBlob(rapidjson::Value &document,
                                     const char *name,
                                     std::optional<BaseBlob<Bits>> &out,
                                     bool *validAcc,
                                     std::string &errorPoint)
{
  if (!document.HasMember(name)) {
    out.reset();
    return;
  }

  out.emplace();
  const auto &field = document[name];
  if (!field.IsString() || !hexParse(out.value().begin(), field, Bits/8)) {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
    return;
  }
}

static inline void jsonParseUInt64(rapidjson::Value &document,
                                   const char *name,
                                   uint64_t *out,
                                   bool *validAcc,
                                   std::string &errorPoint)
{
  if (document.HasMember(name) && document[name].IsUint64()) {
    *out = document[name].GetUint64();
  } else {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
  }
}

static inline void jsonParseUInt64(rapidjson::Value &document,
                                   const char *name,
                                   uint64_t *out,
                                   uint64_t defaultValue,
                                   bool *validAcc,
                                   std::string &errorPoint)
{
  if (!document.HasMember(name)) {
    *out = defaultValue;
    return;
  }

  const auto &field = document[name];
  if (!field.IsUint64()) {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
  }

  *out = field.GetUint64();
}

static inline void jsonParseUInt64(rapidjson::Value &document,
                                   const char *name,
                                   std::optional<uint64_t> &out,
                                   bool *validAcc,
                                   std::string &errorPoint)
{
  if (!document.HasMember(name)) {
    out.reset();
    return;
  }

  const auto &field = document[name];
  if (!field.IsUint64()) {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
  }

  out.emplace(field.GetUint64());
}

static inline void jsonParseString(rapidjson::Value &document,
                                   const char *name,
                                   std::string &out,
                                   bool *validAcc,
                                   std::string &errorPoint)
{
  if (document.HasMember(name) && document[name].IsString()) {
    out = document[name].GetString();
  } else {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
  }
}

static inline void jsonParseString(rapidjson::Value &document,
                                   const char *name,
                                   std::string &out,
                                   const std::string &defaultValue,
                                   bool *validAcc,
                                   std::string &errorPoint)
{
  if (document.HasMember(name)) {
    if (document[name].IsString()) {
      out = document[name].GetString();
    } else {
      *validAcc = false;
      if (errorPoint.empty())
        errorPoint = name;
    }
  } else {
    out = defaultValue;
  }
}
