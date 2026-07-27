// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proto.h"
#include "common/serializeJson.h"

namespace LTC {
Proto::BlockHashTy Proto::Transaction::getTxId() const
{
  SmallStream<4096> stream;
  BTC::Io<Proto::Transaction>::serialize(stream, *this, false);
  return BTC::sha256d(stream.data(), stream.sizeOf());
}

Proto::BlockHashTy Proto::Transaction::getWTxid() const
{
  SmallStream<4096> stream;
  BTC::Io<Proto::Transaction>::serialize(stream, *this);
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
