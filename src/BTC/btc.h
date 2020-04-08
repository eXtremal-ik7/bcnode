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

#include <memory>
#include "blockIndex.h"
#include "../loguru.hpp"

namespace BTC {
namespace Common {
  static constexpr const char *ProjectName = "Bitcoin";
  static constexpr const char *TickerName = "BTC";
  static constexpr const char *DefaultDataDir = "bcnodebtc";
  static constexpr const char *UserAgent = "/bcnode/btc-0.1/";

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

  static inline bool setupChainParams(ChainParams *params, const char *network) {
    if (strcmp(network, "main") == 0) {
      // Setup for mainnet
      params->networkId = NetworkIdMain;
      params->magic = 0xD9B4BEF9;
      params->DefaultPort = 8333;
      params->DefaultRPCPort = 8332;

      params->powLimit.SetHex("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

      {
        // Genesis block
        params->GenesisBlock.header.nVersion = 1;
        params->GenesisBlock.header.hashPrevBlock.SetNull();
        params->GenesisBlock.header.nTime = 1231006505;
        params->GenesisBlock.header.nBits = 0x1d00ffff;
        params->GenesisBlock.header.nNonce = 2083236893;

        BTC::Proto::Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;

        xmstream scriptSig;
        tx.txIn.resize(1);
        tx.txIn[0].sequence = -1;
        tx.txIn[0].previousOutputHash.SetNull();
        tx.txIn[0].previousOutputIndex = -1;
        serialize(scriptSig, static_cast<uint8_t>(0x04));
        serialize(scriptSig, static_cast<uint32_t>(486604799));
        serialize(scriptSig, static_cast<uint8_t>(1));
        serialize(scriptSig, static_cast<uint8_t>(4));
        serialize(scriptSig, std::string("The Times 03/Jan/2009 Chancellor on brink of second bailout for banks"));
        xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);

        const unsigned char genesisOutputScript[] = {
          0x04, 0x67, 0x8a, 0xfd, 0xb0, 0xfe, 0x55, 0x48, 0x27, 0x19, 0x67, 0xf1, 0xa6, 0x71, 0x30, 0xb7,
          0x10, 0x5c, 0xd6, 0xa8, 0x28, 0xe0, 0x39, 0x09, 0xa6, 0x79, 0x62, 0xe0, 0xea, 0x1f, 0x61, 0xde,
          0xb6, 0x49, 0xf6, 0xbc, 0x3f, 0x4c, 0xef, 0x38, 0xc4, 0xf3, 0x55, 0x04, 0xe5, 0x1e, 0xc1, 0x12,
          0xde, 0x5c, 0x38, 0x4d, 0xf7, 0xba, 0x0b, 0x8d, 0x57, 0x8a, 0x4c, 0x70, 0x2b, 0x6b, 0xf1, 0x1d,
          0x5f
        };

        xmstream pkScript;
        tx.txOut.resize(1);
        pkScript.write(static_cast<uint8_t>(sizeof(genesisOutputScript)));
        pkScript.write(genesisOutputScript, sizeof(genesisOutputScript));
        pkScript.write(static_cast<uint8_t>(0xAC)); // OP_CHECKSIG
        tx.txOut[0].value = 50*100000000ULL;
        xvectorFromStream(std::move(pkScript), tx.txOut[0].pkScript);
        params->GenesisBlock.vtx.emplace_back(std::move(tx));
        params->GenesisBlock.header.hashMerkleRoot = calculateMerkleRoot(params->GenesisBlock.vtx);
        genesis_block_hash_assert_eq(params->GenesisBlock.header, "0x000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
      }

      // DNS seeds
      params->DNSSeeds.assign({
        "seed.bitcoin.sipa.be",
        "dnsseed.bluematt.me",
        "dnsseed.bitcoin.dashjr.org",
        "seed.bitcoinstats.com",
        "seed.bitcoin.jonasschnelli.ch",
        "seed.btc.petertodd.org"
      });
    } else if (strcmp(network, "testnet") == 0) {
      // Setup for testnet
      params->networkId = NetworkIdTestnet;
      params->magic = 0x0709110B;
      params->DefaultPort = 18333;
      params->DefaultRPCPort = 18332;

      params->powLimit.SetHex("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

      {
        // Genesis block
        params->GenesisBlock.header.nVersion = 1;
        params->GenesisBlock.header.hashPrevBlock.SetNull();
        params->GenesisBlock.header.nTime = 1296688602;
        params->GenesisBlock.header.nBits = 0x1d00ffff;
        params->GenesisBlock.header.nNonce = 414098458;

        BTC::Proto::Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;

        xmstream scriptSig;
        tx.txIn.resize(1);
        tx.txIn[0].sequence = -1;
        tx.txIn[0].previousOutputHash.SetNull();
        tx.txIn[0].previousOutputIndex = -1;
        serialize(scriptSig, static_cast<uint8_t>(0x04));
        serialize(scriptSig, static_cast<uint32_t>(486604799));
        serialize(scriptSig, static_cast<uint8_t>(1));
        serialize(scriptSig, static_cast<uint8_t>(4));
        serialize(scriptSig, std::string("The Times 03/Jan/2009 Chancellor on brink of second bailout for banks"));
        xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);

        const unsigned char genesisOutputScript[] = {
          0x04, 0x67, 0x8a, 0xfd, 0xb0, 0xfe, 0x55, 0x48, 0x27, 0x19, 0x67, 0xf1, 0xa6, 0x71, 0x30, 0xb7,
          0x10, 0x5c, 0xd6, 0xa8, 0x28, 0xe0, 0x39, 0x09, 0xa6, 0x79, 0x62, 0xe0, 0xea, 0x1f, 0x61, 0xde,
          0xb6, 0x49, 0xf6, 0xbc, 0x3f, 0x4c, 0xef, 0x38, 0xc4, 0xf3, 0x55, 0x04, 0xe5, 0x1e, 0xc1, 0x12,
          0xde, 0x5c, 0x38, 0x4d, 0xf7, 0xba, 0x0b, 0x8d, 0x57, 0x8a, 0x4c, 0x70, 0x2b, 0x6b, 0xf1, 0x1d,
          0x5f
        };

        tx.txOut.resize(1);
        xmstream pkScript;
        pkScript.write(static_cast<uint8_t>(sizeof(genesisOutputScript)));
        pkScript.write(genesisOutputScript, sizeof(genesisOutputScript));
        pkScript.write(static_cast<uint8_t>(0xAC)); // OP_CHECKSIG
        xvectorFromStream(std::move(pkScript), tx.txOut[0].pkScript);
        tx.txOut[0].value = 50*100000000ULL;
        params->GenesisBlock.vtx.emplace_back(std::move(tx));
        params->GenesisBlock.header.hashMerkleRoot = calculateMerkleRoot(params->GenesisBlock.vtx);
        genesis_block_hash_assert_eq(params->GenesisBlock.header, "0x000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943");
      }

      // DNS seeds
      params->DNSSeeds.assign({
        "testnet-seed.bitcoin.jonasschnelli.ch",
        "seed.tbtc.petertodd.org",
        "seed.testnet.bitcoin.sprovoost.nl",
        "testnet-seed.bluematt.me"
      });
    } else if (strcmp(network, "regtest") == 0) {
      // Setup for regtest network
      params->networkId = NetworkIdRegtest;
      params->magic = 0xDAB5BFFA;
      params->DefaultPort = 18444;
      params->DefaultRPCPort = 18443;

      params->powLimit.SetHex("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

      {
        // Genesis block
        params->GenesisBlock.header.nVersion = 1;
        params->GenesisBlock.header.hashPrevBlock.SetNull();
        params->GenesisBlock.header.nTime = 1296688602;
        params->GenesisBlock.header.nBits = 0x207fffff;
        params->GenesisBlock.header.nNonce = 2;

        BTC::Proto::Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;
        tx.txIn.resize(1);
        tx.txIn[0].sequence = -1;
        tx.txIn[0].previousOutputHash.SetNull();
        tx.txIn[0].previousOutputIndex = -1;
        xmstream scriptSig;
        serialize(scriptSig, static_cast<uint8_t>(0x04));
        serialize(scriptSig, static_cast<uint32_t>(486604799));
        serialize(scriptSig, static_cast<uint8_t>(1));
        serialize(scriptSig, static_cast<uint8_t>(4));
        serialize(scriptSig, std::string("The Times 03/Jan/2009 Chancellor on brink of second bailout for banks"));
        xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);

        const unsigned char genesisOutputScript[] = {
          0x04, 0x67, 0x8a, 0xfd, 0xb0, 0xfe, 0x55, 0x48, 0x27, 0x19, 0x67, 0xf1, 0xa6, 0x71, 0x30, 0xb7,
          0x10, 0x5c, 0xd6, 0xa8, 0x28, 0xe0, 0x39, 0x09, 0xa6, 0x79, 0x62, 0xe0, 0xea, 0x1f, 0x61, 0xde,
          0xb6, 0x49, 0xf6, 0xbc, 0x3f, 0x4c, 0xef, 0x38, 0xc4, 0xf3, 0x55, 0x04, 0xe5, 0x1e, 0xc1, 0x12,
          0xde, 0x5c, 0x38, 0x4d, 0xf7, 0xba, 0x0b, 0x8d, 0x57, 0x8a, 0x4c, 0x70, 0x2b, 0x6b, 0xf1, 0x1d,
          0x5f
        };

        tx.txOut.resize(1);
        xmstream pkScript;
        pkScript.write(static_cast<uint8_t>(sizeof(genesisOutputScript)));
        pkScript.write(genesisOutputScript, sizeof(genesisOutputScript));
        pkScript.write(static_cast<uint8_t>(0xAC)); // OP_CHECKSIG
        xvectorFromStream(std::move(pkScript), tx.txOut[0].pkScript);
        tx.txOut[0].value = 50*100000000ULL;
        params->GenesisBlock.vtx.emplace_back(std::move(tx));
        params->GenesisBlock.header.hashMerkleRoot = calculateMerkleRoot(params->GenesisBlock.vtx);
        genesis_block_hash_assert_eq(params->GenesisBlock.header, "0x0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206");
      }
    } else {
      return false;
    }

    return true;
  }

  arith_uint256 GetBlockProof(const BTC::Proto::BlockHeader &header, const ChainParams &chainParams);

  // Check functions
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  bool checkConsensus(const Proto::BlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams);
}
}
