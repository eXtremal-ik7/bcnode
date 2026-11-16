#pragma once

// Spends written as declarations: the output spent, its script sig and
// witness in Core's CScript notation, the transaction around them, the verdict along the flag sets.
// The signatures are made with the test keys when a spend is built, over digests put together
// apart from BTC/sighash.h

#include "BTC/scriptVerify.h"
#include <gtest/gtest.h>
#include <array>
#include <functional>
#include <optional>
#include <source_location>
#include <string>
#include <variant>
#include <vector>

namespace SpendTest {

using Bytes = std::vector<uint8_t>;
using Tx = BTC::Proto::CTransaction;

// Keys

struct CKey {
  enum class EForm : uint8_t {
    Compressed,
    Uncompressed,
    // 06 or 07 by y, as uncompressed otherwise
    Hybrid
  };
  // The secret key is this number
  uint8_t Secret;
  EForm Form = EForm::Compressed;

  Bytes pubKey() const;
  Bytes xOnly() const;
};

inline const CKey K1{1};
inline const CKey K1u{1, CKey::EForm::Uncompressed};
inline const CKey K1h{1, CKey::EForm::Hybrid};
inline const CKey K2{2};
// y odd: 03 compressed, 07 hybrid
inline const CKey K6{6};
inline const CKey K6h{6, CKey::EForm::Hybrid};

// Script items

struct COp {
  uint8_t Code;
};

inline constexpr COp OP_0{0x00};
inline constexpr COp OP_1{0x51};
inline constexpr COp OP_NOP{0x61};

inline constexpr uint8_t SIGHASH_DEFAULT = 0x00;
inline constexpr uint8_t SIGHASH_ALL = 0x01;
inline constexpr uint8_t SIGHASH_NONE = 0x02;
inline constexpr uint8_t SIGHASH_SINGLE = 0x03;
inline constexpr uint8_t SIGHASH_ANYONECANPAY = 0x80;

// An ECDSA signature of the input's digest under HashType, by the output's key unless By names
// another. The rest writes it otherwise or damages it: what BIP66 refuses and the lax parser takes
struct CSig {
  uint8_t HashType = SIGHASH_ALL;
  std::optional<CKey> Signer;
  // The hash type byte written, if not the one signed with
  std::optional<uint8_t> WrittenType;
  bool NegateS = false;
  bool DamageR = false;
  bool GrindHighR = false;
  uint8_t ZerosAheadR = 0;
  bool OneAheadR = false;
  bool OneAheadS = false;
  bool ZeroAheadS = false;
  bool NoZeroR = false;
  bool NoZeroS = false;
  bool LongTotalLength = false;
  uint8_t LengthBytesR = 0;
  bool TotalLengthPlusOne = false;
  bool ByteAfterS = false;
  bool NoR = false;
  bool NoS = false;
  uint8_t SequenceTag = 0x30;
  uint8_t IntegerTagR = 0x02;
  uint8_t IntegerTagS = 0x02;

