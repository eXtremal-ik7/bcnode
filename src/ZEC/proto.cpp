// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "proto.h"
#include "common/serializeJson.h"

namespace BTC {

size_t Io<ZEC::Proto::CompressedG1>::getSerializedSize(const ZEC::Proto::CompressedG1 &data)
{
  return 1 + 32;
}

size_t Io<ZEC::Proto::CompressedG1>::getUnpackedExtraSize(xmstream &src)
{
  src.seek(1 + 32);
  return 0;
}

void Io<ZEC::Proto::CompressedG1>::serialize(xmstream &dst, const ZEC::Proto::CompressedG1 &data)
{
  uint8_t leadingByte = ZEC::Proto::G1_PREFIX_MASK;
  if (data.y_lsb)
    leadingByte |= 1;
  BTC::serialize(dst, leadingByte);
  BTC::serialize(dst, data.x);
}

void Io<ZEC::Proto::CompressedG1>::unserialize(xmstream &src, ZEC::Proto::CompressedG1 &data)
{
  uint8_t leadingByte;
  BTC::unserialize(src, leadingByte);
  if ((leadingByte & (~1)) != ZEC::Proto::G1_PREFIX_MASK) {
    src.seekEnd(0, true);
    return;
  }

  data.y_lsb = leadingByte & 1;
  BTC::unserialize(src, data.x);
}

void Io<ZEC::Proto::CompressedG1>::unpack2(xmstream &src, ZEC::Proto::CompressedG1 *data, uint8_t**)
{
  unserialize(src, *data);
}

size_t Io<ZEC::Proto::CompressedG2>::getSerializedSize(const ZEC::Proto::CompressedG2&)
{
  return 1 + 64;
}

size_t Io<ZEC::Proto::CompressedG2>::getUnpackedExtraSize(xmstream &src)
{
  src.seek(1 + 64);
  return 0;
}

void Io<ZEC::Proto::CompressedG2>::serialize(xmstream &dst, const ZEC::Proto::CompressedG2 &data)
{
  uint8_t leadingByte = ZEC::Proto::G2_PREFIX_MASK;
  if (data.y_gt)
    leadingByte |= 1;
  BTC::serialize(dst, leadingByte);
  BTC::serialize(dst, data.x);
}

void Io<ZEC::Proto::CompressedG2>::unserialize(xmstream &src, ZEC::Proto::CompressedG2 &data)
{
  uint8_t leadingByte;
  BTC::unserialize(src, leadingByte);
  if ((leadingByte & (~1)) != ZEC::Proto::G2_PREFIX_MASK) {
    src.seekEnd(0, true);
    return;
  }

  data.y_gt = leadingByte & 1;
  BTC::unserialize(src, data.x);
}

void Io<ZEC::Proto::CompressedG2>::unpack2(xmstream &src, ZEC::Proto::CompressedG2 *data, uint8_t**)
{
  unserialize(src, *data);
}

size_t Io<ZEC::Proto::PHGRProof>::getSerializedSize(const ZEC::Proto::PHGRProof &data)
{
  return BTC::getSerializedSize(data.g_A) +
    BTC::getSerializedSize(data.g_A_prime) +
    BTC::getSerializedSize(data.g_B) +
    BTC::getSerializedSize(data.g_B_prime) +
    BTC::getSerializedSize(data.g_C) +
    BTC::getSerializedSize(data.g_C_prime) +
    BTC::getSerializedSize(data.g_K) +
    BTC::getSerializedSize(data.g_H);
}

size_t Io<ZEC::Proto::PHGRProof>::getUnpackedExtraSize(xmstream &src)
{
  return BTC::Io<decltype(ZEC::Proto::PHGRProof::g_A)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::PHGRProof::g_A_prime)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::PHGRProof::g_B)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::PHGRProof::g_B_prime)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::PHGRProof::g_C)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::PHGRProof::g_C_prime)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::PHGRProof::g_K)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::PHGRProof::g_H)>::getUnpackedExtraSize(src);
}

