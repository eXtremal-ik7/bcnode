#pragma once

#include "common/hostAddress.h"
#include "common/intrusive_ptr.h"
#include <asyncio/asyncio.h>
#include <p2putils/HttpRequestParse.h>
#include <unordered_map>

#include "BC/bc.h"

class BlockDatabase;
class BlockInMemoryIndex;

namespace BC {
namespace DB {
class Archive;
}

namespace Network {

class Node;
class HttpApiNode;

class HttpApiConnection {
public:
  uintptr_t ref_fetch_add(uintptr_t tag) { return objectIncrementReference(aioObjectHandle(Socket_), tag); }
  uintptr_t ref_fetch_sub(uintptr_t tag) { return objectDecrementReference(aioObjectHandle(Socket_), tag); }

private:
  enum FunctionTy {
    fnUnknown = 0,
    fnAddressesInfo,
    fnAddressesTxs,
    fnAddressesUtxo,
    fnBlocksByHash,
    fnBlocksByHeight,
    fnBlocksLatest,
    fnBlocksList,
    fnBlocksRaw,
    fnBlocksTxs,
    fnMempoolSummary,
    fnMempoolTxs,
    fnSearch,
    fnStatsRichList,
    fnSystemHealth,
    fnSystemSummary,
    fnTxsByBlock,
    fnTxsByTxid,
    fnTxsLatest,
    fnTxsRaw
  };

  static std::unordered_map<std::string, HttpApiConnection::FunctionTy> FunctionNameMap_;

  BlockInMemoryIndex &BlockIndex_;
  BC::Common::ChainParams &ChainParams_;
  BlockDatabase *BlockDb_ = nullptr;
  BC::Network::Node *Node_ = nullptr;
  BC::DB::Archive *Storage_ = nullptr;
  HttpApiNode *HttpNode_ = nullptr;
  aioObject *Socket_ = nullptr;
  HostAddress Address;
  HttpRequestParserState ParserState;
  char buffer[65536];
  size_t oldDataSize = 0;

  std::atomic<unsigned> Deleted_ = 0;

  // RPC context
  struct {
    int Method = hmUnknown;
    FunctionTy Function = fnUnknown;
    std::string Path;
    std::string Request;
  } Context;

  static void socketDestructorCb(aioObjectRoot*, void *arg);
  static void readCb(AsyncOpStatus status, aioObject*, size_t size, void *arg) { static_cast<HttpApiConnection*>(arg)->onRead(status, size); }
  static void writeCb(AsyncOpStatus, aioObject*, size_t, void *arg) { static_cast<HttpApiConnection*>(arg)->onWrite(); }
  static int parseCb(HttpRequestComponent *component, void *arg) { return static_cast<HttpApiConnection*>(arg)->onParse(component); }

public:
  HttpApiConnection(BlockInMemoryIndex &blockIndex,
                    BC::Common::ChainParams &chainParams,
                    BlockDatabase &blockDb,
                    BC::Network::Node &node,
                    BC::DB::Archive &storage,
                    HttpApiNode *httpNode,
                    HostAddress address,
                    aioObject *socket);
  void start();
  int onParse(HttpRequestComponent *component);
  void onRead(AsyncOpStatus status, size_t size);
  void onWrite();

  // Functions
  void onAddressesInfo();
  void onAddressesTxs();
  void onAddressesUtxo();
  void onBlocksByHash();
  void onBlocksByHeight();
  void onBlocksLatest();
  void onBlocksList();
  void onBlocksRaw();
  void onBlocksTxs();
  void onMempoolSummary();
  void onMempoolTxs();
  void onSearch();
  void onStatsRichList();
  void onSystemHealth();
  void onSystemSummary();
  void onTxsByBlock();
  void onTxsByTxid();
  void onTxsLatest();
  void onTxsRaw();

  // Helpers
  void reply404();
  void replyNotImplemented();
  void reply200(xmstream &stream);
  size_t startChunk(xmstream &stream);
  void finishChunk(xmstream &stream, size_t offset);
  void replyWithStatus(const char *status);

  friend class HttpApiNode;
};


class HttpApiNode {
private:
  BlockInMemoryIndex *BlockIndex_;
  BC::Common::ChainParams *ChainParams_;
  BlockDatabase *BlockDb_;
  BC::Network::Node *Node_;
  BC::DB::Archive *Storage_;
  HostAddress LocalAddress;
  aioObject *ServerSocket = nullptr;

private:
  static void acceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socketFd, void *arg);
  void onAccept(HostAddress address, aioObject *socket);

public:
  bool init(BlockInMemoryIndex *blockIndex, BC::Common::ChainParams *chainParams, BlockDatabase *blockDb, BC::Network::Node *node, BC::DB::Archive &storage, asyncBase *mainBase, HostAddress localAddress);
  void removeConnection(HttpApiConnection *connection);
};

}
}
