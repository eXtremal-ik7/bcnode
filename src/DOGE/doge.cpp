// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "doge.h"
#include "validation.h"
#include "crypto/scrypt.h"

bool DOGE::Common::setupChainParams(ChainParams *params, const char *network)
{
  if (strcmp(network, "main") == 0) {
    // Setup for mainnet
    params->networkId = NetworkIdMain;
    params->magic = 0xC0C0C0C0;
    params->DefaultPort = 22556;
    params->DefaultRPCPort = 22555;

    params->PublicKeyPrefix = {30};

    params->powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    // Soft & hard forks
    params->BIP34Height = 1034383;
    // TODO: find it
    params->SegwitHeight = 0;

    {
      params->GenesisBlock.header.nVersion = 1;
      params->GenesisBlock.header.hashPrevBlock.SetNull();
      params->GenesisBlock.header.nTime = 1386325540;
      params->GenesisBlock.header.nBits = 0x1e0ffff0;
      params->GenesisBlock.header.nNonce = 99943;

      Proto::Transaction tx;
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
      BTC::serialize(scriptSig, std::string("Nintondo"));
      xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);

      const unsigned char genesisOutputScript[65] = {
          0x04, 0x01, 0x84, 0x71, 0x0f, 0xa6, 0x89, 0xad, 0x50, 0x23, 0x69, 0x0c, 0x80, 0xf3, 0xa4, 0x9c,
          0x8f, 0x13, 0xf8, 0xd4, 0x5b, 0x8c, 0x85, 0x7f, 0xbc, 0xbc, 0x8b, 0xc4, 0xa8, 0xe4, 0xd3, 0xeb,
          0x4b, 0x10, 0xf4, 0xd4, 0x60, 0x4f, 0xa0, 0x8d, 0xce, 0x60, 0x1a, 0xaf, 0x0f, 0x47, 0x02, 0x16,
          0xfe, 0x1b, 0x51, 0x85, 0x0b, 0x4a, 0xcf, 0x21, 0xb1, 0x79, 0xc4, 0x50, 0x70, 0xac, 0x7b, 0x03,
          0xa9
      };

      xmstream pkScript;
      tx.txOut.resize(1);
      pkScript.write(static_cast<uint8_t>(sizeof(genesisOutputScript)));
      pkScript.write(genesisOutputScript, sizeof(genesisOutputScript));
      pkScript.write(static_cast<uint8_t>(0xAC)); // OP_CHECKSIG
      xvectorFromStream(std::move(pkScript), tx.txOut[0].pkScript);
      tx.txOut[0].value = 88*100000000ULL;
      params->GenesisBlock.vtx.emplace_back(std::move(tx));
      params->GenesisBlock.header.hashMerkleRoot = calculateBlockMerkleRoot(params->GenesisBlock);
      genesis_block_hash_assert_eq<DOGE::X>(params->GenesisBlock.header, "1a91e3dace36e2be3bf030a65679fe821aa1d6ef92e7c9902eb318182c355691");
    }

    // DNS seeds
    params->DNSSeeds.assign({
      "seed.multidoge.org",
      "seed2.multidoge.org"
    });

    // AuxPoW parameters
    params->StrictChainId = true;
  } else if (strcmp(network, "testnet") == 0) {
    // Setup for testnet
    params->networkId = NetworkIdTestnet;
    params->magic = 0xDCB7C1FC;
    params->DefaultPort = 44556;
    params->DefaultRPCPort = 44555;

    params->PublicKeyPrefix = {113};

    params->powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    // Soft & hard forks
    params->BIP34Height = 708658;
    // TODO: find it
    params->SegwitHeight = 0;

    {
      params->GenesisBlock.header.nVersion = 1;
      params->GenesisBlock.header.hashPrevBlock.SetNull();
      params->GenesisBlock.header.nTime = 1391503289;
      params->GenesisBlock.header.nBits = 0x1e0ffff0;
      params->GenesisBlock.header.nNonce = 997879;

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
      BTC::serialize(scriptSig, std::string("Nintondo"));
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
      tx.txOut[0].value = 88*100000000ULL;
      params->GenesisBlock.vtx.emplace_back(std::move(tx));
      params->GenesisBlock.header.hashMerkleRoot = calculateBlockMerkleRoot(params->GenesisBlock);
      genesis_block_hash_assert_eq<DOGE::X>(params->GenesisBlock.header, "bb0a78264637406b6360aad926284d544d7049f45189db5664f3c4d07350559e");
    }

    // DNS seeds
    params->DNSSeeds.assign({
      "testseed.jrn.me.uk",
    });

    // AuxPoW parameters
    params->StrictChainId = false;
  } else if (strcmp(network, "regtest") == 0) {
    params->networkId = NetworkIdRegtest;
    params->magic = 0xDAB5BFFA;

    params->powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    // Soft & hard forks
    params->BIP34Height = 500;
    params->SegwitHeight = std::numeric_limits<uint32_t>::max();

    {
      genesis_block_hash_assert_eq<DOGE::X>(params->GenesisBlock.header, "530827f38f93b43ed12af0b3ad25a288dc02ed74d6d7857862df51fc56c416f9");
    }

    // AuxPoW parameters
    params->StrictChainId = true;
  } else {
    return false;
  }

  return true;
}

