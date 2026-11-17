#pragma once

#include "sigCheck.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// The script interpreter: the spends off the fast path's templates. Core's VerifyScript with its
// rules and errors - legacy scripts, P2SH, witness v0, taproot and tapscript. Signatures are checked
// at once, whatever the mode of the fast path

namespace BTC {

// What the scripts of an input see of the transaction around them
class CScriptInput {
public:
  // Unsigned, as BIP68 compares it
  uint32_t TxVersion = 0;
  uint32_t LockTime = 0;
  uint32_t Sequence = 0;

  // An ECDSA signature with its hash type byte. scriptCode is what the digest takes: for a legacy
  // script with the signatures and OP_CODESEPARATOR taken out
  virtual bool checkEcdsa(Script::CSpan signature, Script::CSpan key, Script::CSpan scriptCode, ESigVersion version) = 0;
  // A BIP340 signature of 64 or 65 bytes against an x-only key: Ok, or why it fails
  virtual EScriptError checkSchnorr(Script::CSpan signature, Script::CSpan key, const CTaprootSpend &spend) = 0;

protected:
  ~CScriptInput() = default;
};

// Core's VerifyScript: the script sig, the script of the output spent and the witness under the flags
EScriptError verifyScript(Script::CSpan scriptSig,
                          Script::CSpan scriptPubKey,
                          std::span<const xvector<uint8_t>> witness,
                          uint32_t flags,
                          CScriptInput &input);

// The input of a CTxChecker: digests from the transaction's shared ones, signatures by libsecp256k1
template<typename TxTy>
class CTxScriptInput final : public CScriptInput {
public:
  CTxScriptInput(CTxChecker<TxTy> &checker, size_t input) : Checker_(checker), Input_(input) {
    const TxTy &tx = checker.tx();
    TxVersion = static_cast<uint32_t>(tx.Version);
    LockTime = tx.LockTime;
    Sequence = tx.TxIn[input].Sequence;
  }

  bool checkEcdsa(Script::CSpan signature, Script::CSpan key, Script::CSpan scriptCode, ESigVersion version) override {
    CSigJob job;
    uint8_t hashType;
    if (!ecdsaJob(signature, key, job, hashType))
      return false;
    const TxTy &tx = Checker_.tx();
    job.Digest = version == ESigVersion::Base
      ? sighashLegacy(tx, Input_, scriptCode.Data, scriptCode.Size, hashType)
      : sighashWitnessV0(tx, Input_, scriptCode.Data, scriptCode.Size, Checker_.spent(Input_).Value, hashType, Checker_.digests());
    return verifySigJob(job);
  }

  EScriptError checkSchnorr(Script::CSpan signature, Script::CSpan key, const CTaprootSpend &spend) override {
    const TxTy &tx = Checker_.tx();
    uint8_t hashType;
    if (EScriptError error = checkSchnorrSignature(signature, tx.TxOut.size() > Input_, hashType); error != EScriptError::Ok)
      return error;
    CSigJob job;
    job.Kind = ESigKind::Schnorr;
    job.PubKeySize = 32;
    memcpy(job.PubKey, key.Data, 32);
    memcpy(job.Signature, signature.Data, 64);
    job.Digest = sighashTaproot(tx, Input_, hashType, Checker_.taprootDigests(), Checker_.spent(Input_), spend);
    return verifySigJob(job) ? EScriptError::Ok : EScriptError::SchnorrSig;
  }

private:
  CTxChecker<TxTy> &Checker_;
  size_t Input_;
};

// One input under the flags, Ok when the spend is valid
template<typename TxTy>
EScriptError interpretInput(CTxChecker<TxTy> &checker, size_t input, uint32_t flags)
{
  const auto &in = checker.tx().TxIn[input];
  // A record of OP_RETURN keeps no script, and no utxo is one: its first opcode is all that runs
  static constexpr uint8_t opReturn[] = {Script::OP_RETURN};
  xvector<uint8_t> restored;
  const Script::CSpan scriptPubKey = Script::restoreScript(checker.spent(input), restored) ? Script::CSpan{restored.data(), restored.size()}
                                                                                          : Script::CSpan{opReturn, 1};

  CTxScriptInput<TxTy> scriptInput(checker, input);
  return verifyScript({in.ScriptSig.data(), in.ScriptSig.size()},
                      scriptPubKey,
                      {in.WitnessStack.data(), in.WitnessStack.size()},
                      flags,
                      scriptInput);
}

}
