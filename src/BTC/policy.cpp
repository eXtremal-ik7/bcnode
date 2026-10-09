// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "policy.h"
#include <algorithm>

namespace BTC {

uint32_t CTxCost::vsize() const
{
  constexpr uint32_t BytesPerSigOp = 20;
  uint32_t weight = std::max(Weight, SigOpsCost * BytesPerSigOp);
  return (weight + Policy::WitnessScaleFactor - 1) / Policy::WitnessScaleFactor;
}

namespace Policy {

namespace {

constexpr size_t MaxP2WSHScriptSize = 3600;
constexpr size_t MaxP2WSHStackItems = 100;
constexpr size_t MaxP2WSHStackItemSize = 80;
constexpr size_t MaxTapscriptStackItemSize = 80;
constexpr int MwebPeginWitnessVersion = 9;

// EvalScript of a scriptSig fails past them: Core's MAX_SCRIPT_ELEMENT_SIZE and MAX_STACK_SIZE
constexpr size_t MaxScriptElementSize = 520;
constexpr size_t MaxStackSize = 1000;

constexpr uint8_t OP_RESERVED = 0x50;
constexpr uint8_t AnnexTag = 0x50;
constexpr uint8_t TaprootLeafMask = 0xFE;
constexpr uint8_t TaprootLeafTapscript = 0xC0;

size_t pubKeySize(uint8_t prefix)
{
  if (prefix == 2 || prefix == 3)
    return 33;
  if (prefix == 4 || prefix == 6 || prefix == 7)
    return 65;
  return 0;
}

}

bool isPushOnly(const uint8_t *p, const uint8_t *end)
{
  uint8_t opcode;
  while (p < end) {
    if (!Script::getOp(p, end, opcode) || opcode > Script::OP_16)
      return false;
  }
  return true;
}

// Small number opcodes push their value, as EvalScript would
bool lastPush(const xvector<uint8_t> &script, std::vector<uint8_t> &result)
{
  const uint8_t *p = script.data();
  const uint8_t *end = p + script.size();
  size_t pushes = 0;
  uint8_t opcode;
  CSpan data;
  while (p < end) {
    data = {};
    if (!Script::getOp(p, end, opcode, &data) || ++pushes > MaxStackSize || data.Size > MaxScriptElementSize)
      return false;
    if (opcode <= Script::OP_PUSHDATA4)
      result.assign(data.Data, data.Data + data.Size);
    else if (opcode == Script::OP_1NEGATE)
      result.assign(1, 0x81);
    else if (opcode >= Script::OP_1 && opcode <= Script::OP_16)
      result.assign(1, static_cast<uint8_t>(opcode - Script::OP_1 + 1));
    else if (opcode == OP_RESERVED)
      return false;
  }
  return pushes != 0;
}

bool witnessProgram(const uint8_t *script, size_t size, int &version, CSpan &program)
{
  if (size < 4 || size > 42)
    return false;
  if (script[0] != Script::OP_0 && (script[0] < Script::OP_1 || script[0] > Script::OP_16))
    return false;
  if (static_cast<size_t>(script[1]) + 2 != size)
    return false;
  version = script[0] == Script::OP_0 ? 0 : script[0] - Script::OP_1 + 1;
  program = {script + 2, size - 2};
  return true;
}

EOutput solve(const xvector<uint8_t> &script)
{
  const uint8_t *s = script.data();
  const size_t size = script.size();

  if (size == 23 && s[0] == Script::OP_HASH160 && s[1] == Script::OP_PUSH20 && s[22] == Script::OP_EQUAL)
    return EOutput::ScriptHash;

  int version;
  CSpan program;
  if (witnessProgram(s, size, version, program)) {
    if (version == 0 && program.Size == 20)
      return EOutput::WitnessKeyHash;
    if (version == 0 && program.Size == 32)
      return EOutput::WitnessScriptHash;
    if (version == 1 && program.Size == 32)
      return EOutput::WitnessTaproot;
    return version != 0 ? EOutput::WitnessUnknown : EOutput::NonStandard;
  }

  if (size >= 1 && s[0] == Script::OP_RETURN && isPushOnly(s + 1, s + size))
    return EOutput::NullData;

  if ((size == 35 || size == 67) && s[0] == size - 2 && pubKeySize(s[1]) == size - 2 && s[size - 1] == Script::OP_CHECKSIG)
    return EOutput::PubKey;

  if (size == 25 && s[0] == Script::OP_DUP && s[1] == Script::OP_HASH160 && s[2] == Script::OP_PUSH20 &&
      s[23] == Script::OP_EQUALVERIFY && s[24] == Script::OP_CHECKSIG)
    return EOutput::PubKeyHash;

  // OP_m <keys> OP_n OP_CHECKMULTISIG
  if (size >= 3 && s[size - 1] == Script::OP_CHECKMULTISIG &&
      s[0] >= Script::OP_1 && s[0] <= Script::OP_16 &&
      s[size - 2] >= Script::OP_1 && s[size - 2] <= Script::OP_16) {
    unsigned required = s[0] - Script::OP_1 + 1;
    unsigned declared = s[size - 2] - Script::OP_1 + 1;
    unsigned keys = 0;
    const uint8_t *p = s + 1;
    const uint8_t *end = s + size - 2;
    uint8_t opcode;
    CSpan data;
    while (p < end) {
      data = {};
      if (!Script::getOp(p, end, opcode, &data) || opcode > Script::OP_PUSHDATA4 || !data.Size || pubKeySize(data.Data[0]) != data.Size)
        return EOutput::NonStandard;
      keys++;
    }
    // IsStandard: up to x-of-3
    if (keys == declared && required <= declared && declared <= 3)
      return EOutput::Multisig;
    return EOutput::NonStandard;
  }

  return EOutput::NonStandard;
}

bool isMwebPegin(const xvector<uint8_t> &script)
{
  int version;
  CSpan program;
  return witnessProgram(script.data(), script.size(), version, program) && version == MwebPeginWitnessVersion && program.Size == 32;
}

int64_t feeForSize(int64_t ratePerKb, size_t size, bool roundUp)
{
  const int64_t product = ratePerKb * static_cast<int64_t>(size);
  int64_t fee = product / 1000 + (roundUp && product % 1000 > 0);
  if (fee == 0 && size && ratePerKb > 0)
    fee = 1;
  return fee;
}

bool spentWitnessProgram(const CPrevout &prevout, const xvector<uint8_t> &scriptSig, int &version, CSpan &program, std::vector<uint8_t> &redeemScript)
{
  switch (prevout.Type) {
    case Script::CUnspentOutputInfo::EScriptHash :
      redeemScript.clear();
      return lastPush(scriptSig, redeemScript) && witnessProgram(redeemScript.data(), redeemScript.size(), version, program);
    case Script::CUnspentOutputInfo::EWitnessPubKeyHash :
      version = 0;
      program = {nullptr, 20};
      return true;
    case Script::CUnspentOutputInfo::EWitnessScriptHash :
      version = 0;
      program = {nullptr, 32};
      return true;
    case Script::CUnspentOutputInfo::EWitnessTaproot :
      version = 1;
      program = {nullptr, 32};
      return true;
    default :
      return false;
  }
}

unsigned witnessSigOps(int version, const CSpan &program, const xvector<xvector<uint8_t>> &witness)
{
  if (version != 0)
    return 0;
  if (program.Size == 20)
    return 1;
  if (program.Size == 32 && !witness.empty()) {
    const xvector<uint8_t> &script = witness.back();
    return Script::sigOpCount(script.data(), script.data() + script.size(), true);
  }
  return 0;
}

bool witnessStandard(int version, const CSpan &program, bool p2sh, const xvector<xvector<uint8_t>> &stack)
{
  if (version == 0 && program.Size == 32) {
    if (stack.back().size() > MaxP2WSHScriptSize || stack.size() - 1 > MaxP2WSHStackItems)
      return false;
    for (size_t i = 0; i + 1 < stack.size(); i++) {
      if (stack[i].size() > MaxP2WSHStackItemSize)
        return false;
    }
  }

  if (version == 1 && program.Size == 32 && !p2sh) {
    size_t items = stack.size();
    if (items >= 2 && !stack[items - 1].empty() && stack[items - 1][0] == AnnexTag)
      return false;
    if (items >= 2) {
      const xvector<uint8_t> &control = stack[items - 1];
      if (control.empty())
        return false;
      if ((control[0] & TaprootLeafMask) == TaprootLeafTapscript) {
        for (size_t i = 0; i + 2 < items; i++) {
          if (stack[i].size() > MaxTapscriptStackItemSize)
            return false;
        }
      }
    } else if (items != 1) {
      return false;
    }
  }

  return true;
}

}
}
