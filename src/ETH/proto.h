#pragma once

#include "rlp.h"

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace ETH {

// bcnode's common types under their ETH names; the RLP leaves for them live in rlp.h
using CHash256 = BaseBlob<256>;
using CAddress = BaseBlob<160>;

// The 'to' of a transaction: an address, or nothing when the transaction creates a contract
struct CToAddress {
  CAddress Address;
  bool Create = false;
};

template<> struct Io<CToAddress> {
  static bool read(const CRlpValue &value, CToAddress &out) {
    if (value.IsList)
      return false;
    if (value.Size == 0) {
      out.Create = true;
      return true;
    }
    if (!rlpToBytes(value, out.Address.begin(), out.Address.size()))
      return false;
    out.Create = false;
    return true;
  }
};

// Transaction envelope types. A legacy transaction is an RLP list, everything since EIP-2718 is
// an RLP string whose first byte is the type.
enum ETxType {
  TxLegacy = 0,
  TxAccessList = 1,   // EIP-2930
  TxDynamicFee = 2,   // EIP-1559
  TxBlob = 3,         // EIP-4844
  TxSetCode = 4,      // EIP-7702
  TxTypeCount
};

// EIP-2718: splits either envelope into the type and the payload list
inline bool txEnvelope(const CRlpValue &tx, unsigned &type, CRlpValue &payload)
{
  if (tx.IsList) {
    type = TxLegacy;
    payload = tx;
    return true;
  }
  if (tx.Size < 2)
    return false;
  type = tx.Payload[0];
  return rlpDecode(tx.Payload + 1, tx.Size - 1, payload) && payload.IsList &&
         payload.RawSize == tx.Size - 1;
}

// The whole header. FieldCount tells the fork apart - 15 up to London, +baseFee 16,
// +withdrawalsRoot 17, +blobGasUsed/+excessBlobGas/+parentBeaconBlockRoot 20,
// +requestsHash 21 - and is the only valid presence test for the fork tail fields
struct CBlockHeader {
  CHash256 ParentHash;
  CHash256 UncleHash;
  CAddress Coinbase;
  CHash256 StateRoot;
  CHash256 TransactionsRoot;
  CHash256 ReceiptsRoot;
  BaseBlob<2048> LogsBloom;
  // Per-block difficulty fits 64 bits on every chain we care about; zero marks post-merge
  uint64_t Difficulty = 0;
  uint64_t Number = 0;
  uint64_t GasLimit = 0;
  uint64_t GasUsed = 0;
  uint64_t Timestamp = 0;
  xvector<uint8_t> ExtraData;
  CHash256 MixHash;             // prevRandao once PostMerge
  uint64_t Nonce = 0;
  uint64_t BaseFee = 0;         // London
  CHash256 WithdrawalsRoot;     // Shanghai
  uint64_t BlobGasUsed = 0;     // Cancun
  uint64_t ExcessBlobGas = 0;
  CHash256 ParentBeaconBlockRoot;
  CHash256 RequestsHash;        // Prague
  // Not a wire field: how many were on the wire, which is what tells the fork apart
  size_t FieldCount = 0;

  static constexpr const char *Name = "header";

  bool postMerge() const { return Difficulty == 0; }

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(d.ParentHash);
    op.io(d.UncleHash);
    op.io(d.Coinbase);
    op.io(d.StateRoot);
    op.io(d.TransactionsRoot);
    op.io(d.ReceiptsRoot);
    op.io(d.LogsBloom);
    op.io(d.Difficulty);
    op.io(d.Number);
    op.io(d.GasLimit);
    op.io(d.GasUsed);
    op.io(d.Timestamp);
    // Longer extraData exists (clique chains sign in it); on this data 32 is the consensus
    // cap. The size, not the member: on the measuring pass the vector is not filled.
    op.check(op.vec(d.ExtraData) <= 32, "extraData is over the mainnet cap");
    op.io(d.MixHash);
    op.fixedU64(d.Nonce);
    if (!op.atEnd())
      op.io(d.BaseFee);
    if (!op.atEnd())
      op.io(d.WithdrawalsRoot);
    if (!op.atEnd()) {
      op.io(d.BlobGasUsed);
      op.io(d.ExcessBlobGas);
      op.io(d.ParentBeaconBlockRoot);
    }
    if (!op.atEnd())
      op.io(d.RequestsHash);
    d.FieldCount = op.drain();
    op.check(d.FieldCount == 15 || d.FieldCount == 16 || d.FieldCount == 17 ||
             d.FieldCount == 20 || d.FieldCount == 21, "has an unexpected field count");
  }
};

