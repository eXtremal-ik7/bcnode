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

  // Transaction format same as BTC: LTC's grew the MWEB sections, dogecoin has none
  using TxIn = LTC::Proto::TxIn;
  using TxOut = LTC::Proto::TxOut;
  using TxWitness = LTC::Proto::TxWitness;
  using Transaction = BTC::Proto::Transaction;

  using PureBlockHeader = LTC::Proto::BlockHeader;

  struct BlockHeader: public PureBlockHeader {
  public:
    static const int32_t VERSION_AUXPOW = (1 << 8);
    // AuxPow
    Transaction ParentBlockCoinbaseTx;
    BaseBlob<256> HashBlock;
    xvector<BaseBlob<256>> MerkleBranch;
    int Index;
    xvector<BaseBlob<256>> ChainMerkleBranch;
    int ChainIndex;
    PureBlockHeader ParentBlock;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool serializeWitness = true) {
      op.io(d.nVersion);
      op.io(d.hashPrevBlock);
      op.io(d.hashMerkleRoot);
      op.io(d.nTime);
      op.io(d.nBits);
      op.io(d.nNonce);
      if (d.nVersion & VERSION_AUXPOW) {
        op.io(d.ParentBlockCoinbaseTx, serializeWitness);
        op.io(d.HashBlock);
        op.io(d.MerkleBranch);
        op.io(d.Index);
        op.io(d.ChainMerkleBranch);
        op.io(d.ChainIndex);
        op.io(d.ParentBlock);
      }
    }
  };

  using CTxValidationData = BTC::Proto::CTxValidationData;
  using CBlockValidationData = BTC::Proto::CBlockValidationData;
  using CBlockLinkedOutputs = BTC::Proto::CBlockLinkedOutputs;
  using CTxLinkedOutputs = BTC::Proto::CTxLinkedOutputs;

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

void serializeJsonInside(xmstream &stream, const DOGE::Proto::BlockHeader &header);
