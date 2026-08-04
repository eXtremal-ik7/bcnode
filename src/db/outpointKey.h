// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include "db/keyHash.h"
#include <functional>

// One output of one transaction: the key of every database that answers a
// question about a single output. utxo asks whether it is unspent, spentdb
// which input took it - the two are complements over the same key space
struct COutpointKey {
  BC::Proto::TxHashTy Tx;
  uint32_t Index;

  friend bool operator==(const COutpointKey &a, const COutpointKey &b) { return a.Tx == b.Tx && a.Index == b.Index; }
};

// the raw on-disk key is Tx immediately followed by Index; readers that parse
// it field by field (utxo cache warmup) rely on it
static_assert(sizeof(COutpointKey) == sizeof(BC::Proto::TxHashTy) + sizeof(uint32_t),
              "unexpected padding in COutpointKey");

template<>
class std::hash<COutpointKey> {
public:
  size_t operator()(const COutpointKey &key) const noexcept {
    return static_cast<size_t>(hashOutpoint(key.Tx.begin(), key.Index).H1);
  }
};