void DOGE::Common::initializeValidationContext(const Proto::Block &block, DB::UTXODb &utxodb)
{
  ::initializeValidationContext<DOGE::X>(block, utxodb);
}

unsigned DOGE::Common::checkBlockStandalone(Proto::Block &block, const ChainParams &chainParams, std::string &error)
{
  bool isValid = true;
  memset(&block.validationData, 0, sizeof(block.validationData));
  applyStandaloneValidation(validateBlockSize<DOGE::X>, block, chainParams, error, &isValid);
  applyStandaloneValidation(validateMerkleRoot<DOGE::X>, block, chainParams, error, &isValid);
  applyStandaloneValidation(validateWitnessCommitment<DOGE::X>, block, chainParams, error, &isValid);
  applyStandaloneValidation(validateAuxPow, block, chainParams, error, &isValid);
  return isValid;
}

bool DOGE::Common::checkBlockContextual(const BlockIndex &index, const Proto::Block &block, const ChainParams &chainParams, std::string &error)
{
  bool isValid = true;
  applyContextualValidation(validateBIP34<DOGE::X>, index, block, chainParams, error, &isValid);
  applyContextualValidation(validateUnexpectedWitness<DOGE::X>, index, block, chainParams, error, &isValid);
  return isValid;
}

void serializeJsonInside(xmstream &stream, const DOGE::Proto::BlockHeader &header)
{
  serializeJson(stream, "version", header.nVersion); stream.write(',');
  serializeJson(stream, "hashPrevBlock", header.hashPrevBlock); stream.write(',');
  serializeJson(stream, "hashMerkleRoot", header.hashMerkleRoot); stream.write(',');
  serializeJson(stream, "time", header.nTime); stream.write(',');
  serializeJson(stream, "bits", header.nBits); stream.write(',');
  serializeJson(stream, "nonce", header.nNonce); stream.write(',');
  serializeJson(stream, "parentBlockCoinbaseTx", header.ParentBlockCoinbaseTx); stream.write(',');
  serializeJson(stream, "hashBlock", header.HashBlock); stream.write(',');
  serializeJson(stream, "merkleBranch", header.MerkleBranch); stream.write(',');
  serializeJson(stream, "index", header.Index); stream.write(',');
  serializeJson(stream, "chainMerkleBranch", header.ChainMerkleBranch); stream.write(',');
  serializeJson(stream, "chainIndex", header.ChainIndex); stream.write(',');
  stream.write("\"parentBlock\":{");
  serializeJsonInside(stream, header.ParentBlock);
  stream.write('}');
}

