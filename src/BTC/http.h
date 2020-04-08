#pragma once

#include "common/hostAddress.h"
#include "common/intrusive_ptr.h"
#include <asyncio/asyncio.h>
#include <p2putils/HttpRequestParse.h>
#include <tbb/concurrent_hash_map.h>

#include "BC/proto.h"

struct BCNodeContext;

namespace BC {
namespace Network {

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
    fnPeerInfo
  };

  BCNodeContext *Context = nullptr;
  HttpApiNode *Node = nullptr;
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
  HttpApiConnection(BCNodeContext &context, HttpApiNode *node, HostAddress address, aioObject *socket);
  void start();
  void OnRead(AsyncOpStatus status, size_t size);
  void OnWrite();

  // Functions
  void OnGetInfo();
  void OnBlockByHash();
  void OnBlockByHeight();
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
  BCNodeContext *Context;
  HostAddress LocalAddress;
  aioObject *ServerSocket = nullptr;

private:
  static void acceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socketFd, void *arg);
  void OnAccept(HostAddress address, aioObject *socket);

public:
  bool init(BCNodeContext &context, HostAddress localAddress);
  void removeConnection(HttpApiConnection *connection);
};

}
}
