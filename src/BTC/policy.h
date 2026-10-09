// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// The mempool policy of the Bitcoin family (mempool-impl-plan.md §5.3): what a transaction valid
// by consensus must also be to enter the mempool - standard, cheap to check, paying for itself.
// One rule to a function, as the consensus rules in validation.h; a coin lists the rules its node
// keeps in checkPolicyStandalone/Contextual. The errors are Core's reject strings

#include "validation.h"

namespace BTC {

// What Core's GetTransactionWeight and GetTransactionSigOpCost count
struct CTxCost {
  uint32_t Size = 0;
  uint32_t BaseSize = 0;
  uint32_t Weight = 0;
  uint32_t SigOpsCost = 0;
  // Weight, or the sigop cost where it is heavier (Core's virtual size)
  uint32_t vsize() const;
};

namespace Policy {

// Core's limits (policy.h, consensus.h, standard.h)
constexpr uint32_t MaxStandardTxWeight = 400000;
constexpr uint32_t MaxStandardTxSigOpsCost = 80000 / 5;
constexpr uint32_t MaxP2SHSigOps = 15;
constexpr uint32_t WitnessScaleFactor = 4;
// A bare multisig coin is typed only up to OP_16 keys (isBareMultisig)
constexpr unsigned MaxBareMultisigKeys = 16;

using CSpan = Script::CSpan;

enum class EOutput {
  NonStandard,
  PubKey,
  PubKeyHash,
  ScriptHash,
  Multisig,
  NullData,
  WitnessKeyHash,
  WitnessScriptHash,
  WitnessTaproot,
  WitnessUnknown
};

// OP_RESERVED counts as a push, as in Core
bool isPushOnly(const uint8_t *p, const uint8_t *end);
// The P2SH redeem script: the last element a push-only scriptSig leaves on the stack. False past
// EvalScript's limits of a push and of the stack
bool lastPush(const xvector<uint8_t> &script, std::vector<uint8_t> &result);
bool witnessProgram(const uint8_t *script, size_t size, int &version, CSpan &program);
// Core's Solver, with the multisig bounds IsStandard puts on it
EOutput solve(const xvector<uint8_t> &script);
static inline bool isWitnessOutput(EOutput type)
{
  return type == EOutput::WitnessKeyHash || type == EOutput::WitnessScriptHash || type == EOutput::WitnessTaproot || type == EOutput::WitnessUnknown;
}
// MWEB: a witness program of version 9 and 32 bytes is a peg-in
bool isMwebPegin(const xvector<uint8_t> &script);
// roundUp: Core's CFeeRate::GetFee; the coins forked from older Core round down
int64_t feeForSize(int64_t ratePerKb, size_t size, bool roundUp);
// The witness program a spend runs: of a P2SH redeem script or of the spent output itself. Only
// version and size are known for the latter
bool spentWitnessProgram(const CPrevout &prevout, const xvector<uint8_t> &scriptSig, int &version, CSpan &program, std::vector<uint8_t> &redeemScript);
// Sigops a witness program spends: only P2WPKH and P2WSH have any
unsigned witnessSigOps(int version, const CSpan &program, const xvector<xvector<uint8_t>> &witness);
// The P2WSH and taproot resource limits of Core's IsWitnessStandard for one input
bool witnessStandard(int version, const CSpan &program, bool p2sh, const xvector<xvector<uint8_t>> &stack);

// An output that costs more to spend than it carries, at the dust relay fee
template<typename OutTy>
bool isDust(const OutTy &out, int64_t dustRelayFee, bool roundUp)
{
  const xvector<uint8_t> &script = out.PkScript;
  if ((!script.empty() && script[0] == Script::OP_RETURN) || script.size() > 10000)
    return false;

  size_t size = Io<OutTy>::getSerializedSize(out);
  int version;
  CSpan program;
  // An input spending it: outpoint, scriptSig length, sequence, and its witness at a quarter
  size += witnessProgram(script.data(), script.size(), version, program) ? 32 + 4 + 1 + 107 / WitnessScaleFactor + 4 : 32 + 4 + 1 + 107 + 4;
  return out.Value < feeForSize(dustRelayFee, size, roundUp);
}

}

// Size, weight and the legacy sigops of the transaction alone
template<typename TxTy>
void measureTx(const TxTy &tx, CTxCost &cost)
{
  const size_t baseSize = Io<TxTy>::getSerializedSize(tx, false);
  const size_t totalSize = Io<TxTy>::getSerializedSize(tx, true);
  cost.Size = static_cast<uint32_t>(totalSize);
  cost.BaseSize = static_cast<uint32_t>(baseSize);
  cost.Weight = static_cast<uint32_t>(baseSize * (Policy::WitnessScaleFactor - 1) + totalSize);

  unsigned sigOps = 0;
  for (const auto &in: tx.TxIn)
    sigOps += Script::sigOpCount(in.ScriptSig.data(), in.ScriptSig.data() + in.ScriptSig.size(), false);
  for (const auto &out: tx.TxOut)
    sigOps += Script::sigOpCount(out.PkScript.data(), out.PkScript.data() + out.PkScript.size(), false);
  cost.SigOpsCost = sigOps * Policy::WitnessScaleFactor;
}

// The sigops the spends add: P2SH redeem scripts and witness programs
template<typename TxTy>
void measureInputs(const TxTy &tx, const CPrevout *prevouts, CTxCost &cost)
{
  std::vector<uint8_t> redeemScript;
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    const auto &in = tx.TxIn[i];
    redeemScript.clear();
    if (prevouts[i].Type == Script::CUnspentOutputInfo::EScriptHash && Policy::lastPush(in.ScriptSig, redeemScript))
      cost.SigOpsCost += Script::sigOpCount(redeemScript.data(), redeemScript.data() + redeemScript.size(), true) * Policy::WitnessScaleFactor;

    int version;
    Policy::CSpan program;
    if (Policy::spentWitnessProgram(prevouts[i], in.ScriptSig, version, program, redeemScript))
      cost.SigOpsCost += Policy::witnessSigOps(version, program, in.WitnessStack);
  }
}

