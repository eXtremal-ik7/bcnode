#pragma once
#include "common/blockDataBase.h"
#include "common/blockSource.h"
#include "common/hostAddress.h"
#include "common/serializedDataCache.h"
#include "common/thread.h"
#include "../loguru.hpp"

#include <asyncio/asyncio.h>
#include <asyncioextras/btc.h>

#include <p2putils/xmstream.h>
#include <tbb/concurrent_queue.h>
#include <tbb/concurrent_unordered_map.h>
#include <array>
#include <chrono>
#include <functional>
#include <set>


struct BCNodeContext;

namespace BC {
namespace Network {

class Node;
class Peer;

class PeerEmptyDeleter {
public:
  void operator()(Peer*) {}
};

typedef atomic_intrusive_ptr<Peer, PeerEmptyDeleter> AtomicPeerPtr;
typedef intrusive_ptr<Peer, PeerEmptyDeleter> PeerPtr;

static constexpr std::chrono::time_point<std::chrono::steady_clock> TimeUnknown = std::chrono::time_point<std::chrono::steady_clock>::max();

class alignas(512) Peer {
private:
  Peer(BlockInMemoryIndex &blockIndex,
       BC::Common::ChainParams &chainParams,
       BC::DB::Storage &storage,
       Node *node,
       asyncBase *base,
       unsigned threadsNum,
       unsigned rtThreadsNum,
       HostAddress address,
       aioObject *object,
       const char *name);

public:
  Peer(const Peer&) = delete;
  Peer(Peer&&) = delete;
  ~Peer();

  void start();

public:
  uintptr_t ref_fetch_add(uintptr_t tag) { return objectIncrementReference(btcSocketHandle(Socket), tag); }
  uintptr_t ref_fetch_sub(uintptr_t tag) { return objectDecrementReference(btcSocketHandle(Socket), tag); }

private:
  enum class MessageTy : unsigned {
    unknown = 0,
    addr,
    block,
    getaddr,
    getblocks,
    getdata,
    getheaders,
    headers,
    inv,
    ping,
    pong,
    reject,
    verack,
    version,
    last
  };

  struct InternalMessage {
    PeerPtr Source;
    MessageTy Type;
    void *Data;
    size_t Size;
    size_t MemorySize;
    std::chrono::time_point<std::chrono::steady_clock> Time;
    InternalMessage(Peer *peer) : Source(peer) {}
  };

  static constexpr const char *messageName(MessageTy type);
  static std::unordered_map<std::string, MessageTy> MessageTypeMap_;
  static std::atomic<unsigned> ActiveThreads_;
  static tbb::concurrent_queue<InternalMessage*> MessageQueue_;


  bool startHeavyOperation(bool *heavyOperation, bool *heavyOperationStarted) {
    *heavyOperation = true;
    *heavyOperationStarted = ActiveThreads_++ < WorkerThreadsNum_;
    return *heavyOperationStarted;
  }

  void finishHeavyOperation(bool heavyOperation, bool heavyOperationStarted) {
    if (heavyOperationStarted)
      processMessageQueue();
    if (heavyOperation)
      ActiveThreads_--;
  }

  xmstream &LocalStream() {
    xmstream &stream = SendStreams[GetWorkerThreadId()];
    stream.reset();
    return stream;
  }

  static void socketDestructorCb(aioObjectRoot*, void *arg);
  static void eventDestructorCb(aioUserEvent*, void *arg);
  static void blockDownloadCb(aioUserEvent*, void *arg);
  static void pingEventCb(aioUserEvent*, void *arg);

  static void onConnectCb(AsyncOpStatus status, aioObject*, void *arg) { static_cast<Peer*>(arg)->onConnect(status); }
  static void onMessageCb(AsyncOpStatus status, BTCSocket*, char*, xmstream*, void *arg) { static_cast<Peer*>(arg)->onMessage(status); }

  void onConnect(AsyncOpStatus status);
  void onMessage(AsyncOpStatus status);
  static void processMessageQueue();

