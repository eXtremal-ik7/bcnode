#pragma once

#include <stdint.h>
#include <string.h>
#include <string>
#include "serialize.h"
#include "common/intrusive_ptr.h"
#include "common/uint256.h"
#include <openssl/sha.h>
#include "../loguru.hpp"


namespace BTC {
class Proto {
public:
  using BlockHashTy = ::uint256;
  using AddressTy = ::uint160;

struct NetworkAddress {
  uint32_t time;
  uint64_t services;
  union {
    uint8_t u8[16];
    uint32_t u32[4];
  } ipv6;

  // Port (network byte order)
  uint16_t port;

  static constexpr uint8_t ipv4mask[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

  void reset() {
    memset(ipv6.u8, 0, sizeof(ipv6));
  }

  bool getIpv4(uint32_t *ipv4) const {
    if (memcmp(ipv6.u8, ipv4mask, sizeof(ipv4mask)) == 0) {
      *ipv4 = ipv6.u32[3];
      return true;
    } else {
      return false;
    }
  }

  void setIpv4(uint32_t ipv4) {
    memcpy(ipv6.u8, ipv4mask, sizeof(ipv4mask));
    ipv6.u32[3] = ipv4;
  }
};

struct NetworkAddressWithoutTime : public NetworkAddress {};

#pragma pack(push, 1)
  struct BlockHeader {
    int32_t nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;

    BlockHashTy GetHash() const {
      uint256 result;
      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, this, sizeof(*this));
      SHA256_Final(result.begin(), &sha256);

      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(result));
      SHA256_Final(result.begin(), &sha256);
      return result;
    }
  };
#pragma pack(pop)

  struct TxIn {
    uint256 previousOutputHash;
    uint32_t previousOutputIndex;
    xvector<uint8_t> scriptSig;
    xvector<xvector<uint8_t>> witnessStack;
    uint32_t sequence;

    static bool unpackWitnessStack(xmstream &src, DynamicPtr<BTC::Proto::TxIn> dst) {
      BTC::unpack(src, DynamicPtr<decltype (dst->witnessStack)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::TxIn, witnessStack)));
      return !dst->witnessStack.empty();
    }

    void serialize(xmstream &stream) const;
    void unserialize(xmstream &stream);
    static void unpack(xmstream &stream, xmstream &out, size_t offset);
    static void unpackFinalize(xmstream &out, size_t offset);
  };

  struct TxOut {
    int64_t value;
    xvector<uint8_t> pkScript;

    void serialize(xmstream &stream) const;
    void unserialize(xmstream &stream);
    static void unpack(xmstream &stream, xmstream &out, size_t offset);
    static void unpackFinalize(xmstream &out, size_t offset);
  };

  struct TxWitness {
    std::vector<uint8_t> data;
  };

  template<typename T>
  struct BlockHeaderNetTy {
    typename T::BlockHeader header;
  };

  template<typename T>
  struct BlockTy {
    typename T::BlockHeader header;
    xvector<typename T::Transaction> vtx;
  };

  template<typename T>
  struct TransactionTy {
    int32_t version;
    xvector<typename T::TxIn> txIn;
    xvector<typename T::TxOut> txOut;
    uint32_t lockTime;

    // Memory only
    uint32_t SerializedDataOffset = 0;
    uint32_t SerializedDataSize = 0;

    bool hasWitness() const {
      for (size_t i = 0; i < txIn.size(); i++) {
        if (!txIn[i].witnessStack.empty())
          return true;
      }

      return false;
    }

    BlockHashTy GetHash() const {
      uint256 result;
      uint8_t buffer[4096];
      xmstream stream(buffer, sizeof(buffer));
      stream.reset();
      BTC::serialize(stream, *this);

      SHA256_CTX sha256;
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, stream.data(), stream.sizeOf());
      SHA256_Final(result.begin(), &sha256);
      SHA256_Init(&sha256);
      SHA256_Update(&sha256, result.begin(), sizeof(result));
      SHA256_Final(result.begin(), &sha256);
      return result;
    }
  };

