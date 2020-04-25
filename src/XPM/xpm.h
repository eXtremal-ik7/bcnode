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

namespace XPM {
  // Using BTC serialization protocol
  using BTC::Io;
  using BTC::serialize;
  using BTC::unserialize;
  using BTC::unpack;
  using BTC::unpackFinalize;
  using BTC::unserializeAndCheck;
}

namespace XPM {
namespace Common {

  static constexpr const char *ProjectName = "Primecoin";
  static constexpr const char *TickerName = "XPM";
  static constexpr const char *DefaultDataDir = "bcnodexpm";
  static constexpr const char *UserAgent = "/bcnode/xpm-0.1/";

  static constexpr size_t MaxBlockSize = BTC::Common::MaxBlockSize;
  static constexpr uint32_t BlocksFileLimit = BTC::Common::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = 256*1048576;

  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet
  };

  using BlockIndex = BTC::Common::BlockIndexTy<XPM::Proto>;

  struct ChainParams {
    int networkId;
    uint32_t magic;
    XPM::Proto::Block GenesisBlock;

    // Prefixes
    uint8_t PublicKeyPrefix;

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

  void initialize();

  static inline bool setupChainParams(ChainParams *params, const char *network) {
    if (strcmp(network, "main") == 0) {
      // Setup for mainnet
      params->networkId = NetworkIdMain;
      params->magic = 0xE7E5E7E4;
      params->DefaultPort = 9911;
      params->DefaultRPCPort = 9912;

      params->PublicKeyPrefix = 23;

      {
        // Genesis block
        // TODO: build it correct way
        params->GenesisBlock.header.nVersion = 2;
        params->GenesisBlock.header.hashPrevBlock.SetNull();
        params->GenesisBlock.header.nTime = 1373064429;
        params->GenesisBlock.header.nBits = 0x06000000;
        params->GenesisBlock.header.nNonce = 383;
        params->GenesisBlock.header.bnPrimeChainMultiplier = ((uint64_t) 532541) * (uint64_t)(2 * 3 * 5 * 7 * 11 * 13 * 17 * 19 * 23);

        XPM::Proto::Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;
        tx.txIn.resize(1);
        tx.txOut.resize(1);
        tx.txIn[0].sequence = -1;
        tx.txIn[0].previousOutputHash.SetNull();
        tx.txIn[0].previousOutputIndex = -1;
        xmstream scriptSig;
        serialize(scriptSig, static_cast<uint8_t>(0));
        XPM::serialize(scriptSig, mpz_class(999));
        serialize(scriptSig, static_cast<uint8_t>(0x4C)); // OP_PUSHDATA1
        serialize(scriptSig, std::string("Sunny King - dedicated to Satoshi Nakamoto and all who have fought for the freedom of mankind"));
        xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);
        tx.txOut[0].value = 100000000;
        params->GenesisBlock.vtx.emplace_back(std::move(tx));
        params->GenesisBlock.header.hashMerkleRoot = calculateMerkleRoot(params->GenesisBlock.vtx);
        genesis_block_hash_assert_eq(params->GenesisBlock.header, "963d17ba4dc753138078a2f56afb3af9674e2546822badff26837db9a0152106");
      }

      params->minimalChainLength = 6;

      // DNS seeds
      params->DNSSeeds.assign({
        "seed.primecoin.info",
        "primeseed.muuttuja.org",
        "seed.primecoin.org",
        "xpm.dnsseed.coinsforall.io"
      });

    } else if (strcmp(network, "testnet") == 0) {
      // Setup for testnet
      params->networkId = NetworkIdTestnet;
      params->magic = 0xC3CBFEFB;
      params->DefaultPort = 9913;
      params->DefaultRPCPort = 9914;

      params->PublicKeyPrefix = 111;

      {
        // Genesis block
        // TODO: build it correct way
        params->GenesisBlock.header.nVersion = 2;
        params->GenesisBlock.header.hashPrevBlock.SetNull();
        params->GenesisBlock.header.nTime = 1373063882;
        params->GenesisBlock.header.nBits = 0x06000000;
        params->GenesisBlock.header.nNonce = 1513;
        params->GenesisBlock.header.bnPrimeChainMultiplier = ((uint64_t) 585641) * (uint64_t)(2 * 3 * 5 * 7 * 11 * 13 * 17 * 19 * 23);

        XPM::Proto::Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;
        tx.txIn.resize(1);
        tx.txOut.resize(1);
        xmstream scriptSig;
        tx.txIn[0].sequence = -1;
        tx.txIn[0].previousOutputHash.SetNull();
        tx.txIn[0].previousOutputIndex = -1;
        serialize(scriptSig, static_cast<uint8_t>(0));
        XPM::serialize(scriptSig, mpz_class(999));
        serialize(scriptSig, static_cast<uint8_t>(0x4C)); // OP_PUSHDATA1
        serialize(scriptSig, std::string("Sunny King - dedicated to Satoshi Nakamoto and all who have fought for the freedom of mankind"));
        xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);
        tx.txOut[0].value = 100000000;
        params->GenesisBlock.vtx.emplace_back(std::move(tx));
        params->GenesisBlock.header.hashMerkleRoot = calculateMerkleRoot(params->GenesisBlock.vtx);
        genesis_block_hash_assert_eq(params->GenesisBlock.header, "221156cf301bc3585e72de34fe1efdb6fbd703bc27cfc468faa1cdd889d0efa0");
      }

      params->minimalChainLength = 2;

      // DNS seeds
      params->DNSSeeds.assign({
        "testseed.primecoin.info",
        "primeseedtn.muuttuja.org",
        "seed.testnet.primecoin.org",
        "xpmtestnet.dnsseed.coinsforall.io"
      });
    } else {
      return false;
    }

    return true;
  }

  arith_uint256 GetBlockProof(const XPM::Proto::BlockHeader &header, const ChainParams &chainParams);

  static inline bool checkBlockSize(const XPM::Proto::Block &block, size_t serializedSize) { return BTC::Common::checkBlockSize(block, serializedSize); }

  // Consensus (PoW)
  void checkConsensusInitialize(CheckConsensusCtx &ctx);
  bool checkConsensus(const XPM::Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams);
};
}

