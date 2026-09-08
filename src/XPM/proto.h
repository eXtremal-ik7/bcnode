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
  struct CBlockHeader {
    int32_t Version;
    BaseBlob<256> HashPrevBlock;
    BaseBlob<256> HashMerkleRoot;
    uint32_t Time;
    uint32_t Bits;
    uint32_t Nonce;
    mpz_class PrimeChainMultiplier;

    BlockHashTy GetHash() const {
      SmallStream<256> localStream;
      BTC::serialize(localStream, PrimeChainMultiplier);
      return BTC::sha256d(this, 4+32+32+4+4+4, localStream.data(), localStream.sizeOf());
    }

    UInt<256> GetOriginalHeaderHash() const {
      return BTC::sha256dInt(this, 4+32+32+4+4+4);
    }

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Version);
      op.io(d.HashPrevBlock);
      op.io(d.HashMerkleRoot);
      op.io(d.Time);
      op.io(d.Bits);
      op.io(d.Nonce);
      op.io(d.PrimeChainMultiplier);
    }
  };
#pragma pack(pop)

  // GetHash hashes the first 80 bytes of the object: layout must stay equal to the wire prefix
  static_assert(sizeof(CBlockHeader) == 80 + sizeof(mpz_class));

  using CTxIn = BTC::Proto::CTxIn;
  using CTxOut = BTC::Proto::CTxOut;
  using CTxWitness = BTC::Proto::CTxWitness;
  using CTransaction = BTC::Proto::CTransaction;
  using CBlock = BTC::Proto::CBlockTy<XPM::Proto>;

  using CBlockValidationData = BTC::Proto::CBlockValidationData;
  using CBlockLinkedOutputs = BTC::Proto::CBlockLinkedOutputs;
  using CTxLinkedOutputs = BTC::Proto::CTxLinkedOutputs;

  using CBlockHeaderNet = BTC::Proto::CBlockHeaderNetTy<XPM::Proto>;
  using CNetworkAddress = BTC::Proto::CNetworkAddress;
  using CInventoryVector = BTC::Proto::CInventoryVector;
  using CMessagePing = BTC::Proto::CMessagePing;
  using CMessagePong = BTC::Proto::CMessagePong;
  using CMessageAddr = BTC::Proto::CMessageAddr;
  using CMessageGetHeaders = BTC::Proto::CMessageGetHeaders;
  using CMessageGetBlocks = BTC::Proto::CMessageGetBlocks;
  using CMessageInv = BTC::Proto::CMessageInv;
  using CMessageBlock = CBlock;
  using CMessageGetData = BTC::Proto::CMessageGetData;
  using CMessageReject = BTC::Proto::CMessageReject;
  using CMessageHeaders = BTC::Proto::CMessageHeadersTy<XPM::Proto>;

  // XPM version message has no relay field
  struct CMessageVersion : public BTC::Proto::CMessageVersion {
    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Version);
      op.io(d.Services);
      op.io(d.Timestamp);
      op.io(d.AddrRecv);
      if (d.Version >= 106) {
        op.io(d.AddrFrom);
        op.io(d.Nonce);
        op.io(d.UserAgent);
        op.io(d.StartHeight);
      }
    }
  };
};
}

// Serialize
namespace BTC {
// The one leaf in the tree with variable-size innards of its own: on the unpacking passes the
// limbs come from the object's arena instead of from GMP's allocator
template<> struct Io<mpz_class> {
  static size_t getSerializedSize(const mpz_class &data);
  static void serialize(xmstream &dst, const mpz_class &data);
  static void unserialize(xmstream &src, mpz_class &data);
  static void read(Ser::CReaderState &op, xmstream &src, mpz_class &data);
};

}

void serializeJsonInside(xmstream &stream, const XPM::Proto::CBlockHeader &header);