  void onVersion(BC::Proto::MessageVersion &version);
  void onVerAck();
  void onGetAddr();
  void onAddr(BC::Proto::MessageAddr &addr);
  void onHeaders(BC::Proto::MessageHeaders &getheaders);
  void onGetBlocks(BC::Proto::MessageGetBlocks &getblocks);
  void onGetData(BC::Proto::MessageGetData &getdata);
  void onGetHeaders(BC::Proto::MessageGetHeaders &getheaders);
  void onPing(BC::Proto::MessagePing &ping);
  void onPong(BC::Proto::MessagePong &pong);
  void onInv(BC::Proto::MessageInv &inv);
  void onBlock(SerializedDataObject *block, std::chrono::time_point<std::chrono::steady_clock> receivedTime);
  void onReject(BC::Proto::MessageReject &reject);

  // Synchronization
  void downloadHeaders(xvector<BC::Proto::BlockHashTy> &&blockLocator, const BC::Proto::BlockHashTy &hashStop);
  bool startDownloadBlocks();
  void downloadBlocks(std::vector<BC::Proto::BlockHashTy> &hashes);
  void cancelDownloadBlocks();
  void ping();

  bool fetchQueuedBlocks(xvector<BC::Proto::BlockHashTy> &hashes);
  void scheduleBlocksDownload(uint64_t usTimeout);
  void getData(const BC::Proto::MessageGetData &getdata);
  void inv(const xvector<BC::Proto::BlockHashTy> &hashes);

private:
  static constexpr unsigned AvgWindowSize = 8;
  static constexpr uint64_t ConnectTimeout = 60*1000000;
  static constexpr int64_t PingResponseTimeoutInSeconds = 30;
  static constexpr int64_t HeadersResponseTimeoutInSeconds = 12;
  static constexpr int64_t BlockDownloadingTimeoutInSeconds = 60;
  static constexpr size_t Limit = 67108864; // 64Mb

  template<typename T, size_t size> T avg(const std::array<T, size> &data, unsigned currentIdx, T defaultValue) const {
    T sum = 0;

    if (currentIdx >= data.size()) {
      for (auto &v: data)
        sum += v;
      return sum / static_cast<T>(data.size());
    } else {
      for (unsigned i = 0; i < currentIdx; i++)
        sum += data[i];
      return currentIdx ? sum / currentIdx : defaultValue;
    }
  }

private:
  BlockInMemoryIndex &BlockIndex_;
  BC::Common::ChainParams &ChainParams_;
  BC::DB::Storage &Storage_;
  HostAddress Address;
  std::string Name;

  Node *ParentNode = nullptr;
  asyncBase *Base = nullptr;
  BTCSocket *Socket = nullptr;
  unsigned ThreadsNum_ = 0;
  unsigned WorkerThreadsNum_ = 0;
  std::atomic<unsigned> Deleted_ = 0;

  std::unique_ptr<xmstream[]> SendStreams;
  xmstream ReceiveStream;
  xmstream UnpackedStream_;
  char Command[12];

  bool Incoming = false;
  bool VersionReceived = false;
  bool VerackReceived = false;
  bool IsConnected = false;

  uint32_t StartHeight = 0;
  uint32_t ProtocolVersion = 0;
  uint64_t Services = 0;
  std::string UserAgent_;

  // Downloading area
  bool IsProducerPeer_;
  atomic_intrusive_ptr<BlockSource> BlockSource_;
  BC::Proto::BlockHashTy headerChainHeight;
  BC::Common::BlockIndex *LastKnownBlock_ = nullptr;
  BC::Common::BlockIndex *LastAskedBlock_ = nullptr;

  aioUserEvent *blockDownloadEvent = nullptr;
  aioUserEvent *pingEvent = nullptr;
  std::atomic<uint32_t> blockDownloading = 0;
  std::atomic<uint32_t> receivedBlocks = 0;
  std::set<BC::Proto::BlockHashTy> ScheduledToDownload_;
  size_t LastBatchSize_ = 1;

  // Statistics area
  unsigned DownloadTimesIdx_ = 0;
  std::array<unsigned, AvgWindowSize> DownloadTimes_;
  std::chrono::time_point<std::chrono::steady_clock> BlockDownloadingStartTime_ = TimeUnknown;

  unsigned PingTimesIdx_ = 0;
  unsigned PingIteration_ = 0;
  std::array<unsigned, AvgWindowSize> PingTimes_;
  std::chrono::time_point<std::chrono::steady_clock> PingStartTime_ = TimeUnknown;
  uint64_t PingLastNonce_ = 0;

