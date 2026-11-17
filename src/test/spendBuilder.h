#pragma once

// Spends written as declarations: the output spent, its script sig and witness - scripts as text
// or in Core's CScript notation - the transaction around them, the verdict along the flag sets.
// The signatures are made with the test keys when a spend is built, over digests put together
// apart from BTC/sighash.h

#include "BTC/scriptVerify.h"
#include <gtest/gtest.h>
#include <array>
#include <functional>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
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
inline const CKey K2u{2, CKey::EForm::Uncompressed};
// y odd: 03 compressed, 07 hybrid
inline const CKey K6{6};
inline const CKey K6u{6, CKey::EForm::Uncompressed};
inline const CKey K6h{6, CKey::EForm::Hybrid};

// Script items

struct COp {
  uint8_t Code;
};

// Core's opcodetype: the constant OP_<name>, and <name> in a script written as text
#define SPEND_TEST_OPCODES(X) \
  X(0, 0x00) X(FALSE, 0x00) X(PUSHDATA1, 0x4c) X(PUSHDATA2, 0x4d) X(PUSHDATA4, 0x4e) X(1NEGATE, 0x4f) \
  X(RESERVED, 0x50) X(1, 0x51) X(2, 0x52) X(3, 0x53) X(4, 0x54) X(5, 0x55) X(6, 0x56) X(7, 0x57) X(8, 0x58) \
  X(9, 0x59) X(10, 0x5a) X(11, 0x5b) X(12, 0x5c) X(13, 0x5d) X(14, 0x5e) X(15, 0x5f) X(16, 0x60) X(NOP, 0x61) \
  X(VER, 0x62) X(IF, 0x63) X(NOTIF, 0x64) X(VERIF, 0x65) X(VERNOTIF, 0x66) X(ELSE, 0x67) X(ENDIF, 0x68) \
  X(VERIFY, 0x69) X(RETURN, 0x6a) X(TOALTSTACK, 0x6b) X(FROMALTSTACK, 0x6c) X(2DROP, 0x6d) X(2DUP, 0x6e) \
  X(3DUP, 0x6f) X(2OVER, 0x70) X(2ROT, 0x71) X(2SWAP, 0x72) X(IFDUP, 0x73) X(DEPTH, 0x74) X(DROP, 0x75) X(DUP, 0x76) \
  X(NIP, 0x77) X(OVER, 0x78) X(PICK, 0x79) X(ROLL, 0x7a) X(ROT, 0x7b) X(SWAP, 0x7c) X(TUCK, 0x7d) X(CAT, 0x7e) \
  X(SUBSTR, 0x7f) X(LEFT, 0x80) X(RIGHT, 0x81) X(SIZE, 0x82) X(INVERT, 0x83) X(AND, 0x84) X(OR, 0x85) X(XOR, 0x86) \
  X(EQUAL, 0x87) X(EQUALVERIFY, 0x88) X(RESERVED1, 0x89) X(RESERVED2, 0x8a) X(1ADD, 0x8b) X(1SUB, 0x8c) X(2MUL, 0x8d) \
  X(2DIV, 0x8e) X(NEGATE, 0x8f) X(ABS, 0x90) X(NOT, 0x91) X(0NOTEQUAL, 0x92) X(ADD, 0x93) X(SUB, 0x94) X(MUL, 0x95) \
  X(DIV, 0x96) X(MOD, 0x97) X(LSHIFT, 0x98) X(RSHIFT, 0x99) X(BOOLAND, 0x9a) X(BOOLOR, 0x9b) X(NUMEQUAL, 0x9c) \
  X(NUMEQUALVERIFY, 0x9d) X(NUMNOTEQUAL, 0x9e) X(LESSTHAN, 0x9f) X(GREATERTHAN, 0xa0) X(LESSTHANOREQUAL, 0xa1) \
  X(GREATERTHANOREQUAL, 0xa2) X(MIN, 0xa3) X(MAX, 0xa4) X(WITHIN, 0xa5) X(RIPEMD160, 0xa6) X(SHA1, 0xa7) \
  X(SHA256, 0xa8) X(HASH160, 0xa9) X(HASH256, 0xaa) X(CODESEPARATOR, 0xab) X(CHECKSIG, 0xac) X(CHECKSIGVERIFY, 0xad) \
  X(CHECKMULTISIG, 0xae) X(CHECKMULTISIGVERIFY, 0xaf) X(NOP1, 0xb0) X(CHECKLOCKTIMEVERIFY, 0xb1) X(NOP2, 0xb1) \
  X(CHECKSEQUENCEVERIFY, 0xb2) X(NOP3, 0xb2) X(NOP4, 0xb3) X(NOP5, 0xb4) X(NOP6, 0xb5) X(NOP7, 0xb6) X(NOP8, 0xb7) \
  X(NOP9, 0xb8) X(NOP10, 0xb9) X(CHECKSIGADD, 0xba)

