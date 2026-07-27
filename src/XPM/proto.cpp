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

size_t Io<mpz_class>::getUnpackedExtraSize(xmstream &src)
{
  uint64_t size;
  unserializeVarSize(src, size);
  if (src.seek<uint8_t>(size)) {
    size_t alignedSize = size % sizeof(mp_limb_t) ? size + (sizeof(mp_limb_t) - size % sizeof(mp_limb_t)) : size;
    size_t limbsNum = alignedSize / sizeof(mp_limb_t);
    return limbsNum * sizeof(mp_limb_t);
  } else {
    return 0;
  }
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

void Io<mpz_class>::unpack2(xmstream &src, mpz_class *data, uint8_t **extraData)
{
  uint64_t size;
  unserializeVarSize(src, size);
  if (const uint8_t *p = src.seek<uint8_t>(size)) {
    size_t alignedSize = size % sizeof(mp_limb_t) ? size + (sizeof(mp_limb_t) - size % sizeof(mp_limb_t)) : size;
    size_t limbsNum = alignedSize / sizeof(mp_limb_t);
    mp_limb_t *limbs = reinterpret_cast<mp_limb_t*>(*extraData);
    (*extraData) += limbsNum*sizeof(mp_limb_t);

    data->get_mpz_t()->_mp_d = limbs;
    data->get_mpz_t()->_mp_size = static_cast<int>(limbsNum);
    data->get_mpz_t()->_mp_alloc = static_cast<int>(limbsNum);
    mpz_import(data->get_mpz_t(), size, -1, 1, -1, 0, p);
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
