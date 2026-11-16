#pragma once

// Core's signature hashes (script/interpreter.cpp): legacy SignatureHash, BIP143, BIP341 key path.
// A template over the transaction: the coins' types share the fields these read, LTC's MWEB part is
// in none of the digests

#include "hash.h"
#include "script.h"
#include <span>

namespace BTC {

enum ESighash : uint8_t {
  ESighashDefault = 0,
  ESighashAll = 1,
  ESighashNone = 2,
  ESighashSingle = 3,
  ESighashAnyoneCanPay = 0x80
};

class CSha256Writer {
public:
  CSha256Writer() { sha256Init(&Ctx_); }

  void write(const void *data, size_t size) { sha256Update(&Ctx_, data, size); }
  void u8(uint8_t value) { write(&value, 1); }
  void le32(uint32_t value) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; i++)
      bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    write(bytes, sizeof(bytes));
  }
  void le64(uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; i++)
      bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    write(bytes, sizeof(bytes));
  }
  void varSize(uint64_t value) {
    if (value < 0xFD) {
      u8(static_cast<uint8_t>(value));
    } else if (value <= 0xFFFF) {
      u8(0xFD);
      write16(static_cast<uint16_t>(value));
    } else if (value <= 0xFFFFFFFF) {
      u8(0xFE);
      le32(static_cast<uint32_t>(value));
    } else {
      u8(0xFF);
      le64(value);
    }
  }
  void script(const uint8_t *data, size_t size) {
    varSize(size);
    write(data, size);
  }
  void blob(const BaseBlob<256> &value) { write(value.begin(), value.size()); }
  template<typename OutTy>
  void output(const OutTy &out) {
    le64(static_cast<uint64_t>(out.Value));
    script(out.PkScript.data(), out.PkScript.size());
  }
  template<typename InTy>
  void outpoint(const InTy &in) {
    write(in.PreviousOutputHash.begin(), in.PreviousOutputHash.size());
    le32(in.PreviousOutputIndex);
  }

  BaseBlob<256> sha256() {
    BaseBlob<256> result;
    sha256Final(&Ctx_, result.begin());
    return result;
  }
  BaseBlob<256> sha256d() {
    BaseBlob<256> first = sha256();
    BaseBlob<256> result;
    ::sha256(first.begin(), first.size(), result.begin());
    return result;
  }

  // BIP340's tagged hash: SHA256(tag) twice ahead of the message
  static CSha256Writer tagged(const BaseBlob<256> &tagHash) {
    CSha256Writer writer;
    writer.blob(tagHash);
    writer.blob(tagHash);
    return writer;
  }

private:
  void write16(uint16_t value) {
    uint8_t bytes[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
    write(bytes, sizeof(bytes));
  }

  CCtxSha256 Ctx_;
};

// Core's PrecomputedTransactionData: what every input of a transaction shares
struct CTxDigests {
  // BIP341's single SHA256; BIP143 takes one more SHA256 of each
  BaseBlob<256> Prevouts;
  BaseBlob<256> Sequences;
  BaseBlob<256> Outputs;
  BaseBlob<256> HashPrevouts;
  BaseBlob<256> HashSequence;
  BaseBlob<256> HashOutputs;
  // BIP341 only: of the outputs the inputs spend
  BaseBlob<256> SpentAmounts;
  BaseBlob<256> SpentScripts;
};

template<typename TxTy>
void txDigests(const TxTy &tx, CTxDigests &digests)
{
  CSha256Writer prevouts;
  CSha256Writer sequences;
  CSha256Writer outputs;
  for (const auto &in: tx.TxIn) {
    prevouts.outpoint(in);
    sequences.le32(in.Sequence);
  }
  for (const auto &out: tx.TxOut)
    outputs.output(out);
  digests.Prevouts = prevouts.sha256();
  digests.Sequences = sequences.sha256();
  digests.Outputs = outputs.sha256();
  ::sha256(digests.Prevouts.begin(), 32, digests.HashPrevouts.begin());
  ::sha256(digests.Sequences.begin(), 32, digests.HashSequence.begin());
  ::sha256(digests.Outputs.begin(), 32, digests.HashOutputs.begin());
}

// The amounts and scripts of the spent outputs, the scripts restored from their records
void spentDigests(std::span<const Script::CUnspentOutputInfo* const> spent, CTxDigests &digests);

