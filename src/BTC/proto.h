#pragma once

#include <stdint.h>
#include <string.h>
#include <string>
#include "serialize.h"
#include "common/baseBlob.h"
#include "common/endiantools.h"
#include "hash.h"
#include "common/blockLayout.h"
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
  // For money bounded by coins actually held or created (balances, mined
  // totals): uint64 is enough while the supply in base units fits it
  using BalanceType = uint64_t;

  enum class EServices : uint64_t {
    Network = 1,
    GetUTXO = 2,
    Bloom = 4,
    Witness = 8,
    NetworkLimited = 1024
  };

struct CNetworkAddressWithoutTime {
  uint64_t Services;
  union {
    uint8_t U8[16];
    uint32_t U32[4];
  } Ipv6;

  // Port (network byte order)
  uint16_t Port;

  static constexpr uint8_t ipv4mask[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

  void reset() {
    memset(Ipv6.U8, 0, sizeof(Ipv6));
  }

  bool getIpv4(uint32_t *ipv4) const {
    if (memcmp(Ipv6.U8, ipv4mask, sizeof(ipv4mask)) == 0) {
      *ipv4 = Ipv6.U32[3];
      return true;
    } else {
      return false;
    }
  }

  void setIpv4(uint32_t ipv4) {
    memcpy(Ipv6.U8, ipv4mask, sizeof(ipv4mask));
    Ipv6.U32[3] = ipv4;
  }

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.Services);
    op.raw(d.Ipv6);
    op.io(d.Port);
  }
};

struct CNetworkAddress {
  uint32_t Time;
  CNetworkAddressWithoutTime Addr;

  template<typename Op, typename Self>
  static void io(Op &op, Self &d) {
    op.io(d.Time);
    op.io(d.Addr);
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
  struct CBlockHeader {
    int32_t Version;
    BaseBlob<256> HashPrevBlock;
    BaseBlob<256> HashMerkleRoot;
    uint32_t Time;
    uint32_t Bits;
    uint32_t Nonce;

    BlockHashTy GetHash() const {
      return sha256d(this, sizeof(*this));
    }

    UInt<256> GetHashAsInteger() const {
      return sha256dInt(this, sizeof(*this));
    }

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      static_assert(std::is_same_v<std::remove_cv_t<Self>, CBlockHeader>);
      op.io(d.Version);
      op.io(d.HashPrevBlock);
      op.io(d.HashMerkleRoot);
      op.io(d.Time);
      op.io(d.Bits);
      op.io(d.Nonce);
    }
  };
#pragma pack(pop)

  // GetHash hashes the object as raw bytes: layout must stay equal to the wire format
  static_assert(sizeof(CBlockHeader) == 80);
  static_assert(std::is_standard_layout_v<CBlockHeader>);

