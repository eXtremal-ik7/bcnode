// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "BC/network.h"
#include "BC/http.h"
#include "BC/nativeApi.h"

#include "common/blockDataBase.h"
#include "common/macro.h"
#include "common/thread.h"
#include "common/utils.h"
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
#else
#include <windows.h>
#endif

static int gReindex = 0;
static int gReindexOnly = 0;
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
  {"reindex-only", no_argument, &gReindexOnly, 1},
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
    if (!uriParseHostPort(addresses[i], &uri, defaultPort)) {
      LOG_F(ERROR, "Can't parse address %s", addresses[i]);
      return false;
    }

    uint16_t port = static_cast<uint16_t>(uri.port);
    if (uri.hostType == URI::HostTypeDNS) {
      struct hostent *host = gethostbyname(uri.domain.c_str());
      if (host) {
        struct in_addr **hostAddrList = (struct in_addr**)host->h_addr_list;
        unsigned j = 0;
        while (hostAddrList[j]) {
          HostAddress ha;
          ha.ipv4 = hostAddrList[j]->s_addr;
          ha.port = port;
          ha.family = AF_INET;
          output.push_back(ha);
          j++;
        }
      }
    } else if (uri.hostType == URI::HostTypeIPv4) {
      HostAddress ha;
      ha.ipv4 = uri.ipv4;
      ha.port = port;
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
  defaultDataDir.append(BC::Configuration::DefaultDataDir);
  auto defaultPath = userHomeDir().append(defaultDataDir);

  puts("bcnode options");
  puts("  --help:\t\tprint this message");
  fprintf(stdout, "  --datadir:\t\tpath of data directory (default: %s)\n", pathToUtf8(defaultPath).c_str());
  puts("  --network:\t\tnetwork name (main, testnet, etc)");
  puts("  --reindex:\t\trebuild block index");
  puts("  --reindex-only:\trebuild block index and exit");
  puts("  --resync:\t\tdelete whole database and re-download it (not supported now)");
  puts("  --watchlog:\t\tview log in current terminal");
  puts("");
}

struct Context {
  // Other objects
  asyncBase *MainBase;

  std::filesystem::path DataDir;
  std::filesystem::path BlocksDir;
  std::filesystem::path IndexDir;
  std::filesystem::path UtxoDir;

  BC::Common::ChainParams ChainParams;

  // Databases
  BlockInMemoryIndex BlockIndex;
  BlockDatabase BlockDb;
  BC::DB::Archive Archive;

  // Storage manager
  BC::DB::Storage Storage;

  // Pull pipeline (shared by the reindex reader and network catch-up)
  CBlockPipeline Pipeline;

  // Network
  BC::Network::Node Node;
  BC::Network::HttpApiNode httpApiNode;
  BC::Network::NativeApiNode nativeApiNode;
};

