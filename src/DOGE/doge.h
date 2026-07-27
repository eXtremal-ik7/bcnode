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
  static constexpr uint32_t BlocksFileLimit = LTC::Configuration::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = LTC::Configuration::DefaultBlockCacheSize;
  static constexpr uint64_t RationalPartSize = LTC::Configuration::RationalPartSize;

  static constexpr const char *ProjectName = "Dogecoin";
  static constexpr const char *TickerName = "DOGE";
  static constexpr const char *DefaultDataDir = "bcnodedoge";
  static constexpr const char *UserAgent = "/bcnode/doge-0.1/";
  static constexpr uint32_t ProtocolVersion = LTC::Configuration::ProtocolVersion;
  static constexpr uint64_t ServicesEnabled = LTC::Configuration::ServicesEnabled;
};

using Script = LTC::Script;

namespace Common {
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

  unsigned getBlockGeneration(const ChainParams &chainParams, BlockIndex *index);

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

  static inline UInt<256> GetBlockProof(const Proto::BlockHeader &header, const ChainParams&) {
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
  using UTXODb = DOGE::DB::UTXODb;
  template<typename T> using Io = BTC::Io<T>;
};
}
