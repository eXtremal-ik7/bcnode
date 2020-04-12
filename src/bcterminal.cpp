// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "BC/bc.h"
#include "BC/connection.h"

#include <asyncio/asyncio.h>
#include <asyncio/socket.h>
#include <p2putils/uriParse.h>

#include <stdarg.h>
#include <inttypes.h>
#include <getopt.h>
#include <mutex>
#include <string>
#include <thread>

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <termios.h>
#endif

enum CmdLineOptsTy {
  clOptHelp = 1,
  clMagic
};

static option cmdLineOpts[] = {
  {"help", no_argument, nullptr, clOptHelp},

  {"magic", required_argument, nullptr, clMagic},
  {nullptr, 0, nullptr, 0}
};

static bool lookupPeer(const char *address, HostAddress *out, uint16_t defaultPort)
{
    URI uri;
    std::string fakeUrl = "http://";
    fakeUrl.append(address);
    if (!uriParse(fakeUrl.c_str(), &uri)) {
      return false;
    }

    uint16_t port = uri.port ? static_cast<uint16_t>(uri.port) : defaultPort;
    if (!uri.domain.empty()) {
      struct hostent *host = gethostbyname(uri.domain.c_str());
      if (host) {
        struct in_addr **hostAddrList = reinterpret_cast<struct in_addr**>(host->h_addr_list);
        out->ipv4 = hostAddrList[0]->s_addr;
        out->port = htons(port);
        out->family = AF_INET;
      } else {
        return false;
      }
    } else if (uri.ipv4) {
      out->ipv4 = uri.ipv4;
      out->port = htons(port);
      out->family = AF_INET;
    } else {
      return false;
    }

  return true;
}

class TextTerminal {
public:
  void onConnect(BC::Network::Connection<TextTerminal> *connection) {
    Connection_ = connection;
    write("Press ENTER for type command\n\n");
    write("Connected to %s; Start height: %u; User agent: %s\n", Address_.c_str(), connection->startHeight(), connection->userAgent().c_str());
  }

  void onDisconnect(BC::Network::Connection<TextTerminal>*) {
    write("Connection closed, press ENTER to exit terminal\n");
    Connected_ = false;
    postQuitOperation(Base_);
  }

  void onUnknownMessage(BC::Network::Connection<TextTerminal>*, const std::string &type) {
    write("Ignore message; type: %s\n", type.c_str());
  }

  void onAddr(BC::Network::Connection<TextTerminal>*, const BC::Proto::MessageAddr &addr) {
    for (const auto &address: addr.addr_list) {
      uint32_t ipv4;
      if (address.getIpv4(&ipv4)) {
        struct in_addr inaddr;
        inaddr.s_addr = ipv4;
        write("address: %s:%hu, services: %" PRIu64 "\n", inet_ntoa(inaddr), xbetoh(address.port), address.services);
      } else {
        write("address: ?ipv6:%hu, services: %" PRIu64 "\n", xbetoh(address.port), address.services);
      }
    }

    write("Total received: %zu peers\n", addr.addr_list.size());
  }

  void onGetAddr(BC::Network::Connection<TextTerminal>*) { write("message: getaddr\n"); }

  void onGetHeaders(BC::Network::Connection<TextTerminal>*, BC::Proto::MessageGetHeaders&) { write("message: getheaders\n"); }
  void onInv(BC::Network::Connection<TextTerminal>*, BC::Proto::MessageInv &msg) {
    write("inv: %zu elements\n", msg.Inventory.size());
    for (const auto &inv: msg.Inventory) {
      switch (inv.type) {
        case BC::Proto::InventoryVector::ERROR : {
          write("'error'\n");
          break;
        }

        case BC::Proto::InventoryVector::MSG_TX : {
          write("transaction %s\n", inv.hash.ToString().c_str());
          break;
        }

        case BC::Proto::InventoryVector::MSG_BLOCK : {
          write("block %s\n", inv.hash.ToString().c_str());
          break;
        }

        case BC::Proto::InventoryVector::MSG_CMPCT_BLOCK : {
          write("compact block %s\n", inv.hash.ToString().c_str());
          break;
        }

        case BC::Proto::InventoryVector::MSG_FILTERED_BLOCK : {
          write("filtered block %s\n", inv.hash.ToString().c_str());
          break;
        }
      }
    }
  }

  void onPing(BC::Network::Connection<TextTerminal>*) {}
  void onPong(BC::Network::Connection<TextTerminal>*, long latency) {
    write("Latency: %lims\n", latency);
  }

  void onReject(BC::Network::Connection<TextTerminal>*, BC::Proto::MessageReject&) { write("message: reject\n"); }

  void onGetBlocks(BC::Network::Connection<TextTerminal>*, BC::Proto::MessageGetBlocks&) { write("message: getblocks\n"); }
  void onGetData(BC::Network::Connection<TextTerminal>*, BC::Proto::MessageGetData&) { write("message: getdata\n"); }
  void onBlock(BC::Network::Connection<TextTerminal>*, BC::Proto::MessageBlock&) { write("message: block\n"); }
  void onHeaders(BC::Network::Connection<TextTerminal>*, BC::Proto::MessageHeaders&) { write("message: headers\n"); }

  void onInvalidMessageFormat(BC::Network::Connection<TextTerminal>*, const std::string &type) {
    write("Can't unserialize incoming message, type: %s\n", type.c_str());
  }

public:
  TextTerminal(asyncBase *base, const std::string &address) :
    Base_(base), Address_(address) {
    LineBuffer_.reset(new char[LineBufferSize]);
  }

