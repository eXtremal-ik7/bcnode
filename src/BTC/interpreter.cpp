#include "interpreter.h"
#include "hash.h"
#include "crypto/ripemd160.h"
#include <openssl/evp.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <algorithm>
#include <string.h>
#include <vector>

namespace BTC {

namespace {

using enum EScriptError;
using enum Script::EOpcode;
using CSpan = Script::CSpan;
using CItem = std::vector<uint8_t>;
using CStack = std::vector<CItem>;
using CWitness = std::span<const xvector<uint8_t>>;

constexpr size_t MaxScriptSize = 10000;
constexpr size_t MaxStackSize = 1000;
constexpr unsigned MaxOpsPerScript = 201;
constexpr int64_t MaxKeysPerMultisig = 20;
constexpr int64_t ValidationWeightPerSigop = 50;
constexpr int64_t ValidationWeightOffset = 50;
constexpr uint8_t AnnexTag = 0x50;
constexpr uint8_t TapscriptLeaf = 0xC0;
constexpr size_t ControlBaseSize = 33;
constexpr size_t ControlNodeSize = 32;
constexpr size_t ControlMaxNodes = 128;
constexpr int64_t LockTimeThreshold = 500000000;
constexpr uint32_t SequenceFinal = 0xFFFFFFFF;
constexpr int64_t SequenceDisableFlag = int64_t{1} << 31;
constexpr int64_t SequenceTypeFlag = int64_t{1} << 22;
constexpr int64_t SequenceMask = 0xFFFF;

template<typename BytesTy>
CSpan span(const BytesTy &bytes)
{
  return {bytes.data(), bytes.size()};
}

// Script numbers: the magnitude little endian, the sign in the top bit of the last byte. An operand
// of arithmetic takes 4 bytes at most, a lock time 5; MINIMALDATA refuses a byte the value does without
bool readNumber(const CItem &item, bool minimal, size_t maxSize, int64_t &value)
{
  if (item.size() > maxSize)
    return false;
  if (minimal && !item.empty() && (item.back() & 0x7F) == 0 && (item.size() == 1 || !(item[item.size() - 2] & 0x80)))
    return false;

  uint64_t magnitude = 0;
  for (size_t i = 0; i < item.size(); i++)
    magnitude |= static_cast<uint64_t>(item[i]) << (8 * i);
  if (!item.empty() && (item.back() & 0x80)) {
    magnitude &= ~(uint64_t{0x80} << (8 * (item.size() - 1)));
    value = -static_cast<int64_t>(magnitude);
  } else {
    value = static_cast<int64_t>(magnitude);
  }
  return true;
}

CItem numberItem(int64_t value)
{
  CItem item;
  const bool negative = value < 0;
  for (uint64_t rest = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value); rest; rest >>= 8)
    item.push_back(static_cast<uint8_t>(rest));
  if (!item.empty() && (item.back() & 0x80))
    item.push_back(negative ? 0x80 : 0x00);
  else if (negative)
    item.back() |= 0x80;
  return item;
}

CItem boolItem(bool value)
{
  return value ? CItem{1} : CItem{};
}

int64_t unaryOp(uint8_t op, int64_t a)
{
  switch (op) {
    case OP_1ADD : return a + 1;
    case OP_1SUB : return a - 1;
    case OP_NEGATE : return -a;
    case OP_ABS : return a < 0 ? -a : a;
    case OP_NOT : return a == 0;
    default : return a != 0;
  }
}

int64_t binaryOp(uint8_t op, int64_t a, int64_t b)
{
  switch (op) {
    case OP_ADD : return a + b;
    case OP_SUB : return a - b;
    case OP_BOOLAND : return a != 0 && b != 0;
    case OP_BOOLOR : return a != 0 || b != 0;
    case OP_NUMEQUAL :
    case OP_NUMEQUALVERIFY : return a == b;
    case OP_NUMNOTEQUAL : return a != b;
    case OP_LESSTHAN : return a < b;
    case OP_GREATERTHAN : return a > b;
    case OP_LESSTHANOREQUAL : return a <= b;
    case OP_GREATERTHANOREQUAL : return a >= b;
    case OP_MIN : return std::min(a, b);
    default : return std::max(a, b);
  }
}

CItem hashItem(uint8_t op, const CItem &data)
{
  switch (op) {
    case OP_RIPEMD160 : {
      CItem hash(20);
      ripemd160(data.data(), data.size(), hash.data());
      return hash;
    }
    case OP_SHA1 : {
      CItem hash(20);
      unsigned size = 0;
      EVP_Digest(data.data(), data.size(), hash.data(), &size, EVP_sha1(), nullptr);
      return hash;
    }
    case OP_SHA256 : {
      CItem hash(32);
      sha256(data.data(), data.size(), hash.data());
      return hash;
    }
    case OP_HASH160 : {
      const BaseBlob<160> hash = sha256FollowRipemd160(data.data(), data.size());
      return CItem(hash.begin(), hash.end());
    }
    default : {
      const BaseBlob<256> hash = sha256d(data.data(), data.size());
      return CItem(hash.begin(), hash.end());
    }
  }
}

// CVE-2010-5137: refused even where they do not run
bool isDisabled(uint8_t op)
{
  return op == OP_CAT || op == OP_SUBSTR || op == OP_LEFT || op == OP_RIGHT || op == OP_INVERT || op == OP_AND || op == OP_OR ||
         op == OP_XOR || op == OP_2MUL || op == OP_2DIV || op == OP_MUL || op == OP_DIV || op == OP_MOD || op == OP_LSHIFT ||
         op == OP_RSHIFT;
}

// BIP342's OP_SUCCESSx: what soft forks give a meaning later
bool isOpSuccess(uint8_t op)
{
  return op == 80 || op == 98 || (op >= 126 && op <= 129) || (op >= 131 && op <= 134) || (op >= 137 && op <= 138) ||
         (op >= 141 && op <= 142) || (op >= 149 && op <= 153) || (op >= 187 && op <= 254);
}

// OP_RESERVED counts as a push here, as in Core
bool isPushOnly(CSpan script)
{
  const uint8_t *p = script.Data;
  const uint8_t *end = script.Data + script.Size;
  uint8_t op;
  while (p < end) {
    if (!Script::getOp(p, end, op) || op > OP_16)
      return false;
  }
  return true;
}

bool isPayToScriptHash(CSpan script)
{
  return script.Size == 23 && script.Data[0] == OP_HASH160 && script.Data[1] == OP_PUSH20 && script.Data[22] == OP_EQUAL;
}

// Core's CScript << data
CItem pushOf(CSpan data)
{
  CItem push;
  const size_t size = data.Size;
  if (size < OP_PUSHDATA1) {
    push = {static_cast<uint8_t>(size)};
  } else if (size <= 0xFF) {
    push = {OP_PUSHDATA1, static_cast<uint8_t>(size)};
  } else if (size <= 0xFFFF) {
    push = {OP_PUSHDATA2, static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8)};
  } else {
    push = {OP_PUSHDATA4, static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size >> 16),
            static_cast<uint8_t>(size >> 24)};
  }
  push.insert(push.end(), data.Data, data.Data + size);
  return push;
}

