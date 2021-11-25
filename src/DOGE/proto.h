// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "LTC/proto.h"

namespace DOGE {
class Proto {
public:
  using BlockHashTy = LTC::Proto::BlockHashTy;
  using TxHashTy = LTC::Proto::TxHashTy;
  using AddressTy = LTC::Proto::AddressTy;

  // Transaction format same as LTC
  using TxIn = LTC::Proto::TxIn;
  using TxOut = LTC::Proto::TxOut;
  using TxWitness = LTC::Proto::TxWitness;
  using Transaction = LTC::Proto::Transaction;

  using PureBlockHeader = LTC::Proto::BlockHeader;

  struct BlockHeader: public PureBlockHeader {
  public:
    static const int32_t VERSION_AUXPOW = (1 << 8);
    // AuxPow
    Transaction ParentBlockCoinbaseTx;
    uint256 HashBlock;
    xvector<uint256> MerkleBranch;
    int Index;
    xvector<uint256> ChainMerkleBranch;
    int ChainIndex;
    PureBlockHeader ParentBlock;
  };

  using CTxValidationData = BTC::Proto::CTxValidationData;

  using BlockHeaderNet = BTC::Proto::BlockHeaderNetTy<DOGE::Proto>;
  using Block = BTC::Proto::BlockTy<DOGE::Proto>;
  using NetworkAddress = LTC::Proto::NetworkAddress;
  using InventoryVector = LTC::Proto::InventoryVector;


  using MessageVersion = LTC::Proto::MessageVersion;
  using MessagePing = LTC::Proto::MessagePing;
  using MessagePong = LTC::Proto::MessagePong;
  using MessageAddr = LTC::Proto::MessageAddr;
  using MessageGetHeaders = LTC::Proto::MessageGetHeaders;
  using MessageGetBlocks = LTC::Proto::MessageGetBlocks;
  using MessageInv = LTC::Proto::MessageInv;
  using MessageBlock = Block;
  using MessageGetData = LTC::Proto::MessageGetData;
  using MessageReject = LTC::Proto::MessageReject;
  using MessageHeaders = BTC::Proto::MessageHeadersTy<DOGE::Proto>;
};
}

namespace BTC {
// Header
template<> struct Io<DOGE::Proto::BlockHeader> {
  static size_t getSerializedSize(const DOGE::Proto::BlockHeader&);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const DOGE::Proto::BlockHeader &data);
  static void unserialize(xmstream &src, DOGE::Proto::BlockHeader &data);
  static void unpack2(xmstream &src, DOGE::Proto::BlockHeader *dst, uint8_t **);
};
}

void serializeJsonInside(xmstream &stream, const DOGE::Proto::BlockHeader &header);
