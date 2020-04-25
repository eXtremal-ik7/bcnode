// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "http.h"
#include "blockIndex.h"
#include "common/blockDataBase.h"
#include "common/merkleTree.h"
#include "db/archive.h"
#include "BC/network.h"
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

BC::Network::HttpApiConnection::HttpApiConnection(BlockInMemoryIndex &blockIndex, BC::Common::ChainParams &chainParams, BlockDatabase &blockDb, BC::Network::Node &node, BC::DB::Archive &storage, HttpApiNode *httpNode, HostAddress address, aioObject *socket) :
  BlockIndex_(blockIndex), ChainParams_(chainParams), BlockDb_(&blockDb), Node_(&node), Storage_(&storage), HttpNode_(httpNode), Socket(socket), Address(address)
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
  static constexpr char tx[] = "tx";
  static constexpr char getBalance[] = "getBalance";
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
    } else if (rawcmp<tx>(component->data)) {
      requiredMethod = hmGet;
      connection->RPCContext.function = fnTx;
    } else if (rawcmp<getBalance>(component->data)) {
      requiredMethod = hmGet;
      connection->RPCContext.function = fnGetBalance;
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

    case fnTx : {
      if (connection->RPCContext.argumentsNum == 0 && component->type == httpRequestDtUriPathElement) {
        std::string hash(component->data.data, component->data.data + component->data.size);
        connection->RPCContext.hash.SetHex(hash);
        connection->RPCContext.argumentsNum = 1;
      }

      if (component->type == httpRequestDtDataLast) {
        if (connection->RPCContext.argumentsNum == 1) {
          connection->OnTx();
        } else {
          connection->Reply404();
          return 0;
        }
      }

      break;
    }

    case fnGetBalance : {
      if (connection->RPCContext.argumentsNum == 0 && component->type == httpRequestDtUriPathElement) {
        connection->RPCContext.address.assign(component->data.data, component->data.data + component->data.size);
        connection->RPCContext.argumentsNum = 1;
      }

      if (component->type == httpRequestDtDataLast) {
        if (connection->RPCContext.argumentsNum == 1) {
          connection->OnGetBalance();
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
    HttpNode_->removeConnection(this);
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
      HttpNode_->removeConnection(this);
      break;
    }

    case ParserResultCancelled : {
      HttpNode_->removeConnection(this);
      break;
    }
  }
}

void BC::Network::HttpApiConnection::OnWrite()
{
  // TODO: check keep alive
  HttpNode_->removeConnection(this);
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
  BC::Common::BlockIndex *index = nullptr;
  BC::Proto::Block block;
  auto handler = [&block](void *data, size_t size) {
    xmstream source(data, size);
    BC::unserialize(source, block);
  };

  {
    BlockSearcher searcher(*BlockDb_, handler, [this](){ postQuitOperation(aioGetBase(Socket)); });
    index = searcher.add(BlockIndex_, RPCContext.hash);
    if (!index) {
      Reply404();
      return;
    }
  }

  xmstream stream;
  Build200(stream);
  size_t offset = StartChunk(stream);
  serializeJson(stream, *index, block);
  FinishChunk(stream, offset);
  aioWrite(Socket, stream.data(), stream.sizeOf(), afNone, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::OnBlockByHeight()
{
  auto It = BlockIndex_.blockHeightIndex().find(RPCContext.height);
  if (It == BlockIndex_.blockHeightIndex().end()) {
    Reply404();
    return;
  }

  BC::Proto::Block block;
  auto handler = [&block](void *data, size_t size) {
    xmstream source(data, size);
    BC::unserialize(source, block);
  };

  {
    BlockSearcher searcher(*BlockDb_, handler, [this](){ postQuitOperation(aioGetBase(Socket)); });
    if (!searcher.add(It->second)) {
      Reply404();
      return;
    }
  }

  xmstream stream;
  Build200(stream);
  size_t offset = StartChunk(stream);
  serializeJson(stream, *It->second, block);
  FinishChunk(stream, offset);
  aioWrite(Socket, stream.data(), stream.sizeOf(), afNone, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::OnTx()
{
  BC::DB::TxDb::QueryResult result;
  if (Storage_->txdb().enabled() && Storage_->txdb().find(RPCContext.hash, BlockIndex_, *BlockDb_, result)) {
    xmstream stream;
    Build200(stream);
    size_t offset = StartChunk(stream);

    stream.write('{');
    serializeJson(stream, "block", result.Block); stream.write(',');
    serializeJson(stream, "tx", result.Tx);
    stream.write('}');

    FinishChunk(stream, offset);
    aioWrite(Socket, stream.data(), stream.sizeOf(), afNone, 0, writeCb, this);
  } else {
    if (!result.DataCorrupted)
      Reply404();
    else
      postQuitOperation(aioGetBase(Socket));
  }
}

void BC::Network::HttpApiConnection::OnGetBalance()
{
  BC::Proto::AddressTy address;
  if (Storage_->balancedb().enabled() && decodeHumanReadableAddress(RPCContext.address, ChainParams_.PublicKeyPrefix, address)) {
    int64_t balance;
    xmstream stream;
    Build200(stream);
    size_t offset = StartChunk(stream);
    stream.write('{');
    if (Storage_->balancedb().find(address, &balance)) {
      serializeJson(stream, "found", true); stream.write(",");
      serializeJson(stream, "balance", balance);
    } else {
      serializeJson(stream, "found", false);
    }

    stream.write('}');
    FinishChunk(stream, offset);
    aioWrite(Socket, stream.data(), stream.sizeOf(), afNone, 0, writeCb, this);
  } else {
    Reply404();
  }
}

void BC::Network::HttpApiConnection::OnPeerInfo()
{
  xmstream stream;
  Build200(stream);
  size_t offset = StartChunk(stream);
  bool firstPeer = true;
  stream.write('[');
  Node_->enumeratePeers([&stream, &firstPeer](Peer *peer) {
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

bool BC::Network::HttpApiNode::init(BlockInMemoryIndex *blockIndex, BC::Common::ChainParams *chainParams, BlockDatabase *blockDb, BC::Network::Node *node, BC::DB::Archive &storage, asyncBase *mainBase, HostAddress localAddress)
{
  BlockIndex_ = blockIndex;
  ChainParams_ = chainParams;
  BlockDb_ = blockDb;
  Node_ = node;
  Storage_ = &storage;
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

  ServerSocket = newSocketIo(mainBase, socketFd);
  aioAccept(ServerSocket, 0, acceptCb, this);
  LOG_F(INFO, "HTTP Api server started at %s", addressAsString);
  return true;
}

void BC::Network::HttpApiNode::OnAccept(HostAddress address, aioObject *socket)
{
  HttpApiConnection *connection = new HttpApiConnection(*BlockIndex_, *ChainParams_, *BlockDb_, *Node_, *Storage_, this, address, socket);
  connection->start();
}

void BC::Network::HttpApiNode::removeConnection(HttpApiConnection *connection)
{
  if (connection->Deleted_++ == 0)
    deleteAioObject(connection->Socket);
}
