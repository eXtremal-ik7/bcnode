#include "BC/bc.h"
#include "common/smallStream.h"

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
    SmallStream<1024> localStream;
    uint64_t nonce = rand();
    PingMap_[nonce] = std::chrono::steady_clock::now();

    BC::Proto::CMessagePing ping;
    ping.Nonce = nonce;
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
    BC::Proto::CMessageVersion msg;
    msg.Version = BC::Configuration::ProtocolVersion;
    msg.Services = 1; // NODE
    msg.Timestamp = static_cast<uint64_t>(time(nullptr));
    msg.AddrRecv.Services = 0;
    msg.AddrRecv.setIpv4(0);
    msg.AddrRecv.Port = 0;
    msg.AddrFrom.Services = 0; // NODE
    msg.AddrFrom.reset();
    msg.AddrFrom.Port = 0;
    msg.Nonce = rand();
    msg.UserAgent = BC::Configuration::UserAgent;
    msg.StartHeight = 1;
    msg.Relay = 1;

    SmallStream<1024> localStream;
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
        result = callHandler<BC::Proto::CMessageAddr>("addr", &Connection::onAddr);
        break;
      case MessageTy::getaddr :
        result = callHandlerEmpty(&Connection::onGetAddr);
        break;
      case MessageTy::getheaders :
        result = callHandler<BC::Proto::CMessageGetHeaders>("getheaders", &Connection::onGetHeaders);
        break;
      case MessageTy::inv :
        result = callHandler<BC::Proto::CMessageInv>("inv", &Connection::onInv);
        break;
      case MessageTy::ping :
        result = callHandler<BC::Proto::CMessagePing>("ping", &Connection::onPing);
        break;
      case MessageTy::pong :
        result = callHandler<BC::Proto::CMessagePong>("pong", &Connection::onPong);
        break;
      case MessageTy::reject :
        result = callHandler<BC::Proto::CMessageReject>("reject", &Connection::onReject);
        break;
      case MessageTy::verack :
        result = callHandlerEmpty(&Connection::onVerack);
        break;
      case MessageTy::version :
        result = callHandler<BC::Proto::CMessageVersion>("version", &Connection::onVersion);
        break;

      // Heavy operations
      case MessageTy::getblocks :
        result = callHandler<BC::Proto::CMessageGetBlocks>("getblocks", &Connection::onGetBlocks);
        break;
      case MessageTy::getdata :
        result = callHandler<BC::Proto::CMessageGetData>("getdata", &Connection::onGetData);
        break;

      // Special handlers
      case MessageTy::block :
        result = callHandler<BC::Proto::CMessageBlock>("block", &Connection::onBlock);
        break;
      case MessageTy::headers :
        result = callHandler<BC::Proto::CMessageHeaders>("headers", &Connection::onHeaders);
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

  void onAddr(BC::Proto::CMessageAddr &addr) {
    Handler_.onAddr(this, addr);
  }

  void onGetAddr() {
    Handler_.onGetAddr(this);
  }

  void onGetHeaders(BC::Proto::CMessageGetHeaders &getheaders) {
    Handler_.onGetHeaders(this, getheaders);
  }

  void onInv(BC::Proto::CMessageInv &inv) {
    Handler_.onInv(this, inv);
  }

  void onPing(BC::Proto::CMessagePing &ping) {
    SmallStream<1024> localStream;
    BC::Proto::CMessagePong pong;
    pong.Nonce = ping.Nonce;
    BC::serialize(localStream, pong);
    sendMessage(MessageTy::pong, localStream.data(), localStream.sizeOf());
    Handler_.onPing(this);
  }

  void onPong(BC::Proto::CMessagePong &pong) {
    auto It = PingMap_.find(pong.Nonce);
    if (It != PingMap_.end()) {
      auto pingTime = It->second;
      auto now = std::chrono::steady_clock::now();
      PingMap_.erase(It);
      Handler_.onPong(this, std::chrono::duration_cast<std::chrono::milliseconds>(now - pingTime).count());
    }
  }

  void onReject(BC::Proto::CMessageReject &reject) {
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

  void onVersion(BC::Proto::CMessageVersion &version) {
    StartHeight_ = version.StartHeight;
    ProtocolVersion_ = version.Version;
    Services_ = version.Services;
    UserAgent_ = version.UserAgent;

    VersionReceived_ = true;
    if (!IsConnected_ && (VersionReceived_ & VerackReceived_)) {
      IsConnected_ = true;
      Handler_.onConnect(this);
      ping();
    }

    sendMessage(MessageTy::verack, nullptr, 0);
  }

  void onGetBlocks(BC::Proto::CMessageGetBlocks &getblocks) {
    Handler_.onGetBlocks(this, getblocks);
  }

  void onGetData(BC::Proto::CMessageGetData &getdata) {
    Handler_.onGetData(this, getdata);
  }

  void onBlock(BC::Proto::CMessageBlock &block) {
    Handler_.onBlock(this, block);
  }

  void onHeaders(BC::Proto::CMessageHeaders &headers) {
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
