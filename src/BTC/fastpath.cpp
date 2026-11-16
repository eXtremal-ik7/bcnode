#include "fastpath.h"
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <string.h>

namespace BTC {

void keyHashScript(const uint8_t *hash, uint8_t (&script)[25])
{
  script[0] = Script::OP_DUP;
  script[1] = Script::OP_HASH160;
  script[2] = Script::OP_PUSH20;
  memcpy(script + 3, hash, 20);
  script[23] = Script::OP_EQUALVERIFY;
  script[24] = Script::OP_CHECKSIG;
}

EScriptError ecdsaChecksig(Script::CSpan sig,
                           Script::CSpan key,
                           Script::CSpan codePush,
                           ESigVersion version,
                           uint32_t flags,
                           CSigJob &job,
                           uint8_t &hashType)
{
  // FindAndDelete: Core drops the signature's push from the scriptCode, the digest here keeps it. A
  // key never parses as DER, r and s in the 20 bytes of a hash are out of reach
  if (version == ESigVersion::Base && (flags & EScriptConstScriptCode) && sig.Size != 0 && sig.Size == codePush.Size &&
      memcmp(sig.Data, codePush.Data, sig.Size) == 0)
    return EScriptError::SigFindAndDelete;

  EScriptError error = checkSignatureEncoding(sig, flags);
  if (error == EScriptError::Ok)
    error = checkPubKeyEncoding(key, flags, version);
  if (error != EScriptError::Ok)
    return error;

  // NULLFAIL refuses a failed non-empty signature at once, otherwise false is left on the stack
  job.Kind = ESigKind::Ecdsa;
  job.Error = (flags & EScriptNullFail) && sig.Size != 0 ? EScriptError::SigNullFail : EScriptError::EvalFalse;
  // Fails whatever the digest: no signature, a key of no size libsecp256k1 parses, no DER
  if (sig.Size == 0 || key.Size > sizeof(job.PubKey) || !ecdsaCompact({sig.Data, sig.Size - 1}, job.Signature))
    return job.Error;
  job.PubKeySize = static_cast<uint8_t>(key.Size);
  memcpy(job.PubKey, key.Data, key.Size);
  hashType = sig.Data[sig.Size - 1];
  return EScriptError::Ok;
}

bool verifySigJob(const CSigJob &job)
{
  if (job.Kind == ESigKind::Schnorr) {
    secp256k1_xonly_pubkey key;
    return secp256k1_xonly_pubkey_parse(secp256k1_context_static, &key, job.PubKey) &&
           secp256k1_schnorrsig_verify(secp256k1_context_static, job.Signature, job.Digest.begin(), job.Digest.size(), &key);
  }

  secp256k1_pubkey key;
  secp256k1_ecdsa_signature sig;
  return secp256k1_ec_pubkey_parse(secp256k1_context_static, &key, job.PubKey, job.PubKeySize) &&
         secp256k1_ecdsa_signature_parse_compact(secp256k1_context_static, &sig, job.Signature) &&
         secp256k1_ecdsa_verify(secp256k1_context_static, &sig, job.Digest.begin(), &key);
}

void CSigBatch::verify(std::vector<uint32_t> &failed) const
{
  for (size_t i = 0; i < Jobs_.size(); i++) {
    if (!verifySigJob(Jobs_[i]))
      failed.push_back(static_cast<uint32_t>(i));
  }
}

void spentDigests(std::span<const Script::CUnspentOutputInfo* const> spent, CTxDigests &digests)
{
  CSha256Writer amounts;
  CSha256Writer scripts;
  xvector<uint8_t> script;
  for (const Script::CUnspentOutputInfo *info: spent) {
    amounts.le64(static_cast<uint64_t>(info->Value));
    // An OP_RETURN record keeps no script, but no input can spend one
    if (!Script::restoreScript(*info, script))
      script.resize(0);
    scripts.script(script.data(), script.size());
  }
  digests.SpentAmounts = amounts.sha256();
  digests.SpentScripts = scripts.sha256();
}

const BaseBlob<256> &tapSighashTag()
{
  static const BaseBlob<256> tag = [] {
    BaseBlob<256> result;
    ::sha256("TapSighash", 10, result.begin());
    return result;
  }();
  return tag;
}

}