struct MessageVersion {
  uint32_t version;
  uint64_t services;
  uint64_t timestamp;
  NetworkAddressWithoutTime addr_recv;
  NetworkAddressWithoutTime addr_from;
  uint64_t nonce;
  std::string user_agent;
  uint32_t start_height;
  bool relay;
};

struct InventoryVector {
  enum {
    ERROR = 0,
    MSG_TX = 1,
    MSG_BLOCK = 2,
    MSG_FILTERED_BLOCK = 3,
    MSG_CMPCT_BLOCK = 4
  };

  uint32_t type;
  uint256 hash;
};

// Template messages
template<typename T>
struct MessageHeadersTy {
  xvector<BlockHeaderNetTy<T>> headers;
};

// BTC messages
struct MessagePing {
  uint64_t nonce;
};

struct MessagePong {
  uint64_t nonce;
};

struct MessageAddr {
  xvector<NetworkAddress> addr_list;
};

struct MessageGetHeaders {
  uint32_t version;
  xvector<uint256> BlockLocatorHashes;
  uint256 HashStop;
};

struct MessageGetBlocks {
  uint32_t version;
  xvector<uint256> BlockLocatorHashes;
  uint256 HashStop;
};

struct MessageInv {
  enum {
    ERROR = 0,
    MSG_TX = 1,
    MSG_BLOCK = 2,
    MSG_FILTERED_BLOCK = 3,
    MSG_CMPCT_BLOCK = 4
  };

  xvector<InventoryVector> Inventory;
};

struct MessageGetData {
  xvector<InventoryVector> inventory;
};

struct MessageReject {
  std::string message;
  int8_t ccode;
  std::string reason;
  uint8_t data[32];
};

  using BlockHeaderNet = BlockHeaderNetTy<BTC::Proto>;
  using Block = BlockTy<BTC::Proto>;
  using Transaction = TransactionTy<BTC::Proto>;
  using MessageHeaders = MessageHeadersTy<BTC::Proto>;
  using MessageBlock = Block;
};
}

namespace BTC {

// Header
template<> struct Io<Proto::BlockHeader> {
  static void serialize(xmstream &dst, const BTC::Proto::BlockHeader &data);
  static void unserialize(xmstream &src, BTC::Proto::BlockHeader &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::BlockHeader> dst) { unserialize(src, *dst.ptr()); }
  static void unpackFinalize(DynamicPtr<BTC::Proto::BlockHeader>) {}
};

// TxIn
template<> struct Io<Proto::TxIn> {
  static void serialize(xmstream &dst, const BTC::Proto::TxIn &data);
  static void unserialize(xmstream &src, BTC::Proto::TxIn &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::TxIn> dst);
  static void unpackFinalize(DynamicPtr<BTC::Proto::TxIn> dst);
};

// TxOut
template<> struct Io<Proto::TxOut> {
  static void serialize(xmstream &dst, const BTC::Proto::TxOut &data);
  static void unserialize(xmstream &src, BTC::Proto::TxOut &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::TxOut> dst);
  static void unpackFinalize(DynamicPtr<BTC::Proto::TxOut> dst);
};

// Transaction
template<typename T> struct Io<Proto::TransactionTy<T>> {
  static inline void serialize(xmstream &dst, const BTC::Proto::TransactionTy<T> &data) {
    uint8_t flags = 0;
    BTC::serialize(dst, data.version);
    if (data.hasWitness()) {
      flags = 1;
      BTC::serializeVarSize(dst, 0);
      BTC::serialize(dst, flags);
    }

    BTC::serialize(dst, data.txIn);
    BTC::serialize(dst, data.txOut);

    if (flags) {
      for (size_t i = 0; i < data.txIn.size(); i++)
        BTC::serialize(dst, data.txIn[i].witnessStack);
    }

    BTC::serialize(dst, data.lockTime);
  }

  static inline void unserialize(xmstream &src, BTC::Proto::TransactionTy<T> &data) {
    uint8_t flags = 0;
    BTC::unserialize(src, data.version);
    BTC::unserialize(src, data.txIn);
    if (data.txIn.empty()) {
      BTC::unserialize(src, flags);
      if (flags != 0) {
        BTC::unserialize(src, data.txIn);
        BTC::unserialize(src, data.txOut);
      }
    } else {
      BTC::unserialize(src, data.txOut);
    }

    if (flags & 1) {
      flags ^= 1;

      for (size_t i = 0; i < data.txIn.size(); i++)
        BTC::unserialize(src, data.txIn[i].witnessStack);

      if (!data.hasWitness()) {
        src.seekEnd(0, true);
        return;
      }
    }

    if (flags) {
      src.seekEnd(0, true);
      return;
    }

    BTC::unserialize(src, data.lockTime);
  }

  static inline void unpack(xmstream &src, DynamicPtr<BTC::Proto::TransactionTy<T>> dst) {
    uint8_t flags = 0;

    BTC::unserialize(src, dst->version);
    BTC::unpack(src, DynamicPtr<decltype(dst->txIn)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txIn)));

