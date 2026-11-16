#include "spendBuilder.h"
#include "BTC/script.h"
#include <p2putils/xmstream.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>
#include <cstring>
#include <iterator>

namespace SpendTest {

namespace {

using BTC::EScriptError;
using BTC::ESigMode;
using BTC::EVerdict;
using TxOut = BTC::Proto::CTxOut;

// Core's STANDARD_SCRIPT_VERIFY_FLAGS; the coins' own sets come with §14.3
constexpr uint32_t StandardFlags =
  BTC::EScriptP2SH | BTC::EScriptStrictEnc | BTC::EScriptDerSig | BTC::EScriptLowS | BTC::EScriptNullDummy |
  BTC::EScriptMinimalData | BTC::EScriptDiscourageUpgradableNops | BTC::EScriptCleanStack |
  BTC::EScriptCheckLockTimeVerify | BTC::EScriptCheckSequenceVerify | BTC::EScriptWitness |
  BTC::EScriptDiscourageUpgradableWitnessProgram | BTC::EScriptMinimalIf | BTC::EScriptNullFail |
  BTC::EScriptWitnessPubKeyType | BTC::EScriptConstScriptCode | BTC::EScriptTaproot |
  BTC::EScriptDiscourageUpgradableTaprootVersion | BTC::EScriptDiscourageOpSuccess |
  BTC::EScriptDiscourageUpgradablePubKeyType;

// Core's MANDATORY_SCRIPT_VERIFY_FLAGS
constexpr uint32_t ConsensusFlags =
  BTC::EScriptP2SH | BTC::EScriptDerSig | BTC::EScriptNullDummy | BTC::EScriptCheckLockTimeVerify |
  BTC::EScriptCheckSequenceVerify | BTC::EScriptWitness | BTC::EScriptTaproot;

// Core gives a block P2SH, WITNESS and TAPROOT from genesis, the rest by height
constexpr uint32_t SoftForkFlags = BTC::EScriptP2SH | BTC::EScriptWitness | BTC::EScriptTaproot;

// In EFlagSet's order
const std::pair<const char*, uint32_t> FlagSets[] = {
  {"none", BTC::EScriptNone},
  {"p2sh", BTC::EScriptP2SH},
  {"p2sh+witness", BTC::EScriptP2SH | BTC::EScriptWitness},
  {"pre-bip66", SoftForkFlags},
  {"consensus", ConsensusFlags},
  {"standard", StandardFlags}
};

// EScriptError's names, in its order
const char *const ErrorNames[] = {
  "Ok", "UnknownError", "EvalFalse", "OpReturn",
  "ScriptSize", "PushSize", "OpCount", "StackSize", "SigCount", "PubKeyCount",
  "Verify", "EqualVerify", "CheckMultisigVerify", "CheckSigVerify", "NumEqualVerify",
  "BadOpcode", "DisabledOpcode", "InvalidStackOperation", "InvalidAltstackOperation", "UnbalancedConditional",
  "NegativeLocktime", "UnsatisfiedLocktime",
  "SigHashtype", "SigDer", "MinimalData", "SigPushOnly", "SigHighS", "SigNullDummy", "PubKeyType", "CleanStack",
  "MinimalIf", "SigNullFail",
  "DiscourageUpgradableNops", "DiscourageUpgradableWitnessProgram", "DiscourageUpgradableTaprootVersion",
  "DiscourageOpSuccess", "DiscourageUpgradablePubKeyType",
  "WitnessProgramWrongLength", "WitnessProgramWitnessEmpty", "WitnessProgramMismatch", "WitnessMalleated",
  "WitnessMalleatedP2SH", "WitnessUnexpected", "WitnessPubKeyType",
  "SchnorrSigSize", "SchnorrSigHashtype", "SchnorrSig", "TaprootWrongControlSize", "TapscriptValidationWeight",
  "TapscriptCheckMultisig", "TapscriptMinimalIf",
  "OpCodeSeparator", "SigFindAndDelete"
};
static_assert(std::size(ErrorNames) == static_cast<size_t>(EScriptError::SigFindAndDelete) + 1);

// Names the transaction in the jobs it makes
constexpr uint32_t TxOrdinal = 7;

template<typename... Ts>
struct COverloaded : Ts... {
  using Ts::operator()...;
};

secp256k1_context *context()
{
  static secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
  return ctx;
}

void secretKey(const CKey &key, uint8_t (&secret)[32])
{
  memset(secret, 0, sizeof(secret));
  secret[31] = key.Secret;
}

void append(Bytes &target, const Bytes &data)
{
  target.insert(target.end(), data.begin(), data.end());
}

void assign(xvector<uint8_t> &target, const Bytes &data)
{
  target.resize(data.size());
  if (!data.empty())
    memcpy(target.data(), data.data(), data.size());
}

// The minimal push: direct up to 75 bytes, OP_0 for nothing
Bytes push(const Bytes &data)
{
  Bytes result;
  if (data.size() <= 75) {
    result.push_back(static_cast<uint8_t>(data.size()));
  } else if (data.size() <= 255) {
    result = {0x4c, static_cast<uint8_t>(data.size())};
  } else {
    result = {0x4d, static_cast<uint8_t>(data.size()), static_cast<uint8_t>(data.size() >> 8)};
  }
  append(result, data);
  return result;
}

Bytes hash160(const Bytes &data)
{
  BaseBlob<160> hash = BTC::sha256FollowRipemd160(data.data(), data.size());
  return Bytes(hash.begin(), hash.begin() + 20);
}

TxOut txOut(int64_t value, const Bytes &script)
{
  TxOut out;
  out.Value = value;
  assign(out.PkScript, script);
  return out;
}

// The records the database would give for the spent outputs
struct CRecords {
  std::vector<Bytes> Data;
  std::vector<const BTC::Script::CUnspentOutputInfo*> Pointers;