void Io<ZEC::Proto::PHGRProof>::serialize(xmstream &dst, const ZEC::Proto::PHGRProof &data)
{
  BTC::serialize(dst, data.g_A);
  BTC::serialize(dst, data.g_A_prime);
  BTC::serialize(dst, data.g_B);
  BTC::serialize(dst, data.g_B_prime);
  BTC::serialize(dst, data.g_C);
  BTC::serialize(dst, data.g_C_prime);
  BTC::serialize(dst, data.g_K);
  BTC::serialize(dst, data.g_H);
}

void Io<ZEC::Proto::PHGRProof>::unserialize(xmstream &src, ZEC::Proto::PHGRProof &data)
{
  BTC::unserialize(src, data.g_A);
  BTC::unserialize(src, data.g_A_prime);
  BTC::unserialize(src, data.g_B);
  BTC::unserialize(src, data.g_B_prime);
  BTC::unserialize(src, data.g_C);
  BTC::unserialize(src, data.g_C_prime);
  BTC::unserialize(src, data.g_K);
  BTC::unserialize(src, data.g_H);
}

void Io<ZEC::Proto::PHGRProof>::unpack2(xmstream &src, ZEC::Proto::PHGRProof *data, uint8_t**)
{
  unserialize(src, *data);
}

size_t Io<ZEC::Proto::SpendDescription>::getSerializedSize(const ZEC::Proto::SpendDescription &data)
{
  return BTC::getSerializedSize(data.cv) +
    BTC::getSerializedSize(data.anchor) +
    BTC::getSerializedSize(data.nullifer) +
    BTC::getSerializedSize(data.rk) +
    BTC::getSerializedSize(data.zkproof) +
    BTC::getSerializedSize(data.spendAuthSig);
}

size_t Io<ZEC::Proto::SpendDescription>::getUnpackedExtraSize(xmstream &src)
{
  return BTC::Io<decltype(ZEC::Proto::SpendDescription::cv)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::SpendDescription::anchor)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::SpendDescription::nullifer)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::SpendDescription::rk)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::SpendDescription::zkproof)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::SpendDescription::spendAuthSig)>::getUnpackedExtraSize(src);
}

void Io<ZEC::Proto::SpendDescription>::serialize(xmstream &dst, const ZEC::Proto::SpendDescription &data)
{
  BTC::serialize(dst, data.cv);
  BTC::serialize(dst, data.anchor);
  BTC::serialize(dst, data.nullifer);
  BTC::serialize(dst, data.rk);
  BTC::serialize(dst, data.zkproof);
  BTC::serialize(dst, data.spendAuthSig);
}

void Io<ZEC::Proto::SpendDescription>::unserialize(xmstream &src, ZEC::Proto::SpendDescription &data)
{
  BTC::unserialize(src, data.cv);
  BTC::unserialize(src, data.anchor);
  BTC::unserialize(src, data.nullifer);
  BTC::unserialize(src, data.rk);
  BTC::unserialize(src, data.zkproof);
  BTC::unserialize(src, data.spendAuthSig);
}

void Io<ZEC::Proto::SpendDescription>::unpack2(xmstream &src, ZEC::Proto::SpendDescription *data, uint8_t **extraData)
{
  unserialize(src, *data);
}

size_t Io<ZEC::Proto::OutputDescription>::getSerializedSize(const ZEC::Proto::OutputDescription &data)
{
  return BTC::getSerializedSize(data.cv) +
    BTC::getSerializedSize(data.cmu) +
    BTC::getSerializedSize(data.ephemeralKey) +
    BTC::getSerializedSize(data.encCiphertext) +
    BTC::getSerializedSize(data.outCiphertext) +
    BTC::getSerializedSize(data.zkproof);
}

