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
  struct NoteEncryption {
      enum { CLEN=MLEN+NOTEENCRYPTION_AUTH_BYTES };
      BaseBlob<256> epk;
      BaseBlob<256> esk;
      unsigned char nonce;
      BaseBlob<256> hSig;
  };

  using ZCNoteEncryption = NoteEncryption<ZC_NOTEPLAINTEXT_SIZE>;

#pragma pack(push, 1)
  struct BlockHeader {
  public:
    static constexpr size_t HEADER_SIZE = 4+32+32+32+4+4+32;

  public:
    int32_t nVersion;
    BaseBlob<256> hashPrevBlock;
    BaseBlob<256> hashMerkleRoot;
    BaseBlob<256> hashLightClientRoot;
    uint32_t nTime;
    uint32_t nBits;
    BaseBlob<256> nNonce;
    xvector<uint8_t> nSolution;

    BlockHashTy GetHash() const {
      SmallStream<2048> localStream;
      BTC::serialize(localStream, nSolution);
      return BTC::sha256d(this, HEADER_SIZE, localStream.data(), localStream.sizeOf());
    }

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.nVersion);
      op.io(d.hashPrevBlock);
      op.io(d.hashMerkleRoot);
      op.io(d.hashLightClientRoot);
      op.io(d.nTime);
      op.io(d.nBits);
      op.io(d.nNonce);
      op.io(d.nSolution);
    }
  };
