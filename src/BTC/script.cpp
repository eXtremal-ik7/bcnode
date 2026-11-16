#include "script.h"
#include "hash.h"
#include "common/base58.h"
#include "common/bech32.h"
#include "common/utils.h"
#include <ctype.h>
#include <initializer_list>

namespace {

using BTC::Script;

// CPubKey::ValidSize: the size the prefix byte promises
bool validKeySize(const Script::CSpan &data)
{
  if (!data.Size)
    return false;
  const uint8_t prefix = data.Data[0];
  if (prefix == 2 || prefix == 3)
    return data.Size == 33;
  if (prefix == 4 || prefix == 6 || prefix == 7)
    return data.Size == 65;
  return false;
}

// Core's GetScriptNumber within 1..20: OP_1..OP_16, or a direct one-byte push of 17..20 - the
// only minimal encodings there are. 0 for anything else
unsigned multisigNumber(uint8_t opcode, const Script::CSpan &data)
{
  if (opcode >= Script::OP_1 && opcode <= Script::OP_16)
    return opcode - Script::OP_1 + 1;
  if (opcode == 1 && data.Size == 1 && data.Data[0] >= 17 && data.Data[0] <= 20)
    return data.Data[0];
  return 0;
}

// Core's MatchMultisig: m <keys> n OP_CHECKMULTISIG, 1 <= m <= n <= 20, keys of any push
bool isBareMultisig(const uint8_t *script, size_t size)
{
  if (size < 1 || script[size - 1] != Script::OP_CHECKMULTISIG)
    return false;

  const uint8_t *p = script;
  const uint8_t *end = script + size;
  uint8_t opcode;
  Script::CSpan data;
  if (!Script::getOp(p, end, opcode, &data))
    return false;
  const unsigned required = multisigNumber(opcode, data);
  if (!required)
    return false;

  unsigned keys = 0;
  for (;;) {
    data = {};
    if (!Script::getOp(p, end, opcode, &data))
      return false;
    if (opcode > Script::OP_PUSHDATA4 || !validKeySize(data))
      break;
    keys++;
  }

  const unsigned declared = multisigNumber(opcode, data);
  return declared >= required && keys == declared && p + 1 == end;
}

void assemble(xvector<uint8_t> &script,
              std::initializer_list<uint8_t> prefix,
              const uint8_t *data,
              size_t size,
              std::initializer_list<uint8_t> suffix)
{
  script.resize(prefix.size() + size + suffix.size());
  uint8_t *p = script.data();
  memcpy(p, prefix.begin(), prefix.size());
  if (size)
    memcpy(p + prefix.size(), data, size);
  if (suffix.size())
    memcpy(p + prefix.size() + size, suffix.begin(), suffix.size());
}

}

