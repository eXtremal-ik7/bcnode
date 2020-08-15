#include "BC/bc.h"

#include <asyncio/asyncio.h>
#include "asyncio/socket.h"
#include <asyncioextras/btc.h>
#include <p2putils/xmstream.h>
#include <chrono>
#include <functional>
#include <map>
#include <unordered_map>

namespace BC {
namespace Network {

template<typename Handler>
class Connection {
public:
  Connection(Handler &handler, asyncBase *base, HostAddress address, uint32_t magic) :
    Handler_(handler),
    Base_(base),
    Address_(address)
  {
    socketTy socketFd = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
    HostAddress localAddress;
    localAddress.family = AF_INET;
    localAddress.ipv4 = INADDR_ANY;
    localAddress.port = 0;
    if (socketBind(socketFd, &localAddress) != 0) {
      socketClose(socketFd);
      return;
    }

    aioObject *object = newSocketIo(base, socketFd);
    Socket_ = btcSocketNew(base, object);
    btcSocketSetMagic(Socket_, magic);
  }

  void start() {
    aioConnect(btcGetPlainSocket(Socket_), &Address_, 5*1000000, onConnectCb, this);
  }

  void close() {
    btcSocketDelete(Socket_);
  }

  void ping() {
    char buffer[1024];
    xmstream localStream(buffer, sizeof(buffer));
    localStream.reset();
    uint64_t nonce = rand();
    PingMap_[nonce] = std::chrono::steady_clock::now();

    BC::Proto::MessagePing ping;
    ping.nonce = nonce;
    BC::serialize(localStream, ping);
    sendMessage(MessageTy::ping, localStream.data(), localStream.sizeOf());
  }

  void getaddr() {
    sendMessage(MessageTy::getaddr, nullptr, 0);
  }

  uint32_t startHeight() { return StartHeight_; }
  const std::string &userAgent() { return UserAgent_; }

private:
  static void onConnectCb(AsyncOpStatus status, aioObject*, void *arg) { static_cast<Connection*>(arg)->onConnect(status); }
  static void onMessageCb(AsyncOpStatus status, BTCSocket*, char*, xmstream*, void *arg) { static_cast<Connection*>(arg)->onMessage(status); }

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

  template<typename Msg, typename Fn> inline bool callHandler(const char *cmd, Fn proc) {
      Msg data;
      if (unserializeAndCheck(ReceiveStream_, data)) {
        aioBtcRecv(Socket_, Command_, ReceiveStream_, Limit_, afNone, 0, onMessageCb, this);
        std::invoke(proc, *this, data);
        return true;
      } else {
        Handler_.onInvalidMessageFormat(this, cmd);
        return false;
      }
  }

  template<typename Fn> inline bool callHandlerEmpty(Fn proc) {
    aioBtcRecv(Socket_, Command_, ReceiveStream_, Limit_, afNone, 0, onMessageCb, this);
    std::invoke(proc, *this);
    return true;
  }