// Core's FindAndDelete: the push taken out wherever it starts at an opcode; the count
size_t findAndDelete(CItem &script, const CItem &push)
{
  CItem result;
  size_t found = 0;
  const uint8_t *p = script.data();
  const uint8_t *end = script.data() + script.size();
  const uint8_t *kept = p;
  uint8_t op;
  do {
    result.insert(result.end(), kept, p);
    while (static_cast<size_t>(end - p) >= push.size() && std::equal(push.begin(), push.end(), p)) {
      p += push.size();
      found++;
    }
    kept = p;
  } while (Script::getOp(p, end, op));

  if (found) {
    result.insert(result.end(), kept, end);
    script = std::move(result);
  }
  return found;
}

// A script cut short fails at its end anyway: its tail is kept as it is
CItem withoutCodeSeparators(const CItem &script)
{
  CItem result;
  const uint8_t *p = script.data();
  const uint8_t *end = script.data() + script.size();
  const uint8_t *kept = p;
  uint8_t op;
  while (p < end) {
    const uint8_t *opBegin = p;
    if (!Script::getOp(p, end, op))
      break;
    if (op == OP_CODESEPARATOR) {
      result.insert(result.end(), kept, opBegin);
      kept = p;
    }
  }
  result.insert(result.end(), kept, end);
  return result;
}

BaseBlob<256> tapLeafHash(uint8_t leafVersion, CSpan script)
{
  static const BaseBlob<256> tag = tagHash("TapLeaf");
  CSha256Writer writer = CSha256Writer::tagged(tag);
  writer.u8(leafVersion);
  writer.script(script.Data, script.Size);
  return writer.sha256();
}

BaseBlob<256> tapBranchHash(const BaseBlob<256> &a, const uint8_t *b)
{
  static const BaseBlob<256> tag = tagHash("TapBranch");
  CSha256Writer writer = CSha256Writer::tagged(tag);
  if (memcmp(a.begin(), b, 32) < 0) {
    writer.blob(a);
    writer.write(b, 32);
  } else {
    writer.write(b, 32);
    writer.blob(a);
  }
  return writer.sha256();
}

// BIP341: the output key is the internal key of the control block tweaked by the root of the leaf's
// Merkle path, its parity in the control block's low bit
bool taprootCommitment(const xvector<uint8_t> &control, CSpan program, const BaseBlob<256> &leafHash)
{
  static const BaseBlob<256> tag = tagHash("TapTweak");
  BaseBlob<256> root = leafHash;
  for (size_t i = ControlBaseSize; i < control.size(); i += ControlNodeSize)
    root = tapBranchHash(root, control.data() + i);

  secp256k1_xonly_pubkey internal;
  if (!secp256k1_xonly_pubkey_parse(secp256k1_context_static, &internal, control.data() + 1))
    return false;
  CSha256Writer tweak = CSha256Writer::tagged(tag);
  tweak.write(control.data() + 1, 32);
  tweak.blob(root);
  const BaseBlob<256> tweakHash = tweak.sha256();
  return secp256k1_xonly_pubkey_tweak_add_check(secp256k1_context_static, program.Data, control[0] & 1, &internal, tweakHash.begin());
}

