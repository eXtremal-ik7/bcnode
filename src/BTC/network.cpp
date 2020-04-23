// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network.h"
#include "common/blockDataBase.h"
#include "db/storage.h"
#include <asyncio/socket.h>

namespace BC {
namespace Network {

std::unordered_map<std::string, Peer::MessageTy> Peer::MessageTypeMap_ = {
  {"addr", Peer::MessageTy::addr},
  {"block", Peer::MessageTy::block},
  {"getaddr", Peer::MessageTy::getaddr},
  {"getblocks", Peer::MessageTy::getblocks},
  {"getdata", Peer::MessageTy::getdata},
  {"getheaders", Peer::MessageTy::getheaders},
  {"headers", Peer::MessageTy::headers},
  {"inv", Peer::MessageTy::inv},
  {"ping", Peer::MessageTy::ping},
  {"pong", Peer::MessageTy::pong},
  {"reject", Peer::MessageTy::reject},
  {"verack", Peer::MessageTy::verack},
  {"version", Peer::MessageTy::version}
};

constexpr const char *Peer::messageName(MessageTy type)
{
  constexpr const char *names[] = {
    "unknown",
    "addr",
    "block",
    "getaddr",
    "getblocks",
    "getdata",
    "getheaders",
    "headers",
    "inv",
    "ping",
    "pong",
    "reject",
    "verack",
    "version"
  };

  return names[static_cast<unsigned>(type)];
}

std::atomic<unsigned> Peer::ActiveThreads_= 0;

tbb::concurrent_queue<Peer::InternalMessage*> Peer::MessageQueue_;

void Peer::socketDestructorCb(aioObjectRoot*, void *arg) { delete static_cast<Peer*>(arg); }
void Peer::eventDestructorCb(aioUserEvent*, void *arg) { objectDecrementReference(btcSocketHandle(static_cast<Peer*>(arg)->Socket), 1); }
void Peer::blockDownloadCb(aioUserEvent*, void *arg) { static_cast<Peer*>(arg)->ParentNode->Sync(static_cast<Peer*>(arg)); }
void Peer::pingEventCb(aioUserEvent*, void *arg) { static_cast<Peer*>(arg)->ping(); }


Peer::Peer(BlockInMemoryIndex &blockIndex,
           BC::Common::ChainParams &chainParams,
           BC::DB::Storage &storage,
           Node *node, asyncBase *base,
           unsigned threadsNum,
           unsigned workerTimeThreadsNum,
           HostAddress address,
           aioObject *object,
           const char *name) :
  BlockIndex_(blockIndex),
  ChainParams_(chainParams),
  Storage_(storage),
  Address(address),
  Name(name),
  ParentNode(node),
  Base(base),
  Socket(nullptr),
  ThreadsNum_(threadsNum),
  WorkerThreadsNum_(workerTimeThreadsNum)
{
  SendStreams.reset(new xmstream[ThreadsNum_]);

  unsigned numberOfMessages = static_cast<unsigned>(MessageTy::last);
  ReceivedBytes_.reset(new uint64_t[ThreadsNum_]);
  SentBytes_.reset(new uint64_t[ThreadsNum_]);
  ReceivedCommands_.reset(new uint64_t[ThreadsNum_ * numberOfMessages]);
  SentCommands_.reset(new uint64_t[ThreadsNum_ * numberOfMessages]);
  memset(ReceivedBytes_.get(), 0, sizeof(uint64_t)*ThreadsNum_);
  memset(SentBytes_.get(), 0, sizeof(uint64_t)*ThreadsNum_);
  memset(ReceivedCommands_.get(), 0, sizeof(uint64_t)*ThreadsNum_*numberOfMessages);
  memset(SentCommands_.get(), 0, sizeof(uint64_t)*ThreadsNum_*numberOfMessages);

  aioObject *peerSocket = nullptr;
  if (!object) {
    socketTy socketFd = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
    HostAddress localAddress;
    localAddress.family = AF_INET;
    localAddress.ipv4 = INADDR_ANY;
    localAddress.port = 0;
    if (socketBind(socketFd, &localAddress) != 0) {
      socketClose(socketFd);
      return;
    }

    peerSocket = newSocketIo(Base, socketFd);
    Incoming = false;
  } else {
    peerSocket = object;
    Incoming = true;
  }

  Socket = btcSocketNew(Base, peerSocket);
  blockDownloadEvent = newUserEvent(Base, 0, blockDownloadCb, this);
  pingEvent = newUserEvent(Base, 0, pingEventCb, this);

  btcSocketSetMagic(Socket, ChainParams_.magic);
  objectSetDestructorCb(btcSocketHandle(Socket), socketDestructorCb, this);
  objectIncrementReference(btcSocketHandle(Socket), 2);
  eventSetDestructorCb(blockDownloadEvent, eventDestructorCb, this);
  eventSetDestructorCb(pingEvent, eventDestructorCb, this);
}

Peer::~Peer()
{
  std::vector<BC::Proto::BlockHashTy> hashes;
  uint32_t sub = 0x10;
  if ((blockDownloading.fetch_add(sub) & 0xF) == 2) {
    for (auto &hash: ScheduledToDownload_)
      hashes.push_back(hash);
  }

  blockDownloading.fetch_sub(sub);
  if (!hashes.empty())
    ParentNode->Sync(hashes);

  LOG_F(WARNING, "Peer %s is disconnected", Name.c_str());
}

void Peer::start()
{
  if (Incoming)
    onConnect(aosSuccess);
  else
    aioConnect(btcGetPlainSocket(Socket), &Address, 5*1000000, onConnectCb, this);
}

void Peer::sendMessage(MessageTy type, void *data, size_t size)
{
  aioBtcSend(Socket, messageName(type), data, size, afNone, 0, nullptr, nullptr);
  unsigned numberOfMessages = static_cast<unsigned>(MessageTy::last);
  unsigned commandId = static_cast<unsigned>(type);
  SentCommands_[GetWorkerThreadId()*numberOfMessages + commandId]++;
  SentBytes_[GetWorkerThreadId()] += size + (4+12+4+4);
}

void Peer::onConnect(AsyncOpStatus status)
{
  if (status != aosSuccess) {
    ParentNode->RemovePeer(this);
    return;
  }

  // Send version message
  BC::Proto::MessageVersion msg;
  msg.version = 70002;
  msg.services = 1; // NODE
  msg.timestamp = static_cast<uint64_t>(time(nullptr));
  msg.addr_recv.services = 0;
  msg.addr_recv.setIpv4(0);
  msg.addr_recv.port = 0;
  msg.addr_from.services = 0; // NODE
  msg.addr_from.reset();
  msg.addr_from.port = 0;
  msg.nonce = ParentNode->localHostNonce();
  msg.user_agent = BC::Common::UserAgent;
  msg.start_height = BlockIndex_.best()->Height;
  msg.relay = 1;

  xmstream &stream = LocalStream();
  BC::serialize(stream, msg);
  sendMessage(MessageTy::version, stream.data(), stream.sizeOf());
  aioBtcRecv(Socket, Command, ReceiveStream, Limit, afNone, ConnectTimeout, onMessageCb, this);
}

void Peer::onMessage(AsyncOpStatus status)
{
  if (status != aosSuccess) {
    ParentNode->RemovePeer(this);
    return;
  }

  bool result = true;
  bool heavyOperation = false;
  bool heavyOperationStarted = false;
  unsigned numberOfMessages = static_cast<unsigned>(MessageTy::last);
  MessageTy command = MessageTypeMap_[Command];

  // Update receive statistics
  unsigned commandId = static_cast<unsigned>(command);
  ReceivedCommands_[GetWorkerThreadId()*numberOfMessages + commandId]++;
  ReceivedBytes_[GetWorkerThreadId()] += ReceiveStream.sizeOf() + (4+12+4+4);

  switch (command) {
    // "real time" operations
    case MessageTy::addr :
      result = callHandler<BC::Proto::MessageAddr>("addr", &Peer::onAddr);
      break;
    case MessageTy::getaddr :
      result = callHandlerEmpty(&Peer::onGetAddr);
      break;
    case MessageTy::getheaders :
      result = callHandler<BC::Proto::MessageGetHeaders>("getheaders", &Peer::onGetHeaders);
      break;
    case MessageTy::inv :
      result = callHandler<BC::Proto::MessageInv>("inv", &Peer::onInv);
      break;
    case MessageTy::ping :
      result = callHandler<BC::Proto::MessagePing>("ping", &Peer::onPing);
      break;
    case MessageTy::pong :
      result = callHandler<BC::Proto::MessagePong>("pong", &Peer::onPong);
      break;
    case MessageTy::reject :
      result = callHandler<BC::Proto::MessageReject>("reject", &Peer::onReject);
      break;
    case MessageTy::verack :
      result = callHandlerEmpty(&Peer::onVerAck);
      break;
    case MessageTy::version :
      result = callHandler<BC::Proto::MessageVersion>("version", &Peer::onVersion);
      break;

    // CPU bound operation
    case MessageTy::getblocks :
      result = startHeavyOperation(&heavyOperation, &heavyOperationStarted) ?
        callHandler<BC::Proto::MessageGetBlocks>("getblocks", &Peer::onGetBlocks) :
        pushInternalMessage<BC::Proto::MessageGetBlocks>("getblocks", MessageTy::getblocks);
      break;
    case MessageTy::getdata :
      result = startHeavyOperation(&heavyOperation, &heavyOperationStarted) ?
        callHandler<BC::Proto::MessageGetData>("getdata", &Peer::onGetData) :
        pushInternalMessage<BC::Proto::MessageGetData>("getdata", MessageTy::getdata);
      break;

    // Special handlers
    case MessageTy::block : {
      // Take serialized block from receive stream
      size_t msize = ReceiveStream.capacity();
      size_t size = ReceiveStream.sizeOf();
      void *data = nullptr;
      if (msize > size*2) {
        // ReceiveStream have too big buffer, allocate memory for serialized block data and call memcpy
        msize = size;
        data = operator new(size);
        memcpy(data, ReceiveStream.data(), size);
      } else {
        // Capture ReceiveStream buffer
        data = ReceiveStream.capture();
      }

      // (async) Receive next message
      aioBtcRecv(Socket, Command, ReceiveStream, Limit, afNone, 0, onMessageCb, this);

      if (startHeavyOperation(&heavyOperation, &heavyOperationStarted)) {
        // Unpacking block
        xmstream serialized(data, size);
        xmstream unpacked(size*2);
        result = BC::unpack<BC::Proto::Block>(serialized, unpacked);
        if (result) {
          size_t unpackedMemorySize = unpacked.capacity();
          intrusive_ptr<SerializedDataObject> object = Storage_.cache().add(data, size, msize, unpacked.capture(), unpackedMemorySize);
          onBlock(object.get(), std::chrono::steady_clock::now());
        } else {
          LOG_F(ERROR, "Peer %s: can't unserialize block", Name.c_str());
          operator delete(data);
        }
      } else {
        InternalMessage *internalMsg = static_cast<InternalMessage*>(operator new(sizeof(InternalMessage)));
        new (internalMsg) InternalMessage(this);
        internalMsg->Data = data;
        internalMsg->Size = size;
        internalMsg->MemorySize = msize;
        internalMsg->Type = MessageTy::block;
        internalMsg->Time = std::chrono::steady_clock::now();
        MessageQueue_.push(internalMsg);
      }

      break;
    }

    case MessageTy::headers : {
      xmstream unpacked(ReceiveStream.sizeOf()*2);
      if (startHeavyOperation(&heavyOperation, &heavyOperationStarted)) {
        result = BC::unpack<BC::Proto::MessageHeaders>(ReceiveStream, unpacked);
        aioBtcRecv(Socket, Command, ReceiveStream, Limit, afNone, 0, onMessageCb, this);

        if (result) {
          onHeaders(*unpacked.data<BC::Proto::MessageHeaders>());
        } else {
          LOG_F(ERROR, "Peer %s: can't unserialize headers message", Name.c_str());
        }
      } else {
        unpacked.reserve<InternalMessage>(1);
        result = BC::unpack<BC::Proto::MessageHeaders>(ReceiveStream, unpacked);
        aioBtcRecv(Socket, Command, ReceiveStream, Limit, afNone, 0, onMessageCb, this);
        if (result) {
          InternalMessage *internalMsg = new (unpacked.data<InternalMessage>()) InternalMessage(this);
          internalMsg->Type = MessageTy::headers;
          internalMsg->Data = unpacked.data<uint8_t>() + sizeof(InternalMessage);
          MessageQueue_.push(static_cast<InternalMessage*>(unpacked.capture()));
        } else {
          LOG_F(ERROR, "Peer %s: can't unserialize headers message", Name.c_str());
        }
      }

      break;
    }

    // Unknown command
    default:
      LOG_F(INFO, "Ignore command %s from %s", Command, Name.c_str());
      aioBtcRecv(Socket, Command, ReceiveStream, Limit, afNone, 0, onMessageCb, this);
      break;
  }

  finishHeavyOperation(heavyOperation, heavyOperationStarted);

  if (!result)
    ParentNode->RemovePeer(this);
}

void Peer::processMessageQueue()
{
  InternalMessage *internalMsg;
  while (MessageQueue_.try_pop(internalMsg)) {
    Peer *peer = internalMsg->Source.get();
    void *data = reinterpret_cast<uint8_t*>(internalMsg) + sizeof(InternalMessage);

    switch (internalMsg->Type) {
      // CPU bound operation
      case MessageTy::getblocks :
        peer->onGetBlocks(*static_cast<BC::Proto::MessageGetBlocks*>(data));
        break;
      case MessageTy::getdata :
        peer->onGetData(*static_cast<BC::Proto::MessageGetData*>(data));
        break;

      // Special handlers
      case MessageTy::block : {
        // Unpacking block
        xmstream serialized(internalMsg->Data, internalMsg->Size);
        xmstream unpacked(internalMsg->Size*2);
        bool result = BC::unpack<BC::Proto::Block>(serialized, unpacked);
        if (result) {
          size_t unpackedMemorySize = unpacked.capacity();
          intrusive_ptr<SerializedDataObject> object = peer->Storage_.cache().add(internalMsg->Data, internalMsg->Size, internalMsg->MemorySize, unpacked.capture(), unpackedMemorySize);
          peer->onBlock(object.get(), internalMsg->Time);
        } else {
          LOG_F(ERROR, "Peer %s: can't unserialize block", peer->Name.c_str());
          peer->ParentNode->RemovePeer(peer);
          operator delete(internalMsg->Data);
        }

        break;
      }
      case MessageTy::headers :
        peer->onHeaders(*static_cast<BC::Proto::MessageHeaders*>(data));
        break;

      default :
        LOG_F(ERROR, "Unknown internal message type %u", static_cast<unsigned>(internalMsg->Type));
        break;
    }

    internalMsg->~InternalMessage();
    operator delete(internalMsg);
  }
}

void Peer::onVersion(BC::Proto::MessageVersion &version)
{
  if (version.nonce == ParentNode->localHostNonce()) {
    LOG_F(INFO, "Connect to ourself detected");
    ParentNode->RemovePeer(this);
    return;
  }

  StartHeight = version.start_height;
  ProtocolVersion = version.version;
  Services = version.services;
  UserAgent_ = version.user_agent;

  VersionReceived = true;
  if (!IsConnected && (VersionReceived & VerackReceived)) {
    IsConnected = true;
    ParentNode->OnPeerConnected(this);
  }

  LOG_F(INFO, "Received version message from %s; user agent: %s, protocol: %u, start height: %u", Name.c_str(), version.user_agent.c_str(), version.version, StartHeight);
  sendMessage(MessageTy::verack, nullptr, 0);
}

void Peer::onVerAck()
{
  VerackReceived = true;
  if (!IsConnected && (VersionReceived & VerackReceived)) {
    IsConnected = true;
    ParentNode->OnPeerConnected(this);
  }
}

void Peer::onGetAddr()
{
  ParentNode->OnGetAddr(this);
}

void Peer::onAddr(BC::Proto::MessageAddr &addr)
{
  LOG_F(WARNING, "Ignore addr message with %zu elements", addr.addr_list.size());
}

void Peer::onHeaders(BC::Proto::MessageHeaders &headers)
{
  if (!headerChainHeight.IsNull() && !headers.headers.empty() && headers.headers[0].header.hashPrevBlock != headerChainHeight) {
    LOG_F(INFO, "%s: invalid header sequence received", Name.c_str());
    return;
  } else {
    auto now = std::chrono::steady_clock::now();
    auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(now - HeaderDownloadingStartTime_).count();
    HeaderDownloadingStartTime_ = TimeUnknown;
    ParentNode->Sync(this, headers.headers, static_cast<unsigned>(interval));
  }
}

void Peer::onGetHeaders(BC::Proto::MessageGetHeaders &getheaders)
{
  BC::Proto::MessageHeaders headers;
  for (const auto &hash: getheaders.BlockLocatorHashes) {
    auto It = BlockIndex_.blockIndex().find(hash);
    if (It != BlockIndex_.blockIndex().end()) {
      unsigned counter = 0;
      BC::Common::BlockIndex *index = It->second;
      index = index->Next;
      while (index && counter < 2000) {
        if (index->Header.GetHash() == getheaders.HashStop)
          break;
        BC::Proto::BlockHeaderNet header;
        // TODO: don't copy header!
        header.header = index->Header;
        headers.headers.emplace_back(std::move(header));
        index = index->Next;
        counter++;
      }

      break;
    }
  }

  xmstream &stream = LocalStream();
  BC::serialize(stream, headers);
  sendMessage(MessageTy::headers, stream.data(), stream.sizeOf());
}

void Peer::onGetBlocks(BC::Proto::MessageGetBlocks &getblocks)
{
  BC::Proto::MessageInv inv;
  BC::Proto::MessageInv inv2;
  for (const auto &hash: getblocks.BlockLocatorHashes) {
    auto It = BlockIndex_.blockIndex().find(hash);
    if (It != BlockIndex_.blockIndex().end()) {
      unsigned counter = 0;
      BC::Common::BlockIndex *index = It->second;
      index = index->Next;
      while (index && counter < 500) {
        if (index->Header.GetHash() == getblocks.HashStop)
          break;
        BC::Proto::InventoryVector iv;
        iv.type = BC::Proto::MessageInv::MSG_BLOCK;
        iv.hash = index->Header.GetHash();
        inv.Inventory.emplace_back(iv);
        index = index->Next;
        counter++;
      }
      break;
    }
  }

  xmstream &stream = LocalStream();
  BC::serialize(stream, inv);
  sendMessage(MessageTy::inv, stream.data(), stream.sizeOf());
}

void Peer::onGetData(BC::Proto::MessageGetData &getdata)
{
  auto handler = [this](void *data, size_t size) {
    sendMessage(MessageTy::block, data, size);
  };

  {
    BlockSearcher searcher(Storage_.blockDb(), handler, [this](){ postQuitOperation(Base); });
    for (const auto &inv: getdata.inventory) {
      if (inv.type == BC::Proto::MessageInv::MSG_BLOCK) {
        searcher.add(BlockIndex_, inv.hash);
      } else {
        // Other data types not supported now
      }
    }
  }

  if (ProtocolVersion == 70001) {
    xmstream &stream = LocalStream();
    BC::Proto::MessageInv inv;
    inv.Inventory.resize(1);
    inv.Inventory[0].type = BC::Proto::MessageInv::MSG_BLOCK;
    inv.Inventory[0].hash = BlockIndex_.best()->Header.GetHash();
    BC::serialize(stream, inv);
    sendMessage(MessageTy::inv, stream.data(), stream.sizeOf());
  }
}

void Peer::onPing(BC::Proto::MessagePing &ping)
{
  xmstream &stream = LocalStream();
  BC::Proto::MessagePong outMsg;
  outMsg.nonce = ping.nonce;
  BC::serialize(stream, outMsg);
  sendMessage(MessageTy::pong, stream.data(), stream.sizeOf());
}

void Peer::onPong(BC::Proto::MessagePong &pong)
{
  if (pong.nonce == PingLastNonce_) {
    auto now = std::chrono::steady_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - PingStartTime_).count();
    PingStartTime_ = TimeUnknown;
    PingIteration_ = 0;
    if (diff > 0) {
      PingTimes_[PingTimesIdx_++ % PingTimes_.size()] = static_cast<unsigned>(diff);

      if (BlockSource_.get())
        userEventStartTimer(pingEvent, 3*1000000, 1);
      else
        userEventStartTimer(pingEvent, 60*1000000, 1);
    }
  }
}

void Peer::onInv(BC::Proto::MessageInv &inv)
{
  BC::Proto::MessageGetData getBlocks;
  for (const auto &element: inv.Inventory) {
    switch (element.type) {
      case BC::Proto::MessageInv::ERROR :
        LOG_F(WARNING, "Peer %s send error: %s", Name.c_str(), element.hash.ToString().c_str());
        break;
      case BC::Proto::MessageInv::MSG_TX :
        break;
      case BC::Proto::MessageInv::MSG_BLOCK : {
        // Check presense of this block
        auto it = BlockIndex_.blockIndex().find(element.hash);
        if (it == BlockIndex_.blockIndex().end() || it->second->IndexState != BSBlock) {
          BC::Proto::InventoryVector iv;
          iv.type = BC::Proto::MessageInv::MSG_BLOCK;
          iv.hash = element.hash;
          getBlocks.inventory.emplace_back(iv);
        }
        break;
      }
      case BC::Proto::MessageInv::MSG_FILTERED_BLOCK :
        break;
      case BC::Proto::MessageInv::MSG_CMPCT_BLOCK :
        break;

    }
  }

  if (!getBlocks.inventory.empty())
    getData(getBlocks);
}

void Peer::onBlock(SerializedDataObject *object, std::chrono::time_point<std::chrono::steady_clock> receivedTime)
{
  uint32_t sub = 0x10;
  bool scheduledBlock = false;
  if ((blockDownloading.fetch_add(sub) & 0xF) == 2) {
    BC::Proto::Block *block = static_cast<BC::Proto::Block*>(object->unpackedData());
    BC::Proto::BlockHashTy hash = block->header.GetHash();
    if (ScheduledToDownload_.count(hash)) {
      scheduledBlock = true;
      if (++receivedBlocks == ScheduledToDownload_.size())
        sub = 0x12;
    }
  }

  bool downloadFinished = (blockDownloading.fetch_sub(sub) - sub) == 0;
  if (downloadFinished & scheduledBlock) {
    // No concurrency here
    auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(receivedTime - BlockDownloadingStartTime_).count();
    BlockDownloadingStartTime_ = TimeUnknown;
    if (interval > 0)
      DownloadTimes_[DownloadTimesIdx_++ % AvgWindowSize] = static_cast<unsigned>(interval);
  }

  ParentNode->Sync(this, object, scheduledBlock, downloadFinished);
}

void Peer::onReject(BC::Proto::MessageReject &reject)
{
  LOG_F(WARNING, "Reject message: %s; reason: %s", reject.message.c_str(), reject.reason.c_str());
}

void Peer::downloadHeaders(xvector<BC::Proto::BlockHashTy> &&blockLocator, const BC::Proto::BlockHashTy &hashStop)
{
  xmstream &stream = LocalStream();
  BC::Proto::MessageGetHeaders msg;
  // TODO: set correct version
  msg.version = 70015;
  msg.BlockLocatorHashes = blockLocator;
  msg.HashStop = hashStop;
  BC::serialize(stream, msg);

  if (blockLocator.size() == 1)
    headerChainHeight = blockLocator[0];

  sendMessage(MessageTy::getheaders, stream.data(), stream.sizeOf());
  HeaderDownloadingStartTime_ = std::chrono::steady_clock::now();
}

void Peer::getData(const BC::Proto::MessageGetData &getdata)
{
  xmstream &stream = LocalStream();
  BC::serialize(stream, getdata);
  sendMessage(MessageTy::getdata, stream.data(), stream.sizeOf());
}

void Peer::inv(const xvector<BC::Proto::BlockHashTy> &hashes)
{
  BC::Proto::MessageInv inv;
  inv.Inventory.resize(hashes.size());
  for (size_t i = 0, ie = hashes.size(); i < ie; i++) {
    inv.Inventory[i].type = BC::Proto::InventoryVector::MSG_BLOCK;
    inv.Inventory[i].hash = hashes[i];
  }

  xmstream &stream = LocalStream();
  BC::serialize(stream, inv);
  sendMessage(MessageTy::inv, stream.data(), stream.sizeOf());
}

bool Peer::startDownloadBlocks()
{
  uint32_t zero = 0;
  if (blockDownloading.compare_exchange_strong(zero, 1)) {
    receivedBlocks = 0;
    return true;
  } else {
    return false;
  }
}

bool Peer::fetchQueuedBlocks(xvector<BC::Proto::BlockHashTy> &hashes)
{
  uint32_t sub = 0x10;
  if ((blockDownloading.fetch_add(sub) & 0xF) == 2) {
    for (auto &hash: ScheduledToDownload_) {
      auto index = BlockIndex_.blockIndex().find(hash);
      if (index == BlockIndex_.blockIndex().end() || index->second->IndexState != BSBlock)
        hashes.emplace_back(hash);
    }
  }

  return ((blockDownloading.fetch_sub(sub) - sub) == 0);
}

void Peer::scheduleBlocksDownload(uint64_t usTimeout)
{
  userEventStartTimer(blockDownloadEvent, usTimeout, 1);
}

void Peer::downloadBlocks(std::vector<BC::Proto::BlockHashTy> &hashes)
{
  ScheduledToDownload_.clear();
  BC::Proto::MessageGetData msg;
  for (const auto &hash: hashes) {
    ScheduledToDownload_.insert(hash);
    BC::Proto::InventoryVector inv;
    inv.type = BC::Proto::InventoryVector::MSG_BLOCK;
    inv.hash = hash;
    msg.inventory.emplace_back(inv);
  }

  receivedBlocks = 0;
  LastBatchSize_ = hashes.size();

  blockDownloading.fetch_add(1);
  xmstream &stream = LocalStream();
  BC::serialize(stream, msg);

  BlockDownloadingStartTime_ = std::chrono::steady_clock::now();
  sendMessage(MessageTy::getdata, stream.data(), stream.sizeOf());
}

void Peer::cancelDownloadBlocks()
{
  blockDownloading = 0;
}

void Peer::ping()
{
  xmstream &stream = LocalStream();
  PingLastNonce_ = static_cast<uint64_t>(rand());

  BC::Proto::MessagePing outMsg;
  outMsg.nonce = PingLastNonce_;
  BC::serialize(stream, outMsg);

  sendMessage(MessageTy::ping, stream.data(), stream.sizeOf());
  PingStartTime_ = std::chrono::steady_clock::now();
}

bool Peer::isAlive()
{
  auto now = std::chrono::steady_clock::now();
  auto pingTime = std::chrono::duration_cast<std::chrono::seconds>(now - PingStartTime_).count();
  auto headerTime = std::chrono::duration_cast<std::chrono::seconds>(now - HeaderDownloadingStartTime_).count();
  auto blockTime = std::chrono::duration_cast<std::chrono::seconds>(now - BlockDownloadingStartTime_).count();

  // Check ping responses
  bool result = true;
  if (pingTime >= PingResponseTimeoutInSeconds) {
    if (++PingIteration_ < 4) {
      ping();
    } else {
      result = false;
      LOG_F(WARNING, "peer %s ping response timeout", Name.c_str());
    }
  }

  if (headerTime >= HeadersResponseTimeoutInSeconds) {
    result = false;
    LOG_F(WARNING, "peer %s getheaders response timeout", Name.c_str());
  }

  if (blockTime >= BlockDownloadingTimeoutInSeconds) {
    result = false;
    LOG_F(WARNING, "peer %s block downloading timeout", Name.c_str());
  }

  intrusive_ptr<BlockSource> blockSourcePtr(BlockSource_);
  if (blockSourcePtr.get() && blockSourcePtr.get()->downloadFinished())
    BlockSource_.reset();

  return result;
}

void Peer::serializeJson(xmstream &stream) const
{
  stream.write('{');
  ::serializeJson(stream, "address", Name); stream.write(',');
  ::serializeJson(stream, "version", ProtocolVersion); stream.write(',');
  ::serializeJson(stream, "userAgent", UserAgent_); stream.write(',');
  ::serializeJson(stream, "services", Services); stream.write(',');
  ::serializeJson(stream, "latency", averagePing()); stream.write(',');
  ::serializeJson(stream, "sentBytes", receivedBytes()); stream.write(',');
  ::serializeJson(stream, "receivedBytes", sentBytes()); stream.write(',');
  stream.write("\"receivedCommands\": [");
  for (unsigned i = 1; i < static_cast<unsigned>(MessageTy::last); i++) {
    if (i != 1)
      stream.write(',');
    stream.write('{');
    ::serializeJson(stream, "name", messageName(static_cast<MessageTy>(i))); stream.write(',');
    ::serializeJson(stream, "value", receivedMessages(static_cast<MessageTy>(i)));
    stream.write('}');
  }
  stream.write("],");
  stream.write("\"sentCommands\": [");
  for (unsigned i = 1; i < static_cast<unsigned>(MessageTy::last); i++) {
    if (i != 1)
      stream.write(',');
    stream.write('{');
    ::serializeJson(stream, "name", messageName(static_cast<MessageTy>(i))); stream.write(',');
    ::serializeJson(stream, "value", sentMessages(static_cast<MessageTy>(i)));
    stream.write('}');
  }
  stream.write("]");
  stream.write('}');
}

void Node::AddPeer(const HostAddress &address, const char *name, aioObject *object)
{
  if (!object && ++OutgoingConnections_ > OutgoingConnectionsLimit_) {
    LOG_F(WARNING, "Can't connect to %s: outgoing connections limit exceeded", name);
    --OutgoingConnections_;
    return;
  } else if (object && ++IncomingConnections_ > IncomingConnectionsLimit_) {
    LOG_F(WARNING, "Can't connect to %s: incoming connections limit exceeded", name);
    --IncomingConnections_;
    return;
  }

  AtomicPeerPtr *ptr = new AtomicPeerPtr;
  auto It = Peers.insert(std::make_pair(address, ptr));
  if (!It.second) {
    delete ptr;
    ptr = It.first->second;
  }

  PeerPtr peer = new  Peer(*BlockIndex_, *ChainParams_, *Storage_, this, Base, ThreadsNum_, WorkerThreadsNum_, address, object, name);
  if (!ptr->compare_and_exchange(nullptr, peer.get())) {
    deleteUserEvent(peer.get()->blockDownloadEvent);
    deleteUserEvent(peer.get()->pingEvent);
    btcSocketDelete(peer.get()->Socket);
    return;
  }

  peer.get()->start();
}

void Node::RemovePeer(Peer *peer)
{
  if (peer->Deleted_++ == 0) {
    LOG_F(WARNING, "Peer %s: disconnecting...", peer->Name.c_str());
    intrusive_ptr<BlockSource> blockSourcePtr(peer->BlockSource_);
    if (blockSourcePtr.get())
      disconnectPeerFromBlockSource(peer, blockSourcePtr);

    deleteUserEvent(peer->blockDownloadEvent);
    deleteUserEvent(peer->pingEvent);
    btcSocketDelete(peer->Socket);
    auto ptr = Peers[peer->Address];
    if (ptr)
      ptr->reset();
  }
}

void Node::OnPeerConnected(Peer *connectedPeer)
{
  // Sync:
  // Connect new peer to existing block source and start downloading blocks
  bool isProducerPeer = false;
  auto blockSource = BlockSources_.head(ThreadsNum_, false, isProducerPeer);
  if (blockSource.get())
    connectPeerToBlockSource(connectedPeer, nullptr, blockSource.get(), false);

  // Start latency measurement
  connectedPeer->ping();
}

void Node::OnGetAddr(Peer *peer)
{
  BC::Proto::MessageAddr addr;

  enumeratePeers([&addr](Peer *connectedPeer) {
    BC::Proto::NetworkAddress networkAddress;
    networkAddress.setIpv4(connectedPeer->Address.ipv4);
    networkAddress.port = connectedPeer->Address.port;
    networkAddress.services = connectedPeer->Services;
    networkAddress.time = static_cast<uint32_t>(time(nullptr));
    addr.addr_list.emplace_back(networkAddress);
  });

  if (!addr.addr_list.empty()) {
    LOG_F(INFO, "Peer %s: send %zu peers in addr message", peer->Name.c_str(), addr.addr_list.size());
    xmstream &stream = peer->LocalStream();
    BC::serialize(stream, addr);
    peer->sendMessage(Peer::MessageTy::addr, stream.data(), stream.sizeOf());
  }
}

void Node::Start()
{
  Sync();
}

void Node::OnBCNodeConnection(HostAddress address, aioObject *object)
{
  char addressAsString[64];
  {
    struct in_addr a;
    a.s_addr = address.ipv4;
    snprintf(addressAsString, sizeof(addressAsString), "%s:%u", inet_ntoa(a), static_cast<unsigned>(htons(address.port)));
  }

  LOG_F(INFO, "Incoming connection from %s", addressAsString);
  AddPeer(address, addressAsString, object);
}

void Node::Sync()
{
  unsigned interval = 4*1000000;

  BC::Common::BlockIndex *best = BlockIndex_->best();
  bool hasConnectedPeers = false;
  std::vector<PeerPtr> candidatesForSync;

  enumeratePeers([this, best, &hasConnectedPeers, &candidatesForSync](Peer *peer) {
    if (!peer->isAlive()) {
      RemovePeer(peer);
      return;
    }

    if (peer->IsConnected) {
      hasConnectedPeers = true;
      if (!peer->BlockSource_.get() && peer->StartHeight > best->Height)
        candidatesForSync.push_back(peer);
    }
  });

  if (!BlockSources_.hasActiveBlockSource() && !candidatesForSync.empty()) {
    // Select peer with longest chain
    auto sortPredicate = [](const PeerPtr &l, const PeerPtr &r) {
      if (l.get()->StartHeight == r.get()->StartHeight)
        return l.get()->averagePing() < r.get()->averagePing();
      else
        return l.get()->StartHeight < r.get()->StartHeight;
    };

    std::sort(candidatesForSync.begin(), candidatesForSync.end(), sortPredicate);
    Peer *peerWithLongestChain = candidatesForSync.back().get();
    bool newSourceCreated = false;
    intrusive_ptr<BlockSource> blockSource = BlockSources_.head(ThreadsNum_, true, newSourceCreated);
    if (newSourceCreated) {
      if (connectPeerToBlockSource(peerWithLongestChain, nullptr, blockSource.get(), true)) {
        // Try connect all other peers to same block source
        enumeratePeers([this, &blockSource](Peer *connectedPeer) {
          connectPeerToBlockSource(connectedPeer, nullptr, blockSource.get(), false);
        });
      } else {
        BlockSources_.releaseBlockSource(blockSource.get());
      }
    }
  } else {
    if (!hasConnectedPeers)
      interval = 500*1000;
  }

  userEventStartTimer(SyncEvent, interval, 1);
}

void Node::Sync(Peer *peer)
{
  scheduleBlocksDownload(peer);
}

void Node::Sync(Peer *currentPeer, const xvector<BC::Proto::BlockHeaderNet> &headers, unsigned)
{
  intrusive_ptr<BlockSource> blockSourcePtr(currentPeer->BlockSource_);
  BlockSource *blockSource = blockSourcePtr.get();
  if (!blockSource)
    return;

  if (currentPeer->IsProducerPeer_) {
    std::vector<BC::Common::BlockIndex*> indexes;
    if (headers.empty()) {
      LOG_F(INFO, "%s: headers downloading finished", currentPeer->Name.c_str());
      blockSource->setHeadersDownloadingFinished();
      return;
    }

    indexes.resize(headers.size());

    // Continue downloading headers
    BC::Proto::BlockHashTy hash = headers.back().header.GetHash();
    BC::Proto::BlockHashTy hashStop;
    hashStop.SetNull();
    currentPeer->downloadHeaders({hash}, hashStop);
    LOG_F(INFO, "Continue headers downloading from %s (start from %s)", currentPeer->Name.c_str(), hash.ToString().c_str());

    // Accept received headers
    BC::Common::CheckConsensusCtx ccCtx;
    BC::Common::checkConsensusInitialize(ccCtx);
    BC::Proto::BlockHashTy prevHash;
    BC::Common::BlockIndex *index = nullptr;
    bool firstIteration = true;
    for (size_t i = 0, ie = headers.size(); i != ie; i++) {
      const BC::Proto::BlockHeader &header = headers[i].header;
      if (!firstIteration && header.hashPrevBlock != prevHash) {
        // TODO: stop downloading process using this peer
        LOG_F(INFO, "%s: invalid header sequence received", currentPeer->Name.c_str());
        disconnectPeerFromBlockSource(currentPeer, blockSourcePtr);
        return;
      }

      index = AddHeader(*BlockIndex_, *ChainParams_, header, ccCtx);
      if (!index) {
        LOG_F(INFO, "%s: invalid header with hash %s received", currentPeer->Name.c_str(), header.GetHash().ToString().c_str());
        disconnectPeerFromBlockSource(currentPeer, blockSourcePtr);
        return;
      }

      indexes[i] = index;

      prevHash = header.GetHash();
      firstIteration = false;
    }

    blockSource->enqueue(std::move(indexes));
  } else {
    if (headers.empty())
      return;

    if (headers.front().header.GetHash() == currentPeer->LastAskedBlock_->Header.GetHash()) {
      // Current peer still on same chain with linked block source
      currentPeer->LastKnownBlock_ = currentPeer->LastAskedBlock_;
    } else {
      // Current peer have more short chain than block source or on another chain
      bool currentPeerOnAnotherChain = true;
      auto I = BlockIndex_->blockIndex().find(headers.front().header.GetHash());
      if (I != BlockIndex_->blockIndex().end()) {
        BC::Common::BlockIndex *index = currentPeer->LastAskedBlock_;
        BC::Common::BlockIndex *receivedIndex = I->second;
        while (index && index->Height > receivedIndex->Height)
          index = index->Prev;

        if (index == receivedIndex) {
          // Peers are on same chains but 'currentPeer' have less block synced
          // Don't use 'currentPeer' for block downloading some time
          currentPeerOnAnotherChain = false;
        }
      }

      if (currentPeerOnAnotherChain) {
        bool newSourceCreated = false;
        auto nextBlockSource = blockSource->next(ThreadsNum_, true, newSourceCreated);
        connectPeerToBlockSource(currentPeer, blockSource, nextBlockSource.get(), newSourceCreated);
      } else {
        disconnectPeerFromBlockSource(currentPeer, blockSourcePtr);
        // TODO: start timer and try reuse this peer later
      }
    }
  }
}

void Node::Sync(Peer *peer, SerializedDataObject *object, bool scheduledBlock, bool downloadFinished)
{
  BC::Common::CheckConsensusCtx ccCtx;
  BC::Common::checkConsensusInitialize(ccCtx);
  auto oldBest = BlockIndex_->best();

  auto callback = [this, peer, scheduledBlock](const std::vector<BC::Common::BlockIndex*> &acceptedBlocks) {
    // Relay blocks
    if (scheduledBlock)
      return;

    enumeratePeers([peer, &acceptedBlocks](Peer *connectedPeer) {
      if (connectedPeer != peer) {
        for (const auto index: acceptedBlocks)
          connectedPeer->inv({index->Header.GetHash()});
      }
    });
  };

  // Need calculate block hash now
  BC::Common::BlockIndex *index;
  if ( (index = AddBlock(*BlockIndex_, *ChainParams_, *Storage_, object, ccCtx, callback)) ) {
    BC::Proto::BlockHashTy hash = index->Header.GetHash();
    if (index->Height == std::numeric_limits<uint32_t>::max()) {
      LOG_F(INFO, "%s: orhpan block %s received, node possible not synchronized", peer->Name.c_str(), hash.ToString().c_str());

      bool newSourceCreated = false;
      auto blockSource = BlockSources_.head(ThreadsNum_, true, newSourceCreated);
      bool connectedToNewBlockSource = connectPeerToBlockSource(peer, nullptr, blockSource.get(), newSourceCreated);

      if (newSourceCreated) {
        if (connectedToNewBlockSource) {
          // Current peer became a block source, try connect other peers to it
          enumeratePeers([this, &blockSource](Peer *connectedPeer) {
            connectPeerToBlockSource(connectedPeer, nullptr, blockSource.get(), false);
          });
        } else {
          BlockSources_.releaseBlockSource(blockSource.get());
        }
      }
    }
  } else {
    LOG_F(WARNING, "%s: invalid block received", peer->Name.c_str());
  }

  if (index) {
    auto newBest = BlockIndex_->best();
    if (downloadFinished) {
      LOG_F(INFO, "Best chain: %s(%u); Last received: %s(%u); cache: %.3lfM",
            newBest->Header.GetHash().ToString().c_str(), newBest->Height,
            index->Header.GetHash().ToString().c_str(), index->Height,
            Storage_->cache().size() / 1048576.0f);
    } else if (!scheduledBlock && newBest != oldBest) {
      LOG_F(INFO, "New best chain: %s(%u)", newBest->Header.GetHash().ToString().c_str(), newBest->Height);
    }
  }

  if (downloadFinished && scheduledBlock && peer->startDownloadBlocks())
    scheduleBlocksDownload(peer);
}

void Node::Sync(std::vector<BC::Proto::BlockHashTy>&)
{
  // TODO: check that hashes belongs to current block source
}

bool Node::StartTcpServer(HostAddress address, const char *name, aioObject **socket, aioAcceptCb callback)
{
  char addressAsString[64];
  {
    struct in_addr a;
    a.s_addr = address.ipv4;
    snprintf(addressAsString, sizeof(addressAsString), "%s:%u", inet_ntoa(a), static_cast<unsigned>(htons(address.port)));
  }

  socketTy socketFd = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(socketFd);
  if (socketBind(socketFd, &address) != 0) {
    LOG_F(ERROR, "Can't start %s server at address %s (bind error; address already used)", name, addressAsString);
    socketClose(socketFd);
    return false;
  }

  if (socketListen(socketFd) != 0) {
    LOG_F(ERROR, "Can't start %s server at address %s (listen error)", name, addressAsString);
    socketClose(socketFd);
    return false;
  }

  *socket = newSocketIo(Base, socketFd);
  aioAccept(bcNodeSocket, 0, callback, this);
  LOG_F(INFO, "%s server started at %s", name, addressAsString);
  return true;
}

void Node::buildBlockLocator(xvector<BC::Proto::BlockHashTy> &hashes, BC::Common::BlockIndex *start)
{
  if (start->Height > 0) {
    uint32_t step = 1;
    hashes.emplace_back(start->Header.GetHash());
    for (uint32_t i = start->Height-1; i > step; i -= step) {
      auto It = BlockIndex_->blockHeightIndex().find(i);
      if (It != BlockIndex_->blockHeightIndex().end())
        hashes.emplace_back(It->second->Header.GetHash());
      else
        break;

      if (hashes.size() >= 8)
        step *=2;
    }
  }

  hashes.emplace_back(ChainParams_->GenesisBlock.header.GetHash());
}

bool Node::connectPeerToBlockSource(Peer *peer, BlockSource *current, BlockSource *next, bool isProducer)
{
  if (peer->BlockSource_.compare_and_exchange(current, next)) {
    LOG_F(INFO, "Begin downloading %s from %s", isProducer ? "headers & blocks" : "blocks", peer->Name.c_str());
    peer->IsProducerPeer_ = isProducer;
    if (isProducer) {
      xvector<BC::Proto::BlockHashTy> blockLocator;
      buildBlockLocator(blockLocator, BlockIndex_->best());
      peer->downloadHeaders(std::move(blockLocator), BC::Proto::BlockHashTy::getNull());
    }

    if (peer->startDownloadBlocks())
      scheduleBlocksDownload(peer);

    peer->ping();
    return true;
  }

  return false;
}

bool Node::switchBlockSource(Peer *peer, BlockSource *current)
{
  bool newSourceCreated = false;
  intrusive_ptr<BlockSource> next = current->next(ThreadsNum_, false, newSourceCreated);
  if (peer->BlockSource_.compare_and_exchange(current, next.get())) {
    if (peer->IsProducerPeer_)
      BlockSources_.releaseBlockSource(current);
    peer->IsProducerPeer_ = false;

    if (next.get() == nullptr)
      LOG_F(INFO, "stop downloading blocks from %s", peer->Name.c_str());

    return next.get();
  } else {
    return false;
  }
}

void Node::disconnectPeerFromBlockSource(Peer *peer, intrusive_ptr<BlockSource> &current)
{
  LOG_F(INFO, "stop downloading blocks from %s", peer->Name.c_str());
  peer->BlockSource_.reset();
  if (peer->IsProducerPeer_)
    BlockSources_.releaseBlockSource(current.get());
}

bool Node::scheduleBlocksDownload(Peer *slave)
{
  intrusive_ptr<BlockSource> blockSourcePtr(slave->BlockSource_);
  if (!blockSourcePtr.get()) {
    LOG_F(ERROR, "no block source for peer %s", slave->Name.c_str());
    slave->cancelDownloadBlocks();
    return false;
  }

  BlockSource &blockSource = *blockSourcePtr.get();

  if (blockSource.downloadFinished()) {
    // All blocks downloaded, switch to other block source or stop downloading blocks
    if (switchBlockSource(slave, &blockSource))
      slave->scheduleBlocksDownload(100*1000);
    else
      slave->cancelDownloadBlocks();

    return false;
  }

  // Calculate coefficient for adjusting batch size
  bool blockCacheOverflow = Storage_->cache().overflow();
  double coeff = (300.0+slave->averagePing()) / slave->averageDownloadTime();
  // Normalize coefficient
  coeff = std::max(coeff, 0.25);
  coeff = std::min(coeff, 4.0);
  if (blockCacheOverflow)
    coeff /= 16.0;
  // Calculate batch size
  size_t batchSize = static_cast<size_t>(slave->LastBatchSize_ * coeff);
  // Normalize batch size
  batchSize = std::max(batchSize, static_cast<size_t>(1));
  batchSize = std::min(batchSize, static_cast<size_t>(1024));

  std::vector<BC::Common::BlockIndex*> indexes;
  if (!blockSource.dequeue(indexes, batchSize, blockCacheOverflow)) {
    // Block source is empty at this moment
    if (blockSource.headersDownloadingFinished() || blockCacheOverflow) {
      // Collect stalled blocks
      blockSource.processStalledBlocks();
      if (!blockSource.dequeue(indexes, batchSize, blockCacheOverflow)) {
        slave->scheduleBlocksDownload(1*1000000);
        return false;
      }

      if (blockCacheOverflow)
        LOG_F(WARNING, "Block cache overflow, retry download stalled blocks");
    } else {
      // Block source downloads headers too slow, wait it
      slave->scheduleBlocksDownload(5*1000000);
      return false;
    }
  }

  LOG_F(WARNING, "%s: download time: %u, last batch size: %zu, new batch size: %zu; ping: %u", slave->Name.c_str(), slave->averageDownloadTime(), slave->LastBatchSize_, batchSize, slave->averagePing());

  if (!slave->IsProducerPeer_) {
    if (!slave->LastKnownBlock_ || indexes.back()->Height > slave->LastKnownBlock_->Height) {
      // Master peer contains last received header, we can build block locator based on this header
      // and use getheaders message for checking slave peer chain
      slave->LastAskedBlock_ = blockSource.lastKnownIndex();
      xvector<BC::Proto::BlockHashTy> hashes;
      buildBlockLocator(hashes, slave->LastAskedBlock_->Prev);
      slave->downloadHeaders(std::move(hashes), slave->LastAskedBlock_->Header.GetHash());
//      LOG_F(WARNING, "Update known index for peer %s %s (from block %s (%u))", slave->Name.c_str(), hashes.front().ToString().c_str(), slave->LastAskedBlock_->Prev->Header.GetHash().ToString().c_str(), slave->LastAskedBlock_->Prev->Height);
    }
  }

  auto now = std::chrono::steady_clock::now();
  std::vector<BC::Proto::BlockHashTy> hashes;
  for (const auto &index: indexes) {
    auto downloadTime = std::chrono::duration_cast<std::chrono::seconds>(now - index->DownloadingStartTime).count();
    if (index->IndexState.load(std::memory_order::memory_order_relaxed) == BSHeader &&
        (index->DownloadingStartTime == TimeUnknown || downloadTime > 8)) {
      index->DownloadingStartTime = now;
      hashes.emplace_back(index->Header.GetHash());
    }
  }

  if (!hashes.empty())
    slave->downloadBlocks(hashes);
  else
    slave->scheduleBlocksDownload(1*1000000);

//  LOG_F(WARNING, "Requested %zu blocks from %s ([%s(%u), %s(%u)]", hashes.size(), slave->Name.c_str(), hashes.front().ToString().c_str(), indexes.front()->Height, hashes.back().ToString().c_str(), indexes.back()->Height);
  return true;
}

}
}
