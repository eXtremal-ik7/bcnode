#include "BC/bc.h"
#include "common/smallStream.h"
#include "crypto/sha256.h"

#include <asyncio/asyncio.h>
#include <asyncio/socket.h>
#include <asyncioextras/btc.h>
#include <openssl/evp.h>
#include <p2putils/uriParse.h>

#include "secp256k1.h"
#include <getopt.h>

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

enum CmdLineOptsTy {
  clOptHelp = 1,
};

static option cmdLineOpts[] = {
  {"help", no_argument, nullptr, clOptHelp},
  {nullptr, 0, nullptr, 0}
};

struct CAddressInfo {
  BC::Proto::AddressTy Address;
  BC::Proto::PrivateKeyTy PrivateKey;
  uint8_t PublicKeyUncompressed[65];
  CAddressInfo() {}
  CAddressInfo(const CAddressInfo &addr) : Address(addr.Address), PrivateKey(addr.PrivateKey) {
    memcpy(PublicKeyUncompressed, addr.PublicKeyUncompressed, sizeof(PublicKeyUncompressed));
  }
  CAddressInfo(const BC::Proto::AddressTy &address,
               const BC::Proto::PrivateKeyTy &privateKey,
               uint8_t *publicKeyUncompressed) : Address(address), PrivateKey(privateKey) {
    memcpy(PublicKeyUncompressed, publicKeyUncompressed, sizeof(PublicKeyUncompressed));
  }
};

struct CLocalUtxo {
  BC::Proto::TxHashTy TxId;
  uint32_t TxOutIndex;
  CAddressInfo Addr;
  uint64_t Value;
  CLocalUtxo() {}
  CLocalUtxo(const BC::Proto::TxHashTy &txId,
             uint32_t txOutIndex,
             const CAddressInfo &addr,
             uint64_t value) : TxId(txId), TxOutIndex(txOutIndex), Addr(addr), Value(value) {}
};

static void printHelpMessage(const char *name)
{
  printf("Usage: %s <options> <address>\n", name);
}

static bool lookupPeer(const char *address, HostAddress *out, uint16_t defaultPort)
{
  URI uri;
  std::string fakeUrl = "http://";
  fakeUrl.append(address);
  if (!uriParse(fakeUrl.c_str(), &uri)) {
    return false;
  }

  uint16_t port = uri.port ? static_cast<uint16_t>(uri.port) : defaultPort;
  if (!uri.domain.empty()) {
    struct hostent *host = gethostbyname(uri.domain.c_str());
    if (host) {
      struct in_addr **hostAddrList = reinterpret_cast<struct in_addr**>(host->h_addr_list);
      out->ipv4 = hostAddrList[0]->s_addr;
      out->port = htons(port);
      out->family = AF_INET;
    } else {
      return false;
    }
  } else if (uri.ipv4) {
    out->ipv4 = uri.ipv4;
    out->port = htons(port);
    out->family = AF_INET;
  } else {
    return false;
  }

  return true;
}

class CTestImpl {
public:
  bool init(asyncBase *base, HostAddress address, const BC::Common::ChainParams &chainParams) {
    Base_ = base;
    Address_ = address;
    ChainParams_ = chainParams;

    // Initialize test context
    srand(12345);
    Context_.Secp256k1Ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    {
      // Miner address
      makePseudoRandomAddress(Context_.Secp256k1Ctx, Context_.MinerAddress);
      printf(" * miner address: %s (pk: %s)\n",
             encodeBase58WithCrc(&chainParams.PublicKeyPrefix[0], chainParams.PublicKeyPrefix.size(), Context_.MinerAddress.Address.begin(), sizeof(BC::Proto::AddressTy)).c_str(),
             encodeBase58WithCrc(&chainParams.SecretKeyPrefix[0], chainParams.SecretKeyPrefix.size(), Context_.MinerAddress.PrivateKey.begin(), sizeof(BC::Proto::PrivateKeyTy)).c_str());

      // Recipient addresses
      for (size_t i = 0; i < Context_.RecipientAddresses.size(); i++)
        makePseudoRandomAddress(Context_.Secp256k1Ctx, Context_.RecipientAddresses[i]);
    }

    socketTy socketFd = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
    HostAddress localAddress;
    localAddress.family = AF_INET;
    localAddress.ipv4 = INADDR_ANY;
    localAddress.port = 0;
    if (socketBind(socketFd, &localAddress) != 0) {
      socketClose(socketFd);
      return false;
    }

    aioObject *object = newSocketIo(Base_, socketFd);
    Socket_ = btcSocketNew(Base_, object);
    btcSocketSetMagic(Socket_, chainParams.magic);

    return true;
  }

