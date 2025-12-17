#include "script.h"
#include <openssl/evp.h>

namespace BTC {

std::string Script::addressToBase58(UnspentOutputInfo::EType type,
                                    BC::Proto::AddressTy &address,
                                    const std::vector<uint8_t> &pubkeyPrefix,
                                    const std::vector<uint8_t> &scriptPrefix)
{
  if (type == Script::UnspentOutputInfo::EPubKey ||
      type == Script::UnspentOutputInfo::EPubKeyHash) {
    return makeHumanReadableAddress(pubkeyPrefix[0], address);
  } else if (type == Script::UnspentOutputInfo::EScriptHash) {
    return makeHumanReadableAddress(scriptPrefix[0], address);
  } else {
    return std::string();
  }
}

Script::UnspentOutputInfo::EType Script::extractSingleAddress(const BC::Proto::TxOut &txOut, BC::Proto::AddressTy &address)
{
  const uint8_t *scriptData = txOut.pkScript.data();
  size_t scriptSize = txOut.pkScript.size();
  if (scriptSize == 35 && scriptData[0] == OP_PUSH_33 && scriptData[34] == OP_CHECKSIG) {
    // P2PK compressed
    // PUSH_33(PublicKey) OP_CHECKSIG
    uint8_t sha256[32];
    {
      CCtxSha256 ctx;
      sha256Init(&ctx);
      sha256Update(&ctx, scriptData + 1, scriptSize - 2);
      sha256Final(&ctx, sha256);
    }

    {
      unsigned outSize = 0;
      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      EVP_DigestInit_ex(ctx, EVP_ripemd160(), nullptr);
      EVP_DigestUpdate(ctx, sha256, sizeof(sha256));
      EVP_DigestFinal_ex(ctx, address.begin(), &outSize);
      EVP_MD_CTX_free(ctx);
    }

    return Script::UnspentOutputInfo::EPubKey;
  } else if (scriptSize == 67 && scriptData[0] == OP_PUSH_65 && scriptData[66] == OP_CHECKSIG) {
    // P2PK uncompressed
    // PUSH_65(PublicKey) OP_CHECKSIG
    uint8_t sha256[32];
    {
      CCtxSha256 ctx;
      sha256Init(&ctx);
      sha256Update(&ctx, scriptData + 1, scriptSize - 2);
      sha256Final(&ctx, sha256);
    }

    {
      unsigned outSize = 0;
      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      EVP_DigestInit_ex(ctx, EVP_ripemd160(), nullptr);
      EVP_DigestUpdate(ctx, sha256, sizeof(sha256));
      EVP_DigestFinal_ex(ctx, address.begin(), &outSize);
      EVP_MD_CTX_free(ctx);
    }

    return Script::UnspentOutputInfo::EPubKey;
  } else if (scriptSize == 25 &&
             scriptData[0] == OP_DUP &&
             scriptData[1] == OP_HASH160 &&
             scriptData[2] == OP_PUSH20 &&
             scriptData[23] == OP_EQUALVERIFY &&
             scriptData[24] == OP_CHECKSIG) {
    // P2PKH
    // OP_DUP OP_HASH160 OP_PUSH20(Address) OP_EQUALVERIFY OP_CHECKSIG
    memcpy(address.begin(), scriptData+3, 20);
    return Script::UnspentOutputInfo::EPubKeyHash;
  } else if (scriptSize == 23 &&
             scriptData[0] == OP_HASH160 &&
             scriptData[1] == OP_PUSH20 &&
             scriptData[22] == OP_EQUAL) {
    // P2SH
    // OP_HASH160 OP_PUSH20(RedeemScriptHash) OP_EQUAL
    memcpy(address.begin(), scriptData+2, 20);
    return Script::UnspentOutputInfo::EScriptHash;
  }

  return Script::UnspentOutputInfo::EInvalid;
}

bool Script::extractSingleAddress(const Script::UnspentOutputInfo &info, BC::Proto::AddressTy &address)
{
  if (info.Type == Script::UnspentOutputInfo::EPubKey) {
    if (info.IsPubKeyCompressed) {
      uint8_t sha256[32];
      {
        CCtxSha256 ctx;
        sha256Init(&ctx);
        sha256Update(&ctx, info.PubKeyCompressed, 33);
        sha256Final(&ctx, sha256);
      }

      {
        unsigned outSize = 0;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_ripemd160(), nullptr);
        EVP_DigestUpdate(ctx, sha256, sizeof(sha256));
        EVP_DigestFinal_ex(ctx, address.begin(), &outSize);
        EVP_MD_CTX_free(ctx);
      }
    } else {
      uint8_t sha256[32];
      {
        CCtxSha256 ctx;
        sha256Init(&ctx);
        sha256Update(&ctx, info.CustomData, 67);
        sha256Final(&ctx, sha256);
      }

      {
        unsigned outSize = 0;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_ripemd160(), nullptr);
        EVP_DigestUpdate(ctx, sha256, sizeof(sha256));
        EVP_DigestFinal_ex(ctx, address.begin(), &outSize);
        EVP_MD_CTX_free(ctx);
      }
    }
    return true;
  } else if (info.Type == Script::UnspentOutputInfo::EPubKeyHash) {
    memcpy(address.begin(), info.PubKeyHash.begin(), sizeof(address));
    return true;
  } else if (info.Type == Script::UnspentOutputInfo::EScriptHash) {
    memcpy(address.begin(), info.ScriptHash.begin(), sizeof(address));
    return true;
  }

  return false;
}

void Script::parseTransactionOutput(const BC::Proto::TxOut &out, xmstream &unspentOutputInfo)
{
  // TODO:
  //  Witness
  //  Taproot
  const uint8_t *script = out.pkScript.data();

  UnspentOutputInfo *info = unspentOutputInfo.reserve<UnspentOutputInfo>(1);
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
    unspentOutputInfo.seekSet(UnspentOutputInfo::customDataOffset());
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
  } else {
    info->Type = UnspentOutputInfo::ENonStandard;
    unspentOutputInfo.seekSet(UnspentOutputInfo::customDataOffset());
    unspentOutputInfo.write(script, out.pkScript.size());
  }
}

}
