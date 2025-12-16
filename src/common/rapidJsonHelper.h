#pragma once
#include "rapidjson/document.h"
#include "common/uint256.h"

static inline uint8_t hexParse(char c)
{
  uint8_t digit = c - '0';
  if (digit >= 10)
    digit -= ('A' - '0' - 10);
  if (digit >= 16)
    digit -= ('a' - 'A');
  return digit < 16 ? digit : 0xFF;
}

template<unsigned BITS>
static inline void jsonParseBaseBlob(rapidjson::Value &document,
                                     const char *name,
                                     base_blob<BITS> &out,
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
  if (!field.IsString()) {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
    return;
  }

  auto s = document[name].GetString();
  auto l = document[name].GetStringLength();
  if (l != BITS/8 * 2) {
    *validAcc = false;
    if (errorPoint.empty())
      errorPoint = name;
    return;
  }

  uint8_t *pOut = static_cast<uint8_t*>(out.begin());
  for (size_t i = 0; i < BITS/8; i++) {
    uint8_t lo  = hexParse(s[i*2+1]);
    uint8_t hi = hexParse(s[i*2]);
    if (lo == 0xFF || hi == 0xFF) {
      *validAcc = false;
      if (errorPoint.empty())
        errorPoint = name;
      return;
    }

    pOut[BITS/8 - i - 1] = (hi << 4) | lo;
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
  if (document.HasMember(name)) {
    if (document[name].IsUint64()) {
      *out = document[name].GetUint64();
    } else {
      *validAcc = false;
      if (errorPoint.empty())
        errorPoint = name;
    }
  } else {
    *out = defaultValue;
  }
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
