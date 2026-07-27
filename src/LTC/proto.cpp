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
Proto::BlockHashTy Proto::Transaction::getTxId() const
{
  SmallStream<4096> stream;
  BTC::Io<Proto::Transaction>::serialize(stream, *this, Proto::SerializeCtx(false, false));
  return BTC::sha256d(stream.data(), stream.sizeOf());
}

Proto::BlockHashTy Proto::Transaction::getWTxid() const
{
  SmallStream<4096> stream;
  BTC::Io<Proto::Transaction>::serialize(stream, *this, Proto::SerializeCtx(true, false));
  return BTC::sha256d(stream.data(), stream.sizeOf());
}
}

namespace BTC {

void serializeForSignature(xmstream &dst,
                           const LTC::Proto::Transaction &data,
                           size_t targetInput,
                           const uint8_t *utxo,
                           size_t utxoSize)
{
  BTC::serialize(dst, data.version);
  serializeVarSize(dst, data.txIn.size());
  for (size_t i = 0; i < data.txIn.size(); i++) {
    if (i == targetInput)
      BTC::serializeForSignature(dst, data.txIn[i], utxo, utxoSize);
    else
      BTC::serializeForSignature(dst, data.txIn[i], nullptr, 0);
  }
  BTC::serialize(dst, data.txOut);
  BTC::serialize(dst, data.lockTime);
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

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Input &data)
{
  using Input = LTC::MWeb::Input;
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJson(stream, "features", data.features); stream.write(',');
  serializeJsonHex(stream, "outputId", data.outputId); stream.write(',');
  serializeJsonHex(stream, "commitment", data.commitment); stream.write(',');
  serializeJsonHex(stream, "outputPubKey", data.outputPubKey); stream.write(',');
  if (data.features & Input::StealthKeyFeatureBit) {
    serializeJsonHex(stream, "inputPubKey", data.inputPubKey); stream.write(',');
  }
  if (data.features & Input::ExtraDataFeatureBit) {
    serializeJson(stream, "extraData", data.extraData); stream.write(',');
  }
  serializeJsonHex(stream, "signature", data.signature);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Output &data)
{
  using OutputMessage = LTC::MWeb::OutputMessage;
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJsonHex(stream, "commitment", data.commitment); stream.write(',');
  serializeJsonHex(stream, "senderPubKey", data.senderPubKey); stream.write(',');
  serializeJsonHex(stream, "receiverPubKey", data.receiverPubKey); stream.write(',');
  serializeJson(stream, "features", data.message.features); stream.write(',');
  if (data.message.features & OutputMessage::StandardFieldsFeatureBit) {
    serializeJsonHex(stream, "keyExchangePubKey", data.message.keyExchangePubKey); stream.write(',');
    serializeJson(stream, "viewTag", data.message.viewTag); stream.write(',');
    serializeJson(stream, "maskedValue", data.message.maskedValue); stream.write(',');
    serializeJsonHex(stream, "maskedNonce", data.message.maskedNonce); stream.write(',');
  }
  if (data.message.features & OutputMessage::ExtraDataFeatureBit) {
    serializeJson(stream, "extraData", data.message.extraData); stream.write(',');
  }
  serializeJsonHex(stream, "rangeProof", data.rangeProof.data(), data.rangeProof.size()); stream.write(',');
  serializeJsonHex(stream, "signature", data.signature);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::PegOutCoin &data)
{
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJson(stream, "amount", data.amount); stream.write(',');
  serializeJson(stream, "pkScript", data.pkScript);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Kernel &data)
{
  using Kernel = LTC::MWeb::Kernel;
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJson(stream, "features", data.features); stream.write(',');
  if (data.features & Kernel::FeeFeatureBit) {
    serializeJson(stream, "fee", data.fee); stream.write(',');
  }
  if (data.features & Kernel::PegInFeatureBit) {
    serializeJson(stream, "pegIn", data.pegIn); stream.write(',');
  }
  if (data.features & Kernel::PegOutFeatureBit) {
    serializeJson(stream, "pegOuts", data.pegOuts); stream.write(',');
  }
  if (data.features & Kernel::HeightLockFeatureBit) {
    serializeJson(stream, "lockHeight", data.lockHeight); stream.write(',');
  }
  if (data.features & Kernel::StealthExcessFeatureBit) {
    serializeJsonHex(stream, "stealthExcess", data.stealthExcess); stream.write(',');
  }
  if (data.features & Kernel::ExtraDataFeatureBit) {
    serializeJson(stream, "extraData", data.extraData); stream.write(',');
  }
  serializeJsonHex(stream, "excess", data.excess); stream.write(',');
  serializeJsonHex(stream, "signature", data.signature);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Transaction &data)
{
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJsonHex(stream, "kernelOffset", data.kernelOffset); stream.write(',');
  serializeJsonHex(stream, "stealthOffset", data.stealthOffset); stream.write(',');
  serializeJson(stream, "inputs", data.body.inputs); stream.write(',');
  serializeJson(stream, "outputs", data.body.outputs); stream.write(',');
  serializeJson(stream, "kernels", data.body.kernels);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Block &data)
{
  jsonFieldName(stream, fieldName);
  stream.write('{');
  serializeJson(stream, "height", data.header.height); stream.write(',');
  serializeJsonHex(stream, "outputRoot", data.header.outputRoot); stream.write(',');
  serializeJsonHex(stream, "kernelRoot", data.header.kernelRoot); stream.write(',');
  serializeJsonHex(stream, "leafsetRoot", data.header.leafsetRoot); stream.write(',');
  serializeJsonHex(stream, "kernelOffset", data.header.kernelOffset); stream.write(',');
  serializeJsonHex(stream, "stealthOffset", data.header.stealthOffset); stream.write(',');
  serializeJson(stream, "outputMmrSize", data.header.outputMmrSize); stream.write(',');
  serializeJson(stream, "kernelMmrSize", data.header.kernelMmrSize); stream.write(',');
  serializeJson(stream, "inputs", data.body.inputs); stream.write(',');
  serializeJson(stream, "outputs", data.body.outputs); stream.write(',');
  serializeJson(stream, "kernels", data.body.kernels);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::Proto::Transaction &data) {
  jsonFieldName(stream, fieldName);

  stream.write('{');
  serializeJson(stream, "txid", data.getTxId()); stream.write(',');
  serializeJson(stream, "version", data.version); stream.write(',');
  serializeJson(stream, "txin", data.txIn); stream.write(',');
  serializeJson(stream, "txout", data.txOut); stream.write(',');
  if (data.hogEx) {
    stream.write("\"hogEx\":true,", 13);
  }
  if (data.hasMweb()) {
    serializeJson(stream, "mweb", data.mwebTx[0]); stream.write(',');
  }
  serializeJson(stream, "lockTime", data.lockTime);
  stream.write('}');
}
