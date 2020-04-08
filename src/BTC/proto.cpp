// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proto.h"
#include "common/serializeJson.h"

namespace BTC {

void Io<Proto::NetworkAddress>::serialize(xmstream &dst, const BTC::Proto::NetworkAddress &data)
{
  BTC::serialize(dst, data.time);
  BTC::serialize(dst, data.services);
  dst.write(data.ipv6, sizeof(data.ipv6));
  BTC::serialize(dst, data.port);
}

void Io<Proto::NetworkAddress>::unserialize(xmstream &src, BTC::Proto::NetworkAddress &data)
{
  BTC::unserialize(src, data.time);
  BTC::unserialize(src, data.services);
  src.read(data.ipv6, sizeof(data.ipv6));
  BTC::unserialize(src, data.port);
}

void Io<Proto::NetworkAddressWithoutTime>::serialize(xmstream &dst, const BTC::Proto::NetworkAddressWithoutTime &data)
{
  BTC::serialize(dst, data.services);
  dst.write(data.ipv6, sizeof(data.ipv6));
  BTC::serialize(dst, data.port);
}

void Io<Proto::NetworkAddressWithoutTime>::unserialize(xmstream &src, BTC::Proto::NetworkAddressWithoutTime &data)
{
  BTC::unserialize(src, data.services);
  src.read(data.ipv6, sizeof(data.ipv6));
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

void Io<Proto::TxIn>::unpack(xmstream &src, DynamicPtr<BTC::Proto::TxIn> dst)
{
  {
    BTC::Proto::TxIn *ptr = dst.ptr();
    BTC::unserialize(src, ptr->previousOutputHash);
    BTC::unserialize(src, ptr->previousOutputIndex);
  }

  BTC::unpack(src, DynamicPtr<decltype (dst->scriptSig)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, scriptSig)));
  {
    BTC::Proto::TxIn *ptr = dst.ptr();
    new (&ptr->witnessStack) decltype(ptr->witnessStack)();
    BTC::unserialize(src, ptr->sequence);
  }
}

void Io<Proto::TxIn>::unpackFinalize(DynamicPtr<BTC::Proto::TxIn> dst)
{
  BTC::unpackFinalize(DynamicPtr<decltype (dst->scriptSig)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, scriptSig)));
  BTC::unpackFinalize(DynamicPtr<decltype (dst->witnessStack)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, witnessStack)));
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

void Io<Proto::TxOut>::unpack(xmstream &src, DynamicPtr<BTC::Proto::TxOut> dst)
{
  BTC::unserialize(src, dst->value);
  BTC::unpack(src, DynamicPtr<decltype (dst->pkScript)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxOut, pkScript)));
}

void Io<Proto::TxOut>::unpackFinalize(DynamicPtr<BTC::Proto::TxOut> dst)
{
  BTC::unpackFinalize(DynamicPtr<decltype (dst->pkScript)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxOut, pkScript)));
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
