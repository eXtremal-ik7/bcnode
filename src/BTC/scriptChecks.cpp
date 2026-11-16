#include "scriptChecks.h"
#include "sighash.h"
#include <secp256k1.h>
#include <string.h>

namespace BTC {

namespace {

// Core's ecdsa_signature_parse_der_lax (pubkey.cpp): what any DER-like signature parses to, an
// overflowing r or s to a signature no key verifies
bool parseDerLax(secp256k1_ecdsa_signature &sig, const uint8_t *input, size_t size)
{
  size_t pos = 0;
  uint8_t compact[64] = {};
  secp256k1_ecdsa_signature_parse_compact(secp256k1_context_static, &sig, compact);

  auto integer = [&](size_t &start, size_t &length) -> bool {
    if (pos == size || input[pos] != 0x02)
      return false;
    pos++;
    if (pos == size)
      return false;
    size_t lenByte = input[pos++];
    if (lenByte & 0x80) {
      lenByte -= 0x80;
      if (lenByte > size - pos)
        return false;
      while (lenByte > 0 && input[pos] == 0) {
        pos++;
        lenByte--;
      }
      if (lenByte >= 4)
        return false;
      length = 0;
      while (lenByte > 0) {
        length = (length << 8) + input[pos];
        pos++;
        lenByte--;
      }
    } else {
      length = lenByte;
    }
    if (length > size - pos)
      return false;
    start = pos;
    pos += length;
    return true;
  };

  if (pos == size || input[pos] != 0x30)
    return false;
  pos++;
  if (pos == size)
    return false;
  size_t lenByte = input[pos++];
  if (lenByte & 0x80) {
    lenByte -= 0x80;
    if (lenByte > size - pos)
      return false;
    pos += lenByte;
  }

  size_t rPos, rLen, sPos, sLen;
  if (!integer(rPos, rLen) || !integer(sPos, sLen))
    return false;

  bool overflow = false;
  while (rLen > 0 && input[rPos] == 0) {
    rLen--;
    rPos++;
  }
  if (rLen > 32)
    overflow = true;
  else
    memcpy(compact + 32 - rLen, input + rPos, rLen);
  while (sLen > 0 && input[sPos] == 0) {
    sLen--;
    sPos++;
  }
  if (sLen > 32)
    overflow = true;
  else
    memcpy(compact + 64 - sLen, input + sPos, sLen);

  if (!overflow)
    overflow = !secp256k1_ecdsa_signature_parse_compact(secp256k1_context_static, &sig, compact);
  if (overflow) {
    memset(compact, 0, sizeof(compact));
    secp256k1_ecdsa_signature_parse_compact(secp256k1_context_static, &sig, compact);
  }
  return true;
}

// Core's IsValidSignatureEncoding (BIP66): strict DER, the hash type byte last
bool isValidSignatureEncoding(const uint8_t *sig, size_t size)
{
  if (size < 9 || size > 73)
    return false;
  if (sig[0] != 0x30 || sig[1] != size - 3)
    return false;
  const size_t lenR = sig[3];
  if (5 + lenR >= size)
    return false;
  const size_t lenS = sig[5 + lenR];
  if (lenR + lenS + 7 != size)
    return false;
  if (sig[2] != 0x02 || lenR == 0 || (sig[4] & 0x80))
    return false;
  if (lenR > 1 && sig[4] == 0x00 && !(sig[5] & 0x80))
    return false;
  if (sig[lenR + 4] != 0x02 || lenS == 0 || (sig[lenR + 6] & 0x80))
    return false;
  if (lenS > 1 && sig[lenR + 6] == 0x00 && !(sig[lenR + 7] & 0x80))
    return false;
  return true;
}

// Core's CPubKey::CheckLowS, the hash type byte dropped
bool isLowS(const uint8_t *sig, size_t size)
{
  secp256k1_ecdsa_signature parsed;
  return parseDerLax(parsed, sig, size - 1) && !secp256k1_ecdsa_signature_normalize(secp256k1_context_static, nullptr, &parsed);
}

bool isDefinedHashType(uint8_t hashType)
{
  const uint8_t base = hashType & ~ESighashAnyoneCanPay;
  return base >= ESighashAll && base <= ESighashSingle;
}

}

bool castToBool(const uint8_t *data, size_t size)
{
  for (size_t i = 0; i < size; i++) {
    if (data[i] != 0)
      return !(i == size - 1 && data[i] == 0x80);
  }
  return false;
}

bool checkMinimalPush(Script::CSpan data, uint8_t opcode)
{
  if (data.Size == 0)
    return opcode == Script::OP_0;
  if (data.Size == 1 && data.Data[0] >= 1 && data.Data[0] <= 16)
    return false;
  if (data.Size == 1 && data.Data[0] == 0x81)
    return false;
  if (data.Size <= 75)
    return opcode == data.Size;
  if (data.Size <= 255)
    return opcode == Script::OP_PUSHDATA1;
  if (data.Size <= 65535)
    return opcode == Script::OP_PUSHDATA2;
  return true;
}

EScriptError checkSignatureEncoding(Script::CSpan signature, uint32_t flags)
{
  if (signature.Size == 0)
    return EScriptError::Ok;
  if ((flags & (EScriptDerSig | EScriptLowS | EScriptStrictEnc)) && !isValidSignatureEncoding(signature.Data, signature.Size))
    return EScriptError::SigDer;
  if ((flags & EScriptLowS) && !isLowS(signature.Data, signature.Size))
    return EScriptError::SigHighS;
  if ((flags & EScriptStrictEnc) && !isDefinedHashType(signature.Data[signature.Size - 1]))
    return EScriptError::SigHashtype;
  return EScriptError::Ok;
}

EScriptError checkPubKeyEncoding(Script::CSpan key, uint32_t flags, ESigVersion version)
{
  const bool compressed = key.Size == 33 && (key.Data[0] == 0x02 || key.Data[0] == 0x03);
  const bool uncompressed = key.Size == 65 && key.Data[0] == 0x04;
  if ((flags & EScriptStrictEnc) && !compressed && !uncompressed)
    return EScriptError::PubKeyType;
  if ((flags & EScriptWitnessPubKeyType) && version == ESigVersion::WitnessV0 && !compressed)
    return EScriptError::WitnessPubKeyType;
  return EScriptError::Ok;
}

bool ecdsaCompact(Script::CSpan der, uint8_t (&compact)[64])
{
  secp256k1_ecdsa_signature sig;
  if (!parseDerLax(sig, der.Data, der.Size))
    return false;
  secp256k1_ecdsa_signature_normalize(secp256k1_context_static, &sig, &sig);
  secp256k1_ecdsa_signature_serialize_compact(secp256k1_context_static, compact, &sig);
  return true;
}

EScriptError checkSchnorrSignature(Script::CSpan signature, bool hasOutput, uint8_t &hashType)
{
  if (signature.Size != 64 && signature.Size != 65)
    return EScriptError::SchnorrSigSize;
  hashType = ESighashDefault;
  if (signature.Size == 65) {
    hashType = signature.Data[64];
    if (hashType == ESighashDefault)
      return EScriptError::SchnorrSigHashtype;
  }
  // SignatureHashSchnorr's own refusals
  if (!(hashType <= ESighashSingle || (hashType >= (ESighashAnyoneCanPay | ESighashAll) && hashType <= (ESighashAnyoneCanPay | ESighashSingle))))
    return EScriptError::SchnorrSigHashtype;
  if ((hashType & 0x03) == ESighashSingle && !hasOutput)
    return EScriptError::SchnorrSigHashtype;
  return EScriptError::Ok;
}

}