// [address, [storageKey...]]
struct CAccessListEntry {
  CAddress Address;
  xvector<CHash256> StorageKeys;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(d.Address);
    op.io(d.StorageKeys);
    op.end();
  }
};

// [chainId, address, nonce, yParity, r, s], EIP-7702
struct CAuthorization {
  UInt<256> ChainId = UInt<256>::zero();   // zero means any chain
  CAddress Address;
  uint64_t Nonce = 0;
  uint64_t YParity = 0;
  UInt<256> R = UInt<256>::zero();
  UInt<256> S = UInt<256>::zero();

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(d.ChainId);
    op.io(d.Address);
    op.io(d.Nonce);
    op.io(d.YParity);
    // Not 0/1: a tuple with a nonsense signature is skipped at application time, not
    // invalid (EIP-7702) - mainnet carries such - so the wire bound is the field's uint8
    op.check(d.YParity <= 255, "authorization yParity does not fit a byte");
    op.io(d.R);
    op.io(d.S);
    op.end();
  }
};

// The payload of every known envelope; fields another type does not carry keep their zero
struct CTransaction {
  unsigned Type = TxLegacy;
  uint64_t ChainId = 0;         // legacy has no such field: v carries the chain, EIP-155
  uint64_t Nonce = 0;
  UInt<256> GasPrice = UInt<256>::zero();           // legacy, 2930
  UInt<256> MaxPriorityFeePerGas = UInt<256>::zero();  // 1559+
  UInt<256> MaxFeePerGas = UInt<256>::zero();
  uint64_t GasLimit = 0;
  CToAddress To;
  UInt<256> Value = UInt<256>::zero();
  xvector<uint8_t> Calldata;
  xvector<CAccessListEntry> AccessList;      // 2930+
  UInt<256> MaxFeePerBlobGas = UInt<256>::zero();   // 4844
  xvector<CHash256> BlobHashes;
  xvector<CAuthorization> Authorizations;    // 7702
  uint64_t V = 0;               // legacy v, or yParity of the typed envelopes
  UInt<256> R = UInt<256>::zero();
  UInt<256> S = UInt<256>::zero();

  template<typename Op, typename Self> static void io(Op &op, Self &d, unsigned type) {
    d.Type = type;
    if (type != TxLegacy)
      op.io(d.ChainId);
    op.io(d.Nonce);
    if (type == TxLegacy || type == TxAccessList) {
      op.io(d.GasPrice);
    } else {
      op.io(d.MaxPriorityFeePerGas);
      op.io(d.MaxFeePerGas);
    }
    op.io(d.GasLimit);
    op.io(d.To);
    if (type == TxBlob)
      op.check(!d.To.Create, "blob transaction creates a contract");
    op.io(d.Value);
    op.io(d.Calldata);
    if (type != TxLegacy)
      op.io(d.AccessList);
    if (type == TxBlob) {
      op.io(d.MaxFeePerBlobGas);
      // The count, not the member: on the measuring pass the vector is not filled
      op.check(op.vec(d.BlobHashes) != 0, "blob transaction without blobs");
    }
    if (type == TxSetCode)
      op.check(op.vec(d.Authorizations) != 0, "set-code transaction without authorizations");
    op.io(d.V);
    if (type != TxLegacy)
      op.check(d.V <= 1, "yParity is out of range");
    op.io(d.R);
    op.io(d.S);
    op.end();
  }
};