#pragma pack(pop)

  // GetHash hashes the first HEADER_SIZE bytes of the object: layout must stay equal to the
  // wire prefix
  static_assert(sizeof(BlockHeader) == BlockHeader::HEADER_SIZE + sizeof(xvector<uint8_t>));

  using BlockHeaderNet = BTC::Proto::BlockHeaderNetTy<ZEC::Proto>;
  using Block = BTC::Proto::BlockTy<ZEC::Proto>;
  using NetworkAddress = BTC::Proto::NetworkAddress;
  using InventoryVector = BTC::Proto::InventoryVector;
  // TxIn & TxOut compatible with BTC, witness stack will not used
  using TxIn = BTC::Proto::TxIn;
  using TxOut = BTC::Proto::TxOut;

  using CBlockValidationData = BTC::Proto::CBlockValidationData;
  using CBlockLinkedOutputs = BTC::Proto::CBlockLinkedOutputs;
  using CTxLinkedOutputs = BTC::Proto::CTxLinkedOutputs;

  struct CompressedG1 {
    bool y_lsb;
    BaseBlob<256> x;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      // the y bit lives in a validated prefix byte
      if constexpr (Op::Writing) {
        uint8_t leadingByte = G1_PREFIX_MASK;
        if (d.y_lsb)
          leadingByte |= 1;
        op.put(leadingByte);
      } else {
        uint8_t leadingByte = 0;
        op.get(leadingByte);
        op.check((leadingByte & ~1) == G1_PREFIX_MASK);
        d.y_lsb = leadingByte & 1;
      }
      op.io(d.x);
    }
  };

  struct CompressedG2 {
    bool y_gt;
    BaseBlob<512> x;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      if constexpr (Op::Writing) {
        uint8_t leadingByte = G2_PREFIX_MASK;
        if (d.y_gt)
          leadingByte |= 1;
        op.put(leadingByte);
      } else {
        uint8_t leadingByte = 0;
        op.get(leadingByte);
        op.check((leadingByte & ~1) == G2_PREFIX_MASK);
        d.y_gt = leadingByte & 1;
      }
      op.io(d.x);
    }
  };

  struct PHGRProof {
    CompressedG1 g_A;
    CompressedG1 g_A_prime;
    CompressedG2 g_B;
    CompressedG1 g_B_prime;
    CompressedG1 g_C;
    CompressedG1 g_C_prime;
    CompressedG1 g_K;
    CompressedG1 g_H;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.g_A);
      op.io(d.g_A_prime);
      op.io(d.g_B);
      op.io(d.g_B_prime);
      op.io(d.g_C);
      op.io(d.g_C_prime);
      op.io(d.g_K);
      op.io(d.g_H);
    }
  };

  struct SpendDescription {
    BaseBlob<256> cv;
    BaseBlob<256> anchor;
    BaseBlob<256> nullifer;
    BaseBlob<256> rk;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;
    std::array<uint8_t, 64> spendAuthSig;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.cv);
      op.io(d.anchor);
      op.io(d.nullifer);
      op.io(d.rk);
      op.io(d.zkproof);
      op.io(d.spendAuthSig);
    }
  };

  struct OutputDescription {
    BaseBlob<256> cv;
    BaseBlob<256> cmu;
    BaseBlob<256> ephemeralKey;
    std::array<uint8_t, ZC_SAPLING_ENCCIPHERTEXT_SIZE> encCiphertext;
    std::array<uint8_t, ZC_SAPLING_OUTCIPHERTEXT_SIZE> outCiphertext;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.cv);
      op.io(d.cmu);
      op.io(d.ephemeralKey);
      op.io(d.encCiphertext);
      op.io(d.outCiphertext);
      op.io(d.zkproof);
    }
  };

  struct JSDescription {
    int64_t vpub_old;
    int64_t vpub_new;
    BaseBlob<256> anchor;
    BaseBlob<256> nullifier1;
    BaseBlob<256> nullifier2;
    BaseBlob<256> commitment1;
    BaseBlob<256> commitment2;
    BaseBlob<256> ephemeralKey;
    std::array<uint8_t, ZCNoteEncryption::CLEN> ciphertext1;
    std::array<uint8_t, ZCNoteEncryption::CLEN> ciphertext2;
    BaseBlob<256> randomSeed;
    BaseBlob<256> mac1;
    BaseBlob<256> mac2;

    PHGRProof phgrProof;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool useGroth) {
      op.io(d.vpub_old);
      op.io(d.vpub_new);
      op.io(d.anchor);
      op.io(d.nullifier1);
      op.io(d.nullifier2);
      op.io(d.commitment1);
      op.io(d.commitment2);
      op.io(d.ephemeralKey);
      op.io(d.randomSeed);
      op.io(d.mac1);
      op.io(d.mac2);
      // the proof representation is picked by the transaction the description belongs to
      if (useGroth)
        op.io(d.zkproof);
      else
        op.io(d.phgrProof);
      op.io(d.ciphertext1);
      op.io(d.ciphertext2);
    }
  };

  struct Transaction {
    bool fOverwintered;
    int32_t version;
    uint32_t nVersionGroupId;
    xvector<TxIn> txIn;
    xvector<TxOut> txOut;
    uint32_t lockTime;
    uint32_t nExpiryHeight;
    int64_t valueBalance;
    xvector<SpendDescription> vShieldedSpend;
    xvector<OutputDescription> vShieldedOutput;
    xvector<JSDescription> vJoinSplit;
    std::array<uint8_t, 32> joinSplitPubKey;
    std::array<uint8_t, 64> joinSplitSig;
    std::array<uint8_t, 64> bindingSig;

    BlockHashTy getTxId() const;
    // ZEC has no witness data, wtxid is always the same as txid
    BlockHashTy getWTxid() const { return getTxId(); }

    // The witness flag of the common block path is accepted and ignored
    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool = true) {
      // fOverwintered is packed into the sign bit of the version word
      uint32_t header;
      if constexpr (Op::Writing) {
        header = (static_cast<uint32_t>(d.fOverwintered) << 31) | static_cast<uint32_t>(d.version);
        op.put(header);
      } else {
        header = 0;
        op.get(header);
        d.fOverwintered = header >> 31;
        d.version = header & 0x7FFFFFFF;
      }

      if (d.fOverwintered)
        op.io(d.nVersionGroupId);

      bool isOverwinterV3 = d.fOverwintered &&
          d.nVersionGroupId == OVERWINTER_VERSION_GROUP_ID &&
          d.version == OVERWINTER_TX_VERSION;
      bool isSaplingV4 =
          d.fOverwintered &&
          d.nVersionGroupId == SAPLING_VERSION_GROUP_ID &&
          d.version == SAPLING_TX_VERSION;
      bool useGroth = d.fOverwintered && d.version >= SAPLING_TX_VERSION;

      if constexpr (!Op::Writing) {
        // an overwintered transaction of an unknown version group is unparsable
        if (d.fOverwintered && !(isOverwinterV3 || isSaplingV4)) {
          op.check(false);
          return;
        }
      }

      op.io(d.txIn);
      op.io(d.txOut);
      op.io(d.lockTime);

      if (isOverwinterV3 || isSaplingV4)
        op.io(d.nExpiryHeight);

      size_t shieldedSpends = 0;
      size_t shieldedOutputs = 0;
      if (isSaplingV4) {
        op.io(d.valueBalance);
        shieldedSpends = op.vec(d.vShieldedSpend);
        shieldedOutputs = op.vec(d.vShieldedOutput);
      }

      if (d.version >= 2) {
        if (op.vec(d.vJoinSplit, useGroth) != 0) {
          op.io(d.joinSplitPubKey);
          op.io(d.joinSplitSig);
        }
      }

      if (isSaplingV4 && (shieldedSpends != 0 || shieldedOutputs != 0))
        op.io(d.bindingSig);
    }
  };

  using MessageVersion = BTC::Proto::MessageVersion;
  using MessagePing = BTC::Proto::MessagePing;
  using MessagePong = BTC::Proto::MessagePong;
  using MessageAddr = BTC::Proto::MessageAddr;
  using MessageGetHeaders = BTC::Proto::MessageGetHeaders;
  using MessageGetBlocks = BTC::Proto::MessageGetBlocks;
  using MessageInv = BTC::Proto::MessageInv;
  using MessageBlock = BTC::Proto::MessageBlock;
  using MessageGetData = BTC::Proto::MessageGetData;
  using MessageReject = BTC::Proto::MessageReject;
  using MessageHeaders = BTC::Proto::MessageHeadersTy<ZEC::Proto>;
};
}

void serializeJson(xmstream &stream, const char *fieldName, const ZEC::Proto::Transaction &data);
void serializeJsonInside(xmstream &stream, const ZEC::Proto::BlockHeader &header);