size_t Io<ZEC::Proto::OutputDescription>::getUnpackedExtraSize(xmstream &src)
{
  return BTC::Io<decltype(ZEC::Proto::OutputDescription::cv)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::OutputDescription::cmu)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::OutputDescription::ephemeralKey)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::OutputDescription::encCiphertext)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::OutputDescription::outCiphertext)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::OutputDescription::zkproof)>::getUnpackedExtraSize(src);
}

void Io<ZEC::Proto::OutputDescription>::serialize(xmstream &dst, const ZEC::Proto::OutputDescription &data)
{
  BTC::serialize(dst, data.cv);
  BTC::serialize(dst, data.cmu);
  BTC::serialize(dst, data.ephemeralKey);
  BTC::serialize(dst, data.encCiphertext);
  BTC::serialize(dst, data.outCiphertext);
  BTC::serialize(dst, data.zkproof);
}

void Io<ZEC::Proto::OutputDescription>::unserialize(xmstream &src, ZEC::Proto::OutputDescription &data)
{
  BTC::unserialize(src, data.cv);
  BTC::unserialize(src, data.cmu);
  BTC::unserialize(src, data.ephemeralKey);
  BTC::unserialize(src, data.encCiphertext);
  BTC::unserialize(src, data.outCiphertext);
  BTC::unserialize(src, data.zkproof);
}

void Io<ZEC::Proto::OutputDescription>::unpack2(xmstream &src, ZEC::Proto::OutputDescription *data, uint8_t **extraData)
{
  unserialize(src, *data);
}

size_t Io<ZEC::Proto::JSDescription, bool>::getSerializedSize(const ZEC::Proto::JSDescription &data, bool useGroth)
{
  size_t size = BTC::getSerializedSize(data.vpub_old) +
    BTC::getSerializedSize(data.vpub_new) +
    BTC::getSerializedSize(data.anchor) +
    BTC::getSerializedSize(data.nullifier1) + BTC::getSerializedSize(data.nullifier2) +
    BTC::getSerializedSize(data.commitment1) + BTC::getSerializedSize(data.commitment2) +
    BTC::getSerializedSize(data.ephemeralKey) +
    BTC::getSerializedSize(data.randomSeed) +
    BTC::getSerializedSize(data.mac1) +
    BTC::getSerializedSize(data.mac2);

  if (useGroth)
    size += BTC::getSerializedSize(data.zkproof);
  else
    size += BTC::getSerializedSize(data.phgrProof);

  size += BTC::getSerializedSize(data.ciphertext1);
  size += BTC::getSerializedSize(data.ciphertext2);
  return size;
}

size_t Io<ZEC::Proto::JSDescription, bool>::getUnpackedExtraSize(xmstream &src, bool useGroth)
{
  size_t size = BTC::Io<decltype(ZEC::Proto::JSDescription::vpub_old)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::vpub_new)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::anchor)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::nullifier1)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::nullifier2)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::commitment1)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::commitment2)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::ephemeralKey)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::randomSeed)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::mac1)>::getUnpackedExtraSize(src) +
    BTC::Io<decltype(ZEC::Proto::JSDescription::mac2)>::getUnpackedExtraSize(src);

  if (useGroth)
    size += BTC::Io<decltype(ZEC::Proto::JSDescription::zkproof)>::getUnpackedExtraSize(src);
  else
    size += BTC::Io<decltype(ZEC::Proto::JSDescription::phgrProof)>::getUnpackedExtraSize(src);

  size += BTC::Io<decltype(ZEC::Proto::JSDescription::ciphertext1)>::getUnpackedExtraSize(src);
  size += BTC::Io<decltype(ZEC::Proto::JSDescription::ciphertext2)>::getUnpackedExtraSize(src);
  return size;
}