BaseBlob<256> annexHash(const xvector<uint8_t> &annex)
{
  CSha256Writer writer;
  writer.script(annex.data(), annex.size());
  return writer.sha256();
}

// The witness as it is serialized: tapscript's budget of signatures
int64_t serializedSize(CWitness witness)
{
  auto varSize = [](size_t size) -> int64_t { return size < 0xFD ? 1 : size <= 0xFFFF ? 3 : size <= 0xFFFFFFFF ? 5 : 9; };
  int64_t size = varSize(witness.size());
  for (const auto &item: witness)
    size += varSize(item.size()) + static_cast<int64_t>(item.size());
  return size;
}

// The IF branches open. What matters is whether all of them run: their count and the depth of the
// first one that does not are enough
class CConditions {
public:
  bool empty() const { return Depth_ == 0; }
  bool allRun() const { return FirstSkipped_ == None; }

  void push(bool run) {
    if (!run && allRun())
      FirstSkipped_ = Depth_;
    Depth_++;
  }

  void pop() {
    Depth_--;
    if (FirstSkipped_ == Depth_)
      FirstSkipped_ = None;
  }

  // ELSE: only the innermost branch changes, and it matters only where it is the first skipped
  void toggle() {
    if (allRun())
      FirstSkipped_ = Depth_ - 1;
    else if (FirstSkipped_ == Depth_ - 1)
      FirstSkipped_ = None;
  }

private:
  static constexpr uint32_t None = UINT32_MAX;
  uint32_t Depth_ = 0;
  uint32_t FirstSkipped_ = None;
};

// The scripts of one input
class CInterpreter {
public:
  CInterpreter(uint32_t flags, CScriptInput &input) : Flags_(flags), Input_(input) {}

  EScriptError verify(CSpan scriptSig, CSpan scriptPubKey, CWitness witness);

private:
  uint32_t Flags_;
  CScriptInput &Input_;
  // Taproot: what a signature commits to besides the transaction, and tapscript's budget of signatures
  CTaprootSpend Taproot_;
  BaseBlob<256> AnnexHash_;
  BaseBlob<256> LeafHash_;
  int64_t WeightLeft_ = 0;

  bool flag(uint32_t flag) const { return (Flags_ & flag) != 0; }

  EScriptError eval(CStack &stack, CSpan script, ESigVersion version);
  EScriptError checksig(const CItem &sig, const CItem &key, CSpan code, ESigVersion version, bool &success);
  EScriptError tapscriptChecksig(const CItem &sig, const CItem &key, bool &success);
  EScriptError checkMultisig(CStack &stack, CSpan code, ESigVersion version, unsigned &opCount, bool &success);
  EScriptError legacyScriptCode(CSpan code, std::span<const CItem> sigs, CItem &result) const;
  EScriptError encodingError(const CItem &sig, const CItem &key, ESigVersion version) const;
  bool lockTimeSatisfied(int64_t lockTime) const;
  bool sequenceSatisfied(int64_t sequence) const;

  EScriptError witnessProgram(CWitness witness, int version, CSpan program, bool p2sh);
  EScriptError taproot(CWitness witness, CSpan program);
  EScriptError witnessScript(CWitness items, CSpan script, ESigVersion version);
};

EScriptError CInterpreter::verify(CSpan scriptSig, CSpan scriptPubKey, CWitness witness)
{
  if (flag(EScriptSigPushOnly) && !isPushOnly(scriptSig))
    return SigPushOnly;

  // The script sig and the output's script run one after the other on one stack (CVE-2010-5141)
  CStack stack;
  if (EScriptError error = eval(stack, scriptSig, ESigVersion::Base); error != Ok)
    return error;
  const bool p2sh = flag(EScriptP2SH) && isPayToScriptHash(scriptPubKey);
  CStack scriptSigStack = p2sh ? stack : CStack();
  if (EScriptError error = eval(stack, scriptPubKey, ESigVersion::Base); error != Ok)
    return error;
  if (stack.empty() || !castToBool(stack.back().data(), stack.back().size()))
    return EvalFalse;

  bool hadWitness = false;
  int version;
  CSpan program;
  if (flag(EScriptWitness) && Script::witnessProgram(scriptPubKey.Data, scriptPubKey.Size, version, program)) {
    hadWitness = true;
    if (scriptSig.Size != 0)
      return WitnessMalleated;
    if (EScriptError error = witnessProgram(witness, version, program, false); error != Ok)
      return error;
    // The witness left its own stack: nothing for CLEANSTACK to look at here
    stack.resize(1);
  }

  // P2SH: the last push of the script sig is the redeem script, it runs on what the pushes before it left
  if (p2sh) {
    if (!isPushOnly(scriptSig))
      return SigPushOnly;
    // Not empty: HASH160 of nothing fails above
    stack = std::move(scriptSigStack);
    const CItem redeem = std::move(stack.back());
    stack.pop_back();
    if (EScriptError error = eval(stack, span(redeem), ESigVersion::Base); error != Ok)
      return error;
    if (stack.empty() || !castToBool(stack.back().data(), stack.back().size()))
      return EvalFalse;

    if (flag(EScriptWitness) && Script::witnessProgram(redeem.data(), redeem.size(), version, program)) {
      hadWitness = true;
      // The script sig is the redeem script's push alone
      if (pushOf(span(redeem)) != CItem(scriptSig.Data, scriptSig.Data + scriptSig.Size))
        return WitnessMalleatedP2SH;
      if (EScriptError error = witnessProgram(witness, version, program, true); error != Ok)
        return error;
      stack.resize(1);
    }
  }

  if (flag(EScriptCleanStack) && stack.size() != 1)
    return CleanStack;
  if (flag(EScriptWitness) && !hadWitness && !witness.empty())
    return WitnessUnexpected;
  return Ok;
}

