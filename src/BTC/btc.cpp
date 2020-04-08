// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "btc.h"
#include <p2putils/xmstream.h>

namespace BTC {
namespace Common {

bool checkConsensus(const BTC::Proto::BlockHeader &header, CheckConsensusCtx&, BTC::Common::ChainParams &chainParams)
{
  bool fNegative;
  bool fOverflow;
  arith_uint256 bnTarget;

  bnTarget.SetCompact(header.nBits, &fNegative, &fOverflow);

  // Check range
  if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(chainParams.powLimit))
    return false;

  // Check proof of work matches claimed amount
  if (UintToArith256(header.GetHash()) > bnTarget)
    return false;

  return true;
}

arith_uint256 GetBlockProof(const BTC::Proto::BlockHeader &header, const ChainParams&)
{
  arith_uint256 bnTarget;
  bool fNegative;
  bool fOverflow;
  bnTarget.SetCompact(header.nBits, &fNegative, &fOverflow);
  if (fNegative || fOverflow || bnTarget == 0)
      return 0;
  // We need to compute 2**256 / (bnTarget+1), but we can't represent 2**256
  // as it's too large for an arith_uint256. However, as 2**256 is at least as large
  // as bnTarget+1, it is equal to ((2**256 - bnTarget - 1) / (bnTarget+1)) + 1,
  // or ~bnTarget / (bnTarget+1) + 1.
  return (~bnTarget / (bnTarget + 1)) + 1;
}

}
}
