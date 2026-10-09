#pragma once

#include "proto.h"
#include "script.h"
#include "merkleTree.h"
#include "common/serializeUtils.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace BTC {

// Same-block spend topology. The map mirrors the input resolvers: a tx
// becomes visible after its own inputs, so only spends of earlier txs
// match. An out-of-range vout still marks the input (the block dies in
// the resolver before any connect) but has no output bit to set.
// A txid match is trusted to be the spend target (BIP30 uniqueness): an
// input spending an older on-disk duplicate would be paired with the local
// twin instead, and the utxodb pair skip would leave the older coin alive.
// Safe: a duplicate txid can only be a coinbase (an identical non-coinbase
// would re-spend its own inputs), unspendable in its own block by maturity
//
// Nothing of a format is in here: the txids are already computed and the rest is the shape
// of the input and output lists, so a coin that lays its block out differently still gets
// its topology from here rather than restating it
template<typename BlockTy>
void fillLocalSpendTopology(const BlockTy &block, BTC::Proto::CBlockValidationData &validation)
{
  size_t inputsNum = 0;
  uint64_t outputsNum = 0;
  for (size_t i = 0; i < block.Vtx.size(); i++) {
    outputsNum += block.Vtx[i].TxOut.size();
    if (i)
      inputsNum += block.Vtx[i].TxIn.size();
  }
  validation.InputLocalTx.resize(inputsNum);
  validation.OutputSpentLocally.resize((outputsNum + 63) / 64);
  memset(validation.OutputSpentLocally.begin(), 0, validation.OutputSpentLocally.size() * sizeof(uint64_t));

  // Value packs the tx index with the block-wide ordinal of its first output
  std::unordered_map<Proto::TxHashTy, uint64_t> txIndexMap;
  size_t inOrdinal = 0;
  uint64_t outOrdinal = 0;
  if (!block.Vtx.empty()) {
    txIndexMap[validation.TxIds[0]] = 0;
    outOrdinal = block.Vtx[0].TxOut.size();
  }
  for (size_t i = 1; i < block.Vtx.size(); i++) {
    const auto &tx = block.Vtx[i];
    for (size_t j = 0; j < tx.TxIn.size(); j++, inOrdinal++) {
      const auto &txin = tx.TxIn[j];
      auto It = txIndexMap.find(txin.PreviousOutputHash);
      if (It == txIndexMap.end()) {
        validation.InputLocalTx[inOrdinal] = Proto::CBlockValidationData::NoLocalTx;
        continue;
      }
      uint32_t localTxIdx = static_cast<uint32_t>(It->second);
      validation.InputLocalTx[inOrdinal] = localTxIdx;
      if (txin.PreviousOutputIndex < block.Vtx[localTxIdx].TxOut.size()) {
        uint64_t bit = (It->second >> 32) + txin.PreviousOutputIndex;
        uint64_t mask = 1ull << (bit & 63);
        // Already spent by an earlier input of this block: the block is invalid, and saying so
        // here saves the linker a set per block
        if (validation.OutputSpentLocally[bit >> 6] & mask)
          validation.LocalSpendInvalid = true;
        validation.OutputSpentLocally[bit >> 6] |= mask;
      } else {
        validation.LocalSpendInvalid = true;
      }
    }
    txIndexMap[validation.TxIds[i]] = static_cast<uint64_t>(i) | (outOrdinal << 32);
    outOrdinal += tx.TxOut.size();
  }
}

