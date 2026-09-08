// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BTC/serialize.h"
#include "BTC/proto.h"

namespace ZEC {
class Proto {
public:
  using BlockHashTy = BTC::Proto::BlockHashTy;
  using TxHashTy = BTC::Proto::TxHashTy;
  using AddressTy = BTC::Proto::AddressTy;
  using BalanceType = BTC::Proto::BalanceType;

  static constexpr uint32_t OVERWINTER_VERSION_GROUP_ID = 0x03C48270;
  static constexpr uint32_t SAPLING_VERSION_GROUP_ID = 0x892F2085;
  static constexpr int32_t OVERWINTER_TX_VERSION = 3;
  static constexpr int32_t SAPLING_TX_VERSION = 4;

  static constexpr uint8_t G1_PREFIX_MASK = 0x02;
  static constexpr uint8_t G2_PREFIX_MASK = 0x0a;

  static constexpr size_t ZC_NUM_JS_INPUTS = 2;
  static constexpr size_t ZC_NUM_JS_OUTPUTS = 2;
  static constexpr size_t INCREMENTAL_MERKLE_TREE_DEPTH = 29;
  static constexpr size_t INCREMENTAL_MERKLE_TREE_DEPTH_TESTING = 4;
  static constexpr size_t SAPLING_INCREMENTAL_MERKLE_TREE_DEPTH = 32;
  static constexpr size_t NOTEENCRYPTION_AUTH_BYTES = 16;
  static constexpr size_t ZC_NOTEPLAINTEXT_LEADING = 1;
  static constexpr size_t ZC_V_SIZE = 8;
  static constexpr size_t ZC_RHO_SIZE = 32;
  static constexpr size_t ZC_R_SIZE = 32;
  static constexpr size_t ZC_MEMO_SIZE = 512;
  static constexpr size_t ZC_DIVERSIFIER_SIZE = 11;
  static constexpr size_t ZC_JUBJUB_POINT_SIZE = 32;
  static constexpr size_t ZC_JUBJUB_SCALAR_SIZE = 32;
  static constexpr size_t ZC_NOTEPLAINTEXT_SIZE = ZC_NOTEPLAINTEXT_LEADING + ZC_V_SIZE + ZC_RHO_SIZE + ZC_R_SIZE + ZC_MEMO_SIZE;
  static constexpr size_t ZC_SAPLING_ENCPLAINTEXT_SIZE = ZC_NOTEPLAINTEXT_LEADING + ZC_DIVERSIFIER_SIZE + ZC_V_SIZE + ZC_R_SIZE + ZC_MEMO_SIZE;
  static constexpr size_t ZC_SAPLING_OUTPLAINTEXT_SIZE = ZC_JUBJUB_POINT_SIZE + ZC_JUBJUB_SCALAR_SIZE;
  static constexpr size_t ZC_SAPLING_ENCCIPHERTEXT_SIZE = ZC_SAPLING_ENCPLAINTEXT_SIZE + NOTEENCRYPTION_AUTH_BYTES;
  static constexpr size_t ZC_SAPLING_OUTCIPHERTEXT_SIZE = ZC_SAPLING_OUTPLAINTEXT_SIZE + NOTEENCRYPTION_AUTH_BYTES;

  static constexpr size_t GROTH_PROOF_SIZE = (
      48 + // π_A
      96 + // π_B
      48); // π_C

  template<size_t MLEN>
  // Spec names: epk and esk are the ephemeral public and secret keys, hSig the signature hash
  struct CNoteEncryption {
      enum { CLEN=MLEN+NOTEENCRYPTION_AUTH_BYTES };
      BaseBlob<256> Epk;
      BaseBlob<256> Esk;
      unsigned char Nonce;
      BaseBlob<256> HSig;
  };

  using CZCNoteEncryption = CNoteEncryption<ZC_NOTEPLAINTEXT_SIZE>;

#pragma pack(push, 1)
  struct CBlockHeader {
  public:
    static constexpr size_t HEADER_SIZE = 4+32+32+32+4+4+32;

  public:
    int32_t Version;
    BaseBlob<256> HashPrevBlock;
    BaseBlob<256> HashMerkleRoot;
    BaseBlob<256> HashLightClientRoot;
    uint32_t Time;
    uint32_t Bits;
    BaseBlob<256> Nonce;
    xvector<uint8_t> Solution;

    BlockHashTy GetHash() const {
      SmallStream<2048> localStream;
      BTC::serialize(localStream, Solution);
      return BTC::sha256d(this, HEADER_SIZE, localStream.data(), localStream.sizeOf());
    }

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Version);
      op.io(d.HashPrevBlock);
      op.io(d.HashMerkleRoot);
      op.io(d.HashLightClientRoot);
      op.io(d.Time);
      op.io(d.Bits);
      op.io(d.Nonce);
      op.io(d.Solution);
    }
  };
