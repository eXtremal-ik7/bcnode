// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "proto.h"
#include "LTC/ltc.h"
#include "common/merkleTree.h"
#include <string.h>

namespace DOGE {
  // Using BTC serialization protocol
  using BTC::Io;
  using BTC::serialize;
  using BTC::unserialize;
  using BTC::unserializeAndCheck;
  using BTC::unpack2;
}

namespace DOGE {
class Configuration {
public:
  static constexpr size_t MaxBlockSize = LTC::Configuration::MaxBlockSize;
  static constexpr uint32_t BlocksFileLimit = LTC::Configuration::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = LTC::Configuration::DefaultBlockCacheSize;

  static constexpr const char *ProjectName = "Dogecoin";
  static constexpr const char *TickerName = "DOGE";
  static constexpr const char *DefaultDataDir = "bcnodedoge";
  static constexpr const char *UserAgent = "/bcnode/doge-0.1/";
  static constexpr uint32_t ProtocolVersion = LTC::Configuration::ProtocolVersion;
  static constexpr uint64_t ServicesEnabled = LTC::Configuration::ServicesEnabled;
};

using Script = LTC::Script;

namespace Common {
  // Inherit BTC chain params
  struct ChainParams {
    int networkId;
    uint32_t magic;
    DOGE::Proto::Block GenesisBlock;

    // Soft&hard forks
    uint32_t BIP34Height;
    uint32_t SegwitHeight;

    // Prefixes
    std::vector<uint8_t> PublicKeyPrefix;

    // Network
    uint16_t DefaultPort;
    uint16_t DefaultRPCPort;
    std::vector<const char*> DNSSeeds;

    // ...
    uint256 powLimit;

    // DOGE aux pow settings
    bool StrictChainId;
  };

  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet,
    NetworkIdRegtest
  };

  using BlockIndex = BTC::Common::BlockIndexTy<DOGE::Proto>;
  using CheckConsensusCtx = LTC::Common::CheckConsensusCtx;

  bool setupChainParams(ChainParams *params, const char *network);
  static inline bool hasWitness() { return true; }

  // Validation functions
  using ValidateStandaloneTy = std::function<bool(const Proto::Block&, const ChainParams&, std::string &error)>;
  using ValidateContextualTy = std::function<bool(const Common::BlockIndex&, const Proto::Block&, const ChainParams&, std::string &error)>;
  static inline void applyStandaloneValidation(ValidateStandaloneTy function, const Proto::Block &block, const ChainParams &chainParams, std::string &error, bool *isValid) {
    if (*isValid)
      *isValid = function(block, chainParams, error);
  }
  static inline void applyContextualValidation(ValidateContextualTy function, const Common::BlockIndex &index, const Proto::Block &block, const ChainParams &chainParams, std::string &error, bool *isValid) {
    if (*isValid)
      *isValid = function(index, block, chainParams, error);
  }

  unsigned getBlockGeneration(const ChainParams &chainParams, BlockIndex *index);
  unsigned checkBlockStandalone(Proto::Block &block, const ChainParams &chainParams, std::string &error);
  bool checkBlockContextual(const BlockIndex &index, const Proto::Block &block, const ChainParams &chainParams, std::string &error);

  static inline arith_uint256 GetBlockProof(const Proto::BlockHeader &header, const ChainParams&) {
    return LTC::Common::GetBlockProof(header);
  }

  static inline void checkConsensusInitialize(CheckConsensusCtx &ctx) { LTC::Common::checkConsensusInitialize(ctx); }
  static inline bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams) {
    return header.nVersion & Proto::BlockHeader::VERSION_AUXPOW ?
      LTC::Common::checkPow(header.ParentBlock, header.nBits, ctx, chainParams.powLimit) :
      LTC::Common::checkPow(header, header.nBits, ctx, chainParams.powLimit);
  }
};

class X {
public:
  using BlockIndex = DOGE::Common::BlockIndex;
  using ChainParams = DOGE::Common::ChainParams;
  using Configuration = DOGE::Configuration;
  using Proto = DOGE::Proto;
  template<typename T> using Io = BTC::Io<T>;
};
}
