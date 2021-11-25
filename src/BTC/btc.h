#pragma once

#include "proto.h"
#include "validation.h"
#include "common/merkleTree.h"
#include "common/serializeJson.h"
#include "common/uint256.h"
#include "common/utils.h"
#include <openssl/sha.h>
#include <atomic>
#include <string.h>
#include <stdint.h>
#include <asyncio/asyncioTypes.h>

#include <functional>
#include <memory>
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

  // Validation functions
  using ValidateStandaloneTy = std::function<bool(const Proto::Block&, const ChainParams&, std::string&)>;
  using ValidateTxStandaloneTy = std::function<bool(const Proto::ValidationData&, const Proto::Transaction&, const ChainParams&, std::string&)>;
  using ValidateContextualTy = std::function<bool(const Common::BlockIndex&, const Proto::Block&, const ChainParams&, std::string&)>;

  static inline void applyStandaloneValidation(ValidateStandaloneTy function, const Proto::Block &block, const ChainParams &chainParams, std::string &error, bool *isValid) {
    if (*isValid)
      *isValid = function(block, chainParams, error);
  }

  static inline void applyStandaloneTxValidation(ValidateTxStandaloneTy function, const Proto::ValidationData &validationData, const Proto::Transaction &tx, const ChainParams &chainParams, std::string &error, bool *isValid) {
    if (*isValid)
      *isValid = function(validationData, tx, chainParams, error);
  }

  static inline void applyContextualValidation(ValidateContextualTy function, const Common::BlockIndex &index, const Proto::Block &block, const ChainParams &chainParams, std::string &error, bool *isValid) {
    if (*isValid)
      *isValid = function(index, block, chainParams, error);
  }

  arith_uint256 GetBlockProof(const BTC::Proto::BlockHeader &header, const ChainParams &chainParams);

  // Check functions
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams);
  void initializeValidationContext(const Proto::Block &block, DB::UTXODb &utxodb);
  bool checkBlockStandalone(const Proto::Block &block, const ChainParams &chainParams, std::string &error);
  bool checkBlockContextual(const BlockIndex &index, const Proto::Block &block, const ChainParams &chainParams, std::string &error);
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