#pragma pack(pop)

  // GetHash hashes the first HEADER_SIZE bytes of the object: layout must stay equal to the
  // wire prefix
  static_assert(sizeof(CBlockHeader) == CBlockHeader::HEADER_SIZE + sizeof(xvector<uint8_t>));

  using CBlockHeaderNet = BTC::Proto::CBlockHeaderNetTy<ZEC::Proto>;
  using CBlock = BTC::Proto::CBlockTy<ZEC::Proto>;
  using CNetworkAddress = BTC::Proto::CNetworkAddress;
  using CInventoryVector = BTC::Proto::CInventoryVector;
  // CTxIn & CTxOut compatible with BTC, witness stack will not used
  using CTxIn = BTC::Proto::CTxIn;
  using CTxOut = BTC::Proto::CTxOut;

  using CBlockValidationData = BTC::Proto::CBlockValidationData;
  using CBlockLinkedOutputs = BTC::Proto::CBlockLinkedOutputs;
  using CTxLinkedOutputs = BTC::Proto::CTxLinkedOutputs;

  struct CCompressedG1 {
    bool YLsb;
    BaseBlob<256> X;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      // the y bit lives in a validated prefix byte
      if constexpr (Op::Writing) {
        uint8_t leadingByte = G1_PREFIX_MASK;
        if (d.YLsb)
          leadingByte |= 1;
        op.put(leadingByte);
      } else {
        uint8_t leadingByte = 0;
        op.get(leadingByte);
        op.check((leadingByte & ~1) == G1_PREFIX_MASK);
        d.YLsb = leadingByte & 1;
      }
      op.io(d.X);
    }
  };

  struct CCompressedG2 {
    bool YGt;
    BaseBlob<512> X;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      if constexpr (Op::Writing) {
        uint8_t leadingByte = G2_PREFIX_MASK;
        if (d.YGt)
          leadingByte |= 1;
        op.put(leadingByte);
      } else {
        uint8_t leadingByte = 0;
        op.get(leadingByte);
        op.check((leadingByte & ~1) == G2_PREFIX_MASK);
        d.YGt = leadingByte & 1;
      }
      op.io(d.X);
    }
  };

  // The eight PHGR proof elements, spec g_A, g_A', g_B, g_B', g_C, g_C', g_K, g_H
  struct CPHGRProof {
    CCompressedG1 GA;
    CCompressedG1 GAPrime;
    CCompressedG2 GB;
    CCompressedG1 GBPrime;
    CCompressedG1 GC;
    CCompressedG1 GCPrime;
    CCompressedG1 GK;
    CCompressedG1 GH;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.GA);
      op.io(d.GAPrime);
      op.io(d.GB);
      op.io(d.GBPrime);
      op.io(d.GC);
      op.io(d.GCPrime);
      op.io(d.GK);
      op.io(d.GH);
    }
  };

  // Spec names: cv is the value commitment, rk the randomized spend key
  struct CSpendDescription {
    BaseBlob<256> Cv;
    BaseBlob<256> Anchor;
    BaseBlob<256> Nullifier;
    BaseBlob<256> Rk;
    std::array<uint8_t, GROTH_PROOF_SIZE> ZkProof;
    std::array<uint8_t, 64> SpendAuthSig;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Cv);
      op.io(d.Anchor);
      op.io(d.Nullifier);
      op.io(d.Rk);
      op.io(d.ZkProof);
      op.io(d.SpendAuthSig);
    }
  };

  struct COutputDescription {
    BaseBlob<256> Cv;
    BaseBlob<256> Cmu;
    BaseBlob<256> EphemeralKey;
    std::array<uint8_t, ZC_SAPLING_ENCCIPHERTEXT_SIZE> EncCiphertext;
    std::array<uint8_t, ZC_SAPLING_OUTCIPHERTEXT_SIZE> OutCiphertext;
    std::array<uint8_t, GROTH_PROOF_SIZE> ZkProof;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Cv);
      op.io(d.Cmu);
      op.io(d.EphemeralKey);
      op.io(d.EncCiphertext);
      op.io(d.OutCiphertext);
      op.io(d.ZkProof);
    }
  };

  // Spec names of the two transparent amounts are vpub_old and vpub_new: what the joinsplit
  // takes out of the transparent pool and what it puts back
  struct CJSDescription {
    int64_t VpubOld;
    int64_t VpubNew;
    BaseBlob<256> Anchor;
    BaseBlob<256> Nullifier1;
    BaseBlob<256> Nullifier2;
    BaseBlob<256> Commitment1;
    BaseBlob<256> Commitment2;
    BaseBlob<256> EphemeralKey;
    std::array<uint8_t, CZCNoteEncryption::CLEN> Ciphertext1;
    std::array<uint8_t, CZCNoteEncryption::CLEN> Ciphertext2;
    BaseBlob<256> RandomSeed;
    BaseBlob<256> Mac1;
    BaseBlob<256> Mac2;

    CPHGRProof PhgrProof;
    std::array<uint8_t, GROTH_PROOF_SIZE> ZkProof;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool useGroth) {
      op.io(d.VpubOld);
      op.io(d.VpubNew);
      op.io(d.Anchor);
      op.io(d.Nullifier1);
      op.io(d.Nullifier2);
      op.io(d.Commitment1);
      op.io(d.Commitment2);
      op.io(d.EphemeralKey);
      op.io(d.RandomSeed);
      op.io(d.Mac1);
      op.io(d.Mac2);
      // the proof representation is picked by the transaction the description belongs to
      if (useGroth)
        op.io(d.ZkProof);
      else
        op.io(d.PhgrProof);
      op.io(d.Ciphertext1);
      op.io(d.Ciphertext2);
    }
  };

  struct CTransaction {
    bool Overwintered;
    int32_t Version;
    uint32_t VersionGroupId;
    xvector<CTxIn> TxIn;
    xvector<CTxOut> TxOut;
    uint32_t LockTime;
    uint32_t ExpiryHeight;
    int64_t ValueBalance;
    xvector<CSpendDescription> ShieldedSpends;
    xvector<COutputDescription> ShieldedOutputs;
    xvector<CJSDescription> JoinSplits;
    std::array<uint8_t, 32> JoinSplitPubKey;
    std::array<uint8_t, 64> JoinSplitSig;
    std::array<uint8_t, 64> BindingSig;

    BlockHashTy getTxId() const;
    // ZEC has no witness data, wtxid is always the same as txid
    BlockHashTy getWTxid() const { return getTxId(); }

    // The witness flag of the common block path is accepted and ignored
    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool = true) {
      // Overwintered is packed into the sign bit of the version word
      uint32_t header;
      if constexpr (Op::Writing) {
        header = (static_cast<uint32_t>(d.Overwintered) << 31) | static_cast<uint32_t>(d.Version);
        op.put(header);
      } else {
        header = 0;
        op.get(header);
        d.Overwintered = header >> 31;
        d.Version = header & 0x7FFFFFFF;
      }

      if (d.Overwintered)
        op.io(d.VersionGroupId);

      bool isOverwinterV3 = d.Overwintered &&
          d.VersionGroupId == OVERWINTER_VERSION_GROUP_ID &&
          d.Version == OVERWINTER_TX_VERSION;
      bool isSaplingV4 =
          d.Overwintered &&
          d.VersionGroupId == SAPLING_VERSION_GROUP_ID &&
          d.Version == SAPLING_TX_VERSION;
      bool useGroth = d.Overwintered && d.Version >= SAPLING_TX_VERSION;

      if constexpr (!Op::Writing) {
        // an overwintered transaction of an unknown version group is unparsable
        if (d.Overwintered && !(isOverwinterV3 || isSaplingV4)) {
          op.check(false);
          return;
        }
      }

      op.io(d.TxIn);
      op.io(d.TxOut);
      op.io(d.LockTime);

      if (isOverwinterV3 || isSaplingV4)
        op.io(d.ExpiryHeight);

      size_t shieldedSpends = 0;
      size_t shieldedOutputs = 0;
      if (isSaplingV4) {
        op.io(d.ValueBalance);
        shieldedSpends = op.vec(d.ShieldedSpends);
        shieldedOutputs = op.vec(d.ShieldedOutputs);
      }

      if (d.Version >= 2) {
        if (op.vec(d.JoinSplits, useGroth) != 0) {
          op.io(d.JoinSplitPubKey);
          op.io(d.JoinSplitSig);
        }
      }

      if (isSaplingV4 && (shieldedSpends != 0 || shieldedOutputs != 0))
        op.io(d.BindingSig);
    }
  };

  using CMessageVersion = BTC::Proto::CMessageVersion;
  using CMessagePing = BTC::Proto::CMessagePing;
  using CMessagePong = BTC::Proto::CMessagePong;
  using CMessageAddr = BTC::Proto::CMessageAddr;
  using CMessageGetHeaders = BTC::Proto::CMessageGetHeaders;
  using CMessageGetBlocks = BTC::Proto::CMessageGetBlocks;
  using CMessageInv = BTC::Proto::CMessageInv;
  using CMessageBlock = BTC::Proto::CMessageBlock;
  using CMessageGetData = BTC::Proto::CMessageGetData;
  using CMessageReject = BTC::Proto::CMessageReject;
  using CMessageHeaders = BTC::Proto::CMessageHeadersTy<ZEC::Proto>;
};
}

void serializeJson(xmstream &stream, const char *fieldName, const ZEC::Proto::CTransaction &data);
void serializeJsonInside(xmstream &stream, const ZEC::Proto::CBlockHeader &header);