  void start() {
    coroutineTy *coro = coroutineNew([](void *arg) {
      static_cast<CTestImpl*>(arg)->run();
    }, this, 0x20000);

    coroutineCall(coro);
  }

private:
  void run() {
    uint32_t tm = 1684160000;

    // Connect
    if (ioConnect(btcGetPlainSocket(Socket_), &Address_, 5*1000000) != aosSuccess) {
      fprintf(stderr, "Can't connect to node\n");
      finish();
    }

    // Send version message
    sendVersion();
    // Wait version and verack
    if (!waitConnectionMsg()) {
      fprintf(stderr, "Can't establish connection\n");
      finish();
    }

    xmstream blockMsg;
    BC::Proto::BlockHashTy prevBlockHash = ChainParams_.GenesisBlock.header.GetHash();
    uint32_t height = 1;
    std::vector<BC::Proto::BlockHashTy> blocks;
    std::vector<CLocalUtxo> localUtxo;

    for (unsigned i = 0; i < 500; i++) {
      BC::Proto::MessageBlock block;
      block.vtx.emplace_back();
      finalizeBlock(block,
                    4,
                    prevBlockHash,
                    tm - 4*(500 - i),
                    0x207fffff,
                    height,
                    maxRewardForHeight(height),
                    Context_.MinerAddress.Address);

      blocks.push_back(block.header.GetHash());
      localUtxo.emplace_back(block.vtx[0].getTxId(), 0u, Context_.MinerAddress, maxRewardForHeight(height));
      blockMsg.reset();
      serialize(blockMsg, block);
      ioBtcSend(Socket_, "block", blockMsg.data(), blockMsg.sizeOf(), afNone, 0);
      prevBlockHash = block.header.GetHash();
      height++;
    }

    {
      // send coins to 12 addresses
      BC::Proto::MessageBlock block;
      block.vtx.emplace_back();

      for (unsigned i = 0; i < 12; i++) {
        BC::Proto::Transaction &tx = block.vtx.emplace_back();
        buildP2PKHTransaction(Context_.Secp256k1Ctx, tx, {localUtxo[i]}, Context_.RecipientAddresses[i].Address, localUtxo[i].Value);
      }

      finalizeBlock(block,
                    4,
                    prevBlockHash,
                    tm,
                    0x207fffff,
                    height,
                    maxRewardForHeight(height),
                    Context_.MinerAddress.Address);

      blockMsg.reset();
      serialize(blockMsg, block);
      ioBtcSend(Socket_, "block", blockMsg.data(), blockMsg.sizeOf(), afNone, 0);
      height++;
    }

    // Remove last block with transactions from main chain
    --height;
    prevBlockHash = blocks.back();
    for (unsigned i = 0; i < 3; i++) {
      BC::Proto::MessageBlock block;
      block.vtx.emplace_back();
      finalizeBlock(block,
                    4,
                    prevBlockHash,
                    tm,
                    0x207fffff,
                    height,
                    maxRewardForHeight(height),
                    Context_.MinerAddress.Address);

      blocks.push_back(block.header.GetHash());
      localUtxo.emplace_back(block.vtx[0].getTxId(), 0u, Context_.MinerAddress, maxRewardForHeight(height));

      blockMsg.reset();
      serialize(blockMsg, block);
      ioBtcSend(Socket_, "block", blockMsg.data(), blockMsg.sizeOf(), afNone, 0);

      prevBlockHash = block.header.GetHash();
      height++;
    }
  }

