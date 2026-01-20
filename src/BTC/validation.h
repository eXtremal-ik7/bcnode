#pragma once

#include "proto.h"
#include "script.h"
#include "common/merkleTree.h"
#include <limits>

namespace BTC {

template<typename BlockTy>
void validationDataInitialize(const BlockTy &block, BTC::Proto::CBlockValidationData &validation)
{
  validation.HasWitnessData = false;
  validation.TxData.resize(block.vtx.size());
  for (size_t i = 0; i < block.vtx.size(); i++) {
    validation.TxData[i].ScriptSigKnownValid.resize(block.vtx[i].txIn.size());
    for (auto &v: validation.TxData[i].ScriptSigKnownValid) {
      v.ScriptSigKnownValid = false;
    }
  }
}


template<typename BlockTy>
bool validateBlockSize(const BlockTy &block, size_t limit, std::string &error)
{
  bool result = BTC::Io<BlockTy>::getSerializedSize(block, false) <= limit;
  if (!result)
    error = "bad-blocksize";
  return result;
}

template<typename BlockTy>
bool validateMerkleRoot(const BlockTy &block, std::string &error) {
  bool result = calculateBlockMerkleRoot(block) == block.header.hashMerkleRoot;
  if (!result)
    error = "bad-merkleroot";
  return result;
}

template<typename BlockTy>
bool validateWitnessCommitment(const BlockTy &block, bool &hasWitness, std::string &error) {
  if (block.vtx.empty() || block.vtx[0].txIn.empty())
    return false;
  const auto &coinbaseTxIn = block.vtx[0].txIn[0];

  // Get commitment txout index
  size_t commitmentPos = std::numeric_limits<size_t>::max();
  for (size_t i = 0, ie = block.vtx[0].txOut.size(); i != ie; ++i) {
    const xvector<uint8_t> &pkScript = block.vtx[0].txOut[i].pkScript;
    if (pkScript.size() >= 38 &&
        pkScript[0] == BTC::Script::OP_RETURN &&
        pkScript[1] == 0x24 &&
        pkScript[2] == 0xaa &&
        pkScript[3] == 0x21 &&
        pkScript[4] == 0xa9 &&
        pkScript[5] == 0xed) {
      // Store last found witness commitment index
      commitmentPos = i;
    }
  }

  if (commitmentPos == std::numeric_limits<size_t>::max()) {
    for (size_t i = 0, ie = block.vtx.size(); i != ie; ++i) {
      const auto &tx = block.vtx[i];
      for (size_t j = 0, je = tx.txIn.size(); j != je; ++j) {
        const auto &txIn = tx.txIn[j];
        if (!txIn.witnessStack.empty()) {
          error = "witness-data-without-commitment";
          return false;
        }
      }
    }
    return true;
  }

  hasWitness = true;

  const uint8_t *commitmentData = block.vtx[0].txOut[commitmentPos].pkScript.data();

  // Check witness nonce
  if (coinbaseTxIn.witnessStack.size() != 1 || coinbaseTxIn.witnessStack[0].size() != 32) {
    error = "bad-witness-nonce";
    return false;
  }
  const uint8_t *witnessNonce = coinbaseTxIn.witnessStack[0].data();

  // Calculate witness merkle root
  BaseBlob<256> witnessMerkleRoot = calculateBlockWitnessMerkleRoot(block);
  // Calculate witness commitment
  BaseBlob<256> commitment;
  {
    CCtxSha256 ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, witnessMerkleRoot.begin(), witnessMerkleRoot.size());
    sha256Update(&ctx, witnessNonce, 32);
    sha256Final(&ctx, commitment.begin());
    sha256Init(&ctx);
    sha256Update(&ctx, commitment.begin(), commitment.size());
    sha256Final(&ctx, commitment.begin());
  }

  bool result = memcmp(commitment.begin(), commitmentData+6, 32) == 0;
  if (!result)
    error = "bad-witness-commitment";
  return result;
}

template<typename BlockTy>
bool validateBIP34(uint32_t height, const BlockTy &block, uint32_t bip34Height, std::string &error) {
  if (height < bip34Height)
    return true;

  if (block.vtx.empty() || block.vtx[0].txIn.empty()) {
    error = "coinbase-height-missing";
    return false;
  }

  auto &coinbaseTxIn = block.vtx[0].txIn[0];

  xmstream src(coinbaseTxIn.scriptSig.data(), coinbaseTxIn.scriptSig.size());

  // Read size followed by little endian number
  uint8_t size = src.read<uint8_t>();
  uint64_t v = 0;
  for (uint8_t i = 0; i < size; i++)
    v |= (static_cast<uint64_t>(src.read<uint8_t>()) << 8*i);

  bool result = !src.eof() && v == height;
  if (!result)
    error = "coinbase-height-mismatch";
  return result;
}

static inline bool validateUnexpectedWitness(uint32_t height, bool hasWitnessData, uint32_t segwitHeight, std::string &error) {
  bool result = !(height < segwitHeight && hasWitnessData);
  error = "unexpected-witness-data";
  return result;
}
}

template<typename X>
bool validateUnexpectedWitness(const typename X::BlockIndex &index, const typename X::Proto::Block &block, const typename X::ChainParams &chainParams, std::string &error) {
  bool result = !(index.Height < chainParams.SegwitHeight && block.validationData.HasWitness);
  error = "unexpected-witness-data";
  return result;
}

template<typename X>
bool validateScriptSig(const typename X::Proto::ValidationData&, const typename X::Proto::Transaction&, const typename X::ChainParams&, std::string&)
{
  return true;
}

template<typename X>
bool validateAmount(const typename X::Proto::ValidationData&, const typename X::Proto::Transaction&, const typename X::ChainParams&, std::string&)
{
  return true;
}
