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
  using BalanceType = BTC::Proto::BalanceType;
  using CBlockHeader = BTC::Proto::CBlockHeader;
  using CBlockHeaderNet = BTC::Proto::CBlockHeaderNet;
  using CNetworkAddress = BTC::Proto::CNetworkAddress;
  using CInventoryVector = BTC::Proto::CInventoryVector;
  using CTxIn = BTC::Proto::CTxIn;
  using CTxOut = BTC::Proto::CTxOut;
  using CTxWitness = BTC::Proto::CTxWitness;

  // Which optional sections this pass writes: a txid drops the witness and the MWEB data, a
  // wtxid only the MWEB data, so the two are independent.
  //
  // IsHogEx is the way back out. The extension block follows the transactions only when the
  // last one is a HogEx, and the measuring pass builds no elements for the block to look at
  // afterwards, so every transaction reports through this pointer as it goes by.
  struct CSerializeCtx {
    bool Witness = true;
    bool Mweb = true;
    bool *IsHogEx = nullptr;

    // a body, not '= default': clang rejects the latter with the '= {}' default argument below
    constexpr CSerializeCtx() {}
    // Implicit: the shared block helpers pass the witness flag on its own
    constexpr CSerializeCtx(bool witness) : Witness(witness) {}
    constexpr CSerializeCtx(bool witness, bool mweb) : Witness(witness), Mweb(mweb) {}
  };

  struct CTransaction {
    int32_t Version;
    xvector<CTxIn> TxIn;
    xvector<CTxOut> TxOut;
    uint32_t LockTime;
    // At most one MWEB transaction. The same flag with nothing behind it marks the HogEx, the
    // transaction that pegs coins in and out and closes the extension block — the wire form
    // has no field of its own for that, so it is kept here
    xvector<MWeb::CTransaction> MwebTx;
    bool HogEx = false;

    bool hasWitness() const {
      for (size_t i = 0; i < TxIn.size(); i++) {
        if (!TxIn[i].WitnessStack.empty())
          return true;
      }

      return false;
    }

    bool hasMweb() const { return !MwebTx.empty(); }

    BlockHashTy getTxId() const;
    BlockHashTy getWTxid() const;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, CSerializeCtx ctx = {}) {
      op.io(d.Version);
      if constexpr (Op::Writing) {
        // segwit: marker and flag ahead of the inputs, witness stacks between the outputs and
        // LockTime. MWEB shares that flag byte and follows the witness stacks
        uint8_t flags = 0;
        if (ctx.Witness && d.hasWitness())
          flags |= 1;
        if (ctx.Mweb && (d.hasMweb() || d.HogEx))
          flags |= 8;

        if (flags) {
          op.put(static_cast<uint8_t>(0));
          op.put(flags);
        }
        op.io(d.TxIn);
        op.io(d.TxOut);
        if (flags & 1) {
          for (size_t i = 0; i < d.TxIn.size(); i++)
            op.io(d.TxIn[i].WitnessStack);
        }
        if (flags & 8)
          op.vec(d.MwebTx);
      } else {
        // an empty input list is the extended format marker: the flag byte follows, then the
        // real lists
        uint8_t flags = 0;
        d.HogEx = false;
        size_t txInCount = op.vec(d.TxIn);
        size_t txOutCount = 0;
        if (txInCount == 0) {
          op.get(flags);
          if (flags != 0) {
            txInCount = op.vec(d.TxIn);
            txOutCount = op.vec(d.TxOut);
          }
        } else {
          txOutCount = op.vec(d.TxOut);
        }

        if (flags & 1) {
          flags ^= 1;
          // the marker with every witness stack empty must have been serialized without
          // the marker: reject, as Core does
          bool anyWitness = false;
          for (size_t i = 0; i < txInCount; i++)
            op.element(d.TxIn, i, [&](auto &in) { anyWitness |= op.vec(in.WitnessStack) != 0; });
          if (!anyWitness) {
            op.check(false);
            return;
          }
        }

        if (flags & 8) {
          flags ^= 8;
          // the presence byte as a count: above one is a form Core's writer cannot produce
          size_t mwebCount = op.vec(d.MwebTx);
          if (mwebCount > 1) {
            op.check(false);
            return;
          }

          // nothing behind the flag means the HogEx, which Core refuses without outputs
          d.HogEx = mwebCount == 0;
          if (d.HogEx && txOutCount == 0) {
            op.check(false);
            return;
          }
        }

        if (flags) {
          op.check(false);
          return;
        }
      }
      op.io(d.LockTime);

      if (ctx.IsHogEx)
        *ctx.IsHogEx = d.HogEx;
    }
  };

  struct CBlock {
    CBlockHeader Header;
    xvector<CTransaction> Vtx;
    xvector<MWeb::CBlock> Mweb;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, CSerializeCtx ctx = {}) {
      op.io(d.Header);

      bool lastIsHogEx = false;
      CSerializeCtx txCtx = ctx;
      txCtx.IsHogEx = &lastIsHogEx;
      size_t txCount = op.vec(d.Vtx, txCtx);

      // Only the block that closes it with a HogEx carries one, so never a coinbase only block
      if (ctx.Mweb && txCount >= 2 && lastIsHogEx) {
        if (op.vec(d.Mweb) > 1)
          op.check(false);
      }
    }

    // Bytes after the transaction list: txPositionsMatchStored checks a stored block's
    // layout against its size and has to account for them
    static size_t extensionSize(const CBlock &d) {
      if (d.Vtx.size() < 2 || !d.Vtx.back().HogEx)
        return 0;
      return BTC::Io<xvector<MWeb::CBlock>>::getSerializedSize(d.Mweb);
    }
  };

  using CBlockValidationData = BTC::Proto::CBlockValidationData;
  using CBlockLinkedOutputs = BTC::Proto::CBlockLinkedOutputs;
  using CTxLinkedOutputs = BTC::Proto::CTxLinkedOutputs;

  using CMessageVersion = BTC::Proto::CMessageVersion;
  using CMessagePing = BTC::Proto::CMessagePing;
  using CMessagePong = BTC::Proto::CMessagePong;
  using CMessageAddr = BTC::Proto::CMessageAddr;
  using CMessageGetHeaders = BTC::Proto::CMessageGetHeaders;
  using CMessageGetBlocks = BTC::Proto::CMessageGetBlocks;
  using CMessageInv = BTC::Proto::CMessageInv;
  using CMessageBlock = CBlock;
  using CMessageGetData = BTC::Proto::CMessageGetData;
  using CMessageReject = BTC::Proto::CMessageReject;
  using CMessageHeaders = BTC::Proto::CMessageHeaders;
};
}

// Serialize
namespace BTC {
void serializeForSignature(xmstream &dst,
                           const LTC::Proto::CTransaction &data,
                           size_t targetInput,
                           const uint8_t *utxo,
                           size_t utxoSize);
}

void serializeJson(xmstream &stream, const char *fieldName, const LTC::Proto::CTransaction &data);
