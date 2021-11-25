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
  using TxHashTy = ::uint256;
  using AddressTy = ::uint160;

  enum class ServicesTy : uint64_t {
    Network = 1,
    GetUTXO = 2,
    Bloom = 4,
    Witness = 8,
    NetworkLimited = 1024
  };

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

  struct CTxInInfoLink {};

  struct CTxValidationData {
    xvector<CTxInInfoLink> TxIns;
    xvector<bool> ScriptSigValid;
  };

  struct ValidationData {
    uint64_t HasWitness : 1;
    uint64_t TxAmoutValidated : 1;
    xvector<CTxValidationData> TxData;
  };

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
  };

  struct TxOut {
    int64_t value;
    xvector<uint8_t> pkScript;
  };

  struct TxWitness {
    std::vector<uint8_t> data;
  };

  template<typename T>
  struct BlockHeaderNetTy {
    typename T::BlockHeader header;
  };

  struct Transaction {
    int32_t version;
    xvector<TxIn> txIn;
    xvector<TxOut> txOut;
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

    BlockHashTy getTxId() const;
    BlockHashTy getWTxid() const;
  };

  template<typename T>
  struct BlockTy {
    typename T::BlockHeader header;
    xvector<typename T::Transaction> vtx;
    // Memory only
    mutable ValidationData validationData;
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
      MSG_WITNESS_FLAG = 1 << 30,

      ERROR = 0,
      MSG_TX = 1,
      MSG_BLOCK = 2,
      MSG_FILTERED_BLOCK = 3,
      MSG_CMPCT_BLOCK = 4,
      MSG_WITNESS_BLOCK = MSG_BLOCK | MSG_WITNESS_FLAG,
      MSG_WITNESS_TX = MSG_TX | MSG_WITNESS_FLAG,
      MSG_FILTERED_WITNESS_BLOCK = MSG_FILTERED_BLOCK | MSG_WITNESS_FLAG
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
  using MessageHeaders = MessageHeadersTy<BTC::Proto>;
  using MessageBlock = Block;
};
}

namespace BTC {

// Header
template<> struct Io<Proto::BlockHeader> {
  static inline size_t getSerializedSize(const BTC::Proto::BlockHeader&) { return 80; }
  static inline size_t getUnpackedExtraSize(xmstream &src) {
    src.seek(80);
    return 0;
  }
  static void serialize(xmstream &dst, const BTC::Proto::BlockHeader &data);
  static void unserialize(xmstream &src, BTC::Proto::BlockHeader &data);
  static inline void unpack2(xmstream &src, Proto::BlockHeader *dst, uint8_t **) { unserialize(src, *dst); }
};

// TxIn
template<> struct Io<Proto::TxIn> {
  static inline size_t getSerializedSize(const BTC::Proto::TxIn &data);
  static inline size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const BTC::Proto::TxIn &data);
  static void unserialize(xmstream &src, BTC::Proto::TxIn &data);
  static void unpack2(xmstream &src, Proto::TxIn *data, uint8_t **extraData);
};

// TxOut
template<> struct Io<Proto::TxOut> {
  static inline size_t getSerializedSize(const BTC::Proto::TxOut &data);
  static inline size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const BTC::Proto::TxOut &data);
  static void unserialize(xmstream &src, BTC::Proto::TxOut &data);
  static void unpack2(xmstream &src, Proto::TxOut *data, uint8_t **extraData);
};

// Transaction
template<> struct Io<Proto::Transaction> {
  static size_t getSerializedSize(const BTC::Proto::Transaction &data, bool serializeWitness=true);
  static size_t getUnpackedExtraSize(xmstream &src);
  static void serialize(xmstream &dst, const BTC::Proto::Transaction &data, bool serializeWitness=true);
  static void unserialize(xmstream &src, BTC::Proto::Transaction &data);
  static void unpack2(xmstream &src, Proto::Transaction *data, uint8_t **extraData);
};