    if (dst->txIn.empty()) {
      BTC::unserialize(src, flags);
      if (flags) {
        BTC::unpack(src, DynamicPtr<decltype(dst->txIn)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txIn)));
        BTC::unpack(src, DynamicPtr<decltype(dst->txOut)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txOut)));
      }
    } else {
      BTC::unpack(src, DynamicPtr<decltype(dst->txOut)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txOut)));
    }

    if (flags & 1) {
      flags ^= 1;

      bool hasWitness = false;
      for (size_t i = 0, ie = dst->txIn.size(); i < ie; i++) {
        size_t txInDataOffset = reinterpret_cast<size_t>(dst->txIn.data());
        size_t txInOffset = txInDataOffset + sizeof(typename T::TxIn)*i;
        hasWitness |= BTC::Proto::TxIn::unpackWitnessStack(src, DynamicPtr<Proto::TxIn>(dst.stream(), txInOffset));
      }

      if (!hasWitness) {
        src.seekEnd(0, true);
        return;
      }
    }

    if (flags) {
      src.seekEnd(0, true);
      return;
    }

    BTC::unserialize(src, dst->lockTime);
  }

  static inline void unpackFinalize(DynamicPtr<BTC::Proto::TransactionTy<T>> dst) {
    BTC::unpackFinalize(DynamicPtr<decltype(dst->txIn)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txIn)));
    BTC::unpackFinalize(DynamicPtr<decltype(dst->txOut)>(dst.stream(), dst.offset()+ offsetof(BTC::Proto::TransactionTy<T>, txOut)));
  }
};

