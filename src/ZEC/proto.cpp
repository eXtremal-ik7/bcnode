// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proto.h"
#include "common/serializeJson.h"

namespace ZEC {
Proto::BlockHashTy Proto::Transaction::getTxId() const
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

void serializeJson(xmstream &stream, const char *fieldName, const ZEC::Proto::Transaction &data) {
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('{');
  serializeJson(stream, "txid", data.getTxId()); stream.write(',');
  serializeJson(stream, "overWintered", data.fOverwintered); stream.write(',');
  serializeJson(stream, "version", data.version); stream.write(',');
  serializeJson(stream, "txin", data.txIn); stream.write(',');
  serializeJson(stream, "txout", data.txOut); stream.write(',');
  serializeJson(stream, "lockTime", data.lockTime);
  stream.write('}');
}

void serializeJsonInside(xmstream &stream, const ZEC::Proto::BlockHeader &header)
{
  serializeJson(stream, "version", header.nVersion); stream.write(',');
  serializeJson(stream, "hashPrevBlock", header.hashPrevBlock); stream.write(',');
  serializeJson(stream, "hashMerkleRoot", header.hashMerkleRoot); stream.write(',');
  serializeJson(stream, "hashLightClientRoot", header.hashLightClientRoot); stream.write(',');
  serializeJson(stream, "time", header.nTime); stream.write(',');
  serializeJson(stream, "bits", header.nBits); stream.write(',');
  serializeJson(stream, "nonce", header.nNonce); stream.write(',');
  serializeJson(stream, "nSolution", header.nSolution);
}
