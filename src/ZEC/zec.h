// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "proto.h"
#include "BTC/btc.h"
#include "BTC/validation.h"
#include "common/merkleTree.h"
#include <string.h>

namespace ZEC {
  // Using BTC serialization protocol
  using BTC::Io;
  using BTC::serialize;
  using BTC::unserialize;
  using BTC::unserializeAndCheck;
  using BTC::unpack2;
}

namespace ZEC {
class Configuration {
public:
  static constexpr size_t MaxBlockSize = 2000000;
  static constexpr uint32_t BlocksFileLimit = BTC::Configuration::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = 256*1048576;

  static constexpr const char *ProjectName = "ZCash";
  static constexpr const char *TickerName = "ZEC";
  static constexpr const char *DefaultDataDir = "bcnodezec";
  static constexpr const char *UserAgent = "/bcnode/zec-0.1/";
  static constexpr uint32_t ProtocolVersion = 170013;
  static constexpr uint64_t ServicesEnabled = static_cast<uint64_t>(BTC::Proto::ServicesTy::Network);
};

using Script = BTC::Script;

namespace Common {
  // Inherit BTC chain params
  struct ChainParams {
    int networkId;
    uint32_t magic;
    ZEC::Proto::Block GenesisBlock;

    // Prefixes
    std::vector<uint8_t> PublicKeyPrefix;

    // Network
    uint16_t DefaultPort;
    uint16_t DefaultRPCPort;
    std::vector<const char*> DNSSeeds;

    uint256 powLimit;
  };

  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet,
    NetworkIdRegtest
  };

  using BlockIndex = BTC::Common::BlockIndexTy<ZEC::Proto>;
  using CheckConsensusCtx = BTC::Common::CheckConsensusCtx;

  bool setupChainParams(ChainParams *params, const char *network);
  static inline bool hasWitness() { return false; }

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

  unsigned getBlockGeneration(const ChainParams &chainParams, ZEC::Common::BlockIndex *index);
  unsigned checkBlockStandalone(Proto::Block &block, const ChainParams &chainParams, std::string &error);
  bool checkBlockContextual(const BlockIndex &index, const Proto::Block &block, const ChainParams &chainParams, std::string &error);

  bool checkPow(const Proto::BlockHeader &header, uint32_t nBits, CheckConsensusCtx &, uint256 &powLimit);
  arith_uint256 GetBlockProof(const Proto::BlockHeader &header);

  static arith_uint256 GetBlockProof(const Proto::BlockHeader &header, const ChainParams&) { return GetBlockProof(header); }
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  static inline bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams) { return checkPow(header, header.nBits, ctx, chainParams.powLimit); }
};

class X {
public:
  using BlockIndex = ZEC::Common::BlockIndex;
  using ChainParams = ZEC::Common::ChainParams;
  using Configuration = ZEC::Configuration;
  using Proto = ZEC::Proto;
  template<typename T> using Io = BTC::Io<T>;
};
}