// The transaction alone

template<typename TxTy>
bool validateStandardVersion(const TxTy &tx, int32_t maxVersion, std::string &error)
{
  if (tx.Version < 1 || tx.Version > maxVersion) {
    error = "version";
    return false;
  }
  return true;
}

static inline bool validateStandardWeight(const CTxCost &cost, std::string &error)
{
  if (cost.Weight > Policy::MaxStandardTxWeight) {
    error = "tx-size";
    return false;
  }
  return true;
}

template<typename TxTy>
bool validateStandardScriptSigs(const TxTy &tx, size_t maxSize, std::string &error)
{
  for (const auto &in: tx.TxIn) {
    if (in.ScriptSig.size() > maxSize) {
      error = "scriptsig-size";
      return false;
    }
    if (!Policy::isPushOnly(in.ScriptSig.data(), in.ScriptSig.data() + in.ScriptSig.size())) {
      error = "scriptsig-not-pushonly";
      return false;
    }
  }
  return true;
}

// maxDataSize: a data output's own limit, where the coin's node still has one. Without segwit the
// coin's node refuses witness outputs
template<typename TxTy>
bool validateStandardOutputs(const TxTy &tx, size_t maxDataSize, bool witness, std::string &error)
{
  for (const auto &out: tx.TxOut) {
    Policy::EOutput type = Policy::solve(out.PkScript);
    if (type == Policy::EOutput::NonStandard ||
        (type == Policy::EOutput::NullData && out.PkScript.size() > maxDataSize) ||
        (!witness && Policy::isWitnessOutput(type))) {
      error = "scriptpubkey";
      return false;
    }
  }
  return true;
}

template<typename TxTy>
bool validateSingleDataOutput(const TxTy &tx, std::string &error)
{
  unsigned dataOutputs = 0;
  for (const auto &out: tx.TxOut)
    dataOutputs += Policy::solve(out.PkScript) == Policy::EOutput::NullData;
  if (dataOutputs > 1) {
    error = "multi-op-return";
    return false;
  }
  return true;
}

// Core 30: any number of data outputs within one budget of bytes
template<typename TxTy>
bool validateDataCarrier(const TxTy &tx, size_t budget, std::string &error)
{
  size_t bytes = 0;
  for (const auto &out: tx.TxOut) {
    if (Policy::solve(out.PkScript) == Policy::EOutput::NullData)
      bytes += out.PkScript.size();
  }
  if (bytes > budget) {
    error = "datacarrier";
    return false;
  }
  return true;
}

template<typename TxTy>
bool validateNoDust(const TxTy &tx, int64_t dustRelayFee, bool roundUp, std::string &error)
{
  for (const auto &out: tx.TxOut) {
    if (Policy::isDust(out, dustRelayFee, roundUp)) {
      error = "dust";
      return false;
    }
  }
  return true;
}

static inline bool validateNonWitnessSize(const CTxCost &cost, uint32_t minSize, std::string &error)
{
  if (cost.BaseSize < minSize) {
    error = "tx-size-small";
    return false;
  }
  return true;
}

