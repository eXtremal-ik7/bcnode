// Copyright (c) 2017, 2021 Pieter Wuille
// Copyright (c) 2026 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bech32.h"

namespace {

const char Charset[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

const int8_t CharsetRev[128] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    15, -1, 10, 17, 21, 20, 26, 30,  7,  5, -1, -1, -1, -1, -1, -1,
    -1, 29, -1, 24, 13, 25,  9,  8, 23, -1, 18, 22, 31, 27, 19, -1,
     1,  0,  3, 16, 11, 28, 12, 14,  6,  4,  2, -1, -1, -1, -1, -1,
    -1, 29, -1, 24, 13, 25,  9,  8, 23, -1, 18, 22, 31, 27, 19, -1,
     1,  0,  3, 16, 11, 28, 12, 14,  6,  4,  2, -1, -1, -1, -1, -1
};

constexpr uint32_t Bech32Const = 1;
constexpr uint32_t Bech32mConst = 0x2bc830a3;

uint32_t polymod(const std::vector<uint8_t> &values)
{
  static const uint32_t Generator[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
  uint32_t chk = 1;
  for (uint8_t value: values) {
    uint8_t top = chk >> 25;
    chk = (chk & 0x1ffffff) << 5 ^ value;
    for (int i = 0; i < 5; i++)
      chk ^= (top >> i) & 1 ? Generator[i] : 0;
  }
  return chk;
}

void hrpExpand(const std::string &hrp, std::vector<uint8_t> &out)
{
  for (char c: hrp)
    out.push_back(c >> 5);
  out.push_back(0);
  for (char c: hrp)
    out.push_back(c & 31);
}

// General power-of-2 base conversion (BIP173 reference)
bool convertBits(const uint8_t *in, size_t inSize, unsigned fromBits, unsigned toBits, bool pad, std::vector<uint8_t> &out)
{
  uint32_t acc = 0;
  unsigned bits = 0;
  const uint32_t maxv = (1u << toBits) - 1;
  const uint32_t maxAcc = (1u << (fromBits + toBits - 1)) - 1;
  for (size_t i = 0; i < inSize; i++) {
    acc = ((acc << fromBits) | in[i]) & maxAcc;
    bits += fromBits;
    while (bits >= toBits) {
      bits -= toBits;
      out.push_back((acc >> bits) & maxv);
    }
  }
  if (pad) {
    if (bits)
      out.push_back((acc << (toBits - bits)) & maxv);
  } else if (bits >= fromBits || ((acc << (toBits - bits)) & maxv)) {
    return false;
  }
  return true;
}

std::string encode(const std::string &hrp, const std::vector<uint8_t> &values, uint32_t checksumConst)
{
  std::vector<uint8_t> data;
  hrpExpand(hrp, data);
  data.insert(data.end(), values.begin(), values.end());
  data.resize(data.size() + 6, 0);
  uint32_t mod = polymod(data) ^ checksumConst;

  std::string result = hrp + '1';
  for (uint8_t value: values)
    result.push_back(Charset[value]);
  for (int i = 0; i < 6; i++)
    result.push_back(Charset[(mod >> (5 * (5 - i))) & 31]);
  return result;
}

// Charset/case/layout check plus checksum evaluation; which variant matched is
// returned through checksumConst
bool decode(const std::string &address, std::string &hrp, std::vector<uint8_t> &values, uint32_t *checksumConst)
{
  if (address.size() > 90)
    return false;

  bool hasLower = false, hasUpper = false;
  for (char c: address) {
    if (c < 33 || c > 126)
      return false;
    hasLower |= (c >= 'a' && c <= 'z');
    hasUpper |= (c >= 'A' && c <= 'Z');
  }
  if (hasLower && hasUpper)
    return false;

  size_t pos = address.rfind('1');
  if (pos == std::string::npos || pos == 0 || pos + 7 > address.size())
    return false;

  hrp.clear();
  for (size_t i = 0; i < pos; i++) {
    char c = address[i];
    hrp.push_back(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }

  values.clear();
  for (size_t i = pos + 1; i < address.size(); i++) {
    int8_t rev = CharsetRev[static_cast<uint8_t>(address[i]) & 0x7f];
    if (rev == -1)
      return false;
    values.push_back(rev);
  }

  std::vector<uint8_t> data;
  hrpExpand(hrp, data);
  data.insert(data.end(), values.begin(), values.end());
  uint32_t mod = polymod(data);
  if (mod != Bech32Const && mod != Bech32mConst)
    return false;

  *checksumConst = mod;
  values.resize(values.size() - 6);
  return true;
}

}

namespace Bech32 {

std::string encodeSegwitAddress(const std::string &hrp, unsigned witnessVersion, const uint8_t *program, size_t programSize)
{
  if (witnessVersion > 16 || programSize < 2 || programSize > 40)
    return std::string();

  std::vector<uint8_t> values;
  values.push_back(witnessVersion);
  convertBits(program, programSize, 8, 5, true, values);
  return encode(hrp, values, witnessVersion == 0 ? Bech32Const : Bech32mConst);
}

bool decodeSegwitAddress(const std::string &hrp, const std::string &address, unsigned *witnessVersion, std::vector<uint8_t> &program)
{
  std::string decodedHrp;
  std::vector<uint8_t> values;
  uint32_t checksumConst;
  if (!decode(address, decodedHrp, values, &checksumConst) || decodedHrp != hrp || values.empty())
    return false;

  unsigned version = values[0];
  if (version > 16 || (version == 0 ? checksumConst != Bech32Const : checksumConst != Bech32mConst))
    return false;

  program.clear();
  if (!convertBits(values.data() + 1, values.size() - 1, 5, 8, false, program))
    return false;
  if (program.size() < 2 || program.size() > 40 || (version == 0 && program.size() != 20 && program.size() != 32))
    return false;

  *witnessVersion = version;
  return true;
}

}
