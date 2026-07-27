#pragma once

#include <stdint.h>
#include <string.h>
#include <string>
#include "serialize.h"
#include "common/baseBlob.h"
#include "common/endiantools.h"
#include "hash.h"
#include "common/smallStream.h"
#include "common/uint.h"
#include <vector>
#include "../loguru.hpp"

namespace BTC {
class Proto {
public:
  using BlockHashTy = ::BaseBlob<256>;
  using TxHashTy = ::BaseBlob<256>;
  using AddressTy = ::BaseBlob<160>;
  using PrivateKeyTy = ::BaseBlob<256>;

  enum class ServicesTy : uint64_t {
    Network = 1,
    GetUTXO = 2,
    Bloom = 4,
    Witness = 8,
    NetworkLimited = 1024
  };

struct NetworkAddressWithoutTime {
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

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.services);
    op.raw(d.ipv6);
    op.io(d.port);
  }
};

struct NetworkAddress {
  uint32_t time;
  NetworkAddressWithoutTime addr;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.time);
    op.io(d.addr);
  }
};

  // struct CTxInInfoLink {};

  // struct CTxValidationData {
  //   xvector<CTxInInfoLink> TxIns;
  //   xvector<bool> ScriptSigValid;
  // };

  // struct ValidationData {
  //   uint64_t HasWitness : 1;
  //   uint64_t TxAmoutValidated : 1;
  //   xvector<CTxValidationData> TxData;
  // };

#pragma pack(push, 1)
  struct BlockHeader {
    int32_t nVersion;
    BaseBlob<256> hashPrevBlock;
    BaseBlob<256> hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;

    BlockHashTy GetHash() const {
      return sha256d(this, sizeof(*this));
    }

    UInt<256> GetHashAsInteger() const {
      return sha256dInt(this, sizeof(*this));
    }

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      static_assert(std::is_same_v<std::remove_cv_t<Self>, BlockHeader>);
      op.io(d.nVersion);
      op.io(d.hashPrevBlock);
      op.io(d.hashMerkleRoot);
      op.io(d.nTime);
      op.io(d.nBits);
      op.io(d.nNonce);
    }
  };
#pragma pack(pop)

  // GetHash hashes the object as raw bytes: layout must stay equal to the wire format
  static_assert(sizeof(BlockHeader) == 80);
  static_assert(std::is_standard_layout_v<BlockHeader>);

  struct TxIn {
    TxHashTy previousOutputHash;
    uint32_t previousOutputIndex;
    xvector<uint8_t> scriptSig;
    xvector<xvector<uint8_t>> witnessStack;
    uint32_t sequence;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.previousOutputHash);
      op.io(d.previousOutputIndex);
      op.io(d.scriptSig);
      // witnessStack: written by the transaction
      op.io(d.sequence);
    }
  };

  struct TxOut {
    int64_t value;
    xvector<uint8_t> pkScript;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.value);
      op.io(d.pkScript);
    }
  };

  struct TxWitness {
    std::vector<uint8_t> data;
  };

  template<typename T>
  struct BlockHeaderNetTy {
    typename T::BlockHeader header;
    // Transaction count of the wire form, a headers entry always carries 0
    VarSize txNum;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.header);
      op.io(d.txNum);
    }
  };

  struct Transaction {
    int32_t version;
    xvector<TxIn> txIn;
    xvector<TxOut> txOut;
    uint32_t lockTime;

    bool hasWitness() const {
      for (size_t i = 0; i < txIn.size(); i++) {
        if (!txIn[i].witnessStack.empty())
          return true;
      }

      return false;
    }

    BlockHashTy getTxId() const;
    BlockHashTy getWTxid() const;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool serializeWitness = true) {
      op.io(d.version);
      if constexpr (Op::Writing) {
        // segwit: marker and flag ahead of the inputs, witness stacks between the outputs
        // and lockTime
        bool witness = d.hasWitness() && serializeWitness;
        if (witness) {
          op.put(static_cast<uint8_t>(0));
          op.put(static_cast<uint8_t>(1));
        }
        op.io(d.txIn);
        op.io(d.txOut);
        if (witness) {
          for (size_t i = 0; i < d.txIn.size(); i++)
            op.io(d.txIn[i].witnessStack);
        }
      } else {
        // an empty input list is the segwit marker: the flag byte follows, then the real lists
        uint8_t flags = 0;
        size_t txInCount = op.vec(d.txIn);
        if (txInCount == 0) {
          op.get(flags);
          if (flags != 0) {
            txInCount = op.vec(d.txIn);
            op.vec(d.txOut);
          }
        } else {
          op.vec(d.txOut);
        }

        if (flags & 1) {
          flags ^= 1;
          // the marker with every witness stack empty must have been serialized without
          // the marker: reject, as Core does
          bool anyWitness = false;
          for (size_t i = 0; i < txInCount; i++)
            op.element(d.txIn, i, [&](auto &in) { anyWitness |= op.vec(in.witnessStack) != 0; });
          if (!anyWitness) {
            op.check(false);
            return;
          }
        }

        if (flags) {
          op.check(false);
          return;
        }
      }
      op.io(d.lockTime);
    }
  };

  template<typename T>
  struct BlockTy {
    typename T::BlockHeader header;
    xvector<typename T::Transaction> vtx;
    // Memory only
    // mutable ValidationData validationData;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool serializeWitness = true) {
      op.io(d.header, serializeWitness);
      op.vec(d.vtx, serializeWitness);
    }
  };

  struct CTxLinkedOutputs {
    xvector<xvector<uint8_t>> TxIn;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.TxIn);
    }
  };

  struct CBlockLinkedOutputs {
    bool AllOutputsFound = false;
    xvector<CTxLinkedOutputs> Tx;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      // AllOutputsFound is memory only
      op.io(d.Tx);
    }
  };

  struct CTxInValidationData {
    bool ScriptSigKnownValid;
  };

  struct CTxValidationData {
    xvector<CTxInValidationData> ScriptSigKnownValid;
  };

  struct CBlockValidationData {
    bool HasWitnessData = false;
    xvector<CTxValidationData> TxData;
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

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      static_assert(std::is_same_v<std::remove_cv_t<Self>, MessageVersion>);
      op.io(d.version);
      op.io(d.services);
      op.io(d.timestamp);
      op.io(d.addr_recv);
      if (d.version >= 106) {
        op.io(d.addr_from);
        op.io(d.nonce);
        op.io(d.user_agent);
        op.io(d.start_height);
        if (d.version >= 70001)
          op.io(d.relay);
      }
    }
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
    BaseBlob<256> hash;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.type);
      op.io(d.hash);
    }
  };

  // Template messages
  template<typename T>
  struct MessageHeadersTy {
    xvector<BlockHeaderNetTy<T>> headers;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.headers);
    }
  };

  // BTC messages
  struct MessagePing {
    uint64_t nonce;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.nonce);
    }
  };

  struct MessagePong {
    uint64_t nonce;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.nonce);
    }
  };

  struct MessageAddr {
    xvector<NetworkAddress> addr_list;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.addr_list);
    }
  };

  struct MessageGetHeaders {
    uint32_t version;
    xvector<BaseBlob<256>> BlockLocatorHashes;
    BaseBlob<256> HashStop;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.version);
      op.io(d.BlockLocatorHashes);
      op.io(d.HashStop);
    }
  };

  struct MessageGetBlocks {
    uint32_t version;
    xvector<BaseBlob<256>> BlockLocatorHashes;
    BaseBlob<256> HashStop;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.version);
      op.io(d.BlockLocatorHashes);
      op.io(d.HashStop);
    }
  };

  struct MessageInv {
    xvector<InventoryVector> Inventory;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Inventory);
    }
  };

  struct MessageGetData {
    xvector<InventoryVector> inventory;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.inventory);
    }
  };

  struct MessageReject {
    std::string message;
    int8_t ccode;
    std::string reason;
    uint8_t data[32];

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.message);
      op.io(d.ccode);
      op.io(d.reason);
      // TODO: serialize data
    }
  };

  using BlockHeaderNet = BlockHeaderNetTy<BTC::Proto>;
  using Block = BlockTy<BTC::Proto>;
  using MessageHeaders = MessageHeadersTy<BTC::Proto>;
  using MessageBlock = Block;
};
}

