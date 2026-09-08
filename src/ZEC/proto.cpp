// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proto.h"
#include "common/serializeJson.h"

namespace ZEC {
Proto::BlockHashTy Proto::CTransaction::getTxId() const
{
  SmallStream<4096> stream;
  BTC::Io<Proto::CTransaction>::serialize(stream, *this);
  return BTC::sha256d(stream.data(), stream.sizeOf());
}
}

void serializeJson(xmstream &stream, const char *fieldName, const ZEC::Proto::CTransaction &data) {
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('{');
  serializeJson(stream, "txid", data.getTxId()); stream.write(',');
  serializeJson(stream, "overWintered", data.Overwintered); stream.write(',');
  serializeJson(stream, "version", data.Version); stream.write(',');
  serializeJson(stream, "txin", data.TxIn); stream.write(',');
  serializeJson(stream, "txout", data.TxOut); stream.write(',');
  serializeJson(stream, "lockTime", data.LockTime);
  stream.write('}');
}

void serializeJsonInside(xmstream &stream, const ZEC::Proto::CBlockHeader &header)
{
  serializeJson(stream, "version", header.Version); stream.write(',');
  serializeJson(stream, "hashPrevBlock", header.HashPrevBlock); stream.write(',');
  serializeJson(stream, "hashMerkleRoot", header.HashMerkleRoot); stream.write(',');
  serializeJson(stream, "hashLightClientRoot", header.HashLightClientRoot); stream.write(',');
  serializeJson(stream, "time", header.Time); stream.write(',');
  serializeJson(stream, "bits", header.Bits); stream.write(',');
  serializeJson(stream, "nonce", header.Nonce); stream.write(',');
  serializeJson(stream, "nSolution", header.Solution);
}
