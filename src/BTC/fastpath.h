#pragma once

#include "script.h"
#include "scriptChecks.h"
#include "sigCheck.h"
#include "sighash.h"
#include <optional>
#include <string.h>

// Spends checked without the interpreter: a template of the spent
// output and the spend fixes what runs, the fast path runs it with Core's rules and errors

namespace BTC {

// One input of a template under the flags; nothing when the spend is off the templates. The
// record's type and the shape of the script sig pick the template, the rest of it is checked as it
// goes. batch takes the jobs of Defer
template<typename TxTy>
std::optional<CInputVerdict> fastPathInput(CTxChecker<TxTy> &checker, size_t input, uint32_t flags, ESigMode mode, CSigBatch *batch)
{
  using Info = Script::CUnspentOutputInfo;
  const TxTy &tx = checker.tx();
  const auto &in = tx.TxIn[input];
  const Info &spent = checker.spent(input);
  auto valid = []() { return CInputVerdict{EVerdict::Valid, EScriptError::Ok}; };
  auto invalid = [](EScriptError error) { return CInputVerdict{EVerdict::Invalid, error}; };
  auto hash160Is = [](Script::CSpan data, const uint8_t *hash) {
    return memcmp(sha256FollowRipemd160(data.Data, data.Size).begin(), hash, 20) == 0;
  };

  // Off the template: the interpreter's, unless a push already failed
  EScriptError pushError = EScriptError::Ok;
  auto offTemplate = [&]() -> std::optional<CInputVerdict> {
    if (pushError != EScriptError::Ok)
      return invalid(pushError);
    return std::nullopt;
  };
  // The data of the script sig's next push, an opcode OP_0 to 75; false on anything else. Core runs
  // the script sig push by push: under MINIMALDATA a byte OP_1..OP_16 or OP_1NEGATE pushes is its
  // error at that very push, whatever follows
  const uint8_t *p = in.ScriptSig.data();
  const uint8_t *end = p + in.ScriptSig.size();
  auto readPush = [&](Script::CSpan &data) {
    if (p == end || *p > Script::OP_PUSH_75 || static_cast<size_t>(end - p - 1) < *p)
      return false;
    data = {p + 1, *p};
    if ((flags & EScriptMinimalData) && !checkMinimalPush(data, *p)) {
      pushError = EScriptError::MinimalData;
      return false;
    }
    p += 1 + *p;
    return true;
  };

  CSigJob job;
  uint8_t hashType = 0;

  switch (spent.Type) {
    case Info::EPubKeyCompressed :
    case Info::EPubKeyUncompressed :
    case Info::EPubKeyHybrid : {
      // <sig> | <key> CHECKSIG
      // A witness: Core names it once the script succeeds, the spend fails all the same; the mempool
      // refuses it ahead of scripts (bad-witness-nonstandard)
      if ((flags & EScriptWitness) && !in.WitnessStack.empty())
        return invalid(EScriptError::WitnessUnexpected);
      Script::CSpan sig;
      if (!readPush(sig) || p != end)
        return offTemplate();
      const Script::CSpan key = spent.Type == Info::EPubKeyCompressed ? Script::CSpan{spent.PubKeyCompressed, 33}
                                                                      : Script::CSpan{spent.CustomData, 65};
      if (auto error = ecdsaChecksig(sig, key, key, ESigVersion::Base, flags, job, hashType); error != EScriptError::Ok)
        return invalid(error);
      if (mode != ESigMode::Trust) {
        uint8_t script[67];
        script[0] = static_cast<uint8_t>(key.Size);
        memcpy(script + 1, key.Data, key.Size);
        script[1 + key.Size] = Script::OP_CHECKSIG;
        job.Digest = sighashLegacy(tx, input, script, key.Size + 2, hashType);
      }
      break;
    }

    case Info::EPubKeyHash : {
      // <sig> <key> | DUP HASH160 <hash> EQUALVERIFY CHECKSIG; a witness as for P2PK
      if ((flags & EScriptWitness) && !in.WitnessStack.empty())
        return invalid(EScriptError::WitnessUnexpected);
      Script::CSpan sig, key;
      if (!readPush(sig) || !readPush(key) || p != end)
        return offTemplate();
      const uint8_t *hash = spent.PubKeyHash.begin();
      if (!hash160Is(key, hash))
        return invalid(EScriptError::EqualVerify);
      if (auto error = ecdsaChecksig(sig, key, {hash, 20}, ESigVersion::Base, flags, job, hashType); error != EScriptError::Ok)
        return invalid(error);
      if (mode != ESigMode::Trust) {
        uint8_t script[25];
        keyHashScript(hash, script);
        job.Digest = sighashLegacy(tx, input, script, sizeof(script), hashType);
      }
      break;
    }

    case Info::EWitnessPubKeyHash :
    case Info::EScriptHash : {
      const uint8_t *program = spent.WitnessProgram;
      if (spent.Type == Info::EScriptHash) {
        // <redeem> | HASH160 <hash> EQUAL, the simple transfer: the redeem script of P2WPKH alone
        Script::CSpan redeem;
        if (!readPush(redeem) || p != end || redeem.Size != 22 || redeem.Data[0] != Script::OP_0 ||
            redeem.Data[1] != Script::OP_PUSH20)
          return offTemplate();
        if (!hash160Is(redeem, spent.ScriptHash.begin()))
          return invalid(EScriptError::EvalFalse);
        if (!(flags & EScriptP2SH))
          return valid();
        program = redeem.Data + 2;
      } else {
        // Core runs a script sig ahead of the witness: one here is the interpreter's
        if (p != end)
          return offTemplate();
      }

      // OP_0 <program> leaves the program on the stack; without WITNESS that is all
      if (!castToBool(program, 20))
        return invalid(EScriptError::EvalFalse);
      if (!(flags & EScriptWitness))
        return valid();
      // BIP141: the two witness elements run DUP HASH160 <program> EQUALVERIFY CHECKSIG
      if (in.WitnessStack.size() != 2)
        return invalid(EScriptError::WitnessProgramMismatch);
      if (in.WitnessStack[0].size() > MaxScriptElementSize || in.WitnessStack[1].size() > MaxScriptElementSize)
        return invalid(EScriptError::PushSize);
      const Script::CSpan sig{in.WitnessStack[0].data(), in.WitnessStack[0].size()};
      const Script::CSpan key{in.WitnessStack[1].data(), in.WitnessStack[1].size()};
      if (!hash160Is(key, program))
        return invalid(EScriptError::EqualVerify);
      if (auto error = ecdsaChecksig(sig, key, {}, ESigVersion::WitnessV0, flags, job, hashType); error != EScriptError::Ok)
        return invalid(error);
      if (mode != ESigMode::Trust) {
        uint8_t script[25];
        keyHashScript(program, script);
        job.Digest = sighashWitnessV0(tx, input, script, sizeof(script), spent.Value, hashType, checker.digests());
      }
      break;
    }

    case Info::EWitnessTaproot : {
      // OP_1 <program>; under TAPROOT two witness elements and more are the script path or an annex
      const bool taproot = (flags & EScriptWitness) && (flags & EScriptTaproot);
      if (p != end || (taproot && in.WitnessStack.size() > 1))
        return offTemplate();
      if (!castToBool(spent.WitnessProgram, 32))
        return invalid(EScriptError::EvalFalse);
      // Without TAPROOT a v1 program is a future one, valid as it is
      if (!taproot)
        return valid();
      if (in.WitnessStack.empty())
        return invalid(EScriptError::WitnessProgramWitnessEmpty);
      const Script::CSpan sig{in.WitnessStack[0].data(), in.WitnessStack[0].size()};
      if (auto error = checkSchnorrSignature(sig, tx.TxOut.size() > input, hashType); error != EScriptError::Ok)
        return invalid(error);
      job.Kind = ESigKind::Schnorr;
      job.Error = EScriptError::SchnorrSig;
      job.PubKeySize = 32;
      memcpy(job.PubKey, spent.WitnessProgram, 32);
      memcpy(job.Signature, sig.Data, 64);
      if (mode != ESigMode::Trust)
        job.Digest = sighashTaproot(tx, input, hashType, checker.taprootDigests(), spent);
      break;
    }

    default :
      return offTemplate();
  }

  if (mode == ESigMode::Trust)
    return valid();
  if (mode == ESigMode::Defer) {
    job.Tx = checker.ordinal();
    job.Input = static_cast<uint32_t>(input);
    batch->add(job);
    return CInputVerdict{EVerdict::Pending, EScriptError::Ok};
  }
  return verifySigJob(job) ? valid() : invalid(job.Error);
}

}