void Io<ZEC::Proto::JSDescription, bool>::serialize(xmstream &dst, const ZEC::Proto::JSDescription &data, bool useGroth)
{
  BTC::serialize(dst, data.vpub_old);
  BTC::serialize(dst, data.vpub_new);
  BTC::serialize(dst, data.anchor);
  BTC::serialize(dst, data.nullifier1); BTC::serialize(dst, data.nullifier2);
  BTC::serialize(dst, data.commitment1); BTC::serialize(dst, data.commitment2);
  BTC::serialize(dst, data.ephemeralKey);
  BTC::serialize(dst, data.randomSeed);
  BTC::serialize(dst, data.mac1); BTC::serialize(dst, data.mac2);
  if (useGroth)
    BTC::serialize(dst, data.zkproof);
  else
    BTC::serialize(dst, data.phgrProof);

  BTC::serialize(dst, data.ciphertext1);
  BTC::serialize(dst, data.ciphertext2);
}

void Io<ZEC::Proto::JSDescription, bool>::unserialize(xmstream &src, ZEC::Proto::JSDescription &data, bool useGroth)
{
  BTC::unserialize(src, data.vpub_old);
  BTC::unserialize(src, data.vpub_new);
  BTC::unserialize(src, data.anchor);
  BTC::unserialize(src, data.nullifier1); BTC::unserialize(src, data.nullifier2);
  BTC::unserialize(src, data.commitment1); BTC::unserialize(src, data.commitment2);
  BTC::unserialize(src, data.ephemeralKey);
  BTC::unserialize(src, data.randomSeed);
  BTC::unserialize(src, data.mac1); BTC::unserialize(src, data.mac2);
  if (useGroth)
    BTC::unserialize(src, data.zkproof);
  else
    BTC::unserialize(src, data.phgrProof);

  BTC::unserialize(src, data.ciphertext1);
  BTC::unserialize(src, data.ciphertext2);
}

void Io<ZEC::Proto::JSDescription, bool>::unpack2(xmstream &src, ZEC::Proto::JSDescription *data, uint8_t **extraData, bool useGroth)
{
  unserialize(src, *data, useGroth);
}

size_t Io<ZEC::Proto::BlockHeader>::getSerializedSize(const ZEC::Proto::BlockHeader &data)
{
  return ZEC::Proto::BlockHeader::HEADER_SIZE + BTC::getSerializedSize(data.nSolution);
}

size_t Io<ZEC::Proto::BlockHeader>::getUnpackedExtraSize(xmstream &src)
{
  src.seek(ZEC::Proto::BlockHeader::HEADER_SIZE);
  return Io<xvector<uint8_t>>::getUnpackedExtraSize(src);
}

void Io<ZEC::Proto::BlockHeader>::serialize(xmstream &dst, const ZEC::Proto::BlockHeader &data)
{
  BTC::serialize(dst, data.nVersion);
  BTC::serialize(dst, data.hashPrevBlock);
  BTC::serialize(dst, data.hashMerkleRoot);
  BTC::serialize(dst, data.hashLightClientRoot);
  BTC::serialize(dst, data.nTime);
  BTC::serialize(dst, data.nBits);
  BTC::serialize(dst, data.nNonce);
  BTC::serialize(dst, data.nSolution);
}

void Io<ZEC::Proto::BlockHeader>::unserialize(xmstream &src, ZEC::Proto::BlockHeader &data)
{
  BTC::unserialize(src, data.nVersion);
  BTC::unserialize(src, data.hashPrevBlock);
  BTC::unserialize(src, data.hashMerkleRoot);
  BTC::unserialize(src, data.hashLightClientRoot);
  BTC::unserialize(src, data.nTime);
  BTC::unserialize(src, data.nBits);
  BTC::unserialize(src, data.nNonce);
  BTC::unserialize(src, data.nSolution);
}

void Io<ZEC::Proto::BlockHeader>::unpack2(xmstream &src, ZEC::Proto::BlockHeader *data, uint8_t **extraData)
{
  BTC::unserialize(src, data->nVersion);
  BTC::unserialize(src, data->hashPrevBlock);
  BTC::unserialize(src, data->hashMerkleRoot);
  BTC::unserialize(src, data->hashLightClientRoot);
  BTC::unserialize(src, data->nTime);
  BTC::unserialize(src, data->nBits);
  BTC::unserialize(src, data->nNonce);
  Io<xvector<uint8_t>>::unpack2(src, &data->nSolution, extraData);
}