// Core's SignatureHash for SigVersion::BASE. scriptCode has no OP_CODESEPARATOR; SIGHASH_SINGLE
// without its output signs 1, Core's old bug kept by consensus
template<typename TxTy>
BaseBlob<256> sighashLegacy(const TxTy &tx, size_t input, const uint8_t *scriptCode, size_t scriptCodeSize, uint8_t hashType)
{
  const uint8_t base = hashType & 0x1F;
  const bool anyoneCanPay = (hashType & ESighashAnyoneCanPay) != 0;
  const bool single = base == ESighashSingle;
  const bool none = base == ESighashNone;
  if (single && input >= tx.TxOut.size()) {
    BaseBlob<256> one;
    one.begin()[0] = 1;
    return one;
  }

  CSha256Writer writer;
  writer.le32(static_cast<uint32_t>(tx.Version));
  const size_t inputs = anyoneCanPay ? 1 : tx.TxIn.size();
  writer.varSize(inputs);
  for (size_t i = 0; i < inputs; i++) {
    const size_t index = anyoneCanPay ? input : i;
    const auto &in = tx.TxIn[index];
    writer.outpoint(in);
    if (index == input)
      writer.script(scriptCode, scriptCodeSize);
    else
      writer.varSize(0);
    writer.le32(index != input && (single || none) ? 0 : in.Sequence);
  }

  const size_t outputs = none ? 0 : single ? input + 1 : tx.TxOut.size();
  writer.varSize(outputs);
  for (size_t i = 0; i < outputs; i++) {
    if (single && i != input) {
      // CTxOut(): value -1, empty script
      writer.le64(UINT64_MAX);
      writer.varSize(0);
    } else {
      writer.output(tx.TxOut[i]);
    }
  }
  writer.le32(tx.LockTime);
  writer.le32(hashType);
  return writer.sha256d();
}

// BIP143
template<typename TxTy>
BaseBlob<256> sighashWitnessV0(const TxTy &tx,
                               size_t input,
                               const uint8_t *scriptCode,
                               size_t scriptCodeSize,
                               int64_t amount,
                               uint8_t hashType,
                               const CTxDigests &digests)
{
  const uint8_t base = hashType & 0x1F;
  const bool anyoneCanPay = (hashType & ESighashAnyoneCanPay) != 0;
  const BaseBlob<256> zero;

  BaseBlob<256> hashOutputs;
  if (base != ESighashSingle && base != ESighashNone) {
    hashOutputs = digests.HashOutputs;
  } else if (base == ESighashSingle && input < tx.TxOut.size()) {
    CSha256Writer output;
    output.output(tx.TxOut[input]);
    hashOutputs = output.sha256d();
  }

  const auto &in = tx.TxIn[input];
  CSha256Writer writer;
  writer.le32(static_cast<uint32_t>(tx.Version));
  writer.blob(anyoneCanPay ? zero : digests.HashPrevouts);
  writer.blob(!anyoneCanPay && base != ESighashSingle && base != ESighashNone ? digests.HashSequence : zero);
  writer.outpoint(in);
  writer.script(scriptCode, scriptCodeSize);
  writer.le64(static_cast<uint64_t>(amount));
  writer.le32(in.Sequence);
  writer.blob(hashOutputs);
  writer.le32(tx.LockTime);
  writer.le32(hashType);
  return writer.sha256d();
}

const BaseBlob<256> &tapSighashTag();

// BIP341 for the key path: no annex, ext_flag 0. The hash type is one Core's SignatureHashSchnorr
// takes (checkSchnorrSignature): defined, SIGHASH_SINGLE with its output
template<typename TxTy>
BaseBlob<256> sighashTaproot(const TxTy &tx,
                             size_t input,
                             uint8_t hashType,
                             const CTxDigests &digests,
                             const Script::CUnspentOutputInfo &spent)
{
  const uint8_t outputType = hashType == ESighashDefault ? ESighashAll : (hashType & 0x03);
  const bool anyoneCanPay = (hashType & ESighashAnyoneCanPay) != 0;

  CSha256Writer writer = CSha256Writer::tagged(tapSighashTag());
  // Epoch
  writer.u8(0);
  writer.u8(hashType);
  writer.le32(static_cast<uint32_t>(tx.Version));
  writer.le32(tx.LockTime);
  if (!anyoneCanPay) {
    writer.blob(digests.Prevouts);
    writer.blob(digests.SpentAmounts);
    writer.blob(digests.SpentScripts);
    writer.blob(digests.Sequences);
  }
  if (outputType == ESighashAll)
    writer.blob(digests.Outputs);
  // Spend type: key path, no annex
  writer.u8(0);
  const auto &in = tx.TxIn[input];
  if (anyoneCanPay) {
    xvector<uint8_t> script;
    Script::restoreScript(spent, script);
    writer.outpoint(in);
    writer.le64(static_cast<uint64_t>(spent.Value));
    writer.script(script.data(), script.size());
    writer.le32(in.Sequence);
  } else {
    writer.le32(static_cast<uint32_t>(input));
  }
  if (outputType == ESighashSingle) {
    CSha256Writer output;
    output.output(tx.TxOut[input]);
    writer.blob(output.sha256());
  }
  return writer.sha256();
}

}
