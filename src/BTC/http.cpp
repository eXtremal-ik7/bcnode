// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "http.h"
#include "blockIndex.h"
#include "common/blockDataBase.h"
#include "common/jsonSerializer.h"
#include "common/rapidJsonHelper.h"
#include "common/smallStream.h"
#include "common/utils.h"
#include "db/archive.h"
#include "BC/network.h"
#include <asyncio/socket.h>
#include <stdio.h>
#include <type_traits>
#include "../loguru.hpp"

// The block nonce is a 32 bit integer in most coins, but a 256 bit blob in ZEC
template<typename T>
static inline void addNonce(JSON::Object &object, const T &nonce)
{
  if constexpr (std::is_integral_v<T>)
    object.addInt("nonce", nonce);
  else
    object.addString("nonce", nonce.getHexLE());
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

void BC::Network::HttpApiConnection::onAddressesInfo(rapidjson::Document &request)
{
  bool isValid = true;
  std::string errorField;
  std::string address;
  jsonParseString(request, "address", address, &isValid, errorField);
  if (!isValid) {
    replyWithError("REQUEST_FORMAT_ERROR", "", errorField, "");
    return;
  }

  BC::Script::CAddress addressHash;
  if (!BC::Script::addressFromString(address, ChainParams_.PublicKeyPrefix, ChainParams_.ScriptPrefix, ChainParams_.Bech32Prefix, addressHash)) {
    replyWithError("REQUEST_FORMAT_ERROR", "", "address", address);
    return;
  }

  if (!Storage_->AddrDb_) {
    replyWithError("DATABASE_NOT_ENABLED", "", "", "");
    return;
  }

  // Unknown address is not an error: all counters are zero
  DB::CAddrValue info;
  Storage_->AddrDb_->queryAddr(addressHash, info);

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object reply(stream);
    reply.addField("address");
    {
      JSON::Object object(stream);
      object.addString("address", address);
      object.addString("balance", FormatMoney(info.Received - info.Sent, BC::Configuration::RationalPartSize));
      object.addString("total_received", FormatMoney(info.Received, BC::Configuration::RationalPartSize));
      object.addString("total_sent", FormatMoney(info.Sent, BC::Configuration::RationalPartSize));
      object.addString("total_mined", FormatMoney(info.Mined, BC::Configuration::RationalPartSize));
      object.addInt("tx_count", info.TxCount);
      object.addInt("txin_count", info.TxInCount);
      object.addInt("txout_count", info.TxOutCount);
      object.addInt("utxo_count", info.TxOutCount - info.TxInCount);
      object.addInt("mined_tx_count", info.MinedTxCount);
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::onAddressesTxs(rapidjson::Document &request)
{
  bool isValid = true;
  std::string errorField;
  std::string address;
  struct {
    uint64_t offset;
    uint64_t limit;
    std::string sort;
  } pagination;

  jsonParseString(request, "address", address, &isValid, errorField);
  jsonParseUInt64(request, "offset", &pagination.offset, 0, &isValid, errorField);
  jsonParseUInt64(request, "limit", &pagination.limit, 50, &isValid, errorField);
  jsonParseString(request, "sort", pagination.sort, "desc", &isValid, errorField);
  if (!isValid) {
    replyWithError("REQUEST_FORMAT_ERROR", "", errorField, "");
    return;
  }

  if (pagination.sort != "asc" && pagination.sort != "desc") {
    replyWithError("REQUEST_FORMAT_ERROR", "", "sort", "must be asc or desc");
    return;
  }
  if (pagination.limit > 500)
    pagination.limit = 500;

  BC::Script::CAddress addressHash;
  if (!BC::Script::addressFromString(address, ChainParams_.PublicKeyPrefix, ChainParams_.ScriptPrefix, ChainParams_.Bech32Prefix, addressHash)) {
    replyWithError("REQUEST_FORMAT_ERROR", "", "address", address);
    return;
  }

  // The transaction database is not on this path: a history element carries the
  // position of its transaction and the block file is read directly
  if (!Storage_->AddrHistoryDb_) {
    replyWithError("DATABASE_NOT_ENABLED", "", "", "");
    return;
  }

  // The address tx count arrives with any range read; a probe read of the first
  // element gets it before the page position can be computed
  uint64_t total = 0;
  {
    DB::CQueryAddrHistory probe;
    if (Storage_->AddrHistoryDb_->queryAddrHistory(addressHash, 0, 1, probe))
      total = probe.TotalTxCount;
  }

  bool isAscending = pagination.sort == "asc";
  uint64_t from = 0;
  uint64_t count = 0;
  if (pagination.offset < total) {
    if (isAscending) {
      from = pagination.offset;
      count = total - pagination.offset;
    } else {
      uint64_t end = total - pagination.offset;
      from = end > pagination.limit ? end - pagination.limit : 0;
      count = end - from;
    }
    if (count > pagination.limit)
      count = pagination.limit;
  }

  DB::CQueryAddrHistory history;
  if (count)
    Storage_->AddrHistoryDb_->queryAddrHistory(addressHash, from, count, history);

  const BC::Common::BlockIndex *best = BlockIndex_.best();

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object reply(stream);
    reply.addString("address", address);
    reply.addField("items");
    {
      JSON::Array itemsArray(stream);
      for (size_t i = 0; i < history.Items.size(); i++) {
        // Descending order shows the newest transaction first
        const auto &item = history.Items[isAscending ? i : history.Items.size() - 1 - i];

        // The element itself says which block paid the address, so a BIP30 repeat
        // needs no special case: both of its inclusions are elements of this history
        BC::Common::BlockIndex *index = BlockIndex_.indexByHeight(item.Height);
        if (!index) {
          replyWithError("BLOCK_NOT_FOUND", "", "", std::to_string(item.Height));
          return;
        }

        DB::CQueryTransactionResult queryResult;
        if (!DB::readTransactionAt(index, item.TxIndex, item.TxOffset, item.TxSize, *BlockDb_, queryResult)) {
          replyWithError("DATABASE_CORRUPTED", "", "", index->Header.GetHash().getHexLE());
          return;
        }

        itemsArray.addField();
        serializeTx(stream, queryResult.Tx, queryResult.LinkedOutputs, index, queryResult.TxNum == 0, best->Height - index->Height, &item.Aggregate);
      }
    }

    reply.addField("pagination");
    {
      JSON::Object paginationObject(stream);
      paginationObject.addInt("total", total);
      paginationObject.addInt("limit", pagination.limit);
      paginationObject.addInt("offset", pagination.offset);
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
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

  auto object = objectByIndex(index, ChainParams_, *BlockDb_);
  if (!object.get()) {
    replyWithError("DATABASE_CORRUPTED", "", "" ,"");
    return;
  }

  // Serialize block
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  {
    JSON::Object reply(stream);
    reply.addField("block");
    serializeBlock(stream, index, object.get(), hash);
  }
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
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

  auto object = objectByIndex(index, ChainParams_, *BlockDb_);
  if (!object.get()) {
    replyWithError("DATABASE_CORRUPTED", "", "" ,"");
    return;
  }

  // Serialize block
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  {
    JSON::Object reply(stream);
    reply.addField("block");
    serializeBlock(stream, index, object.get(), index->Header.GetHash());
  }
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::onBlocksLatest(rapidjson::Document&)
{
  BC::Common::BlockIndex *index = BlockIndex_.best();
  if (!index) {
    replyWithError("BLOCK_NOT_FOUND", "", "" ,"");
    return;
  }

  auto object = objectByIndex(index, ChainParams_, *BlockDb_);
  if (!object.get()) {
    replyWithError("DATABASE_CORRUPTED", "", "" ,"");
    return;
  }

  // Serialize block
  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);
  {
    JSON::Object reply(stream);
    reply.addField("block");
    serializeBlock(stream, index, object.get(), index->Header.GetHash());
  }
  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::onBlocksList(rapidjson::Document &request)
{
  bool isValid = true;
  std::string errorField;
  struct {
    uint64_t offset;
    uint64_t limit;
    std::string sort;
  } pagination;

  jsonParseUInt64(request, "offset", &pagination.offset, 0, &isValid, errorField);
  jsonParseUInt64(request, "limit", &pagination.limit, 20, &isValid, errorField);
  jsonParseString(request, "sort", pagination.sort, "desc", &isValid, errorField);
  if (!isValid) {
    replyWithError("REQUEST_FORMAT_ERROR", "", errorField, "");
    return;
  }

  if (pagination.sort != "asc" && pagination.sort != "desc") {
    replyWithError("REQUEST_FORMAT_ERROR", "", "sort", "must be asc or desc");
    return;
  }

  bool isAscending = pagination.sort == "asc";

  BC::Common::BlockIndex *best = BlockIndex_.best();
  BC::Common::BlockIndex *current;
  if (isAscending) {
    current = BlockIndex_.indexByHeight(pagination.offset);
  } else {
    if (pagination.offset == 0)
      current = best;
    else
      current = BlockIndex_.indexByHeight(best->Height - pagination.offset);
  }

  if (!current) {
    replyWithError("BLOCK_NOT_FOUND", "", "" ,"");
    return;
  }

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object reply(stream);
    reply.addField("items");

    {
      JSON::Array itemsArray(stream);
      uint64_t i = 0;
      while (current && i < pagination.limit) {
        auto object = objectByIndex(current, ChainParams_, *BlockDb_);
        if (!object.get()) {
          replyWithError("DATABASE_CORRUPTED", "", "" ,"");
          return;
        }

        itemsArray.addField();
        serializeBlock(stream, current, object.get(), current->Header.GetHash());

        i++;
        current = isAscending ? current->Next : current->Prev;
      }
    }

    reply.addField("pagination");
    {
      JSON::Object paginationObject(stream);
      paginationObject.addInt("total", best->Height + 1);
      paginationObject.addInt("limit", pagination.limit);
      paginationObject.addInt("offset", pagination.offset);
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::onBlocksRaw(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onBlocksTxs(rapidjson::Document &request)
{
  bool isValid = true;
  std::string errorField;
  std::optional<BC::Proto::BlockHashTy> hash;
  std::optional<uint64_t> height;
  struct {
    uint64_t offset;
    uint64_t limit;
  } pagination;

  jsonParseBaseBlob(request, "block_hash", hash, &isValid, errorField);
  jsonParseUInt64(request, "block_height", height, &isValid, errorField);
  jsonParseUInt64(request, "offset", &pagination.offset, 0, &isValid, errorField);
  jsonParseUInt64(request, "limit", &pagination.limit, 50, &isValid, errorField);
  if (!isValid) {
    replyWithError("REQUEST_FORMAT_ERROR", "", errorField, "");
    return;
  }

  BC::Common::BlockIndex *index = nullptr;
  if (height.has_value()) {
    index = BlockIndex_.indexByHeight(height.value());
  } else if (hash.has_value()) {
    index = BlockIndex_.indexByHash(hash.value());
  } else {
    replyWithError("REQUEST_FORMAT_ERROR", "", "", "both block_hash and block_height missing");
    return;
  }

  if (!index) {
    replyWithError("BLOCK_NOT_FOUND", "", "" ,"");
    return;
  }

  auto object = objectByIndex(index, ChainParams_, *BlockDb_);
  if (!object.get()) {
    replyWithError("DATABASE_CORRUPTED", "", "" ,"");
    return;
  }

  const BC::Common::BlockIndex *best = BlockIndex_.best();
  const BC::Proto::Block &block = *object.get()->block();
  const BC::Proto::CBlockLinkedOutputs &blockOutputs = object.get()->linkedOutputs();

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object reply(stream);
    reply.addField("block");
    {
      JSON::Object blockObject(stream);
      blockObject.addString("hash", index->Header.GetHash().getHexLE());
      blockObject.addInt("height", index->Height);
      blockObject.addInt("timestamp", index->Header.nTime);
      blockObject.addInt("tx_count", block.vtx.size());
    }
    reply.addField("items");

    {
      JSON::Array itemsArray(stream);

      for (size_t i = pagination.offset; i < block.vtx.size() && i - pagination.offset < pagination.limit; i++) {
        const BC::Proto::Transaction &tx = block.vtx[i];
        const BC::Proto::CTxLinkedOutputs &txOutputs = blockOutputs.Tx[i];
        itemsArray.addField();
        serializeTx(stream, tx, txOutputs, index, i == 0, best->Height - index->Height);
      }
    }

    reply.addField("pagination");
    {
      JSON::Object paginationObject(stream);
      paginationObject.addInt("total", block.vtx.size());
      paginationObject.addInt("limit", pagination.limit);
      paginationObject.addInt("offset", pagination.offset);
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
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

void BC::Network::HttpApiConnection::onStatsRichList(rapidjson::Document &request)
{
  bool isValid = true;
  std::string errorField;
  std::string sortBy;
  struct {
    uint64_t offset;
    uint64_t limit;
  } pagination;

  jsonParseUInt64(request, "offset", &pagination.offset, 0, &isValid, errorField);
  jsonParseUInt64(request, "limit", &pagination.limit, 100, &isValid, errorField);
  jsonParseString(request, "sort_by", sortBy, "balance", &isValid, errorField);
  if (!isValid) {
    replyWithError("REQUEST_FORMAT_ERROR", "", errorField, "");
    return;
  }

  if (pagination.limit > 500)
    pagination.limit = 500;
  if (pagination.offset + pagination.limit > 1000) {
    replyWithError("REQUEST_FORMAT_ERROR", "", "offset", "rich list depth is limited to 1000");
    return;
  }

  if (!Storage_->AddrDb_) {
    replyWithError("DATABASE_NOT_ENABLED", "", "", "");
    return;
  }

  std::vector<std::pair<BC::Script::CAddress, DB::CAddrValue>> top;
  if (!Storage_->AddrDb_->queryTop(sortBy, pagination.offset, pagination.limit, top)) {
    replyWithError("INDEX_NOT_ENABLED", "", "sort_by", sortBy);
    return;
  }

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object reply(stream);
    reply.addField("items");
    {
      JSON::Array itemsArray(stream);
      for (size_t i = 0; i < top.size(); i++) {
        DB::CAddrValue &value = top[i].second;

        std::string address58 = BC::Script::addressToString(top[i].first, ChainParams_.PublicKeyPrefix, ChainParams_.ScriptPrefix, ChainParams_.Bech32Prefix);
        if (address58.empty())
          address58 = bin2hexLowerCase(top[i].first.Data, top[i].first.payloadSize());

        itemsArray.addField();
        {
          JSON::Object itemObject(stream);
          itemObject.addInt("rank", pagination.offset + i + 1);
          itemObject.addString("address", address58);
          itemObject.addString("balance", FormatMoney(value.Received - value.Sent, BC::Configuration::RationalPartSize));
          itemObject.addString("total_received", FormatMoney(value.Received, BC::Configuration::RationalPartSize));
          itemObject.addString("total_sent", FormatMoney(value.Sent, BC::Configuration::RationalPartSize));
          itemObject.addInt("tx_count", value.TxCount);
          itemObject.addNull("percentage_of_supply");
        }
      }
    }

    reply.addField("pagination");
    {
      JSON::Object paginationObject(stream);
      paginationObject.addNull("total");
      paginationObject.addInt("limit", pagination.limit);
      paginationObject.addInt("offset", pagination.offset);
    }
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
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
      node.addString("best_block_hash", BlockIndex_.best()->Header.GetHash().getHexLE());
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
    object.addString("best_block_hash", BlockIndex_.best()->Header.GetHash().getHexLE());
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

void BC::Network::HttpApiConnection::onTxsByTxid(rapidjson::Document &request)
{
  bool isValid = true;
  std::string errorField;
  BC::Proto::TxHashTy txid;
  jsonParseBaseBlob(request, "txid", txid, &isValid, errorField);
  if (!isValid) {
    replyWithError("REQUEST_FORMAT_ERROR", "", errorField, "");
    return;
  }

  DB::CQueryTransactionResult queryResult;
  if (!Storage_->TransactionDb_->queryTransaction(txid, BlockIndex_, *BlockDb_, queryResult)) {
    replyWithError("DATABASE_NOT_ENABLED", "", "", "");
    return;
  }
  if (!queryResult.Found) {
    replyWithError("TRANSACTION_NOT_FOUND", "", "" ,"");
    return;
  }
  if (queryResult.DataCorrupted) {
    replyWithError("DATABASE_CORRUPTED", "", "" ,"");
    return;
  }

  const BC::Common::BlockIndex *best = BlockIndex_.best();
  BC::Common::BlockIndex *index = BlockIndex_.indexByHash(queryResult.Block);
  // A BIP30 repeat is stored under the block that came first, but the copy that
  // matters is the later one: its outputs are the coins that live. The bytes are
  // the same, only the place differs; both places go into the reply
  if (const BTC::Common::CBIP30Repeat *repeat = bip30Repeat(txid)) {
    if (BC::Common::BlockIndex *repeatIndex = BlockIndex_.indexByHash(repeat->Hash))
      index = repeatIndex;
  }
  if (!index) {
    replyWithError("BLOCK_NOT_FOUND", "", "", queryResult.Block.getHexLE());
    return;
  }

  xmstream stream;
  reply200(stream);
  size_t offset = startChunk(stream);

  {
    JSON::Object reply(stream);
    reply.addField("tx");
    serializeTx(stream, queryResult.Tx, queryResult.LinkedOutputs, index, queryResult.TxNum == 0, best->Height - index->Height);
  }

  finishChunk(stream, offset);
  aioWrite(Socket_, stream.data(), stream.sizeOf(), afWaitAll, 0, writeCb, this);
}

void BC::Network::HttpApiConnection::onTxsLatest(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onTxsRaw(rapidjson::Document&)
{
  replyNotImplemented();
}

void BC::Network::HttpApiConnection::onRead(AsyncOpStatus status, size_t bytesRead)
{
  if (status != aosSuccess) {
    HttpNode_->removeConnection(this);
    return;
  }

  // What the parser gets is what has arrived: the retained tail at the front of
  // the buffer plus this read. Handing it the whole capacity instead makes it
  // read past the data - a body that came in its own segment is then taken from
  // uninitialized memory
  httpRequestSetBuffer(&ParserState, buffer, oldDataSize + bytesRead);

  switch (httpRequestParse(&ParserState, nullptr, parseCb, this)) {
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
  // The read below lands at the head of the buffer, so nothing is retained in
  // front of it: the tail of the request just answered is not one
  oldDataSize = 0;
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

  SmallStream<4096> stream;
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

void BC::Network::HttpApiConnection::serializeBlock(xmstream &stream,
                                                    const BC::Common::BlockIndex *index,
                                                    const BC::Common::CIndexCacheObject *object,
                                                    const BC::Proto::BlockHashTy &hash)
{
  uint32_t bits = xhtobe(index->Header.nBits);

  JSON::Object blockObject(stream);
  blockObject.addInt("height", index->Height);
  blockObject.addString("hash", hash.getHexLE());
  if (index->Prev)
    blockObject.addString("previous_hash", index->Prev->Header.GetHash().getHexLE());
  else
    blockObject.addNull("previous_hash");

  if (index->Next)
    blockObject.addString("next_hash", index->Next->Header.GetHash().getHexLE());
  else
    blockObject.addNull("next_hash");

  blockObject.addInt("timestamp", index->Header.nTime);
  blockObject.addString("merkle_root", index->Header.hashMerkleRoot.getHexLE());
  blockObject.addInt("version", index->Header.nVersion);
  blockObject.addString("bits", bin2hexLowerCase(&bits, sizeof(bits)));
  addNonce(blockObject, index->Header.nNonce);
  blockObject.addInt("size_bytes", index->SerializedBlockSize);
  blockObject.addNull("weight");
  blockObject.addInt("tx_count", object->block()->vtx.size());
  blockObject.addNull("difficulty");
  // TODO: best block has 0 or 1 confirmations ?
  blockObject.addInt("confirmations", BlockIndex_.best()->Height - index->Height);

  int64_t reward = 0;
  BC::Proto::Transaction &coinbase = object->block()->vtx[0];
  for (const auto &txOut : coinbase.txOut)
    reward += txOut.value;

  blockObject.addString("reward", FormatMoney(reward, BC::Configuration::RationalPartSize));
  blockObject.addNull("fees_total");
  blockObject.addBoolean("is_orphan", !index->OnChain);
}

const BTC::Common::CBIP30Repeat *BC::Network::HttpApiConnection::bip30Repeat(const BC::Proto::TxHashTy &txid) const
{
  // Two entries on Bitcoin, none anywhere else
  for (const auto &repeat: ChainParams_.BIP30Repeats) {
    if (repeat.TxId == txid)
      return &repeat;
  }

  return nullptr;
}

void BC::Network::HttpApiConnection::serializeTx(xmstream &stream,
                                                 const BC::Proto::Transaction &tx,
                                                 const BC::Proto::CTxLinkedOutputs &txOutputs,
                                                 const BC::Common::BlockIndex *index,
                                                 bool isCoinbase,
                                                 uint64_t confirmations,
                                                 const BC::Proto::BalanceType *balanceAfter)
{
  JSON::Object txObject(stream);

  int64_t valueIn = 0;
  int64_t valueOut = 0;
  int64_t fee = 0;
  for (const auto &txOut: tx.txOut)
    valueOut += txOut.value;

  if (!isCoinbase) {
    for (const auto &linkedTxin: txOutputs.TxIn) {
      BC::Script::UnspentOutputInfo *outputInfo = (BC::Script::UnspentOutputInfo*)linkedTxin.data();
      valueIn += outputInfo->Value;
    }
    fee = valueIn - valueOut;
  }

  const BC::Proto::TxHashTy txid = tx.getTxId();

  // "spent by" for every output at once. Without spentdb the fields below stay
  // null: the reply keeps its shape whatever the node is configured with
  std::vector<DB::CQuerySpentResult> spent;
  if (Storage_->SpentDb_)
    Storage_->SpentDb_->querySpentOutputs(txid, static_cast<uint32_t>(tx.txOut.size()), spent);

  txObject.addString("txid", txid.getHexLE());
  txObject.addString("hash", tx.getWTxid().getHexLE());
  txObject.addString("block_hash", index->Header.GetHash().getHexLE());
  txObject.addInt("block_height", index->Height);
  txObject.addInt("timestamp", index->Header.nTime);
  txObject.addInt("size_bytes", BC::Io<BC::Proto::Transaction>::getSerializedSize(tx, true));
  txObject.addInt("version", tx.version);
  txObject.addInt("locktime", tx.lockTime);
  txObject.addInt("confirmations", confirmations);
  txObject.addString("value_in", FormatMoney(valueIn, BC::Configuration::RationalPartSize));
  txObject.addString("value_out", FormatMoney(valueOut, BC::Configuration::RationalPartSize));
  txObject.addString("fee", FormatMoney(fee, BC::Configuration::RationalPartSize));
  // Set for the addresses/txs context only: the address balance right after this tx
  if (balanceAfter)
    txObject.addString("balance_after", FormatMoney(*balanceAfter, BC::Configuration::RationalPartSize));

  // Only a BIP30 repeat has more than one place in the chain: the same transaction
  // sits in two blocks, and the coins of the earlier copy are dead - the later one
  // overwrote them with identical outputs. For everyone else block_hash is the whole
  // answer and the field is absent
  if (const BTC::Common::CBIP30Repeat *repeat = bip30Repeat(txid)) {
    txObject.addField("inclusions");
    JSON::Array inclusionsArray(stream);
    auto inclusion = [&](const BC::Proto::BlockHashTy &hash, uint32_t height, bool superseded) {
      inclusionsArray.addField();
      JSON::Object inclusionObject(stream);
      inclusionObject.addString("block_hash", hash.getHexLE());
      inclusionObject.addInt("block_height", height);
      inclusionObject.addBoolean("superseded", superseded);
    };

    // The repeat destroyed the outputs of the copy below it
    inclusion(repeat->TwinHash, repeat->TwinHeight, true);
    inclusion(repeat->Hash, repeat->Height, false);
  }

  txObject.addField("inputs");
  {
    JSON::Array inputsArray(stream);
    for (size_t i = 0; i < tx.txIn.size(); i++) {
      const BC::Proto::TxIn &txin = tx.txIn[i];
      const auto &linkedTxin = txOutputs.TxIn[i];
      std::string address58;
      BC::Script::CAddress address;
      int64_t value = 0;

      if (!isCoinbase) {
        BC::Script::UnspentOutputInfo *outputInfo = (BC::Script::UnspentOutputInfo*)linkedTxin.data();
        if (BC::Script::extractAddress(*outputInfo, address))
          address58 = BC::Script::addressToString(address, ChainParams_.PublicKeyPrefix, ChainParams_.ScriptPrefix, ChainParams_.Bech32Prefix);
        value = outputInfo->Value;
      }

      inputsArray.addField();
      {
        JSON::Object inputObject(stream);
        inputObject.addString("txid", txin.previousOutputHash.getHexLE());
        inputObject.addInt("vout_index", txin.previousOutputIndex);
        if (!address58.empty())
          inputObject.addString("address", address58);
        else
          inputObject.addNull("address");

        if (!isCoinbase) {
          inputObject.addString("value", FormatMoney(value, BC::Configuration::RationalPartSize));
        } else {
          inputObject.addNull("value");
        }

        inputObject.addBoolean("coinbase", isCoinbase);
      }
    }
  }
  txObject.addField("outputs");
  {
    JSON::Array outputsArray(stream);
    for (size_t i = 0; i < tx.txOut.size(); i++) {
      std::string address58;
      BC::Script::CAddress address;
      const BC::Proto::TxOut &txOut = tx.txOut[i];

      if (BC::Script::extractAddress(txOut, address))
        address58 = BC::Script::addressToString(address, ChainParams_.PublicKeyPrefix, ChainParams_.ScriptPrefix, ChainParams_.Bech32Prefix);

      outputsArray.addField();
      {
        JSON::Object outputObject(stream);
        outputObject.addInt("index", i);
        if (!address58.empty())
          outputObject.addString("address", address58);
        else
          outputObject.addNull("address");
        outputObject.addString("value", FormatMoney(txOut.value, BC::Configuration::RationalPartSize));
        outputObject.addString("script_pub_key", bin2hexLowerCase(txOut.pkScript.begin(), txOut.pkScript.size()));

        if (i < spent.size() && spent[i].Found) {
          outputObject.addBoolean("spent", true);
          outputObject.addString("spent_in_txid", spent[i].Value.SpentBy.getHexLE());
          outputObject.addInt("spent_in_input", spent[i].Value.InputIndex);
          outputObject.addInt("spent_at_height", spent[i].Value.Height);
        } else {
          // No spentdb: unknown, not unspent
          if (Storage_->SpentDb_)
            outputObject.addBoolean("spent", false);
          else
            outputObject.addNull("spent");
          outputObject.addNull("spent_in_txid");
          outputObject.addNull("spent_in_input");
          outputObject.addNull("spent_at_height");
        }
      }
    }
  }
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
  snprintf(hex, sizeof(hex), "%08x", static_cast<unsigned>(stream.offsetOf() - offset - 10));
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
    snprintf(addressAsString, sizeof(addressAsString), "%s:%u", inet_ntoa(a), static_cast<unsigned>(LocalAddress.port));
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