int main(int argc, char **argv)
{
#ifdef WIN32
  // Paths are logged as UTF-8; without this the console decodes them as the active code page
  SetConsoleOutputCP(CP_UTF8);
#endif

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

  loguru::init(argc, argv);
  loguru::set_thread_name("main");

  if (initializeAsyncIo(aiNone) != 0) {
    LOG_F(ERROR, "Can't initialize asyncio library");
    return 1;
  }

  Context context;
  if (!dataDir.empty())
    context.DataDir = dataDir;

  LOG_F(INFO, "Starting for %s/%s", BC::Configuration::ProjectName, BC::Configuration::TickerName);

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
    // The walk from a candidate stops at the connected chain, and this is where it starts
    genesisIndex->Prepared = true;
    {
      xmstream stream;
      BTC::serialize(stream, context.ChainParams.GenesisBlock);
      genesisIndex->FileNo = 0;
      genesisIndex->FileOffset = 0;
      genesisIndex->SerializedBlockSize = static_cast<uint32_t>(stream.sizeOf());

      size_t unpackedSize = 0;
      stream.seekSet(0);
      BC::Proto::Block *unpacked = BTC::unpack2<BC::Proto::Block>(stream, &unpackedSize);
      BC::Common::CIndexCacheObject *genesisObject = new BC::Common::CIndexCacheObject(nullptr, nullptr, stream.sizeOf(), 0, unpacked, unpackedSize);

      auto &outputs = genesisObject->linkedOutputs();
      outputs.Tx.resize(context.ChainParams.GenesisBlock.vtx.size());
      for (size_t i = 0; i < context.ChainParams.GenesisBlock.vtx.size(); i++)
        outputs.Tx[i].TxIn.resize(context.ChainParams.GenesisBlock.vtx[i].txIn.size());

      BC::Common::initializeValidationContext(*unpacked, genesisObject->validationData());
      genesisObject->validationData().InputsResolved = true;

      genesisIndex->Serialized.reset(genesisObject);
    }

    BC::Proto::BlockHashTy hash = context.ChainParams.GenesisBlock.header.GetHash();
    context.BlockIndex.blockIndex().insert(std::pair(hash, genesisIndex));
    context.BlockIndex.blockHeightIndex().insert(std::pair(0, genesisIndex));
    context.BlockIndex.setGenesis(genesisIndex, context.ChainParams.GenesisBlock);
    context.BlockIndex.setBest(genesisIndex);
    LOG_F(INFO, "Adding genesis block %s", hash.getHexLE().c_str());
  }

  {
    // Setup data dir
    if (context.DataDir.empty()) {
      std::string defaultDataDir;
#ifndef WIN32
      defaultDataDir.append(".");
#endif
      defaultDataDir.append(BC::Configuration::DefaultDataDir);
      context.DataDir = userHomeDir().append(defaultDataDir);
    }

    context.DataDir.append(gNetwork);
    std::filesystem::create_directories(context.DataDir);
    auto debugPath = context.DataDir / "debug.log";
    // add_file opens the name with _fsopen/fopen, so it wants the narrow OS encoding
    loguru::add_file(debugPath.string().c_str(), loguru::Append, loguru::Verbosity_INFO);
    if (!gWatchLog)
      loguru::g_stderr_verbosity = loguru::Verbosity_ERROR;
    LOG_F(INFO, "Using %s as data directory", pathToUtf8(context.DataDir).c_str());

    if (!std::filesystem::exists(context.DataDir)) {
      if (!std::filesystem::create_directories(context.DataDir)) {
        LOG_F(ERROR, "Can't create data directory %s", pathToUtf8(context.DataDir).c_str());
        return 1;
      }
    }
  }

  // Loading full configuration
  std::vector<const char*> addressesForLookup;
  config4cpp::StringVector addNode;
  config4cpp::StringVector forceNode;
  std::filesystem::path configPath = context.DataDir / "bcnode.conf";
  context.BlocksDir = context.DataDir / "blocks";
  context.IndexDir = context.DataDir / "index";
  context.UtxoDir = context.DataDir / "utxo";
  config4cpp::Configuration *cfg = config4cpp::Configuration::create();
  uint16_t bcnodePort = 0;
  uint16_t httpApiPort = 0;
  uint16_t nativeApiPort = 0;
  unsigned workerThreadsNum = 0;
  unsigned rtThreadsNum = 1;
  unsigned outgoingConnectionsLimit = 16;
  unsigned incomingConnectionsLimit = std::numeric_limits<unsigned>::max();
  bool archiveEnabled = false;
  unsigned waveThreadsNum = 0;
  // Parsed block data waiting to connect; the reindex reader throttles on it
  size_t blockCacheSize = BC::Configuration::DefaultBlockCacheSize;
  // Block data connected and waiting for the storage thread; 0 - same as the block cache
  size_t storageBacklogSize = 0;
  CBlockPipeline::CParams pipelineParams;
  if (std::filesystem::exists(configPath)) {
    try {
      cfg->parse(configPath.string().c_str());

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

      // Pull pipeline
      waveThreadsNum = cfg->lookupInt("bcnode", "waveThreadsNum", 0);
      pipelineParams.SegmentSizeLimit = static_cast<size_t>(cfg->lookupInt("bcnode", "segmentSizeMb", static_cast<int>(pipelineParams.SegmentSizeLimit / 1048576))) * 1048576;
      pipelineParams.SegmentBlocksLimit = static_cast<size_t>(cfg->lookupInt("bcnode", "segmentBlocks", static_cast<int>(pipelineParams.SegmentBlocksLimit)));
      pipelineParams.BiteFloorSize = static_cast<size_t>(cfg->lookupInt("bcnode", "biteFloorMb", static_cast<int>(pipelineParams.BiteFloorSize / 1048576))) * 1048576;
      pipelineParams.BiteFloorBlocks = static_cast<size_t>(cfg->lookupInt("bcnode", "biteFloorBlocks", static_cast<int>(pipelineParams.BiteFloorBlocks)));
      pipelineParams.ReadyQueueDepth = static_cast<size_t>(cfg->lookupInt("bcnode", "readyQueueDepth", static_cast<int>(pipelineParams.ReadyQueueDepth)));
      pipelineParams.PrepLanes = static_cast<size_t>(cfg->lookupInt("bcnode", "prepLanes", static_cast<int>(pipelineParams.PrepLanes)));
      pipelineParams.RawSizeLimit = static_cast<size_t>(cfg->lookupInt("bcnode", "readAheadMb", static_cast<int>(pipelineParams.RawSizeLimit / 1048576))) * 1048576;
      pipelineParams.PreparedSizeLimit = static_cast<size_t>(cfg->lookupInt("bcnode", "preparedMb", static_cast<int>(pipelineParams.PreparedSizeLimit / 1048576))) * 1048576;
      pipelineParams.Prefetch = cfg->lookupBoolean("bcnode", "prefetch", pipelineParams.Prefetch);
      storageBacklogSize = static_cast<size_t>(cfg->lookupInt("bcnode", "storageBacklogMb", 0)) * 1048576;
      blockCacheSize = static_cast<size_t>(cfg->lookupInt("bcnode", "blockCacheSizeMb", static_cast<int>(blockCacheSize / 1048576))) * 1048576;

      {
        const char *p = cfg->lookupString("bcnode", "indexPath", nullptr);
        if (p)
          context.IndexDir = p;
      }
      {
        const char *p = cfg->lookupString("bcnode", "utxoPath", nullptr);
        if (p)
          context.UtxoDir = p;
      }

      archiveEnabled = cfg->lookupBoolean("archive", "enabled", false);
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

  context.Storage.cache().setLimit(blockCacheSize);
  // Same budget for what waits to be written: it is the same block data, one step further on
  pipelineParams.StorageBacklogLimit = storageBacklogSize ? storageBacklogSize : blockCacheSize;

  // Lookup peers (DNS seeds and user defined nodes)
  // TODO: do it asynchronously (now using std::async)
  constexpr unsigned lookupThreadsNum = 16;
  std::vector<HostAddress> seeds[lookupThreadsNum];
  std::future<bool> workers[lookupThreadsNum];
  if (!gReindexOnly) {
    for (unsigned i = 0; i < lookupThreadsNum; i++)
      workers[i] = std::async(std::launch::async, LookupPeers, std::ref(addressesForLookup), context.ChainParams.DefaultPort, std::ref(seeds[i]), i, lookupThreadsNum);
  }

  // Handling special modes:
  //   - resync
  //   - reindex
  if (gReindexOnly)
    gReindex = 1;
  if (gReindex)
    gResync = 1;

  if (gResync) {
    // Remove utxo database
    std::error_code ec;
    std::filesystem::remove_all(context.UtxoDir, ec);
    if (ec) {
      LOG_F(ERROR, "Failed to remove database %s", "utxo");
      return 1;
    }

    // Remove all archive databases
    if (!context.Archive.purge(cfg, context.DataDir))
      return 1;
  }

  if (gReindex) {
    // Remove block index database
    std::error_code errc;
    std::filesystem::create_directories(context.IndexDir, errc);
    for (std::filesystem::directory_iterator I(context.IndexDir), IE; I != IE; ++I)
      std::filesystem::remove_all(I->path());
  }

  // Initialize storage
  if (!context.BlockDb.init(context.BlocksDir, context.IndexDir, context.ChainParams))
    return 1;
  context.Storage.init(context.BlockDb, context.BlockIndex, context.ChainParams, context.Archive);
  context.Storage.utxodb().setupCache(context.UtxoDir, context.BlockIndex);

  // Loading index
  if (!loadingBlockIndex(context.BlockIndex, context.BlocksDir, context.IndexDir))
    return 1;

  // Initialize databases
  if (!archiveEnabled) {
    // Archive disabled, processing UTXO database
    BC::Common::BlockIndex *utxoBestBlock;
    std::vector<BC::Common::BlockIndex*> forDisconnect;

    if (!context.Storage.utxodb().initialize(context.BlockIndex, context.UtxoDir, context.Storage, cfg, &utxoBestBlock, forDisconnect))
      return 1;
    if (!BC::DB::dbDisconnectBlocks(context.Storage.utxodb(), context.BlockIndex, context.ChainParams, context.Storage, forDisconnect))
      return 1;
    if (!BC::DB::dbConnectBlocks(context.Storage.utxodb(), utxoBestBlock, {}, nullptr,
                                 context.BlockIndex, context.ChainParams, context.Storage,
                                 pipelineParams.SegmentSizeLimit, "UTXO database"))
      return 1;
  } else {
    // Initialize full archive
    if (!context.Archive.init(context.BlockIndex, context.ChainParams, context.Storage, context.DataDir, context.UtxoDir, cfg))
      return 1;
  }

  context.MainBase = createAsyncBase(amOSDefault, totalThreadsNum);
  if (!context.MainBase) {
    LOG_F(ERROR, "Can't create asyncio base");
    return 1;
  }

  // Initialize storage manager
  if (!context.Storage.run([&context]() { postQuitOperation(context.MainBase); }))
    return 1;

  // Pull pipeline: shared by the reindex reader and network catch-up
  pipelineParams.WaveThreads = waveThreadsNum;
  if (!context.Pipeline.start(context.BlockIndex, context.ChainParams, context.Storage, pipelineParams))
    return 1;

  if (gReindex) {
    if (!reindex(context.BlockIndex, context.BlocksDir, context.ChainParams, context.Storage, context.Pipeline)) {
      context.Pipeline.stop();
      postQuitOperation(context.MainBase);
      return 1;
    }

    if (gReindexOnly) {
      context.Pipeline.stop();
      LOG_F(INFO, "Reindex done, exiting");
      return 0;
    }
  }

  // Starting daemon
  context.Node.Init(context.BlockIndex, context.ChainParams, context.Storage, context.Pipeline, context.MainBase, totalThreadsNum, workerThreadsNum, outgoingConnectionsLimit, incomingConnectionsLimit);

  for (size_t i = 0; i < lookupThreadsNum; i++) {
    if (!workers[i].get())
      return 1;
  }

  // Results are per lookup thread, and a thread takes every lookupThreadsNum-th address: its slot
  // names the address it looked up only while there are no more addresses than threads
  for (size_t i = 0; i < lookupThreadsNum; i++) {
    std::string addrEnumeration;
    for (size_t j = 0; j < seeds[i].size(); j++) {
      struct in_addr addr;
      addr.s_addr = seeds[i][j].ipv4;
      char addressAsString[64];
      snprintf(addressAsString, sizeof(addressAsString), "%s:%u", inet_ntoa(addr), static_cast<unsigned>(seeds[i][j].port));
      if (!addrEnumeration.empty())
        addrEnumeration.append(", ");
      addrEnumeration.append(addressAsString);
      // Connecting is Sync's business: it does the first attempt and every one after a drop
      context.Node.AddSeedAddress(seeds[i][j], addressAsString);
    }

    if (!addrEnumeration.empty() && i < addressesForLookup.size())
      LOG_F(INFO, "%s -> %s", addressesForLookup[i], addrEnumeration.c_str());
  }

  LOG_F(INFO, "DNS seeds: found %zu peers", context.Node.SeedCount());

  context.Node.Start();

  {
    HostAddress address;
    address.family = AF_INET;
    address.ipv4 = 0;

    // Start main bcnode server
    address.port = bcnodePort;
    if (!context.Node.StartBCNodeServer(address))
      return 1;

    // Start http api
    address.port = httpApiPort;
    if (!context.httpApiNode.init(&context.BlockIndex, &context.ChainParams, &context.BlockDb, &context.Node, context.Archive, context.MainBase, address))
      return 1;

    // Start native api
    if (nativeApiPort) {
      address.port = nativeApiPort;
      if (!context.nativeApiNode.init(context.MainBase, address))
        return 1;
    }
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

  // Drains the segments already prepared
  context.Pipeline.stop();

  LOG_F(INFO, "done");
  return 0;
}
