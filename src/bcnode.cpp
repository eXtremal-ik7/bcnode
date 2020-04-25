// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "BC/network.h"
#include "BC/http.h"
#include "BC/nativeApi.h"

#include "common/blockDataBase.h"
#include "common/thread.h"
#include <db/archive.h>
#include <db/storage.h>
#include "loguru.hpp"

#include "asyncio/asyncio.h"
#include "asyncio/socket.h"
__NO_DEPRECATED_BEGIN
#include "config4cpp/Configuration.h"
__NO_DEPRECATED_END
#include "p2putils/uriParse.h"

#include <getopt.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <filesystem>
#include <future>
#include <string>

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

static int gReindex = 0;
static int gResync = 0;
static int gWatchLog = 0;
static const char *gNetwork = "main";


enum CmdLineOptsTy {
  clOptHelp = 1,
  clOptDataDir,
  clOptNetwork
};

static option cmdLineOpts[] = {
  {"help", no_argument, nullptr, clOptHelp},

  {"datadir", required_argument, nullptr, clOptDataDir},
  {"network", required_argument, nullptr, clOptNetwork},
  {"reindex", no_argument, &gReindex, 1},
  {"resync", no_argument, &gResync, 1},
  {"watchlog", no_argument, &gWatchLog, 1},
  {nullptr, 0, nullptr, 0}
};

static int interrupted = 0;
static void sigIntHandler(int) { interrupted = 1; }

std::filesystem::path userHomeDir()
{
  char homedir[512];
#ifdef _WIN32
  snprintf(homedir, sizeof(homedir), "%s%s\\AppData\\Roaming", getenv("HOMEDRIVE"), getenv("HOMEPATH"));
#else
  snprintf(homedir, sizeof(homedir), "%s", getenv("HOME"));
#endif
  return homedir;
}


static bool LookupPeers(std::vector<const char*> &addresses, uint16_t defaultPort, std::vector<HostAddress> &output, unsigned threadIdx, unsigned threadsNum)
{
  for (size_t i = threadIdx; i < addresses.size(); i += threadsNum) {
    URI uri;
    std::string fakeUrl = "http://";
    fakeUrl.append(addresses[i]);
    if (!uriParse(fakeUrl.c_str(), &uri)) {
      LOG_F(ERROR, "Can't parse address %s", addresses[i]);
      return false;
    }

    uint16_t port = uri.port ? uri.port : defaultPort;
    if (!uri.domain.empty()) {
      struct hostent *host = gethostbyname(uri.domain.c_str());
      if (host) {
        struct in_addr **hostAddrList = (struct in_addr**)host->h_addr_list;
        unsigned j = 0;
        while (hostAddrList[j]) {
          HostAddress ha;
          ha.ipv4 = hostAddrList[j]->s_addr;
          ha.port = htons(port);
          ha.family = AF_INET;
          output.push_back(ha);
          j++;
        }
      }
    } else if (uri.ipv4) {
      HostAddress ha;
      ha.ipv4 = uri.ipv4;
      ha.port = htons(port);
      ha.family = AF_INET;
      output.push_back(ha);
    } else {
      LOG_F(ERROR, "Can't parse address %s", addresses[i]);
      return false;
    }
  }

  return true;
}

void printHelpMessage()
{
  std::string defaultDataDir;
#ifndef WIN32
  defaultDataDir.append(".");
#endif
  defaultDataDir.append(BC::Common::DefaultDataDir);
  auto defaultPath = userHomeDir().append(defaultDataDir);

  puts("bcnode options");
  puts("  --help:\t\tprint this message");
  fprintf(stdout, "  --datadir:\t\tpath of data directory (default: %s)\n", defaultPath.u8string().c_str());
  puts("  --network:\t\tnetwork name (main, testnet, etc)");
  puts("  --reindex:\t\trebuild block index");
  puts("  --resync:\t\tdelete whole database and re-download it (not supported now)");
  puts("  --watchlog:\t\tview log in current terminal");
  puts("");
}