// Block
template<typename T> struct Io<Proto::BlockTy<T>> {
  static inline void serialize(xmstream &dst, const BTC::Proto::BlockTy<T> &data) {
    BTC::serialize(dst, data.header);
    BTC::serialize(dst, data.vtx);
  }

  static inline void unserialize(xmstream &src, BTC::Proto::BlockTy<T> &data) {
    size_t blockDataOffset = src.offsetOf();

    BTC::unserialize(src, data.header);

    uint64_t txNum = 0;
    unserializeVarSize(src, txNum);
    if (txNum > src.remaining()) {
      src.seekEnd(0, true);
      return;
    }

    data.vtx.resize(txNum);
    for (uint64_t i = 0; i < txNum; i++) {
      data.vtx[i].SerializedDataOffset = static_cast<uint32_t>(src.offsetOf() - blockDataOffset);
      BTC::unserialize(src, data.vtx[i]);
      data.vtx[i].SerializedDataSize = static_cast<uint32_t>(src.offsetOf() - data.vtx[i].SerializedDataOffset);
    }
  }

  static inline void unpack(xmstream &src, DynamicPtr<BTC::Proto::BlockTy<T>> dst) {
    size_t blockDataOffset = src.offsetOf();
    BTC::unpack(src, DynamicPtr<decltype (dst->header)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));

    {
      auto vtxDst = DynamicPtr<decltype (dst->vtx)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, vtx));
      uint64_t txNum;
      unserializeVarSize(src, txNum);
      size_t dataOffset = vtxDst.stream().offsetOf();
      vtxDst.stream().reserve(txNum*sizeof(typename T::Transaction));

      new (vtxDst.ptr()) xvector<typename T::Transaction>(reinterpret_cast<typename T::Transaction*>(dataOffset), txNum, false);
      for (uint64_t i = 0; i < txNum; i++) {
        auto txPtr = DynamicPtr<typename T::Transaction>(vtxDst.stream(), dataOffset + sizeof(typename T::Transaction)*i);
        txPtr->SerializedDataOffset = static_cast<uint32_t>(src.offsetOf() - blockDataOffset);
        BTC::unpack(src, txPtr);
        txPtr->SerializedDataSize = static_cast<uint32_t>(src.offsetOf() - txPtr->SerializedDataOffset);
      }
    }
  }

  static inline void unpackFinalize(DynamicPtr<BTC::Proto::BlockTy<T>> dst) {
    BTC::unpackFinalize(DynamicPtr<decltype (dst->header)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));
    BTC::unpackFinalize(DynamicPtr<decltype (dst->vtx)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, vtx)));
  }
};

// Network messages
// Network address
template<> struct Io<Proto::NetworkAddress> {
  static void serialize(xmstream &dst, const BTC::Proto::NetworkAddress &data);
  static void unserialize(xmstream &src, BTC::Proto::NetworkAddress &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::NetworkAddress> dst) { unserialize(src, *dst.ptr()); }
  static void unpackFinalize(DynamicPtr<BTC::Proto::NetworkAddress>) {}
};

template<> struct Io<Proto::NetworkAddressWithoutTime> {
  static void serialize(xmstream &dst, const BTC::Proto::NetworkAddressWithoutTime &data);
  static void unserialize(xmstream &src, BTC::Proto::NetworkAddressWithoutTime &data);
  static void unpack(xmstream &src, DynamicPtr<BTC::Proto::NetworkAddressWithoutTime> dst) { unserialize(src, *dst.ptr()); }
  static void unpackFinalize(DynamicPtr<BTC::Proto::NetworkAddressWithoutTime>) {}
};

// Block header network message
template<typename T> struct Io<Proto::BlockHeaderNetTy<T>> {
  static inline void serialize(xmstream &dst, const BTC::Proto::BlockHeaderNetTy<T> &data) {
    BTC::serialize(dst, data.header);
    BTC::serializeVarSize(dst, 0);
  }

  static inline void unserialize(xmstream &src, BTC::Proto::BlockHeaderNetTy<T> &data) {
    uint64_t txNum;
    BTC::unserialize(src, data.header);
    BTC::unserializeVarSize(src, txNum);
  }

  static inline void unpack(xmstream &src, DynamicPtr<BTC::Proto::BlockHeaderNetTy<T>> dst) {
    uint64_t txNum;
    BTC::unpack(src, DynamicPtr<decltype (dst->header)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));
    BTC::unserializeVarSize(src, txNum);
  }

  static inline void unpackFinalize(DynamicPtr<BTC::Proto::BlockHeaderNetTy<T>> dst) {
    BTC::unpackFinalize(DynamicPtr<decltype (dst->header)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));
  }
};

// Inventory vector
template<> struct Io<Proto::InventoryVector> {
  static void serialize(xmstream &dst, const BTC::Proto::InventoryVector &data);
  static void unserialize(xmstream &src, BTC::Proto::InventoryVector &data);
};

  // Version
template<> struct Io<Proto::MessageVersion> {
  static void serialize(xmstream &dst, const BTC::Proto::MessageVersion &data);
  static void unserialize(xmstream &src, BTC::Proto::MessageVersion &data);
};