  CSig By(const CKey &key) const { return with([&](CSig &s) { s.Signer = key; }); }
  CSig AsType(uint8_t type) const { return with([&](CSig &s) { s.WrittenType = type; }); }
  // n - S
  CSig HighS() const { return with([](CSig &s) { s.NegateS = true; }); }
  CSig FlipR() const { return with([](CSig &s) { s.DamageR = true; }); }
  // A nonce whose R has the top bit set, DER writes a zero ahead of it
  CSig HighR() const { return with([](CSig &s) { s.GrindHighR = true; }); }
  CSig PadR(uint8_t zeros = 1) const { return with([&](CSig &s) { s.ZerosAheadR = zeros; }); }
  CSig PadS() const { return with([](CSig &s) { s.ZeroAheadS = true; }); }
  // The zero that keeps a high integer positive dropped: negative in DER, the same to the lax parser
  CSig UnpadR() const { return with([](CSig &s) { s.NoZeroR = true; }); }
  CSig UnpadS() const { return with([](CSig &s) { s.NoZeroS = true; }); }
  // 0x01 ahead of the 32 bytes: 33 significant bytes, strict DER, a zero signature to libsecp256k1
  CSig OverflowR() const { return with([](CSig &s) { s.OneAheadR = true; }); }
  CSig OverflowS() const { return with([](CSig &s) { s.OneAheadS = true; }); }
  CSig LongTotal() const { return with([](CSig &s) { s.LongTotalLength = true; }); }
  // The length of R in the long form, zeros ahead of it
  CSig LongR(uint8_t bytes = 1) const { return with([&](CSig &s) { s.LengthBytesR = bytes; }); }
  CSig WrongTotal() const { return with([](CSig &s) { s.TotalLengthPlusOne = true; }); }
  CSig Trailing() const { return with([](CSig &s) { s.ByteAfterS = true; }); }
  CSig EmptyR() const { return with([](CSig &s) { s.NoR = true; }); }
  CSig EmptyS() const { return with([](CSig &s) { s.NoS = true; }); }
  CSig Tag(uint8_t tag) const { return with([&](CSig &s) { s.SequenceTag = tag; }); }
  CSig TagR(uint8_t tag) const { return with([&](CSig &s) { s.IntegerTagR = tag; }); }
  CSig TagS(uint8_t tag) const { return with([&](CSig &s) { s.IntegerTagS = tag; }); }

private:
  template<typename Change>
  CSig with(Change change) const {
    CSig result = *this;
    change(result);
    return result;
  }
};

inline CSig Sig(uint8_t hashType = SIGHASH_ALL)
{
  CSig sig;
  sig.HashType = hashType;
  return sig;
}

// A BIP340 signature: 64 bytes for SIGHASH_DEFAULT, 65 with the hash type byte
struct CSchnorr {
  std::optional<uint8_t> HashType;
  std::optional<CKey> Signer;
  std::optional<uint8_t> WrittenType;
  bool DamageR = false;

  CSchnorr By(const CKey &key) const { CSchnorr s = *this; s.Signer = key; return s; }
  CSchnorr AsType(uint8_t type) const { CSchnorr s = *this; s.WrittenType = type; return s; }
  CSchnorr FlipR() const { CSchnorr s = *this; s.DamageR = true; return s; }
};

inline CSchnorr Schnorr()
{
  return {};
}

inline CSchnorr Schnorr(uint8_t hashType)
{
  CSchnorr sig;
  sig.HashType = hashType;
  return sig;
}

// What a push or a witness element holds
using CElement = std::variant<Bytes, CKey, CSig, CSchnorr>;

struct CPushData1 {
  CElement Data;
};

inline CPushData1 Pushdata1(CElement data)
{
  return {std::move(data)};
}

// Script bytes as they are
struct CRaw {
  Bytes Data;
};

inline CRaw Raw(Bytes data)
{
  return {std::move(data)};
}

struct CScriptSig {
  std::vector<std::variant<CElement, CPushData1, COp, CRaw>> Items;
};

// A direct push of the element, OP_0 for nothing
inline CScriptSig operator<<(CScriptSig script, CElement element) { script.Items.emplace_back(std::move(element)); return script; }
inline CScriptSig operator<<(CScriptSig script, CPushData1 push) { script.Items.emplace_back(std::move(push)); return script; }
inline CScriptSig operator<<(CScriptSig script, COp op) { script.Items.emplace_back(op); return script; }
inline CScriptSig operator<<(CScriptSig script, CRaw raw) { script.Items.emplace_back(std::move(raw)); return script; }

inline CScriptSig ScriptSig()
{
  return {};
}

struct CWitness {
  std::vector<CElement> Elements;
};

inline CWitness operator<<(CWitness witness, CElement element) { witness.Elements.push_back(std::move(element)); return witness; }

inline CWitness Witness()
{
  return {};
}

// Outputs

struct COutput {
  enum class EDigest : uint8_t {
    // Off the fast path: a signature of zeros
    None,
    Legacy,
    Bip143,
    Bip341
  };
  Bytes Script;
  EDigest Digest = EDigest::None;
  // Legacy: the script itself; BIP143: the P2PKH script of the program
  Bytes ScriptCode;
  std::optional<CKey> Key;
};

COutput P2PK(const CKey &key);
COutput P2PKH(const CKey &key);
// Of any data pushed in place of the key
COutput P2PKH(const Bytes &pushed);
COutput P2WPKH(const CKey &key);
COutput P2WPKH(const Bytes &program);
COutput P2SH(const COutput &redeem);
COutput P2TR(const CKey &key);
COutput P2TR(const Bytes &program);
COutput P2WSH(const Bytes &program);
// OP_version and a push of the program
COutput WitnessProgram(uint8_t version, const Bytes &program);
COutput Anchor();
COutput Output(const Bytes &script);

Bytes Hash160(const CKey &key);
// The redeem script: the script of the output
Bytes Redeem(const COutput &output);
Bytes Annex();
Bytes Zeros(size_t size);
// Zeros, the sign bit in the last byte: false to CastToBool
Bytes NegativeZero(size_t size);
Bytes Concat(Bytes left, const Bytes &right);

// The transaction around the input

// A change after the signatures are made, the input checked is the one built
struct CChange {
  std::string Name;
  std::function<void(Tx &tx, std::vector<BTC::Proto::CTxOut> &spent, size_t input)> Apply;
};

CChange OutputValue(size_t index);
CChange OutputScript(size_t index);
CChange AddOutput();
CChange SpentAmount();
CChange OtherAmount();
CChange OtherScript();
CChange Sequence();
CChange OtherSequence();
CChange Outpoint();
CChange OtherOutpoint();
CChange OtherScriptSig();
CChange AddInput();
CChange LockTime();
CChange Version();

// The other inputs spend P2WPKH of K2, the outputs pay P2PKH of K2
struct CTx {
  size_t Inputs = 1;
  size_t Outputs = 1;
  size_t Checked = 0;
  CChange Then = {};
};

// Verdicts

struct CVerdict {
  BTC::EScriptError Error = BTC::EScriptError::Ok;
  bool Interpreted = false;