size_t Io<ZEC::Proto::Transaction>::getSerializedSize(const ZEC::Proto::Transaction &data, bool)
{
  size_t size = 0;
  size += BTC::getSerializedSize(data.version);
  if (data.fOverwintered)
    size += BTC::getSerializedSize(data.nVersionGroupId);
  size += BTC::getSerializedSize(data.txIn);
  size += BTC::getSerializedSize(data.txOut);
  size += BTC::getSerializedSize(data.lockTime);

  bool isOverwinterV3 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::OVERWINTER_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::OVERWINTER_TX_VERSION;
  bool isSaplingV4 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::SAPLING_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::SAPLING_TX_VERSION;
  bool useGroth = data.fOverwintered && data.version >= ZEC::Proto::SAPLING_TX_VERSION;

  if (isOverwinterV3 || isSaplingV4)
    size += BTC::getSerializedSize(data.nExpiryHeight);
  if (isSaplingV4) {
    size += BTC::getSerializedSize(data.valueBalance);
    size += BTC::getSerializedSize(data.vShieldedSpend);
    size += BTC::getSerializedSize(data.vShieldedOutput);
  }

  if (data.version >= 2) {
    size += Io<decltype (data.vJoinSplit), bool>::getSerializedSize(data.vJoinSplit, useGroth);
    if (!data.vJoinSplit.empty()) {
      size += BTC::getSerializedSize(data.joinSplitPubKey);
      size += BTC::getSerializedSize(data.joinSplitSig);
    }
  }

  if (isSaplingV4 && !(data.vShieldedSpend.empty() && data.vShieldedOutput.empty()))
    size += BTC::getSerializedSize(data.bindingSig);

  return size;
}

size_t Io<ZEC::Proto::Transaction>::getUnpackedExtraSize(xmstream &src)
{
  uint32_t header;
  bool fOverwintered;
  int32_t version;
  uint32_t nVersionGroupId;

  BTC::unserialize(src, header);
  fOverwintered = header >> 31;
  version = header & 0x7FFFFFFF;
  if (fOverwintered) {
    BTC::unserialize(src, nVersionGroupId);
  }

  size_t size =
    Io<decltype (ZEC::Proto::Transaction::txIn)>::getUnpackedExtraSize(src) +
    Io<decltype (ZEC::Proto::Transaction::txOut)>::getUnpackedExtraSize(src);

  size += Io<decltype (ZEC::Proto::Transaction::lockTime)>::getUnpackedExtraSize(src);

  bool isOverwinterV3 =
      fOverwintered &&
      nVersionGroupId == ZEC::Proto::OVERWINTER_VERSION_GROUP_ID &&
      version == ZEC::Proto::OVERWINTER_TX_VERSION;
  bool isSaplingV4 =
      fOverwintered &&
      nVersionGroupId == ZEC::Proto::SAPLING_VERSION_GROUP_ID &&
      version == ZEC::Proto::SAPLING_TX_VERSION;
  bool useGroth = fOverwintered && version >= ZEC::Proto::SAPLING_TX_VERSION;
  if (fOverwintered && !(isOverwinterV3 || isSaplingV4)) {
    src.seekEnd(0, true);
    return 0;
  }

  uint64_t vShieldedSpendSize = 0;
  uint64_t vShieldedOutputSize = 0;
  if (isOverwinterV3 || isSaplingV4)
    size += Io<decltype (ZEC::Proto::Transaction::nExpiryHeight)>::getUnpackedExtraSize(src);
  if (isSaplingV4) {
    size += Io<decltype (ZEC::Proto::Transaction::valueBalance)>::getUnpackedExtraSize(src);
    size += Io<decltype (ZEC::Proto::Transaction::vShieldedSpend)>::getUnpackedExtraSize(src, &vShieldedSpendSize);
    size += Io<decltype (ZEC::Proto::Transaction::vShieldedOutput)>::getUnpackedExtraSize(src, &vShieldedOutputSize);
  }
  if (version >= 2) {
    uint64_t count = 0;
    size += Io<decltype (ZEC::Proto::Transaction::vJoinSplit), bool>::getUnpackedExtraSize(src, &count, useGroth);
    if (count) {
      size += Io<decltype (ZEC::Proto::Transaction::joinSplitPubKey)>::getUnpackedExtraSize(src);
      size += Io<decltype (ZEC::Proto::Transaction::joinSplitSig)>::getUnpackedExtraSize(src);
    }
  }
  if (isSaplingV4 && !(!vShieldedSpendSize && !vShieldedOutputSize))
    size += Io<decltype (ZEC::Proto::Transaction::bindingSig)>::getUnpackedExtraSize(src);

  return size;
}

