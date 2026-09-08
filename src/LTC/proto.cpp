// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proto.h"
#include "common/serializeJson.h"

namespace LTC {
// Both ids cover the canonical transaction alone: Core hashes with SERIALIZE_NO_MWEB, so the
// flag bit and the MWEB section are out of the txid and the wtxid alike.
//
// Core has one more case, an MWEB only transaction identified by its first kernel rather than
// by its serialization. Those live in the mempool and in the extension block, never in a
// block's transaction list, so nothing hashed here can be one.
Proto::BlockHashTy Proto::CTransaction::getTxId() const
{
  SmallStream<4096> stream;
  BTC::Io<Proto::CTransaction>::serialize(stream, *this, Proto::CSerializeCtx(false, false));
  return BTC::sha256d(stream.data(), stream.sizeOf());
}

Proto::BlockHashTy Proto::CTransaction::getWTxid() const
{
  SmallStream<4096> stream;
  BTC::Io<Proto::CTransaction>::serialize(stream, *this, Proto::CSerializeCtx(true, false));
  return BTC::sha256d(stream.data(), stream.sizeOf());
}
}

namespace BTC {

void serializeForSignature(xmstream &dst,
                           const LTC::Proto::CTransaction &data,
                           size_t targetInput,
                           const uint8_t *utxo,
                           size_t utxoSize)
{
  BTC::serialize(dst, data.Version);
  serializeVarSize(dst, data.TxIn.size());
  for (size_t i = 0; i < data.TxIn.size(); i++) {
    if (i == targetInput)
      BTC::serializeForSignature(dst, data.TxIn[i], utxo, utxoSize);
    else
      BTC::serializeForSignature(dst, data.TxIn[i], nullptr, 0);
  }
  BTC::serialize(dst, data.TxOut);
  BTC::serialize(dst, data.LockTime);
}

}

namespace {

void jsonFieldName(xmstream &stream, const char *fieldName)
{
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }
}

// MWEB prints its byte strings in wire order; the little endian hex of serializeJson.h is for
// the hashes of the canonical chain
void serializeJsonHex(xmstream &stream, const char *fieldName, const uint8_t *data, size_t size)
{
  jsonFieldName(stream, fieldName);
  stream.write('\"');
  char *out = stream.reserve<char>(size*2);
  for (size_t i = 0; i < size; i++) {
    out[i*2+0] = hexDigit(data[i] >> 4);
    out[i*2+1] = hexDigit(data[i] & 0x0F);
  }
  stream.write('\"');
}

