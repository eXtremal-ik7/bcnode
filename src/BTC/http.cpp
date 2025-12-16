// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "http.h"
#include "blockIndex.h"
#include "common/blockDataBase.h"
#include "common/jsonSerializer.h"
#include "common/rapidJsonHelper.h"
#include "common/utils.h"
#include "db/archive.h"
#include "BC/network.h"
#include <asyncio/socket.h>
#include <stdio.h>
#include "../loguru.hpp"

static inline bool rawcmp(Raw data, const char *operand) {
  size_t opSize = strlen(operand);
  return data.size == opSize && memcmp(data.data, operand, opSize) == 0;
}

namespace BC {
namespace Network {

std::unordered_map<std::string, HttpApiConnection::FunctionTy> HttpApiConnection::FunctionNameMap_ = {
  {"api/v1/addresses/info", fnAddressesInfo},
  {"api/v1/addresses/txs", fnAddressesTxs},
  {"api/v1/addresses/utxo", fnAddressesUtxo},
  {"api/v1/blocks/by_hash", fnBlocksByHash},
  {"api/v1/blocks/by_height", fnBlocksByHeight},
  {"api/v1/blocks/latest", fnBlocksLatest},
  {"api/v1/blocks/list", fnBlocksList},
  {"api/v1/blocks/raw", fnBlocksRaw},
  {"api/v1/blocks/txs", fnBlocksTxs},
  {"api/v1/mempool/summary", fnMempoolSummary},
  {"api/v1/mempool/txs", fnMempoolTxs},
  {"api/v1/search", fnSearch},
  {"api/v1/stats/rich_list", fnStatsRichList},
  {"api/v1/system/health", fnSystemHealth},
  {"api/v1/system/summary", fnSystemSummary},
  {"api/v1/txs/by_block_hash", fnTxsByBlockHash},
  {"api/v1/txs/by_block_height", fnTxsByBlockHeight},
  {"api/v1/txs/by_txid", fnTxsByTxid},
  {"api/v1/txs/latest", fnTxsLatest},
  {"api/v1/txs/raw", fnTxsRaw}
};

// HttpApiConnection

void BC::Network::HttpApiConnection::socketDestructorCb(aioObjectRoot*, void *arg)
{
  delete static_cast<BC::Network::HttpApiConnection*>(arg);
}

BC::Network::HttpApiConnection::HttpApiConnection(BlockInMemoryIndex &blockIndex, BC::Common::ChainParams &chainParams, BlockDatabase &blockDb, BC::Network::Node &node, BC::DB::Archive &storage, HttpApiNode *httpNode, HostAddress address, aioObject *socket) :
  BlockIndex_(blockIndex), ChainParams_(chainParams), BlockDb_(&blockDb), Node_(&node), Storage_(&storage), HttpNode_(httpNode), Socket_(socket), Address(address)
{
  httpRequestParserInit(&ParserState);
  objectSetDestructorCb(aioObjectHandle(Socket_), socketDestructorCb, this);
}

void BC::Network::HttpApiConnection::start()
{
  aioRead(Socket_, buffer, sizeof(buffer), afNone, 0, readCb, this);
}

int BC::Network::HttpApiConnection::onParse(HttpRequestComponent *component)
{
  switch (component->type) {
    case httpRequestDtMethod : {
      Context.Method = component->method;
      break;
    }
    case httpRequestDtUriPathElement : {
      if (!Context.Path.empty())
        Context.Path.push_back('/');
      Context.Path.append(component->data.data, component->data.data + component->data.size);
      break;
    }
    case httpRequestDtData : {
      Context.Request.append(component->data.data, component->data.data + component->data.size);
      break;
    }
    case httpRequestDtDataLast : {
      // Response here
      // Append last request data
      Context.Request.append(component->data.data, component->data.data + component->data.size);

      // Search function
      // All functions uses POST method
      auto It = FunctionNameMap_.find(Context.Path);
      if (It == FunctionNameMap_.end() || Context.Method != hmPost) {
        reply404();
        return 0;
      }
      Context.Function = It->second;

      // Parse request (must be object)
      rapidjson::Document document;
      document.Parse(!Context.Request.empty() ? Context.Request.c_str() : "{}");
      if (document.HasParseError() || !document.IsObject()) {
        replyWithError("INVALID_JSON", "", "", "");
        return 1;
      }

      switch (Context.Function) {
        case fnAddressesInfo: onAddressesInfo(document); break;
        case fnAddressesTxs : onAddressesTxs(document); break;
        case fnAddressesUtxo : onAddressesUtxo(document); break;
        case fnBlocksByHash : onBlocksByHash(document); break;
        case fnBlocksByHeight : onBlocksByHeight(document); break;
        case fnBlocksLatest : onBlocksLatest(document); break;
        case fnBlocksList : onBlocksList(document); break;
        case fnBlocksRaw : onBlocksRaw(document); break;
        case fnBlocksTxs : onBlocksTxs(document); break;
        case fnMempoolSummary : onMempoolSummary(document); break;
        case fnMempoolTxs : onMempoolTxs(document); break;
        case fnSearch : onSearch(document); break;
        case fnStatsRichList : onStatsRichList(document); break;
        case fnSystemHealth : onSystemHealth(document); break;
        case fnSystemSummary : onSystemSummary(document); break;
        case fnTxsByBlockHash : onTxsByBlockHash(document); break;
        case fnTxsByBlockHeight : onTxsByBlockHeight(document); break;
        case fnTxsByTxid : onTxsByTxid(document); break;
        case fnTxsLatest : onTxsLatest(document); break;
        case fnTxsRaw : onTxsRaw(document); break;
        default: reply404(); return 1;
      }

      break;
    }
    default :
      break;
  }

  return 1;
}

void BC::Network::HttpApiConnection::onAddressesInfo(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onAddressesTxs(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onAddressesUtxo(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onBlocksByHash(rapidjson::Document &request)
{
  bool isValid = true;
  std::string errorField;
  BC::Proto::BlockHashTy hash;
  jsonParseBaseBlob(request, "block_hash", hash, &isValid, errorField);
  if (!isValid) {
    replyWithError("REQUEST_FORMAT_ERROR", "", errorField, "");
    return;
  }

  // Search block in index
  BC::Common::BlockIndex *index = BlockIndex_.indexByHash(hash);
  if (!index) {
    replyWithError("BLOCK_NOT_FOUND", "", "" ,"");
    return;
  }

  auto object = objectByIndex(index, *BlockDb_);
  if (!object.get()) {
    replyWithError("DATABASE_CORRUPTED", "", "" ,"");
    return;
  }

  // Serialize block
  replyBlock(index, object.get(), hash);
}

void BC::Network::HttpApiConnection::onBlocksByHeight(rapidjson::Document &request)
{
  bool isValid = true;
  std::string errorField;
  uint64_t height;
  jsonParseUInt64(request, "block_height", &height, &isValid, errorField);
  if (!isValid) {
    replyWithError("REQUEST_FORMAT_ERROR", "", errorField, "");
    return;
  }

  // Search block in index
  BC::Common::BlockIndex *index = BlockIndex_.indexByHeight(height);
  if (!index) {
    replyWithError("BLOCK_NOT_FOUND", "", "" ,"");
    return;
  }

  auto object = objectByIndex(index, *BlockDb_);
  if (!object.get()) {
    replyWithError("DATABASE_CORRUPTED", "", "" ,"");
    return;
  }

  // Serialize block
  replyBlock(index, object.get(), index->Header.GetHash());
}

void BC::Network::HttpApiConnection::onBlocksLatest(rapidjson::Document&)
{
  BC::Common::BlockIndex *index = BlockIndex_.best();
  if (!index) {
    replyWithError("BLOCK_NOT_FOUND", "", "" ,"");
    return;
  }

  auto object = objectByIndex(index, *BlockDb_);
  if (!object.get()) {
    replyWithError("DATABASE_CORRUPTED", "", "" ,"");
    return;
  }

  // Serialize block
  replyBlock(index, object.get(), index->Header.GetHash());
}

void BC::Network::HttpApiConnection::onBlocksList(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onBlocksRaw(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onBlocksTxs(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onMempoolSummary(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onMempoolTxs(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onSearch(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onStatsRichList(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onSystemHealth(rapidjson::Document&)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object object(stream);
    object.addString("status", "ok");
    // TODO: get real version
    object.addString("version", "0.1");
    object.addField("node");
    {
      JSON::Object node(stream);
      node.addBoolean("connected", Node_->PeerCount() > 0);
      node.addInt("best_block_height", BlockIndex_.best()->Height);
      node.addString("best_block_hash", BlockIndex_.best()->Header.GetHash().ToString());
    }
    object.addInt("time", time(nullptr));
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::onSystemSummary(rapidjson::Document&)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object object(stream);
    object.addString("coin", BC::Configuration::ProjectName);
    object.addString("symbol", BC::Configuration::TickerName);
    object.addNull("chain");
    object.addInt("best_block_height", BlockIndex_.best()->Height);
    object.addString("best_block_hash", BlockIndex_.best()->Header.GetHash().ToString());
    object.addNull("difficulty");
    object.addNull("hashrate");
    object.addNull("hashrate_unit");
    object.addNull("price_btc");
    object.addNull("price_usd");
    object.addNull("mempool_tx_count");
    object.addNull("mempool_size_bytes");
    object.addNull("circulating_supply");
    object.addNull("addresses_total");
    object.addNull("txs_total");
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::onTxsByBlockHash(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onTxsByBlockHeight(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onTxsByTxid(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onTxsLatest(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onTxsRaw(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onRead(AsyncOpStatus status, size_t)
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

      aioRead(Socket_, buffer+oldDataSize, sizeof(buffer)-oldDataSize, afNone, 0, readCb, this);
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

void BC::Network::HttpApiConnection::onWrite()
{
  // TODO: check keep alive
  socketShutdown(aioObjectSocket(this->Socket_), SOCKET_SHUTDOWN_READWRITE);
  aioRead(Socket_, buffer, sizeof(buffer), afNone, 0, readCb, this);
}

void BC::Network::HttpApiConnection::reply200(xmstream &stream)
{
  const char reply200[] = "HTTP/1.1 200 OK\r\nServer: bcnode\r\nTransfer-Encoding: chunked\r\n\r\n";
  stream.write(reply200, sizeof(reply200)-1);
}

void BC::Network::HttpApiConnection::reply404()
{
  const char reply404[] = "HTTP/1.1 404 Not Found\r\nServer: bcnode\r\nTransfer-Encoding: chunked\r\n\r\n";
  const char html[] = "<html><head><title>Not Found</title></head><body><h1>404 Not Found</h1></body></html>";

  char buffer[4096];
  xmstream stream(buffer, sizeof(buffer));
  stream.write(reply404, sizeof(reply404)-1);

  size_t offset = startChunk(stream);
  stream.write(html);
  finishChunk(stream, offset);

  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::replyWithError(const std::string &code,
                                                    const std::string &message,
                                                    const std::string &field,
                                                    const std::string &reason)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object object(stream);
    object.addField("error");
    {
      JSON::Object errorObject(stream);
      errorObject.addString("code", code);
      errorObject.addString("message", message);
      errorObject.addField("details");
      {
        JSON::Object detailsObject(stream);
        detailsObject.addString("field", field);
        detailsObject.addString("reason", reason);
      }
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::replyBlock(const BC::Common::BlockIndex *index,
                                                const BC::Common::CIndexCacheObject *object,
                                                const BC::Proto::BlockHashTy &hash)
{
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    uint32_t bits = xhtobe(index->Header.nBits);

    JSON::Object reply(stream);
    reply.addInt("height", index->Height);
    reply.addString("hash", hash.GetHex());
    if (index->Prev)
      reply.addString("previous_hash", index->Prev->Header.GetHash().GetHex());
    else
      reply.addNull("previous_hash");

    if (index->Next)
      reply.addString("next_hash", index->Next->Header.GetHash().GetHex());
    else
      reply.addNull("next_hash");

    reply.addInt("timestamp", index->Header.nTime);
    reply.addString("merkle_root", index->Header.hashMerkleRoot.GetHex());
    reply.addInt("version", index->Header.nVersion);
    reply.addString("bits", bin2hexLowerCase(&bits, sizeof(bits)));
    reply.addInt("nonce", index->Header.nNonce);
    reply.addInt("size_bytes", index->SerializedBlockSize);
    reply.addNull("weight");
    reply.addInt("tx_count", object->block()->vtx.size());
    reply.addNull("difficulty");
    // TODO: best block has 0 or 1 confirmations ?
    reply.addInt("confirmations", BlockIndex_.best()->Height - index->Height);

    int64_t reward = 0;
    BC::Proto::Transaction &coinbase = object->block()->vtx[0];
    for (const auto &txOut : coinbase.txOut)
      reward += txOut.value;

    reply.addString("reward", FormatMoney(reward, BC::Configuration::RationalPartSize));
    reply.addNull("fees_total");
    reply.addBoolean("is_orphan", index->OnChain);
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

size_t BC::Network::HttpApiConnection::startChunk(xmstream &stream)
{
  size_t offset = stream.offsetOf();
  stream.write("00000000\r\n", 10);
  return offset;
}

void BC::Network::HttpApiConnection::finishChunk(xmstream &stream, size_t offset)
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
    static_cast<HttpApiNode*>(arg)->onAccept(address, newSocketIo(aioGetBase(object), socketFd));
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

void BC::Network::HttpApiNode::onAccept(HostAddress address, aioObject *socket)
{
  HttpApiConnection *connection = new HttpApiConnection(*BlockIndex_, *ChainParams_, *BlockDb_, *Node_, *Storage_, this, address, socket);
  connection->start();
}

void BC::Network::HttpApiNode::removeConnection(HttpApiConnection *connection)
{
  if (connection->Deleted_++ == 0)
    deleteAioObject(connection->Socket_);
}

}
}