EScriptError CInterpreter::eval(CStack &stack, CSpan script, ESigVersion version)
{
  const bool tapscript = version == ESigVersion::Tapscript;
  const bool minimal = flag(EScriptMinimalData);
  if (!tapscript && script.Size > MaxScriptSize)
    return ScriptSize;

  CStack alt;
  CConditions conditions;
  const uint8_t *p = script.Data;
  const uint8_t *end = script.Data + script.Size;
  // A signature signs the script from the last OP_CODESEPARATOR run on
  const uint8_t *codeBegin = p;
  Taproot_.CodeSeparator = 0xFFFFFFFF;
  unsigned opCount = 0;

  // The i-th item from the top, 1 for the top one
  auto at = [&](size_t i) -> CItem & { return stack[stack.size() - i]; };
  auto number = [&](size_t i, int64_t &value) { return readNumber(at(i), minimal, 4, value); };
  auto pop = [&](size_t count) { stack.resize(stack.size() - count); };
  // Copies of `count` items, the first `from` from the top: DUP is (1, 1), 2OVER (4, 2)
  auto copy = [&](size_t from, size_t count) {
    for (size_t i = 0; i < count; i++) {
      CItem item = at(from);
      stack.push_back(std::move(item));
    }
  };

  for (uint32_t position = 0; p < end; position++) {
    uint8_t op;
    CSpan data;
    if (!Script::getOp(p, end, op, &data))
      return BadOpcode;
    if (data.Size > MaxScriptElementSize)
      return PushSize;
    // Pushes and OP_RESERVED do not count
    if (!tapscript && op > OP_16 && ++opCount > MaxOpsPerScript)
      return OpCount;
    if (isDisabled(op))
      return DisabledOpcode;
    if (op == OP_CODESEPARATOR && version == ESigVersion::Base && flag(EScriptConstScriptCode))
      return OpCodeSeparator;

    // A branch not run still sees its IF, ELSE and ENDIF
    const bool run = conditions.allRun();
    if (op <= OP_PUSHDATA4) {
      if (run) {
        if (minimal && !checkMinimalPush(data, op))
          return MinimalData;
        stack.emplace_back(data.Data, data.Data + data.Size);
      }
    } else if (run || (op >= OP_IF && op <= OP_ENDIF)) {
      switch (op) {
        case OP_1NEGATE : case OP_1 : case OP_2 : case OP_3 : case OP_4 : case OP_5 : case OP_6 : case OP_7 : case OP_8 :
        case OP_9 : case OP_10 : case OP_11 : case OP_12 : case OP_13 : case OP_14 : case OP_15 : case OP_16 :
          stack.push_back(numberItem(op - (OP_1 - 1)));
          break;

        // Control

        case OP_NOP :
          break;

        case OP_NOP1 : case OP_NOP4 : case OP_NOP5 : case OP_NOP6 : case OP_NOP7 : case OP_NOP8 : case OP_NOP9 : case OP_NOP10 :
          if (flag(EScriptDiscourageUpgradableNops))
            return DiscourageUpgradableNops;
          break;

        case OP_CHECKLOCKTIMEVERIFY :
        case OP_CHECKSEQUENCEVERIFY : {
          // NOP2 and NOP3 before their soft forks
          const bool lockTime = op == OP_CHECKLOCKTIMEVERIFY;
          if (!flag(lockTime ? EScriptCheckLockTimeVerify : EScriptCheckSequenceVerify))
            break;
          if (stack.empty())
            return InvalidStackOperation;
          // 5 bytes: lock times are unsigned 32-bit numbers
          int64_t value;
          if (!readNumber(at(1), minimal, 5, value))
            return UnknownError;
          if (value < 0)
            return NegativeLocktime;
          if (!(lockTime ? lockTimeSatisfied(value) : sequenceSatisfied(value)))
            return UnsatisfiedLocktime;
          break;
        }

        case OP_IF :
        case OP_NOTIF : {
          bool value = false;
          if (run) {
            if (stack.empty())
              return UnbalancedConditional;
            // Exactly empty or 1: a consensus rule of tapscript, a policy of witness v0
            const CItem &top = at(1);
            const bool notMinimal = top.size() > 1 || (top.size() == 1 && top[0] != 1);
            if (tapscript && notMinimal)
              return TapscriptMinimalIf;
            if (version == ESigVersion::WitnessV0 && flag(EScriptMinimalIf) && notMinimal)
              return MinimalIf;
            value = castToBool(top.data(), top.size()) != (op == OP_NOTIF);
            pop(1);
          }
          conditions.push(value);
          break;
        }

        case OP_ELSE :
          if (conditions.empty())
            return UnbalancedConditional;
          conditions.toggle();
          break;

        case OP_ENDIF :
          if (conditions.empty())
            return UnbalancedConditional;
          conditions.pop();
          break;

        case OP_VERIFY :
          if (stack.empty())
            return InvalidStackOperation;
          if (!castToBool(at(1).data(), at(1).size()))
            return Verify;
          pop(1);
          break;

        case OP_RETURN :
          return OpReturn;

        // Stack

        case OP_TOALTSTACK :
          if (stack.empty())
            return InvalidStackOperation;
          alt.push_back(std::move(at(1)));
          pop(1);
          break;

        case OP_FROMALTSTACK :
          if (alt.empty())
            return InvalidAltstackOperation;
          stack.push_back(std::move(alt.back()));
          alt.pop_back();
          break;

        case OP_2DROP :
          if (stack.size() < 2)
            return InvalidStackOperation;
          pop(2);
          break;

        case OP_2DUP :
          if (stack.size() < 2)
            return InvalidStackOperation;
          copy(2, 2);
          break;

        case OP_3DUP :
          if (stack.size() < 3)
            return InvalidStackOperation;
          copy(3, 3);
          break;

        case OP_2OVER :
          if (stack.size() < 4)
            return InvalidStackOperation;
          copy(4, 2);
          break;

        case OP_2ROT :
          // x1 x2 x3 x4 x5 x6 -> x3 x4 x5 x6 x1 x2
          if (stack.size() < 6)
            return InvalidStackOperation;
          std::rotate(stack.end() - 6, stack.end() - 4, stack.end());
          break;

        case OP_2SWAP :
          if (stack.size() < 4)
            return InvalidStackOperation;
          std::rotate(stack.end() - 4, stack.end() - 2, stack.end());
          break;

        case OP_IFDUP :
          if (stack.empty())
            return InvalidStackOperation;
          if (castToBool(at(1).data(), at(1).size()))
            copy(1, 1);
          break;

        case OP_DEPTH :
          stack.push_back(numberItem(static_cast<int64_t>(stack.size())));
          break;

        case OP_DROP :
          if (stack.empty())
            return InvalidStackOperation;
          pop(1);
          break;

        case OP_DUP :
          if (stack.empty())
            return InvalidStackOperation;
          copy(1, 1);
          break;

        case OP_NIP :
          if (stack.size() < 2)
            return InvalidStackOperation;
          stack.erase(stack.end() - 2);
          break;

        case OP_OVER :
          if (stack.size() < 2)
            return InvalidStackOperation;
          copy(2, 1);
          break;

        case OP_PICK :
        case OP_ROLL : {
          // The n-th item under n, copied or moved to the top
          if (stack.size() < 2)
            return InvalidStackOperation;
          int64_t n;
          if (!number(1, n))
            return UnknownError;
          pop(1);
          if (n < 0 || n >= static_cast<int64_t>(stack.size()))
            return InvalidStackOperation;
          CItem item = at(static_cast<size_t>(n) + 1);
          if (op == OP_ROLL)
            stack.erase(stack.end() - n - 1);
          stack.push_back(std::move(item));
          break;
        }

        case OP_ROT :
          // x1 x2 x3 -> x2 x3 x1
          if (stack.size() < 3)
            return InvalidStackOperation;
          std::rotate(stack.end() - 3, stack.end() - 2, stack.end());
          break;

        case OP_SWAP :
          if (stack.size() < 2)
            return InvalidStackOperation;
          std::swap(at(1), at(2));
          break;

        case OP_TUCK : {
          // x1 x2 -> x2 x1 x2
          if (stack.size() < 2)
            return InvalidStackOperation;
          CItem top = at(1);
          stack.insert(stack.end() - 2, std::move(top));
          break;
        }

        case OP_SIZE :
          if (stack.empty())
            return InvalidStackOperation;
          stack.push_back(numberItem(static_cast<int64_t>(at(1).size())));
          break;

        // Logic and numbers

        case OP_EQUAL :
        case OP_EQUALVERIFY : {
          if (stack.size() < 2)
            return InvalidStackOperation;
          const bool equal = at(1) == at(2);
          pop(2);
          if (op == OP_EQUALVERIFY && !equal)
            return EqualVerify;
          if (op == OP_EQUAL)
            stack.push_back(boolItem(equal));
          break;
        }

        case OP_1ADD : case OP_1SUB : case OP_NEGATE : case OP_ABS : case OP_NOT : case OP_0NOTEQUAL : {
          if (stack.empty())
            return InvalidStackOperation;
          int64_t a;
          if (!number(1, a))
            return UnknownError;
          at(1) = numberItem(unaryOp(op, a));
          break;
        }

        case OP_ADD : case OP_SUB : case OP_BOOLAND : case OP_BOOLOR : case OP_NUMEQUAL : case OP_NUMEQUALVERIFY :
        case OP_NUMNOTEQUAL : case OP_LESSTHAN : case OP_GREATERTHAN : case OP_LESSTHANOREQUAL : case OP_GREATERTHANOREQUAL :
        case OP_MIN : case OP_MAX : {
          if (stack.size() < 2)
            return InvalidStackOperation;
          int64_t a, b;
          if (!number(2, a) || !number(1, b))
            return UnknownError;
          pop(2);
          const int64_t result = binaryOp(op, a, b);
          if (op == OP_NUMEQUALVERIFY && !result)
            return NumEqualVerify;
          if (op != OP_NUMEQUALVERIFY)
            stack.push_back(numberItem(result));
          break;
        }

        case OP_WITHIN : {
          // x min max -> min <= x < max
          if (stack.size() < 3)
            return InvalidStackOperation;
          int64_t x, min, max;
          if (!number(3, x) || !number(2, min) || !number(1, max))
            return UnknownError;
          pop(3);
          stack.push_back(boolItem(min <= x && x < max));
          break;
        }

        // Crypto

        case OP_RIPEMD160 : case OP_SHA1 : case OP_SHA256 : case OP_HASH160 : case OP_HASH256 :
          if (stack.empty())
            return InvalidStackOperation;
          at(1) = hashItem(op, at(1));
          break;

        case OP_CODESEPARATOR :
          codeBegin = p;
          Taproot_.CodeSeparator = position;
          break;

        case OP_CHECKSIG :
        case OP_CHECKSIGVERIFY : {
          if (stack.size() < 2)
            return InvalidStackOperation;
          bool success;
          if (EScriptError error = checksig(at(2), at(1), {codeBegin, static_cast<size_t>(end - codeBegin)}, version, success); error != Ok)
            return error;
          pop(2);
          if (op == OP_CHECKSIGVERIFY && !success)
            return CheckSigVerify;
          if (op == OP_CHECKSIG)
            stack.push_back(boolItem(success));
          break;
        }

        case OP_CHECKSIGADD : {
          // sig n key -> n + 1 if the signature is valid
          if (!tapscript)
            return BadOpcode;
          if (stack.size() < 3)
            return InvalidStackOperation;
          int64_t n;
          if (!number(2, n))
            return UnknownError;
          bool success;
          if (EScriptError error = checksig(at(3), at(1), {}, version, success); error != Ok)
            return error;
          pop(3);
          stack.push_back(numberItem(n + success));
          break;
        }

        case OP_CHECKMULTISIG :
        case OP_CHECKMULTISIGVERIFY : {
          if (tapscript)
            return TapscriptCheckMultisig;
          bool success;
          if (EScriptError error = checkMultisig(stack, {codeBegin, static_cast<size_t>(end - codeBegin)}, version, opCount, success);
              error != Ok)
            return error;
          if (op == OP_CHECKMULTISIGVERIFY && !success)
            return CheckMultisigVerify;
          if (op == OP_CHECKMULTISIG)
            stack.push_back(boolItem(success));
          break;
        }

        // OP_RESERVED, OP_VER, OP_VERIF, OP_VERNOTIF, OP_RESERVED1, OP_RESERVED2 and bytes no opcode has
        default :
          return BadOpcode;
      }
    }

    if (stack.size() + alt.size() > MaxStackSize)
      return StackSize;
  }

  return conditions.empty() ? Ok : UnbalancedConditional;
}