// Headers
template<typename T> struct Io<Proto::MessageHeadersTy<T>> {
  static inline void serialize(xmstream &dst, const BTC::Proto::MessageHeadersTy<T> &data) {
    BTC::serialize(dst, data.headers);
  }

  static inline void unserialize(xmstream &src, BTC::Proto::MessageHeadersTy<T> &data) {
    BTC::unserialize(src, data.headers);
  }

  static inline void unpack(xmstream &src, DynamicPtr<BTC::Proto::MessageHeadersTy<T>> dst) {
    BTC::unpack(src, DynamicPtr<decltype (dst->headers)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));
  }

  static inline void unpackFinalize(DynamicPtr<BTC::Proto::MessageHeadersTy<T>> dst) {
    BTC::unpackFinalize(DynamicPtr<decltype (dst->headers)>(dst.stream(), dst.offset() + offsetof(BTC::Proto::BlockTy<T>, header)));
  }
};

  // Ping
template<> struct Io<Proto::MessagePing> {
  static void serialize(xmstream &dst, const BTC::Proto::MessagePing &data);
  static void unserialize(xmstream &src, BTC::Proto::MessagePing &data);
};

// Pong
template<> struct Io<Proto::MessagePong> {
  static void serialize(xmstream &dst, const BTC::Proto::MessagePong &data);
  static void unserialize(xmstream &src, BTC::Proto::MessagePong &data);
};

// Addr
template<> struct Io<Proto::MessageAddr> {
  static void serialize(xmstream &dst, const BTC::Proto::MessageAddr &data);
  static void unserialize(xmstream &src, BTC::Proto::MessageAddr &data);
};

// GetHeaders
template<> struct Io<Proto::MessageGetHeaders> {
  static void serialize(xmstream &dst, const BTC::Proto::MessageGetHeaders &data);
  static void unserialize(xmstream &src, BTC::Proto::MessageGetHeaders &data);
};

// GetBlocks
template<> struct Io<Proto::MessageGetBlocks> {
  static void serialize(xmstream &dst, const BTC::Proto::MessageGetBlocks &data);
  static void unserialize(xmstream &src, BTC::Proto::MessageGetBlocks &data);
};

// Inv
template<> struct Io<Proto::MessageInv> {
  static void serialize(xmstream &dst, const BTC::Proto::MessageInv &data);
  static void unserialize(xmstream &src, BTC::Proto::MessageInv &data);
};

  // GetData
template<> struct Io<Proto::MessageGetData> {
  static void serialize(xmstream &dst, const BTC::Proto::MessageGetData &data);
  static void unserialize(xmstream &src, BTC::Proto::MessageGetData &data);
};

  // Reject
template<> struct Io<Proto::MessageReject> {
  static void serialize(xmstream &dst, const BTC::Proto::MessageReject &data);
  static void unserialize(xmstream &src, BTC::Proto::MessageReject &data);
};

}

// For HTTP API
template<typename T>
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TransactionTy<T> &data) {
  if (fieldName) {
    stream.write('\"');
    stream.write(fieldName, strlen(fieldName));
    stream.write("\":", 2);
  }

  stream.write('{');
  serializeJson(stream, "version", data.version); stream.write(',');
  serializeJson(stream, "txin", data.txIn); stream.write(',');
  serializeJson(stream, "txout", data.txOut); stream.write(',');
  serializeJson(stream, "lockTime", data.lockTime);
  stream.write('}');
}

void serializeJsonInside(xmstream &stream, const BTC::Proto::BlockHeader &header);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxIn &txin);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxOut &txout);

std::string makeHumanReadableAddress(uint8_t pubkeyAddressPrefix, const BTC::Proto::AddressTy &address);
bool decodeHumanReadableAddress(const std::string &hrAddress, uint8_t pubkeyAddressPrefix, BTC::Proto::AddressTy &address);
