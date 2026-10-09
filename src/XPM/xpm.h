// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "proto.h"

#include "BTC/blockIndex.h"
#include "BTC/defaults.h"
#include "BTC/validation.h"
#include "BTC/merkleTree.h"
#include "common/utils.h"

#include <string.h>
#include <functional>

namespace XPM {

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

namespace XPM {
class Configuration {
public:
  static constexpr size_t MaxBlockSize = BTC::Common::MaxBlockSize;
  // Consensus: Primecoin's MAX_BLOCK_SIGOPS_COST / WITNESS_SCALE_FACTOR, as Bitcoin's
  static constexpr unsigned MaxBlockSigOps = 20000;
  static constexpr uint32_t BlocksFileLimit = BTC::Common::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = 256*1048576;
  static constexpr uint64_t RationalPartSize = 100000000ULL;
  // Consensus: Primecoin's MAX_MONEY (Bitcoin's) and MIN_TXOUT_AMOUNT
  static constexpr int64_t MaxMoney = 21000000LL * 100000000LL;
  static constexpr int64_t MinTxOutAmount = 1000000;
  // Mempool policy. The fees are Bitcoin Core 28's, not checked against this coin's own node
  static constexpr int64_t MinRelayTxFee = 1000;
  static constexpr int64_t DustRelayTxFee = 3000;
  static constexpr bool FeeRoundsUp = false;
  static constexpr int32_t MaxStandardTxVersion = 1;
  static constexpr uint32_t MinStandardTxNonWitnessSize = 82;
  // Primecoin's own limit
  static constexpr size_t MaxStandardScriptSigSize = 500;
  static constexpr size_t MaxOpReturnRelay = 83;
  // Segwit never activated: witness outputs are not standard
  static constexpr bool HasWitness = false;

  static constexpr const char *ProjectName = "Primecoin";
  static constexpr const char *TickerName = "XPM";
  static constexpr const char *DefaultDataDir = "bcnodexpm";
  static constexpr const char *UserAgent = "/bcnode/xpm-0.1/";
  static constexpr uint32_t ProtocolVersion = 70002;
  static constexpr uint64_t ServicesEnabled = static_cast<uint64_t>(BTC::Proto::EServices::Network);
};

using Script = BTC::Script;

namespace Common {
  // What the block's place in the chain says about it; Bitcoin's answer applies
  // unchanged, the pinned repeats are empty and BIP34Height carries the rest
  using BTC::Common::fillChainContext;

  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet
  };

  using BlockIndex = BTC::Common::BlockIndexTy<XPM::Proto>;
  using CIndexCacheObject = BTC::Common::CIndexCacheObject;

  struct ChainParams {
    int networkId;
    uint32_t magic;
    XPM::Proto::CBlock GenesisBlock;
    // No feed carries the genesis block, so the databases see it only if this asks for it;
    // like Bitcoin's, XPM's genesis coinbase is unspendable and stays out of them
    bool ConnectGenesis = false;

    uint32_t BIP34Height;
    // No BIP30 repeats on XPM; kept for the shared HTTP code
    std::vector<BTC::Common::CBIP30Repeat> BIP30Repeats;

    // Prefixes
    std::vector<uint8_t> PublicKeyPrefix;
    std::vector<uint8_t> ScriptPrefix;
    // No segwit on XPM; kept for the shared HTTP code
    std::string Bech32Prefix;

    // Network
    uint16_t DefaultPort;
    uint16_t DefaultRPCPort;
    std::vector<const char*> DNSSeeds;

    // XPM specific
    uint32_t minimalChainLength;
  };

  struct CheckConsensusCtx {
    mpz_t bnPrimeChainOrigin;
    mpz_t bn;
    mpz_t exp;
    mpz_t EulerResult;
    mpz_t FermatResult;
    mpz_t two;
  };

  bool setupChainParams(ChainParams *params, const char *network);
  void initialize();
  static inline bool hasWitness() { return false; }
  // Blocks a coinbase output waits before it may be spent (Core's COINBASE_MATURITY)
  static inline uint32_t coinbaseMaturity(const ChainParams&, uint32_t) { return 3000; }

  UInt<256> GetBlockProof(const XPM::Proto::CBlockHeader &header, const ChainParams &chainParams);

  // Consensus (PoW)
  void checkConsensusInitialize(CheckConsensusCtx &ctx);
  bool checkConsensus(const XPM::Proto::CBlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams);
  static inline void checkConsensusMulti(const XPM::Proto::CBlockHeader *const *headers,
                                         size_t count,
                                         CheckConsensusCtx &ctx,
                                         ChainParams &chainParams,
                                         bool *results) {
    for (size_t i = 0; i < count; i++)
      results[i] = checkConsensus(*headers[i], ctx, chainParams);
  }

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
};

class X {
public:
  using BlockIndex = XPM::Common::BlockIndex;
  using ChainParams = XPM::Common::ChainParams;
  using Configuration = XPM::Configuration;
  using Proto = XPM::Proto;
  using UTXODb = XPM::DB::UTXODb;
  template<typename T> using Io = BTC::Io<T>;
};

}