// CHECKSIG, CHECKSIGVERIFY, CHECKSIGADD: an error ends the script, success is the signature's
EScriptError CInterpreter::checksig(const CItem &sig, const CItem &key, CSpan code, ESigVersion version, bool &success)
{
  if (version == ESigVersion::Tapscript)
    return tapscriptChecksig(sig, key, success);

  CItem legacyCode;
  if (version == ESigVersion::Base) {
    if (EScriptError error = legacyScriptCode(code, {&sig, 1}, legacyCode); error != Ok)
      return error;
    code = span(legacyCode);
  }
  if (EScriptError error = encodingError(sig, key, version); error != Ok)
    return error;
  success = Input_.checkEcdsa(span(sig), span(key), code, version);
  // NULLFAIL: a signature that fails is empty
  if (!success && flag(EScriptNullFail) && !sig.empty())
    return SigNullFail;
  return Ok;
}

// BIP342: an empty signature fails and the script goes on, any other that fails ends it. Keys of
// other sizes are for soft forks to come and pass
EScriptError CInterpreter::tapscriptChecksig(const CItem &sig, const CItem &key, bool &success)
{
  success = !sig.empty();
  if (success && (WeightLeft_ -= ValidationWeightPerSigop) < 0)
    return TapscriptValidationWeight;
  if (key.empty())
    return PubKeyType;
  if (key.size() != 32)
    return flag(EScriptDiscourageUpgradablePubKeyType) ? DiscourageUpgradablePubKeyType : Ok;
  return success ? Input_.checkSchnorr(span(sig), span(key), Taproot_) : Ok;
}