// The envelope is unwrapped here, so a vector of transactions reads as any other vector:
// an element is either the legacy list itself or a string holding type-then-payload
template<> struct Io<CTransaction> {
  template<typename Op>
  static void read(Op &op, const CRlpValue &value, CTransaction &out) {
    unsigned type;
    CRlpValue payload;
    if (!txEnvelope(value, type, payload)) {
      op.fail("transaction envelope is malformed");
      return;
    }
    if (type >= TxTypeCount) {
      op.fail("unknown transaction type");
      return;
    }
    op.read(payload, out, type);
  }
};

// [index, validatorIndex, address, amount], EIP-4895; not a transaction - the amount lands
// on the balance with no code run and no gas paid
struct CWithdrawal {
  uint64_t Index = 0;
  uint64_t ValidatorIndex = 0;
  CAddress Address;
  uint64_t AmountGwei = 0;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(d.Index);
    op.io(d.ValidatorIndex);
    op.io(d.Address);
    op.io(d.AmountGwei);
    op.end();
  }
};

// [transactions, uncles, withdrawals?]; every transaction decodes in full, an uncle is a
// complete header and walks as such
struct CBlockBody {
  xvector<CTransaction> Transactions;
  xvector<CBlockHeader> Uncles;
  xvector<CWithdrawal> Withdrawals;
  bool HasWithdrawals = false;    // Shanghai; presence must match the header's withdrawalsRoot

  static constexpr const char *Name = "body";

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(d.Transactions);
    op.io(d.Uncles);
    d.HasWithdrawals = !op.atEnd();
    if (d.HasWithdrawals)
      op.io(d.Withdrawals);
    op.end();
  }
};

// Status of a receipt: empty means failure, 0x01 success, and a pre-Byzantium receipt carries
// an intermediate state root where the status of later ones sits
enum EReceiptStatus {
  RsFailed = 0,
  RsSuccess,
  RsPostState
};

struct CReceiptStatus {
  EReceiptStatus Value = RsFailed;
  CHash256 PostState;           // meaningful for RsPostState only
};

template<> struct Io<CReceiptStatus> {
  static bool read(const CRlpValue &value, CReceiptStatus &out) {
    if (value.IsList)
      return false;
    if (value.Size == 0) {
      out.Value = RsFailed;
      return true;
    }
    if (value.Size == 32) {
      out.Value = RsPostState;
      return rlpToBytes(value, out.PostState.begin(), out.PostState.size());
    }
    if (value.Size == 1 && value.Payload[0] == 0x01) {
      out.Value = RsSuccess;
      return true;
    }
    return false;
  }
};

// [address, topics, data]
struct CLog {
  CAddress Address;
  xvector<CHash256> Topics;
  xvector<uint8_t> Data;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(d.Address);
    op.io(d.Topics);
    op.io(d.Data);
    op.end();
  }
};

// [tx-type, post-state-or-status, cumulative-gas, logs], go-ethereum core/types/receipt.go
struct CReceipt {
  uint64_t Type = 0;
  CReceiptStatus Status;
  uint64_t CumulativeGasUsed = 0;
  xvector<CLog> Logs;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(d.Type);
    op.io(d.Status);
    op.io(d.CumulativeGasUsed);
    op.io(d.Logs);
    op.end();
  }
};

// The receipts record of a block is a bare list of receipts, not a field inside one
struct CBlockReceipts {
  xvector<CReceipt> Receipts;

  static constexpr const char *Name = "receipts";

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.items(d.Receipts);
  }
};

// keccak256 of the header's own RLP - which is exactly the record as stored, so nothing is
// re-encoded here
CHash256 blockHash(const uint8_t *headerRlp, size_t size);

}
