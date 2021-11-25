// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zec.h"
#include "BTC/script.h"

static ZEC::Proto::Block createGenesisBlock(int64_t genesisReward, uint32_t nTime, uint32_t nBits, const uint256 &nNonce, const std::string &equihashSolution)
{
  ZEC::Proto::Block block;
  block.header.nVersion = 4;
  block.header.hashPrevBlock.SetNull();
  block.header.hashMerkleRoot.SetNull();
  block.header.hashLightClientRoot.SetNull();
  block.header.nTime = nTime;
  block.header.nBits = nBits;
  block.header.nNonce = nNonce;
  block.header.nSolution.resize(equihashSolution.size() / 2);
  hex2bin(equihashSolution.data(), equihashSolution.size(), block.header.nSolution.data());

  ZEC::Proto::Transaction &tx = block.vtx.emplace_back();

  tx.version = 1;

  ZEC::Proto::TxIn &txin = tx.txIn.emplace_back();
  {
    xmstream stream;
    BTC::serialize(stream, static_cast<uint8_t>(0x04));
    BTC::serialize(stream, static_cast<uint32_t>(520617983));
    BTC::serialize(stream, static_cast<uint8_t>(1));
    BTC::serialize(stream, static_cast<uint8_t>(4));
    // blake2s(b'The Economist 2016-10-29 Known unknown: Another crypto-currency is born. BTC#436254 0000000000000000044f321997f336d2908cf8c8d6893e88dbf067e2d949487d ETH#2521903 483039a6b6bd8bd05f0584f9a078d075e454925eb71c1f13eaff59b405a721bb DJIA close on 27 Oct 2016: 18,169.68')
    BTC::serialize(stream, std::string("Zcash0b9c4eef8b7cc417ee5001e3500984b6fea35683a7cac141a043c42064835d34"));
    xvectorFromStream(std::move(stream), tx.txIn[0].scriptSig);
  }

  txin.previousOutputHash.SetNull();
  txin.previousOutputIndex = 0xFFFFFFFF;
  txin.sequence = 0xFFFFFFFF;

  ZEC::Proto::TxOut &txout = tx.txOut.emplace_back();
  txout.value = genesisReward;
  {
    static const std::string signature = "04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f";
    xmstream stream;
    BTC::serialize(stream, static_cast<uint8_t>(signature.size()/2));
    hex2bin(signature.data(), signature.size(), stream.reserve(signature.size()/2));
    BTC::serialize(stream, static_cast<uint8_t>(BTC::Script::OP_CHECKSIG));
    xvectorFromStream(std::move(stream), tx.txOut[0].pkScript);
  }

  tx.lockTime = 0;

  block.header.hashMerkleRoot = calculateBlockMerkleRoot(block);
  return block;
}