// <dummy> <sig>... <m> <key>... <n> CHECKMULTISIG. The signatures go through the keys in order, the
// last ones first; it succeeds when all m find their key. The dummy is a bug consensus keeps
EScriptError CInterpreter::checkMultisig(CStack &stack, CSpan code, ESigVersion version, unsigned &opCount, bool &success)
{
  auto at = [&](size_t i) -> CItem & { return stack[stack.size() - i]; };
  const bool minimal = flag(EScriptMinimalData);

  if (stack.empty())
    return InvalidStackOperation;
  int64_t keys;
  if (!readNumber(at(1), minimal, 4, keys))
    return UnknownError;
  if (keys < 0 || keys > MaxKeysPerMultisig)
    return PubKeyCount;
  // Every key counts as an opcode
  opCount += static_cast<unsigned>(keys);
  if (opCount > MaxOpsPerScript)
    return OpCount;
  if (stack.size() < static_cast<size_t>(keys) + 2)
    return InvalidStackOperation;
  int64_t sigs;
  if (!readNumber(at(static_cast<size_t>(keys) + 2), minimal, 4, sigs))
    return UnknownError;
  if (sigs < 0 || sigs > keys)
    return SigCount;
  const size_t items = static_cast<size_t>(keys + sigs) + 3;
  if (stack.size() < items)
    return InvalidStackOperation;

  const std::span<const CItem> signatures(stack.end() - (keys + 2 + sigs), stack.end() - (keys + 2));
  CItem legacyCode;
  if (version == ESigVersion::Base) {
    if (EScriptError error = legacyScriptCode(code, signatures, legacyCode); error != Ok)
      return error;
    code = span(legacyCode);
  }

  size_t sig = static_cast<size_t>(keys) + 3;
  size_t key = 2;
  int64_t sigsLeft = sigs;
  int64_t keysLeft = keys;
  success = true;
  while (success && sigsLeft > 0) {
    // Encodings as the pairs are tried: CHECKMULTISIG NOT shows the order under STRICTENC
    if (EScriptError error = encodingError(at(sig), at(key), version); error != Ok)
      return error;
    if (Input_.checkEcdsa(span(at(sig)), span(at(key)), code, version)) {
      sig++;
      sigsLeft--;
    }
    key++;
    keysLeft--;
    // More signatures left than keys: they cannot all match
    success = sigsLeft <= keysLeft;
  }

  if (!success && flag(EScriptNullFail)) {
    for (const CItem &signature: signatures) {
      if (!signature.empty())
        return SigNullFail;
    }
  }
  if (flag(EScriptNullDummy) && !at(items).empty())
    return SigNullDummy;
  stack.resize(stack.size() - items);
  return Ok;
}