  void makePseudoRandomAddress(secp256k1_context *secp256k1Ctx, CAddressInfo &address) {
    bool result;
    secp256k1_pubkey pubKey;
    size_t pkLen = 65;

    for (unsigned i = 0; i < address.PrivateKey.size(); i++)
      address.PrivateKey.begin()[i] = rand();
    result = secp256k1_ec_pubkey_create(secp256k1Ctx, &pubKey, address.PrivateKey.begin());
    assert(result && "secp256k1_ec_pubkey_create failed");

    result = secp256k1_ec_pubkey_serialize(secp256k1Ctx, address.PublicKeyUncompressed, &pkLen, &pubKey, SECP256K1_EC_UNCOMPRESSED);
    assert(result && pkLen == 65 && "secp256k1_ec_pubkey_serialize failed");

    uint8_t sha256[32];
    {
      CCtxSha256 ctx;
      sha256Init(&ctx);
      sha256Update(&ctx, address.PublicKeyUncompressed, pkLen);
      sha256Final(&ctx, sha256);
    }

    {
      unsigned outSize = 0;
      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      EVP_DigestInit_ex(ctx, EVP_ripemd160(), nullptr);
      EVP_DigestUpdate(ctx, sha256, sizeof(sha256));
      EVP_DigestFinal_ex(ctx, address.Address.begin(), &outSize);
      EVP_MD_CTX_free(ctx);
    }
  }

  void sendVersion() {
    BC::Proto::MessageVersion msg;
    msg.version = BC::Configuration::ProtocolVersion;
    msg.services = 1; // NODE
    msg.timestamp = static_cast<uint64_t>(time(nullptr));
    msg.addr_recv.services = 0;
    msg.addr_recv.setIpv4(0);
    msg.addr_recv.port = 0;
    msg.addr_from.services = 0; // NODE
    msg.addr_from.reset();
    msg.addr_from.port = 0;
    msg.nonce = rand();
    msg.user_agent = BC::Configuration::UserAgent;
    msg.start_height = 1;
    msg.relay = 1;

    char buffer[1024];
    xmstream localStream(buffer, sizeof(buffer));
    localStream.reset();
    BC::serialize(localStream, msg);
    ioBtcSend(Socket_, "version", localStream.data(), localStream.sizeOf(), afNone, 5000000);
  }

  void finish() {
    postQuitOperation(Base_);
  }

  uint64_t maxRewardForHeight(uint32_t height) {
    return 50ULL*100000000 / (1u << (height / 150));
  }

  bool waitConnectionMsg() {
    bool versionReceived = false;
    bool verackReceived = false;
    bool needReceive = true;
    while ( needReceive && ioBtcRecv(Socket_, Command_, ReceiveStream_, Limit_, afNone, 5*1000000) >= 0) {
      if (strcmp(Command_, "version") == 0) {
        BC::Proto::MessageVersion version;
        if (!unserializeAndCheck(ReceiveStream_, version))
          return false;
        printf(" * Version received %s height %u\n", version.user_agent.c_str(), version.start_height);

        if (version.start_height != 0) {
          fprintf(stderr, " * ERROR: node not clean\n");
          return false;
        }

        ioBtcSend(Socket_, "verack", nullptr, 0, afNone, 0);
        versionReceived = true;
      } else if (strcmp(Command_, "verack") == 0) {
        verackReceived = true;
      }

      needReceive = !versionReceived || !verackReceived;
    }

    return !needReceive;
  }