template<typename BlockTy>
void validationDataInitialize(const BlockTy &block, BTC::Proto::CBlockValidationData &validation)
{
  validation.HasWitnessData = false;
  validation.InputsResolved = false;
  validation.InputsInvalid = false;
  validation.LocalSpendInvalid = false;
  validation.TxIds.resize(block.Vtx.size());
  for (size_t i = 0; i < block.Vtx.size(); i++)
    validation.TxIds[i] = block.Vtx[i].getTxId();
  BTC::fillTxLayout(block, validation.TxLayout);
  validation.TxData.resize(block.Vtx.size());
  for (size_t i = 0; i < block.Vtx.size(); i++) {
    validation.TxData[i].ScriptSigKnownValid.resize(block.Vtx[i].TxIn.size());
    for (auto &v: validation.TxData[i].ScriptSigKnownValid) {
      v.ScriptSigKnownValid = false;
    }
  }

  // What the parsed output blob is sized by; the topology pass counts for itself
  uint64_t outputsNum = 0;
  for (size_t i = 0; i < block.Vtx.size(); i++)
    outputsNum += block.Vtx[i].TxOut.size();

  // Parsed here, where several blocks are parsed in parallel: the connect stage copies the
  // record instead of walking the script. An OP_RETURN output is left empty - "not a utxo" to a
  // database that knows nothing of types
  {
    xmstream outputData;
    outputData.reserve(outputsNum * sizeof(Script::CUnspentOutputInfo));
    outputData.reset();
    validation.OutputDataOffset.resize(outputsNum + 1);

    size_t ordinal = 0;
    for (size_t i = 0; i < block.Vtx.size(); i++) {
      const auto &tx = block.Vtx[i];
      for (size_t j = 0; j < tx.TxOut.size(); j++, ordinal++) {
        size_t begin = outputData.offsetOf();
        validation.OutputDataOffset[ordinal] = static_cast<uint32_t>(begin);
        Script::parseTransactionOutput(tx, j, outputData);
        const Script::CUnspentOutputInfo *info =
          reinterpret_cast<const Script::CUnspentOutputInfo*>(outputData.data<uint8_t>() + begin);
        if (info->Type == Script::CUnspentOutputInfo::EOpReturn)
          outputData.seekSet(begin);
      }
    }

    validation.OutputDataOffset[outputsNum] = static_cast<uint32_t>(outputData.offsetOf());
    xvectorFromStream(std::move(outputData), validation.OutputData);
  }

  BTC::fillLocalSpendTopology(block, validation);
}


// The limit applies to the base size, hence the context dropping the witness data; a coin
// whose blocks carry more passes its own, as LTC does to keep the extension block out of the
// count the way Core's SERIALIZE_NO_MWEB does
template<typename BlockTy, typename CtxTy = bool>
bool validateBlockSize(const BlockTy &block, size_t limit, std::string &error, CtxTy ctx = false)
{
  bool result = BTC::Io<BlockTy>::getSerializedSize(block, ctx) <= limit;
  if (!result)
    error = "bad-blocksize";
  return result;
}

template<typename BlockTy>
bool validateMerkleRoot(const BlockTy &block, std::string &error) {
  bool result = calculateBlockMerkleRoot(block) == block.Header.HashMerkleRoot;
  if (!result)
    error = "bad-merkleroot";
  return result;
}

// Variant consuming txids precomputed by validationDataInitialize - checking
// them against the header makes them trustworthy for every later consumer.
// calculateMerkleRoot folds the array in place, hence the copy
template<typename BlockTy>
bool validateMerkleRoot(const BlockTy &block, const xvector<Proto::TxHashTy> &txIds, std::string &error) {
  assert(txIds.size() == block.Vtx.size());
  std::unique_ptr<BaseBlob<256>[]> hashes(new BaseBlob<256>[txIds.size()]);
  std::copy(txIds.begin(), txIds.end(), hashes.get());
  bool result = calculateMerkleRoot(hashes.get(), txIds.size()) == block.Header.HashMerkleRoot;
  if (!result)
    error = "bad-merkleroot";
  return result;
}

