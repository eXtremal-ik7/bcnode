// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BTC/serialize.h"
#include "common/baseBlob.h"
#include "common/xvector.h"
#include <array>

// MimbleWimble extension blocks, wire format of litecoin/src/libmw/include/mw.
//
// An optional MWEB object (Core's OptionalPtr: a presence byte, then the object) is read here
// as a vector of at most one element — the compact size of 0 and 1 is that same byte — so the
// payload takes arena space only when it is there.
//
// Ids, commitments and signatures are carried as opaque bytes; checking them would mean blake3,
// bulletproofs and MMRs, which this node does not do.

namespace LTC {
namespace MWeb {

using Hash = BaseBlob<256>;
using BlindingFactor = BaseBlob<256>;
using Commitment = BaseBlob<264>;
using PublicKey = BaseBlob<264>;
using Signature = BaseBlob<512>;
// The nonce masking an output value
using Nonce = BaseBlob<128>;
// Unlike every other byte string here, written without a length prefix
using RangeProof = std::array<uint8_t, 675>;

struct Input {
  enum FeatureBit {
    StealthKeyFeatureBit = 0x01,
    ExtraDataFeatureBit = 0x02
  };

  uint8_t features = 0;
  Hash outputId;
  Commitment commitment;
  PublicKey outputPubKey;
  PublicKey inputPubKey;
  xvector<uint8_t> extraData;
  Signature signature;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.features);
    op.io(d.outputId);
    op.io(d.commitment);
    op.io(d.outputPubKey);
    if (d.features & StealthKeyFeatureBit)
      op.io(d.inputPubKey);
    if (d.features & ExtraDataFeatureBit)
      op.io(d.extraData);
    op.io(d.signature);
  }
};

// What the receiver decrypts to recover the value, masked with keys only the two parties derive
struct OutputMessage {
  enum FeatureBit {
    StandardFieldsFeatureBit = 0x01,
    ExtraDataFeatureBit = 0x02
  };

  uint8_t features = 0;
  PublicKey keyExchangePubKey;
  uint8_t viewTag = 0;
  uint64_t maskedValue = 0;
  Nonce maskedNonce;
  xvector<uint8_t> extraData;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.features);
    if (d.features & StandardFieldsFeatureBit) {
      op.io(d.keyExchangePubKey);
      op.io(d.viewTag);
      op.io(d.maskedValue);
      op.io(d.maskedNonce);
    }
    if (d.features & ExtraDataFeatureBit)
      op.io(d.extraData);
  }
};

struct Output {
  Commitment commitment;
  PublicKey senderPubKey;
  PublicKey receiverPubKey;
  OutputMessage message;
  RangeProof rangeProof;
  Signature signature;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.commitment);
    op.io(d.senderPubKey);
    op.io(d.receiverPubKey);
    op.io(d.message);
    op.io(d.rangeProof);
    op.io(d.signature);
  }
};

// Coins leaving the extension block for the canonical chain
struct PegOutCoin {
  int64_t amount = 0;
  xvector<uint8_t> pkScript;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.varint(d.amount);
    // an empty pegout script is rejected, as Core does
    op.check(op.vec(d.pkScript) != 0);
  }
};

struct Kernel {
  enum FeatureBit {
    FeeFeatureBit = 0x01,
    PegInFeatureBit = 0x02,
    PegOutFeatureBit = 0x04,
    HeightLockFeatureBit = 0x08,
    StealthExcessFeatureBit = 0x10,
    ExtraDataFeatureBit = 0x20
  };

  uint8_t features = 0;
  int64_t fee = 0;
  int64_t pegIn = 0;
  xvector<PegOutCoin> pegOuts;
  int32_t lockHeight = 0;
  PublicKey stealthExcess;
  xvector<uint8_t> extraData;
  // Remainder of the commitment sum, and the signature proving it is a valid public key
  Commitment excess;
  Signature signature;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    // Core writes the optional groups by presence and reads them by feature bit; driving both
    // off the bits round trips exactly, including a bit set over an empty group
    op.io(d.features);
    if (d.features & FeeFeatureBit)
      op.varint(d.fee);
    if (d.features & PegInFeatureBit)
      op.varint(d.pegIn);
    if (d.features & PegOutFeatureBit)
      op.io(d.pegOuts);
    if (d.features & HeightLockFeatureBit)
      op.varint(d.lockHeight);
    if (d.features & StealthExcessFeatureBit)
      op.io(d.stealthExcess);
    if (d.features & ExtraDataFeatureBit)
      op.io(d.extraData);
    op.io(d.excess);
    op.io(d.signature);
  }
};

// Shared by a transaction and an extension block
struct TxBody {
  xvector<Input> inputs;
  xvector<Output> outputs;
  xvector<Kernel> kernels;

  // The count goes out through the context: a transaction rejects a body with no kernels, and
  // the measuring pass builds no elements to count afterwards
  template<typename Op, typename Self>
  static void io(Op &op, Self &d, size_t *kernelCount = nullptr) {
    op.vec(d.inputs);
    op.vec(d.outputs);
    size_t kernels = op.vec(d.kernels);
    if (kernelCount)
      *kernelCount = kernels;
  }
};

struct Transaction {
  BlindingFactor kernelOffset;
  BlindingFactor stealthOffset;
  TxBody body;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.kernelOffset);
    op.io(d.stealthOffset);
    size_t kernelCount = 0;
    op.io(d.body, &kernelCount);
    // a transaction with no kernel is rejected, as Core does
    op.check(kernelCount != 0);
  }
};

struct Header {
  int32_t height = 0;
  Hash outputRoot;
  Hash kernelRoot;
  Hash leafsetRoot;
  BlindingFactor kernelOffset;
  BlindingFactor stealthOffset;
  uint64_t outputMmrSize = 0;
  uint64_t kernelMmrSize = 0;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.varint(d.height);
    op.io(d.outputRoot);
    op.io(d.kernelRoot);
    op.io(d.leafsetRoot);
    op.io(d.kernelOffset);
    op.io(d.stealthOffset);
    op.varint(d.outputMmrSize);
    op.varint(d.kernelMmrSize);
  }
};

struct Block {
  Header header;
  TxBody body;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.header);
    op.io(d.body);
  }
};

}
}

// For HTTP API
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Input &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Output &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::PegOutCoin &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Kernel &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Transaction &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::Block &data);