  static int64_t buildP2PKHTransaction(secp256k1_context *secp256k1Ctx,
                                       BC::Proto::Transaction &tx,
                                       const std::vector<CLocalUtxo> &inputs,
                                       const BC::Proto::AddressTy &output,
                                       int64_t value) {
    uint8_t p2pkhOutput[sizeof(BC::Proto::AddressTy) + 5];
    p2pkhOutput[0] = BTC::Script::OP_DUP;
    p2pkhOutput[1] = BTC::Script::OP_HASH160;
    p2pkhOutput[2] = sizeof(BTC::Proto::AddressTy);
    p2pkhOutput[3 + sizeof(BC::Proto::AddressTy)] = BTC::Script::OP_EQUALVERIFY;
    p2pkhOutput[4 + sizeof(BC::Proto::AddressTy)] = BTC::Script::OP_CHECKSIG;

    int64_t change = 0;

    tx.version = 1;

    for (size_t i = 0; i < inputs.size(); i++) {
      BC::Proto::TxIn &txIn = tx.txIn.emplace_back();
      txIn.previousOutputHash = inputs[i].TxId;
      txIn.previousOutputIndex = inputs[i].TxOutIndex;
      txIn.sequence = std::numeric_limits<uint32_t>::max();
      change += inputs[i].Value;
    }

    // single output
    BC::Proto::TxOut &txOut = tx.txOut.emplace_back();
    txOut.value = value;
    txOut.pkScript.resize(sizeof(BC::Proto::AddressTy) + 5);
    xmstream p2pkh(txOut.pkScript.data(), txOut.pkScript.size());
    p2pkh.write<uint8_t>(BTC::Script::OP_DUP);
    p2pkh.write<uint8_t>(BTC::Script::OP_HASH160);
    p2pkh.write<uint8_t>(sizeof(BTC::Proto::AddressTy));
    p2pkh.write(output.begin(), sizeof(BC::Proto::AddressTy));
    p2pkh.write<uint8_t>(BTC::Script::OP_EQUALVERIFY);
    p2pkh.write<uint8_t>(BTC::Script::OP_CHECKSIG);

    tx.lockTime = 0;

    // sign transaction
    for (size_t i = 0; i < inputs.size(); i++) {
      BC::Proto::TxIn &txIn = tx.txIn[i];

      // restore utxo scriptPubKey
      memcpy(p2pkhOutput + 3, inputs[i].Addr.Address.begin(), sizeof(BC::Proto::AddressTy));

      // serialize transaction for sign
      uint8_t sigHash[32];
      SmallStream<4096> sigData;
      BC::Io<BC::Proto::Transaction>::serializeForSignature(sigData, tx, i, p2pkhOutput, sizeof(p2pkhOutput));
      sigData.write<int32_t>(1);

      {
        CCtxSha256 ctx;
        sha256Init(&ctx);
        sha256Update(&ctx, sigData.data(), sigData.sizeOf());
        sha256Final(&ctx, sigHash);
        sha256Init(&ctx);
        sha256Update(&ctx, sigHash, sizeof(sigHash));
        sha256Final(&ctx, sigHash);
      }

      {
        secp256k1_ecdsa_signature sig;
        unsigned char extraEntropy[32];
        size_t sigLen = 72;
        uint8_t sigData[72];
        // Only for repeatability of result, don't use rand() or
        // others weak random generators for extra entropy!!
        for (unsigned j = 0; j < 32; j++)
          extraEntropy[j] = rand();

        secp256k1_ecdsa_sign(secp256k1Ctx, &sig, sigHash, inputs[i].Addr.PrivateKey.begin(), secp256k1_nonce_function_rfc6979, extraEntropy);
        secp256k1_ecdsa_signature_serialize_der(secp256k1Ctx, sigData, &sigLen, &sig);

        // Build tx input
        {
          xmstream txin;
          txin.write<uint8_t>(sigLen+1);
          txin.write(sigData, sigLen);
          txin.write<uint8_t>(1);
          txin.write<uint8_t>(sizeof(inputs[i].Addr.PublicKeyUncompressed));
          txin.write(inputs[i].Addr.PublicKeyUncompressed, sizeof(inputs[i].Addr.PublicKeyUncompressed));
          xvectorFromStream(std::move(txin), txIn.scriptSig);
        }
      }
    }

    change -= value;
    return change;
  }

