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
  uint256 result;
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
  uint256 result;
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

void Io<Proto::NetworkAddress>::serialize(xmstream &dst, const BTC::Proto::NetworkAddress &data)
{
  BTC::serialize(dst, data.time);
  BTC::serialize(dst, data.services);
  dst.write(data.ipv6.u8, sizeof(data.ipv6.u8));
  BTC::serialize(dst, data.port);
}

void Io<Proto::NetworkAddress>::unserialize(xmstream &src, BTC::Proto::NetworkAddress &data)
{
  BTC::unserialize(src, data.time);
  BTC::unserialize(src, data.services);
  src.read(data.ipv6.u8, sizeof(data.ipv6.u8));
  BTC::unserialize(src, data.port);
  data.port = data.port;
}

void Io<Proto::NetworkAddressWithoutTime>::serialize(xmstream &dst, const BTC::Proto::NetworkAddressWithoutTime &data)
{
  BTC::serialize(dst, data.services);
  dst.write(data.ipv6.u8, sizeof(data.ipv6.u8));
  BTC::serialize(dst, data.port);
}

void Io<Proto::NetworkAddressWithoutTime>::unserialize(xmstream &src, BTC::Proto::NetworkAddressWithoutTime &data)
{
  BTC::unserialize(src, data.services);
  src.read(data.ipv6.u8, sizeof(data.ipv6.u8));
  BTC::unserialize(src, data.port);
}

void Io<Proto::BlockHeader>::serialize(xmstream &dst, const BTC::Proto::BlockHeader &data)
{
  BTC::serialize(dst, data.nVersion);
  BTC::serialize(dst, data.hashPrevBlock);
  BTC::serialize(dst, data.hashMerkleRoot);
  BTC::serialize(dst, data.nTime);
  BTC::serialize(dst, data.nBits);
  BTC::serialize(dst, data.nNonce);
}

void Io<Proto::BlockHeader>::unserialize(xmstream &src, BTC::Proto::BlockHeader &data)
{
  BTC::unserialize(src, data.nVersion);
  BTC::unserialize(src, data.hashPrevBlock);
  BTC::unserialize(src, data.hashMerkleRoot);
  BTC::unserialize(src, data.nTime);
  BTC::unserialize(src, data.nBits);
  BTC::unserialize(src, data.nNonce);
}

size_t Io<Proto::TxIn>::getSerializedSize(const BTC::Proto::TxIn &data)
{
  size_t size = 0;
  size += BTC::getSerializedSize(data.previousOutputHash);
  size += BTC::getSerializedSize(data.previousOutputIndex);
  size += BTC::getSerializedSize(data.scriptSig);
  size += BTC::getSerializedSize(data.sequence);
  return size;
}

size_t Io<Proto::TxIn>::getUnpackedExtraSize(xmstream &src)
{
  size_t scriptSigSize = 0;
  src.seek(sizeof(Proto::TxIn::previousOutputHash) +
           sizeof(Proto::TxIn::previousOutputIndex));
  scriptSigSize = Io<decltype (Proto::TxIn::scriptSig)>::getUnpackedExtraSize(src);
  src.seek(sizeof(Proto::TxIn::sequence));
  return scriptSigSize;
}

void Io<Proto::TxIn>::serialize(xmstream &stream, const BTC::Proto::TxIn &data)
{
  BTC::serialize(stream, data.previousOutputHash);
  BTC::serialize(stream, data.previousOutputIndex);
  BTC::serialize(stream, data.scriptSig);
  BTC::serialize(stream, data.sequence);
}

void Io<Proto::TxIn>::unserialize(xmstream &stream, BTC::Proto::TxIn &data)
{
  BTC::unserialize(stream, data.previousOutputHash);
  BTC::unserialize(stream, data.previousOutputIndex);
  BTC::unserialize(stream, data.scriptSig);
  BTC::unserialize(stream, data.sequence);
}

void Io<Proto::TxIn>::unpack2(xmstream &src, Proto::TxIn *data, uint8_t **extraData)
{
  new (&data->witnessStack) decltype(data->witnessStack)();
  BTC::unserialize(src, data->previousOutputHash);
  BTC::unserialize(src, data->previousOutputIndex);
  Io<decltype (data->scriptSig)>::unpack2(src, &data->scriptSig, extraData);
  BTC::unserialize(src, data->sequence);
}

