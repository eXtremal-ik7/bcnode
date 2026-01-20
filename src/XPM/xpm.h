// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "proto.h"

#include "BTC/blockIndex.h"
#include "BTC/defaults.h"
#include "BTC/validation.h"
#include "common/merkleTree.h"
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
  using BTC::unserialize;
  using BTC::unserializeAndCheck;
  using BTC::unpack2;
}

namespace XPM {
class Configuration {
public:
  static constexpr size_t MaxBlockSize = BTC::Common::MaxBlockSize;
  static constexpr uint32_t BlocksFileLimit = BTC::Common::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = 256*1048576;
  static constexpr uint64_t RationalPartSize = 100000000ULL;

  static constexpr const char *ProjectName = "Primecoin";
  static constexpr const char *TickerName = "XPM";
  static constexpr const char *DefaultDataDir = "bcnodexpm";
  static constexpr const char *UserAgent = "/bcnode/xpm-0.1/";
  static constexpr uint32_t ProtocolVersion = 70002;
  static constexpr uint64_t ServicesEnabled = static_cast<uint64_t>(BTC::Proto::ServicesTy::Network);
};

using Script = BTC::Script;

namespace Common {
  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet
  };

  using BlockIndex = BTC::Common::BlockIndexTy<XPM::Proto>;
  using CIndexCacheObject = BTC::Common::CIndexCacheObject;

  struct ChainParams {
    int networkId;
    uint32_t magic;
    XPM::Proto::Block GenesisBlock;

    uint32_t BIP34Height;

    // Prefixes
    std::vector<uint8_t> PublicKeyPrefix;
    std::vector<uint8_t> ScriptPrefix;

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

  UInt<256> GetBlockProof(const XPM::Proto::BlockHeader &header, const ChainParams &chainParams);

  // Consensus (PoW)
  void checkConsensusInitialize(CheckConsensusCtx &ctx);
  bool checkConsensus(const XPM::Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams);

  static inline void initializeValidationContext(const Proto::Block &block, Proto::CBlockValidationData &ctx) { BTC::validationDataInitialize(block, ctx); }

  bool checkBlockStandalone(const Proto::Block &block,
                            Proto::CBlockValidationData &validation,
                            const ChainParams &chainParams,
                            std::string &error);
  bool checkBlockContextual(const BlockIndex &index,
                            const Proto::Block &block,
                            const Proto::CBlockValidationData &validation,
                            const Proto::CBlockLinkedOutputs &linkedOutputs,
                            const ChainParams &chainParams,
                            std::string &error);
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