bool ZEC::Common::setupChainParams(ChainParams *params, const char *network)
{
  if (strcmp(network, "main") == 0) {
    // Setup for mainnet
    params->networkId = NetworkIdMain;
    params->magic = 0x6427E924;
    params->DefaultPort = 8233;
    params->DefaultRPCPort = 8232;
    params->PublicKeyPrefix = {0x1C,0xB8};    // t1

    params->powLimit = uint256S("0007ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    {
      static const std::string equihashSolution =
          "000a889f00854b8665cd555f4656f68179d31ccadc1b1f7fb0952726313b16941da348284d67add4686121d4e3d930160c1348d8191c25f12b267a6a9c131b5031cbf8af1f79c9d513076a216ec87ed0"
          "45fa966e01214ed83ca02dc1797270a454720d3206ac7d931a0a680c5c5e099057592570ca9bdf6058343958b31901fce1a15a4f38fd347750912e14004c73dfe588b903b6c03166582eeaf30529b140"
          "72a7b3079e3a684601b9b3024054201f7440b0ee9eb1a7120ff43f713735494aa27b1f8bab60d7f398bca14f6abb2adbf29b04099121438a7974b078a11635b594e9170f1086140b4173822dd6978944"
          "83e1c6b4e8b8dcd5cb12ca4903bc61e108871d4d915a9093c18ac9b02b6716ce1013ca2c1174e319c1a570215bc9ab5f7564765f7be20524dc3fdf8aa356fd94d445e05ab165ad8bb4a0db096c097618"
          "c81098f91443c719416d39837af6de85015dca0de89462b1d8386758b2cf8a99e00953b308032ae44c35e05eb71842922eb69797f68813b59caf266cb6c213569ae3280505421a7e3a0a37fdf8e2ea35"
          "4fc5422816655394a9454bac542a9298f176e211020d63dee6852c40de02267e2fc9d5e1ff2ad9309506f02a1a71a0501b16d0d36f70cdfd8de78116c0c506ee0b8ddfdeb561acadf31746b5a9dd32c2"
          "1930884397fb1682164cb565cc14e089d66635a32618f7eb05fe05082b8a3fae620571660a6b89886eac53dec109d7cbb6930ca698a168f301a950be152da1be2b9e07516995e20baceebecb5579d7cd"
          "bc16d09f3a50cb3c7dffe33f26686d4ff3f8946ee6475e98cf7b3cf9062b6966e838f865ff3de5fb064a37a21da7bb8dfd2501a29e184f207caaba364f36f2329a77515dcb710e29ffbf73e2bbd773fa"
          "b1f9a6b005567affff605c132e4e4dd69f36bd201005458cfbd2c658701eb2a700251cefd886b1e674ae816d3f719bac64be649c172ba27a4fd55947d95d53ba4cbc73de97b8af5ed4840b659370c556"
          "e7376457f51e5ebb66018849923db82c1c9a819f173cccdb8f3324b239609a300018d0fb094adf5bd7cbb3834c69e6d0b3798065c525b20f040e965e1a161af78ff7561cd874f5f1b75aa0bc77f72058"
          "9e1b810f831eac5073e6dd46d00a2793f70f7427f0f798f2f53a67e615e65d356e66fe40609a958a05edb4c175bcc383ea0530e67ddbe479a898943c6e3074c6fcc252d6014de3a3d292b03f0d88d312"
          "fe221be7be7e3c59d07fa0f2f4029e364f1f355c5d01fa53770d0cd76d82bf7e60f6903bc1beb772e6fde4a70be51d9c7e03c8d6d8dfb361a234ba47c470fe630820bbd920715621b9fbedb49fcee165"
          "ead0875e6c2b1af16f50b5d6140cc981122fcbcf7c5a4e3772b3661b628e08380abc545957e59f634705b1bbde2f0b4e055a5ec5676d859be77e20962b645e051a880fddb0180b4555789e1f9344a436"
          "a84dc5579e2553f1e5fb0a599c137be36cabbed0319831fea3fddf94ddc7971e4bcf02cdc93294a9aab3e3b13e3b058235b4f4ec06ba4ceaa49d675b4ba80716f3bc6976b1fbf9c8bf1f3e3a4dc1cd83"
          "ef9cf816667fb94f1e923ff63fef072e6a19321e4812f96cb0ffa864da50ad74deb76917a336f31dce03ed5f0303aad5e6a83634f9fcc371096f8288b8f02ddded5ff1bb9d49331e4a84dbe154316443"
          "8fde9ad71dab024779dcdde0b6602b5ae0a6265c14b94edd83b37403f4b78fcd2ed555b596402c28ee81d87a909c4e8722b30c71ecdd861b05f61f8b1231795c76adba2fdefa451b283a5d527955b9f3"
         "de1b9828e7b2e74123dd47062ddcc09b05e7fa13cb2212a6fdbc65d7e852cec463ec6fd929f5b8483cf3052113b13dac91b69f49d1b7d1aec01c4a68e41ce157";

      params->GenesisBlock = createGenesisBlock(0, 1477641360, 0x1f07ffff, uint256S("0x0000000000000000000000000000000000000000000000000000000000001257"), equihashSolution);
      genesis_block_hash_assert_eq<ZEC::X>(params->GenesisBlock.header, "00040fe8ec8471911baa1db1266ea15dd06b4a8a5c453883c000b031973dce08");
    }

    // DNS seeds
    params->DNSSeeds.assign({
      "dnsseed.z.cash",
      "dnsseed.str4d.xyz",
      "mainnet.seeder.zfnd.org",
      "mainnet.is.yolo.money"
    });
  } else if (strcmp(network, "testnet") == 0) {
    // Setup for testnet
    params->networkId = NetworkIdTestnet;
    params->magic = 0xBFF91AFA;
    params->DefaultPort = 18233;
    params->DefaultRPCPort = 18232;
    params->PublicKeyPrefix = {0x1D,0x25};    // tm
    params->powLimit = uint256S("07ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    {
      static const std::string equihashSolution =
          "00a6a51259c3f6732481e2d035197218b7a69504461d04335503cd69759b2d02bd2b53a9653f42cb33c608511c953673fa9da76170958115fe92157ad3bb5720d927f18e09459bf5c6072973e143e20f"
          "9bdf0584058c96b7c2234c7565f100d5eea083ba5d3dbaff9f0681799a113e7beff4a611d2b49590563109962baa149b628aae869af791f2f70bb041bd7ebfa658570917f6654a142b05e7ec0289a4f4"
          "6470be7be5f693b90173eaaa6e84907170f32602204f1f4e1c04b1830116ffd0c54f0b1caa9a5698357bd8aa1f5ac8fc93b405265d824ba0e49f69dab5446653927298e6b7bdc61ee86ff31c07bde863"
          "31b4e500d42e4e50417e285502684b7966184505b885b42819a88469d1e9cf55072d7f3510f85580db689302eab377e4e11b14a91fdd0df7627efc048934f0aff8e7eb77eb17b3a95de13678004f2512"
          "293891d8baf8dde0ef69be520a58bbd6038ce899c9594cf3e30b8c3d9c7ecc832d4c19a6212747b50724e6f70f6451f78fd27b58ce43ca33b1641304a916186cfbe7dbca224f55d08530ba851e4df22b"
          "af7ab7078e9cbea46c0798b35a750f54103b0cdd08c81a6505c4932f6bfbd492a9fced31d54e98b6370d4c96600552fcf5b37780ed18c8787d03200963600db297a8f05dfa551321d17b9917edadcda5"
          "1e274830749d133ad226f8bb6b94f13b4f77e67b35b71f52112ce9ba5da706ad9573584a2570a4ff25d29ab9761a06bdcf2c33638bf9baf2054825037881c14adf3816ba0cbd0fca689aad3ce16f2fe3"
          "62c98f48134a9221765d939f0b49677d1c2447e56b46859f1810e2cf23e82a53e0d44f34dae932581b3b7f49eaec59af872cf9de757a964f7b33d143a36c270189508fcafe19398e4d2966948164d405"
          "56b05b7ff532f66f5d1edc41334ef742f78221dfe0c7ae2275bb3f24c89ae35f00afeea4e6ed187b866b209dc6e83b660593fce7c40e143beb07ac86c56f39e895385924667efe3a3f031938753c7764"
          "a2dbeb0a643fd359c46e614873fd0424e435fa7fac083b9a41a9d6bf7e284eee537ea7c50dd239f359941a43dc982745184bf3ee31a8dc850316aa9c6b66d6985acee814373be3458550659e1a06287c"
          "3b3b76a185c5cb93e38c1eebcf34ff072894b6430aed8d34122dafd925c46a515cca79b0269c92b301890ca6b0dc8b679cdac0f23318c105de73d7a46d16d2dad988d49c22e9963c117960bdc70ef0db"
          "6b091cf09445a516176b7f6d58ec29539166cc8a38bbff387acefffab2ea5faad0e8bb70625716ef0edf61940733c25993ea3de9f0be23d36e7cb8da10505f9dc426cd0e6e5b173ab4fff8c37e1f1fb5"
          "6d1ea372013d075e0934c6919393cfc21395eea20718fad03542a4162a9ded66c814ad8320b2d7c2da3ecaf206da34c502db2096d1c46699a91dd1c432f019ad434e2c1ce507f91104f66f491fed37b2"
          "25b8e0b2888c37276cfa0468fc13b8d593fd9a2675f0f5b20b8a15f8fa7558176a530d6865738ddb25d3426dab905221681cf9da0e0200eea5b2eba3ad3a5237d2a391f9074bf1779a2005cee43eec2b"
          "058511532635e0fea61664f531ac2b356f40db5c5d275a4cf5c82d468976455af4e3362cc8f71aa95e71d394aff3ead6f7101279f95bcd8a0fedce1d21cb3c9f6dd3b182fce0db5d6712981b651f2917"
          "8a24119968b14783cafa713bc5f2a65205a42e4ce9dc7ba462bdb1f3e4553afc15f5f39998fdb53e7e231e3e520a46943734a007c2daa1eda9f495791657eefcac5c32833936e568d06187857ed04d7b"
          "97167ae207c5c5ae54e528c36016a984235e9c5b2f0718d7b3aa93c7822ccc772580b6599671b3c02ece8a21399abd33cfd3028790133167d0a97e7de53dc8ff";

      params->GenesisBlock = createGenesisBlock(0, 1477648033, 0x2007ffff, uint256S("0x0000000000000000000000000000000000000000000000000000000000000006"), equihashSolution);
      genesis_block_hash_assert_eq<ZEC::X>(params->GenesisBlock.header, "05a60a92d99d85997cce3b87616c089f6124d7342af37106edc76126334a2c38");
    }

    // DNS seeds
    params->DNSSeeds.assign({
      "dnsseed.testnet.z.cash",
      "testnet.seeder.zfnd.org",
      "testnet.is.yolo.money"
    });
  } else if (strcmp(network, "regtest") == 0) {
    params->networkId = NetworkIdRegtest;
    params->magic = 0x5F3FE8AA;
    params->DefaultPort = 18344;
    params->DefaultRPCPort = 18232;
    params->PublicKeyPrefix = {0x1D,0x25};    // tm
    params->powLimit = uint256S("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f");

    // Soft & hard forks
    {
      static const std::string equihashSolution =
          "00a6a51259c3f6732481e2d035197218b7a69504461d04335503cd69759b2d02bd2b53a9653f42cb33c608511c953673fa9da76170958115fe92157ad3bb5720d927f18e09459bf5c6072973e143e20f"
          "9bdf0584058c96b7c2234c7565f100d5eea083ba5d3dbaff9f0681799a113e7beff4a611d2b49590563109962baa149b628aae869af791f2f70bb041bd7ebfa658570917f6654a142b05e7ec0289a4f4"
          "6470be7be5f693b90173eaaa6e84907170f32602204f1f4e1c04b1830116ffd0c54f0b1caa9a5698357bd8aa1f5ac8fc93b405265d824ba0e49f69dab5446653927298e6b7bdc61ee86ff31c07bde863"
          "31b4e500d42e4e50417e285502684b7966184505b885b42819a88469d1e9cf55072d7f3510f85580db689302eab377e4e11b14a91fdd0df7627efc048934f0aff8e7eb77eb17b3a95de13678004f2512"
          "293891d8baf8dde0ef69be520a58bbd6038ce899c9594cf3e30b8c3d9c7ecc832d4c19a6212747b50724e6f70f6451f78fd27b58ce43ca33b1641304a916186cfbe7dbca224f55d08530ba851e4df22b"
          "af7ab7078e9cbea46c0798b35a750f54103b0cdd08c81a6505c4932f6bfbd492a9fced31d54e98b6370d4c96600552fcf5b37780ed18c8787d03200963600db297a8f05dfa551321d17b9917edadcda5"
          "1e274830749d133ad226f8bb6b94f13b4f77e67b35b71f52112ce9ba5da706ad9573584a2570a4ff25d29ab9761a06bdcf2c33638bf9baf2054825037881c14adf3816ba0cbd0fca689aad3ce16f2fe3"
          "62c98f48134a9221765d939f0b49677d1c2447e56b46859f1810e2cf23e82a53e0d44f34dae932581b3b7f49eaec59af872cf9de757a964f7b33d143a36c270189508fcafe19398e4d2966948164d405"
          "56b05b7ff532f66f5d1edc41334ef742f78221dfe0c7ae2275bb3f24c89ae35f00afeea4e6ed187b866b209dc6e83b660593fce7c40e143beb07ac86c56f39e895385924667efe3a3f031938753c7764"
          "a2dbeb0a643fd359c46e614873fd0424e435fa7fac083b9a41a9d6bf7e284eee537ea7c50dd239f359941a43dc982745184bf3ee31a8dc850316aa9c6b66d6985acee814373be3458550659e1a06287c"
          "3b3b76a185c5cb93e38c1eebcf34ff072894b6430aed8d34122dafd925c46a515cca79b0269c92b301890ca6b0dc8b679cdac0f23318c105de73d7a46d16d2dad988d49c22e9963c117960bdc70ef0db"
          "6b091cf09445a516176b7f6d58ec29539166cc8a38bbff387acefffab2ea5faad0e8bb70625716ef0edf61940733c25993ea3de9f0be23d36e7cb8da10505f9dc426cd0e6e5b173ab4fff8c37e1f1fb5"
          "6d1ea372013d075e0934c6919393cfc21395eea20718fad03542a4162a9ded66c814ad8320b2d7c2da3ecaf206da34c502db2096d1c46699a91dd1c432f019ad434e2c1ce507f91104f66f491fed37b2"
          "25b8e0b2888c37276cfa0468fc13b8d593fd9a2675f0f5b20b8a15f8fa7558176a530d6865738ddb25d3426dab905221681cf9da0e0200eea5b2eba3ad3a5237d2a391f9074bf1779a2005cee43eec2b"
          "058511532635e0fea61664f531ac2b356f40db5c5d275a4cf5c82d468976455af4e3362cc8f71aa95e71d394aff3ead6f7101279f95bcd8a0fedce1d21cb3c9f6dd3b182fce0db5d6712981b651f2917"
          "8a24119968b14783cafa713bc5f2a65205a42e4ce9dc7ba462bdb1f3e4553afc15f5f39998fdb53e7e231e3e520a46943734a007c2daa1eda9f495791657eefcac5c32833936e568d06187857ed04d7b"
          "97167ae207c5c5ae54e528c36016a984235e9c5b2f0718d7b3aa93c7822ccc772580b6599671b3c02ece8a21399abd33cfd3028790133167d0a97e7de53dc8ff";

      params->GenesisBlock = createGenesisBlock(0, 1296688602, 0x200f0f0f, uint256S("0x0000000000000000000000000000000000000000000000000000000000000009"), equihashSolution);
      genesis_block_hash_assert_eq<ZEC::X>(params->GenesisBlock.header, "0000000000000000000000000000000000000000000000000000000000000000");
    }
  } else {
    return false;
  }

  return true;
}