  CVerdict(BTC::EScriptError error = BTC::EScriptError::Ok, bool interpreted = false) : Error(error), Interpreted(interpreted) {}
  bool operator==(const CVerdict&) const = default;
};

// Off the templates: the interpreter's. A stub for now, it passes every spend
inline const CVerdict Interpreter{BTC::EScriptError::Ok, true};

void PrintTo(const CVerdict &verdict, std::ostream *os);

// The flag sets, each holding the one before it: none, P2SH, WITNESS, TAPROOT (a block before
// BIP66), Core's mandatory flags (a block of today), the standard ones (the mempool)
enum class EFlagSet : uint8_t {
  None,
  P2sh,
  Segwit,
  Taproot,
  Bip66,
  Standard
};

struct CFrom {
  EFlagSet From;
  CVerdict Verdict;
};

inline CFrom P2sh(CVerdict verdict) { return {EFlagSet::P2sh, verdict}; }
inline CFrom Segwit(CVerdict verdict) { return {EFlagSet::Segwit, verdict}; }
inline CFrom Taproot(CVerdict verdict) { return {EFlagSet::Taproot, verdict}; }
inline CFrom Bip66(CVerdict verdict) { return {EFlagSet::Bip66, verdict}; }
inline CFrom Standard(CVerdict verdict) { return {EFlagSet::Standard, verdict}; }

struct CVerdicts {
  std::array<CVerdict, 6> At;

  CVerdicts(BTC::EScriptError error) { At.fill(error); }
  CVerdicts(CVerdict verdict) { At.fill(verdict); }
};

// verdicts, and from's verdict from its flag set on
CVerdicts operator|(CVerdicts verdicts, const CFrom &from);

// Checks

// Built and checked under each flag set in the three modes
void spend(const COutput &output, const CScriptSig &scriptSig, const CVerdicts &verdicts,
           std::source_location where = std::source_location::current());
void spend(const COutput &output, const CWitness &witness, const CVerdicts &verdicts,
           std::source_location where = std::source_location::current());
void spend(const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, const CVerdicts &verdicts,
           std::source_location where = std::source_location::current());
void spend(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, const CVerdicts &verdicts,
           std::source_location where = std::source_location::current());
void spend(const CTx &tx, const COutput &output, const CWitness &witness, const CVerdicts &verdicts,
           std::source_location where = std::source_location::current());
void spend(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, const CVerdicts &verdicts,
           std::source_location where = std::source_location::current());

// Sync's batch: spends deferred under the mandatory flags into one batch, each the transaction its
// position names
class CBatch {
public:
  // The spend is to wait for its signature alone
  void add(const COutput &output, const CScriptSig &scriptSig, const CWitness &witness = {}, const CTx &tx = {},
           std::source_location where = std::source_location::current());
  const BTC::CSigBatch &jobs() const { return Jobs_; }
  // The positions of the jobs that fail
  std::vector<uint32_t> failed() const;

private:
  BTC::CSigBatch Jobs_;
};

}