template<unsigned Bits>
void serializeJsonHex(xmstream &stream, const char *fieldName, const BaseBlob<Bits> &data)
{
  serializeJsonHex(stream, fieldName, data.begin(), data.size());
}

}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CInput &data)
{
  using Input = LTC::MWeb::CInput;
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJson(stream, "features", data.Features); stream.write(',');
  serializeJsonHex(stream, "outputId", data.OutputId); stream.write(',');
  serializeJsonHex(stream, "commitment", data.Commitment); stream.write(',');
  serializeJsonHex(stream, "outputPubKey", data.OutputPubKey); stream.write(',');
  if (data.Features & Input::StealthKeyFeatureBit) {
    serializeJsonHex(stream, "inputPubKey", data.InputPubKey); stream.write(',');
  }
  if (data.Features & Input::ExtraDataFeatureBit) {
    serializeJson(stream, "extraData", data.ExtraData); stream.write(',');
  }
  serializeJsonHex(stream, "signature", data.Signature);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::COutput &data)
{
  using OutputMessage = LTC::MWeb::COutputMessage;
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJsonHex(stream, "commitment", data.Commitment); stream.write(',');
  serializeJsonHex(stream, "senderPubKey", data.SenderPubKey); stream.write(',');
  serializeJsonHex(stream, "receiverPubKey", data.ReceiverPubKey); stream.write(',');
  serializeJson(stream, "features", data.Message.Features); stream.write(',');
  if (data.Message.Features & OutputMessage::StandardFieldsFeatureBit) {
    serializeJsonHex(stream, "keyExchangePubKey", data.Message.KeyExchangePubKey); stream.write(',');
    serializeJson(stream, "viewTag", data.Message.ViewTag); stream.write(',');
    serializeJson(stream, "maskedValue", data.Message.MaskedValue); stream.write(',');
    serializeJsonHex(stream, "maskedNonce", data.Message.MaskedNonce); stream.write(',');
  }
  if (data.Message.Features & OutputMessage::ExtraDataFeatureBit) {
    serializeJson(stream, "extraData", data.Message.ExtraData); stream.write(',');
  }
  serializeJsonHex(stream, "rangeProof", data.RangeProof.data(), data.RangeProof.size()); stream.write(',');
  serializeJsonHex(stream, "signature", data.Signature);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CPegOutCoin &data)
{
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJson(stream, "amount", data.Amount); stream.write(',');
  serializeJson(stream, "pkScript", data.PkScript);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CKernel &data)
{
  using Kernel = LTC::MWeb::CKernel;
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJson(stream, "features", data.Features); stream.write(',');
  if (data.Features & Kernel::FeeFeatureBit) {
    serializeJson(stream, "fee", data.Fee); stream.write(',');
  }
  if (data.Features & Kernel::PegInFeatureBit) {
    serializeJson(stream, "pegIn", data.PegIn); stream.write(',');
  }
  if (data.Features & Kernel::PegOutFeatureBit) {
    serializeJson(stream, "pegOuts", data.PegOuts); stream.write(',');
  }
  if (data.Features & Kernel::HeightLockFeatureBit) {
    serializeJson(stream, "lockHeight", data.LockHeight); stream.write(',');
  }
  if (data.Features & Kernel::StealthExcessFeatureBit) {
    serializeJsonHex(stream, "stealthExcess", data.StealthExcess); stream.write(',');
  }
  if (data.Features & Kernel::ExtraDataFeatureBit) {
    serializeJson(stream, "extraData", data.ExtraData); stream.write(',');
  }
  serializeJsonHex(stream, "excess", data.Excess); stream.write(',');
  serializeJsonHex(stream, "signature", data.Signature);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CTransaction &data)
{
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJsonHex(stream, "kernelOffset", data.KernelOffset); stream.write(',');
  serializeJsonHex(stream, "stealthOffset", data.StealthOffset); stream.write(',');
  serializeJson(stream, "inputs", data.Body.Inputs); stream.write(',');
  serializeJson(stream, "outputs", data.Body.Outputs); stream.write(',');
  serializeJson(stream, "kernels", data.Body.Kernels);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CBlock &data)
{
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJson(stream, "height", data.Header.Height); stream.write(',');
  serializeJsonHex(stream, "outputRoot", data.Header.OutputRoot); stream.write(',');
  serializeJsonHex(stream, "kernelRoot", data.Header.KernelRoot); stream.write(',');
  serializeJsonHex(stream, "leafsetRoot", data.Header.LeafsetRoot); stream.write(',');
  serializeJsonHex(stream, "kernelOffset", data.Header.KernelOffset); stream.write(',');
  serializeJsonHex(stream, "stealthOffset", data.Header.StealthOffset); stream.write(',');
  serializeJson(stream, "outputMmrSize", data.Header.OutputMmrSize); stream.write(',');
  serializeJson(stream, "kernelMmrSize", data.Header.KernelMmrSize); stream.write(',');
  serializeJson(stream, "inputs", data.Body.Inputs); stream.write(',');
  serializeJson(stream, "outputs", data.Body.Outputs); stream.write(',');
  serializeJson(stream, "kernels", data.Body.Kernels);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::Proto::CTransaction &data) {
  jsonFieldName(stream, fieldName);

  stream.write('{');
  serializeJson(stream, "txid", data.getTxId()); stream.write(',');
  serializeJson(stream, "version", data.Version); stream.write(',');
  serializeJson(stream, "txin", data.TxIn); stream.write(',');
  serializeJson(stream, "txout", data.TxOut); stream.write(',');
  if (data.HogEx) {
    stream.write("\"hogEx\":true,", 13);
  }
  if (data.hasMweb()) {
    serializeJson(stream, "mweb", data.MwebTx[0]); stream.write(',');
  }
  serializeJson(stream, "lockTime", data.LockTime);
  stream.write('}');
}
