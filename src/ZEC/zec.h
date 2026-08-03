// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "proto.h"
#include "BTC/btc.h"
#include "BTC/validation.h"
#include "BTC/merkleTree.h"
#include <string.h>

namespace ZEC {

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

namespace ZEC {
class Configuration {
public:
  static constexpr size_t MaxBlockSize = 2000000;
  static constexpr uint32_t BlocksFileLimit = BTC::Configuration::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = 256*1048576;
  static constexpr uint64_t RationalPartSize = 100000000ULL;

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
  using ChainParams = BTC::Common::ChainParamsTy<ZEC::Proto>;

  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet,
    NetworkIdRegtest
  };

  using BlockIndex = BTC::Common::BlockIndexTy<ZEC::Proto>;
  using CIndexCacheObject = BTC::Common::CIndexCacheObject;
  using CheckConsensusCtx = BTC::Common::CheckConsensusCtx;

  bool setupChainParams(ChainParams *params, const char *network);
  static inline bool hasWitness() { return false; }

  unsigned getBlockGeneration(const ChainParams &chainParams, ZEC::Common::BlockIndex *index);

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

  bool checkPow(const Proto::BlockHeader &header, uint32_t nBits, CheckConsensusCtx &, const UInt<256> &powLimit);
  UInt<256> GetBlockProof(const Proto::BlockHeader &header);

  static inline UInt<256> GetBlockProof(const Proto::BlockHeader &header, const ChainParams&) { return GetBlockProof(header); }
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  static inline bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams) { return checkPow(header, header.nBits, ctx, chainParams.powLimit); }
  static inline void checkConsensusMulti(const Proto::BlockHeader *const *headers,
                                         size_t count,
                                         CheckConsensusCtx &ctx,
                                         ChainParams &chainParams,
                                         bool *results) {
    for (size_t i = 0; i < count; i++)
      results[i] = checkConsensus(*headers[i], ctx, chainParams);
  }
};

class X {
public:
  using BlockIndex = ZEC::Common::BlockIndex;
  using ChainParams = ZEC::Common::ChainParams;
  using Configuration = ZEC::Configuration;
  using Proto = ZEC::Proto;
  using UTXODb = ZEC::DB::UTXODb;
  template<typename T> using Io = BTC::Io<T>;
};
}
