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
      uint256 epk;
      uint256 esk;
      unsigned char nonce;
      uint256 hSig;
  };

  using ZCNoteEncryption = NoteEncryption<ZC_NOTEPLAINTEXT_SIZE>;

#pragma pack(push, 1)
  struct BlockHeader {
  public:
    static constexpr size_t HEADER_SIZE = 4+32+32+32+4+4+32;

  public:
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint256 hashLightClientRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint256 nNonce;
    xvector<uint8_t> nSolution;

    BlockHashTy GetHash() const {
      uint8_t buffer[2048];
      uint256 result;
      xmstream localStream(buffer, sizeof(buffer));
      localStream.reset();
      BTC::serialize(localStream, nSolution);

      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, this, HEADER_SIZE);
      SHA256_Update(&sha256, localStream.data(), localStream.sizeOf());
      SHA256_Final(result.begin(), &sha256);

      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(result));
      SHA256_Final(result.begin(), &sha256);
      return result;
    }
  };
#pragma pack(pop)

  using BlockHeaderNet = BTC::Proto::BlockHeaderNetTy<ZEC::Proto>;
  using Block = BTC::Proto::BlockTy<ZEC::Proto>;
  using NetworkAddress = BTC::Proto::NetworkAddress;
  using InventoryVector = BTC::Proto::InventoryVector;
  // TxIn & TxOut compatible with BTC, witness stack will not used
  using TxIn = BTC::Proto::TxIn;
  using TxOut = BTC::Proto::TxOut;

  struct CompressedG1 {
    bool y_lsb;
    base_blob<256> x;
  };

  struct CompressedG2 {
    bool y_gt;
    base_blob<512> x;
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
  };

  struct SpendDescription {
    uint256 cv;
    uint256 anchor;
    uint256 nullifer;
    uint256 rk;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;
    std::array<uint8_t, 64> spendAuthSig;
  };

  struct OutputDescription {
    uint256 cv;
    uint256 cmu;
    uint256 ephemeralKey;
    std::array<uint8_t, ZC_SAPLING_ENCCIPHERTEXT_SIZE> encCiphertext;
    std::array<uint8_t, ZC_SAPLING_OUTCIPHERTEXT_SIZE> outCiphertext;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;
  };

  struct JSDescription {
    int64_t vpub_old;
    int64_t vpub_new;
    uint256 anchor;
    uint256 nullifier1;
    uint256 nullifier2;
    uint256 commitment1;
    uint256 commitment2;
    uint256 ephemeralKey;
    std::array<uint8_t, ZCNoteEncryption::CLEN> ciphertext1;
    std::array<uint8_t, ZCNoteEncryption::CLEN> ciphertext2;
    uint256 randomSeed;
    uint256 mac1;
    uint256 mac2;

    PHGRProof phgrProof;
    std::array<uint8_t, GROTH_PROOF_SIZE> zkproof;
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

    // Memory only
    uint32_t SerializedDataOffset = 0;
    uint32_t SerializedDataSize = 0;

    BlockHashTy getTxId() const;
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

// Serialize
namespace BTC {
template<> struct Io<ZEC::Proto::CompressedG1> {
  static size_t getSerializedSize(const ZEC::Proto::CompressedG1 &data);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const ZEC::Proto::CompressedG1 &data);
  static void unserialize(xmstream &src, ZEC::Proto::CompressedG1 &data);
  static void unpack2(xmstream &src, ZEC::Proto::CompressedG1 *data, uint8_t **extraData);
};

template<> struct Io<ZEC::Proto::CompressedG2> {
  static size_t getSerializedSize(const ZEC::Proto::CompressedG2 &data);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const ZEC::Proto::CompressedG2 &data);
  static void unserialize(xmstream &src, ZEC::Proto::CompressedG2 &data);
  static void unpack2(xmstream &src, ZEC::Proto::CompressedG2 *data, uint8_t **extraData);
};

template<> struct Io<ZEC::Proto::PHGRProof> {
  static size_t getSerializedSize(const ZEC::Proto::PHGRProof &data);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const ZEC::Proto::PHGRProof &data);
  static void unserialize(xmstream &src, ZEC::Proto::PHGRProof &data);
  static void unpack2(xmstream &src, ZEC::Proto::PHGRProof *data, uint8_t **extraData);
};

template<> struct Io<ZEC::Proto::SpendDescription> {
  static size_t getSerializedSize(const ZEC::Proto::SpendDescription &data);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const ZEC::Proto::SpendDescription &data);
  static void unserialize(xmstream &src, ZEC::Proto::SpendDescription &data);
  static void unpack2(xmstream &src, ZEC::Proto::SpendDescription *data, uint8_t **extraData);
};

template<> struct Io<ZEC::Proto::OutputDescription> {
  static size_t getSerializedSize(const ZEC::Proto::OutputDescription &data);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const ZEC::Proto::OutputDescription &data);
  static void unserialize(xmstream &src, ZEC::Proto::OutputDescription &data);
  static void unpack2(xmstream &src, ZEC::Proto::OutputDescription *data, uint8_t **extraData);
};

template<> struct Io<ZEC::Proto::JSDescription, bool> {
  static size_t getSerializedSize(const ZEC::Proto::JSDescription &data, bool useGroth);
  static size_t getUnpackedExtraSize(xmstream &src, bool useGroth);
  static void serialize(xmstream &dst, const ZEC::Proto::JSDescription &data, bool useGroth);
  static void unserialize(xmstream &src, ZEC::Proto::JSDescription &data, bool useGroth);
  static void unpack2(xmstream &src, ZEC::Proto::JSDescription *data, uint8_t **extraData, bool useGroth);
};

template<> struct Io<ZEC::Proto::BlockHeader> {
  static size_t getSerializedSize(const ZEC::Proto::BlockHeader &data);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const ZEC::Proto::BlockHeader &data);
  static void unserialize(xmstream &src, ZEC::Proto::BlockHeader &data);
  static void unpack2(xmstream &src, ZEC::Proto::BlockHeader *data, uint8_t **extraData);
};

template<> struct Io<ZEC::Proto::Transaction> {
  static size_t getSerializedSize(const ZEC::Proto::Transaction &data, bool);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const ZEC::Proto::Transaction &data);
  static void unserialize(xmstream &src, ZEC::Proto::Transaction &data);
  static void unpack2(xmstream &src, ZEC::Proto::Transaction *data, uint8_t **extraData);
};

}

void serializeJson(xmstream &stream, const char *fieldName, const ZEC::Proto::Transaction &data);
void serializeJsonInside(xmstream &stream, const ZEC::Proto::BlockHeader &header);
