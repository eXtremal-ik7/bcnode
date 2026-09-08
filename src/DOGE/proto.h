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
  // The dogecoin supply in base units outgrows uint64 around 2032
  using BalanceType = UInt<128>;

  // Transaction format same as BTC: LTC's grew the MWEB sections, dogecoin has none
  using CTxIn = LTC::Proto::CTxIn;
  using CTxOut = LTC::Proto::CTxOut;
  using CTxWitness = LTC::Proto::CTxWitness;
  using CTransaction = BTC::Proto::CTransaction;

  using CPureBlockHeader = LTC::Proto::CBlockHeader;

  struct CBlockHeader: public CPureBlockHeader {
  public:
    static const int32_t VERSION_AUXPOW = (1 << 8);
    // AuxPow
    CTransaction ParentBlockCoinbaseTx;
    BaseBlob<256> HashBlock;
    xvector<BaseBlob<256>> MerkleBranch;
    int Index;
    xvector<BaseBlob<256>> ChainMerkleBranch;
    int ChainIndex;
    CPureBlockHeader ParentBlock;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool serializeWitness = true) {
      op.io(d.Version);
      op.io(d.HashPrevBlock);
      op.io(d.HashMerkleRoot);
      op.io(d.Time);
      op.io(d.Bits);
      op.io(d.Nonce);
      if (d.Version & VERSION_AUXPOW) {
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

  using CBlockHeaderNet = BTC::Proto::CBlockHeaderNetTy<DOGE::Proto>;
  using CBlock = BTC::Proto::CBlockTy<DOGE::Proto>;
  using CNetworkAddress = LTC::Proto::CNetworkAddress;
  using CInventoryVector = LTC::Proto::CInventoryVector;


  using CMessageVersion = LTC::Proto::CMessageVersion;
  using CMessagePing = LTC::Proto::CMessagePing;
  using CMessagePong = LTC::Proto::CMessagePong;
  using CMessageAddr = LTC::Proto::CMessageAddr;
  using CMessageGetHeaders = LTC::Proto::CMessageGetHeaders;
  using CMessageGetBlocks = LTC::Proto::CMessageGetBlocks;
  using CMessageInv = LTC::Proto::CMessageInv;
  using CMessageBlock = CBlock;
  using CMessageGetData = LTC::Proto::CMessageGetData;
  using CMessageReject = LTC::Proto::CMessageReject;
  using CMessageHeaders = BTC::Proto::CMessageHeadersTy<DOGE::Proto>;
};
}

void serializeJsonInside(xmstream &stream, const DOGE::Proto::CBlockHeader &header);
