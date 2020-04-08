// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "http.h"
#include "blockIndex.h"
#include <common/bcnode.h>
#include "common/blockDataBase.h"
#include "common/merkleTree.h"
#include <asyncio/socket.h>
#include <stdio.h>
#include "../loguru.hpp"

template<const char s[]>
static bool rawcmp(Raw data) {
  return data.size == strlen(s) && memcmp(data.data, s, strlen(s)) == 0;
}

// HttpApiConnection

void BC::Network::HttpApiConnection::socketDestructorCb(aioObjectRoot*, void *arg)
{
  delete static_cast<BC::Network::HttpApiConnection*>(arg);
}

BC::Network::HttpApiConnection::HttpApiConnection(BCNodeContext &context, HttpApiNode *node, HostAddress address, aioObject *socket) :
  Context(&context), Node(node), Socket(socket), Address(address)
{
  httpRequestParserInit(&ParserState);
  objectSetDestructorCb(aioObjectHandle(Socket), socketDestructorCb, this);
}

void BC::Network::HttpApiConnection::start()
{
  aioRead(Socket, buffer, sizeof(buffer), afNone, 0, readCb, this);
}

int BC::Network::HttpApiConnection::parseCb(HttpRequestComponent *component, void *arg)
{
  static constexpr char getInfo[] = "getInfo";
  static constexpr char blockByHash[] = "blockByHash";
  static constexpr char blockByHeight[] = "blockByHeight";
  static constexpr char peerInfo[] = "peerInfo";
  BC::Network::HttpApiConnection *connection = static_cast<BC::Network::HttpApiConnection*>(arg);
  if (component->type == httpRequestDtMethod) {
    connection->RPCContext.method = component->method;
    connection->RPCContext.function = fnUnknown;
    connection->RPCContext.argumentsNum = 0;
    return 1;
  }

  if (connection->RPCContext.function == fnUnknown && component->type == httpRequestDtUriPathElement) {
    int requiredMethod = hmUnknown;
    if (rawcmp<getInfo>(component->data)) {
      requiredMethod = hmGet;
      connection->RPCContext.function = fnGetInfo;
    } else if (rawcmp<blockByHash>(component->data)) {
      requiredMethod = hmGet;
      connection->RPCContext.function = fnBlockByHash;
    } else if (rawcmp<blockByHeight>(component->data)) {
      requiredMethod = hmGet;
      connection->RPCContext.function = fnBlockByHeight;
    } else if (rawcmp<peerInfo>(component->data)) {
      requiredMethod = hmGet;
      connection->RPCContext.function = fnPeerInfo;
    } else {
      connection->Reply404();
      return 0;
    }

    if (connection->RPCContext.method != requiredMethod) {
      connection->Reply404();
      return 0;
    }

    return 1;
  }

  switch (connection->RPCContext.function) {
    case fnGetInfo : {
      if (component->type == httpRequestDtDataLast)
        connection->OnGetInfo();
      break;
    }

    case fnBlockByHash : {
      if (connection->RPCContext.argumentsNum == 0 && component->type == httpRequestDtUriPathElement) {
        std::string hash(component->data.data, component->data.data + component->data.size);
        connection->RPCContext.hash.SetHex(hash);
        connection->RPCContext.argumentsNum = 1;
      }

      if (component->type == httpRequestDtDataLast) {
        if (connection->RPCContext.argumentsNum == 1) {
          connection->OnBlockByHash();
        } else {
          connection->Reply404();
          return 0;
        }

      }

      break;
    }

    case fnBlockByHeight : {
      if (connection->RPCContext.argumentsNum == 0 && component->type == httpRequestDtUriPathElement) {
        std::string height(component->data.data, component->data.data + component->data.size);
        connection->RPCContext.height = xatoi<unsigned>(height.c_str());
        connection->RPCContext.argumentsNum = 1;
      }

      if (component->type == httpRequestDtDataLast) {
        if (connection->RPCContext.argumentsNum == 1) {
          connection->OnBlockByHeight();
        } else {
          connection->Reply404();
          return 0;
        }
      }

      break;
    }

    case fnPeerInfo : {
      if (component->type == httpRequestDtDataLast)
        connection->OnPeerInfo();
      break;
    }

    case fnUnknown : {
      connection->Reply404();
      return 0;
    }
  }

  return 1;
}

void BC::Network::HttpApiConnection::OnRead(AsyncOpStatus status, size_t)
{
  if (status != aosSuccess) {
    Node->removeConnection(this);
    return;
  }

  httpRequestSetBuffer(&ParserState, buffer + oldDataSize, sizeof(buffer) - oldDataSize);

  switch (httpRequestParse(&ParserState, parseCb, this)) {
    case ParserResultOk : {
      // TODO: check keep-alive
      break;
    }

    case ParserResultNeedMoreData : {
      // copy 'tail' to begin of buffer
      oldDataSize = httpRequestDataRemaining(&ParserState);
      if (oldDataSize)
        memcpy(buffer, httpRequestDataPtr(&ParserState), oldDataSize);

      aioRead(Socket, buffer+oldDataSize, sizeof(buffer)-oldDataSize, afNone, 0, readCb, this);
      break;
    }

    case ParserResultError : {
      Node->removeConnection(this);
      break;
    }

    case ParserResultCancelled : {
      Node->removeConnection(this);
      break;
    }
  }
}

void BC::Network::HttpApiConnection::OnWrite()
{
  // TODO: check keep alive
  Node->removeConnection(this);
}

