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

  unsigned getBlockGeneration(const ChainParams &chainParams, LTC::Common::BlockIndex *index);

  bool checkPow(const Proto::BlockHeader &header, uint32_t nBits, CheckConsensusCtx &, uint256 &powLimit);
  arith_uint256 GetBlockProof(const Proto::BlockHeader &header);

  static inline arith_uint256 GetBlockProof(const Proto::BlockHeader &header, const ChainParams&) { return GetBlockProof(header); }
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  static inline bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams) { return checkPow(header, header.nBits, ctx, chainParams.powLimit); }

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
  using BlockIndex = LTC::Common::BlockIndex;
  using ChainParams = LTC::Common::ChainParams;
  using Configuration = LTC::Configuration;
  using Proto = LTC::Proto;
  using UTXODb = LTC::DB::UTXODb;
  template<typename T> using Io = BTC::Io<T>;
};
}