template<typename BlockTy>
bool validateWitnessCommitment(const BlockTy &block, bool &hasWitness, std::string &error) {
  if (block.Vtx.empty() || block.Vtx[0].TxIn.empty()) {
    error = "bad-coinbase-missing";
    return false;
  }
  const auto &coinbaseTxIn = block.Vtx[0].TxIn[0];

  // Get commitment txout index
  size_t commitmentPos = std::numeric_limits<size_t>::max();
  for (size_t i = 0, ie = block.Vtx[0].TxOut.size(); i != ie; ++i) {
    const xvector<uint8_t> &pkScript = block.Vtx[0].TxOut[i].PkScript;
    if (pkScript.size() >= 38 &&
        pkScript[0] == BTC::Script::OP_RETURN &&
        pkScript[1] == 0x24 &&
        pkScript[2] == 0xaa &&
        pkScript[3] == 0x21 &&
        pkScript[4] == 0xa9 &&
        pkScript[5] == 0xed) {
      // Store last found witness commitment index
      commitmentPos = i;
    }
  }

  if (commitmentPos == std::numeric_limits<size_t>::max()) {
    for (size_t i = 0, ie = block.Vtx.size(); i != ie; ++i) {
      const auto &tx = block.Vtx[i];
      for (size_t j = 0, je = tx.TxIn.size(); j != je; ++j) {
        const auto &txIn = tx.TxIn[j];
        if (!txIn.WitnessStack.empty()) {
          error = "witness-data-without-commitment";
          return false;
        }
      }
    }
    return true;
  }

  hasWitness = true;

  const uint8_t *commitmentData = block.Vtx[0].TxOut[commitmentPos].PkScript.data();

  // Check witness nonce
  if (coinbaseTxIn.WitnessStack.size() != 1 || coinbaseTxIn.WitnessStack[0].size() != 32) {
    // Miners embedded the commitment output before segwit activation, in
    // blocks with no witness data at all; Core checks it only after activation.
    // Height-free equivalent: skip the check when nothing carries witness data
    bool blockHasWitnessData = false;
    for (size_t i = 0, ie = block.Vtx.size(); i != ie && !blockHasWitnessData; ++i) {
      const auto &tx = block.Vtx[i];
      for (size_t j = 0, je = tx.TxIn.size(); j != je; ++j) {
        if (!tx.TxIn[j].WitnessStack.empty()) {
          blockHasWitnessData = true;
          break;
        }
      }
    }

    if (!blockHasWitnessData) {
      hasWitness = false;
      return true;
    }

    error = "bad-witness-nonce";
    return false;
  }
  const uint8_t *witnessNonce = coinbaseTxIn.WitnessStack[0].data();

  // Calculate witness merkle root
  BaseBlob<256> witnessMerkleRoot = calculateBlockWitnessMerkleRoot(block);
  // Calculate witness commitment
  BaseBlob<256> commitment = sha256d(witnessMerkleRoot.begin(), witnessMerkleRoot.size(), witnessNonce, 32);

  bool result = memcmp(commitment.begin(), commitmentData+6, 32) == 0;
  if (!result)
    error = "bad-witness-commitment";
  return result;
}

template<typename BlockTy>
bool validateBIP34(uint32_t height, const BlockTy &block, uint32_t bip34Height, std::string &error) {
  if (height < bip34Height)
    return true;

  if (block.Vtx.empty() || block.Vtx[0].TxIn.empty()) {
    error = "coinbase-height-missing";
    return false;
  }

  auto &coinbaseTxIn = block.Vtx[0].TxIn[0];

  xmstream src(coinbaseTxIn.ScriptSig.data(), coinbaseTxIn.ScriptSig.size());

  // Read size followed by little endian number
  uint8_t size = src.read<uint8_t>();
  uint64_t v = 0;
  for (uint8_t i = 0; i < size; i++)
    v |= (static_cast<uint64_t>(src.read<uint8_t>()) << 8*i);

  bool result = !src.eof() && v == height;
  if (!result)
    error = "coinbase-height-mismatch";
  return result;
}

static inline bool validateUnexpectedWitness(uint32_t height, bool hasWitnessData, uint32_t segwitHeight, std::string &error) {
  bool result = !(height < segwitHeight && hasWitnessData);
  if (!result)
    error = "unexpected-witness-data";
  return result;
}

