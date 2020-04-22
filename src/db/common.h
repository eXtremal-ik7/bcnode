// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include <map>

namespace BC {

namespace DB {

enum DatabaseTy {
  DbTransactions = 0,
  DbAddrBalance,
  DbAddrTransactions,
  DbTopBalance,
  DbTopTxCount,

  DbCount
};

enum ActionTy {
  Connect = 0,
  Disconnect,
  WriteData
};


struct BitMap {
  bool Affected[DbCount];
  BitMap() {
    memset(Affected, 0, sizeof(Affected));
  }
};

struct IndexCompare {
  bool operator()(const BC::Common::BlockIndex *l, const BC::Common::BlockIndex *r) const {
    if (l->Height == r->Height)
      return l < r;
    else
      return l->Height > r->Height;
  }
};

using IndexDbMap = std::map<BC::Common::BlockIndex*, BitMap, IndexCompare>;

}
}
