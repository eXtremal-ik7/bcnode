// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BTC/serialize.h"
#include "BTC/proto.h"

namespace LTC {
class Proto {
public:
  using BlockHashTy = BTC::Proto::BlockHashTy;
  using TxHashTy = BTC::Proto::TxHashTy;
  using AddressTy = BTC::Proto::AddressTy;
  using BlockHeader = BTC::Proto::BlockHeader;
  using BlockHeaderNet = BTC::Proto::BlockHeaderNet;
  using Block = BTC::Proto::BlockTy<LTC::Proto>;
  using NetworkAddress = BTC::Proto::NetworkAddress;
  using InventoryVector = BTC::Proto::InventoryVector;
  using TxIn = BTC::Proto::TxIn;
  using TxOut = BTC::Proto::TxOut;
  using TxWitness = BTC::Proto::TxWitness;

  struct MWebTx {

  };

  struct Transaction {
    int32_t version;
    xvector<TxIn> txIn;
    xvector<TxOut> txOut;
    uint32_t lockTime;

    // Memory only
    uint32_t SerializedDataOffset = 0;
    uint32_t SerializedDataSize = 0;

    bool hasWitness() const {
      for (size_t i = 0; i < txIn.size(); i++) {
        if (!txIn[i].witnessStack.empty())
          return true;
      }

      return false;
    }

    BlockHashTy getTxId() const;
    BlockHashTy getWTxid() const;
  };

  using CBlockValidationData = BTC::Proto::CBlockValidationData;
  using CBlockLinkedOutputs = BTC::Proto::CBlockLinkedOutputs;
  using CTxLinkedOutputs = BTC::Proto::CTxLinkedOutputs;

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
  using MessageHeaders = BTC::Proto::MessageHeaders;
};
}

// Serialize
namespace BTC {
template<> struct Io<LTC::Proto::Transaction> {
  static size_t getSerializedSize(const LTC::Proto::Transaction &data, bool serializeWitness=true);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const LTC::Proto::Transaction &data, bool serializeWitness=true);
  static void unserialize(xmstream &src, LTC::Proto::Transaction &data);
  static void unpack2(xmstream &src, LTC::Proto::Transaction *data, uint8_t **extraData);

  static void serializeForSignature(xmstream &dst,
                                    const LTC::Proto::Transaction &data,
                                    size_t targetInput,
                                    const uint8_t *utxo,
                                    size_t utxoSize);
};
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::Proto::Transaction &data);
