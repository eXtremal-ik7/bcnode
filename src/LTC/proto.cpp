// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proto.h"
#include "common/serializeJson.h"

namespace LTC {
Proto::BlockHashTy Proto::Transaction::getTxId() const
{
  BaseBlob<256> result;
  uint8_t buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  stream.reset();
  BTC::Io<Proto::Transaction>::serialize(stream, *this, false);

  CCtxSha256 sha256;
  sha256Init(&sha256);
  sha256Update(&sha256, stream.data(), stream.sizeOf());
  sha256Final(&sha256, result.begin());
  sha256Init(&sha256);
  sha256Update(&sha256, result.begin(), sizeof(result));
  sha256Final(&sha256, result.begin());
  return result;
}

Proto::BlockHashTy Proto::Transaction::getWTxid() const
{
  BaseBlob<256> result;
  uint8_t buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  stream.reset();
  BTC::Io<Proto::Transaction>::serialize(stream, *this);

  CCtxSha256 sha256;
  sha256Init(&sha256);
  sha256Update(&sha256, stream.data(), stream.sizeOf());
  sha256Final(&sha256, result.begin());
  sha256Init(&sha256);
  sha256Update(&sha256, result.begin(), sizeof(result));
  sha256Final(&sha256, result.begin());
  return result;
}
}

namespace BTC {

size_t Io<LTC::Proto::Transaction>::getSerializedSize(const LTC::Proto::Transaction &data, bool serializeWitness)
{
  size_t size = 0;
  uint8_t flags = 0;
  size += BTC::getSerializedSize(data.version);

  if (data.hasWitness() && serializeWitness) {
    flags = 1;
    size += BTC::getSerializedVarSizeSize(0);
    size += BTC::getSerializedSize(flags);
  }

  size += BTC::getSerializedSize(data.txIn);
  size += BTC::getSerializedSize(data.txOut);

  if (flags) {
    for (size_t i = 0; i < data.txIn.size(); i++)
      size += BTC::getSerializedSize(data.txIn[i].witnessStack);
  }

  size += BTC::getSerializedSize(data.lockTime);
  return size;
}

size_t Io<LTC::Proto::Transaction>::getUnpackedExtraSize(xmstream &src)
{
  size_t result = 0;
  uint8_t flags = 0;
  src.seek(sizeof(Proto::Transaction::version));

  uint64_t txInCount = 0;
  result += Io<decltype (Proto::Transaction::txIn)>::getUnpackedExtraSize(src, &txInCount);
  if (txInCount == 0) {
    BTC::unserialize(src, flags);
    if (flags != 0) {
      result += Io<decltype (Proto::Transaction::txIn)>::getUnpackedExtraSize(src, &txInCount);
      result += Io<decltype (Proto::Transaction::txOut)>::getUnpackedExtraSize(src);
    }
  } else {
    result += Io<decltype (Proto::Transaction::txOut)>::getUnpackedExtraSize(src);
  }

  if (flags & 1) {
    flags ^= 1;
    bool hasWitness = false;
    for (size_t i = 0; i < txInCount; i++) {
      uint64_t witnessCount = 0;
      result += Io<decltype (Proto::TxIn::witnessStack)>::getUnpackedExtraSize(src, &witnessCount);
      if (witnessCount != 0)
        hasWitness = true;
    }

    if (!hasWitness)
      src.seekEnd(0, true);
  }

  if (flags)
    src.seekEnd(0, true);

  src.seek(sizeof(Proto::Transaction::lockTime));
  return result;
}

void Io<LTC::Proto::Transaction>::serialize(xmstream &dst, const LTC::Proto::Transaction &data, bool serializeWitness)
{
  uint8_t flags = 0;
  BTC::serialize(dst, data.version);
  if (data.hasWitness() && serializeWitness) {
    flags = 1;
    BTC::serializeVarSize(dst, 0);
    BTC::serialize(dst, flags);
  }

  BTC::serialize(dst, data.txIn);
  BTC::serialize(dst, data.txOut);

  if (flags) {
    for (size_t i = 0; i < data.txIn.size(); i++)
      BTC::serialize(dst, data.txIn[i].witnessStack);
  }

  BTC::serialize(dst, data.lockTime);
}

void Io<LTC::Proto::Transaction>::unserialize(xmstream &src, LTC::Proto::Transaction &data)
{
  uint8_t flags = 0;
  BTC::unserialize(src, data.version);
  BTC::unserialize(src, data.txIn);
  if (data.txIn.empty()) {
    BTC::unserialize(src, flags);
    if (flags != 0) {
      BTC::unserialize(src, data.txIn);
      BTC::unserialize(src, data.txOut);
    }
  } else {
    BTC::unserialize(src, data.txOut);
  }

  if (flags & 1) {
    flags ^= 1;

    for (size_t i = 0; i < data.txIn.size(); i++)
      BTC::unserialize(src, data.txIn[i].witnessStack);

    if (!data.hasWitness()) {
      src.seekEnd(0, true);
      return;
    }
  }

  if (flags) {
    src.seekEnd(0, true);
    return;
  }

  BTC::unserialize(src, data.lockTime);
}

void Io<LTC::Proto::Transaction>::unpack2(xmstream &src, LTC::Proto::Transaction *data, uint8_t **extraData)
{
  uint8_t flags = 0;
  BTC::unserialize(src, data->version);
  Io<decltype (data->txIn)>::unpack2(src, &data->txIn, extraData);
  if (data->txIn.empty()) {
    BTC::unserialize(src, flags);
    if (flags != 0) {
      Io<decltype (data->txIn)>::unpack2(src, &data->txIn, extraData);
      Io<decltype (data->txOut)>::unpack2(src, &data->txOut, extraData);
    }
  } else {
    Io<decltype (data->txOut)>::unpack2(src, &data->txOut, extraData);
  }

  if (flags & 1) {
    flags ^= 1;

    for (size_t i = 0; i < data->txIn.size(); i++)
      Io<decltype (data->txIn[i].witnessStack)>::unpack2(src, &data->txIn[i].witnessStack, extraData);

    if (!data->hasWitness()) {
      src.seekEnd(0, true);
      return;
    }
  }

  if (flags) {
    src.seekEnd(0, true);
    return;
  }

  BTC::unserialize(src, data->lockTime);
}

void Io<LTC::Proto::Transaction>::serializeForSignature(xmstream &dst,
    const LTC::Proto::Transaction &data,
    size_t targetInput,
    const uint8_t *utxo,
    size_t utxoSize)
{
  BTC::serialize(dst, data.version);
  serializeVarSize(dst, data.txIn.size());
  for (size_t i = 0; i < data.txIn.size(); i++) {
    if (i == targetInput)
      Io<Proto::TxIn>::serializeForSignature(dst, data.txIn[i], utxo, utxoSize);
    else
      Io<Proto::TxIn>::serializeForSignature(dst, data.txIn[i], nullptr, 0);
  }
  BTC::serialize(dst, data.txOut);
  BTC::serialize(dst, data.lockTime);
}

}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::Proto::Transaction &data) {
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('{');
  serializeJson(stream, "txid", data.getTxId()); stream.write(',');
  serializeJson(stream, "version", data.version); stream.write(',');
  serializeJson(stream, "txin", data.txIn); stream.write(',');
  serializeJson(stream, "txout", data.txOut); stream.write(',');
  serializeJson(stream, "lockTime", data.lockTime);
  stream.write('}');
}
