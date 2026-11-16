// The BIP34 rule against Core's CScript() << height: every encoding but that one is refused

#include "BC/bc.h"
#include "BTC/validation.h"
#include "common/utils.h"
#include <gtest/gtest.h>
#include <string>

namespace {

void setScriptSig(BTC::Proto::CBlock &block, const std::string &hex)
{
  block.Vtx.resize(1);
  block.Vtx[0].TxIn.resize(1);
  xvector<uint8_t> &scriptSig = block.Vtx[0].TxIn[0].ScriptSig;
  scriptSig.resize(hex.size() / 2);
  if (!hex.empty())
    hex2bin(hex.data(), hex.size(), scriptSig.data());
}

bool passes(uint32_t height, const std::string &scriptSig, uint32_t activation = 0)
{
  BTC::Proto::CBlock block;
  setScriptSig(block, scriptSig);
  std::string error;
  bool result = BTC::validateBIP34(height, block, activation, error);
  if (!result) {
    EXPECT_EQ(error, "bad-cb-height");
  }
  return result;
}

struct CEncoding {
  uint32_t Height;
  const char *Push;
};

// The byte and sign boundaries of CScriptNum, and the small heights Core pushes as opcodes
const CEncoding Encodings[] = {
  {0, "00"},
  {1, "51"},
  {16, "60"},
  {17, "0111"},
  {127, "017f"},
  {128, "028000"},
  {255, "02ff00"},
  {256, "020001"},
  {32767, "02ff7f"},
  {32768, "03008000"},
  {227931, "035b7a03"},
  {8388607, "03ffff7f"},
  {8388608, "0400008000"},
  {0xFFFFFFFF, "05ffffffff00"},
};

}

TEST(BIP34, CoinbaseHeight)
{
  for (const CEncoding &e: Encodings) {
    SCOPED_TRACE(e.Height);
    EXPECT_TRUE(passes(e.Height, e.Push));
    // Whatever the miner puts after the height
    EXPECT_TRUE(passes(e.Height, std::string(e.Push) + "062f503253482f"));
    // One height lower or higher is another push
    if (e.Height != 0) {
      EXPECT_FALSE(passes(e.Height - 1, e.Push));
    }
    if (e.Height != 0xFFFFFFFF) {
      EXPECT_FALSE(passes(e.Height + 1, e.Push));
    }
  }

  // Non-minimal: a zero byte too many, a PUSHDATA1, the sign byte missing
  EXPECT_FALSE(passes(227931, "045b7a0300"));
  EXPECT_FALSE(passes(227931, "4c035b7a03"));
  EXPECT_FALSE(passes(128, "0180"));
  EXPECT_FALSE(passes(8388608, "03000080"));
  // The small heights as data pushes
  EXPECT_FALSE(passes(5, "0105"));
  EXPECT_FALSE(passes(16, "0110"));
  // Cut short, empty
  EXPECT_FALSE(passes(227931, "035b7a"));
  EXPECT_FALSE(passes(227931, ""));
  // A length byte past eight once overflowed the reader
  EXPECT_FALSE(passes(227931, "095b7a03000000000000000000"));

  // Below the activation anything goes
  EXPECT_TRUE(passes(227930, "", 227931));
  EXPECT_TRUE(passes(227930, "045b7a0300", 227931));
  EXPECT_FALSE(passes(227931, "045b7a0300", 227931));

  // No coinbase input at all
  {
    BTC::Proto::CBlock block;
    std::string error;
    EXPECT_FALSE(BTC::validateBIP34(227931, block, 0, error));
    EXPECT_EQ(error, "bad-cb-height");
  }

  // Coinbase scriptSigs of real blocks. Primecoin 0, 1, 16 and 17: OP_0, OP_1, OP_16, then a push
  EXPECT_TRUE(passes(0, "0002e7034c5d53756e6e7920"));
  EXPECT_TRUE(passes(1, "51013100062f503253482f"));
  EXPECT_TRUE(passes(16, "60011f00062f503253482f"));
  EXPECT_TRUE(passes(17, "0111010a00062f503253482f"));
  // Bitcoin 227931, the first block under BIP34 (coinbase 48c23788...)
  EXPECT_TRUE(passes(227931, "035b7a030e436f696e4c61622e2f503253482f045c00000008d629b6d62e07d099", 227931));
}
