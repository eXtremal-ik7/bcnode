// The output record against Core's Solver: the type of a script, the script back from the record,
// the same address from either

#include "BC/bc.h"
#include "BTC/policy.h"
#include "BTC/script.h"
#include "common/utils.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using Info = BTC::Script::CUnspentOutputInfo;

std::string repeat(const std::string &hex, size_t times)
{
  std::string result;
  for (size_t i = 0; i < times; i++)
    result += hex;
  return result;
}

void setBytes(xvector<uint8_t> &out, const std::string &hex)
{
  out.resize(hex.size() / 2);
  if (!hex.empty())
    hex2bin(hex.data(), hex.size(), out.data());
}

const std::string Key33 = "02" + repeat("11", 32);
const std::string Key65 = "04" + repeat("22", 64);
const std::string Hash20 = repeat("33", 20);
const std::string Hash32 = repeat("44", 32);

std::string keys(size_t count)
{
  return repeat("21" + Key33, count);
}

struct CCase {
  std::string Script;
  uint8_t Type;
  // Core's IsStandard: an output that may enter the mempool
  bool Standard;
};

const std::vector<CCase> Cases = {
  // P2PK by the prefix of the key (CPubKey::ValidSize)
  {"21" + Key33 + "ac", Info::EPubKeyCompressed, true},
  {"2103" + repeat("11", 32) + "ac", Info::EPubKeyCompressed, true},
  {"2104" + repeat("11", 32) + "ac", Info::ENonStandard, false},
  {"41" + Key65 + "ac", Info::EPubKeyUncompressed, true},
  {"4106" + repeat("22", 64) + "ac", Info::EPubKeyHybrid, true},
  {"4107" + repeat("22", 64) + "ac", Info::EPubKeyHybrid, true},
  {"4102" + repeat("22", 64) + "ac", Info::ENonStandard, false},

  {"76a914" + Hash20 + "88ac", Info::EPubKeyHash, true},
  {"a914" + Hash20 + "87", Info::EScriptHash, true},
  {"0014" + Hash20, Info::EWitnessPubKeyHash, true},
  {"0020" + Hash32, Info::EWitnessScriptHash, true},
  {"5120" + Hash32, Info::EWitnessTaproot, true},
  {"51024e73", Info::EAnchor, true},

  // Witness programs of no known kind; v0 of another size is no witness output at all
  {"0015" + repeat("55", 21), Info::ENonStandard, false},
  {"51024e74", Info::EWitnessUnknown, true},
  {"5220" + Hash32, Info::EWitnessUnknown, true},
  {"6028" + repeat("55", 40), Info::EWitnessUnknown, true},
  {"6029" + repeat("55", 41), Info::ENonStandard, false},

  // Shorter than the union: the length in the record tells them from its zero padding
  {"", Info::ENonStandard, false},
  {"51", Info::ENonStandard, false},
  {"0000", Info::ENonStandard, false},
  // Longer than 252 bytes: the length takes three
  {repeat("61", 300), Info::ENonStandard, false},

  // Unspendable; NULL_DATA only when pushes follow
  {"6a", Info::EOpReturn, true},
  {"6a04deadbeef", Info::EOpReturn, true},
  {"6a61", Info::EOpReturn, false},

  // Bare multisig as MatchMultisig; IsStandard takes up to x-of-3
  {"51" + keys(1) + "51ae", Info::EMultisig, true},
  {"52" + keys(1) + "41" + Key65 + "52ae", Info::EMultisig, true},
  {"51" + keys(3) + "53ae", Info::EMultisig, true},
  {"51" + keys(4) + "54ae", Info::EMultisig, false},
  {"51" + keys(16) + "60ae", Info::EMultisig, false},
  {"51" + keys(17) + "0111ae", Info::EMultisig, false},
  {"0114" + keys(20) + "0114ae", Info::EMultisig, false},
  // A key pushed by OP_PUSHDATA1 is still a key
  {"514c21" + Key33 + "51ae", Info::EMultisig, true},
  // m of zero, m above n, n not the number of keys
  {"00" + keys(1) + "51ae", Info::ENonStandard, false},
  {"52" + keys(1) + "51ae", Info::ENonStandard, false},
  {"51" + keys(1) + "52ae", Info::ENonStandard, false},
  // A key of no known prefix, an opcode before OP_CHECKMULTISIG
  {"512105" + repeat("11", 32) + "51ae", Info::ENonStandard, false},
  {"51" + keys(1) + "5161ae", Info::ENonStandard, false},
  // Numbers past OP_16 only as a direct push of one byte, 20 at most; 16 only as OP_16
  {"51" + keys(17) + "4c0111ae", Info::ENonStandard, false},
  {"51" + keys(16) + "0110ae", Info::ENonStandard, false},
  {"51" + keys(21) + "0115ae", Info::ENonStandard, false},
};

}

TEST(OutputInfo, Solver)
{
  for (const CCase &c: Cases) {
    SCOPED_TRACE(c.Script.size() > 80 ? c.Script.substr(0, 80) + "..." : c.Script);
    BTC::Proto::CTxOut out;
    out.Value = 12345;
    setBytes(out.PkScript, c.Script);

    EXPECT_EQ(BTC::Script::classify(out.PkScript.data(), out.PkScript.size()), c.Type);
    uint8_t type;
    EXPECT_EQ(BTC::Policy::isStandardOutput(out.PkScript, type), c.Standard);
    EXPECT_EQ(type, c.Type);
    EXPECT_EQ(BTC::Policy::isDataOutput(out.PkScript), c.Standard && c.Type == Info::EOpReturn);

    xmstream stream;
    BTC::Script::parseTransactionOutput(out, stream);
    ASSERT_GE(stream.sizeOf(), sizeof(Info));
    const Info &info = *static_cast<const Info*>(stream.data());
    EXPECT_EQ(info.Type, c.Type);
    EXPECT_EQ(info.Value, out.Value);

    // Lossless: the record gives the script back, all but OP_RETURN, which never reaches the database
    xvector<uint8_t> restored;
    if (c.Type == Info::EOpReturn) {
      EXPECT_FALSE(BTC::Script::restoreScript(info, restored));
    } else {
      ASSERT_TRUE(BTC::Script::restoreScript(info, restored));
      EXPECT_EQ(bin2hexLowerCase(restored.data(), restored.size()), c.Script);
      // The sigops of the record are those of the script
      EXPECT_EQ(BTC::Script::sigOpCount(info),
                BTC::Script::sigOpCount(out.PkScript.data(), out.PkScript.data() + out.PkScript.size(), true));
    }

    // The receiving side takes the address from the output, the spending side from the record
    BTC::Script::CAddress fromOutput;
    BTC::Script::CAddress fromRecord;
    const bool hasAddress = BTC::Script::extractAddress(out, fromOutput);
    EXPECT_EQ(BTC::Script::extractAddress(info, fromRecord), hasAddress);
    if (hasAddress) {
      EXPECT_TRUE(fromOutput == fromRecord);
    }
  }
}