  struct CTxIn {
    TxHashTy PreviousOutputHash;
    uint32_t PreviousOutputIndex;
    xvector<uint8_t> ScriptSig;
    xvector<xvector<uint8_t>> WitnessStack;
    uint32_t Sequence;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.PreviousOutputHash);
      op.io(d.PreviousOutputIndex);
      op.io(d.ScriptSig);
      // WitnessStack: written by the transaction
      op.io(d.Sequence);
    }
  };

  struct CTxOut {
    int64_t Value;
    xvector<uint8_t> PkScript;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Value);
      op.io(d.PkScript);
    }
  };

  struct CTxWitness {
    std::vector<uint8_t> Data;
  };

  template<typename T>
  struct CBlockHeaderNetTy {
    typename T::CBlockHeader Header;
    // Transaction count of the wire form, a headers entry always carries 0
    VarSize TxNum;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Header);
      op.io(d.TxNum);
    }
  };

  struct CTransaction {
    int32_t Version;
    xvector<CTxIn> TxIn;
    xvector<CTxOut> TxOut;
    uint32_t LockTime;

    bool hasWitness() const {
      for (size_t i = 0; i < TxIn.size(); i++) {
        if (!TxIn[i].WitnessStack.empty())
          return true;
      }

      return false;
    }

    BlockHashTy getTxId() const;
    BlockHashTy getWTxid() const;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool serializeWitness = true) {
      op.io(d.Version);
      if constexpr (Op::Writing) {
        // segwit: marker and flag ahead of the inputs, witness stacks between the outputs
        // and LockTime
        bool witness = d.hasWitness() && serializeWitness;
        if (witness) {
          op.put(static_cast<uint8_t>(0));
          op.put(static_cast<uint8_t>(1));
        }
        op.io(d.TxIn);
        op.io(d.TxOut);
        if (witness) {
          for (size_t i = 0; i < d.TxIn.size(); i++)
            op.io(d.TxIn[i].WitnessStack);
        }
      } else {
        // an empty input list is the segwit marker: the flag byte follows, then the real lists
        uint8_t flags = 0;
        size_t txInCount = op.vec(d.TxIn);
        if (txInCount == 0) {
          op.get(flags);
          if (flags != 0) {
            txInCount = op.vec(d.TxIn);
            op.vec(d.TxOut);
          }
        } else {
          op.vec(d.TxOut);
        }

        if (flags & 1) {
          flags ^= 1;
          // the marker with every witness stack empty must have been serialized without
          // the marker: reject, as Core does
          bool anyWitness = false;
          for (size_t i = 0; i < txInCount; i++)
            op.element(d.TxIn, i, [&](auto &in) { anyWitness |= op.vec(in.WitnessStack) != 0; });
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
      op.io(d.LockTime);
    }
  };

  template<typename T>
  struct CBlockTy {
    typename T::CBlockHeader Header;
    xvector<typename T::CTransaction> Vtx;
    // Memory only
    // mutable ValidationData validationData;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d, bool serializeWitness = true) {
      op.io(d.Header, serializeWitness);
      op.vec(d.Vtx, serializeWitness);
    }
  };

  struct CTxLinkedOutputs {
    xvector<xvector<uint8_t>> TxIn;

    size_t memorySize() const {
      size_t size = TxIn.memoryBytes();
      for (const xvector<uint8_t> &txIn: TxIn)
        size += txIn.memoryBytes();
      return size;
    }

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.TxIn);
    }
  };

  struct CBlockLinkedOutputs {
    xvector<CTxLinkedOutputs> Tx;

    // One allocation per input, so the accounting has to walk it
    size_t memorySize() const {
      size_t size = Tx.memoryBytes();
      for (const CTxLinkedOutputs &tx: Tx)
        size += tx.memorySize();
      return size;
    }

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Tx);
    }
  };

  struct CTxInValidationData {
    bool ScriptSigKnownValid;
  };

  struct CTxValidationData {
    xvector<CTxInValidationData> ScriptSigKnownValid;

    size_t memorySize() const { return ScriptSigKnownValid.memoryBytes(); }
  };

  // Memory only, never serialized: per-block precomputed context. Every path
  // that reaches a database connect/disconnect runs validationDataInitialize
  // first, so consumers assert on it instead of recomputing
  struct CBlockValidationData {
    static constexpr uint32_t NoLocalTx = 0xFFFFFFFFu;

    bool HasWitnessData = false;
    bool InputsResolved = false;
    // The run proved the block spends what it may not: no lookup afterwards can make it valid
    bool InputsInvalid = false;
    // Same-block topology found what only an invalid block has: two inputs taking one output of
    // this block, or an input taking an output that does not exist. Found where the topology is
    // built, so the linking of a segment needs no per block bookkeeping for it
    bool LocalSpendInvalid = false;
    // This block repeats an earlier coinbase transaction: its outputs overwrite the twin's
    // coins, destroying them (ChainParams::BIP30Repeats). Set by the contextual check
    bool CoinbaseRepeat = false;
    // The block sits below BIP34, where nothing forbids a coinbase from repeating an
    // earlier one even if this particular block does not. Its coinbase outputs may land
    // on a live coin, so the utxo db must keep them overwrite-safe. Set by the contextual check
    bool CoinbaseMayRepeat = false;
    // txid of every transaction, parallel to block.Vtx ([0] = coinbase);
    // checkBlockStandalone verifies them against the header merkle root
    xvector<TxHashTy> TxIds;
    // Byte layout of the same transactions inside the stored block. Computed here
    // once because more than one database keeps positions instead of txids, and
    // walking the block again per database would parse it twice
    xvector<CDataSpan32> TxLayout;
    // Same-block spend topology, derived from TxIds. InputLocalTx: for every
    // input of vtx[1..] in block walk order, the index of the earlier tx of
    // this block whose output it spends (NoLocalTx otherwise).
    // OutputSpentLocally: bit per output of all txs in walk order, set when a
    // later tx of the same block spends it - such a pair is invisible outside
    // its block and never touches the utxo db or cache
    xvector<uint32_t> InputLocalTx;
    xvector<uint64_t> OutputSpentLocally;
    xvector<CTxValidationData> TxData;
    // Outputs parsed once, off the connect thread: serialized UnspentOutputInfo of every output
    // in walk order, empty record for an OP_RETURN one. Databases copy these bytes
    xvector<uint8_t> OutputData;
    xvector<uint32_t> OutputDataOffset;
    // Cross-block spend topology of one run: outputs of this block spent by a later block of the
    // same run, and the inputs spending them. The utxo db skips both sides like a same-block pair.
    // Honoured only while the two sides move together - the run connects as one operation, and a
    // disconnect that splits the pair puts the output back and drops the marks
    xvector<uint64_t> OutputSpentInBatch;
    xvector<uint64_t> InputSpendsInBatch;

    bool outputSpentLocally(size_t ordinal) const { return (OutputSpentLocally[ordinal >> 6] >> (ordinal & 63)) & 1u; }
    bool outputSpentInBatch(size_t ordinal) const {
      return !OutputSpentInBatch.empty() && ((OutputSpentInBatch[ordinal >> 6] >> (ordinal & 63)) & 1u);
    }
    bool inputSpendsInBatch(size_t ordinal) const {
      return !InputSpendsInBatch.empty() && ((InputSpendsInBatch[ordinal >> 6] >> (ordinal & 63)) & 1u);
    }
    // The pair holds only while both blocks are on the chain: the disconnect
    // that splits it puts the output back and drops the marks
    void dropPairs() {
      OutputSpentInBatch.resize(0);
      InputSpendsInBatch.resize(0);
    }

    // Built after the block is already in the block cache and outweighs it: the cache limit
    // means nothing unless this is charged to it too
    size_t memorySize() const {
      size_t size = TxIds.memoryBytes() + TxLayout.memoryBytes() + InputLocalTx.memoryBytes() + OutputSpentLocally.memoryBytes() +
                    TxData.memoryBytes() + OutputData.memoryBytes() + OutputDataOffset.memoryBytes() +
                    OutputSpentInBatch.memoryBytes() + InputSpendsInBatch.memoryBytes();
      for (const CTxValidationData &tx: TxData)
        size += tx.memorySize();
      return size;
    }

    // Parsed output record; size 0 means the output is not a utxo (OP_RETURN)
    const void *outputData(size_t ordinal, size_t &size) const {
      uint32_t begin = OutputDataOffset[ordinal];
      size = OutputDataOffset[ordinal + 1] - begin;
      return OutputData.begin() + begin;
    }
  };

  struct CMessageVersion {
    uint32_t Version;
    uint64_t Services;
    uint64_t Timestamp;
    CNetworkAddressWithoutTime AddrRecv;
    CNetworkAddressWithoutTime AddrFrom;
    uint64_t Nonce;
    std::string UserAgent;
    uint32_t StartHeight;
    bool Relay;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      static_assert(std::is_same_v<std::remove_cv_t<Self>, CMessageVersion>);
      op.io(d.Version);
      op.io(d.Services);
      op.io(d.Timestamp);
      op.io(d.AddrRecv);
      if (d.Version >= 106) {
        op.io(d.AddrFrom);
        op.io(d.Nonce);
        op.io(d.UserAgent);
        op.io(d.StartHeight);
        if (d.Version >= 70001)
          op.io(d.Relay);
      }
    }
  };

  struct CInventoryVector {
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

    uint32_t Type;
    BaseBlob<256> Hash;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Type);
      op.io(d.Hash);
    }
  };

  // Template messages
  template<typename T>
  struct CMessageHeadersTy {
    xvector<CBlockHeaderNetTy<T>> Headers;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Headers);
    }
  };

  // BTC messages
  struct CMessagePing {
    uint64_t Nonce;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Nonce);
    }
  };

  struct CMessagePong {
    uint64_t Nonce;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Nonce);
    }
  };

  struct CMessageAddr {
    xvector<CNetworkAddress> AddrList;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.AddrList);
    }
  };

  struct CMessageGetHeaders {
    uint32_t Version;
    xvector<BaseBlob<256>> BlockLocatorHashes;
    BaseBlob<256> HashStop;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Version);
      op.io(d.BlockLocatorHashes);
      op.io(d.HashStop);
    }
  };

  struct CMessageGetBlocks {
    uint32_t Version;
    xvector<BaseBlob<256>> BlockLocatorHashes;
    BaseBlob<256> HashStop;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Version);
      op.io(d.BlockLocatorHashes);
      op.io(d.HashStop);
    }
  };

  struct CMessageInv {
    xvector<CInventoryVector> Inventory;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Inventory);
    }
  };

  struct CMessageGetData {
    xvector<CInventoryVector> Inventory;

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Inventory);
    }
  };

  struct CMessageReject {
    std::string Message;
    int8_t Code;
    std::string Reason;
    uint8_t Data[32];

    template<typename Op, typename Self>
    static void io(Op &op, Self &d) {
      op.io(d.Message);
      op.io(d.Code);
      op.io(d.Reason);
      // TODO: serialize data
    }
  };

  using CBlockHeaderNet = CBlockHeaderNetTy<BTC::Proto>;
  using CBlock = CBlockTy<BTC::Proto>;
  using CMessageHeaders = CMessageHeadersTy<BTC::Proto>;
  using CMessageBlock = CBlock;
};
}