  void finalizeBlock(BC::Proto::Block &block,
                     uint32_t version,
                     const BC::Proto::BlockHashTy &prev,
                     uint32_t time,
                     uint32_t bits,
                     uint64_t height,
                     uint64_t reward,
                     const BC::Proto::AddressTy &miningAddress)
  {
    block.header.nVersion = version;
    block.header.hashPrevBlock = prev;
    block.header.nTime = time;
    block.header.nBits = bits;
    block.header.nNonce = 0;

    // Build coinbase transaction
    BC::Proto::Transaction &coinbaseTx = block.vtx[0];
    coinbaseTx.version = 2;
    {
      // txin
      BC::Proto::TxIn &txIn = coinbaseTx.txIn.emplace_back();
      txIn.previousOutputHash.SetNull();
      txIn.previousOutputIndex = 0xFFFFFFFFu;
      // Witness nonce
      // Use default: 0
      txIn.witnessStack.resize(1);
      txIn.witnessStack[0].resize(32);
      memset(txIn.witnessStack[0].data(), 0, 32);

      // scriptsig
      xmstream scriptsig;
      // Height
      serializeForCoinbase(scriptsig, height);
      // Message
      scriptsig.write("test");
      // Extra nonce (8 bytes)
      scriptsig.write<uint64_t>(0);

      xvectorFromStream(std::move(scriptsig), txIn.scriptSig);
      txIn.sequence = std::numeric_limits<uint32_t>::max();
    }
    {
      // txout
      BC::Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
      txOut.value = reward;

      // pkscript (use single P2PKH)
      txOut.pkScript.resize(sizeof(BC::Proto::AddressTy) + 5);
      xmstream p2pkh(txOut.pkScript.data(), txOut.pkScript.size());
      p2pkh.write<uint8_t>(BTC::Script::OP_DUP);
      p2pkh.write<uint8_t>(BTC::Script::OP_HASH160);
      p2pkh.write<uint8_t>(sizeof(BC::Proto::AddressTy));
      p2pkh.write(miningAddress.begin(), miningAddress.size());
      p2pkh.write<uint8_t>(BTC::Script::OP_EQUALVERIFY);
      p2pkh.write<uint8_t>(BTC::Script::OP_CHECKSIG);
      {
        // segwit
        // calculate commitment
        xmstream witnessCommitment;
        std::vector<uint256> witnessHashes;
        witnessHashes.emplace_back();
        witnessHashes.back().SetNull();
        for (size_t i = 1; i < block.vtx.size(); i++)
          witnessHashes.push_back(block.vtx[i].getWTxid());
        uint256 witnessMerkleRoot = calculateMerkleRoot(&witnessHashes[0], witnessHashes.size());
        uint256 commitment;
        {
          uint8_t defaultWitnessNonce[32];
          memset(defaultWitnessNonce, 0, sizeof(defaultWitnessNonce));
          CCtxSha256 ctx;
          sha256Init(&ctx);
          sha256Update(&ctx, witnessMerkleRoot.begin(), witnessMerkleRoot.size());
          sha256Update(&ctx, defaultWitnessNonce, 32);
          sha256Final(&ctx, commitment.begin());
          sha256Init(&ctx);
          sha256Update(&ctx, commitment.begin(), commitment.size());
          sha256Final(&ctx, commitment.begin());
        }

        uint8_t prefix[6] = {0x6A, 0x24, 0xAA, 0x21, 0xA9, 0xED};
        witnessCommitment.write(prefix, sizeof(prefix));
        witnessCommitment.write(commitment.begin(), commitment.size());

        BC::Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
        txOut.value = 0;
        txOut.pkScript.resize(witnessCommitment.sizeOf());
        memcpy(txOut.pkScript.data(), witnessCommitment.data(), witnessCommitment.sizeOf());
      }
    }

    coinbaseTx.lockTime = 0;

    // Merkle root
    std::vector<BC::Proto::TxHashTy> txHashes;
    for (size_t i = 0; i < block.vtx.size(); i++)
      txHashes.push_back(block.vtx[i].getTxId());
    block.header.hashMerkleRoot = calculateMerkleRoot(&txHashes[0], txHashes.size());

    // Nonce
    {
      BC::Common::CheckConsensusCtx ctx;
      BC::Common::checkConsensusInitialize(ctx);
      while (!BC::Common::checkConsensus(block.header, ctx, ChainParams_))
        block.header.nNonce++;
    }
  }

