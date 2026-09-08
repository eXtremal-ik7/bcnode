#pragma once

#include "pb.h"

#include <vector>

#include <stddef.h>
#include <stdint.h>

namespace TRX {

// A TRON block id is not a plain hash: the leading 8 bytes are the height, big-endian, and
// only the remaining 24 come from sha256 of the header's raw_data. Which is why an id sorts
// by height, and why the block store can be walked in order.
BaseBlob<256> blockId(const uint8_t *rawData, size_t size, int64_t number);

// sha256 of whatever is handed over: the txid is taken of a transaction's raw_data, the
// merkle leaf of the entire serialized transaction. Not the same bytes, so not the same hash.
BaseBlob<256> sha256Of(const uint8_t *data, size_t size);

// The bytes a hash is taken over, found without decoding anything: a block id comes from the
// header's raw_data, a merkle leaf from a whole serialized transaction. Locating them here
// rather than storing spans in the model is what keeps the model to wire fields only, and it
// is the same split the ETH side has - the entity is data, the caller hashes the bytes.
bool blockHeaderRawData(const uint8_t *block, size_t size, const uint8_t *&raw, size_t &rawSize);
bool transactionMerkleLeaves(const uint8_t *block, size_t size, std::vector<BaseBlob<256>> &leaves);

// TRON's merkle tree, which is not Bitcoin's: one round of sha256 rather than two, and an odd
// node is promoted unchanged instead of being paired with itself. Consumes the level vector.
BaseBlob<256> merkleRoot(BaseBlob<256> *level, size_t count);

// Transaction.Contract.ContractType. Sparse on purpose - the gaps are numbers that were used
// during development and never made it to mainnet.
enum EContractType {
  AccountCreate = 0,
  Transfer = 1,
  TransferAsset = 2,
  VoteAsset = 3,
  VoteWitness = 4,
  WitnessCreate = 5,
  AssetIssue = 6,
  WitnessUpdate = 8,
  ParticipateAssetIssue = 9,
  AccountUpdate = 10,
  FreezeBalance = 11,
  UnfreezeBalance = 12,
  WithdrawBalance = 13,
  UnfreezeAsset = 14,
  UpdateAsset = 15,
  ProposalCreate = 16,
  ProposalApprove = 17,
  ProposalDelete = 18,
  SetAccountId = 19,
  Custom = 20,
  CreateSmartContract = 30,
  TriggerSmartContract = 31,
  GetContract = 32,
  UpdateSetting = 33,
  ExchangeCreate = 41,
  ExchangeInject = 42,
  ExchangeWithdraw = 43,
  ExchangeTransaction = 44,
  UpdateEnergyLimit = 45,
  AccountPermissionUpdate = 46,
  ClearABI = 48,
  UpdateBrokerage = 49,
  ShieldedTransfer = 51,
  MarketSellAsset = 52,
  MarketCancelOrder = 53,
  FreezeBalanceV2 = 54,
  UnfreezeBalanceV2 = 55,
  WithdrawExpireUnfreeze = 56,
  DelegateResource = 57,
  UnDelegateResource = 58,
  CancelAllUnfreezeV2 = 59,
  ContractTypeCount = 60
};

const char *contractTypeName(int32_t type);

// google.protobuf.Any, which is how a contract's payload is carried: a type url naming the
// message and its serialization, left undecoded here
struct CAny {
  xvector<uint8_t> TypeUrl;
  xvector<uint8_t> Value;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.TypeUrl);
    op.io(2, d.Value);
    op.end();
  }
};

// AccountId, and the authority that names one. Neither is used by anything on mainnet: the
// auths field predates the permission system that replaced it.
struct CAccountId {
  xvector<uint8_t> Name;
  xvector<uint8_t> Address;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.Name);
    op.io(2, d.Address);
    op.end();
  }
};

struct CAuthority {
  CAccountId Account;
  xvector<uint8_t> PermissionName;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.Account);
    op.io(2, d.PermissionName);
    op.end();
  }
};

