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

using HashTy = BaseBlob<256>;
using BlindingFactorTy = BaseBlob<256>;
using CommitmentTy = BaseBlob<264>;
using PublicKeyTy = BaseBlob<264>;
using SignatureTy = BaseBlob<512>;
// The nonce masking an output value
using NonceTy = BaseBlob<128>;
// Unlike every other byte string here, written without a length prefix
using RangeProofTy = std::array<uint8_t, 675>;

struct CInput {
  enum FeatureBit {
    StealthKeyFeatureBit = 0x01,
    ExtraDataFeatureBit = 0x02
  };

  uint8_t Features = 0;
  HashTy OutputId;
  CommitmentTy Commitment;
  PublicKeyTy OutputPubKey;
  PublicKeyTy InputPubKey;
  xvector<uint8_t> ExtraData;
  SignatureTy Signature;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.Features);
    op.io(d.OutputId);
    op.io(d.Commitment);
    op.io(d.OutputPubKey);
    if (d.Features & StealthKeyFeatureBit)
      op.io(d.InputPubKey);
    if (d.Features & ExtraDataFeatureBit)
      op.io(d.ExtraData);
    op.io(d.Signature);
  }
};

// What the receiver decrypts to recover the value, masked with keys only the two parties derive
struct COutputMessage {
  enum FeatureBit {
    StandardFieldsFeatureBit = 0x01,
    ExtraDataFeatureBit = 0x02
  };

  uint8_t Features = 0;
  PublicKeyTy KeyExchangePubKey;
  uint8_t ViewTag = 0;
  uint64_t MaskedValue = 0;
  NonceTy MaskedNonce;
  xvector<uint8_t> ExtraData;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.Features);
    if (d.Features & StandardFieldsFeatureBit) {
      op.io(d.KeyExchangePubKey);
      op.io(d.ViewTag);
      op.io(d.MaskedValue);
      op.io(d.MaskedNonce);
    }
    if (d.Features & ExtraDataFeatureBit)
      op.io(d.ExtraData);
  }
};

struct COutput {
  CommitmentTy Commitment;
  PublicKeyTy SenderPubKey;
  PublicKeyTy ReceiverPubKey;
  COutputMessage Message;
  RangeProofTy RangeProof;
  SignatureTy Signature;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.Commitment);
    op.io(d.SenderPubKey);
    op.io(d.ReceiverPubKey);
    op.io(d.Message);
    op.io(d.RangeProof);
    op.io(d.Signature);
  }
};

// Coins leaving the extension block for the canonical chain
struct CPegOutCoin {
  int64_t Amount = 0;
  xvector<uint8_t> PkScript;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.varint(d.Amount);
    // an empty pegout script is rejected, as Core does
    op.check(op.vec(d.PkScript) != 0);
  }
};

struct CKernel {
  enum FeatureBit {
    FeeFeatureBit = 0x01,
    PegInFeatureBit = 0x02,
    PegOutFeatureBit = 0x04,
    HeightLockFeatureBit = 0x08,
    StealthExcessFeatureBit = 0x10,
    ExtraDataFeatureBit = 0x20
  };

  uint8_t Features = 0;
  int64_t Fee = 0;
  int64_t PegIn = 0;
  xvector<CPegOutCoin> PegOuts;
  int32_t LockHeight = 0;
  PublicKeyTy StealthExcess;
  xvector<uint8_t> ExtraData;
  // Remainder of the commitment sum, and the signature proving it is a valid public key
  CommitmentTy Excess;
  SignatureTy Signature;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    // Core writes the optional groups by presence and reads them by feature bit; driving both
    // off the bits round trips exactly, including a bit set over an empty group
    op.io(d.Features);
    if (d.Features & FeeFeatureBit)
      op.varint(d.Fee);
    if (d.Features & PegInFeatureBit)
      op.varint(d.PegIn);
    if (d.Features & PegOutFeatureBit)
      op.io(d.PegOuts);
    if (d.Features & HeightLockFeatureBit)
      op.varint(d.LockHeight);
    if (d.Features & StealthExcessFeatureBit)
      op.io(d.StealthExcess);
    if (d.Features & ExtraDataFeatureBit)
      op.io(d.ExtraData);
    op.io(d.Excess);
    op.io(d.Signature);
  }
};

// Shared by a transaction and an extension block
struct CTxBody {
  xvector<CInput> Inputs;
  xvector<COutput> Outputs;
  xvector<CKernel> Kernels;

  // The count goes out through the context: a transaction rejects a body with no kernels, and
  // the measuring pass builds no elements to count afterwards
  template<typename Op, typename Self>
  static void io(Op &op, Self &d, size_t *kernelCount = nullptr) {
    op.vec(d.Inputs);
    op.vec(d.Outputs);
    size_t kernels = op.vec(d.Kernels);
    if (kernelCount)
      *kernelCount = kernels;
  }
};

struct CTransaction {
  BlindingFactorTy KernelOffset;
  BlindingFactorTy StealthOffset;
  CTxBody Body;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.KernelOffset);
    op.io(d.StealthOffset);
    size_t kernelCount = 0;
    op.io(d.Body, &kernelCount);
    // a transaction with no kernel is rejected, as Core does
    op.check(kernelCount != 0);
  }
};

struct CHeader {
  int32_t Height = 0;
  HashTy OutputRoot;
  HashTy KernelRoot;
  HashTy LeafsetRoot;
  BlindingFactorTy KernelOffset;
  BlindingFactorTy StealthOffset;
  uint64_t OutputMmrSize = 0;
  uint64_t KernelMmrSize = 0;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.varint(d.Height);
    op.io(d.OutputRoot);
    op.io(d.KernelRoot);
    op.io(d.LeafsetRoot);
    op.io(d.KernelOffset);
    op.io(d.StealthOffset);
    op.varint(d.OutputMmrSize);
    op.varint(d.KernelMmrSize);
  }
};

struct CBlock {
  CHeader Header;
  CTxBody Body;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.Header);
    op.io(d.Body);
  }
};

}
}

// For HTTP API
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CInput &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::COutput &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CPegOutCoin &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CKernel &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CTransaction &data);
void serializeJson(xmstream &stream, const char *fieldName, const LTC::MWeb::CBlock &data);