// Transactions: Core's CheckTransaction, CheckTxInputs, IsFinalTx and SequenceLocks, one rule to a
// function. Finding the outputs a transaction spends is the caller's business. Scripts and the
// coins' own parts (MWEB, the shielded pools) are not checked yet: taken as valid

// A spent output as the rules see it
struct CPrevout {
  int64_t Value = 0;
  // Script::CUnspentOutputInfo::EType: policy needs no more of the script than its form
  uint8_t Type = 0;
  // The height of the block that created it
  uint32_t Height = 0;
  bool Coinbase = false;
  // MWEB: a peg-out, a HogEx output past the first
  bool Pegout = false;
};

// The block a transaction is checked for
struct CTxContext {
  uint32_t Height = 0;
  // Its own time: locktime counts against it where BIP113 is not consensus. For the mempool, now
  int64_t BlockTime = 0;
  int64_t MedianTimePast = 0;
  // Of a block below on the same chain: BIP68 time locks count from it
  std::function<int64_t(uint32_t)> MedianTimePastAt;
};

// What the mempool policy measures (policy.h)
struct CTxCost;

// Core's IsCoinBase: one input, the null prevout
template<typename TxTy>
bool isCoinbase(const TxTy &tx)
{
  return tx.TxIn.size() == 1 && tx.TxIn[0].PreviousOutputHash.isNull() && tx.TxIn[0].PreviousOutputIndex == 0xFFFFFFFF;
}

// The coinbase first, and only there
template<typename BlockTy>
bool validateBlockCoinbase(const BlockTy &block, std::string &error)
{
  if (block.Vtx.empty() || !isCoinbase(block.Vtx[0])) {
    error = "bad-cb-missing";
    return false;
  }
  for (size_t i = 1; i < block.Vtx.size(); i++) {
    if (isCoinbase(block.Vtx[i])) {
      error = "bad-cb-multiple";
      return false;
    }
  }
  return true;
}

// The legacy sigops of every script: Core's CheckBlock limit, before the inputs are known
template<typename BlockTy>
bool validateBlockSigOps(const BlockTy &block, unsigned limit, std::string &error)
{
  unsigned sigOps = 0;
  for (const auto &tx: block.Vtx) {
    for (const auto &in: tx.TxIn)
      sigOps += Script::sigOpCount(in.ScriptSig.data(), in.ScriptSig.data() + in.ScriptSig.size(), false);
    for (const auto &out: tx.TxOut)
      sigOps += Script::sigOpCount(out.PkScript.data(), out.PkScript.data() + out.PkScript.size(), false);
  }
  if (sigOps > limit) {
    error = "bad-blk-sigops";
    return false;
  }
  return true;
}

template<typename TxTy>
bool validateTxNotEmpty(const TxTy &tx, std::string &error)
{
  if (tx.TxIn.empty()) {
    error = "bad-txns-vin-empty";
    return false;
  }
  if (tx.TxOut.empty()) {
    error = "bad-txns-vout-empty";
    return false;
  }
  return true;
}

// Litecoin's: an MWEB-only transaction has both in its MWEB part
template<typename TxTy>
bool validateTxNotEmptyMweb(const TxTy &tx, std::string &error)
{
  if (tx.hasMweb() && tx.TxIn.empty() && tx.TxOut.empty())
    return true;
  return validateTxNotEmpty(tx, error);
}

// Zcash's: the shielded parts stand in for either side
template<typename TxTy>
bool validateTxNotEmptyShielded(const TxTy &tx, std::string &error)
{
  if (tx.TxIn.empty() && tx.JoinSplits.empty() && tx.ShieldedSpends.empty()) {
    error = "bad-txns-vin-empty";
    return false;
  }
  if (tx.TxOut.empty() && tx.JoinSplits.empty() && tx.ShieldedOutputs.empty()) {
    error = "bad-txns-vout-empty";
    return false;
  }
  return true;
}

