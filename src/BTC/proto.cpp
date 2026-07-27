// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proto.h"
#include "common/base58.h"
#include "common/serializeJson.h"

namespace BTC {

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

void serializeForSignature(xmstream &dst, const BTC::Proto::TxIn &data, const uint8_t *utxo, size_t utxoSize)
{
  BTC::serialize(dst, data.previousOutputHash);
  BTC::serialize(dst, data.previousOutputIndex);
  if (utxo) {
    serializeVarSize(dst, utxoSize);
    dst.write(utxo, utxoSize);
  } else {
    serializeVarSize(dst, 0);
  }

  BTC::serialize(dst, data.sequence);
}

void serializeForSignature(xmstream &dst,
                           const Proto::Transaction &data,
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

void serializeJsonInside(xmstream &stream, const BTC::Proto::BlockHeader &header)
{
  serializeJson(stream, "version", header.nVersion); stream.write(',');
  serializeJson(stream, "hashPrevBlock", header.hashPrevBlock); stream.write(',');
  serializeJson(stream, "hashMerkleRoot", header.hashMerkleRoot); stream.write(',');
  serializeJson(stream, "time", header.nTime); stream.write(',');
  serializeJson(stream, "bits", header.nBits); stream.write(',');
  serializeJson(stream, "nonce", header.nNonce);
}

void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxIn &txin)
{
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('{');
  serializeJson(stream, "previousOutputHash", txin.previousOutputHash); stream.write(',');
  serializeJson(stream, "previousOutputIndex", txin.previousOutputIndex); stream.write(',');
  serializeJson(stream, "scriptsig", txin.scriptSig); stream.write(',');
  serializeJson(stream, "sequence", txin.sequence);
  if (!txin.witnessStack.empty()) {
    stream.write(',');
    serializeJson(stream, "witnessStack", txin.witnessStack);
  }
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxOut &txout)
{
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('{');
  serializeJson(stream, "value", txout.value); stream.write(',');
  serializeJson(stream, "pkscript", txout.pkScript);
  stream.write('}');
}

void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::Transaction &data) {
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

std::string encodeBase58WithCrc(const uint8_t *prefix, unsigned prefixSize, const uint8_t *address, unsigned addressSize)
{
  std::vector<uint8_t> data(prefixSize + 4 + addressSize);
  for (unsigned i = 0; i < prefixSize; i++)
    data[i] = prefix[i];
  memcpy(data.data() + prefixSize, address, addressSize);

  {
    uint8_t sha256[32];
    CCtxSha256 ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, data.data(), data.size() - 4);
    sha256Final(&ctx, sha256);

    sha256Init(&ctx);
    sha256Update(&ctx, sha256, sizeof(sha256));
    sha256Final(&ctx, sha256);
    memcpy(data.data() + prefixSize + addressSize, sha256, 4);
  }

  return EncodeBase58(data.data(), data.data() + data.size());
}

bool decodeBase58WithCrc(const std::string &base58, const uint8_t *prefix, unsigned prefixSize, uint8_t *address, unsigned addressSize)
{
  std::vector<uint8_t> data;
  if (!DecodeBase58(base58.c_str(), data) ||
      data.size() != prefixSize + addressSize + 4 ||
      memcmp(&data[0], prefix, prefixSize))
    return false;

  uint32_t addrHash;
  memcpy(&addrHash, &data[prefixSize + sizeof(BTC::Proto::AddressTy)], 4);

  // Compute sha256 and take first 4 bytes
  uint8_t sha256[32];
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, &data[0], data.size() - 4);
  sha256Final(&ctx, sha256);

  sha256Init(&ctx);
  sha256Update(&ctx, sha256, sizeof(sha256));
  sha256Final(&ctx, sha256);

  if (reinterpret_cast<uint32_t*>(sha256)[0] != addrHash)
    return false;

  memcpy(address, &data[prefixSize], sizeof(BTC::Proto::AddressTy));
  return true;
}

std::string makeHumanReadableAddress(uint8_t pubkeyAddressPrefix, const BTC::Proto::AddressTy &address)
{
  uint8_t data[sizeof(BTC::Proto::AddressTy) + 5];
  data[0] = pubkeyAddressPrefix;
  memcpy(&data[1], address.begin(), sizeof(BTC::Proto::AddressTy));

  uint8_t sha256[32];
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, &data[0], sizeof(data) - 4);
  sha256Final(&ctx, sha256);

  sha256Init(&ctx);
  sha256Update(&ctx, sha256, sizeof(sha256));
  sha256Final(&ctx, sha256);

  memcpy(data+1+sizeof(BTC::Proto::AddressTy), sha256, 4);
  return EncodeBase58(data, data+sizeof(data));
}

bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, BTC::Proto::AddressTy &address)
{
  const size_t prefixSize = pubkeyAddressPrefix.size();
  std::vector<uint8_t> data;
  if (!DecodeBase58(hrAddress.c_str(), data) ||
      data.size() != prefixSize + sizeof(BTC::Proto::AddressTy) + 4 ||
      memcmp(&data[0], &pubkeyAddressPrefix[0], prefixSize))
    return false;

  uint32_t addrHash;
  memcpy(&addrHash, &data[prefixSize + sizeof(BTC::Proto::AddressTy)], 4);

  // Compute sha256 and take first 4 bytes
  uint8_t sha256[32];
  CCtxSha256 ctx;
  sha256Init(&ctx);
  sha256Update(&ctx, &data[0], data.size() - 4);
  sha256Final(&ctx, sha256);

  sha256Init(&ctx);
  sha256Update(&ctx, sha256, sizeof(sha256));
  sha256Final(&ctx, sha256);

  if (reinterpret_cast<uint32_t*>(sha256)[0] != addrHash)
    return false;

  memcpy(address.begin(), &data[prefixSize], sizeof(BTC::Proto::AddressTy));
  return true;
}