#define SPEND_TEST_OPCODE(name, code) inline constexpr COp OP_##name{code};
SPEND_TEST_OPCODES(SPEND_TEST_OPCODE)
#undef SPEND_TEST_OPCODE

// A byte no name is given to
inline constexpr COp Op(uint8_t code) { return {code}; }

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
  bool TypeTwice = false;
  bool NoR = false;
  bool NoS = false;
  uint8_t SequenceTag = 0x30;
  uint8_t IntegerTagR = 0x02;
  uint8_t IntegerTagS = 0x02;
  // The script code signed, if not the output's
  std::optional<Bytes> ScriptCode;

  CSig By(const CKey &key) const { return with([&](CSig &s) { s.Signer = key; }); }
  CSig AsType(uint8_t type) const { return with([&](CSig &s) { s.WrittenType = type; }); }
  // The script code as the interpreter takes it at this CHECKSIG: from the last OP_CODESEPARATOR run, the
  // pushes of the signatures checked taken out of a script that is not segwit's
  CSig Over(const Bytes &code) const { return with([&](CSig &s) { s.ScriptCode = code; }); }
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
  // The hash type byte once more, after the signature the lengths cover
  CSig DoubleType() const { return with([](CSig &s) { s.TypeTwice = true; }); }
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
  // Tapscript: the position of the last OP_CODESEPARATOR run, counted in opcodes; none by default
  uint32_t CodeSeparator = 0xffffffff;

  CSchnorr By(const CKey &key) const { CSchnorr s = *this; s.Signer = key; return s; }
  CSchnorr Separator(uint32_t position) const { CSchnorr s = *this; s.CodeSeparator = position; return s; }
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

// The script the output is the hash of: P2SH's redeem script, P2WSH's witness script
struct CRedeemScript {};
struct CWitnessScript {};

inline CRedeemScript Redeem() { return {}; }
inline CWitnessScript WitnessScript() { return {}; }

// Taproot's script path: the script of the leaf spent and the control block of the output
struct CLeafScript {};
struct CControl {};

inline CLeafScript LeafScript() { return {}; }
inline CControl Control() { return {}; }

// What a push or a witness element holds
using CElement = std::variant<Bytes, CKey, CSig, CSchnorr, CRedeemScript, CWitnessScript, CLeafScript, CControl>;

// OP_PUSHDATA1, 2 or 4 whatever the size
struct CPushData {
  CElement Data;
  uint8_t LengthBytes;
};

inline CPushData Pushdata1(CElement data) { return {std::move(data), 1}; }
inline CPushData Pushdata2(CElement data) { return {std::move(data), 2}; }
inline CPushData Pushdata4(CElement data) { return {std::move(data), 4}; }

// A number as Core's CScript << int64_t writes it: OP_1NEGATE, OP_0 to OP_16, else the push of its
// minimal CScriptNum
struct CNum {
  int64_t Value;
};

// Script bytes as they are
struct CRaw {
  Bytes Data;
};

inline CRaw Raw(Bytes data)
{
  return {std::move(data)};
}

// A script: a script sig, or what an output, a redeem or a witness script holds - there with no
// signature in it
struct CScriptSig {
  std::vector<std::variant<CElement, CPushData, COp, CRaw, CNum>> Items;
};

// The minimal push of the element, as Core's CScript << vector: OP_0 for nothing
inline CScriptSig operator<<(CScriptSig script, CElement element) { script.Items.emplace_back(std::move(element)); return script; }
inline CScriptSig operator<<(CScriptSig script, CPushData push) { script.Items.emplace_back(std::move(push)); return script; }
inline CScriptSig operator<<(CScriptSig script, COp op) { script.Items.emplace_back(op); return script; }
inline CScriptSig operator<<(CScriptSig script, CRaw raw) { script.Items.emplace_back(std::move(raw)); return script; }
inline CScriptSig operator<<(CScriptSig script, int64_t number) { script.Items.emplace_back(CNum{number}); return script; }
inline CScriptSig operator<<(CScriptSig script, const CScriptSig &part)
{
  script.Items.insert(script.Items.end(), part.Items.begin(), part.Items.end());
  return script;
}