  std::chrono::time_point<std::chrono::steady_clock> HeaderDownloadingStartTime_ = TimeUnknown;

  std::unique_ptr<uint64_t[]> ReceivedBytes_;
  std::unique_ptr<uint64_t[]> SentBytes_;
  std::unique_ptr<uint64_t[]> ReceivedCommands_;
  std::unique_ptr<uint64_t[]> SentCommands_;

  bool isAlive();
  unsigned averageDownloadTime() const { return avg(DownloadTimes_, DownloadTimesIdx_, 500u); }
  unsigned averagePing() const { return avg(PingTimes_, PingTimesIdx_, 100u); }

  uint64_t receivedBytes() const {
    uint64_t sum = 0;
    for (unsigned i = 0; i < ThreadsNum_; i++)
      sum += ReceivedBytes_[i];
    return sum;
  }

  uint64_t sentBytes() const {
    uint64_t sum = 0;
    for (unsigned i = 0; i < ThreadsNum_; i++)
      sum += SentBytes_[i];
    return sum;
  }

  uint64_t receivedMessages(MessageTy type) const {
    uint64_t sum = 0;
    unsigned numberOfMessages = static_cast<unsigned>(MessageTy::last);
    unsigned commandId = static_cast<unsigned>(type);
    for (unsigned i = 0; i < ThreadsNum_; i++)
      sum += ReceivedCommands_[i*numberOfMessages + commandId];
    return sum;
  }

  uint64_t sentMessages(MessageTy type) const {
    uint64_t sum = 0;
    unsigned numberOfMessages = static_cast<unsigned>(MessageTy::last);
    unsigned commandId = static_cast<unsigned>(type);
    for (unsigned i = 0; i < ThreadsNum_; i++)
      sum += SentCommands_[i*numberOfMessages + commandId];
    return sum;
  }

private:
  template<typename Fn> inline bool callHandlerEmpty(Fn proc) {
    aioBtcRecv(Socket, Command, ReceiveStream, Limit, afNone, 0, onMessageCb, this);
    std::invoke(proc, *this);
    return true;
  }

  template<typename Msg, typename Fn> inline bool callHandler(const char *cmd, Fn proc) {
      Msg data;
      if (unserializeAndCheck(ReceiveStream, data)) {
        aioBtcRecv(Socket, Command, ReceiveStream, Limit, afNone, 0, onMessageCb, this);
        std::invoke(proc, *this, data);
        return true;
      } else {
        LOG_F(INFO, "Can't unserialize message %s", cmd);
        return false;
      }
  }

  template<typename Msg> inline bool pushInternalMessage(const char *cmd, MessageTy type) {
    uint8_t *memory = static_cast<uint8_t*>(operator new(sizeof(InternalMessage) + sizeof(Msg)));
    InternalMessage *internalMsg = reinterpret_cast<InternalMessage*>(memory);
    Msg *msg = reinterpret_cast<Msg*>(memory + sizeof(InternalMessage));
    new (internalMsg) InternalMessage(this);
    new (msg) Msg;
    if (unserializeAndCheck(ReceiveStream, *msg)) {
      aioBtcRecv(Socket, Command, ReceiveStream, Limit, afNone, 0, onMessageCb, this);
      internalMsg->Type = type;
      internalMsg->Data = msg;
      MessageQueue_.push(internalMsg);
      return true;
    } else {
      LOG_F(INFO, "Can't unserialize message %s", cmd);
      operator delete(memory);
      return false;
    }
  }

  void sendMessage(MessageTy type, void *data, size_t size);

private:
  friend class Node;

public:
  void serializeJson(xmstream &stream) const;
};

class Node {
private:
  typedef std::vector<BC::Proto::BlockHashTy> HashArray;

private:
  BlockInMemoryIndex *BlockIndex_;
  BC::Common::ChainParams *ChainParams_;
  BC::DB::Storage *Storage_;
  asyncBase *Base;
  unsigned ThreadsNum_;
  unsigned WorkerThreadsNum_;
  unsigned OutgoingConnectionsLimit_;
  unsigned IncomingConnectionsLimit_;
  tbb::concurrent_unordered_map<HostAddress, AtomicPeerPtr*, HostAddressCompare, HostAddressEqual> Peers;

