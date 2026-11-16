#pragma once

#include "fastpath.h"
#include "interpreter.h"
#include "common/scriptCheck.h"
#include <algorithm>
#include <string>

// Script checks, Core's CheckInputScripts: an input, a whole
// transaction under the flags of a block, or the mempool's two passes. A coin calls these from its
// checkBlockScripts and checkPolicyScripts with its own flags

namespace BTC {

// One input under the flags: Core's flag sets for blocks, the standard and the mandatory ones for
// the mempool's two passes. The fast path takes the five templates, the interpreter the rest
template<typename TxTy>
CInputVerdict verifyInput(CTxChecker<TxTy> &checker, size_t input, uint32_t flags, ESigMode mode, CSigBatch *batch)
{
  if (std::optional<CInputVerdict> verdict = fastPathInput(checker, input, flags, mode, batch))
    return *verdict;
  const EScriptError error = interpretInput(checker, input, flags);
  return {error == EScriptError::Ok ? EVerdict::Valid : EVerdict::Invalid, error, true};
}

// The input a transaction's scripts failed at; Tx is the ordinal the caller named it by
struct CScriptFailure {
  uint32_t Tx = 0;
  uint32_t Input = 0;
  EScriptError Error = EScriptError::Ok;
};

// What a check went through
struct CScriptCount {
  uint64_t Inputs = 0;
  // Left to the interpreter: unchecked while it is a stub
  uint64_t Interpreted = 0;
};

// Every input under a block's flags. spent - the records of the outputs the inputs spend, one to
// an input. False at the first invalid input; in Defer the signatures go to the batch, and a job
// that fails there is the caller's to report by its ordinal
template<typename TxTy>
bool verifyTransaction(const TxTy &tx,
                       std::span<const Script::CUnspentOutputInfo* const> spent,
                       uint32_t ordinal,
                       uint32_t flags,
                       ESigMode mode,
                       CSigBatch *batch,
                       CScriptFailure &failure,
                       CScriptCount &count)
{
  CTxChecker<TxTy> checker(tx, spent, ordinal);
  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    const CInputVerdict verdict = verifyInput(checker, i, flags, mode, batch);
    count.Inputs++;
    count.Interpreted += verdict.Interpreted;
    if (verdict.Result == EVerdict::Invalid) {
      failure = {ordinal, static_cast<uint32_t>(i), verdict.Error};
      return false;
    }
  }
  return true;
}

// Transactions [firstTx, lastTx) of a block under its flags but those skip names, each against the
// linked outputs of its inputs: what a coin's checkBlockScripts does. The signatures of the fast
// path are put off into a batch and verified together; Trust takes them as valid. The coinbase
// spends nothing
template<typename BlockTy>
bool verifyBlockScripts(const BlockTy &block,
                        const Proto::CBlockLinkedOutputs &linkedOutputs,
                        size_t firstTx,
                        size_t lastTx,
                        uint32_t flags,
                        ESigMode mode,
                        const uint64_t *skip,
                        CScriptCheckResult &result)
{
  // A batch no longer than the cache holds
  constexpr size_t BatchSize = 1024;

  CSigBatch batch;
  std::vector<uint32_t> failed;
  std::vector<const Script::CUnspentOutputInfo*> spent;
  CScriptCount count;
  CScriptFailure failure;
  bool isValid = true;

  auto flush = [&]() {
    failed.clear();
    batch.verify(failed);
    result.Signatures += batch.size();
    if (!failed.empty()) {
      const CSigJob &job = batch[failed.front()];
      failure = {job.Tx, job.Input, job.Error};
      isValid = false;
    }
    batch.clear();
  };

  for (size_t i = std::max<size_t>(firstTx, 1); i < lastTx && isValid; i++) {
    if (scriptTxSkipped(skip, i))
      continue;
    const auto &tx = block.Vtx[i];
    const auto &linked = linkedOutputs.Tx[i].TxIn;
    spent.resize(tx.TxIn.size());
    for (size_t j = 0; j < spent.size(); j++)
      spent[j] = reinterpret_cast<const Script::CUnspentOutputInfo*>(linked[j].data());
    isValid = verifyTransaction(tx, spent, static_cast<uint32_t>(i), flags, mode, &batch, failure, count);
    if (isValid && batch.size() >= BatchSize)
      flush();
  }
  // After a failed rule too: a signature put off for an earlier input is the first invalid one
  if (isValid || batch.size())
    flush();

  result.Inputs += count.Inputs;
  result.Unchecked += count.Interpreted;
  if (!isValid) {
    result.Tx = failure.Tx;
    result.Input = failure.Input;
    result.Error = scriptErrorString(failure.Error);
  }
  return isValid;
}

// The mempool's passes. The standard flags; an input that fails them is tried under the mandatory
// ones, which tells a non-standard spend from an invalid one. reason is Core's reject string
template<typename TxTy>
EScriptRefusal verifyTransactionPolicy(const TxTy &tx,
                                       std::span<const Script::CUnspentOutputInfo* const> spent,
                                       uint32_t standardFlags,
                                       uint32_t mandatoryFlags,
                                       std::string &reason,
                                       CScriptCount &count)
{
  CTxChecker<TxTy> checker(tx, spent);
  auto passes = [&](uint32_t flags) {
    for (size_t i = 0; i < tx.TxIn.size(); i++) {
      if (verifyInput(checker, i, flags, ESigMode::Now, nullptr).Result != EVerdict::Valid)
        return false;
    }
    return true;
  };

  for (size_t i = 0; i < tx.TxIn.size(); i++) {
    CInputVerdict verdict = verifyInput(checker, i, standardFlags, ESigMode::Now, nullptr);
    count.Inputs++;
    count.Interpreted += verdict.Interpreted;
    if (verdict.Result == EVerdict::Valid)
      continue;

    EScriptRefusal refusal = EScriptRefusal::Mandatory;
    if (standardFlags != mandatoryFlags) {
      const CInputVerdict mandatory = verifyInput(checker, i, mandatoryFlags, ESigMode::Now, nullptr);
      if (mandatory.Result == EVerdict::Valid)
        refusal = EScriptRefusal::NonStandard;
      else
        verdict = mandatory;
    }
    reason = refusal == EScriptRefusal::NonStandard ? "non-mandatory-script-verify-flag (" : "mandatory-script-verify-flag-failed (";
    reason += scriptErrorString(verdict.Error);
    reason += ')';

    // Core's PolicyScriptChecks: passes with WITNESS off and fails with it on - only the witness
    // is missing. CLEANSTACK goes with WITNESS, it cannot stay on alone
    const uint32_t noCleanStack = standardFlags & ~static_cast<uint32_t>(EScriptCleanStack);
    if (!tx.hasWitness() && passes(noCleanStack & ~static_cast<uint32_t>(EScriptWitness)) && !passes(noCleanStack))
      refusal = EScriptRefusal::WitnessStripped;
    return refusal;
  }

  return EScriptRefusal::None;
}

}