bool ZEC::Common::checkPow(const Proto::BlockHeader &header, uint32_t nBits, CheckConsensusCtx&, uint256 &powLimit)
{
  // TODO: equihash check implement
  return true;
}

arith_uint256 ZEC::Common::GetBlockProof(const Proto::BlockHeader &header)
{
  arith_uint256 bnTarget;
  bool fNegative;
  bool fOverflow;
  bnTarget.SetCompact(header.nBits, &fNegative, &fOverflow);
  if (fNegative || fOverflow || bnTarget == 0)
      return 0;
  // We need to compute 2**256 / (bnTarget+1), but we can't represent 2**256
  // as it's too large for an arith_uint256. However, as 2**256 is at least as large
  // as bnTarget+1, it is equal to ((2**256 - bnTarget - 1) / (bnTarget+1)) + 1,
  // or ~bnTarget / (bnTarget+1) + 1.
  return (~bnTarget / (bnTarget + 1)) + 1;
}

void ZEC::Common::initializeValidationContext(const Proto::Block &block, DB::UTXODb &utxodb)
{
  ::initializeValidationContext<ZEC::X>(block, utxodb);
}

unsigned ZEC::Common::checkBlockStandalone(Proto::Block &block, const ChainParams &chainParams, std::string &error)
{
  bool isValid = true;
  memset(&block.validationData, 0, sizeof(block.validationData));
  applyStandaloneValidation(validateBlockSize<ZEC::X>, block, chainParams, error, &isValid);
  applyStandaloneValidation(validateMerkleRoot<ZEC::X>, block, chainParams, error, &isValid);
  return isValid;
}

bool ZEC::Common::checkBlockContextual(const BlockIndex &index, const Proto::Block &block, const ChainParams &chainParams, std::string &error)
{
  bool isValid = true;
  return isValid;
}
