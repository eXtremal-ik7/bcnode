#pragma once

#include "scriptChecks.h"
#include "sighash.h"
#include <span>
#include <vector>

// What checking an input takes, whoever checks it (the fast path, the interpreter): the
// transaction's checker, the mode signatures are verified in, their batch and the verdict

namespace BTC {

enum class ESigKind : uint8_t {
  Ecdsa,
  Schnorr
};

// A signature check: all libsecp256k1 takes, and whose it is
struct CSigJob {
  ESigKind Kind = ESigKind::Ecdsa;
  uint8_t PubKeySize = 0;
  uint8_t PubKey[65];
  // ECDSA: r and s, S normalized as Core's CPubKey::Verify does; Schnorr: as in the witness
  uint8_t Signature[64];
  BaseBlob<256> Digest;
  // The spend's error when it fails, under the flags it was made with
  EScriptError Error = EScriptError::Ok;
  uint32_t Tx = 0;
  uint32_t Input = 0;
};

bool verifySigJob(const CSigJob &job);

class CSigBatch {
public:
  void add(const CSigJob &job) { Jobs_.push_back(job); }
  size_t size() const { return Jobs_.size(); }
  const CSigJob &operator[](size_t index) const { return Jobs_[index]; }
  void clear() { Jobs_.clear(); }
  // The positions of the jobs that failed. The SIMD kernel's place: one by one through
  // libsecp256k1 for now
  void verify(std::vector<uint32_t> &failed) const;

private:
  std::vector<CSigJob> Jobs_;
};

// Core's EvalChecksigPreTapscript ahead of the digest: Ok and the job, or the error. codePush is
// the push of a legacy scriptCode, the key or its hash, FindAndDelete looks for the signature there
EScriptError ecdsaChecksig(Script::CSpan sig,
                           Script::CSpan key,
                           Script::CSpan codePush,
                           ESigVersion version,
                           uint32_t flags,
                           CSigJob &job,
                           uint8_t &hashType);

// What becomes of a fast path signature; the interpreter always checks at once
enum class ESigMode : uint8_t {
  // libsecp256k1 right away: the mempool
  Now,
  // A job into the batch: sync
  Defer,
  // No digest, no signature, the rest checked: a reindex of our own data
  Trust
};

enum class EVerdict : uint8_t {
  Valid,
  Invalid,
  // All but the signature checked, its job is in the batch
  Pending
};

struct CInputVerdict {
  EVerdict Result = EVerdict::Invalid;
  EScriptError Error = EScriptError::Ok;
  // Off the fast path's templates: the interpreter's verdict
  bool Interpreted = false;
};

// A transaction and the records of the outputs its inputs spend, one to an input; the digests
// every input shares on the first need, with no lock: one thread to a checker. ordinal names the
// transaction in its jobs
template<typename TxTy>
class CTxChecker {
public:
  CTxChecker(const TxTy &tx, std::span<const Script::CUnspentOutputInfo* const> spent, uint32_t ordinal = 0) :
    Tx_(tx), Spent_(spent), Ordinal_(ordinal) {}

  const TxTy &tx() const { return Tx_; }
  const Script::CUnspentOutputInfo &spent(size_t input) const { return *Spent_[input]; }
  uint32_t ordinal() const { return Ordinal_; }

  const CTxDigests &digests() {
    if (!HasTxDigests_) {
      txDigests(Tx_, Digests_);
      HasTxDigests_ = true;
    }
    return Digests_;
  }

  const CTxDigests &taprootDigests() {
    digests();
    if (!HasSpentDigests_) {
      spentDigests(Spent_, Digests_);
      HasSpentDigests_ = true;
    }
    return Digests_;
  }

private:
  const TxTy &Tx_;
  std::span<const Script::CUnspentOutputInfo* const> Spent_;
  uint32_t Ordinal_;
  CTxDigests Digests_;
  bool HasTxDigests_ = false;
  bool HasSpentDigests_ = false;
};

}
