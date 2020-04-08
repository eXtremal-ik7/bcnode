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
class Common {
public:
  static constexpr const char *ProjectName = "Litecoin";
  static constexpr const char *TickerName = "LTC";
  static constexpr const char *DefaultDataDir = "bcnodeltc";
  static constexpr const char *UserAgent = "/bcnode/ltc-0.1/";

  static constexpr size_t MaxBlockSize = BTC::Common::MaxBlockSize;
  static constexpr uint32_t BlocksFileLimit = BTC::Common::BlocksFileLimit;
  static constexpr size_t DefaultBlockCacheSize = 256*1048576;

  // Inherit BTC chain params
  using ChainParams = BTC::Common::ChainParams;

  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet,
    NetworkIdRegtest
  };

  using BlockIndex = BTC::Common::BlockIndex;
  using CheckConsensusCtx = BTC::Common::CheckConsensusCtx;

  static inline bool setupChainParams(ChainParams *params, const char *network) {
    if (strcmp(network, "main") == 0) {
      // Setup for mainnet
      params->networkId = NetworkIdMain;
      params->magic = 0xDBB6C0FB;
      params->DefaultPort = 9333;
      params->DefaultRPCPort = 9332;

      params->powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

      {
        params->GenesisBlock.header.nVersion = 1;
        params->GenesisBlock.header.hashPrevBlock.SetNull();
        params->GenesisBlock.header.nTime = 1317972665;
        params->GenesisBlock.header.nBits = 0x1e0ffff0;
        params->GenesisBlock.header.nNonce = 2084524493;

        LTC::Proto::Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;

        tx.txIn.resize(1);
        tx.txIn[0].sequence = -1;
        tx.txIn[0].previousOutputHash.SetNull();
        tx.txIn[0].previousOutputIndex = -1;
        xmstream scriptSig;
        BTC::serialize(scriptSig, static_cast<uint8_t>(0x04));
        BTC::serialize(scriptSig, static_cast<uint32_t>(486604799));
        BTC::serialize(scriptSig, static_cast<uint8_t>(1));
        BTC::serialize(scriptSig, static_cast<uint8_t>(4));
        BTC::serialize(scriptSig, std::string("NY Times 05/Oct/2011 Steve Jobs, Apple’s Visionary, Dies at 56"));
        xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);

        const unsigned char genesisOutputScript[65] = {
            0x04, 0x01, 0x84, 0x71, 0x0f, 0xa6, 0x89, 0xad, 0x50, 0x23, 0x69, 0x0c, 0x80, 0xf3, 0xa4, 0x9c, 0x8f, 0x13, 0xf8, 0xd4, 0x5b, 0x8c, 0x85, 0x7f, 0xbc, 0xbc, 0x8b, 0xc4, 0xa8, 0xe4, 0xd3, 0xeb,
            0x4b, 0x10, 0xf4, 0xd4, 0x60, 0x4f, 0xa0, 0x8d, 0xce, 0x60, 0x1a, 0xaf, 0x0f, 0x47, 0x02, 0x16, 0xfe, 0x1b, 0x51, 0x85, 0x0b, 0x4a, 0xcf, 0x21, 0xb1, 0x79, 0xc4, 0x50, 0x70, 0xac, 0x7b, 0x03, 0xa9
        };

        xmstream pkScript;
        tx.txOut.resize(1);
        pkScript.write(static_cast<uint8_t>(sizeof(genesisOutputScript)));
        pkScript.write(genesisOutputScript, sizeof(genesisOutputScript));
        pkScript.write(static_cast<uint8_t>(0xAC)); // OP_CHECKSIG
        xvectorFromStream(std::move(pkScript), tx.txOut[0].pkScript);
        tx.txOut[0].value = 50*100000000ULL;
        params->GenesisBlock.vtx.emplace_back(std::move(tx));
        params->GenesisBlock.header.hashMerkleRoot = calculateMerkleRoot(params->GenesisBlock.vtx);
        genesis_block_hash_assert_eq(params->GenesisBlock.header, "12a765e31ffd4059bada1e25190f6e98c99d9714d334efa41a195a7e7e04bfe2");
      }

      // DNS seeds
      params->DNSSeeds.assign({
        "seed-a.litecoin.loshan.co.uk",
        "dnsseed.thrasher.io",
        "dnsseed.litecointools.com",
        "dnsseed.litecoinpool.org",
        "dnsseed.koin-project.com"
      });
    } else if (strcmp(network, "testnet") == 0) {
      // Setup for testnet
      params->networkId = NetworkIdTestnet;
      params->magic = 0xF1C8D2FD;
      params->DefaultPort = 19335;
      params->DefaultRPCPort = 19332;

      params->powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

      {
        params->GenesisBlock.header.nVersion = 1;
        params->GenesisBlock.header.hashPrevBlock.SetNull();
        params->GenesisBlock.header.nTime = 1486949366;
        params->GenesisBlock.header.nBits = 0x1e0ffff0;
        params->GenesisBlock.header.nNonce = 293345;

        LTC::Proto::Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;

        xmstream scriptSig;
        tx.txIn.resize(1);
        tx.txIn[0].sequence = -1;
        tx.txIn[0].previousOutputHash.SetNull();
        tx.txIn[0].previousOutputIndex = -1;
        BTC::serialize(scriptSig, static_cast<uint8_t>(0x04));
        BTC::serialize(scriptSig, static_cast<uint32_t>(486604799));
        BTC::serialize(scriptSig, static_cast<uint8_t>(1));
        BTC::serialize(scriptSig, static_cast<uint8_t>(4));
        BTC::serialize(scriptSig, std::string("NY Times 05/Oct/2011 Steve Jobs, Apple’s Visionary, Dies at 56"));
        xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);

        const unsigned char genesisOutputScript[65] = {
            0x04, 0x01, 0x84, 0x71, 0x0f, 0xa6, 0x89, 0xad, 0x50, 0x23, 0x69, 0x0c, 0x80, 0xf3, 0xa4, 0x9c, 0x8f, 0x13, 0xf8, 0xd4, 0x5b, 0x8c, 0x85, 0x7f, 0xbc, 0xbc, 0x8b, 0xc4, 0xa8, 0xe4, 0xd3, 0xeb,
            0x4b, 0x10, 0xf4, 0xd4, 0x60, 0x4f, 0xa0, 0x8d, 0xce, 0x60, 0x1a, 0xaf, 0x0f, 0x47, 0x02, 0x16, 0xfe, 0x1b, 0x51, 0x85, 0x0b, 0x4a, 0xcf, 0x21, 0xb1, 0x79, 0xc4, 0x50, 0x70, 0xac, 0x7b, 0x03, 0xa9
        };

        xmstream pkScript;
        tx.txOut.resize(1);
        pkScript.write(static_cast<uint8_t>(sizeof(genesisOutputScript)));
        pkScript.write(genesisOutputScript, sizeof(genesisOutputScript));
        pkScript.write(static_cast<uint8_t>(0xAC)); // OP_CHECKSIG
        xvectorFromStream(std::move(pkScript), tx.txOut[0].pkScript);
        tx.txOut[0].value = 50*100000000ULL;
        params->GenesisBlock.vtx.emplace_back(std::move(tx));
        params->GenesisBlock.header.hashMerkleRoot = calculateMerkleRoot(params->GenesisBlock.vtx);
        genesis_block_hash_assert_eq(params->GenesisBlock.header, "4966625a4b2851d9fdee139e56211a0d88575f59ed816ff5e6a63deb4e3e29a0");
      }

      // DNS seeds
      params->DNSSeeds.assign({
        "testnet-seed.litecointools.com",
        "seed-b.litecoin.loshan.co.uk",
        "dnsseed-testnet.thrasher.io"
      });
    } else if (strcmp(network, "regtest") == 0) {
      params->networkId = NetworkIdRegtest;
      params->magic = 0xDAB5BFFA;

      params->powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

      {
        genesis_block_hash_assert_eq(params->GenesisBlock.header, "530827f38f93b43ed12af0b3ad25a288dc02ed74d6d7857862df51fc56c416f9");
      }
    } else {
      return false;
    }

    return true;
  }

  static arith_uint256 GetBlockProof(const LTC::Proto::BlockHeader &header, const ChainParams &chainParams);

  static inline bool checkBlockSize(const LTC::Proto::Block &block, size_t serializedSize) { return BTC::Common::checkBlockSize(block, serializedSize); }
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  static bool checkConsensus(const LTC::Proto::BlockHeader &header, CheckConsensusCtx &ctx, BC::Common::ChainParams &chainParams);
};
}
