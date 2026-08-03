#include "script.h"
#include "hash.h"
#include "common/base58.h"
#include "common/bech32.h"
#include "common/utils.h"
#include <ctype.h>

namespace {

// OP_m <push pubkey>{n} OP_n OP_CHECKMULTISIG; 33/65-byte keys, 1 <= m <= n <= 16
bool isBareMultisig(const uint8_t *script, size_t size)
{
  if (size < 37 || script[size - 1] != BTC::Script::OP_CHECKMULTISIG)
    return false;

  uint8_t m = script[0];
  uint8_t n = script[size - 2];
  if (m < BTC::Script::OP_1 || m > BTC::Script::OP_16 ||
      n < BTC::Script::OP_1 || n > BTC::Script::OP_16 ||
      m > n)
    return false;

  size_t keysNum = 0;
  size_t offset = 1;
  while (offset < size - 2) {
    uint8_t push = script[offset];
    if (push != 33 && push != 65)
      return false;
    offset += 1 + push;
    keysNum++;
  }

  return offset == size - 2 && keysNum == static_cast<size_t>(n - BTC::Script::OP_1 + 1);
}

}

namespace BTC {

bool Script::extractAddress(const BC::Proto::TxOut &txOut, CAddress &address)
{
  const uint8_t *scriptData = txOut.pkScript.data();
  size_t scriptSize = txOut.pkScript.size();

  if (scriptSize == 25 &&
      scriptData[0] == OP_DUP &&
      scriptData[1] == OP_HASH160 &&
      scriptData[2] == OP_PUSH20 &&
      scriptData[23] == OP_EQUALVERIFY &&
      scriptData[24] == OP_CHECKSIG) {
    // P2PKH
    // OP_DUP OP_HASH160 OP_PUSH20(Address) OP_EQUALVERIFY OP_CHECKSIG
    address.set(UnspentOutputInfo::EPubKeyHash, scriptData + 3, 20);
    return true;
  } else if (scriptSize == 23 &&
             scriptData[0] == OP_HASH160 &&
             scriptData[1] == OP_PUSH20 &&
             scriptData[22] == OP_EQUAL) {
    // P2SH
    // OP_HASH160 OP_PUSH20(RedeemScriptHash) OP_EQUAL
    address.set(UnspentOutputInfo::EScriptHash, scriptData + 2, 20);
    return true;
  } else if (scriptSize == 22 && scriptData[0] == OP_0 && scriptData[1] == OP_PUSH20) {
    // P2WPKH
    // OP_0 OP_PUSH20(PubKeyHash)
    address.set(UnspentOutputInfo::EWitnessPubKeyHash, scriptData + 2, 20);
    return true;
  } else if (scriptSize == 34 && scriptData[0] == OP_0 && scriptData[1] == OP_PUSH32) {
    // P2WSH
    // OP_0 OP_PUSH32(WitnessScriptHash)
    address.set(UnspentOutputInfo::EWitnessScriptHash, scriptData + 2, 32);
    return true;
  } else if (scriptSize == 34 && scriptData[0] == OP_1 && scriptData[1] == OP_PUSH32) {
    // P2TR
    // OP_1 OP_PUSH32(XOnlyPubKey)
    address.set(UnspentOutputInfo::EWitnessTaproot, scriptData + 2, 32);
    return true;
  } else if (scriptSize == 35 && scriptData[0] == OP_PUSH_33 && scriptData[34] == OP_CHECKSIG) {
    // P2PK compressed
    // PUSH_33(PublicKey) OP_CHECKSIG
    auto hash = sha256FollowRipemd160(scriptData + 1, 33);
    address.set(UnspentOutputInfo::EPubKeyHash, hash.begin(), hash.size());
    return true;
  } else if (scriptSize == 67 && scriptData[0] == OP_PUSH_65 && scriptData[66] == OP_CHECKSIG) {
    // P2PK uncompressed
    // PUSH_65(PublicKey) OP_CHECKSIG
    auto hash = sha256FollowRipemd160(scriptData + 1, 65);
    address.set(UnspentOutputInfo::EPubKeyHash, hash.begin(), hash.size());
    return true;
  } else if (isBareMultisig(scriptData, scriptSize)) {
    // Synthetic multisig identity: hash160 of the whole script
    auto hash = sha256FollowRipemd160(scriptData, scriptSize);
    address.set(UnspentOutputInfo::EMultisig, hash.begin(), hash.size());
    return true;
  }

  return false;
}

bool Script::extractAddress(const UnspentOutputInfo &info, CAddress &address)
{
  switch (info.Type) {
    case UnspentOutputInfo::EPubKey : {
      // P2PK folds into the legacy address of the same key
      auto hash = info.IsPubKeyCompressed ?
        sha256FollowRipemd160(info.PubKeyCompressed, 33) :
        sha256FollowRipemd160(info.CustomData, 65);
      address.set(UnspentOutputInfo::EPubKeyHash, hash.begin(), hash.size());
      return true;
    }
    case UnspentOutputInfo::EPubKeyHash :
      address.set(UnspentOutputInfo::EPubKeyHash, info.PubKeyHash.begin(), info.PubKeyHash.size());
      return true;
    case UnspentOutputInfo::EScriptHash :
      address.set(UnspentOutputInfo::EScriptHash, info.ScriptHash.begin(), info.ScriptHash.size());
      return true;
    case UnspentOutputInfo::EWitnessPubKeyHash :
      address.set(UnspentOutputInfo::EWitnessPubKeyHash, info.WitnessProgram, 20);
      return true;
    case UnspentOutputInfo::EWitnessScriptHash :
      address.set(UnspentOutputInfo::EWitnessScriptHash, info.WitnessProgram, 32);
      return true;
    case UnspentOutputInfo::EWitnessTaproot :
      address.set(UnspentOutputInfo::EWitnessTaproot, info.WitnessProgram, 32);
      return true;
    case UnspentOutputInfo::EMultisig :
      address.set(UnspentOutputInfo::EMultisig, info.ScriptHash.begin(), info.ScriptHash.size());
      return true;
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
    case UnspentOutputInfo::EPubKeyHash :
      if (pubkeyPrefix.empty())
        return std::string();
      return encodeBase58WithCrc(pubkeyPrefix.data(), pubkeyPrefix.size(), address.Data, 20);
    case UnspentOutputInfo::EScriptHash :
      if (scriptPrefix.empty())
        return std::string();
      return encodeBase58WithCrc(scriptPrefix.data(), scriptPrefix.size(), address.Data, 20);
    case UnspentOutputInfo::EWitnessPubKeyHash :
      if (bech32Prefix.empty())
        return std::string();
      return Bech32::encodeSegwitAddress(bech32Prefix, 0, address.Data, 20);
    case UnspentOutputInfo::EWitnessScriptHash :
      if (bech32Prefix.empty())
        return std::string();
      return Bech32::encodeSegwitAddress(bech32Prefix, 0, address.Data, 32);
    case UnspentOutputInfo::EWitnessTaproot :
      if (bech32Prefix.empty())
        return std::string();
      return Bech32::encodeSegwitAddress(bech32Prefix, 1, address.Data, 32);
    case UnspentOutputInfo::EMultisig : {
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
          address.set(UnspentOutputInfo::EPubKeyHash, &data[prefixSize], 20);
          return true;
        }
        if (prefixSize == scriptPrefix.size() && memcmp(data.data(), scriptPrefix.data(), prefixSize) == 0) {
          address.set(UnspentOutputInfo::EScriptHash, &data[prefixSize], 20);
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
        address.set(UnspentOutputInfo::EWitnessPubKeyHash, program.data(), 20);
        return true;
      } else if (witnessVersion == 0 && program.size() == 32) {
        address.set(UnspentOutputInfo::EWitnessScriptHash, program.data(), 32);
        return true;
      } else if (witnessVersion == 1 && program.size() == 32) {
        address.set(UnspentOutputInfo::EWitnessTaproot, program.data(), 32);
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
    address.set(UnspentOutputInfo::EMultisig, hash.begin(), hash.size());
    return true;
  }

  return false;
}

void Script::parseTransactionOutput(const BC::Proto::TxOut &out, xmstream &unspentOutputInfo)
{
  const uint8_t *script = out.pkScript.data();

  // Records of many outputs are appended to one stream (the parsed output blob
  // of a block), so every seek inside this one is relative to where it started
  const size_t base = unspentOutputInfo.offsetOf();
  UnspentOutputInfo *info = unspentOutputInfo.reserve<UnspentOutputInfo>(1);
  // Whatever the type leaves unused (the rest of the union, the fields it does
  // not set) goes to disk with the record: zero it, or the same coin gets
  // different bytes in different runs
  memset(static_cast<void*>(info), 0, sizeof(UnspentOutputInfo));
  info->Value = out.value;

  if (out.pkScript.size() >= 1 && script[0] == OP_RETURN) {
    info->Type = UnspentOutputInfo::EOpReturn;
  } else if (out.pkScript.size() == 35 && script[0] == OP_PUSH_33 && script[34] == OP_CHECKSIG) {
    // P2PK compressed
    // PUSH_33(PublicKey) OP_CHECKSIG
    info->Type = UnspentOutputInfo::EPubKey;
    info->IsPubKeyCompressed = true;
    memcpy(info->PubKeyCompressed, script+1, 33);
  } else if (out.pkScript.size() == 67 && script[0] == OP_PUSH_65 && script[66] == OP_CHECKSIG) {
    // P2PK uncompressed
    // PUSH_65(PublicKey) OP_CHECKSIG
    info->Type = UnspentOutputInfo::EPubKey;
    info->IsPubKeyCompressed = false;
    unspentOutputInfo.seekSet(base + UnspentOutputInfo::customDataOffset());
    unspentOutputInfo.write(script+1, 65);
  } else if (out.pkScript.size() == 25 &&
             script[0] == OP_DUP &&
             script[1] == OP_HASH160 &&
             script[2] == OP_PUSH20 &&
             script[23] == OP_EQUALVERIFY &&
             script[24] == OP_CHECKSIG) {
    // P2PKH
    // OP_DUP OP_HASH160 OP_PUSH20(Address) OP_EQUALVERIFY OP_CHECKSIG
    info->Type = UnspentOutputInfo::EPubKeyHash;
    memcpy(info->PubKeyHash.begin(), script+3, 20);
  } else if (out.pkScript.size() == 23 &&
             script[0] == OP_HASH160 &&
             script[1] == OP_PUSH20 &&
             script[22] == OP_EQUAL) {
    // P2SH
    // OP_HASH160 OP_PUSH20(RedeemScriptHash) OP_EQUAL
    info->Type = UnspentOutputInfo::EScriptHash;
    memcpy(info->ScriptHash.begin(), script+2, 20);
  } else if (out.pkScript.size() == 22 && script[0] == OP_0 && script[1] == OP_PUSH20) {
    // P2WPKH
    // OP_0 OP_PUSH20(PubKeyHash)
    info->Type = UnspentOutputInfo::EWitnessPubKeyHash;
    memcpy(info->WitnessProgram, script+2, 20);
  } else if (out.pkScript.size() == 34 && script[0] == OP_0 && script[1] == OP_PUSH32) {
    // P2WSH
    // OP_0 OP_PUSH32(WitnessScriptHash)
    info->Type = UnspentOutputInfo::EWitnessScriptHash;
    memcpy(info->WitnessProgram, script+2, 32);
  } else if (out.pkScript.size() == 34 && script[0] == OP_1 && script[1] == OP_PUSH32) {
    // P2TR
    // OP_1 OP_PUSH32(XOnlyPubKey)
    info->Type = UnspentOutputInfo::EWitnessTaproot;
    memcpy(info->WitnessProgram, script+2, 32);
  } else if (isBareMultisig(script, out.pkScript.size())) {
    // The spend path needs only the synthetic identity, not the keys
    info->Type = UnspentOutputInfo::EMultisig;
    info->ScriptHash = sha256FollowRipemd160(script, out.pkScript.size());
  } else {
    info->Type = UnspentOutputInfo::ENonStandard;
    unspentOutputInfo.seekSet(base + UnspentOutputInfo::customDataOffset());
    unspentOutputInfo.write(script, out.pkScript.size());
  }

  // Custom data shorter than the union it replaces still leaves a whole record
  if (unspentOutputInfo.offsetOf() < base + sizeof(UnspentOutputInfo))
    unspentOutputInfo.seekSet(base + sizeof(UnspentOutputInfo));
}

}