  void close() {
    Connection_->close();
    Connected_ = false;
    postQuitOperation(Base_);
  }

  bool connected() { return Connected_; }

  int read(char *out, size_t size) {
    // Wait key press and lock terminal (use WinAPI and Posix API)
    char first = 0;
#ifdef WIN32
    DWORD mode;
    DWORD bytesRead;
    HANDLE stdInput = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(stdInput, &mode);
    SetConsoleMode(stdInput, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    ReadConsole(stdInput, &first, 1, &bytesRead, NULL);
    SetConsoleMode(stdInput, mode);
#else
    termios oldSettings;
    termios newSettings;
    tcgetattr(0, &oldSettings);
    newSettings = oldSettings;
    newSettings.c_lflag &= ~(ICANON | ECHO);
    newSettings.c_cc[VTIME] = 0;
    newSettings.c_cc[VMIN] = 1;
    tcsetattr(0, TCSANOW, &newSettings);
    while (getchar() != '\n')
      continue;
    tcsetattr(0, TCSANOW, &oldSettings);
#endif

    if (!Connected_)
      return 0;

    // Lock terminal until user press ENTER
    TerminalMutex_.lock();
    std::cout << "\nterminal> " << first;
    std::cin.getline(out, size);
    flush();
    TerminalMutex_.unlock();
    return 1;
  }

  void write(const char *fmt, ...) {
    if (TerminalMutex_.try_lock()) {
      flush();
      va_list args;
      va_start(args, fmt);
      vfprintf(stdout, fmt, args);
      TerminalMutex_.unlock();
    } else {
      std::lock_guard lock(BufferMutex_);
      va_list args;
      va_start(args, fmt);
      vsnprintf(LineBuffer_.get(), LineBufferSize, fmt, args);
      TerminalBuffer_.write(LineBuffer_.get(), strlen(LineBuffer_.get()));
    }
  }

  void flush() {
    std::lock_guard lock(BufferMutex_);
    fwrite(TerminalBuffer_.data(), 1, TerminalBuffer_.sizeOf(), stdout);
    TerminalBuffer_.reset();
  }

  void ping() {
    Connection_->ping();
  }

  void getaddr() {
    Connection_->getaddr();
  }

private:
  static constexpr size_t LineBufferSize = 1<<20;

  asyncBase *Base_;
  std::string Address_;
  BC::Network::Connection<TextTerminal> *Connection_;
  bool Connected_ = true;
  std::mutex TerminalMutex_;
  std::mutex BufferMutex_;
  std::unique_ptr<char[]> LineBuffer_;
  xmstream TerminalBuffer_;
};

static void inputThreadProc(TextTerminal &terminal)
{
  constexpr size_t InputSizeLimit = 1<<24;
  std::unique_ptr<char[]> inputBuffer(new char[InputSizeLimit]);
  for (;;) {
    terminal.read(inputBuffer.get(), InputSizeLimit);
    if (!terminal.connected())
      return;

    if (strcmp(inputBuffer.get(), "getaddr") == 0) {
      terminal.getaddr();
    } else if (strcmp(inputBuffer.get(), "ping") == 0) {
      terminal.ping();
    } else if (strcmp(inputBuffer.get(), "quit") == 0) {
      terminal.close();
      break;
    }
  }
}

void printHelpMessage(const char *name) {
  printf("Usage: %s <options> <address>\n", name);
  printf("Options:\n");
  printf("  --magic <hexadecimal number>: redefine magic field in bitcoin protocol messages\n");
  printf("\n");
  printf("Supported terminal commands:\n");
  printf("  getaddr: send 'getaddr' message, show returned peers\n");
  printf("  ping: send 'ping' message, show latency\n");
}

int main(int argc, char **argv)
{
  initializeSocketSubsystem();

  // Parsing command line
  int res;
  int index = 0;
  std::string addressString;
  uint32_t magic = 0;

  while ((res = getopt_long(argc, argv, "", cmdLineOpts, &index)) != -1) {
    switch (res) {
      case clOptHelp :
        printHelpMessage(argv[0]);
        return 0;
      case clMagic :
        magic = static_cast<uint32_t>(std::stoul(optarg, nullptr, 16));
        break;
      case ':' :
        fprintf(stderr, "Error: option %s missing argument\n", cmdLineOpts[index].name);
        break;
      case '?' :
        exit(1);
      default :
        break;
    }
  }

  if (optind == argc) {
    fprintf(stderr, "ERROR: You must specify node address\n");
    exit(1);
  }

  addressString = argv[optind];

  // Check network id
  BC::Common::ChainParams chainParams;
  if (!BC::Common::setupChainParams(&chainParams, "main")) {
    fprintf(stderr, "Unknown network: %s", "main");
    exit(1);
  }

  if (magic == 0)
    magic = chainParams.magic;

  HostAddress address;
  if (!lookupPeer(addressString.c_str(), &address, chainParams.DefaultPort)) {
    fprintf(stderr, "ERROR: Can't resolve %s\n", addressString.c_str());
    return 1;
  }

  asyncBase *base = createAsyncBase(amOSDefault);
  TextTerminal handler(base, addressString);
  BC::Network::Connection<TextTerminal> connection(handler, base, address, magic);
  connection.start();

  std::thread inputThread(inputThreadProc, std::ref(handler));
  asyncLoop(base);
  inputThread.join();
  return 0;
}