  aioObject *bcNodeSocket = nullptr;
  aioObject *httpApiSocket = nullptr;
  aioObject *nativeApiSocket = nullptr;

  std::atomic<unsigned> OutgoingConnections_ = 0;
  std::atomic<unsigned> IncomingConnections_ = 0;

  // Synchonization data
  uint64_t LocalHostNonce_;
  BlockSourceList BlockSources_;
  aioUserEvent *SyncEvent = nullptr;

  static void SyncCb(aioUserEvent*, void *arg) { static_cast<Node*>(arg)->Sync(); }
  static void bcnodeAcceptCb(AsyncOpStatus status, aioObject *object, HostAddress address, socketTy socketFd, void *arg) {
    if (status == aosSuccess)
      static_cast<Network::Node*>(arg)->OnBCNodeConnection(address, newSocketIo(aioGetBase(object), socketFd));
    else
      LOG_F(ERROR, "BCNode accept connection failed");
    aioAccept(object, 0, bcnodeAcceptCb, arg);
  }

  void OnBCNodeConnection(HostAddress address, aioObject *object);

public:
  void Init(BlockInMemoryIndex &blockIndex,
            BC::Common::ChainParams &chainParams,
            BC::DB::Storage &storage,
            asyncBase *base,
            unsigned threadsNum,
            unsigned workerThreadsNum,
            unsigned outgoingConnectionsLimit,
            unsigned incomingConnectionsLimit) {
    BlockIndex_ = &blockIndex;
    ChainParams_ = &chainParams;
    Storage_ = &storage;
    Base = base;
    ThreadsNum_ = threadsNum;
    WorkerThreadsNum_ = workerThreadsNum;
    OutgoingConnectionsLimit_ = outgoingConnectionsLimit;
    IncomingConnectionsLimit_ = incomingConnectionsLimit;
    SyncEvent = newUserEvent(base, 0, SyncCb, this);
    for (unsigned i = 0; i < sizeof(LocalHostNonce_); i++)
      reinterpret_cast<uint8_t*>(&LocalHostNonce_)[i] = rand();
  }

  size_t PeerCount() { return Peers.size(); }
  void AddPeer(const HostAddress &address, const char *name, aioObject *object);
  void RemovePeer(Peer *peer);
  void OnPeerConnected(Peer *peer);
  void OnGetAddr(Peer *peer);

  void Start();
  bool StartBCNodeServer(HostAddress address) { return StartTcpServer(address, "BCNode", &bcNodeSocket, bcnodeAcceptCb); }

  uint64_t localHostNonce() { return LocalHostNonce_; }

  // Synchronization functions
  void Sync();
  void Sync(Peer *peer);
  void Sync(Peer *peer, const xvector<BC::Proto::BlockHeaderNet> &headers, unsigned downloadTimeInMilliSeconds);
  void Sync(Peer *peer, SerializedDataObject *object, bool scheduledBlock, bool downloadFinished);
  void Sync(std::vector<BC::Proto::BlockHashTy> &hashes);

  // API
  void enumeratePeers(std::function<void(Peer*)> callback) {
    for (auto peer: Peers) {
      PeerPtr ptr(*peer.second);
      if (ptr.get())
        callback(ptr.get());
    }
  }

private:
  bool StartTcpServer(HostAddress, const char *name, aioObject **socket, aioAcceptCb callback);
  void buildBlockLocator(xvector<BC::Proto::BlockHashTy> &hashes, BC::Common::BlockIndex *start);

  bool connectPeerToBlockSource(Peer *peer, BlockSource *current, BlockSource *next, bool isProducer);
  bool switchBlockSource(Peer *peer, BlockSource *current);
  void disconnectPeerFromBlockSource(Peer *peer, intrusive_ptr<BlockSource> &current);

  /// scheduleBlocksDownload - schedule for download next portion of blocks
  ///   @param slave: source of blocks
  bool scheduleBlocksDownload(Peer *slave);
};

}
}

static inline void serializeJson(xmstream &stream, const BC::Network::Peer &peer) { peer.serializeJson(stream); }