namespace BTC {

bool Script::getOp(const uint8_t *&p, const uint8_t *end, uint8_t &opcode, CSpan *data)
{
  if (p >= end)
    return false;
  opcode = *p++;
  if (opcode > OP_PUSHDATA4)
    return true;

  size_t size = opcode;
  if (opcode == OP_PUSHDATA1) {
    if (end - p < 1)
      return false;
    size = p[0];
    p += 1;
  } else if (opcode == OP_PUSHDATA2) {
    if (end - p < 2)
      return false;
    size = p[0] | (static_cast<size_t>(p[1]) << 8);
    p += 2;
  } else if (opcode == OP_PUSHDATA4) {
    if (end - p < 4)
      return false;
    size = p[0] | (static_cast<size_t>(p[1]) << 8) | (static_cast<size_t>(p[2]) << 16) | (static_cast<size_t>(p[3]) << 24);
    p += 4;
  }

  if (static_cast<size_t>(end - p) < size)
    return false;
  if (data)
    *data = {p, size};
  p += size;
  return true;
}

unsigned Script::sigOpCount(const uint8_t *p, const uint8_t *end, bool accurate)
{
  unsigned count = 0;
  uint8_t last = 0xFF;
  uint8_t opcode;
  while (p < end && getOp(p, end, opcode)) {
    if (opcode == OP_CHECKSIG || opcode == OP_CHECKSIGVERIFY) {
      count++;
    } else if (opcode == OP_CHECKMULTISIG || opcode == OP_CHECKMULTISIGVERIFY) {
      count += accurate && last >= OP_1 && last <= OP_16 ? last - OP_1 + 1 : 20;
    }
    last = opcode;
  }
  return count;
}

bool Script::witnessProgram(const uint8_t *script, size_t size, int &version, CSpan &program)
{
  if (size < 4 || size > 42)
    return false;
  if (script[0] != OP_0 && (script[0] < OP_1 || script[0] > OP_16))
    return false;
  if (static_cast<size_t>(script[1]) + 2 != size)
    return false;
  version = script[0] == OP_0 ? 0 : script[0] - OP_1 + 1;
  program = {script + 2, size - 2};
  return true;
}

uint8_t Script::classify(const uint8_t *s, size_t size)
{
  // In Solver's order
  if (size == 23 && s[0] == OP_HASH160 && s[1] == OP_PUSH20 && s[22] == OP_EQUAL)
    return CUnspentOutputInfo::EScriptHash;

  int version;
  CSpan program;
  if (witnessProgram(s, size, version, program)) {
    if (version == 0 && program.Size == 20)
      return CUnspentOutputInfo::EWitnessPubKeyHash;
    if (version == 0 && program.Size == 32)
      return CUnspentOutputInfo::EWitnessScriptHash;
    if (version == 1 && program.Size == 32)
      return CUnspentOutputInfo::EWitnessTaproot;
    if (version == 1 && program.Size == 2 && program.Data[0] == 0x4E && program.Data[1] == 0x73)
      return CUnspentOutputInfo::EAnchor;
    return version != 0 ? CUnspentOutputInfo::EWitnessUnknown : CUnspentOutputInfo::ENonStandard;
  }

  if (size >= 1 && s[0] == OP_RETURN)
    return CUnspentOutputInfo::EOpReturn;

  if (size == 35 && s[0] == OP_PUSH_33 && s[34] == OP_CHECKSIG && (s[1] == 2 || s[1] == 3))
    return CUnspentOutputInfo::EPubKeyCompressed;
  if (size == 67 && s[0] == OP_PUSH_65 && s[66] == OP_CHECKSIG) {
    if (s[1] == 4)
      return CUnspentOutputInfo::EPubKeyUncompressed;
    if (s[1] == 6 || s[1] == 7)
      return CUnspentOutputInfo::EPubKeyHybrid;
    return CUnspentOutputInfo::ENonStandard;
  }

  if (size == 25 && s[0] == OP_DUP && s[1] == OP_HASH160 && s[2] == OP_PUSH20 &&
      s[23] == OP_EQUALVERIFY && s[24] == OP_CHECKSIG)
    return CUnspentOutputInfo::EPubKeyHash;

  if (isBareMultisig(s, size))
    return CUnspentOutputInfo::EMultisig;
  return CUnspentOutputInfo::ENonStandard;
}

Script::CSpan Script::storedScript(const CUnspentOutputInfo &info)
{
  const uint8_t *p = info.CustomData;
  size_t prefix = 1;
  uint64_t size = p[0];
  if (p[0] >= 0xFD) {
    prefix = p[0] == 0xFD ? 3 : p[0] == 0xFE ? 5 : 9;
    size = 0;
    for (size_t i = 1; i < prefix; i++)
      size |= static_cast<uint64_t>(p[i]) << (8 * (i - 1));
  }
  return {p + prefix, static_cast<size_t>(size)};
}

unsigned Script::sigOpCount(const CUnspentOutputInfo &info)
{
  switch (info.Type) {
    case CUnspentOutputInfo::EPubKeyCompressed :
    case CUnspentOutputInfo::EPubKeyUncompressed :
    case CUnspentOutputInfo::EPubKeyHybrid :
    case CUnspentOutputInfo::EPubKeyHash :
      return 1;
    case CUnspentOutputInfo::EMultisig :
    case CUnspentOutputInfo::ENonStandard : {
      CSpan script = storedScript(info);
      return sigOpCount(script.Data, script.Data + script.Size, true);
    }
    default :
      return 0;
  }
}

bool Script::restoreScript(const CUnspentOutputInfo &info, xvector<uint8_t> &script)
{
  switch (info.Type) {
    case CUnspentOutputInfo::EPubKeyCompressed :
      assemble(script, {OP_PUSH_33}, info.PubKeyCompressed, 33, {OP_CHECKSIG});
      return true;
    case CUnspentOutputInfo::EPubKeyUncompressed :
    case CUnspentOutputInfo::EPubKeyHybrid :
      assemble(script, {OP_PUSH_65}, info.CustomData, 65, {OP_CHECKSIG});
      return true;
    case CUnspentOutputInfo::EPubKeyHash :
      assemble(script, {OP_DUP, OP_HASH160, OP_PUSH20}, info.PubKeyHash.begin(), 20, {OP_EQUALVERIFY, OP_CHECKSIG});
      return true;
    case CUnspentOutputInfo::EScriptHash :
      assemble(script, {OP_HASH160, OP_PUSH20}, info.ScriptHash.begin(), 20, {OP_EQUAL});
      return true;
    case CUnspentOutputInfo::EWitnessPubKeyHash :
      assemble(script, {OP_0, OP_PUSH20}, info.WitnessProgram, 20, {});
      return true;
    case CUnspentOutputInfo::EWitnessScriptHash :
      assemble(script, {OP_0, OP_PUSH32}, info.WitnessProgram, 32, {});
      return true;
    case CUnspentOutputInfo::EWitnessTaproot :
      assemble(script, {OP_1, OP_PUSH32}, info.WitnessProgram, 32, {});
      return true;
    case CUnspentOutputInfo::EAnchor :
      assemble(script, {OP_1, 0x02, 0x4E, 0x73}, nullptr, 0, {});
      return true;
    case CUnspentOutputInfo::EMultisig :
    case CUnspentOutputInfo::EWitnessUnknown :
    case CUnspentOutputInfo::ENonStandard : {
      CSpan stored = storedScript(info);
      assemble(script, {}, stored.Data, stored.Size, {});
      return true;
    }
    default :
      return false;
  }
}

bool Script::extractAddress(const BC::Proto::CTxOut &txOut, CAddress &address)
{
  const uint8_t *s = txOut.PkScript.data();
  const size_t size = txOut.PkScript.size();
  switch (classify(s, size)) {
    case CUnspentOutputInfo::EPubKeyCompressed : {
      // P2PK folds into the legacy address of the same key
      auto hash = sha256FollowRipemd160(s + 1, 33);
      address.set(CUnspentOutputInfo::EPubKeyHash, hash.begin(), hash.size());
      return true;
    }
    case CUnspentOutputInfo::EPubKeyUncompressed :
    case CUnspentOutputInfo::EPubKeyHybrid : {
      auto hash = sha256FollowRipemd160(s + 1, 65);
      address.set(CUnspentOutputInfo::EPubKeyHash, hash.begin(), hash.size());
      return true;
    }
    case CUnspentOutputInfo::EPubKeyHash :
      address.set(CUnspentOutputInfo::EPubKeyHash, s + 3, 20);
      return true;
    case CUnspentOutputInfo::EScriptHash :
      address.set(CUnspentOutputInfo::EScriptHash, s + 2, 20);
      return true;
    case CUnspentOutputInfo::EWitnessPubKeyHash :
      address.set(CUnspentOutputInfo::EWitnessPubKeyHash, s + 2, 20);
      return true;
    case CUnspentOutputInfo::EWitnessScriptHash :
      address.set(CUnspentOutputInfo::EWitnessScriptHash, s + 2, 32);
      return true;
    case CUnspentOutputInfo::EWitnessTaproot :
      address.set(CUnspentOutputInfo::EWitnessTaproot, s + 2, 32);
      return true;
    case CUnspentOutputInfo::EMultisig : {
      // Synthetic multisig identity: hash160 of the whole script
      auto hash = sha256FollowRipemd160(s, size);
      address.set(CUnspentOutputInfo::EMultisig, hash.begin(), hash.size());
      return true;
    }
    default :
      return false;
  }
}

bool Script::extractAddress(const CUnspentOutputInfo &info, CAddress &address)
{
  switch (info.Type) {
    case CUnspentOutputInfo::EPubKeyCompressed : {
      // P2PK folds into the legacy address of the same key
      auto hash = sha256FollowRipemd160(info.PubKeyCompressed, 33);
      address.set(CUnspentOutputInfo::EPubKeyHash, hash.begin(), hash.size());
      return true;
    }
    case CUnspentOutputInfo::EPubKeyUncompressed :
    case CUnspentOutputInfo::EPubKeyHybrid : {
      auto hash = sha256FollowRipemd160(info.CustomData, 65);
      address.set(CUnspentOutputInfo::EPubKeyHash, hash.begin(), hash.size());
      return true;
    }
    case CUnspentOutputInfo::EPubKeyHash :
      address.set(CUnspentOutputInfo::EPubKeyHash, info.PubKeyHash.begin(), info.PubKeyHash.size());
      return true;
    case CUnspentOutputInfo::EScriptHash :
      address.set(CUnspentOutputInfo::EScriptHash, info.ScriptHash.begin(), info.ScriptHash.size());
      return true;
    case CUnspentOutputInfo::EWitnessPubKeyHash :
      address.set(CUnspentOutputInfo::EWitnessPubKeyHash, info.WitnessProgram, 20);
      return true;
    case CUnspentOutputInfo::EWitnessScriptHash :
      address.set(CUnspentOutputInfo::EWitnessScriptHash, info.WitnessProgram, 32);
      return true;
    case CUnspentOutputInfo::EWitnessTaproot :
      address.set(CUnspentOutputInfo::EWitnessTaproot, info.WitnessProgram, 32);
      return true;
    case CUnspentOutputInfo::EMultisig : {
      // The same synthetic identity the receiving side takes from the output
      CSpan script = storedScript(info);
      auto hash = sha256FollowRipemd160(script.Data, script.Size);
      address.set(CUnspentOutputInfo::EMultisig, hash.begin(), hash.size());
      return true;
    }
    default :
      return false;
  }
}

std::string Script::addressToString(const CAddress &address,
                                    const std::vector<uint8_t> &pubkeyPrefix,
                                    const std::vector<uint8_t> &scriptPrefix,
                                    const std::string &bech32Prefix)
{
  switch (address.Type) {
    case CUnspentOutputInfo::EPubKeyHash :
      if (pubkeyPrefix.empty())
        return std::string();
      return encodeBase58WithCrc(pubkeyPrefix.data(), pubkeyPrefix.size(), address.Data, 20);
    case CUnspentOutputInfo::EScriptHash :
      if (scriptPrefix.empty())
        return std::string();
      return encodeBase58WithCrc(scriptPrefix.data(), scriptPrefix.size(), address.Data, 20);
    case CUnspentOutputInfo::EWitnessPubKeyHash :
      if (bech32Prefix.empty())
        return std::string();
      return Bech32::encodeSegwitAddress(bech32Prefix, 0, address.Data, 20);
    case CUnspentOutputInfo::EWitnessScriptHash :
      if (bech32Prefix.empty())
        return std::string();
      return Bech32::encodeSegwitAddress(bech32Prefix, 0, address.Data, 32);
    case CUnspentOutputInfo::EWitnessTaproot :
      if (bech32Prefix.empty())
        return std::string();
      return Bech32::encodeSegwitAddress(bech32Prefix, 1, address.Data, 32);
    case CUnspentOutputInfo::EMultisig : {
      // No standard human-readable form for bare multisig; "m-" + hash160 hex
      std::string result = "m-";
      result.append(bin2hexLowerCase(address.Data, 20));
      return result;
    }
    default :
      return std::string();
  }
}

bool Script::addressFromString(const std::string &hrAddress,
                               const std::vector<uint8_t> &pubkeyPrefix,
                               const std::vector<uint8_t> &scriptPrefix,
                               const std::string &bech32Prefix,
                               CAddress &address)
{
  if (hrAddress.empty())
    return false;

  // Base58check with the pubkey or script prefix
  {
    std::vector<uint8_t> data;
    if (DecodeBase58(hrAddress.c_str(), data) && data.size() > 24) {
      size_t prefixSize = data.size() - 24;
      uint32_t checksum;
      memcpy(&checksum, &data[data.size() - 4], 4);
      if (BTC::sha256dChecksum(data.data(), data.size() - 4) == checksum) {
        if (prefixSize == pubkeyPrefix.size() && memcmp(data.data(), pubkeyPrefix.data(), prefixSize) == 0) {
          address.set(CUnspentOutputInfo::EPubKeyHash, &data[prefixSize], 20);
          return true;
        }
        if (prefixSize == scriptPrefix.size() && memcmp(data.data(), scriptPrefix.data(), prefixSize) == 0) {
          address.set(CUnspentOutputInfo::EScriptHash, &data[prefixSize], 20);
          return true;
        }
      }
    }
  }

  // Segwit bech32/bech32m
  if (!bech32Prefix.empty()) {
    unsigned witnessVersion;
    std::vector<uint8_t> program;
    if (Bech32::decodeSegwitAddress(bech32Prefix, hrAddress, &witnessVersion, program)) {
      if (witnessVersion == 0 && program.size() == 20) {
        address.set(CUnspentOutputInfo::EWitnessPubKeyHash, program.data(), 20);
        return true;
      } else if (witnessVersion == 0 && program.size() == 32) {
        address.set(CUnspentOutputInfo::EWitnessScriptHash, program.data(), 32);
        return true;
      } else if (witnessVersion == 1 && program.size() == 32) {
        address.set(CUnspentOutputInfo::EWitnessTaproot, program.data(), 32);
        return true;
      }
      return false;
    }
  }

  // Synthetic multisig identity as rendered by addressToString
  if (hrAddress.size() == 42 && hrAddress[0] == 'm' && hrAddress[1] == '-') {
    BaseBlob<160> hash;
    for (char c: hrAddress.substr(2)) {
      if (!isxdigit(static_cast<unsigned char>(c)))
        return false;
    }
    hash.setHexRaw(hrAddress.c_str() + 2);
    address.set(CUnspentOutputInfo::EMultisig, hash.begin(), hash.size());
    return true;
  }

  return false;
}

void Script::parseTransactionOutput(const BC::Proto::CTxOut &out, xmstream &unspentOutputInfo)
{
  const uint8_t *script = out.PkScript.data();
  const size_t size = out.PkScript.size();

  // Records of many outputs are appended to one stream (the parsed output blob
  // of a block), so every seek inside this one is relative to where it started
  const size_t base = unspentOutputInfo.offsetOf();
  CUnspentOutputInfo *info = unspentOutputInfo.reserve<CUnspentOutputInfo>(1);
  // Whatever the type leaves unused (the rest of the union, the fields it does
  // not set) goes to disk with the record: zero it, or the same coin gets
  // different bytes in different runs
  memset(static_cast<void*>(info), 0, sizeof(CUnspentOutputInfo));
  info->Value = out.Value;
  info->Type = classify(script, size);

  // Writing past the union moves the buffer: info is not touched after
  switch (info->Type) {
    case CUnspentOutputInfo::EPubKeyCompressed :
      memcpy(info->PubKeyCompressed, script + 1, 33);
      break;
    case CUnspentOutputInfo::EPubKeyUncompressed :
    case CUnspentOutputInfo::EPubKeyHybrid :
      unspentOutputInfo.seekSet(base + CUnspentOutputInfo::customDataOffset());
      unspentOutputInfo.write(script + 1, 65);
      break;
    case CUnspentOutputInfo::EPubKeyHash :
      memcpy(info->PubKeyHash.begin(), script + 3, 20);
      break;
    case CUnspentOutputInfo::EScriptHash :
      memcpy(info->ScriptHash.begin(), script + 2, 20);
      break;
    case CUnspentOutputInfo::EWitnessPubKeyHash :
      memcpy(info->WitnessProgram, script + 2, 20);
      break;
    case CUnspentOutputInfo::EWitnessScriptHash :
    case CUnspentOutputInfo::EWitnessTaproot :
      memcpy(info->WitnessProgram, script + 2, 32);
      break;
    case CUnspentOutputInfo::EAnchor :
    case CUnspentOutputInfo::EOpReturn :
      break;
    default :
      // The script as it is: a spend is checked against it. The length goes first, or a short
      // script would not tell itself from the zero padding
      unspentOutputInfo.seekSet(base + CUnspentOutputInfo::customDataOffset());
      serializeVarSize(unspentOutputInfo, size);
      unspentOutputInfo.write(script, size);
      break;
  }

  // Custom data shorter than the union it replaces still leaves a whole record
  if (unspentOutputInfo.offsetOf() < base + sizeof(CUnspentOutputInfo))
    unspentOutputInfo.seekSet(base + sizeof(CUnspentOutputInfo));
}

}