inline CScriptSig ScriptSig() { return {}; }
inline CScriptSig Script() { return {}; }

// A script as text: what Core's ParseScript takes, and names for what Core writes in bytes. Words, spaces apart:
//   12  -1            a number, as << does it
//   ADD  OP_ADD       an opcode, any of SPEND_TEST_OPCODES
//   'abc'             the push of the text
//   <c0ffee>          the minimal push of the bytes; <61*520> of 520 bytes 0x61, <> of nothing
//   PUSHDATA1<c0ff>   the push by that opcode whatever the size; PUSHDATA2, PUSHDATA4
//   0x4c01            bytes as they are, pushed by nothing
//   NOP*201           the word 201 times over; (DUP DROP)*20 several words
CScriptSig Script(std::string_view text, std::source_location where = std::source_location::current());
CScriptSig ScriptSig(std::string_view text, std::source_location where = std::source_location::current());

// The script count times over
CScriptSig Repeat(size_t count, const CScriptSig &script);
// The bytes of a script that holds no signature
Bytes Code(const CScriptSig &script);
// "00ff"_hex
Bytes operator""_hex(const char *text, size_t size);

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
    Bip341,
    // BIP342: BIP341's with the leaf
    Tapscript
  };
  Bytes Script;
  EDigest Digest = EDigest::None;
  // Legacy: the script that runs CHECKSIG; BIP143: the P2PKH script of the program, the witness script
  Bytes ScriptCode;
  std::optional<CKey> Key;
  // What Redeem() and WitnessScript() push
  Bytes Redeem;
  Bytes WitnessScript;
  // What LeafScript() and Control() push; the leaf's TapLeaf hash, its signatures commit to it
  Bytes LeafScript;
  Bytes Control;
  Bytes LeafHash;
};

// A leaf of a taproot tree: tapscript under 0xc0, a leaf of the future under another version
struct CTapLeaf {
  Bytes Script;
  uint8_t Version = 0xc0;
};

inline CTapLeaf Leaf(const Bytes &script, uint8_t version = 0xc0) { return {script, version}; }
CTapLeaf Leaf(const CScriptSig &script, uint8_t version = 0xc0);
// TapLeaf hash: a branch of the tree that is this leaf
Bytes LeafHash(const CTapLeaf &leaf);

COutput P2PK(const CKey &key);
COutput P2PKH(const CKey &key);
// Of any data pushed in place of the key
COutput P2PKH(const Bytes &pushed);
COutput P2WPKH(const CKey &key);
COutput P2WPKH(const Bytes &program);
COutput P2SH(const COutput &redeem);
// The script as it is, as the redeem script, as the witness script; signatures sign it
COutput Bare(const CScriptSig &script);
COutput P2SH(const CScriptSig &redeem);
COutput P2WSH(const CScriptSig &witnessScript);
COutput P2TR(const CKey &key);
COutput P2TR(const Bytes &program);
// The output of the internal key committed to a tree that has the leaf, path the hashes beside it from
// the leaf up. Schnorr signatures sign under the leaf, by the internal key unless By names another
COutput P2TR(const CKey &internal, const CTapLeaf &leaf, const std::vector<Bytes> &path = {});
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
  // input: the one checked, a change may move it
  std::function<void(Tx &tx, std::vector<BTC::Proto::CTxOut> &spent, size_t &input)> Apply;
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
// The annex, the last witness element, a byte longer
CChange OtherAnnex();
// The input checked and another change places, their outputs with them
CChange Permute();

// The other inputs spend P2WPKH of K2, the outputs pay P2PKH of K2
struct CTx {
  size_t Inputs = 1;
  size_t Outputs = 1;
  size_t Checked = 0;
  CChange Then = {};
  int32_t Version = 2;
  // Of every input
  uint32_t Sequence = 0xfffffffd;
  uint32_t LockTime = 0;
  // Of the inputs not checked, if another
  std::optional<uint32_t> OtherSequence = {};
  // The script of every output, if not P2PKH of K2
  Bytes OutputScript = {};
};

