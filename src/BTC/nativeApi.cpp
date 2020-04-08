// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nativeApi.h"
#include <common/bcnode.h>
#include <asyncio/socket.h>
#include "../loguru.hpp"


void BC::Network::NativeApiNode::acceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socketFd, void *arg)
{
  if (status == aosSuccess)
    static_cast<NativeApiNode*>(arg)->OnAccept(address, newSocketIo(aioGetBase(object), socketFd));
  else
    LOG_F(ERROR, "Native api accept connection failed");
  aioAccept(object, 0, acceptCb, arg);
}

bool BC::Network::NativeApiNode::init(BCNodeContext &context, HostAddress localAddress)
{
  Context = &context;
  LocalAddress = localAddress;

  char addressAsString[64];
  {
    struct in_addr a;
    a.s_addr = LocalAddress.ipv4;
    snprintf(addressAsString, sizeof(addressAsString), "%s:%u", inet_ntoa(a), static_cast<unsigned>(htons(LocalAddress.port)));
  }

  socketTy socketFd = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(socketFd);
  if (socketBind(socketFd, &LocalAddress) != 0) {
    LOG_F(ERROR, "Can't start Native API server at address %s (bind error; address already used)", addressAsString);
    socketClose(socketFd);
    return false;
  }

  if (socketListen(socketFd) != 0) {
    LOG_F(ERROR, "Can't start Native API server at address %s (listen error)", addressAsString);
    socketClose(socketFd);
    return false;
  }

  ServerSocket = newSocketIo(Context->networkBase, socketFd);
  aioAccept(ServerSocket, 0, acceptCb, this);
  LOG_F(INFO, "Native Api server started at %s", addressAsString);
  return true;
}

void BC::Network::NativeApiNode::OnAccept(HostAddress, aioObject*)
{
  LOG_F(WARNING, "New Native connection");
}