// What a filled market order matched against
struct CMarketOrderDetail {
  xvector<uint8_t> MakerOrderId;
  xvector<uint8_t> TakerOrderId;
  int64_t FillSellQuantity = 0;
  int64_t FillBuyQuantity = 0;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.MakerOrderId);
    op.io(2, d.TakerOrderId);
    op.io(3, d.FillSellQuantity);
    op.io(4, d.FillBuyQuantity);
    op.end();
  }
};

// One entry of a protobuf map, which on the wire is nothing but a repeated two-field message.
// This is the only map in anything a block hashes, and protobuf-java keeps a parsed one in
// wire order rather than sorting it - which is the whole reason re-serializing a block
// reproduces its merkle leaf byte for byte.
struct CMapEntryStringInt64 {
  xvector<uint8_t> Key;
  int64_t Value = 0;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.Key);
    op.io(2, d.Value);
    op.end();
  }
};

// One contract inside a transaction. Mainnet only ever carries a single one - the field is
// repeated for a multi-contract extension that never happened.
struct CContract {
  int32_t Type = 0;
  CAny Parameter;
  xvector<uint8_t> Provider;
  xvector<uint8_t> ContractName;
  int32_t PermissionId = 0;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.Type);
    op.io(2, d.Parameter);
    op.io(3, d.Provider);
    op.io(4, d.ContractName);
    op.io(5, d.PermissionId);
    op.end();
  }
};

enum EResultCode {
  RcSuccess = 0,
  RcFailed = 1
};

// Transaction.Result as it rides inside a block, which is not TransactionInfo: the block
// carries the outcome, the receipt store carries the detail. Most of these fields belong to
// contract types that predate the split and are simply never set on a modern transaction.
struct CResult {
  // Fee is here on the wire but is never set inside a block: the actuator's fee goes to
  // TransactionInfo in the receipt store, and what a block keeps is contractRet. Reading it
  // over five million blocks sums to zero, which is the answer, not a bug.
  int64_t Fee = 0;
  int32_t Ret = 0;
  int32_t ContractRet = 0;
  // Field 4 belongs to no schema this repository has ever carried - the proto history for
  // Tron.proto starts in 2019-08 and it was already gone by then - yet a hundred and sixty
  // transactions from 2018 carry it, every one of them empty. Read as the byte string it is
  // on the wire, because a field that goes unread is a field the reader is guessing about.
  xvector<uint8_t> Legacy4;
  xvector<uint8_t> AssetIssueId;
  int64_t WithdrawAmount = 0;
  int64_t UnfreezeAmount = 0;
  int64_t ExchangeReceivedAmount = 0;
  int64_t ExchangeInjectAnotherAmount = 0;
  int64_t ExchangeWithdrawAnotherAmount = 0;
  int64_t ExchangeId = 0;
  int64_t ShieldedTransactionFee = 0;
  xvector<uint8_t> OrderId;
  xvector<CMarketOrderDetail> OrderDetails;
  int64_t WithdrawExpireAmount = 0;
  xvector<CMapEntryStringInt64> CancelUnfreezeV2Amount;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.Fee);
    op.io(2, d.Ret);
    op.io(3, d.ContractRet);
    op.io(4, d.Legacy4);
    op.io(14, d.AssetIssueId);
    op.io(15, d.WithdrawAmount);
    op.io(16, d.UnfreezeAmount);
    op.io(18, d.ExchangeReceivedAmount);
    op.io(19, d.ExchangeInjectAnotherAmount);
    op.io(20, d.ExchangeWithdrawAnotherAmount);
    op.io(21, d.ExchangeId);
    op.io(22, d.ShieldedTransactionFee);
    op.io(25, d.OrderId);
    op.io(26, d.OrderDetails);
    op.io(27, d.WithdrawExpireAmount);
    op.io(28, d.CancelUnfreezeV2Amount);
    op.end();
  }
};