  explicit CRecords(const std::vector<TxOut> &spent) : Data(spent.size()) {
    for (size_t i = 0; i < spent.size(); i++) {
      xmstream stream;
      BTC::Script::parseTransactionOutput(spent[i], stream);
      const uint8_t *record = static_cast<const uint8_t*>(stream.data());
      Data[i].assign(record, record + stream.sizeOf());
      Pointers.push_back(reinterpret_cast<const BTC::Script::CUnspentOutputInfo*>(Data[i].data()));
    }
  }
};

// The transaction a spend is built in
struct CBuilt {
  Tx Transaction;
  std::vector<TxOut> Spent;
  size_t Input = 0;
  // Which signatures, for the trace
  std::string Note;
};

TxOut otherSpent(size_t index)
{
  return txOut(200000 + 1000 * static_cast<int64_t>(index), P2WPKH(K2).Script);
}

BTC::Proto::CTxIn txIn(size_t index)
{
  BTC::Proto::CTxIn in;
  memset(in.PreviousOutputHash.begin(), static_cast<int>(0x11 * (index + 1)), in.PreviousOutputHash.size());
  in.PreviousOutputIndex = static_cast<uint32_t>(index);
  in.Sequence = 0xfffffffd;
  return in;
}

TxOut paid(size_t index)
{
  return txOut(50000 + 1000 * static_cast<int64_t>(index), P2PKH(K2).Script);
}

size_t other(size_t input)
{
  return input == 0 ? 1 : 0;
}

// DER

// A 32-byte big endian value as a DER integer: no zeros ahead, one when the top bit is set
Bytes derInteger(const uint8_t *value)
{
  size_t start = 0;
  while (start < 31 && value[start] == 0)
    start++;
  Bytes result;
  if (value[start] & 0x80)
    result.push_back(0x00);
  result.insert(result.end(), value + start, value + 32);
  return result;
}

void dropZero(Bytes &integer, const char *name)
{
  if (integer.size() < 2 || integer[0] != 0x00 || !(integer[1] & 0x80)) {
    ADD_FAILURE() << "no zero ahead of " << name << " to drop";
    return;
  }
  integer.erase(integer.begin());
}

Bytes der(const CSig &sig, const uint8_t (&compact)[64])
{
  Bytes r = derInteger(compact);
  Bytes s = derInteger(compact + 32);
  if (sig.NoZeroR)
    dropZero(r, "R");
  if (sig.NoZeroS)
    dropZero(s, "S");
  if (sig.OneAheadR) {
    r.assign(compact, compact + 32);
    r.insert(r.begin(), 0x01);
  }
  if (sig.OneAheadS) {
    s.assign(compact + 32, compact + 64);
    s.insert(s.begin(), 0x01);
  }
  r.insert(r.begin(), sig.ZerosAheadR, 0x00);
  if (sig.ZeroAheadS)
    s.insert(s.begin(), 0x00);
  if (sig.NoR)
    r.clear();
  if (sig.NoS)
    s.clear();

  Bytes body{sig.IntegerTagR};
  if (sig.LengthBytesR) {
    body.push_back(static_cast<uint8_t>(0x80 | sig.LengthBytesR));
    body.insert(body.end(), sig.LengthBytesR - 1, 0x00);
  }
  body.push_back(static_cast<uint8_t>(r.size()));
  append(body, r);
  body.push_back(sig.IntegerTagS);
  body.push_back(static_cast<uint8_t>(s.size()));
  append(body, s);
  if (sig.ByteAfterS)
    body.push_back(0x01);

  Bytes result{sig.SequenceTag};
  if (sig.LongTotalLength)
    result.push_back(0x81);
  result.push_back(static_cast<uint8_t>(body.size() + (sig.TotalLengthPlusOne ? 1 : 0)));
  append(result, body);
  return result;
}

// Signatures

char hex(unsigned digit)
{
  return "0123456789abcdef"[digit & 0xf];
}

void noteSignature(CBuilt &b, const char *kind, unsigned hashType)
{
  b.Note += b.Note.empty() ? "" : ", ";
  b.Note += std::string(kind) + " 0x" + hex(hashType >> 4) + hex(hashType);
}

// The digest a signature here signs is put together from the texts: Core's first SignatureHash, a
// copy of the transaction changed and serialized, BIP143 and BIP341 field by field. It shares no line
// with BTC/sighash.h, which verifies: a byte wrong there fails the spend

void putLe32(Bytes &target, uint32_t value)
{
  for (unsigned i = 0; i < 4; i++)
    target.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void putLe64(Bytes &target, uint64_t value)
{
  for (unsigned i = 0; i < 8; i++)
    target.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

// CompactSize of one byte: no script here is longer
void putSized(Bytes &target, const Bytes &data)
{
  EXPECT_LT(data.size(), 0xfdu);
  target.push_back(static_cast<uint8_t>(data.size()));
  append(target, data);
}

void putOutpoint(Bytes &target, const BTC::Proto::CTxIn &in)
{
  target.insert(target.end(), in.PreviousOutputHash.begin(), in.PreviousOutputHash.begin() + 32);
  putLe32(target, in.PreviousOutputIndex);
}

Bytes bytesOf(const xvector<uint8_t> &data)
{
  return Bytes(data.data(), data.data() + data.size());
}

void putOutput(Bytes &target, const TxOut &out)
{
  putLe64(target, static_cast<uint64_t>(out.Value));
  putSized(target, bytesOf(out.PkScript));
}

Bytes sha256Of(const Bytes &data)
{
  Bytes hash(32);
  ::sha256(data.data(), data.size(), hash.data());
  return hash;
}

Bytes sha256dOf(const Bytes &data)
{
  return sha256Of(sha256Of(data));
}

BaseBlob<256> blobOf(const Bytes &hash)
{
  BaseBlob<256> result;
  memcpy(result.begin(), hash.data(), 32);
  return result;
}

// Core's first SignatureHash: the script code for the script sig of the input and no other script,
// the inputs and outputs the hash type leaves, serialized as for a txid, the type after it
BaseBlob<256> legacyDigest(const Tx &tx, size_t input, const Bytes &scriptCode, uint8_t hashType)
{
  const unsigned type = hashType & 0x1f;
  const bool anyoneCanPay = (hashType & SIGHASH_ANYONECANPAY) != 0;
  // Core's old bug kept by consensus: the digest is 1
  if (type == SIGHASH_SINGLE && input >= tx.TxOut.size())
    return blobOf(Concat({0x01}, Zeros(31)));

  Tx copy;
  copy.Version = tx.Version;
  copy.LockTime = tx.LockTime;
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    if (anyoneCanPay && i != input)
      continue;
    BTC::Proto::CTxIn in;
    in.PreviousOutputHash = tx.TxIn[i].PreviousOutputHash;
    in.PreviousOutputIndex = tx.TxIn[i].PreviousOutputIndex;
    in.Sequence = tx.TxIn[i].Sequence;
    if (i == input)
      assign(in.ScriptSig, scriptCode);
    else if (type == SIGHASH_NONE || type == SIGHASH_SINGLE)
      in.Sequence = 0;
    copy.TxIn.emplace_back(std::move(in));
  }
  if (type == SIGHASH_SINGLE) {
    // Up to the output of the input; the ones before it are CTxOut(), -1 and no script
    for (size_t i = 0; i < input; i++)
      copy.TxOut.emplace_back(txOut(-1, {}));
    copy.TxOut.emplace_back(txOut(tx.TxOut[input].Value, bytesOf(tx.TxOut[input].PkScript)));
  } else if (type != SIGHASH_NONE) {
    for (const auto &out: tx.TxOut)
      copy.TxOut.emplace_back(txOut(out.Value, bytesOf(out.PkScript)));
  }

  xmstream stream;
  BTC::Io<Tx>::serialize(stream, copy, false);
  Bytes message(stream.data<uint8_t>(), stream.data<uint8_t>() + stream.sizeOf());
  putLe32(message, hashType);
  return blobOf(sha256dOf(message));
}

// BIP143, its ten items in their order
BaseBlob<256> bip143Digest(const Tx &tx, size_t input, const Bytes &scriptCode, int64_t amount, uint8_t hashType)
{
  const unsigned type = hashType & 0x1f;
  const bool anyoneCanPay = (hashType & SIGHASH_ANYONECANPAY) != 0;
  const bool allOutputs = type != SIGHASH_NONE && type != SIGHASH_SINGLE;
  const bool ownOutput = type == SIGHASH_SINGLE && input < tx.TxOut.size();
  Bytes prevouts, sequences, outputs;
  for (const auto &in: tx.TxIn) {
    putOutpoint(prevouts, in);
    putLe32(sequences, in.Sequence);
  }
  if (allOutputs) {
    for (const auto &out: tx.TxOut)
      putOutput(outputs, out);
  } else if (ownOutput) {
    putOutput(outputs, tx.TxOut[input]);
  }

  const BTC::Proto::CTxIn &in = tx.TxIn[input];
  Bytes message;
  putLe32(message, static_cast<uint32_t>(tx.Version));
  append(message, anyoneCanPay ? Zeros(32) : sha256dOf(prevouts));
  append(message, anyoneCanPay || !allOutputs ? Zeros(32) : sha256dOf(sequences));
  putOutpoint(message, in);
  putSized(message, scriptCode);
  putLe64(message, static_cast<uint64_t>(amount));
  putLe32(message, in.Sequence);
  append(message, allOutputs || ownOutput ? sha256dOf(outputs) : Zeros(32));
  putLe32(message, tx.LockTime);
  putLe32(message, hashType);
  return blobOf(sha256dOf(message));
}

// BIP341's SigMsg of the key path with no annex, the epoch ahead of it, under the TapSighash tag.
// The amounts and scripts are those of the outputs themselves, not of their records
BaseBlob<256> bip341Digest(const Tx &tx, const std::vector<TxOut> &spent, size_t input, uint8_t hashType)
{
  const bool anyoneCanPay = (hashType & SIGHASH_ANYONECANPAY) != 0;
  const unsigned outputType = hashType & 0x03;
  const BTC::Proto::CTxIn &in = tx.TxIn[input];
  Bytes message{0x00, hashType};
  putLe32(message, static_cast<uint32_t>(tx.Version));
  putLe32(message, tx.LockTime);
  if (!anyoneCanPay) {
    Bytes prevouts, amounts, scripts, sequences;
    for (size_t i = 0; i < tx.TxIn.size(); i++) {
      putOutpoint(prevouts, tx.TxIn[i]);
      putLe64(amounts, static_cast<uint64_t>(spent[i].Value));
      putSized(scripts, bytesOf(spent[i].PkScript));
      putLe32(sequences, tx.TxIn[i].Sequence);
    }
    append(message, sha256Of(prevouts));
    append(message, sha256Of(amounts));
    append(message, sha256Of(scripts));
    append(message, sha256Of(sequences));
  }
  if (outputType != SIGHASH_NONE && outputType != SIGHASH_SINGLE) {
    Bytes outputs;
    for (const auto &out: tx.TxOut)
      putOutput(outputs, out);
    append(message, sha256Of(outputs));
  }
  // The spend type
  message.push_back(0x00);
  if (anyoneCanPay) {
    putOutpoint(message, in);
    putOutput(message, spent[input]);
    putLe32(message, in.Sequence);
  } else {
    putLe32(message, static_cast<uint32_t>(input));
  }
  if (outputType == SIGHASH_SINGLE) {
    Bytes own;
    putOutput(own, tx.TxOut[input]);
    append(message, sha256Of(own));
  }

  const Bytes tag = sha256Of({'T', 'a', 'p', 'S', 'i', 'g', 'h', 'a', 's', 'h'});
  return blobOf(sha256Of(Concat(Concat(tag, tag), message)));
}

BaseBlob<256> ecdsaDigest(const CBuilt &b, const COutput &output, uint8_t hashType)
{
  switch (output.Digest) {
    case COutput::EDigest::Legacy :
      return legacyDigest(b.Transaction, b.Input, output.ScriptCode, hashType);
    case COutput::EDigest::Bip143 :
      return bip143Digest(b.Transaction, b.Input, output.ScriptCode, b.Spent[b.Input].Value, hashType);
    default :
      return BaseBlob<256>();
  }
}

BaseBlob<256> schnorrDigest(const CBuilt &b, const COutput &output, uint8_t hashType)
{
  // An undefined hash type, SIGHASH_SINGLE without its output: refused ahead of the digest
  const bool defined = hashType <= SIGHASH_SINGLE || (hashType >= 0x81 && hashType <= 0x83);
  const bool single = (hashType & 0x03) == SIGHASH_SINGLE;
  if (output.Digest != COutput::EDigest::Bip341 || !defined || (single && b.Input >= b.Transaction.TxOut.size()))
    return BaseBlob<256>();
  return bip341Digest(b.Transaction, b.Spent, b.Input, hashType);
}

std::optional<CKey> signer(const std::optional<CKey> &own, const COutput &output)
{
  if (own)
    return own;
  if (!output.Key)
    ADD_FAILURE() << "a signature with no key: the output has none, By names one";
  return output.Key;
}

Bytes ecdsa(CBuilt &b, const COutput &output, const CSig &sig)
{
  std::optional<CKey> key = signer(sig.Signer, output);
  if (!key)
    return {};
  noteSignature(b, "Sig", sig.HashType);
  const BaseBlob<256> digest = ecdsaDigest(b, output, sig.HashType);
  uint8_t secret[32];
  secretKey(*key, secret);
  uint8_t compact[64];
  // RFC6979, then extra data until R comes high
  for (uint32_t attempt = 0;; attempt++) {
    uint8_t extra[32] = {};
    memcpy(extra, &attempt, sizeof(attempt));
    secp256k1_ecdsa_signature signature;
    EXPECT_TRUE(secp256k1_ecdsa_sign(context(), &signature, digest.begin(), secret, nullptr, attempt ? extra : nullptr));
    secp256k1_ecdsa_signature_serialize_compact(context(), compact, &signature);
    if (!sig.GrindHighR || (compact[0] & 0x80))
      break;
  }
  if (sig.NegateS) {
    EXPECT_TRUE(secp256k1_ec_seckey_negate(context(), compact + 32));
  }
  if (sig.DamageR)
    compact[31] ^= 1;
  Bytes result = der(sig, compact);
  result.push_back(sig.WrittenType.value_or(sig.HashType));
  return result;
}

Bytes schnorr(CBuilt &b, const COutput &output, const CSchnorr &sig)
{
  std::optional<CKey> key = signer(sig.Signer, output);
  if (!key)
    return {};
  const uint8_t hashType = sig.HashType.value_or(SIGHASH_DEFAULT);
  noteSignature(b, "Schnorr", hashType);
  const BaseBlob<256> digest = schnorrDigest(b, output, hashType);
  uint8_t secret[32];
  secretKey(*key, secret);
  secp256k1_keypair keypair;
  EXPECT_TRUE(secp256k1_keypair_create(context(), &keypair, secret));
  uint8_t data[64];
  EXPECT_TRUE(secp256k1_schnorrsig_sign32(context(), data, digest.begin(), &keypair, nullptr));
  if (sig.DamageR)
    data[31] ^= 1;
  Bytes result(data, data + 64);
  if (sig.WrittenType)
    result.push_back(*sig.WrittenType);
  else if (sig.HashType)
    result.push_back(*sig.HashType);
  return result;
}

Bytes element(CBuilt &b, const COutput &output, const CElement &item)
{
  return std::visit(COverloaded{
    [](const Bytes &data) { return data; },
    [](const CKey &key) { return key.pubKey(); },
    [&](const CSig &sig) { return ecdsa(b, output, sig); },
    [&](const CSchnorr &sig) { return schnorr(b, output, sig); }
  }, item);
}

// False and a failure if the shape names no input to check
bool make(const CTx &shape, const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, CBuilt &b)
{
  if (shape.Checked >= shape.Inputs) {
    ADD_FAILURE() << "the input checked is out of the transaction";
    return false;
  }

  b.Input = shape.Checked;
  Tx &tx = b.Transaction;
  tx.Version = 2;
  tx.LockTime = 0;
  for (size_t i = 0; i < shape.Inputs; i++) {
    tx.TxIn.emplace_back(txIn(i));
    b.Spent.push_back(i == shape.Checked ? txOut(100000, output.Script) : otherSpent(i));
  }
  for (size_t i = 0; i < shape.Outputs; i++)
    tx.TxOut.emplace_back(paid(i));

  // The digests take neither the script sig nor the witness of the input
  Bytes script;
  for (const auto &item: scriptSig.Items) {
    std::visit(COverloaded{
      [&](const CElement &data) { append(script, push(element(b, output, data))); },
      [&](const CPushData1 &pushData) {
        Bytes data = element(b, output, pushData.Data);
        script.push_back(0x4c);
        script.push_back(static_cast<uint8_t>(data.size()));
        append(script, data);
      },
      [&](const COp &op) { script.push_back(op.Code); },
      [&](const CRaw &raw) { append(script, raw.Data); }
    }, item);
  }
  BTC::Proto::CTxIn &in = tx.TxIn[b.Input];
  assign(in.ScriptSig, script);
  in.WitnessStack.resize(witness.Elements.size());
  for (size_t i = 0; i < witness.Elements.size(); i++)
    assign(in.WitnessStack[i], element(b, output, witness.Elements[i]));

  if (shape.Then.Apply) {
    shape.Then.Apply(tx, b.Spent, b.Input);
    b.Note = "after " + shape.Then.Name + (b.Note.empty() ? "" : ", ") + b.Note;
  }
  return true;
}

// Now against the verdicts; Defer the same at once, or one job the batch decides the same; Trust is
// Defer with every signature valid
void checkSpend(const CBuilt &b, const CVerdicts &verdicts, std::source_location where)
{
  const Tx &tx = b.Transaction;
  const size_t input = b.Input;
  testing::ScopedTrace trace(where.file_name(), static_cast<int>(where.line()), b.Note);
  CRecords records(b.Spent);
  BTC::CTxChecker<Tx> checker(tx, records.Pointers, TxOrdinal);
  auto verdictOf = [](const BTC::CInputVerdict &verdict) {
    return CVerdict(verdict.Result == EVerdict::Valid ? EScriptError::Ok : verdict.Error, verdict.Interpreted);
  };

  for (size_t i = 0; i < std::size(FlagSets); i++) {
    const auto &[name, flags] = FlagSets[i];
    const CVerdict &expected = verdicts.At[i];
    testing::ScopedTrace flagsTrace(where.file_name(), static_cast<int>(where.line()), std::string("flags ") + name);

    // Now: libsecp256k1 at once
    EXPECT_EQ(verdictOf(BTC::verifyInput(checker, input, flags, ESigMode::Now, nullptr)), expected);

    // Defer: every soft fork on, a valid spend of the templates is a signature's
    BTC::CSigBatch batch;
    const BTC::CInputVerdict deferred = BTC::verifyInput(checker, input, flags, ESigMode::Defer, &batch);
    CVerdict deferredVerdict = verdictOf(deferred);
    if (deferred.Result == EVerdict::Pending) {
      ASSERT_EQ(batch.size(), 1u);
      EXPECT_EQ(batch[0].Tx, TxOrdinal);
      EXPECT_EQ(batch[0].Input, input);
      std::vector<uint32_t> failed;
      batch.verify(failed);
      deferredVerdict = failed.empty() ? CVerdict(EScriptError::Ok) : CVerdict(batch[0].Error);
    } else {
      EXPECT_EQ(batch.size(), 0u);
      EXPECT_FALSE((flags & SoftForkFlags) == SoftForkFlags && expected == CVerdict(EScriptError::Ok)) << "no signature job";
    }
    EXPECT_EQ(deferredVerdict, expected) << "Defer";

    // Trust: Defer with every signature valid
    EXPECT_EQ(verdictOf(BTC::verifyInput(checker, input, flags, ESigMode::Trust, nullptr)),
              deferred.Result == EVerdict::Pending ? CVerdict(EScriptError::Ok) : deferredVerdict) << "Trust";
  }
}

void build(const CTx &shape, const COutput &output, const CScriptSig &scriptSig, const CWitness &witness,
           const CVerdicts &verdicts, std::source_location where)
{
  CBuilt b;
  bool made;
  {
    testing::ScopedTrace trace(where.file_name(), static_cast<int>(where.line()), "building the spend");
    made = make(shape, output, scriptSig, witness, b);
  }
  if (made)
    checkSpend(b, verdicts, where);
}

}

// Keys

Bytes CKey::pubKey() const
{
  uint8_t secret[32];
  secretKey(*this, secret);
  secp256k1_pubkey key;
  EXPECT_TRUE(secp256k1_ec_pubkey_create(context(), &key, secret));
  uint8_t data[65];
  size_t size = sizeof(data);
  secp256k1_ec_pubkey_serialize(context(), data, &size, &key,
                                Form == EForm::Compressed ? SECP256K1_EC_COMPRESSED : SECP256K1_EC_UNCOMPRESSED);
  if (Form == EForm::Hybrid)
    data[0] = static_cast<uint8_t>(0x06 | (data[64] & 1));
  return Bytes(data, data + size);
}

Bytes CKey::xOnly() const
{
  uint8_t secret[32];
  secretKey(*this, secret);
  secp256k1_keypair keypair;
  EXPECT_TRUE(secp256k1_keypair_create(context(), &keypair, secret));
  secp256k1_xonly_pubkey key;
  secp256k1_keypair_xonly_pub(context(), &key, nullptr, &keypair);
  uint8_t data[32];
  secp256k1_xonly_pubkey_serialize(context(), data, &key);
  return Bytes(data, data + 32);
}

// Outputs

COutput P2PK(const CKey &key)
{
  COutput output;
  output.Script = push(key.pubKey());
  output.Script.push_back(0xac);
  output.Digest = COutput::EDigest::Legacy;
  output.ScriptCode = output.Script;
  output.Key = key;
  return output;
}

COutput P2PKH(const Bytes &pushed)
{
  COutput output;
  output.Script = {0x76, 0xa9, 0x14};
  append(output.Script, hash160(pushed));
  append(output.Script, {0x88, 0xac});
  output.Digest = COutput::EDigest::Legacy;
  output.ScriptCode = output.Script;
  return output;
}

COutput P2PKH(const CKey &key)
{
  COutput output = P2PKH(key.pubKey());
  output.Key = key;
  return output;
}

COutput P2WPKH(const Bytes &program)
{
  COutput output;
  output.Script = {0x00, 0x14};
  append(output.Script, program);
  output.Digest = COutput::EDigest::Bip143;
  output.ScriptCode = {0x76, 0xa9, 0x14};
  append(output.ScriptCode, program);
  append(output.ScriptCode, {0x88, 0xac});
  return output;
}

COutput P2WPKH(const CKey &key)
{
  COutput output = P2WPKH(hash160(key.pubKey()));
  output.Key = key;
  return output;
}

COutput P2SH(const COutput &redeem)
{
  COutput output = redeem;
  output.Script = {0xa9, 0x14};
  append(output.Script, hash160(redeem.Script));
  output.Script.push_back(0x87);
  return output;
}

COutput P2TR(const Bytes &program)
{
  COutput output = WitnessProgram(1, program);
  output.Digest = COutput::EDigest::Bip341;
  return output;
}

COutput P2TR(const CKey &key)
{
  COutput output = P2TR(key.xOnly());
  output.Key = key;
  return output;
}

COutput P2WSH(const Bytes &program)
{
  return WitnessProgram(0, program);
}

COutput WitnessProgram(uint8_t version, const Bytes &program)
{
  return Output(Concat({static_cast<uint8_t>(version == 0 ? 0x00 : 0x50 + version)}, push(program)));
}

COutput Anchor()
{
  return Output({0x51, 0x02, 0x4e, 0x73});
}

COutput Output(const Bytes &script)
{
  COutput output;
  output.Script = script;
  return output;
}

Bytes Hash160(const CKey &key)
{
  return hash160(key.pubKey());
}

Bytes Redeem(const COutput &output)
{
  return output.Script;
}

Bytes Annex()
{
  return {0x50, 0x00};
}

Bytes Zeros(size_t size)
{
  return Bytes(size, 0x00);
}

Bytes NegativeZero(size_t size)
{
  Bytes result(size, 0x00);
  result.back() = 0x80;
  return result;
}

Bytes Concat(Bytes left, const Bytes &right)
{
  append(left, right);
  return left;
}

// Changes

CChange OutputValue(size_t index)
{
  return {"OutputValue(" + std::to_string(index) + ")", [index](Tx &tx, std::vector<TxOut>&, size_t) { tx.TxOut[index].Value ^= 1; }};
}

CChange OutputScript(size_t index)
{
  return {"OutputScript(" + std::to_string(index) + ")", [index](Tx &tx, std::vector<TxOut>&, size_t) { tx.TxOut[index].PkScript.back() ^= 1; }};
}

CChange AddOutput()
{
  return {"AddOutput", [](Tx &tx, std::vector<TxOut>&, size_t) { tx.TxOut.emplace_back(paid(tx.TxOut.size())); }};
}

CChange SpentAmount()
{
  return {"SpentAmount", [](Tx&, std::vector<TxOut> &spent, size_t input) { spent[input].Value += 1; }};
}

CChange OtherAmount()
{
  return {"OtherAmount", [](Tx&, std::vector<TxOut> &spent, size_t input) { spent.at(other(input)).Value += 1; }};
}

CChange OtherScript()
{
  return {"OtherScript", [](Tx&, std::vector<TxOut> &spent, size_t input) { spent.at(other(input)).PkScript.back() ^= 1; }};
}

CChange Sequence()
{
  return {"Sequence", [](Tx &tx, std::vector<TxOut>&, size_t input) { tx.TxIn[input].Sequence ^= 1; }};
}

CChange OtherSequence()
{
  return {"OtherSequence", [](Tx &tx, std::vector<TxOut>&, size_t input) { tx.TxIn[other(input)].Sequence ^= 1; }};
}

CChange Outpoint()
{
  return {"Outpoint", [](Tx &tx, std::vector<TxOut>&, size_t input) { tx.TxIn[input].PreviousOutputIndex ^= 1; }};
}

CChange OtherOutpoint()
{
  return {"OtherOutpoint", [](Tx &tx, std::vector<TxOut>&, size_t input) { tx.TxIn[other(input)].PreviousOutputIndex ^= 1; }};
}

CChange OtherScriptSig()
{
  return {"OtherScriptSig", [](Tx &tx, std::vector<TxOut>&, size_t input) { assign(tx.TxIn[other(input)].ScriptSig, {0x51}); }};
}

CChange AddInput()
{
  return {"AddInput", [](Tx &tx, std::vector<TxOut> &spent, size_t) {
    tx.TxIn.emplace_back(txIn(tx.TxIn.size()));
    spent.push_back(otherSpent(spent.size()));
  }};
}

CChange LockTime()
{
  return {"LockTime", [](Tx &tx, std::vector<TxOut>&, size_t) { tx.LockTime ^= 1; }};
}

CChange Version()
{
  return {"Version", [](Tx &tx, std::vector<TxOut>&, size_t) { tx.Version ^= 1; }};
}

// Verdicts

void PrintTo(const CVerdict &verdict, std::ostream *os)
{
  if (verdict.Interpreted) {
    *os << "Interpreter";
    if (verdict.Error != EScriptError::UnknownError)
      *os << " " << ErrorNames[static_cast<size_t>(verdict.Error)];
    return;
  }
  *os << ErrorNames[static_cast<size_t>(verdict.Error)];
}

CVerdicts operator|(CVerdicts verdicts, const CFrom &from)
{
  for (size_t i = static_cast<size_t>(from.From); i < verdicts.At.size(); i++)
    verdicts.At[i] = from.Verdict;
  return verdicts;
}

// Checks

void spend(const COutput &output, const CScriptSig &scriptSig, const CVerdicts &verdicts, std::source_location where)
{
  build(CTx(), output, scriptSig, CWitness(), verdicts, where);
}

void spend(const COutput &output, const CWitness &witness, const CVerdicts &verdicts, std::source_location where)
{
  build(CTx(), output, CScriptSig(), witness, verdicts, where);
}

void spend(const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, const CVerdicts &verdicts,
           std::source_location where)
{
  build(CTx(), output, scriptSig, witness, verdicts, where);
}

void spend(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, const CVerdicts &verdicts, std::source_location where)
{
  build(tx, output, scriptSig, CWitness(), verdicts, where);
}

void spend(const CTx &tx, const COutput &output, const CWitness &witness, const CVerdicts &verdicts, std::source_location where)
{
  build(tx, output, CScriptSig(), witness, verdicts, where);
}

void spend(const CTx &tx, const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, const CVerdicts &verdicts,
           std::source_location where)
{
  build(tx, output, scriptSig, witness, verdicts, where);
}

// Batch

void CBatch::add(const COutput &output, const CScriptSig &scriptSig, const CWitness &witness, const CTx &tx,
                 std::source_location where)
{
  testing::ScopedTrace trace(where.file_name(), static_cast<int>(where.line()), "batch");
  CBuilt b;
  if (!make(tx, output, scriptSig, witness, b))
    return;
  const uint32_t position = static_cast<uint32_t>(Jobs_.size());
  CRecords records(b.Spent);
  BTC::CTxChecker<Tx> checker(b.Transaction, records.Pointers, position);
  const BTC::CInputVerdict verdict = BTC::verifyInput(checker, b.Input, ConsensusFlags, ESigMode::Defer, &Jobs_);
  EXPECT_TRUE(verdict.Result == EVerdict::Pending) << "not a spend that waits for its signature alone";
  ASSERT_EQ(Jobs_.size(), position + 1u);
  EXPECT_EQ(Jobs_[position].Tx, position);
  EXPECT_EQ(Jobs_[position].Input, b.Input);
}

std::vector<uint32_t> CBatch::failed() const
{
  std::vector<uint32_t> result;
  Jobs_.verify(result);
  return result;
}

}
