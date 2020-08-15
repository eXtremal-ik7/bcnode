// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proto.h"
#include "common/serializeJson.h"

namespace BTC {

size_t Io<mpz_class>::getSerializedSize(const mpz_class &data)
{
  size_t size = 0;
  constexpr mp_limb_t one = 1;
  auto bnSize = mpz_sizeinbase(data.get_mpz_t(), 256);
  auto serializedSize = bnSize;
  if (data.get_mpz_t()->_mp_size) {
      mp_limb_t signBit = one << (8*(bnSize % sizeof(mp_limb_t)) - 1);
      if (data.get_mpz_t()->_mp_d[data.get_mpz_t()->_mp_size-1] & signBit)
          serializedSize++;
  }

  size += getSerializedVarSizeSize(serializedSize);
  size += bnSize;
  if (serializedSize > bnSize)
    size += 1;
  return size;
}

void Io<mpz_class>::serialize(xmstream &dst, const mpz_class &data)
{
  constexpr mp_limb_t one = 1;
  auto bnSize = mpz_sizeinbase(data.get_mpz_t(), 256);
  auto serializedSize = bnSize;
  if (data.get_mpz_t()->_mp_size) {
      mp_limb_t signBit = one << (8*(bnSize % sizeof(mp_limb_t)) - 1);
      if (data.get_mpz_t()->_mp_d[data.get_mpz_t()->_mp_size-1] & signBit)
          serializedSize++;
  }

  serializeVarSize(dst, serializedSize);
  mpz_export(dst.reserve<uint8_t>(bnSize), nullptr, -1, 1, -1, 0, data.get_mpz_t());
  if (serializedSize > bnSize)
    dst.write<uint8_t>(0);
}

void Io<mpz_class>::unserialize(xmstream &src, mpz_class &data)
{
  uint64_t size;
  unserializeVarSize(src, size);
  if (const uint8_t *p = src.seek<uint8_t>(size))
    mpz_import(data.get_mpz_t(), size, -1, 1, -1, 0, p);
}

void Io<mpz_class>::unpack(xmstream &src, DynamicPtr<mpz_class> dst)
{
  uint64_t size;
  unserializeVarSize(src, size);
  if (const uint8_t *p = src.seek<uint8_t>(size)) {
    size_t alignedSize = size % sizeof(mp_limb_t) ? size + (sizeof(mp_limb_t) - size % sizeof(mp_limb_t)) : size;
    size_t limbsNum = alignedSize / sizeof(mp_limb_t);
    size_t dataOffset = dst.stream().offsetOf();

    dst.stream().reserve<mp_limb_t>(limbsNum);

    mpz_class *mpz = dst.ptr();
    mp_limb_t *data = reinterpret_cast<mp_limb_t*>(dst.stream().data<uint8_t>() + dataOffset);
    mpz->get_mpz_t()->_mp_d = data;
    mpz->get_mpz_t()->_mp_size = static_cast<int>(limbsNum);
    mpz->get_mpz_t()->_mp_alloc = static_cast<int>(limbsNum);
    mpz_import(mpz->get_mpz_t(), size, -1, 1, -1, 0, p);

    // Change address to stream offset
    mpz->get_mpz_t()->_mp_d = reinterpret_cast<mp_limb_t*>(dataOffset);
  }
}

void Io<mpz_class>::unpackFinalize(DynamicPtr<mpz_class> dst)
{
  mpz_class *mpz = dst.ptr();
  mpz->get_mpz_t()->_mp_d = reinterpret_cast<mp_limb_t*>(dst.stream().data<uint8_t>() + reinterpret_cast<size_t>(mpz->get_mpz_t()->_mp_d));
}

size_t Io<XPM::Proto::BlockHeader>::getSerializedSize(const XPM::Proto::BlockHeader &data)
{
  return 80 + BTC::getSerializedSize(data.bnPrimeChainMultiplier);
}

void Io<XPM::Proto::BlockHeader>::serialize(xmstream &dst, const XPM::Proto::BlockHeader &data)
{
  BTC::serialize(dst, data.nVersion);
  BTC::serialize(dst, data.hashPrevBlock);
  BTC::serialize(dst, data.hashMerkleRoot);
  BTC::serialize(dst, data.nTime);
  BTC::serialize(dst, data.nBits);
  BTC::serialize(dst, data.nNonce);
  BTC::serialize(dst, data.bnPrimeChainMultiplier);
}

void Io<XPM::Proto::BlockHeader>::unserialize(xmstream &src, XPM::Proto::BlockHeader &data)
{
  BTC::unserialize(src, data.nVersion);
  BTC::unserialize(src, data.hashPrevBlock);
  BTC::unserialize(src, data.hashMerkleRoot);
  BTC::unserialize(src, data.nTime);
  BTC::unserialize(src, data.nBits);
  BTC::unserialize(src, data.nNonce);
  BTC::unserialize(src, data.bnPrimeChainMultiplier);
}

void Io<XPM::Proto::BlockHeader>::unpack(xmstream &src, DynamicPtr<XPM::Proto::BlockHeader> dst)
{
  XPM::Proto::BlockHeader *ptr = dst.ptr();

  // TODO: memcpy on little-endian
  BTC::unserialize(src, ptr->nVersion);
  BTC::unserialize(src, ptr->hashPrevBlock);
  BTC::unserialize(src, ptr->hashMerkleRoot);
  BTC::unserialize(src, ptr->nTime);
  BTC::unserialize(src, ptr->nBits);
  BTC::unserialize(src, ptr->nNonce);

  // unserialize prime chain multiplier
  BTC::unpack(src, DynamicPtr<mpz_class>(dst.stream(), dst.offset()+offsetof(XPM::Proto::BlockHeader, bnPrimeChainMultiplier)));
}

void Io<XPM::Proto::BlockHeader>::unpackFinalize(DynamicPtr<XPM::Proto::BlockHeader> dst)
{
  BTC::unpackFinalize(DynamicPtr<mpz_class>(dst.stream(), dst.offset()+offsetof(XPM::Proto::BlockHeader, bnPrimeChainMultiplier)));
}

void Io<XPM::Proto::MessageVersion>::serialize(xmstream &stream, const XPM::Proto::MessageVersion &data)
{
  BTC::serialize(stream, data.version);
  BTC::serialize(stream, data.services);
  BTC::serialize(stream, data.timestamp);
  BTC::serialize(stream, data.addr_recv);
  if (data.version >= 106) {
    BTC::serialize(stream, data.addr_from);
    BTC::serialize(stream, data.nonce);
    BTC::serialize(stream, data.user_agent);
    BTC::serialize(stream, data.start_height);
  }
}

void Io<XPM::Proto::MessageVersion>::unserialize(xmstream &stream, XPM::Proto::MessageVersion &data)
{
  BTC::unserialize(stream, data.version);
  BTC::unserialize(stream, data.services);
  BTC::unserialize(stream, data.timestamp);
  BTC::unserialize(stream, data.addr_recv);
  if (data.version >= 106) {
    BTC::unserialize(stream, data.addr_from);
    BTC::unserialize(stream, data.nonce);
    BTC::unserialize(stream, data.user_agent);
    BTC::unserialize(stream, data.start_height);
  }
}

}

void serializeJsonInside(xmstream &stream, const XPM::Proto::BlockHeader &header)
{
  std::string bnPrimeChainMultiplier = header.bnPrimeChainMultiplier.get_str();
  serializeJson(stream, "version", header.nVersion); stream.write(',');
  serializeJson(stream, "hashPrevBlock", header.hashPrevBlock); stream.write(',');
  serializeJson(stream, "hashMerkleRoot", header.hashMerkleRoot); stream.write(',');
  serializeJson(stream, "time", header.nTime); stream.write(',');
  serializeJson(stream, "bits", header.nBits); stream.write(',');
  serializeJson(stream, "nonce", header.nNonce); stream.write(',');
  serializeJson(stream, "bnPrimeChainMultiplier", bnPrimeChainMultiplier);
}