void BC::Network::HttpApiConnection::OnGetInfo()
{
  xmstream stream;
  Build200(stream);
  size_t offset = StartChunk(stream);
  stream.write("{}", 2);
  FinishChunk(stream, offset);
  aioWrite(Socket, stream.data(), stream.sizeOf(), afNone, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::OnBlockByHash()
{
  size_t size;
  BlockSearcher searcher(*Context);
  BC::Common::BlockIndex *index = searcher.add(RPCContext.hash);
  if (index) {
    searcher.fetchPending();
    if (void *data = searcher.next(&size)) {
      BC::Proto::Block block;
      {
        xmstream source(data, size);
        BC::unserialize(source, block);
      }

      xmstream stream;
      Build200(stream);
      size_t offset = StartChunk(stream);
      serializeJson(stream, *index, block);
      FinishChunk(stream, offset);
      aioWrite(Socket, stream.data(), stream.sizeOf(), afNone, 0, writeCb, this);
      return;
    }
  }

  Reply404();
}

void BC::Network::HttpApiConnection::OnBlockByHeight()
{
  auto It = Context->blockHeightIndex.find(RPCContext.height);
  if (It == Context->blockHeightIndex.end()) {
    Reply404();
    return;
  }

  size_t size;
  BlockSearcher searcher(*Context);
  BC::Common::BlockIndex *index = searcher.add(It->second->Header.GetHash());
  if (index) {
    searcher.fetchPending();
    if (void *data = searcher.next(&size)) {
      BC::Proto::Block block;
      {
        xmstream source(data, size);
        BC::unserialize(source, block);
      }

      xmstream stream;
      Build200(stream);
      size_t offset = StartChunk(stream);
      serializeJson(stream, *index, block);
      FinishChunk(stream, offset);
      aioWrite(Socket, stream.data(), stream.sizeOf(), afNone, 0, writeCb, this);
      return;
    }
  }

  Reply404();
}

void BC::Network::HttpApiConnection::OnPeerInfo()
{
  xmstream stream;
  Build200(stream);
  size_t offset = StartChunk(stream);
  bool firstPeer = true;
  stream.write('[');
  Context->PeerManager.enumeratePeers([&stream, &firstPeer](Peer *peer) {
    if (!firstPeer)
      stream.write(',');
    serializeJson(stream, *peer);
    firstPeer = false;
  });
  stream.write(']');

  FinishChunk(stream, offset);
  aioWrite(Socket, stream.data(), stream.sizeOf(), afNone, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::Build200(xmstream &stream)
{
  const char reply200[] = "HTTP/1.1 200 OK\r\nServer: bcnode\r\nTransfer-Encoding: chunked\r\n\r\n";
  stream.write(reply200, sizeof(reply200)-1);
}

void BC::Network::HttpApiConnection::Reply404()
{
  const char reply404[] = "HTTP/1.1 404 Not Found\r\nServer: bcnode\r\nTransfer-Encoding: chunked\r\n\r\n";
  const char html[] = "<html><head><title>Not Found</title></head><body><h1>404 Not Found</h1></body></html>";

  char buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  stream.write(reply404, sizeof(reply404)-1);

  size_t offset = StartChunk(stream);
  stream.write(html);
  FinishChunk(stream, offset);

  aioWrite(Socket, stream.data(), stream.sizeOf(), afNone, 0, writeCb, this);
}

size_t BC::Network::HttpApiConnection::StartChunk(xmstream &stream)
{
  size_t offset = stream.offsetOf();
  stream.write("00000000\r\n", 10);
  return offset;
}

void BC::Network::HttpApiConnection::FinishChunk(xmstream &stream, size_t offset)
{
  char hex[16];
  char finishData[] = "\r\n0\r\n\r\n";
  sprintf(hex, "%08x", static_cast<unsigned>(stream.offsetOf() - offset - 10));
  memcpy(stream.data<uint8_t>() + offset, hex, 8);
  stream.write(finishData, sizeof(finishData));
}

// HttpApiNode

void BC::Network::HttpApiNode::acceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socketFd, void *arg)
{
  if (status == aosSuccess)
    static_cast<HttpApiNode*>(arg)->OnAccept(address, newSocketIo(aioGetBase(object), socketFd));
  else
    LOG_F(ERROR, "HTTP api accept connection failed");
  aioAccept(object, 0, acceptCb, arg);
}

bool BC::Network::HttpApiNode::init(BCNodeContext &context, HostAddress localAddress)
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
    LOG_F(ERROR, "Can't start HTTP API server at address %s (bind error; address already used)", addressAsString);
    socketClose(socketFd);
    return false;
  }

  if (socketListen(socketFd) != 0) {
    LOG_F(ERROR, "Can't start HTTP API server at address %s (listen error)", addressAsString);
    socketClose(socketFd);
    return false;
  }

  ServerSocket = newSocketIo(Context->networkBase, socketFd);
  aioAccept(ServerSocket, 0, acceptCb, this);
  LOG_F(INFO, "HTTP Api server started at %s", addressAsString);
  return true;
}

void BC::Network::HttpApiNode::OnAccept(HostAddress address, aioObject *socket)
{
  HttpApiConnection *connection = new HttpApiConnection(*Context, this, address, socket);
  connection->start();
}

void BC::Network::HttpApiNode::removeConnection(HttpApiConnection *connection)
{
  if (connection->Deleted_++ == 0)
    deleteAioObject(connection->Socket);
}
