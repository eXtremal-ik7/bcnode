// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BTC/proto.h"
#include "common/bigNum.h"

namespace XPM {
class Proto {
public:
  using BlockHashTy = BTC::Proto::BlockHashTy;
  using TxHashTy = BTC::Proto::TxHashTy;
  using AddressTy = BTC::Proto::AddressTy;
  using BalanceType = BTC::Proto::BalanceType;

  // Data structures
#pragma pack(push, 1)
  struct BlockHeader {
    int32_t nVersion;
    BaseBlob<256> hashPrevBlock;
    BaseBlob<256> hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    mpz_class bnPrimeChainMultiplier;

    BlockHashTy GetHash() const {
      SmallStream<256> localStream;
      BTC::serialize(localStream, bnPrimeChainMultiplier);
      return BTC::sha256d(this, 4+32+32+4+4+4, localStream.data(), localStream.sizeOf());
    }

    UInt<256> GetOriginalHeaderHash() const {
      return BTC::sha256dInt(this, 4+32+32+4+4+4);
    }

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.nVersion);
      op.io(d.hashPrevBlock);
      op.io(d.hashMerkleRoot);
      op.io(d.nTime);
      op.io(d.nBits);
      op.io(d.nNonce);
      op.io(d.bnPrimeChainMultiplier);
    }
  };
#pragma pack(pop)

  // GetHash hashes the first 80 bytes of the object: layout must stay equal to the wire prefix
  static_assert(sizeof(BlockHeader) == 80 + sizeof(mpz_class));

  using TxIn = BTC::Proto::TxIn;
  using TxOut = BTC::Proto::TxOut;
  using TxWitness = BTC::Proto::TxWitness;
  using Transaction = BTC::Proto::Transaction;
  using Block = BTC::Proto::BlockTy<XPM::Proto>;

  using CBlockValidationData = BTC::Proto::CBlockValidationData;
  using CBlockLinkedOutputs = BTC::Proto::CBlockLinkedOutputs;
  using CTxLinkedOutputs = BTC::Proto::CTxLinkedOutputs;

  using BlockHeaderNet = BTC::Proto::BlockHeaderNetTy<XPM::Proto>;
  using NetworkAddress = BTC::Proto::NetworkAddress;
  using InventoryVector = BTC::Proto::InventoryVector;
  using MessagePing = BTC::Proto::MessagePing;
  using MessagePong = BTC::Proto::MessagePong;
  using MessageAddr = BTC::Proto::MessageAddr;
  using MessageGetHeaders = BTC::Proto::MessageGetHeaders;
  using MessageGetBlocks = BTC::Proto::MessageGetBlocks;
  using MessageInv = BTC::Proto::MessageInv;
  using MessageBlock = Block;
  using MessageGetData = BTC::Proto::MessageGetData;
  using MessageReject = BTC::Proto::MessageReject;
  using MessageHeaders = BTC::Proto::MessageHeadersTy<XPM::Proto>;

  // XPM version message has no relay field
  struct MessageVersion : public BTC::Proto::MessageVersion {
    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.version);
      op.io(d.services);
      op.io(d.timestamp);
      op.io(d.addr_recv);
      if (d.version >= 106) {
        op.io(d.addr_from);
        op.io(d.nonce);
        op.io(d.user_agent);
        op.io(d.start_height);
      }
    }
  };
};
}

// Serialize
namespace BTC {
template<> struct Io<mpz_class> {
  static size_t getSerializedSize(const mpz_class &data);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const mpz_class &data);
  static void unserialize(xmstream &src, mpz_class &data);
  static void unpack2(xmstream &src, mpz_class *data, uint8_t **extraData);
};

}

void serializeJsonInside(xmstream &stream, const XPM::Proto::BlockHeader &header);