struct CTransaction {
  // Transaction.raw: everything the sender signs. The txid is sha256 of exactly these bytes,
  // which is why signatures are not in here and cannot change it.
  struct CRaw {
    xvector<uint8_t> RefBlockBytes;
    int64_t RefBlockNum = 0;
    xvector<uint8_t> RefBlockHash;
    int64_t Expiration = 0;
    xvector<CAuthority> Auths;
    xvector<uint8_t> Data;
    xvector<CContract> Contracts;
    xvector<uint8_t> Scripts;
    int64_t Timestamp = 0;
    int64_t FeeLimit = 0;

    template<typename Op, typename Self> static void io(Op &op, Self &d) {
      op.io(1, d.RefBlockBytes);
      op.io(3, d.RefBlockNum);
      op.io(4, d.RefBlockHash);
      op.io(8, d.Expiration);
      op.io(9, d.Auths);
      op.io(10, d.Data);
      op.io(11, d.Contracts);
      op.io(12, d.Scripts);
      op.io(14, d.Timestamp);
      op.io(18, d.FeeLimit);
      op.end();
    }
  };

  CRaw RawData;
  xvector<xvector<uint8_t>> Signatures;
  xvector<CResult> Results;

  static constexpr const char *Name = "transaction";

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.required(1, d.RawData);
    op.io(2, d.Signatures);
    op.io(5, d.Results);
    op.end();
  }
};


struct CBlockHeader {
  // BlockHeader.raw. The block id is sha256 of these bytes, so witness_signature - which sits
  // outside - cannot change it, the same split as a transaction's.
  struct CRaw {
    int64_t Timestamp = 0;
    BaseBlob<256> TxTrieRoot;         // absent on a block that carries no transactions
    BaseBlob<256> ParentHash;
    int64_t Number = 0;
    int64_t WitnessId = 0;
    xvector<uint8_t> WitnessAddress;
    int32_t Version = 0;
    xvector<uint8_t> AccountStateRoot;     // a later fork; absent on everything before it

    template<typename Op, typename Self> static void io(Op &op, Self &d) {
      op.io(1, d.Timestamp);
      op.io(2, d.TxTrieRoot);
      op.io(3, d.ParentHash);
      op.io(7, d.Number);
      op.io(8, d.WitnessId);
      op.io(9, d.WitnessAddress);
      op.io(10, d.Version);
      op.io(11, d.AccountStateRoot);
      op.end();
    }
  };

  CRaw RawData;
  xvector<uint8_t> WitnessSignature;

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.required(1, d.RawData);
    op.io(2, d.WitnessSignature);
    op.end();
  }
};

// The two contract payloads worth unwrapping from the Any: between them they are almost all
// of mainnet. The rest stay undecoded - forty-odd schemas to count what the type already says.
struct CTransferContract {
  xvector<uint8_t> OwnerAddress;
  xvector<uint8_t> ToAddress;
  int64_t Amount = 0;

  static constexpr const char *Name = "TransferContract";

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.OwnerAddress);
    op.io(2, d.ToAddress);
    op.io(3, d.Amount);
    op.end();
  }
};

struct CTriggerSmartContract {
  xvector<uint8_t> OwnerAddress;
  xvector<uint8_t> ContractAddress;
  int64_t CallValue = 0;
  xvector<uint8_t> Data;
  int64_t CallTokenValue = 0;
  int64_t TokenId = 0;

  static constexpr const char *Name = "TriggerSmartContract";

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.OwnerAddress);
    op.io(2, d.ContractAddress);
    op.io(3, d.CallValue);
    op.io(4, d.Data);
    op.io(5, d.CallTokenValue);
    op.io(6, d.TokenId);
    op.end();
  }
};

// Transactions come first on the wire because protobuf-java writes fields in field-number
// order and the header is field 2 - a layout choice with real consequences for anyone who
// wants the header without reading the body.
struct CBlock {
  xvector<CTransaction> Transactions;
  CBlockHeader Header;

  static constexpr const char *Name = "block";

  template<typename Op, typename Self> static void io(Op &op, Self &d) {
    op.io(1, d.Transactions);
    op.required(2, d.Header);
    op.end();
  }
};

}
