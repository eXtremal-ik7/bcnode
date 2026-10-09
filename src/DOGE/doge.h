// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "proto.h"
#include "LTC/ltc.h"
#include "BTC/merkleTree.h"
#include "crypto/scrypt.h"
#include <algorithm>
#include <string.h>

namespace DOGE {

namespace DB {
class UTXODb;
}

  // Using BTC serialization protocol
  using BTC::Io;
  using BTC::serialize;
  using BTC::serializeForSignature;
  using BTC::unserialize;
  using BTC::unserializeAndCheck;
  using BTC::unpack2;
}

namespace DOGE {
class Configuration {
public:
  static constexpr size_t MaxBlockSize = LTC::Configuration::MaxBlockSize;
  static constexpr unsigned MaxBlockSigOps = LTC::Configuration::MaxBlockSigOps;
  static constexpr uint32_t BlocksFileLimit = LTC::Configuration::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = LTC::Configuration::DefaultBlockCacheSize;
  static constexpr uint64_t RationalPartSize = LTC::Configuration::RationalPartSize;
  // Consensus: Dogecoin Core's MAX_MONEY, ten billion coins
  static constexpr int64_t MaxMoney = 10000000000LL * 100000000LL;
  // Mempool policy. The fees are Bitcoin Core 28's, not checked against this coin's own node
  static constexpr int64_t MinRelayTxFee = 1000;
  static constexpr int64_t DustRelayTxFee = 3000;
  static constexpr bool FeeRoundsUp = false;
  static constexpr int32_t MaxStandardTxVersion = 2;
  static constexpr size_t MaxStandardScriptSigSize = 1650;
  static constexpr size_t MaxOpReturnRelay = 83;
  // Segwit never activated: witness outputs are not standard
  static constexpr bool HasWitness = false;

  static constexpr const char *ProjectName = "Dogecoin";
  static constexpr const char *TickerName = "DOGE";
  static constexpr const char *DefaultDataDir = "bcnodedoge";
  static constexpr const char *UserAgent = "/bcnode/doge-0.1/";
  static constexpr uint32_t ProtocolVersion = LTC::Configuration::ProtocolVersion;
  static constexpr uint64_t ServicesEnabled = LTC::Configuration::ServicesEnabled;
};

using Script = LTC::Script;

namespace Common {
  // What the block's place in the chain says about it; Bitcoin's answer applies
  // unchanged, the pinned repeats are empty and BIP34Height carries the rest
  using BTC::Common::fillChainContext;

  // Inherit BTC chain params, add the aux pow settings on top
  struct ChainParams: public BTC::Common::ChainParamsTy<DOGE::Proto> {
    bool StrictChainId;
  };

  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet,
    NetworkIdRegtest
  };

  using BlockIndex = BTC::Common::BlockIndexTy<DOGE::Proto>;
  using CIndexCacheObject = BTC::Common::CIndexCacheObject;
  using CheckConsensusCtx = LTC::Common::CheckConsensusCtx;

  bool setupChainParams(ChainParams *params, const char *network);
  static inline bool hasWitness() { return true; }
  // Blocks a coinbase output waits before it may be spent. Digishield (145000) raised it from 30
  // to 240, and the creation height picks the rule; regtest keeps 60 throughout
  static inline uint32_t coinbaseMaturity(const ChainParams &chainParams, uint32_t height) {
    if (chainParams.networkId == NetworkIdRegtest)
      return 60;
    return height < 145000 ? 30 : 240;
  }

  unsigned getBlockGeneration(const ChainParams &chainParams, BlockIndex *index);

  static inline void initializeValidationContext(const Proto::CBlock &block, Proto::CBlockValidationData &ctx) { BTC::validationDataInitialize(block, ctx); }

  bool checkBlockStandalone(const Proto::CBlock &block,
                            Proto::CBlockValidationData &validation,
                            const ChainParams &chainParams,
                            std::string &error);
  bool checkBlockContextual(const BlockIndex &index,
                            const Proto::CBlock &block,
                            Proto::CBlockValidationData &validation,
                            const Proto::CBlockLinkedOutputs &linkedOutputs,
                            const ChainParams &chainParams,
                            std::string &error);

  // A transaction alone: Core's CheckTransaction
  bool checkTransactionStandalone(const Proto::CTransaction &tx, const ChainParams &chainParams, std::string &error);
  // With the outputs it spends, for the block it would enter: CheckTxInputs and IsFinalTx. Sets
  // the fee once the input values pass
  bool checkTransactionContextual(const Proto::CTransaction &tx,
                                  const BTC::CPrevout *prevouts,
                                  const BTC::CTxContext &context,
                                  const ChainParams &chainParams,
                                  int64_t &fee,
                                  std::string &error);
  // Mempool policy on the transaction alone and with the outputs it spends: what this coin's node
  // asks beyond consensus (BTC/policy.h)
  bool checkPolicyStandalone(const Proto::CTransaction &tx, const BTC::CTxCost &cost, std::string &error);
  bool checkPolicyContextual(const Proto::CTransaction &tx,
                             const BTC::CPrevout *prevouts,
                             const BTC::CTxContext &context,
                             const BTC::CTxCost &cost,
                             int64_t fee,
                             std::string &error);
  // The longest transaction checkPolicyStandalone can pass: the network drops longer ones unparsed
  size_t maxStandardTxSize();

  static inline UInt<256> GetBlockProof(const Proto::CBlockHeader &header, const ChainParams&) {
    return LTC::Common::GetBlockProof(header);
  }

  static inline void checkConsensusInitialize(CheckConsensusCtx &ctx) { LTC::Common::checkConsensusInitialize(ctx); }
  static inline bool checkConsensus(const Proto::CBlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams) {
    return header.Version & Proto::CBlockHeader::VERSION_AUXPOW ?
      LTC::Common::checkPow(header.ParentBlock, header.Bits, ctx, chainParams.powLimit) :
      LTC::Common::checkPow(header, header.Bits, ctx, chainParams.powLimit);
  }

  // Auxpow keeps the hashed header and its target apart: sorted out here, before the scrypt path
  static inline void checkConsensusMulti(const Proto::CBlockHeader *const *headers,
                                         size_t count,
                                         CheckConsensusCtx&,
                                         ChainParams &chainParams,
                                         bool *results) {
    for (size_t base = 0; base < count; base += SCRYPT_WAYS) {
      size_t num = std::min<size_t>(SCRYPT_WAYS, count - base);
      const Proto::CPureBlockHeader *hashed[SCRYPT_WAYS];
      uint32_t nBits[SCRYPT_WAYS];

      for (size_t i = 0; i < num; i++) {
        const Proto::CBlockHeader *header = headers[base + i];
        hashed[i] = header->Version & Proto::CBlockHeader::VERSION_AUXPOW ? &header->ParentBlock : header;
        nBits[i] = header->Bits;
      }

      LTC::Common::checkPowMulti(hashed, nBits, num, chainParams.powLimit, results + base);
    }
  }
};

class X {
public:
  using BlockIndex = DOGE::Common::BlockIndex;
  using ChainParams = DOGE::Common::ChainParams;
  using Configuration = DOGE::Configuration;
  using Proto = DOGE::Proto;
  using UTXODb = DOGE::DB::UTXODb;
  template<typename T> using Io = BTC::Io<T>;
};
}