// The script code of a legacy digest: the pushes of the signatures checked taken out (FindAndDelete,
// refused by CONST_SCRIPTCODE), then OP_CODESEPARATOR. The last signature first, as Core does
EScriptError CInterpreter::legacyScriptCode(CSpan code, std::span<const CItem> sigs, CItem &result) const
{
  result.assign(code.Data, code.Data + code.Size);
  for (auto sig = sigs.rbegin(); sig != sigs.rend(); ++sig) {
    if (findAndDelete(result, pushOf(span(*sig))) && flag(EScriptConstScriptCode))
      return SigFindAndDelete;
  }
  result = withoutCodeSeparators(result);
  return Ok;
}

EScriptError CInterpreter::encodingError(const CItem &sig, const CItem &key, ESigVersion version) const
{
  EScriptError error = checkSignatureEncoding(span(sig), Flags_);
  return error != Ok ? error : checkPubKeyEncoding(span(key), Flags_, version);
}

// BIP65: a lock of the transaction's kind, height or time, not past its lock time, and the input not
// final - a final one turns the lock time off
bool CInterpreter::lockTimeSatisfied(int64_t lockTime) const
{
  const int64_t txLockTime = Input_.LockTime;
  if ((lockTime < LockTimeThreshold) != (txLockTime < LockTimeThreshold))
    return false;
  return lockTime <= txLockTime && Input_.Sequence != SequenceFinal;
}

// BIP112 against the input's relative lock (BIP68): the same kind, height or time, not past it
bool CInterpreter::sequenceSatisfied(int64_t sequence) const
{
  // The operand's disable bit: a NOP, for soft forks to come
  if (sequence & SequenceDisableFlag)
    return true;
  const int64_t txSequence = Input_.Sequence;
  if (Input_.TxVersion < 2 || (txSequence & SequenceDisableFlag))
    return false;
  const int64_t wanted = sequence & (SequenceTypeFlag | SequenceMask);
  const int64_t locked = txSequence & (SequenceTypeFlag | SequenceMask);
  if ((wanted < SequenceTypeFlag) != (locked < SequenceTypeFlag))
    return false;
  return wanted <= locked;
}

