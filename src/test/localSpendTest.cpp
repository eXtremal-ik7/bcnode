// The spends of a block's own outputs its validation data proves invalid ahead of the database

#include "BC/bc.h"
#include "BTC/validation.h"
#include <gtest/gtest.h>
#include <vector>

namespace {

using Script = std::vector<uint8_t>;

const Script Paid{0x51};
const Script Data{0x6a, 0x01, 0x00};

BTC::Proto::CTxIn input(const BTC::Proto::TxHashTy &tx, uint32_t index)
{
  BTC::Proto::CTxIn in;
  in.PreviousOutputHash = tx;
  in.PreviousOutputIndex = index;
  in.Sequence = 0xffffffff;
  return in;
}

// A coinbase, a transaction with the outputs, a transaction that spends those of them named
bool invalid(const std::vector<Script> &outputs, const std::vector<uint32_t> &spent)
{
  BTC::Proto::CBlock block;
  block.Vtx.resize(3);
  for (auto &tx: block.Vtx) {
    tx.Version = 1;
    tx.LockTime = 0;
  }
  block.Vtx[0].TxIn.emplace_back(input(BTC::Proto::TxHashTy(), 0xffffffff));
  block.Vtx[1].TxIn.emplace_back(input(BTC::Proto::TxHashTy(), 7));
  for (const Script &script: outputs) {
    BTC::Proto::CTxOut out;
    out.Value = 1000;
    out.PkScript.resize(script.size());
    std::copy(script.begin(), script.end(), out.PkScript.begin());
    block.Vtx[1].TxOut.emplace_back(std::move(out));
  }
  for (uint32_t index: spent)
    block.Vtx[2].TxIn.emplace_back(input(block.Vtx[1].getTxId(), index));

  BTC::Proto::CBlockValidationData validation;
  BTC::validationDataInitialize(block, validation);
  return validation.LocalSpendInvalid;
}

}

TEST(LocalSpend, Invalid)
{
  EXPECT_FALSE(invalid({Paid, Paid}, {1}));
  EXPECT_FALSE(invalid({Paid, Paid}, {1, 0}));
  // Twice; an output the transaction does not have
  EXPECT_TRUE(invalid({Paid, Paid}, {1, 1}));
  EXPECT_TRUE(invalid({Paid, Paid}, {2}));
  // An OP_RETURN output is no utxo, as in Core: not there to spend, there to pass by
  EXPECT_TRUE(invalid({Paid, Data}, {1}));
  EXPECT_FALSE(invalid({Data, Paid}, {1}));
}
