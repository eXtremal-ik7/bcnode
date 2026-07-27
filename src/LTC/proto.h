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

    bool hasWitness() const {
      for (size_t i = 0; i < txIn.size(); i++) {
        if (!txIn[i].witnessStack.empty())
          return true;
      }

      return false;
    }

    BlockHashTy getTxId() const;
    BlockHashTy getWTxid() const;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool serializeWitness = true) {
      op.io(d.version);
      if constexpr (Op::Writing) {
        // segwit: marker and flag ahead of the inputs, witness stacks between the outputs
        // and lockTime
        bool witness = d.hasWitness() && serializeWitness;
        if (witness) {
          op.put(static_cast<uint8_t>(0));
          op.put(static_cast<uint8_t>(1));
        }
        op.io(d.txIn);
        op.io(d.txOut);
        if (witness) {
          for (size_t i = 0; i < d.txIn.size(); i++)
            op.io(d.txIn[i].witnessStack);
        }
      } else {
        // an empty input list is the segwit marker: the flag byte follows, then the real lists
        uint8_t flags = 0;
        size_t txInCount = op.vec(d.txIn);
        if (txInCount == 0) {
          op.get(flags);
          if (flags != 0) {
            txInCount = op.vec(d.txIn);
            op.vec(d.txOut);
          }
        } else {
          op.vec(d.txOut);
        }

        if (flags & 1) {
          flags ^= 1;
          // the marker with every witness stack empty must have been serialized without
          // the marker: reject, as Core does
          bool anyWitness = false;
          for (size_t i = 0; i < txInCount; i++)
            op.element(d.txIn, i, [&](auto &in) { anyWitness |= op.vec(in.witnessStack) != 0; });
          if (!anyWitness) {
            op.check(false);
            return;
          }
        }

        if (flags) {
          op.check(false);
          return;
        }
      }
      op.io(d.lockTime);
    }
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
void serializeForSignature(xmstream &dst,
                           const LTC::Proto::Transaction &data,
                           size_t targetInput,
                           const uint8_t *utxo,
                           size_t utxoSize);
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::Proto::Transaction &data);