namespace BTC {

// Not part of the Io contract: the input being signed is replaced by the utxo it spends
void serializeForSignature(xmstream &dst, const BTC::Proto::TxIn &data, const uint8_t *utxo, size_t utxoSize);
void serializeForSignature(xmstream &dst,
                           const BTC::Proto::Transaction &data,
                           size_t targetInput,
                           const uint8_t *utxo,
                           size_t utxoSize);

// Where each transaction lies inside the serialized block, derived from the block itself: the
// header size and the transaction count give the offset of the first one, every next offset is
// the previous plus that transaction's size. Exact because unserializeVarSize rejects non
// minimal encodings, so no stored block can be non canonical; the sum is checked against the
// stored size to make sure of it.
struct CTxPosition {
  uint32_t Offset;
  uint32_t Size;
};

// What a coin writes after the transaction list, LTC's MWEB extension block being the only one
template<typename BlockTy>
static inline size_t blockExtensionSize(const BlockTy &block)
{
  if constexpr (requires { BlockTy::extensionSize(block); })
    return BlockTy::extensionSize(block);
  else
    return 0;
}

template<typename BlockTy>
static inline bool enumerateTransactions(const BlockTy &block,
                                        uint32_t storedSize,
                                        std::vector<CTxPosition> &out)
{
  using HeaderTy = std::remove_cvref_t<decltype(block.header)>;
  using TransactionTy = std::remove_cvref_t<decltype(block.vtx[0])>;

  size_t offset = Io<HeaderTy>::getSerializedSize(block.header) +
                  getSerializedVarSizeSize(block.vtx.size());
  out.resize(block.vtx.size());
  for (size_t i = 0; i < block.vtx.size(); i++) {
    size_t size = Io<TransactionTy>::getSerializedSize(block.vtx[i], true);
    out[i] = {static_cast<uint32_t>(offset), static_cast<uint32_t>(size)};
    offset += size;
  }

  return offset + blockExtensionSize(block) == storedSize;
}

}

// For HTTP API
void serializeJsonInside(xmstream &stream, const BTC::Proto::BlockHeader &header);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxIn &txin);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::TxOut &txout);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::Transaction &data);

std::string encodeBase58WithCrc(const uint8_t *prefix, unsigned prefixSize, const uint8_t *address, unsigned addressSize);
bool decodeBase58WithCrc(const std::string &base58, const uint8_t *prefix, unsigned prefixSize, uint8_t *address, unsigned addressSize);
std::string makeHumanReadableAddress(uint8_t pubkeyAddressPrefix, const BTC::Proto::AddressTy &address);
bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, BTC::Proto::AddressTy &address);
