// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BTC/serialize.h"
#include "BTC/proto.h"
#include "mweb.h"

namespace LTC {
class Proto {
public:
  using BlockHashTy = BTC::Proto::BlockHashTy;
  using TxHashTy = BTC::Proto::TxHashTy;
  using AddressTy = BTC::Proto::AddressTy;
  using BlockHeader = BTC::Proto::BlockHeader;
  using BlockHeaderNet = BTC::Proto::BlockHeaderNet;
  using NetworkAddress = BTC::Proto::NetworkAddress;
  using InventoryVector = BTC::Proto::InventoryVector;
  using TxIn = BTC::Proto::TxIn;
  using TxOut = BTC::Proto::TxOut;
  using TxWitness = BTC::Proto::TxWitness;

  // Which optional sections this pass writes: a txid drops the witness and the MWEB data, a
  // wtxid only the MWEB data, so the two are independent.
  //
  // IsHogEx is the way back out. The extension block follows the transactions only when the
  // last one is a HogEx, and the measuring pass builds no elements for the block to look at
  // afterwards, so every transaction reports through this pointer as it goes by.
  struct SerializeCtx {
    bool Witness = true;
    bool Mweb = true;
    bool *IsHogEx = nullptr;

    // a body, not '= default': clang rejects the latter with the '= {}' default argument below
    constexpr SerializeCtx() {}
    // Implicit: the shared block helpers pass the witness flag on its own
    constexpr SerializeCtx(bool witness) : Witness(witness) {}
    constexpr SerializeCtx(bool witness, bool mweb) : Witness(witness), Mweb(mweb) {}
  };

  struct Transaction {
    int32_t version;
    xvector<TxIn> txIn;
    xvector<TxOut> txOut;
    uint32_t lockTime;
    // At most one MWEB transaction. The same flag with nothing behind it marks the HogEx, the
    // transaction that pegs coins in and out and closes the extension block — the wire form
    // has no field of its own for that, so it is kept here
    xvector<MWeb::Transaction> mwebTx;
    bool hogEx = false;

    bool hasWitness() const {
      for (size_t i = 0; i < txIn.size(); i++) {
        if (!txIn[i].witnessStack.empty())
          return true;
      }

      return false;
    }

    bool hasMweb() const { return !mwebTx.empty(); }

    BlockHashTy getTxId() const;
    BlockHashTy getWTxid() const;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, SerializeCtx ctx = {}) {
      op.io(d.version);
      if constexpr (Op::Writing) {
        // segwit: marker and flag ahead of the inputs, witness stacks between the outputs and
        // lockTime. MWEB shares that flag byte and follows the witness stacks
        uint8_t flags = 0;
        if (ctx.Witness && d.hasWitness())
          flags |= 1;
        if (ctx.Mweb && (d.hasMweb() || d.hogEx))
          flags |= 8;

        if (flags) {
          op.put(static_cast<uint8_t>(0));
          op.put(flags);
        }
        op.io(d.txIn);
        op.io(d.txOut);
        if (flags & 1) {
          for (size_t i = 0; i < d.txIn.size(); i++)
            op.io(d.txIn[i].witnessStack);
        }
        if (flags & 8)
          op.vec(d.mwebTx);
      } else {
        // an empty input list is the extended format marker: the flag byte follows, then the
        // real lists
        uint8_t flags = 0;
        d.hogEx = false;
        size_t txInCount = op.vec(d.txIn);
        size_t txOutCount = 0;
        if (txInCount == 0) {
          op.get(flags);
          if (flags != 0) {
            txInCount = op.vec(d.txIn);
            txOutCount = op.vec(d.txOut);
          }
        } else {
          txOutCount = op.vec(d.txOut);
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

        if (flags & 8) {
          flags ^= 8;
          // the presence byte as a count: above one is a form Core's writer cannot produce
          size_t mwebCount = op.vec(d.mwebTx);
          if (mwebCount > 1) {
            op.check(false);
            return;
          }

          // nothing behind the flag means the HogEx, which Core refuses without outputs
          d.hogEx = mwebCount == 0;
          if (d.hogEx && txOutCount == 0) {
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

      if (ctx.IsHogEx)
        *ctx.IsHogEx = d.hogEx;
    }
  };

  struct Block {
    BlockHeader header;
    xvector<Transaction> vtx;
    xvector<MWeb::Block> mweb;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, SerializeCtx ctx = {}) {
      op.io(d.header);

      bool lastIsHogEx = false;
      SerializeCtx txCtx = ctx;
      txCtx.IsHogEx = &lastIsHogEx;
      size_t txCount = op.vec(d.vtx, txCtx);

      // Only the block that closes it with a HogEx carries one, so never a coinbase only block
      if (ctx.Mweb && txCount >= 2 && lastIsHogEx) {
        if (op.vec(d.mweb) > 1)
          op.check(false);
      }
    }

    // Bytes after the transaction list: enumerateTransactions checks a stored block's layout
    // against its size and has to account for them
    static size_t extensionSize(const Block &d) {
      if (d.vtx.size() < 2 || !d.vtx.back().hogEx)
        return 0;
      return BTC::Io<xvector<MWeb::Block>>::getSerializedSize(d.mweb);
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
  using MessageBlock = Block;
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
