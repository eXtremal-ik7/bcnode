// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "proto.h"
#include "BTC/btc.h"
#include "BTC/merkleTree.h"
#include <string.h>

namespace LTC {
  // Using BTC serialization protocol
  using BTC::Io;
  using BTC::serialize;
  using BTC::serializeForSignature;
  using BTC::unserialize;
  using BTC::unserializeAndCheck;
  using BTC::unpack2;
}

namespace LTC {

namespace DB {
class UTXODb;
}

class Configuration {
public:
  static constexpr size_t MaxBlockSize = BTC::Configuration::MaxBlockSize;
  static constexpr unsigned MaxBlockSigOps = BTC::Configuration::MaxBlockSigOps;
  static constexpr uint32_t BlocksFileLimit = BTC::Configuration::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = 256*1048576;
  static constexpr uint64_t RationalPartSize = 100000000ULL;
  // Consensus: Litecoin Core's MAX_MONEY
  static constexpr int64_t MaxMoney = 84000000LL * 100000000LL;
  // Mempool policy, Litecoin Core 0.21's defaults: units per 1000 bytes
  static constexpr int64_t MinRelayTxFee = 1000;
  static constexpr int64_t DustRelayTxFee = 30000;
  static constexpr bool FeeRoundsUp = false;
  static constexpr int32_t MaxStandardTxVersion = 2;
  static constexpr uint32_t MinStandardTxNonWitnessSize = 82;
  static constexpr size_t MaxStandardScriptSigSize = 1650;
  static constexpr size_t MaxOpReturnRelay = 83;
  static constexpr bool HasWitness = true;

  static constexpr const char *ProjectName = "Litecoin";
  static constexpr const char *TickerName = "LTC";
  static constexpr const char *DefaultDataDir = "bcnodeltc";
  static constexpr const char *UserAgent = "/bcnode/ltc-0.1/";
  static constexpr uint32_t ProtocolVersion = BTC::Configuration::ProtocolVersion;
  static constexpr uint64_t ServicesEnabled = BTC::Configuration::ServicesEnabled;
};

using Script = BTC::Script;

namespace Common {
  // What the block's place in the chain says about it; Bitcoin's answer applies
  // unchanged, the pinned repeats are empty and BIP34Height carries the rest
  using BTC::Common::fillChainContext;

  // Inherit BTC chain params
  using ChainParams = BTC::Common::ChainParamsTy<LTC::Proto>;

  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet,
    NetworkIdRegtest
  };

  using BlockIndex = BTC::Common::BlockIndexTy<LTC::Proto>;
  using CIndexCacheObject = BTC::Common::CIndexCacheObject;
  using CheckConsensusCtx = BTC::Common::CheckConsensusCtx;

  bool setupChainParams(ChainParams *params, const char *network);
  static inline bool hasWitness() { return true; }
  // Blocks a coinbase output waits before it may be spent (Core's COINBASE_MATURITY)
  static inline uint32_t coinbaseMaturity(const ChainParams&, uint32_t) { return 100; }

  unsigned getBlockGeneration(const ChainParams &chainParams, LTC::Common::BlockIndex *index);

  bool checkPow(const Proto::CBlockHeader &header, uint32_t nBits, CheckConsensusCtx &, const UInt<256> &powLimit);
  // Group of headers at once: hashing is all a block check costs, and a multi-way kernel wants
  // its inputs collected. Targets come apart - auxpow hashes the parent against the child target
  void checkPowMulti(const Proto::CBlockHeader *const *headers,
                     const uint32_t *nBits,
                     size_t count,
                     const UInt<256> &powLimit,
                     bool *results);
  UInt<256> GetBlockProof(const Proto::CBlockHeader &header);

  static inline UInt<256> GetBlockProof(const Proto::CBlockHeader &header, const ChainParams&) { return GetBlockProof(header); }
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  static inline bool checkConsensus(const Proto::CBlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams) { return checkPow(header, header.Bits, ctx, chainParams.powLimit); }
  void checkConsensusMulti(const Proto::CBlockHeader *const *headers,
                           size_t count,
                           CheckConsensusCtx &ctx,
                           ChainParams &chainParams,
                           bool *results);

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
  // With the outputs it spends, for the block it would enter: CheckTxInputs, IsFinalTx and
  // SequenceLocks. Sets the fee once the input values pass
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
};

class X {
public:
  using BlockIndex = LTC::Common::BlockIndex;
  using ChainParams = LTC::Common::ChainParams;
  using Configuration = LTC::Configuration;
  using Proto = LTC::Proto;
  using UTXODb = LTC::DB::UTXODb;
  template<typename T> using Io = BTC::Io<T>;
};
}