struct Context {
  // Other objects
  asyncBase *MainBase;
  std::filesystem::path DataDir;
  BC::Common::ChainParams ChainParams;

  // Databases
  BlockInMemoryIndex BlockIndex;
  BlockDatabase BlockDb;
  BC::DB::Archive Archive;

  // Storage manager
  BC::DB::Storage Storage;

  // Network
  BC::Network::Node Node;
  BC::Network::HttpApiNode httpApiNode;
  BC::Network::NativeApiNode nativeApiNode;
};

int main(int argc, char **argv)
{ 
  // Parsing command line
  int res;
  int index = 0;
  std::filesystem::path dataDir;
  while ((res = getopt_long(argc, argv, "", cmdLineOpts, &index)) != -1) {
    switch (res) {
      case clOptHelp :
        printHelpMessage();
        return 0;
      case clOptDataDir :
        dataDir = optarg;
        break;
      case clOptNetwork :
        gNetwork = optarg;
        break;
      case ':' :
        fprintf(stderr, "Error: option %s missing argument\n", cmdLineOpts[index].name);
        break;
      case '?' :
        exit(1);
      default :
        break;
    }
  }

  initializeSocketSubsystem();
  loguru::init(argc, argv);
  loguru::set_thread_name("main");

  Context context;
  if (!dataDir.empty())
    context.DataDir = dataDir;

  LOG_F(INFO, "Starting for %s/%s", BC::Common::ProjectName, BC::Common::TickerName);

  // Check network id
  if (!BC::Common::setupChainParams(&context.ChainParams, gNetwork)) {
    LOG_F(ERROR, "Unknown network: %s", gNetwork);
    return 1;
  }

  {
    // Add genesis block to index
    BC::Common::BlockIndex *genesisIndex = BC::Common::BlockIndex::create(BSBlock, nullptr);
    genesisIndex->SuccessorHeaders.set(nullptr, 1);
    genesisIndex->SuccessorBlocks.set(nullptr, 1);
    genesisIndex->Height = 0;
    genesisIndex->Header = context.ChainParams.GenesisBlock.header;
    genesisIndex->ChainWork = BC::Common::GetBlockProof(genesisIndex->Header, context.ChainParams);
    genesisIndex->OnChain = true;
    {
      xmstream stream;
      BTC::serialize(stream, context.ChainParams.GenesisBlock);
      genesisIndex->FileNo = 0;
      genesisIndex->FileOffset = 0;
      genesisIndex->SerializedBlockSize = static_cast<uint32_t>(stream.sizeOf());
    }
    BC::Proto::BlockHashTy hash = context.ChainParams.GenesisBlock.header.GetHash();
    context.BlockIndex.blockIndex().insert(std::pair(hash, genesisIndex));
    context.BlockIndex.blockHeightIndex().insert(std::pair(0, genesisIndex));
    context.BlockIndex.setGenesis(genesisIndex);
    context.BlockIndex.setBest(genesisIndex);
    LOG_F(INFO, "Adding genesis block %s", hash.ToString().c_str());
  }

  {
    // Setup data dir
    if (context.DataDir.empty()) {
      std::string defaultDataDir;
#ifndef WIN32
      defaultDataDir.append(".");
#endif
      defaultDataDir.append(BC::Common::DefaultDataDir);
      context.DataDir = userHomeDir().append(defaultDataDir);
    }

    context.DataDir.append(gNetwork);
    std::filesystem::create_directories(context.DataDir);
    auto debugPath = context.DataDir / "debug.log";
    loguru::add_file(debugPath.u8string().c_str(), loguru::Append, loguru::Verbosity_INFO);
    if (!gWatchLog)
      loguru::g_stderr_verbosity = loguru::Verbosity_ERROR;
    LOG_F(INFO, "Using %s as data directory", context.DataDir.u8string().c_str());

    if (!std::filesystem::exists(context.DataDir)) {
      if (!std::filesystem::create_directories(context.DataDir)) {
        LOG_F(ERROR, "Can't create data directory %s", context.DataDir.u8string().c_str());
        return 1;
      }
    }
  }

  // Loading full configuration
  std::vector<const char*> addressesForLookup;
  config4cpp::StringVector addNode;
  config4cpp::StringVector forceNode;
  std::filesystem::path configPath = context.DataDir / "bcnode.conf";
  config4cpp::Configuration *cfg = config4cpp::Configuration::create();
  uint16_t bcnodePort = 0;
  uint16_t httpApiPort = 0;
  uint16_t nativeApiPort = 0;
  unsigned workerThreadsNum = 0;
  unsigned rtThreadsNum = 1;
  unsigned outgoingConnectionsLimit = 16;
  unsigned incomingConnectionsLimit = std::numeric_limits<unsigned>::max();
  if (std::filesystem::exists(configPath)) {
    try {
      cfg->parse(configPath.u8string().c_str());


      cfg->lookupList("bcnode", "addNode", addNode, config4cpp::StringVector());
      cfg->lookupList("bcnode", "forceNode", forceNode, config4cpp::StringVector());
      if (forceNode.length() != 0) {
        for (int i = 0; i < forceNode.length(); i++)
          addressesForLookup.push_back(forceNode[i]);
      } else {
        addressesForLookup.insert(addressesForLookup.begin(), context.ChainParams.DNSSeeds.begin(), context.ChainParams.DNSSeeds.end());
        for (int i = 0; i < addNode.length(); i++)
          addressesForLookup.push_back(addNode[i]);
      }

      bcnodePort = cfg->lookupInt("bcnode", "port", context.ChainParams.DefaultPort);
      httpApiPort = cfg->lookupInt("bcnode", "httpApiPort", context.ChainParams.DefaultRPCPort);
      nativeApiPort = cfg->lookupInt("bcnode", "nativeApiPort", 0);
      workerThreadsNum = cfg->lookupInt("bcnode", "workerThreadsNum", 0);
      rtThreadsNum = cfg->lookupInt("bcnode", "realTimeThreadsNum", 1);

      outgoingConnectionsLimit = cfg->lookupInt("bcnode", "outgoingConnectionsLimit", 16);
      incomingConnectionsLimit = cfg->lookupInt("bcnode", "incomingConnectionsLimit", std::numeric_limits<unsigned>::max());

      // Load database configs
      context.Archive.txdb().getConfiguration(cfg);
      context.Archive.balancedb().getConfiguration(cfg);
    } catch(const config4cpp::ConfigurationException& ex) {
      LOG_F(ERROR, "%s", ex.c_str());
      return 1;
    }
  } else {
    // Setup default params
    bcnodePort = context.ChainParams.DefaultPort;
    httpApiPort = context.ChainParams.DefaultRPCPort;
    addressesForLookup.insert(addressesForLookup.begin(), context.ChainParams.DNSSeeds.begin(), context.ChainParams.DNSSeeds.end());
  }

  if (workerThreadsNum == 0)
    workerThreadsNum = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 2;
  unsigned totalThreadsNum = workerThreadsNum + rtThreadsNum;

  context.Storage.cache().setLimit(BC::Common::DefaultBlockCacheSize);

  // Lookup peers (DNS seeds and user defined nodes)
  // TODO: do it asynchronously (now using std::async)
  constexpr unsigned lookupThreadsNum = 16;
  std::vector<HostAddress> seeds[lookupThreadsNum];
  std::future<bool> workers[lookupThreadsNum];
  for (unsigned i = 0; i < lookupThreadsNum; i++)
    workers[i] = std::async(std::launch::async, LookupPeers, std::ref(addressesForLookup), context.ChainParams.DefaultPort, std::ref(seeds[i]), i, lookupThreadsNum);

  // Handling special modes:
  //   - resync
  //   - reindex
  if (gResync) {
    gReindex = 0;
  }

  if (gReindex && !reindex(context.BlockIndex, context.DataDir, context.ChainParams, context.Storage)) {
    return 1;
  }

  // Loading index
  if (!gResync && !gReindex && !loadingBlockIndex(context.BlockIndex, context.DataDir)) {
    return 1;
  }

  context.MainBase = createAsyncBase(amOSDefault);

  // Initialize storage
  if (!context.BlockDb.init(context.DataDir, context.ChainParams))
    return 1;

  // Initialize archive
  BC::DB::IndexDbMap forDisconnect;
  BC::Common::BlockIndex *firstBlocks[BC::DB::DbCount] = {nullptr};
  if (!context.Archive.init(context.BlockIndex, context.BlockDb, firstBlocks, forDisconnect))
    return 1;
  if (!context.Archive.sync(context.BlockIndex, context.ChainParams, context.BlockDb, firstBlocks, forDisconnect))
    return 1;

  // Initialize storage manager
  if (!context.Storage.init(context.BlockDb, context.Archive, [&context]() { postQuitOperation(context.MainBase); }))
    return 1;

  // Starting daemon
  context.Node.Init(context.BlockIndex, context.ChainParams, context.Storage, context.MainBase, totalThreadsNum, workerThreadsNum, outgoingConnectionsLimit, incomingConnectionsLimit);

  for (size_t i = 0; i < lookupThreadsNum; i++) {
    if (!workers[i].get())
      return 1;
  }

  for (size_t i = 0; i < addressesForLookup.size(); i++) {
    std::string addrEnumeration;
    for (size_t j = 0; j < seeds[i].size(); j++) {
      struct in_addr addr;
      addr.s_addr = seeds[i][j].ipv4;
      if (!addrEnumeration.empty())
        addrEnumeration.append(", ");
      addrEnumeration.append(inet_ntoa(addr));
      addrEnumeration.push_back(':');
      addrEnumeration.append(std::to_string(htons(seeds[i][j].port)));
      context.Node.AddPeer(seeds[i][j], inet_ntoa(addr), nullptr);
    }

    LOG_F(INFO, "%s -> %s", addressesForLookup[i], addrEnumeration.c_str());
  }

  LOG_F(INFO, "DNS seeds: found %zu peers", context.Node.PeerCount());

  context.Node.Start();

  {
    HostAddress address;
    address.family = AF_INET;
    address.ipv4 = 0;

    // Start main bcnode server
    address.port = htons(bcnodePort);
    if (!context.Node.StartBCNodeServer(address))
      return 1;

    // Start http api
    address.port = htons(httpApiPort);
    if (!context.httpApiNode.init(&context.BlockIndex, &context.ChainParams, &context.BlockDb, &context.Node, context.Archive, context.MainBase, address))
      return 1;

    // Start native api
    if (nativeApiPort && !context.nativeApiNode.init(context.MainBase, address))
      return 1;
  }

  std::unique_ptr<std::thread[]> workerThreads(new std::thread[totalThreadsNum]);
  for (unsigned i = 0; i < totalThreadsNum; i++) {
    workerThreads[i] = std::thread([](asyncBase *base, unsigned i, unsigned rtThreadsNum) {
      char threadName[16];
      InitializeWorkerThread();
      snprintf(threadName, sizeof(threadName), i < rtThreadsNum ? "rtworker%u" : "worker%u", GetWorkerThreadId());
      loguru::set_thread_name(threadName);
      asyncLoop(base);
    }, context.MainBase, i, rtThreadsNum);
  }

  // SIGINT (CTRL+C) monitoring
  signal(SIGINT, sigIntHandler);
  signal(SIGTERM, sigIntHandler);
  std::thread sigIntThread([&context]() {
    while (!interrupted)
      std::this_thread::sleep_for(std::chrono::seconds(1));
    LOG_F(INFO, "Interrupted by user");
    postQuitOperation(context.MainBase);
  });

  sigIntThread.detach();

  for (unsigned i = 0; i < totalThreadsNum; i++)
    workerThreads[i].join();

  LOG_F(INFO, "done");
  return 0;
}