// MWEB policy (MWEB::Policy::IsStandardTx): canonical outputs carry exactly the kernels of their
// peg-ins. A transaction here has no MWEB part, so any peg-in mismatches
template<typename TxTy>
bool validateMwebPegins(const TxTy &tx, std::string &error)
{
  for (const auto &out: tx.TxOut) {
    if (Policy::isMwebPegin(out.PkScript)) {
      error = "kernel-mismatch";
      return false;
    }
  }
  return true;
}

// With the outputs it spends

// Core's AreInputsStandard: every spent output of a known form, a P2SH redeem script within the
// sigop limit
template<typename TxTy>
bool validateStandardInputs(const TxTy &tx, const CPrevout *prevouts, std::string &error)
{
  std::vector<uint8_t> redeemScript;
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    const uint8_t type = prevouts[i].Type;
    bool standard = type != Script::CUnspentOutputInfo::ENonStandard &&
                    type != Script::CUnspentOutputInfo::EOpReturn &&
                    type != Script::CUnspentOutputInfo::EInvalid;
    if (standard && type == Script::CUnspentOutputInfo::EScriptHash) {
      redeemScript.clear();
      standard = Policy::lastPush(tx.TxIn[i].ScriptSig, redeemScript) &&
                 Script::sigOpCount(redeemScript.data(), redeemScript.data() + redeemScript.size(), true) <= Policy::MaxP2SHSigOps;
    }
    if (!standard) {
      error = "bad-txns-nonstandard-inputs";
      return false;
    }
  }
  return true;
}

// Core 30's CheckSigopsBIP54: the legacy sigops of the scriptSigs and of what they spend, P2SH
// redeem scripts counted accurately. A bare multisig coin keeps no key count: it counts the most its
// type allows
template<typename TxTy>
bool validateLegacySigOps(const TxTy &tx, const CPrevout *prevouts, unsigned limit, std::string &error)
{
  std::vector<uint8_t> redeemScript;
  unsigned sigOps = 0;
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    const xvector<uint8_t> &scriptSig = tx.TxIn[i].ScriptSig;
    sigOps += Script::sigOpCount(scriptSig.data(), scriptSig.data() + scriptSig.size(), true);
    switch (prevouts[i].Type) {
      case Script::CUnspentOutputInfo::EPubKey :
      case Script::CUnspentOutputInfo::EPubKeyHash :
        sigOps++;
        break;
      case Script::CUnspentOutputInfo::EMultisig :
        sigOps += Policy::MaxBareMultisigKeys;
        break;
      case Script::CUnspentOutputInfo::EScriptHash :
        redeemScript.clear();
        if (Policy::isPushOnly(scriptSig.data(), scriptSig.data() + scriptSig.size()) && Policy::lastPush(scriptSig, redeemScript))
          sigOps += Script::sigOpCount(redeemScript.data(), redeemScript.data() + redeemScript.size(), true);
        break;
      default :
        break;
    }
    if (sigOps > limit) {
      error = "bad-txns-nonstandard-inputs";
      return false;
    }
  }
  return true;
}

// Core's IsWitnessStandard: a witness only where a witness program is spent, within its limits
template<typename TxTy>
bool validateStandardWitness(const TxTy &tx, const CPrevout *prevouts, std::string &error)
{
  std::vector<uint8_t> redeemScript;
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    const auto &in = tx.TxIn[i];
    if (in.WitnessStack.empty())
      continue;
    int version;
    Policy::CSpan program;
    const bool p2sh = prevouts[i].Type == Script::CUnspentOutputInfo::EScriptHash;
    if (!Policy::spentWitnessProgram(prevouts[i], in.ScriptSig, version, program, redeemScript) ||
        !Policy::witnessStandard(version, program, p2sh, in.WitnessStack)) {
      error = "bad-witness-nonstandard";
      return false;
    }
  }
  return true;
}

static inline bool validateStandardSigOps(const CTxCost &cost, std::string &error)
{
  if (cost.SigOpsCost > Policy::MaxStandardTxSigOpsCost) {
    error = "bad-txns-too-many-sigops";
    return false;
  }
  return true;
}

static inline bool validateMinRelayFee(int64_t fee, const CTxCost &cost, int64_t minRelayTxFee, bool roundUp, std::string &error)
{
  if (fee < Policy::feeForSize(minRelayTxFee, cost.vsize(), roundUp)) {
    error = "min relay fee not met";
    return false;
  }
  return true;
}

}