// Block
template<typename T> struct Io<Proto::BlockTy<T>> {
  static inline size_t getSerializedSize(const BTC::Proto::BlockTy<T> &data, bool serializeWitness=true) {
    size_t size = Io<decltype(data.header)>::getSerializedSize(data.header) + getSerializedVarSizeSize(data.vtx.size());
    for (const auto &tx: data.vtx)
      size += Io<typename T::Transaction>::getSerializedSize(tx, serializeWitness);
    return size;
  }

  static inline size_t getUnpackedExtraSize(xmstream &src) {
    return Io<decltype(Proto::BlockTy<T>::header)>::getUnpackedExtraSize(src) +
           Io<decltype(Proto::BlockTy<T>::vtx)>::getUnpackedExtraSize(src);
  }

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

  static inline void unpack2(xmstream &src, Proto::BlockTy<T> *data, uint8_t **extraData) {
    BTC::Io<decltype(data->header)>::unpack2(src, &data->header, extraData);
    BTC::Io<decltype(data->vtx)>::unpack2(src, &data->vtx, extraData);
  }
};

// Network messages
// Network address
template<> struct Io<Proto::NetworkAddress> {
  static void serialize(xmstream &dst, const BTC::Proto::NetworkAddress &data);
  static void unserialize(xmstream &src, BTC::Proto::NetworkAddress &data);
};

template<> struct Io<Proto::NetworkAddressWithoutTime> {
  static void serialize(xmstream &dst, const BTC::Proto::NetworkAddressWithoutTime &data);
  static void unserialize(xmstream &src, BTC::Proto::NetworkAddressWithoutTime &data);
};

// Block header network message
template<typename T> struct Io<Proto::BlockHeaderNetTy<T>> {
  static inline size_t getUnpackedExtraSize(xmstream &src) {
    uint64_t txNum;
    size_t result = Io<decltype(Proto::BlockHeaderNetTy<T>::header)>::getUnpackedExtraSize(src);
    BTC::unserializeVarSize(src, txNum);
    return result;
  }

  static inline void serialize(xmstream &dst, const BTC::Proto::BlockHeaderNetTy<T> &data) {
    BTC::serialize(dst, data.header);
    BTC::serializeVarSize(dst, 0);
  }

  static inline void unserialize(xmstream &src, BTC::Proto::BlockHeaderNetTy<T> &data) {
    uint64_t txNum;
    BTC::unserialize(src, data.header);
    BTC::unserializeVarSize(src, txNum);
  }

  static inline void unpack2(xmstream &src, Proto::BlockHeaderNetTy<T> *data, uint8_t **extraData) {
    uint64_t txNum;
    Io<decltype(Proto::BlockHeaderNetTy<T>::header)>::unpack2(src, &data->header, extraData);
    BTC::unserializeVarSize(src, txNum);
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
  static inline size_t getUnpackedExtraSize(xmstream &src) {
    return Io<decltype(Proto::MessageHeadersTy<T>::headers)>::getUnpackedExtraSize(src);
  }

  static inline void serialize(xmstream &dst, const BTC::Proto::MessageHeadersTy<T> &data) {
    BTC::serialize(dst, data.headers);
  }

  static inline void unserialize(xmstream &src, BTC::Proto::MessageHeadersTy<T> &data) {
    BTC::unserialize(src, data.headers);
  }

  static inline void unpack2(xmstream &src, Proto::MessageHeadersTy<T> *data, uint8_t **extraData) {
    Io<decltype(Proto::MessageHeadersTy<T>::headers)>::unpack2(src, &data->headers, extraData);
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
void serializeJsonInside(xmstream &stream, const BTC::Proto::BlockHeader &header);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxIn &txin);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxOut &txout);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::Transaction &data);

std::string makeHumanReadableAddress(uint8_t pubkeyAddressPrefix, const BTC::Proto::AddressTy &address);
bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, BTC::Proto::AddressTy &address);