void Io<Proto::TxIn>::serializeForSignature(xmstream &dst, const BTC::Proto::TxIn &data, const uint8_t *utxo, size_t utxoSize)
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

size_t Io<Proto::TxOut>::getSerializedSize(const BTC::Proto::TxOut &data)
{
  size_t size = 0;
  size += BTC::getSerializedSize(data.value);
  size += BTC::getSerializedSize(data.pkScript);
  return size;
}

size_t Io<Proto::TxOut>::getUnpackedExtraSize(xmstream &src)
{
  src.seek(sizeof(Proto::TxOut::value));
  return Io<decltype (Proto::TxOut::pkScript)>::getUnpackedExtraSize(src);
}

void Io<Proto::TxOut>::serialize(xmstream &src, const BTC::Proto::TxOut &data)
{
  BTC::serialize(src, data.value);
  BTC::serialize(src, data.pkScript);
}

void Io<Proto::TxOut>::unserialize(xmstream &dst, BTC::Proto::TxOut &data)
{
  BTC::unserialize(dst, data.value);
  BTC::unserialize(dst, data.pkScript);
}

void Io<Proto::TxOut>::unpack2(xmstream &src, Proto::TxOut *data, uint8_t **extraData)
{
  BTC::unserialize(src, data->value);
  Io<decltype (Proto::TxOut::pkScript)>::unpack2(src, &data->pkScript, extraData);
}

size_t Io<Proto::Transaction>::getSerializedSize(const BTC::Proto::Transaction &data, bool serializeWitness)
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

size_t Io<Proto::Transaction>::getUnpackedExtraSize(xmstream &src)
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

void Io<Proto::Transaction>::serialize(xmstream &dst, const BTC::Proto::Transaction &data, bool serializeWitness)
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

void Io<Proto::Transaction>::unserialize(xmstream &src, BTC::Proto::Transaction &data)
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

void Io<Proto::Transaction>::unpack2(xmstream &src, Proto::Transaction *data, uint8_t **extraData)
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