// A signature apart from a spend, for a script that holds it: of `code` at the input `tx` checks,
// by the key By names. Its digest takes no other script of the spend
Bytes Signature(const CSig &sig, const CTx &tx, COutput::EDigest digest, const Bytes &code);

// A signature picked ahead of its digest and the key it signs a digest under: for a script its own
// digest covers, where a key cannot sign
struct CChosenSig {
  // DER, the hash type after it
  Bytes Signature;
  uint8_t HashType;
  std::array<uint8_t, 32> Nonce, R, S;

  // Compressed
  Bytes KeyFor(const CTx &tx, COutput::EDigest digest, const Bytes &code) const;
};

CChosenSig ChosenSig(uint8_t seed, uint8_t hashType = SIGHASH_ALL, bool highS = false);

// Verdicts

struct CVerdict {
  BTC::EScriptError Error = BTC::EScriptError::Ok;
  bool Interpreted = false;

  CVerdict(BTC::EScriptError error = BTC::EScriptError::Ok, bool interpreted = false) : Error(error), Interpreted(interpreted) {}
  bool operator==(const CVerdict&) const = default;
};

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

// Off the templates: the fast path passes the spend on, the interpreter gives the verdict
inline CVerdict Interpreter(BTC::EScriptError error) { return {error, true}; }
inline CVerdicts Interpreter(CVerdicts verdicts)
{
  for (CVerdict &verdict: verdicts.At)
    verdict.Interpreted = true;
  return verdicts;
}

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

// A vector of Core's script_tests: the verdict under one flag set, in the transaction Core tests
// with - version 1, final sequences, no lock time. The interpreter alone, and the whole check when
// the fast path takes the spend. Then Core's DoTest: a valid spend stays valid with flags taken
// away, an invalid one invalid with flags added
void interpret(const COutput &output, const CScriptSig &scriptSig, uint32_t flags, BTC::EScriptError expected,
               std::source_location where = std::source_location::current());
void interpret(const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, uint32_t flags, BTC::EScriptError expected,
               std::source_location where = std::source_location::current());
void interpret(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, uint32_t flags,
               BTC::EScriptError expected, std::source_location where = std::source_location::current());
// The two scripts as text, in the order Core's vectors have them and they run in: the script sig, then the
// script of the output
void interpret(std::string_view scriptSig, std::string_view scriptPubKey, uint32_t flags, BTC::EScriptError expected,
               std::source_location where = std::source_location::current());

// Every flag there is
inline constexpr uint32_t AllFlags = (1u << 21) - 1;
// Core's STANDARD_SCRIPT_VERIFY_FLAGS
inline constexpr uint32_t StandardFlags =
  BTC::EScriptP2SH | BTC::EScriptStrictEnc | BTC::EScriptDerSig | BTC::EScriptLowS | BTC::EScriptNullDummy |
  BTC::EScriptMinimalData | BTC::EScriptDiscourageUpgradableNops | BTC::EScriptCleanStack |
  BTC::EScriptCheckLockTimeVerify | BTC::EScriptCheckSequenceVerify | BTC::EScriptWitness |
  BTC::EScriptDiscourageUpgradableWitnessProgram | BTC::EScriptMinimalIf | BTC::EScriptNullFail |
  BTC::EScriptWitnessPubKeyType | BTC::EScriptConstScriptCode | BTC::EScriptTaproot |
  BTC::EScriptDiscourageUpgradableTaprootVersion | BTC::EScriptDiscourageOpSuccess |
  BTC::EScriptDiscourageUpgradablePubKeyType;

// A vector of Core's tx_valid, an input of it: valid under every flag but `excluded`, and with any
// more flags taken away; invalid once any of `excluded` is back
void txValid(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, uint32_t excluded,
             std::source_location where = std::source_location::current());
void txValid(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, uint32_t excluded,
             std::source_location where = std::source_location::current());
// A vector of Core's tx_invalid: the error under `flags`, invalid with any flags added, valid once
// any of `flags` is taken away
void txInvalid(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, uint32_t flags,
               BTC::EScriptError expected, std::source_location where = std::source_location::current());
void txInvalid(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, uint32_t flags, BTC::EScriptError expected,
               std::source_location where = std::source_location::current());
// A spend of Core's checkinputs_test: under any flags, valid when none of `failing` is among them
void validUnless(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, uint32_t failing,
                 std::source_location where = std::source_location::current());
void validUnless(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, uint32_t failing,
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