  static constexpr const char *messageName(MessageTy type) {
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

  std::unordered_map<std::string, MessageTy> MessageTypeMap_ = {
    {"addr", Connection::MessageTy::addr},
    {"block", Connection::MessageTy::block},
    {"getaddr", Connection::MessageTy::getaddr},
    {"getblocks", Connection::MessageTy::getblocks},
    {"getdata", Connection::MessageTy::getdata},
    {"getheaders", Connection::MessageTy::getheaders},
    {"headers", Connection::MessageTy::headers},
    {"inv", Connection::MessageTy::inv},
    {"ping", Connection::MessageTy::ping},
    {"pong", Connection::MessageTy::pong},
    {"reject", Connection::MessageTy::reject},
    {"verack", Connection::MessageTy::verack},
    {"version", Connection::MessageTy::version}
  };

  void sendMessage(MessageTy type, void *data, size_t size) {
    aioBtcSend(Socket_, messageName(type), data, size, afNone, 0, nullptr, nullptr);
  }

  void onConnect(AsyncOpStatus status) {
    if (status != aosSuccess) {
      btcSocketDelete(Socket_);
      Handler_.onDisconnect(this);
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
    msg.nonce = rand();
    msg.user_agent = BC::Configuration::UserAgent;
    msg.start_height = 1;
    msg.relay = 1;

    char buffer[1024];
    xmstream localStream(buffer, sizeof(buffer));
    localStream.reset();
    BC::serialize(localStream, msg);
    sendMessage(MessageTy::version, localStream.data(), localStream.sizeOf());
    aioBtcRecv(Socket_, Command_, ReceiveStream_, Limit_, afNone, ConnectTimeout_, onMessageCb, this);
  }

  void onMessage(AsyncOpStatus status) {
    if (status != aosSuccess) {
      btcSocketDelete(Socket_);
      Handler_.onDisconnect(this);
      return;
    }

    MessageTy command = MessageTypeMap_[Command_];
    bool result = true;
    switch (command) {
      // "real time" operations
      case MessageTy::addr :
        result = callHandler<BC::Proto::MessageAddr>("addr", &Connection::onAddr);
        break;
      case MessageTy::getaddr :
        result = callHandlerEmpty(&Connection::onGetAddr);
        break;
      case MessageTy::getheaders :
        result = callHandler<BC::Proto::MessageGetHeaders>("getheaders", &Connection::onGetHeaders);
        break;
      case MessageTy::inv :
        result = callHandler<BC::Proto::MessageInv>("inv", &Connection::onInv);
        break;
      case MessageTy::ping :
        result = callHandler<BC::Proto::MessagePing>("ping", &Connection::onPing);
        break;
      case MessageTy::pong :
        result = callHandler<BC::Proto::MessagePong>("pong", &Connection::onPong);
        break;
      case MessageTy::reject :
        result = callHandler<BC::Proto::MessageReject>("reject", &Connection::onReject);
        break;
      case MessageTy::verack :
        result = callHandlerEmpty(&Connection::onVerack);
        break;
      case MessageTy::version :
        result = callHandler<BC::Proto::MessageVersion>("version", &Connection::onVersion);
        break;

      // Heavy operations
      case MessageTy::getblocks :
        result = callHandler<BC::Proto::MessageGetBlocks>("getblocks", &Connection::onGetBlocks);
        break;
      case MessageTy::getdata :
        result = callHandler<BC::Proto::MessageGetData>("getdata", &Connection::onGetData);
        break;

      // Special handlers
      case MessageTy::block :
        result = callHandler<BC::Proto::MessageBlock>("block", &Connection::onBlock);
        break;
      case MessageTy::headers :
        result = callHandler<BC::Proto::MessageHeaders>("headers", &Connection::onHeaders);
        break;
      default :
        Handler_.onUnknownMessage(this, Command_);
        aioBtcRecv(Socket_, Command_, ReceiveStream_, Limit_, afNone, 0, onMessageCb, this);
        break;
    }

    if (!result) {
      btcSocketDelete(Socket_);
      Handler_.onDisconnect(this);
    }
  }

  void onAddr(BC::Proto::MessageAddr &addr) {
    Handler_.onAddr(this, addr);
  }

  void onGetAddr() {
    Handler_.onGetAddr(this);
  }

  void onGetHeaders(BC::Proto::MessageGetHeaders &getheaders) {
    Handler_.onGetHeaders(this, getheaders);
  }

  void onInv(BC::Proto::MessageInv &inv) {
    Handler_.onInv(this, inv);
  }

  void onPing(BC::Proto::MessagePing &ping) {
    char buffer[1024];
    xmstream localStream(buffer, sizeof(buffer));
    localStream.reset();
    BC::Proto::MessagePong pong;
    pong.nonce = ping.nonce;
    BC::serialize(localStream, pong);
    sendMessage(MessageTy::pong, localStream.data(), localStream.sizeOf());
    Handler_.onPing(this);
  }

  void onPong(BC::Proto::MessagePong &pong) {
    auto It = PingMap_.find(pong.nonce);
    if (It != PingMap_.end()) {
      auto pingTime = It->second;
      auto now = std::chrono::steady_clock::now();
      PingMap_.erase(It);
      Handler_.onPong(this, std::chrono::duration_cast<std::chrono::milliseconds>(now - pingTime).count());
    }
  }

  void onReject(BC::Proto::MessageReject &reject) {
    Handler_.onReject(this, reject);
  }

  void onVerack() {
    VerackReceived_ = true;
    if (!IsConnected_ && (VersionReceived_ & VerackReceived_)) {
      IsConnected_ = true;
      Handler_.onConnect(this);
      ping();
    }
  }

  void onVersion(BC::Proto::MessageVersion &version) {
    StartHeight_ = version.start_height;
    ProtocolVersion_ = version.version;
    Services_ = version.services;
    UserAgent_ = version.user_agent;

    VersionReceived_ = true;
    if (!IsConnected_ && (VersionReceived_ & VerackReceived_)) {
      IsConnected_ = true;
      Handler_.onConnect(this);
      ping();
    }

    sendMessage(MessageTy::verack, nullptr, 0);
  }

  void onGetBlocks(BC::Proto::MessageGetBlocks &getblocks) {
    Handler_.onGetBlocks(this, getblocks);
  }

  void onGetData(BC::Proto::MessageGetData &getdata) {
    Handler_.onGetData(this, getdata);
  }

  void onBlock(BC::Proto::MessageBlock &block) {
    Handler_.onBlock(this, block);
  }

  void onHeaders(BC::Proto::MessageHeaders &headers) {
    Handler_.onHeaders(this, headers);
  }

private:
  Handler &Handler_;
  asyncBase *Base_;
  HostAddress Address_;
  BTCSocket *Socket_;
  char Command_[12];
  xmstream ReceiveStream_;

  bool VersionReceived_ = false;
  bool VerackReceived_ = false;
  bool IsConnected_ = false;
  uint32_t StartHeight_ = 0;
  uint32_t ProtocolVersion_ = 0;
  uint64_t Services_ = 0;
  std::string UserAgent_;

  std::map<uint64_t, std::chrono::time_point<std::chrono::steady_clock>> PingMap_;

  static constexpr size_t Limit_ = 67108864; // 64Mb
  static constexpr uint64_t ConnectTimeout_ = 5*1000000;


};

}
}