void Io<ZEC::Proto::Transaction>::serialize(xmstream &dst, const ZEC::Proto::Transaction &data)
{
  uint32_t header = (static_cast<int32_t>(data.fOverwintered) << 31) | data.version;
  BTC::serialize(dst, header);

  if (data.fOverwintered)
    BTC::serialize(dst, data.nVersionGroupId);

  bool isOverwinterV3 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::OVERWINTER_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::OVERWINTER_TX_VERSION;
  bool isSaplingV4 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::SAPLING_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::SAPLING_TX_VERSION;
  bool useGroth = data.fOverwintered && data.version >= ZEC::Proto::SAPLING_TX_VERSION;

  BTC::serialize(dst, data.txIn);
  BTC::serialize(dst, data.txOut);
  BTC::serialize(dst, data.lockTime);

  if (isOverwinterV3 || isSaplingV4)
    BTC::serialize(dst, data.nExpiryHeight);
  if (isSaplingV4) {
    BTC::serialize(dst, data.valueBalance);
    BTC::serialize(dst, data.vShieldedSpend);
    BTC::serialize(dst, data.vShieldedOutput);
  }
  if (data.version >= 2) {
    BTC::Io<decltype (data.vJoinSplit), bool>::serialize(dst, data.vJoinSplit, useGroth);
    if (!data.vJoinSplit.empty()) {
      BTC::serialize(dst, data.joinSplitPubKey);
      BTC::serialize(dst, data.joinSplitSig);
    }
  }
  if (isSaplingV4 && !(data.vShieldedSpend.empty() && data.vShieldedOutput.empty()))
    BTC::serialize(dst, data.bindingSig);
}

void Io<ZEC::Proto::Transaction>::unserialize(xmstream &src, ZEC::Proto::Transaction &data)
{
  uint32_t header;
  BTC::unserialize(src, header);
  data.fOverwintered = header >> 31;
  data.version = header & 0x7FFFFFFF;

  if (data.fOverwintered)
    BTC::unserialize(src, data.nVersionGroupId);

  bool isOverwinterV3 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::OVERWINTER_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::OVERWINTER_TX_VERSION;
  bool isSaplingV4 =
      data.fOverwintered &&
      data.nVersionGroupId == ZEC::Proto::SAPLING_VERSION_GROUP_ID &&
      data.version == ZEC::Proto::SAPLING_TX_VERSION;
  bool useGroth = data.fOverwintered && data.version >= ZEC::Proto::SAPLING_TX_VERSION;

  if (data.fOverwintered && !(isOverwinterV3 || isSaplingV4)) {
    src.seekEnd(0, true);
    return;
  }

  BTC::unserialize(src, data.txIn);
  BTC::unserialize(src, data.txOut);
  BTC::unserialize(src, data.lockTime);

  if (isOverwinterV3 || isSaplingV4)
    BTC::unserialize(src, data.nExpiryHeight);
  if (isSaplingV4) {
    BTC::unserialize(src, data.valueBalance);
    BTC::unserialize(src, data.vShieldedSpend);
    BTC::unserialize(src, data.vShieldedOutput);
  }
  if (data.version >= 2) {
    BTC::Io<decltype (data.vJoinSplit), bool>::unserialize(src, data.vJoinSplit, useGroth);
    if (!data.vJoinSplit.empty()) {
      BTC::unserialize(src, data.joinSplitPubKey);
      BTC::unserialize(src, data.joinSplitSig);
    }
  }
  if (isSaplingV4 && !(data.vShieldedSpend.empty() && data.vShieldedOutput.empty()))
    BTC::unserialize(src, data.bindingSig);
}