// The transaction fits a block. The size is the coin's: without the witness (and MWEB for
// Litecoin) as Core counts it, the whole transaction for Zcash
static inline bool validateTxSize(size_t size, size_t limit, std::string &error)
{
  if (size > limit) {
    error = "bad-txns-oversize";
    return false;
  }
  return true;
}

template<typename TxTy>
bool validateTxOutputValues(const TxTy &tx, int64_t maxMoney, std::string &error)
{
  int64_t total = 0;
  for (const auto &out: tx.TxOut) {
    if (out.Value < 0) {
      error = "bad-txns-vout-negative";
      return false;
    }
    if (out.Value > maxMoney) {
      error = "bad-txns-vout-toolarge";
      return false;
    }
    total += out.Value;
    if (total > maxMoney) {
      error = "bad-txns-txouttotal-toolarge";
      return false;
    }
  }
  return true;
}

// Primecoin's: no output below the minimum
template<typename TxTy>
bool validateTxOutputMinimum(const TxTy &tx, int64_t minimum, std::string &error)
{
  for (const auto &out: tx.TxOut) {
    if (out.Value < minimum) {
      error = "bad-txns-vout-belowminimum";
      return false;
    }
  }
  return true;
}

// A coinbase carries a scriptSig of 2 to 100 bytes; any other transaction spends no null prevout
template<typename TxTy>
bool validateTxPrevoutNull(const TxTy &tx, std::string &error)
{
  auto isNull = [](const auto &in) { return in.PreviousOutputHash.isNull() && in.PreviousOutputIndex == 0xFFFFFFFF; };
  if (isCoinbase(tx)) {
    const size_t size = tx.TxIn[0].ScriptSig.size();
    if (size < 2 || size > 100) {
      error = "bad-cb-length";
      return false;
    }
    return true;
  }

  for (const auto &in: tx.TxIn) {
    if (isNull(in)) {
      error = "bad-txns-prevout-null";
      return false;
    }
  }
  return true;
}

template<typename TxTy>
bool validateTxDuplicateInputs(const TxTy &tx, std::string &error)
{
  std::vector<std::pair<Proto::TxHashTy, uint32_t>> outpoints;
  outpoints.reserve(tx.TxIn.size());
  for (const auto &in: tx.TxIn)
    outpoints.emplace_back(in.PreviousOutputHash, in.PreviousOutputIndex);
  std::sort(outpoints.begin(), outpoints.end(), [](const auto &l, const auto &r) {
    int c = memcmp(l.first.begin(), r.first.begin(), l.first.size());
    return c < 0 || (c == 0 && l.second < r.second);
  });

  if (std::adjacent_find(outpoints.begin(), outpoints.end()) != outpoints.end()) {
    error = "bad-txns-inputs-duplicate";
    return false;
  }
  return true;
}

// The outputs passed validateTxOutputValues. Sets the fee once the values pass
template<typename TxTy>
bool validateTxInputValues(const TxTy &tx, const CPrevout *prevouts, int64_t maxMoney, int64_t &fee, std::string &error)
{
  int64_t valueIn = 0;
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    if (prevouts[i].Value < 0 || prevouts[i].Value > maxMoney) {
      error = "bad-txns-inputvalues-outofrange";
      return false;
    }
    valueIn += prevouts[i].Value;
    if (valueIn > maxMoney) {
      error = "bad-txns-inputvalues-outofrange";
      return false;
    }
  }

  int64_t valueOut = 0;
  for (const auto &out: tx.TxOut)
    valueOut += out.Value;
  if (valueIn < valueOut) {
    error = "bad-txns-in-belowout";
    return false;
  }

  fee = valueIn - valueOut;
  return true;
}

// maturity(height): the blocks a coinbase output created at that height waits
template<typename TxTy, typename MaturityTy>
bool validateTxCoinbaseMaturity(const TxTy &tx, const CPrevout *prevouts, uint32_t height, MaturityTy maturity, std::string &error)
{
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    if (prevouts[i].Coinbase && height - prevouts[i].Height < maturity(prevouts[i].Height)) {
      error = "bad-txns-premature-spend-of-coinbase";
      return false;
    }
  }
  return true;
}

