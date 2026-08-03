#pragma once

#include "proto.h"
#include "validation.h"
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
  static constexpr uint64_t RationalPartSize = 100000000ULL;

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
    // The coin's own block type: LTC's is not the generic BlockTy the others alias
    typename T::Block GenesisBlock;

    // Soft&hard forks
    uint32_t BIP34Height;
    uint32_t SegwitHeight;

    // Blocks valid despite repeating an earlier coinbase transaction: BIP30 was
    // not enforced when they were mined, so the repeat overwrites the earlier
    // coin. Pinned by height AND hash (Core's IsBIP30Repeat) so no other block
    // at the same height inherits the exemption; BIP34 made new ones impossible
    std::vector<std::pair<uint32_t, Proto::BlockHashTy>> BIP30Repeats;

    // Prefixes
    std::vector<uint8_t> PublicKeyPrefix;
    std::vector<uint8_t> ScriptPrefix;
    std::vector<uint8_t> SecretKeyPrefix;
    // Segwit address HRP (BIP173); empty on chains without segwit
    std::string Bech32Prefix;

    // Network
    uint16_t DefaultPort;
    uint16_t DefaultRPCPort;
    std::vector<const char*> DNSSeeds;

    // ...
    UInt<256> powLimit;
  };

  struct CheckConsensusCtx {};

  using BlockIndex = BlockIndexTy<BTC::Proto>;
  using ChainParams = ChainParamsTy<BTC::Proto>;

  bool setupChainParams(ChainParams *params, const char *network);
  static inline bool hasWitness() { return true; }

  UInt<256> GetBlockProof(const BTC::Proto::BlockHeader &header, const ChainParams &chainParams);

  // Check functions
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams);
  // Group check: the pipeline hands over a whole chunk so a chain with a multi-way hash kernel
  // can use it. Here the group is just walked
  static inline void checkConsensusMulti(const Proto::BlockHeader *const *headers,
                                         size_t count,
                                         CheckConsensusCtx &ctx,
                                         ChainParams &chainParams,
                                         bool *results) {
    for (size_t i = 0; i < count; i++)
      results[i] = checkConsensus(*headers[i], ctx, chainParams);
  }

  static inline void initializeValidationContext(const Proto::Block &block, Proto::CBlockValidationData &ctx) { BTC::validationDataInitialize(block, ctx); }

  bool checkBlockStandalone(const Proto::Block &block,
                            Proto::CBlockValidationData &validation,
                            const ChainParams &chainParams,
                            std::string &error);
  // Validation data is non-const: this is where a block learns the consensus
  // exemptions its place in the chain grants it
  bool checkBlockContextual(const BlockIndex &index,
                            const Proto::Block &block,
                            Proto::CBlockValidationData &validation,
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
