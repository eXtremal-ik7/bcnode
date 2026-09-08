#include "proto.h"

#include "crypto/sha256.h"

#include <string.h>

namespace TRX {

bool CPbSurvey::Lax = false;
uint64_t CPbSurvey::Seen = 0;
bool CPbSurvey::SeenHigh = false;

BaseBlob<256> sha256Of(const uint8_t *data, size_t size)
{
  BaseBlob<256> result;
  sha256(data, size, result.begin());
  return result;
}

BaseBlob<256> blockId(const uint8_t *rawData, size_t size, int64_t number)
{
  BaseBlob<256> result = sha256Of(rawData, size);
  const uint64_t height = static_cast<uint64_t>(number);
  for (unsigned i = 0; i < 8; i++)
    result.begin()[i] = static_cast<uint8_t>(height >> (56 - i * 8));
  return result;
}

// Block.block_header is field 2, BlockHeader.raw_data field 1. The numbers live in the shapes
// too, but reaching these bytes must not cost a decode of the transactions that precede them.
bool blockHeaderRawData(const uint8_t *block, size_t size, const uint8_t *&raw, size_t &rawSize)
{
  CPbMessage scan(block, size);
  CPbField field;
  while (scan.next(field)) {
    if (field.Number != 2 || field.Wire != PbBytes)
      continue;
    CPbMessage header(field.Payload, field.Size);
    CPbField inner;
    while (header.next(inner)) {
      if (inner.Number != 1 || inner.Wire != PbBytes)
        continue;
      raw = inner.Payload;
      rawSize = inner.Size;
      return true;
    }
    return false;
  }
  return false;
}

// Block.transactions is field 1, and a leaf is sha256 of the whole serialized transaction -
// signatures and ret included, unlike a txid, which hashes raw_data alone
bool transactionMerkleLeaves(const uint8_t *block, size_t size, std::vector<BaseBlob<256>> &leaves)
{
  leaves.clear();
  CPbMessage scan(block, size);
  CPbField field;
  while (scan.next(field)) {
    if (field.Number == 1 && field.Wire == PbBytes)
      leaves.push_back(sha256Of(field.Payload, field.Size));
  }
  return !scan.bad();
}

BaseBlob<256> merkleRoot(BaseBlob<256> *level, size_t count)
{
  if (!count)
    return BaseBlob<256>::zero();

  while (count > 1) {
    size_t out = 0;
    for (size_t i = 0; i < count; i += 2, out++) {
      if (i + 1 == count) {
        // The odd one out is promoted as it is. Bitcoin would hash it against itself, and
        // getting this wrong reproduces every root except the ones that matter.
        level[out] = level[i];
        continue;
      }
      uint8_t pair[64];
      memcpy(pair, level[i].begin(), 32);
      memcpy(pair + 32, level[i + 1].begin(), 32);
      // out <= i always, so writing the parent cannot clobber a child still to be read
      sha256(pair, sizeof(pair), level[out].begin());
    }
    count = out;
  }
  return level[0];
}

const char *contractTypeName(int32_t type)
{
  switch (type) {
    case AccountCreate:           return "AccountCreate";
    case Transfer:                return "Transfer";
    case TransferAsset:           return "TransferAsset";
    case VoteAsset:               return "VoteAsset";
    case VoteWitness:             return "VoteWitness";
    case WitnessCreate:           return "WitnessCreate";
    case AssetIssue:              return "AssetIssue";
    case WitnessUpdate:           return "WitnessUpdate";
    case ParticipateAssetIssue:   return "ParticipateAssetIssue";
    case AccountUpdate:           return "AccountUpdate";
    case FreezeBalance:           return "FreezeBalance";
    case UnfreezeBalance:         return "UnfreezeBalance";
    case WithdrawBalance:         return "WithdrawBalance";
    case UnfreezeAsset:           return "UnfreezeAsset";
    case UpdateAsset:             return "UpdateAsset";
    case ProposalCreate:          return "ProposalCreate";
    case ProposalApprove:         return "ProposalApprove";
    case ProposalDelete:          return "ProposalDelete";
    case SetAccountId:            return "SetAccountId";
    case Custom:                  return "Custom";
    case CreateSmartContract:     return "CreateSmartContract";
    case TriggerSmartContract:    return "TriggerSmartContract";
    case GetContract:             return "GetContract";
    case UpdateSetting:           return "UpdateSetting";
    case ExchangeCreate:          return "ExchangeCreate";
    case ExchangeInject:          return "ExchangeInject";
    case ExchangeWithdraw:        return "ExchangeWithdraw";
    case ExchangeTransaction:     return "ExchangeTransaction";
    case UpdateEnergyLimit:       return "UpdateEnergyLimit";
    case AccountPermissionUpdate: return "AccountPermissionUpdate";
    case ClearABI:                return "ClearABI";
    case UpdateBrokerage:         return "UpdateBrokerage";
    case ShieldedTransfer:        return "ShieldedTransfer";
    case MarketSellAsset:         return "MarketSellAsset";
    case MarketCancelOrder:       return "MarketCancelOrder";
    case FreezeBalanceV2:         return "FreezeBalanceV2";
    case UnfreezeBalanceV2:       return "UnfreezeBalanceV2";
    case WithdrawExpireUnfreeze:  return "WithdrawExpireUnfreeze";
    case DelegateResource:        return "DelegateResource";
    case UnDelegateResource:      return "UnDelegateResource";
    case CancelAllUnfreezeV2:     return "CancelAllUnfreezeV2";
    default:                      return "unknown";
  }
}

}