template<typename TxTy>
bool validateTxPegoutMaturity(const TxTy &tx, const CPrevout *prevouts, uint32_t height, uint32_t maturity, std::string &error)
{
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    if (prevouts[i].Pegout && height - prevouts[i].Height < maturity) {
      error = "bad-txns-premature-spend-of-pegout";
      return false;
    }
  }
  return true;
}

// validateUnexpectedWitness for one transaction: no witness before segwit
template<typename TxTy>
bool validateTxUnexpectedWitness(const TxTy &tx, uint32_t height, uint32_t segwitHeight, std::string &error)
{
  const bool hasWitness = std::any_of(tx.TxIn.begin(), tx.TxIn.end(), [](const auto &in) { return !in.WitnessStack.empty(); });
  return validateUnexpectedWitness(height, hasWitness, segwitHeight, error);
}

// Locktime against the height and, past the threshold, a time: the block's own, or its median time
// past where BIP113 holds
template<typename TxTy>
bool validateTxFinal(const TxTy &tx, uint32_t height, int64_t time, std::string &error)
{
  constexpr int64_t LocktimeThreshold = 500000000;
  const int64_t lockTime = tx.LockTime;
  if (lockTime == 0 || lockTime < (lockTime < LocktimeThreshold ? static_cast<int64_t>(height) : time))
    return true;

  for (const auto &in: tx.TxIn) {
    if (in.Sequence != 0xFFFFFFFF) {
      error = "bad-txns-nonfinal";
      return false;
    }
  }
  return true;
}

// BIP68 relative locks, counted from the blocks that created the spent outputs
template<typename TxTy>
bool validateTxSequenceLocks(const TxTy &tx, const CPrevout *prevouts, const CTxContext &context, std::string &error)
{
  constexpr uint32_t DisableFlag = 1u << 31;
  constexpr uint32_t TypeFlag = 1u << 22;
  constexpr uint32_t Mask = 0x0000FFFF;
  constexpr int Granularity = 9;

  // Version as unsigned, as Core compares it
  if (static_cast<uint32_t>(tx.Version) < 2)
    return true;

  int64_t minHeight = -1;
  int64_t minTime = -1;
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    const uint32_t sequence = tx.TxIn[i].Sequence;
    if (sequence & DisableFlag)
      continue;
    const int64_t coinHeight = prevouts[i].Height;
    if (sequence & TypeFlag) {
      int64_t coinTime = context.MedianTimePastAt(static_cast<uint32_t>(std::max<int64_t>(coinHeight - 1, 0)));
      minTime = std::max(minTime, coinTime + (static_cast<int64_t>(sequence & Mask) << Granularity) - 1);
    } else {
      minHeight = std::max(minHeight, coinHeight + static_cast<int64_t>(sequence & Mask) - 1);
    }
  }

  if (minHeight >= static_cast<int64_t>(context.Height) || minTime >= context.MedianTimePast) {
    error = "bad-txns-nonfinal";
    return false;
  }
  return true;
}
}

template<typename X>
bool validateUnexpectedWitness(const typename X::BlockIndex &index, const typename X::Proto::CBlock &block, const typename X::ChainParams &chainParams, std::string &error) {
  bool result = !(index.Height < chainParams.SegwitHeight && block.validationData.HasWitness);
  error = "unexpected-witness-data";
  return result;
}

template<typename X>
bool validateScriptSig(const typename X::Proto::ValidationData&, const typename X::Proto::CTransaction&, const typename X::ChainParams&, std::string&)
{
  return true;
}

template<typename X>
bool validateAmount(const typename X::Proto::ValidationData&, const typename X::Proto::CTransaction&, const typename X::ChainParams&, std::string&)
{
  return true;
}