  static inline void serializeForCoinbase(xmstream &stream, int64_t value) {
    if (value == -1 || (value >= 1 && value <= 16)) {
      stream.write<uint8_t>(value + 0x51 - 1);
    } else if (value == 0) {
      stream.write<uint8_t>(0);
    } else {
      size_t offset = stream.offsetOf();
      stream.write<uint8_t>(0);

      bool isNegative = value < 0;
      uint64_t absValue = isNegative ? -value : value;
      while (absValue >= 0x100)  {
        stream.write<uint8_t>(absValue & 0xFF);
        absValue >>= 8;
      }

      if (absValue & 0x80) {
        stream.write<uint8_t>(static_cast<uint8_t>(absValue));
        stream.write<uint8_t>(isNegative ? 0x80 : 0);
      } else {
        stream.write<uint8_t>(static_cast<uint8_t>(absValue | (isNegative ? 0x80 : 0)));
      }

      stream.data<uint8_t>()[offset] = static_cast<uint8_t>(stream.offsetOf() - offset - 1);
    }
  }

private:
  asyncBase *Base_ = nullptr;
  HostAddress Address_;
  BC::Common::ChainParams ChainParams_;
  BTCSocket *Socket_;
  char Command_[12];
  xmstream ReceiveStream_;

  struct {
    secp256k1_context *Secp256k1Ctx;
    CAddressInfo MinerAddress;
    std::array<CAddressInfo, 16> RecipientAddresses;
  } Context_;

  static constexpr size_t Limit_ = 67108864; // 64Mb
};

int main(int argc, char **argv)
{
  std::string addressString;

  initializeSocketSubsystem();

  int res;
  int index = 0;
  while ((res = getopt_long(argc, argv, "", cmdLineOpts, &index)) != -1) {
    switch (res) {
      case clOptHelp :
        printHelpMessage(argv[0]);
        return 0;
      case ':' :
        fprintf(stderr, "Error: option %s missing argument\n", cmdLineOpts[index].name);
        break;
      case '?' :
        exit(1);
      default :
        break;
    }
  }

  if (optind == argc) {
    fprintf(stderr, "ERROR: You must specify node address\n");
    exit(1);
  }

  addressString = argv[optind];

  BC::Common::ChainParams chainParams;
  if (!BC::Common::setupChainParams(&chainParams, "regtest")) {
    fprintf(stderr, "Unknown network: %s", "main");
    exit(1);
  }

  // Connect to node by p2p
  HostAddress address;
  if (!lookupPeer(addressString.c_str(), &address, chainParams.DefaultPort)) {
    fprintf(stderr, "ERROR: Can't resolve %s\n", addressString.c_str());
    return 1;
  }

  asyncBase *base = createAsyncBase(amOSDefault);

  CTestImpl test;
  if (!test.init(base, address, chainParams)) {
    fprintf(stderr, "ERROR: Test suite initialization failed\n");
    return 1;
  }

  test.start();
  asyncLoop(base);
  return 0;
}