EScriptError CInterpreter::witnessProgram(CWitness witness, int version, CSpan program, bool p2sh)
{
  if (version == 0) {
    if (program.Size == 32) {
      // P2WSH: the last element is the script, the program its SHA256
      if (witness.empty())
        return WitnessProgramWitnessEmpty;
      const xvector<uint8_t> &script = witness.back();
      BaseBlob<256> hash;
      sha256(script.data(), script.size(), hash.begin());
      if (memcmp(hash.begin(), program.Data, 32) != 0)
        return WitnessProgramMismatch;
      return witnessScript(witness.first(witness.size() - 1), span(script), ESigVersion::WitnessV0);
    }
    if (program.Size == 20) {
      // P2WPKH: the signature and the key run the script of P2PKH
      if (witness.size() != 2)
        return WitnessProgramMismatch;
      uint8_t script[25];
      keyHashScript(program.Data, script);
      return witnessScript(witness, {script, sizeof(script)}, ESigVersion::WitnessV0);
    }
    return WitnessProgramWrongLength;
  }

  if (version == 1 && program.Size == 32 && !p2sh)
    return taproot(witness, program);
  // Pay to anchor; any other program is for soft forks to come
  const bool anchor = version == 1 && program.Size == 2 && program.Data[0] == 0x4E && program.Data[1] == 0x73;
  if (anchor && !p2sh)
    return Ok;
  return flag(EScriptDiscourageUpgradableWitnessProgram) ? DiscourageUpgradableWitnessProgram : Ok;
}

EScriptError CInterpreter::taproot(CWitness witness, CSpan program)
{
  if (!flag(EScriptTaproot))
    return Ok;
  if (witness.empty())
    return WitnessProgramWitnessEmpty;

  // The annex: the last of two elements and more when it starts with 0x50. It is signed, not run
  CWitness stack = witness;
  if (stack.size() >= 2 && !stack.back().empty() && stack.back()[0] == AnnexTag) {
    AnnexHash_ = annexHash(stack.back());
    Taproot_.AnnexHash = &AnnexHash_;
    stack = stack.first(stack.size() - 1);
  }

  // Key path: a signature by the output key
  if (stack.size() == 1)
    return Input_.checkSchnorr(span(stack[0]), program, Taproot_);

  // Script path: the script, then the control block that proves it a leaf of the output key's tree
  const xvector<uint8_t> &control = stack.back();
  const xvector<uint8_t> &script = stack[stack.size() - 2];
  if (control.size() < ControlBaseSize || control.size() > ControlBaseSize + ControlNodeSize * ControlMaxNodes ||
      (control.size() - ControlBaseSize) % ControlNodeSize != 0)
    return TaprootWrongControlSize;
  const uint8_t leafVersion = control[0] & 0xFE;
  LeafHash_ = tapLeafHash(leafVersion, span(script));
  if (!taprootCommitment(control, program, LeafHash_))
    return WitnessProgramMismatch;
  Taproot_.LeafHash = &LeafHash_;

  if (leafVersion != TapscriptLeaf)
    return flag(EScriptDiscourageUpgradableTaprootVersion) ? DiscourageUpgradableTaprootVersion : Ok;
  // Signatures are paid for by the size of the whole witness
  WeightLeft_ = serializedSize(witness) + ValidationWeightOffset;
  return witnessScript(stack.first(stack.size() - 2), span(script), ESigVersion::Tapscript);
}

// A witness script on the witness items before it. It leaves one item, and that is true
EScriptError CInterpreter::witnessScript(CWitness items, CSpan script, ESigVersion version)
{
  if (version == ESigVersion::Tapscript) {
    // An OP_SUCCESSx anywhere makes the spend valid ahead of every other rule
    const uint8_t *p = script.Data;
    const uint8_t *end = script.Data + script.Size;
    uint8_t op;
    while (p < end) {
      if (!Script::getOp(p, end, op))
        return BadOpcode;
      if (isOpSuccess(op))
        return flag(EScriptDiscourageOpSuccess) ? DiscourageOpSuccess : Ok;
    }
    if (items.size() > MaxStackSize)
      return StackSize;
  }

  CStack stack;
  stack.reserve(items.size());
  for (const xvector<uint8_t> &item: items) {
    if (item.size() > MaxScriptElementSize)
      return PushSize;
    stack.emplace_back(item.data(), item.data() + item.size());
  }
  if (EScriptError error = eval(stack, script, version); error != Ok)
    return error;
  if (stack.size() != 1)
    return CleanStack;
  return castToBool(stack.back().data(), stack.back().size()) ? Ok : EvalFalse;
}

}

EScriptError verifyScript(CSpan scriptSig, CSpan scriptPubKey, std::span<const xvector<uint8_t>> witness, uint32_t flags, CScriptInput &input)
{
  return CInterpreter(flags, input).verify(scriptSig, scriptPubKey, witness);
}

}
