#pragma once

#include "common/hostAddress.h"
#include "common/intrusive_ptr.h"
#include <asyncio/asyncio.h>
#include <p2putils/HttpRequestParse.h>
#include <tbb/concurrent_hash_map.h>

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
  uintptr_t ref_fetch_add(uintptr_t tag) { return objectIncrementReference(aioObjectHandle(Socket), tag); }
  uintptr_t ref_fetch_sub(uintptr_t tag) { return objectDecrementReference(aioObjectHandle(Socket), tag); }

private:
  enum FunctionTy {
    fnUnknown = 0,
    fnGetInfo,
    fnBlockByHash,
    fnBlockByHeight,
    fnTx,
    fnPeerInfo
  };

  BlockInMemoryIndex &BlockIndex_;
  BC::Common::ChainParams &ChainParams_;
  BlockDatabase *BlockDb_ = nullptr;
  BC::Network::Node *Node_ = nullptr;
  BC::DB::Archive *Storage_ = nullptr;
  HttpApiNode *HttpNode_ = nullptr;
  aioObject *Socket = nullptr;
  HostAddress Address;
  HttpRequestParserState ParserState;
  char buffer[65536];
  size_t oldDataSize = 0;

  std::atomic<unsigned> Deleted_ = 0;

  // RPC context
  struct {
    int method = hmUnknown;
    FunctionTy function = fnUnknown;
    unsigned argumentsNum = 0;

    // getInfo
    // blockByHash
    // tx
    BC::Proto::BlockHashTy hash;
    // blockByHeight
    uint32_t height;
    // peerInfo
  } RPCContext;

  static void socketDestructorCb(aioObjectRoot*, void *arg);
  static void readCb(AsyncOpStatus status, aioObject*, size_t size, void *arg) { static_cast<HttpApiConnection*>(arg)->OnRead(status, size); }
  static void writeCb(AsyncOpStatus, aioObject*, size_t, void *arg) { static_cast<HttpApiConnection*>(arg)->OnWrite(); }
  static int parseCb(HttpRequestComponent *component, void *arg);

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
  void OnRead(AsyncOpStatus status, size_t size);
  void OnWrite();

  // Functions
  void OnGetInfo();
  void OnBlockByHash();
  void OnBlockByHeight();
  void OnTx();
  void OnPeerInfo();

  // Helpers
  void Reply404();
  void Build200(xmstream &stream);
  size_t StartChunk(xmstream &stream);
  void FinishChunk(xmstream &stream, size_t offset);

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
  void OnAccept(HostAddress address, aioObject *socket);

public:
  bool init(BlockInMemoryIndex *blockIndex, BC::Common::ChainParams *chainParams, BlockDatabase *blockDb, BC::Network::Node *node, BC::DB::Archive &storage, asyncBase *mainBase, HostAddress localAddress);
  void removeConnection(HttpApiConnection *connection);
};

}
}
