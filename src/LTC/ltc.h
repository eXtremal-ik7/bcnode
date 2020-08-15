// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "proto.h"
#include "BTC/btc.h"
#include "common/merkleTree.h"
#include <string.h>

namespace LTC {
  // Using BTC serialization protocol
  using BTC::Io;
  using BTC::serialize;
  using BTC::unserialize;
  using BTC::unpack;
  using BTC::unpackFinalize;
  using BTC::unserializeAndCheck;
}

namespace LTC {
class Configuration {
public:
  static constexpr size_t MaxBlockSize = BTC::Configuration::MaxBlockSize;
  static constexpr uint32_t BlocksFileLimit = BTC::Configuration::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = 256*1048576;

  static constexpr const char *ProjectName = "Litecoin";
  static constexpr const char *TickerName = "LTC";
  static constexpr const char *DefaultDataDir = "bcnodeltc";
  static constexpr const char *UserAgent = "/bcnode/ltc-0.1/";
  static constexpr uint32_t ProtocolVersion = BTC::Configuration::ProtocolVersion;
  static constexpr uint64_t ServicesEnabled = BTC::Configuration::ServicesEnabled;
};

using Script = BTC::Script;

namespace Common {
  // Inherit BTC chain params
  using ChainParams = BTC::Common::ChainParams;

  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet,
    NetworkIdRegtest
  };

  using BlockIndex = BTC::Common::BlockIndex;
  using CheckConsensusCtx = BTC::Common::CheckConsensusCtx;

  bool setupChainParams(ChainParams *params, const char *network);
  static inline bool hasWitness() { return true; }

  unsigned getBlockGeneration(const ChainParams &chainParams, LTC::Common::BlockIndex *index);
  unsigned checkBlockStandalone(Proto::Block &block, const ChainParams &chainParams, std::string &error);
  bool checkBlockContextual(const BlockIndex &index, const Proto::Block &block, const ChainParams &chainParams, std::string &error);

  arith_uint256 GetBlockProof(const LTC::Proto::BlockHeader &header, const ChainParams &chainParams);
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  bool checkConsensus(const LTC::Proto::BlockHeader &header, CheckConsensusCtx &ctx, BC::Common::ChainParams &chainParams);
};

class X {
public:
  using BlockIndex = LTC::Common::BlockIndex;
  using ChainParams = LTC::Common::ChainParams;
  using Configuration = LTC::Configuration;
  using Proto = LTC::Proto;
  template<typename T> using Io = BTC::Io<T>;
};
}