void Io<ZEC::Proto::Transaction>::unpack2(xmstream &src, ZEC::Proto::Transaction *data, uint8_t **extraData)
{
  uint32_t header;
  BTC::unserialize(src, header);
  data->fOverwintered = header >> 31;
  data->version = header & 0x7FFFFFFF;

  if (data->fOverwintered)
    BTC::unserialize(src, data->nVersionGroupId);

  bool isOverwinterV3 =
      data->fOverwintered &&
      data->nVersionGroupId == ZEC::Proto::OVERWINTER_VERSION_GROUP_ID &&
      data->version == ZEC::Proto::OVERWINTER_TX_VERSION;
  bool isSaplingV4 =
      data->fOverwintered &&
      data->nVersionGroupId == ZEC::Proto::SAPLING_VERSION_GROUP_ID &&
      data->version == ZEC::Proto::SAPLING_TX_VERSION;
  bool useGroth = data->fOverwintered && data->version >= ZEC::Proto::SAPLING_TX_VERSION;

  if (data->fOverwintered && !(isOverwinterV3 || isSaplingV4)) {
    src.seekEnd(0, true);
    return;
  }

  Io<decltype (data->txIn)>::unpack2(src, &data->txIn, extraData);
  Io<decltype (data->txOut)>::unpack2(src, &data->txOut, extraData);
  BTC::unserialize(src, data->lockTime);

  new(&data->vShieldedSpend) xvector<ZEC::Proto::SpendDescription>;
  new(&data->vShieldedOutput) xvector<ZEC::Proto::OutputDescription>;
  new(&data->vJoinSplit) xvector<ZEC::Proto::JSDescription>;

  if (isOverwinterV3 || isSaplingV4)
    BTC::unserialize(src, data->nExpiryHeight);
  if (isSaplingV4) {
    BTC::unserialize(src, data->valueBalance);
    BTC::Io<decltype (data->vShieldedSpend)>::unpack2(src, &data->vShieldedSpend, extraData);
    BTC::Io<decltype (data->vShieldedOutput)>::unpack2(src, &data->vShieldedOutput, extraData);
  }
  if (data->version >= 2) {
    BTC::Io<decltype (data->vJoinSplit), bool>::unpack2(src, &data->vJoinSplit, extraData, useGroth);
    if (!data->vJoinSplit.empty()) {
      BTC::unserialize(src, data->joinSplitPubKey);
      BTC::unserialize(src, data->joinSplitSig);
    }
  }
  if (isSaplingV4 && !(data->vShieldedSpend.empty() && data->vShieldedOutput.empty()))
    BTC::unserialize(src, data->bindingSig);
}
}

namespace ZEC {
Proto::BlockHashTy Proto::Transaction::getTxId() const
{
  uint256 result;
  uint8_t buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  stream.reset();
  BTC::Io<Proto::Transaction>::serialize(stream, *this);

  SHA256_CTX sha256;
  SHA256_Init(&sha256);
  SHA256_Update(&sha256, stream.data(), stream.sizeOf());
  SHA256_Final(result.begin(), &sha256);
  SHA256_Init(&sha256);
  SHA256_Update(&sha256, result.begin(), sizeof(result));
  SHA256_Final(result.begin(), &sha256);
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