namespace BTC {
namespace Common {

// A block valid despite repeating an earlier coinbase transaction: BIP30 was not
// enforced when it was mined, so the repeat overwrites the earlier coin. Pinned by
// height AND hash (Core's IsBIP30Repeat) so no other block at the same height
// inherits the exemption; BIP34 made new ones impossible. The twin and the shared
// txid are pinned too: the databases keyed by txid hold one inclusion of such a
// transaction, and a query answers with both. Lives here so every chain's
// ChainParams can carry it
struct CBIP30Repeat {
  uint32_t Height;
  Proto::BlockHashTy Hash;
  // The earlier block, whose coins the repeat destroys
  uint32_t TwinHeight;
  Proto::BlockHashTy TwinHash;
  // Coinbase txid both blocks carry; the transactions are byte for byte the same
  Proto::TxHashTy TxId;
};

// The part of the validation context that follows from the block's place in the chain and
// not from its bytes. The contextual check is not the only caller: a block reloaded from
// disk for a disconnect or for a database catching up never runs one, and the databases
// have to undo exactly what they did. Bitcoin's answer is about the coinbase - the pinned
// repeats and the BIP34 threshold below which any coinbase may collide with an earlier one
template<typename BlockIndexTy, typename ChainParamsTy, typename ValidationDataTy>
static inline void fillChainContext(const BlockIndexTy &index,
                                    const ChainParamsTy &chainParams,
                                    ValidationDataTy &validation)
{
  // Height first: only the exempt blocks ever pay for the hash
  validation.CoinbaseRepeat = false;
  for (const auto &repeat: chainParams.BIP30Repeats) {
    if (index.Height == repeat.Height && index.Header.GetHash() == repeat.Hash) {
      validation.CoinbaseRepeat = true;
      break;
    }
  }

  validation.CoinbaseMayRepeat = index.Height < chainParams.BIP34Height;
}

}

// Not part of the Io contract: the input being signed is replaced by the utxo it spends
void serializeForSignature(xmstream &dst, const BTC::Proto::CTxIn &data, const uint8_t *utxo, size_t utxoSize);
void serializeForSignature(xmstream &dst,
                           const BTC::Proto::CTransaction &data,
                           size_t targetInput,
                           const uint8_t *utxo,
                           size_t utxoSize);

// Where each transaction lies inside the serialized block: the header size and the transaction
// count give the offset of the first one, every next offset is the previous plus that
// transaction's size. Exact because unserializeVarSize rejects non minimal encodings, so no
// stored block can be non canonical; txLayoutMatchesStored checks the sum against the stored size.
// Bitcoin's block layout, so it stays here - a coin that lays its block out differently fills
// the spans its own way
template<typename CBlockTy, typename VectorTy>
static inline void fillTxLayout(const CBlockTy &block, VectorTy &out)
{
  using HeaderTy = std::remove_cvref_t<decltype(block.Header)>;
  using TransactionTy = std::remove_cvref_t<decltype(block.Vtx[0])>;

  size_t offset = Io<HeaderTy>::getSerializedSize(block.Header) +
                  getSerializedVarSizeSize(block.Vtx.size());
  out.resize(block.Vtx.size());
  for (size_t i = 0; i < block.Vtx.size(); i++) {
    size_t size = Io<TransactionTy>::getSerializedSize(block.Vtx[i], true);
    out[i] = {static_cast<uint32_t>(offset), static_cast<uint32_t>(size)};
    offset += size;
  }
}

}

// For HTTP API
void serializeJsonInside(xmstream &stream, const BTC::Proto::CBlockHeader &header);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::CTxIn &txin);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::CTxOut &txout);
void serializeJson(xmstream &stream, const char *fieldName, const BTC::Proto::CTransaction &data);

std::string encodeBase58WithCrc(const uint8_t *prefix, unsigned prefixSize, const uint8_t *address, unsigned addressSize);
bool decodeBase58WithCrc(const std::string &base58, const uint8_t *prefix, unsigned prefixSize, uint8_t *address, unsigned addressSize);
std::string makeHumanReadableAddress(uint8_t pubkeyAddressPrefix, const BTC::Proto::AddressTy &address);
bool decodeHumanReadableAddress(const std::string &hrAddress, const std::vector<uint8_t> &pubkeyAddressPrefix, BTC::Proto::AddressTy &address);
