#include "script.h"
#include "openssl/ripemd.h"
#include "openssl/sha.h"

namespace BTC {

bool Script::decodeStandardOutput(const BC::Proto::TxOut &out, BC::Proto::AddressTy &address)
{
  if (out.pkScript.size() == 35 || out.pkScript.size() == 67) {
    // Pay to public key
    uint8_t sha256[32];
    {
      SHA256_CTX ctx;
      SHA256_Init(&ctx);
      SHA256_Update(&ctx, out.pkScript.data() + 1, out.pkScript.size() - 2);
      SHA256_Final(sha256, &ctx);
    }

    {
      RIPEMD160_CTX ctx;
      RIPEMD160_Init(&ctx);
      RIPEMD160_Update(&ctx, sha256, sizeof(sha256));
      RIPEMD160_Final(address.begin(), &ctx);
    }

    return true;
  }


  if (out.pkScript.size() == sizeof(BC::Proto::AddressTy) + 5) {
    // Pay to public key hash
    if (out.pkScript[0] == OP_DUP &&
        out.pkScript[1] == OP_HASH160 &&
        out.pkScript[2] == sizeof(BC::Proto::AddressTy) &&
        out.pkScript[sizeof(BC::Proto::AddressTy) + 3] == OP_EQUALVERIFY &&
        out.pkScript[sizeof(BC::Proto::AddressTy) + 4] == OP_CHECKSIG) {
      memcpy(address.begin(), &out.pkScript[3], sizeof(BC::Proto::AddressTy));
      return true;
    }
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
  if (out.pkScript.size() == 35 && script[0] == OP_PUSH_33 && script[34] == OP_CHECKSIG) {
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