void Io<Proto::Transaction>::serializeForSignature(xmstream &dst,
                                                   const Proto::Transaction &data,
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

void Io<Proto::CTxLinkedOutputs>::serialize(xmstream &dst, const BTC::Proto::CTxLinkedOutputs &data)
{
  BTC::serialize(dst, data.TxIn);
}

void Io<Proto::CTxLinkedOutputs>::unserialize(xmstream &src, BTC::Proto::CTxLinkedOutputs &data)
{
  BTC::unserialize(src, data.TxIn);
}

void Io<Proto::CBlockLinkedOutputs>::serialize(xmstream &dst, const BTC::Proto::CBlockLinkedOutputs &data)
{
  BTC::serialize(dst, data.Tx);
}

void Io<Proto::CBlockLinkedOutputs>::unserialize(xmstream &src, BTC::Proto::CBlockLinkedOutputs &data)
{
  BTC::unserialize(src, data.Tx);
}

void Io<Proto::InventoryVector>::serialize(xmstream &dst, const BTC::Proto::InventoryVector &data)
{
  BTC::serialize(dst, data.type);
  BTC::serialize(dst, data.hash);
}

void Io<Proto::InventoryVector>::unserialize(xmstream &src, BTC::Proto::InventoryVector &data)
{
  BTC::unserialize(src, data.type);
  BTC::unserialize(src, data.hash);
}

void Io<Proto::MessageVersion>::serialize(xmstream &dst, const BTC::Proto::MessageVersion &data)
{
  BTC::serialize(dst, data.version);
  BTC::serialize(dst, data.services);
  BTC::serialize(dst, data.timestamp);
  BTC::serialize(dst, data.addr_recv);
  if (data.version >= 106) {
    BTC::serialize(dst, data.addr_from);
    BTC::serialize(dst, data.nonce);
    BTC::serialize(dst, data.user_agent);
    BTC::serialize(dst, data.start_height);
    if (data.version >= 70001)
      BTC::serialize(dst, data.relay);
  }
}

void Io<Proto::MessageVersion>::unserialize(xmstream &src, BTC::Proto::MessageVersion &data)
{
  BTC::unserialize(src, data.version);
  BTC::unserialize(src, data.services);
  BTC::unserialize(src, data.timestamp);
  BTC::unserialize(src, data.addr_recv);
  if (data.version >= 106) {
    BTC::unserialize(src, data.addr_from);
    BTC::unserialize(src, data.nonce);
    BTC::unserialize(src, data.user_agent);
    BTC::unserialize(src, data.start_height);
    if (data.version >= 70001)
      BTC::unserialize(src, data.relay);
  }
}

void Io<Proto::MessagePing>::serialize(xmstream &dst, const BTC::Proto::MessagePing &data)
{
  BTC::serialize(dst, data.nonce);
}

void Io<Proto::MessagePing>::unserialize(xmstream &src, BTC::Proto::MessagePing &data)
{
  BTC::unserialize(src, data.nonce);
}

void Io<Proto::MessagePong>::serialize(xmstream &dst, const BTC::Proto::MessagePong &data)
{
  BTC::serialize(dst, data.nonce);
}

void Io<Proto::MessagePong>::unserialize(xmstream &src, BTC::Proto::MessagePong &data)
{
  BTC::unserialize(src, data.nonce);
}

void Io<Proto::MessageAddr>::serialize(xmstream &dst, const BTC::Proto::MessageAddr &data)
{
  BTC::serialize(dst, data.addr_list);
}

void Io<Proto::MessageAddr>::unserialize(xmstream &src, BTC::Proto::MessageAddr &data)
{
  BTC::unserialize(src, data.addr_list);
}

void Io<Proto::MessageGetHeaders>::serialize(xmstream &dst, const BTC::Proto::MessageGetHeaders &data)
{
  BTC::serialize(dst, data.version);
  BTC::serialize(dst, data.BlockLocatorHashes);
  BTC::serialize(dst, data.HashStop);
}

void Io<Proto::MessageGetHeaders>::unserialize(xmstream &src, BTC::Proto::MessageGetHeaders &data)
{
  BTC::unserialize(src, data.version);
  BTC::unserialize(src, data.BlockLocatorHashes);
  BTC::unserialize(src, data.HashStop);
}

void Io<Proto::MessageGetBlocks>::serialize(xmstream &dst, const BTC::Proto::MessageGetBlocks &data)
{
  BTC::serialize(dst, data.version);
  BTC::serialize(dst, data.BlockLocatorHashes);
  BTC::serialize(dst, data.HashStop);
}

void Io<Proto::MessageGetBlocks>::unserialize(xmstream &src, BTC::Proto::MessageGetBlocks &data)
{
  BTC::unserialize(src, data.version);
  BTC::unserialize(src, data.BlockLocatorHashes);
  BTC::unserialize(src, data.HashStop);
}

void Io<Proto::MessageInv>::serialize(xmstream &dst, const BTC::Proto::MessageInv &data)
{
  BTC::serialize(dst, data.Inventory);
}

void Io<Proto::MessageInv>::unserialize(xmstream &src, BTC::Proto::MessageInv &data)
{
  BTC::unserialize(src, data.Inventory);
}

void Io<Proto::MessageGetData>::serialize(xmstream &dst, const BTC::Proto::MessageGetData &data)
{
  BTC::serialize(dst, data.inventory);
}

void Io<Proto::MessageGetData>::unserialize(xmstream &src, BTC::Proto::MessageGetData &data)
{
  BTC::unserialize(src, data.inventory);
}

void Io<Proto::MessageReject>::serialize(xmstream &dst, const BTC::Proto::MessageReject &data)
{
  BTC::serialize(dst, data.message);
  BTC::serialize(dst, data.ccode);
  BTC::serialize(dst, data.reason);
  // TODO: unserialize data
}

void Io<Proto::MessageReject>::unserialize(xmstream &src, BTC::Proto::MessageReject &data)
{
  BTC::unserialize(src, data.message);
  BTC::unserialize(src, data.ccode);
  BTC::unserialize(src, data.reason);
  // TODO: unserialize data
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
  // Using dynamic stack allocation
  uint8_t data[prefixSize + 4 + addressSize];
  for (unsigned i = 0; i < prefixSize; i++)
    data[i] = prefix[i];
  memcpy(data + prefixSize, address, addressSize);

  {
    uint8_t sha256[32];
    CCtxSha256 ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, &data[0], sizeof(data) - 4);
    sha256Final(&ctx, sha256);

    sha256Init(&ctx);
    sha256Update(&ctx, sha256, sizeof(sha256));
    sha256Final(&ctx, sha256);
    memcpy(data + prefixSize + addressSize, sha256, 4);
  }

  return EncodeBase58(data, data + sizeof(data));
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
