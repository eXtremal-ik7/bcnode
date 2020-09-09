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
  using BlockHeader = LTC::Proto::BlockHeader;
  using BlockHeaderNet = LTC::Proto::BlockHeaderNet;
  using Block = LTC::Proto::Block;
  using NetworkAddress = LTC::Proto::NetworkAddress;
  using InventoryVector = LTC::Proto::InventoryVector;
  using TxIn = LTC::Proto::TxIn;
  using TxOut = LTC::Proto::TxOut;
  using TxWitness = LTC::Proto::TxWitness;
  using Transaction = LTC::Proto::Transaction;

  using MessageVersion = LTC::Proto::MessageVersion;
  using MessagePing = LTC::Proto::MessagePing;
  using MessagePong = LTC::Proto::MessagePong;
  using MessageAddr = LTC::Proto::MessageAddr;
  using MessageGetHeaders = LTC::Proto::MessageGetHeaders;
  using MessageGetBlocks = LTC::Proto::MessageGetBlocks;
  using MessageInv = LTC::Proto::MessageInv;
  using MessageBlock = LTC::Proto::MessageBlock;
  using MessageGetData = LTC::Proto::MessageGetData;
  using MessageReject = LTC::Proto::MessageReject;
  using MessageHeaders = LTC::Proto::MessageHeaders;
};
}
