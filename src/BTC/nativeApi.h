#pragma once

#include "common/hostAddress.h"
#include "common/intrusive_ptr.h"
#include <asyncio/asyncio.h>

struct BCNodeContext;

namespace BC {
namespace Network {

class NativeApiNode;

class NativeApiConnection {
public:
  uintptr_t ref_fetch_add(uintptr_t tag) { return objectIncrementReference(aioObjectHandle(Socket), tag); }
  uintptr_t ref_fetch_sub(uintptr_t tag) { return objectDecrementReference(aioObjectHandle(Socket), tag); }

private:
  aioObject *Socket;
};

class NativeApiConnectionEmptyDeleter {
public:
  void operator()(NativeApiConnection*) {}
};

class NativeApiNode {
public:
  typedef intrusive_ptr<NativeApiConnection, NativeApiConnectionEmptyDeleter> ConnectionPtr;

private:
  BCNodeContext *Context;
  asyncBase *MainBase_;
  HostAddress LocalAddress;
  aioObject *ServerSocket = nullptr;

private:
  static void acceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socketFd, void *arg);
  void OnAccept(HostAddress address, aioObject *socket);

public:
  bool init(asyncBase *mainBase, HostAddress localAddress);
};

}
}
