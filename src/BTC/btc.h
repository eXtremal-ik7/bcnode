#pragma once

#include "proto.h"
#include "validation.h"
#include "common/uint256.h"
#include <openssl/sha.h>
#include <string.h>
#include <stdint.h>
#include <asyncio/asyncioTypes.h>

#include "blockIndex.h"
#include "../loguru.hpp"

namespace BTC {

namespace DB {
class UTXODb;
}

class Configuration {
public:
  static constexpr size_t MaxBlockSize = 1000000;
  static constexpr uint32_t BlocksFileLimit = 128*1048576;
  static constexpr size_t DefaultBlockCacheSize = 512*1048576;
  static constexpr bool HasWitness = true;
  static constexpr uint32_t ProtocolVersion = 70015;
  static constexpr uint64_t ServicesEnabled =
    static_cast<uint64_t>(BTC::Proto::ServicesTy::Network) |
    static_cast<uint64_t>(BTC::Proto::ServicesTy::Witness);

  static constexpr const char *ProjectName = "Bitcoin";
  static constexpr const char *TickerName = "BTC";
  static constexpr const char *DefaultDataDir = "bcnodebtc";
  static constexpr const char *UserAgent = "/bcnode/btc-0.1/";
};

namespace Common {
  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet,
    NetworkIdRegtest
  };


  template<typename T>
  struct ChainParamsTy {
    int networkId;
    uint32_t magic;
    BTC::Proto::BlockTy<T> GenesisBlock;

    // Soft&hard forks
    uint32_t BIP34Height;
    uint32_t SegwitHeight;

    // Prefixes
    std::vector<uint8_t> PublicKeyPrefix;
    std::vector<uint8_t> ScriptPrefix;
    std::vector<uint8_t> SecretKeyPrefix;

    // Network
    uint16_t DefaultPort;
    uint16_t DefaultRPCPort;
    std::vector<const char*> DNSSeeds;

    // ...
    uint256 powLimit;
  };

  struct CheckConsensusCtx {};

  using BlockIndex = BlockIndexTy<BTC::Proto>;
  using ChainParams = ChainParamsTy<BTC::Proto>;

  bool setupChainParams(ChainParams *params, const char *network);
  static inline bool hasWitness() { return true; }

  arith_uint256 GetBlockProof(const BTC::Proto::BlockHeader &header, const ChainParams &chainParams);

  // Check functions
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams);

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
}

class X {
public:
  using BlockIndex = BTC::Common::BlockIndex;
  using ChainParams = BTC::Common::ChainParams;
  using Configuration = BTC::Configuration;
  using Proto = BTC::Proto;
  using UTXODb = BTC::DB::UTXODb;
  template<typename T> using Io = BTC::Io<T>;
};
}
